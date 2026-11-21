# One VM with two emulated xHCIs and the debug cable between them. The
# first is the target: its Debug Capability is dbc_net, a network interface
# on the DbC rings, in the "target" network namespace. The cable's device end
# (usb-dbc-peer) enumerates on the second, the debug host, where dbcnet.ko
# presents it as a network interface. Both are the NixOS modules this flake
# exports, on a stock kernel.
#
# Then the same target with the userspace debug host, dbcnet-host.
#
# With tty, xhci-hcd carries ttyDBC too, and the test goes on to the tty
# path (ttyDBC and dbcnet-tty) that needs no patched kernel, with stock PPP
# over the same ttys as a baseline. Without it, the module has no tty code,
# which the test checks instead.
{
  lib,
  testers,
  dbcnet,
  qemu-dbc,
  tty ? true,
}:
testers.runNixOSTest {
  name = "dbcnet-dbc" + lib.optionalString (!tty) "-notty";

  nodes.machine =
    {
      config,
      lib,
      pkgs,
      ...
    }:
    {
      imports = [
        ./nixos-target.nix
        ./nixos-debug-host.nix
      ];
      hardware.dbc-net = { inherit tty; };
      boot.kernelModules = [
        "tun"
        "ppp_async"
        "usbip-host"
        "vhci-hcd"
      ];
      environment.systemPackages = [
        dbcnet
        config.boot.kernelPackages.usbip
        pkgs.iperf3
        pkgs.ppp
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
        # A USB stick on the target, for USB/IP: 256 MiB of zeroes.
        "-device qemu-xhci,id=tgtusb"
        "-blockdev driver=null-co,node-name=stick,size=268435456,read-zeroes=on"
        "-device usb-storage,drive=stick,bus=tgtusb.0"
      ];
    };

  testScript =
    builtins.readFile ./vm-common.py
    + ''

      def dbc(attr=""):
          return machine.succeed("dirname $(ls /sys/bus/pci/devices/*/dbc)").strip() + "/" + attr

      def dbc_state():
          return machine.succeed(f"cat {dbc('dbc')}").strip()

      def carrier(prefix):
          return machine.succeed(f"{prefix}cat /sys/class/net/dbc0/carrier").strip() == "1"

      def bench(label, dst):
          print(f"BENCH {label}")
          print(machine.succeed(f"iperf3 -c {dst} -t 5 -f m"))
          print(machine.succeed(f"iperf3 -c {dst} -t 5 -f m -R"))

      def mtu(n):
          machine.succeed(f"{TARGET}ip link set dbc0 mtu {n}", f"ip link set dbc0 mtu {n}")

      def throughput(label):
          start("iperf", f"{TARGET}iperf3 -s")
          wait(f"{TARGET}ss -ltn | rg -q 5201")
          bench(f"{label} mtu 9000", "10.77.0.1")
          mtu(1500)
          bench(f"{label} mtu 1500", "10.77.0.1")
          mtu(9000)
          machine.succeed("systemctl stop iperf")

      # The debug host's interface: the DbC device's one interface. The
      # device is 1d6b:0012 for dbc_net, 1d6b:0010 for ttyDBC.
      def host_intf():
          return machine.succeed(
              "basename $(dirname $(rg -lx '001[02]' /sys/bus/usb/devices/*/idProduct)):1.0"
          ).strip()

      def host_driver():
          return machine.succeed(
              f"basename $(readlink -f /sys/bus/usb/devices/{host_intf()}/driver) || true"
          ).strip()

      def host_link_up():
          wait(f"test {dbc0_driver} = \"$(basename $(readlink -f /sys/class/net/dbc0/device/driver))\"")
          link_up("", "10.77.0.2")

      dbc0_driver = "dbcnet"

      tty = ${if tty then "True" else "False"}

      with subtest("the replacement xhci-hcd is the one loaded, with dbc_net on"):
          path = machine.succeed("modinfo -n xhci_hcd").strip()
          assert "/updates/" in path, path
          if tty:
              assert machine.succeed("cat /sys/module/xhci_hcd/parameters/dbc_net").strip() == "Y"
          else:
              # dbc_net is the only function, so there is nothing to choose.
              machine.fail("test -e /sys/module/xhci_hcd/parameters/dbc_net")
              machine.fail("rg -q dbc_serial /proc/tty/drivers")
          # Nothing loads dbcnet.ko until a dbc_net device appears.
          machine.fail("test -d /sys/bus/usb/drivers/dbcnet")

      with subtest("dbc_net is an interface before DbC is enabled, without carrier"):
          wait("ls /sys/bus/pci/devices/*/dbc")
          assert dbc_state() == "disabled", dbc_state()
          machine.succeed("ip link show dbc0")
          machine.succeed("ip link set dbc0 netns target")
          machine.fail("ls /dev/ttyDBC0")

      with subtest("guest DbC enumerates on the cable's far end at SuperSpeed"):
          # DbC has no interrupts; the driver polls every 64 ms when idle.
          machine.succeed(f"echo 1 > {dbc('dbc_poll_interval_ms')}")
          machine.succeed(f"echo enable > {dbc('dbc')}")
          wait("lsusb -d 1d6b:0012")
          wait(f"test $(cat {dbc('dbc')}) = configured")
          speed = device_speed("0012")
          assert speed == "5000", f"expected SuperSpeed, got {speed!r}"
          print(machine.succeed("lsusb -v -d 1d6b:0012"))

      with subtest("dbcnet.ko loads for the dbc_net target, and usb_debug stays out"):
          wait("test -e /sys/class/net/dbc0")
          assert host_driver() == "dbcnet", host_driver()
          machine.fail("test -d /sys/bus/usb/drivers/usb_debug")
          link_up(TARGET, "10.77.0.1")
          host_link_up()
          assert carrier(TARGET)
          wait("ping -c 1 -W 1 10.77.0.1")

      # usbnet itself refuses these, so that a full frame needs no ZLP.
      with subtest("an MTU that is a multiple of the packet size is allowed"):
          mtu(8192)
          ping("", "10.77.0.1", 8192 + 4)
          ping(TARGET, "10.77.0.2", 8192 + 4)
          mtu(9000)

      subtest_frames()
      with subtest("throughput"):
          throughput("dbc_net + dbcnet.ko")

      # The product ID keeps them apart; no udev rule or load order needed.
      with subtest("usb_debug cannot take a dbc_net target"):
          intf = host_intf()
          machine.succeed(
              f"echo {intf} > /sys/bus/usb/drivers/dbcnet/unbind",
              "modprobe usb_debug",
          )
          machine.fail(f"echo {intf} > /sys/bus/usb/drivers/usb_debug/bind")
          machine.succeed(f"echo {intf} > /sys/bus/usb/drivers/dbcnet/bind")
          host_link_up()
          wait("ping -c 1 -W 1 10.77.0.1")

      with subtest("the link comes back after DbC is disabled and re-enabled"):
          machine.succeed(f"echo disable > {dbc('dbc')}")
          wait("! test -e /sys/class/net/dbc0")
          machine.fail("lsusb -d 1d6b:0012")
          assert not carrier(TARGET)
          machine.succeed(f"echo enable > {dbc('dbc')}")
          # The target's interface and address outlive the link; the host's
          # interface is new.
          host_link_up()
          wait("ping -c 1 -W 1 10.77.0.1")
          ping("", "10.77.0.1", 4096)

      with subtest("the link comes back after the cable is unplugged"):
          machine.send_monitor_command("device_del cable")
          wait(f"test $(cat {dbc('dbc')}) = enabled")
          wait("! test -e /sys/class/net/dbc0")
          assert not carrier(TARGET)
          machine.send_monitor_command("device_add usb-dbc-peer,id=cable,dbc=dbct,bus=dbch.0")
          host_link_up()
          wait("ping -c 1 -W 1 10.77.0.1")
          ping("", "10.77.0.1", 4096)

      # The userspace debug host, for hosts without dbcnet.ko.
      with subtest("dbcnet-host drives the same target from userspace"):
          machine.succeed(f"echo {host_intf()} > /sys/bus/usb/drivers/dbcnet/unbind")
          wait("! test -e /sys/class/net/dbc0")
          start("dbcnet-host", "dbcnet-host --tun dbc0")
          connects(1)
          link_up("", "10.77.0.2")
          wait("ping -c 1 -W 1 10.77.0.1")
          for frame in (1024, 4096, 9004):
              ping("", "10.77.0.1", frame)
              ping(TARGET, "10.77.0.2", frame)
          throughput("dbc_net + dbcnet-host")

      with subtest("the debug host must end a packet-multiple transfer with a ZLP"):
          machine.succeed("systemctl stop dbcnet-host")
          start("dbcnet-host", "dbcnet-host --tun dbc0 --no-zlp")
          connects(2)
          link_up("", "10.77.0.2")
          wait("ping -c 1 -W 1 10.77.0.1")
          # dbc_net's 64 KiB receive TRB sees 1024 bytes and no short packet,
          # so it waits for more and this echo request never arrives in time.
          machine.fail(f"ping -c 1 -W 2 -s {1024 - OVERHEAD} 10.77.0.1")
          machine.succeed("systemctl stop dbcnet-host")

      # What gadget mode would give, without a UDC on either side: a USB device
      # on the target shows up on the debug host, carried by USB/IP over dbc0.
      with subtest("USB/IP carries the target's USB stick to the debug host"):
          machine.succeed(f"echo {host_intf()} > /sys/bus/usb/drivers/dbcnet/bind")
          host_link_up()
          wait("ping -c 1 -W 1 10.77.0.1")
          busid = machine.succeed(
              "basename $(dirname $(rg -lx 46f4 /sys/bus/usb/devices/*/idVendor))"
          ).strip()
          machine.succeed(f"usbip bind -b {busid}")
          start("usbipd", f"{TARGET}usbipd")
          wait(f"{TARGET}ss -ltn | rg -q 3240")
          machine.succeed(f"usbip attach -r 10.77.0.1 -b {busid}")
          wait("readlink -f /sys/class/block/sd* | rg -q vhci")
          disk = machine.succeed(
              "basename $(readlink -f /sys/class/block/sd* | rg vhci | head -1)"
          ).strip()
          print("BENCH usbip read over dbc_net + dbcnet.ko")
          print(machine.succeed(f"dd if=/dev/{disk} of=/dev/null bs=1M iflag=direct 2>&1"))
          machine.succeed(f"cmp -n 67108864 /dev/{disk} /dev/zero")
          port = machine.succeed("usbip port | rg -o '^Port (\\d+)' -r '$1' | head -1").strip()
          machine.succeed(f"usbip detach -p {port}")
          wait("! readlink -f /sys/class/block/sd* | rg -q vhci")
          machine.succeed(f"usbip unbind -b {busid}", "systemctl stop usbipd")
    ''
    + lib.optionalString (!tty) ''

      with subtest("without the tty function, nothing brings ttyDBC back"):
          pci = machine.succeed(f"basename {dbc()}").strip()
          drv = machine.succeed(f"readlink -f /sys/bus/pci/devices/{pci}/driver").strip()
          machine.succeed(f"echo {pci} > {drv}/unbind", f"echo {pci} > {drv}/bind")
          wait("ls /sys/bus/pci/devices/*/dbc")
          machine.succeed(f"echo enable > {dbc('dbc')}")
          wait(f"test $(cat {dbc('dbc')}) = configured")
          wait("test -e /sys/class/net/dbc0")
          assert host_driver() == "dbcnet", host_driver()
          machine.fail("ls /dev/ttyDBC*")
    ''
    + lib.optionalString tty ''

      # The rest runs on ttyDBC: rebinding the controller with dbc_net off
      # brings the tty function back in its place.
      with subtest("the tty function returns when dbc_net is off"):
          pci = machine.succeed(f"basename {dbc()}").strip()
          drv = machine.succeed(f"readlink -f /sys/bus/pci/devices/{pci}/driver").strip()
          machine.succeed(
              "echo 0 > /sys/module/xhci_hcd/parameters/dbc_net",
              f"echo {pci} > {drv}/unbind",
          )
          machine.fail(f"{TARGET}ip link show dbc0")
          machine.succeed(f"echo {pci} > {drv}/bind")
          wait("ls /sys/bus/pci/devices/*/dbc")
          machine.succeed(f"echo 1 > {dbc('dbc_poll_interval_ms')}")
          machine.succeed(f"echo enable > {dbc('dbc')}")
          wait(f"test $(cat {dbc('dbc')}) = configured")
          # 1d6b:0010: usb_debug's, not dbcnet's.
          wait("test -c /dev/ttyDBC0 && test -c /dev/ttyUSB0")
          assert host_driver() == "usb_debug", host_driver()

      with subtest("ttyDBC and usb_debug's ttyUSB carry bytes both ways"):
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

      with subtest("dbcnet-tty bridges ttyDBC for kernels without dbc_net"):
          start("dbcnet-tty", f"{TARGET}dbcnet-tty --tun dbc0")
          start("dbcnet-host", "dbcnet-host --tun dbc0")
          connects(3)
          wait("journalctl -o cat -u dbcnet-tty | rg -q 'dbcnet-tty: connected'")
          link_up(TARGET, "10.77.0.1")
          link_up("", "10.77.0.2")
          wait("ping -c 1 -W 1 10.77.0.1")
          for frame in (1024, 4096, 9004):
              ping("", "10.77.0.1", frame)
              ping(TARGET, "10.77.0.2", frame)
          throughput("dbcnet-tty + dbcnet-host")

      subtest_target_pause("dbcnet-tty")

      # DbC never sends a zero-length packet: unpadded, a 1024-byte reply ends
      # on a packet boundary and only reaches the host at its read timeout.
      with subtest("target to host without padding"):
          machine.succeed("systemctl stop dbcnet-tty")
          start("dbcnet-tty", f"{TARGET}dbcnet-tty --tun dbc0 --no-pad")
          wait("test $(journalctl -o cat -u dbcnet-tty | rg -c 'dbcnet-tty: connected') -ge 2")
          link_up(TARGET, "10.77.0.1")
          wait("ping -c 1 -W 1 10.77.0.1")
          out = machine.succeed(f"ping -c 1 -W 2 -s {1024 - OVERHEAD} 10.77.0.1")
          assert rtt_max(out) >= 50, f"expected a timeout-delayed reply: {out}"

      # The baseline that needs no code of ours: stock PPP over ttyDBC and
      # usb_debug's ttyUSB. Reported, not asserted.
      with subtest("throughput of PPP over ttyDBC and ttyUSB"):
          machine.succeed("systemctl stop dbcnet-host dbcnet-tty")
          # Re-enumerate, so usb_debug takes the interface back from libusb.
          machine.succeed(f"echo disable > {dbc('dbc')}")
          wait("! lsusb -d 1d6b:0010")
          machine.succeed(f"echo enable > {dbc('dbc')}")
          wait(f"test $(cat {dbc('dbc')}) = configured")
          wait("test -c /dev/ttyDBC0 && test -c /dev/ttyUSB0")
          # asyncmap 0 escapes only the flag and escape bytes; no compression.
          ppp = "nodetach noauth local nocrtscts asyncmap 0 noccp novj mtu 1500 mru 1500"
          start("ppp-target", f"{TARGET}pppd /dev/ttyDBC0 {ppp} 10.88.0.1:10.88.0.2")
          start("ppp-host", f"pppd /dev/ttyUSB0 {ppp} 10.88.0.2:10.88.0.1")
          wait("ping -c 1 -W 1 10.88.0.1")
          start("iperf", f"{TARGET}iperf3 -s")
          wait(f"{TARGET}ss -ltn | rg -q 5201")
          bench("ppp mtu 1500", "10.88.0.1")
          machine.succeed("systemctl stop iperf ppp-target ppp-host")
    '';
}
