# One VM with two emulated xHCIs and the debug cable between them: the
# guest's own DbC driver (ttyDBC) on the first is the target, bridged to TUN
# by dbcnet-tty in the "target" network namespace; the cable's device end
# (usb-dbc-peer) enumerates on the second, where usb_debug and then
# dbcnet-host take it as the debug host. Needs QEMU with DbC emulation and a
# kernel with CONFIG_USB_XHCI_DBGCAP.
{
  testers,
  linuxPackagesFor,
  dbcnet,
  linux-dbgcap,
  qemu-dbc,
}:
testers.runNixOSTest {
  name = "dbcnet-dbc";

  nodes.machine =
    { lib, pkgs, ... }:
    {
      boot.kernelPackages = linuxPackagesFor linux-dbgcap;
      boot.kernelModules = [ "tun" ];
      environment.systemPackages = [
        dbcnet
        pkgs.iperf3
        pkgs.ripgrep
        pkgs.usbutils
      ];
      virtualisation.cores = 2;
      # The test framework pins its own QEMU.
      virtualisation.qemu.package = lib.mkForce qemu-dbc;
      virtualisation.qemu.options = [
        "-device qemu-xhci,id=dbct,dbc=on"
        "-device qemu-xhci,id=dbch"
        "-device usb-dbc-peer,id=cable,dbc=dbct,bus=dbch.0"
      ];
    };

  testScript = builtins.readFile ./vm-common.py + ''

    def dbc(attr=""):
        return machine.succeed("dirname $(ls /sys/bus/pci/devices/*/dbc)").strip() + "/" + attr

    def dbc_state():
        return machine.succeed(f"cat {dbc('dbc')}").strip()

    def tty_connects(n):
        wait(f"test $(journalctl -o cat -u dbcnet-tty | rg -c 'dbcnet-tty: connected') -ge {n}")

    with subtest("guest DbC enumerates on the cable's far end at SuperSpeed"):
        wait("ls /sys/bus/pci/devices/*/dbc")
        assert dbc_state() == "disabled", dbc_state()
        machine.fail("lsusb -d 1d6b:0010")
        # DbC has no interrupts; the driver polls every 64 ms when idle.
        machine.succeed(f"echo 1 > {dbc('dbc_poll_interval_ms')}")
        machine.succeed(f"echo enable > {dbc('dbc')}")
        wait("lsusb -d 1d6b:0010")
        wait(f"test $(cat {dbc('dbc')}) = configured")
        speed = device_speed("0010")
        assert speed == "5000", f"expected SuperSpeed, got {speed!r}"
        print(machine.succeed("lsusb -v -d 1d6b:0010"))
        product = machine.succeed("cat $(dirname $(rg -lx 0010 /sys/bus/usb/devices/*/idProduct))/product").strip()
        assert product == "Linux USB Debug Target", product

    with subtest("ttyDBC and usb_debug's ttyUSB carry bytes both ways"):
        wait("test -c /dev/ttyDBC0 && test -c /dev/ttyUSB0")
        machine.succeed(
            "stty -F /dev/ttyDBC0 raw -echo",
            "stty -F /dev/ttyUSB0 raw -echo",
            "head -c 1048576 /dev/urandom > /tmp/blob",
        )
        for src, dst in (("/dev/ttyUSB0", "/dev/ttyDBC0"), ("/dev/ttyDBC0", "/dev/ttyUSB0")):
            machine.succeed(
                f"(timeout 60 head -c 1048576 {dst} > /tmp/rx) & sleep 1;"
                f" cat /tmp/blob > {src}; wait; cmp /tmp/blob /tmp/rx"
            )

    with subtest("host takes the endpoints over from usb_debug"):
        start("dbcnet-tty", f"{TARGET}dbcnet-tty --tun dbc0")
        start("dbcnet-host", "dbcnet-host --tun dbc0")
        connects(1)
        tty_connects(1)
        link_up(TARGET, "10.77.0.1")
        link_up("", "10.77.0.2")
        wait("ping -c 1 -W 1 10.77.0.1")

    subtest_frames()
    subtest_throughput()
    subtest_target_pause("dbcnet-tty")

    with subtest("ttyDBC reads one packet per TRB, so the host needs no ZLP"):
        machine.succeed("systemctl stop dbcnet-host")
        start("dbcnet-host", "dbcnet-host --tun dbc0 --no-zlp")
        connects(2)
        link_up("", "10.77.0.2")
        wait("ping -c 1 -W 1 10.77.0.1")
        ping("", "10.77.0.1", 1024)
        machine.succeed("systemctl stop dbcnet-host")
        start("dbcnet-host", "dbcnet-host --tun dbc0")
        connects(3)
        link_up("", "10.77.0.2")

    # DbC never sends a zero-length packet: unpadded, a 1024-byte reply ends
    # on a packet boundary and only reaches the host at its read timeout.
    with subtest("target to host without padding"):
        machine.succeed("systemctl stop dbcnet-tty")
        start("dbcnet-tty", f"{TARGET}dbcnet-tty --tun dbc0 --no-pad")
        tty_connects(2)
        link_up(TARGET, "10.77.0.1")
        wait("ping -c 1 -W 1 10.77.0.1")
        out = machine.succeed(f"ping -c 1 -W 2 -s {1024 - OVERHEAD} 10.77.0.1")
        assert rtt_max(out) >= 50, f"expected a timeout-delayed reply: {out}"
        machine.succeed("systemctl stop dbcnet-tty")
        start("dbcnet-tty", f"{TARGET}dbcnet-tty --tun dbc0")
        tty_connects(3)
        link_up(TARGET, "10.77.0.1")
        ping("", "10.77.0.1", 1024)

    with subtest("both ends reconnect after DbC is disabled and re-enabled"):
        machine.succeed(f"echo disable > {dbc('dbc')}")
        wait("journalctl -o cat -u dbcnet-host | rg -q 'dbcnet-host: disconnected'")
        wait("journalctl -o cat -u dbcnet-tty | rg -q 'dbcnet-tty: disconnected'")
        machine.fail("lsusb -d 1d6b:0010")
        machine.succeed(f"echo enable > {dbc('dbc')}")
        connects(4)
        tty_connects(4)
        wait("ping -c 1 -W 1 10.77.0.1")
        ping("", "10.77.0.1", 4096)

    with subtest("both ends reconnect after the cable is unplugged"):
        machine.send_monitor_command("device_del cable")
        wait(f"test $(cat {dbc('dbc')}) = enabled")
        wait("! lsusb -d 1d6b:0010")
        machine.send_monitor_command("device_add usb-dbc-peer,id=cable,dbc=dbct,bus=dbch.0")
        connects(5)
        tty_connects(5)
        wait("ping -c 1 -W 1 10.77.0.1")
        ping("", "10.77.0.1", 4096)
  '';
}
