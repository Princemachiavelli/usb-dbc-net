# Shared by the VM tests (prepended to each testScript): the debug-host end
# (dbcnet-host) runs in the default network namespace at 10.77.0.2, the
# target end in the "target" namespace at 10.77.0.1, each on a dbc0 TUN.

TARGET = "ip netns exec target "
# ICMP echo: frame = 4 (dbcnet) + 20 (IPv4) + 8 (ICMP) + payload.
OVERHEAD = 32


def wait(cmd):
    machine.wait_until_succeeds(cmd, timeout=60)


# Transient units start with systemd's PATH, not the system profile.
def start(unit, cmd):
    machine.succeed(f"systemd-run --collect --setenv=PATH --unit={unit} {cmd}")


def link_up(prefix, addr):
    wait(f"{prefix}ip link show dbc0")
    machine.succeed(
        f"{prefix}sysctl -qw net.ipv6.conf.dbc0.disable_ipv6=1",
        f"{prefix}ip addr add {addr}/24 dev dbc0",
        f"{prefix}ip link set dbc0 mtu 9000 up",
    )


def connects(n):
    wait(f"test $(journalctl -o cat -u dbcnet-host | rg -c 'dbcnet-host: connected') -ge {n}")


def rtt_max(out):
    # "rtt min/avg/max/mdev = 0.1/0.2/0.3/0.0 ms"
    return float(out.split("rtt min/avg/max/mdev = ")[1].split("/")[2])


# dbcnet-host's IN read gives up after 200 ms and delivers what it has, so a
# missing short packet shows up as latency rather than loss: bound the RTT.
def ping(prefix, dst, frame):
    out = machine.succeed(f"{prefix}ping -c 3 -i 0.2 -W 2 -s {frame - OVERHEAD} {dst}")
    assert rtt_max(out) < 50, f"frame {frame}: {out}"


def device_speed(product):
    return machine.succeed(
        f"cat $(dirname $(rg -lx {product} /sys/bus/usb/devices/*/idProduct))/speed"
    ).strip()


def subtest_frames():
    with subtest("frames of every size, including exact packet multiples"):
        for frame in (64, 1023, 1024, 1025, 2048, 4096, 8192, 9004):
            ping("", "10.77.0.1", frame)
            ping(TARGET, "10.77.0.2", frame)


def subtest_throughput():
    with subtest("throughput"):
        start("iperf", f"{TARGET}iperf3 -s")
        wait(f"{TARGET}ss -ltn | rg -q 5201")
        print(machine.succeed("iperf3 -c 10.77.0.1 -t 5 -f m"))
        print(machine.succeed("iperf3 -c 10.77.0.1 -t 5 -f m -R"))
        machine.succeed("systemctl stop iperf")


# The target stops reading for longer than any transfer timeout, so the debug
# host's bulk OUT sits partly taken. Abandoning it would leave the target's
# decoder mid-frame, swallowing what follows; waiting it out loses nothing.
# Not for dbc-impostor: stopping it interrupts raw_gadget's I/O ioctls, which
# drop the partly filled request, a loss real DbC has no counterpart for.
def subtest_target_pause(proc):
    with subtest("a target that stops reading for a while loses nothing"):
        start("iperf", f"{TARGET}iperf3 -s")
        wait(f"{TARGET}ss -ltn | rg -q 5201")
        print(machine.succeed(
            f"(sleep 2; kill -STOP $(pgrep -x {proc}); sleep 4; kill -CONT $(pgrep -x {proc})) &"
            " iperf3 -c 10.77.0.1 -t 10 -f m"
        ))
        machine.succeed("systemctl stop iperf")
        for frame in (1024, 9004):
            ping("", "10.77.0.1", frame)
            ping(TARGET, "10.77.0.2", frame)


machine.wait_for_unit("multi-user.target")
machine.succeed("ip netns add target", "ip -n target link set lo up")
