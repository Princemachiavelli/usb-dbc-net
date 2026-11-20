#!/usr/bin/env python3
"""Report whether each xHCI controller implements the Debug Capability (DbC).

Walks the xHCI extended-capability list (xHCI spec 5.3.6 / 7) by mapping
BAR0 read-only through sysfs. Works on a kernel without
CONFIG_USB_XHCI_DBGCAP. Needs root (sysfs resource files are 0600).

    sudo python3 scripts/probe-dbc.py
"""

import mmap
import os
import sys

PCI = "/sys/bus/pci/devices"
XHCI_CLASS = 0x0C0330
EXT_CAP_NAMES = {
    1: "USB Legacy Support",
    2: "Supported Protocol",
    3: "Extended Power Management",
    4: "I/O Virtualization",
    5: "Message Interrupt",
    6: "Local Memory",
    10: "Debug Capability (DbC)",
    17: "Extended Message Interrupt",
}


def read_sysfs(dev, name):
    with open(os.path.join(PCI, dev, name)) as f:
        return f.read().strip()


def xhci_devices():
    for dev in sorted(os.listdir(PCI)):
        if int(read_sysfs(dev, "class"), 16) == XHCI_CLASS:
            yield dev


def probe(dev):
    path = os.path.join(PCI, dev, "resource0")
    size = os.stat(path).st_size
    fd = os.open(path, os.O_RDONLY | os.O_SYNC)
    try:
        mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fd)
    # 32-bit loads only: MMIO registers must not be read bytewise.
    regs = memoryview(mm).cast("I")

    def rd(off):
        return regs[off // 4]

    try:
        if rd(0) == 0xFFFFFFFF:
            print("  registers read all-ones: controller is runtime-suspended;")
            print(f"  run: echo on | sudo tee {PCI}/{dev}/power/control")
            return
        hci_version = rd(0) >> 16
        xecp = (rd(0x10) >> 16) * 4
        print(f"  HCIVERSION {hci_version >> 8:x}.{hci_version & 0xff:02x}")
        dbc = False
        off = xecp
        while off:
            hdr = rd(off)
            cap_id, nxt = hdr & 0xFF, (hdr >> 8) & 0xFF
            name = EXT_CAP_NAMES.get(cap_id, "vendor/unknown")
            line = f"  ext cap @0x{off:04x}: id {cap_id:3d} {name}"
            if cap_id == 2:
                major, minor = hdr >> 24, (hdr >> 16) & 0xFF
                ports = rd(off + 8)
                first, count = ports & 0xFF, (ports >> 8) & 0xFF
                line += f" USB {major}.{minor:x} ports {first}-{first + count - 1}"
            elif cap_id == 10:
                dbc = True
                enabled = bool(rd(off + 0x20) >> 31)  # DCCTRL.DCE
                line += f" (DCCTRL.DCE={int(enabled)})"
            print(line)
            off = off + nxt * 4 if nxt else 0
        print(f"  => DbC {'PRESENT' if dbc else 'absent'}")
    finally:
        regs.release()
        mm.close()


def main():
    if os.geteuid() != 0:
        sys.exit("needs root: sysfs PCI resource files are root-only")
    for dev in xhci_devices():
        ids = f"{read_sysfs(dev, 'vendor')[2:]}:{read_sysfs(dev, 'device')[2:]}"
        print(f"{dev} [{ids}]")
        try:
            probe(dev)
        except OSError as e:
            # CONFIG_IO_STRICT_DEVMEM refuses to map regions a bound driver
            # has claimed; xhci debugfs (reg-ext-dbc:NN) is the easier check.
            print(f"  cannot map BAR0: {e}")
            print("  (unbind xhci_hcd or boot with iomem=relaxed, or check")
            print(f"   /sys/kernel/debug/usb/xhci/{dev}/reg-ext-dbc:*)")


if __name__ == "__main__":
    main()
