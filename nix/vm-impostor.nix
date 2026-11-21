# One VM, no DbC hardware: dbc-impostor presents DbC's descriptors through
# raw_gadget on a SuperSpeed dummy_hcd, in its own network namespace as the
# "target"; dbcnet-host drives it through libusb from the default namespace as
# the "debug host". IP flows between the two over the emulated USB bus.
{ testers, dbcnet }:
testers.runNixOSTest {
  name = "dbcnet-impostor";

  nodes.machine =
    { pkgs, ... }:
    {
      boot.kernelModules = [
        "dummy_hcd"
        "raw_gadget"
        "tun"
      ];
      boot.extraModprobeConfig = "options dummy_hcd is_super_speed=1";
      environment.systemPackages = [
        dbcnet
        pkgs.iperf3
        pkgs.ripgrep
        pkgs.usbutils
      ];
      virtualisation.cores = 2;
    };

  testScript = builtins.readFile ./vm-common.py + ''

    with subtest("impostor enumerates at SuperSpeed as DbC"):
        start("impostor", f"{TARGET}dbc-impostor --tun dbc0")
        wait("lsusb -d 1d6b:0010")
        speed = device_speed("0010")
        assert speed == "5000", f"expected SuperSpeed, got {speed!r}"
        print(machine.succeed("lsusb -v -d 1d6b:0010"))

    with subtest("host takes the endpoints over from usb_debug"):
        start("dbcnet-host", "dbcnet-host --tun dbc0")
        connects(1)
        link_up(TARGET, "10.77.0.1")
        link_up("", "10.77.0.2")
        wait("ping -c 1 -W 1 10.77.0.1")

    subtest_frames()
    subtest_throughput()

    with subtest("a transfer ending on a packet boundary needs a ZLP"):
        machine.succeed("systemctl stop dbcnet-host")
        start("dbcnet-host", "dbcnet-host --tun dbc0 --no-zlp")
        connects(2)
        link_up("", "10.77.0.2")
        wait("ping -c 1 -W 1 10.77.0.1")
        # The impostor's 32 KiB read sees 1024 bytes and no short packet, so
        # it waits for more and this echo request never arrives in time.
        machine.fail(f"ping -c 1 -W 2 -s {1024 - OVERHEAD} 10.77.0.1")
        machine.succeed("systemctl stop dbcnet-host")
        start("dbcnet-host", "dbcnet-host --tun dbc0")
        connects(3)
        link_up("", "10.77.0.2")
        ping("", "10.77.0.1", 1024)

    with subtest("host side reconnects after the device goes away"):
        machine.succeed("systemctl stop impostor")
        wait("journalctl -o cat -u dbcnet-host | rg -q 'dbcnet-host: disconnected'")
        start("impostor", f"{TARGET}dbc-impostor --tun dbc0")
        connects(4)
        link_up(TARGET, "10.77.0.1")
        wait("ping -c 1 -W 1 10.77.0.1")
        ping("", "10.77.0.1", 4096)

    # Real ttyDBC never queues a zero-length TRB, so this is what the debug
    # host sees from it when a write ends on a packet boundary.
    with subtest("target to host without a ZLP"):
        machine.succeed("systemctl stop impostor")
        start("impostor", f"{TARGET}dbc-impostor --tun dbc0 --no-zlp")
        connects(5)
        link_up(TARGET, "10.77.0.1")
        wait("ping -c 1 -W 1 10.77.0.1")
        # The echo reply is not lost: the host's 200 ms read timeout returns
        # the partial transfer. It is late instead.
        out = machine.succeed(f"ping -c 1 -W 2 -s {1024 - OVERHEAD} 10.77.0.1")
        assert rtt_max(out) >= 50, f"expected a timeout-delayed reply: {out}"
        machine.succeed("systemctl stop impostor")
        start("impostor", f"{TARGET}dbc-impostor --tun dbc0")
        connects(6)
        link_up(TARGET, "10.77.0.1")
        ping("", "10.77.0.1", 1024)
  '';
}
