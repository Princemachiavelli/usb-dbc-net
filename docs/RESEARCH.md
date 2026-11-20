# Host-to-host networking over USB without gadget mode

Research notes, 2026-11-20. Goal: an IP link between two ordinary PCs over a
USB cable, where neither PC has a USB device controller (UDC) — no
`configfs` gadget, no `g_ether`, no dual-role port.

## 1. The constraint everything follows from

USB is strictly master/slave. On every link exactly one end is the host
(drives VBUS, schedules every transfer) and the other is a device (answers).
Two host controllers wired together do nothing useful: neither will ever
issue a token the other answers, and a generic A-to-A cable with VBUS
connected ties two 5 V supplies together.

So "host to host" always means *something* plays the device role. Without a
UDC on either PC there are exactly three places that something can live:

| # | Who plays "device" | Speeds | Linux today |
|---|--------------------|--------|-------------|
| A | A **bridge chip inside the cable** that is a device to *both* hosts | USB 1.1 / 2.0 / 3.0 | mainline netdev (`plusb`, `net1080`, `gl620a`, `cdc_subset`) |
| B | The **xHCI Debug Capability (DbC)** — a device-mode function built into many xHCI host controllers | USB 3.x SuperSpeed only | mainline, but only as a tty (`/dev/ttyDBC0`) |
| C | **USB4 / Thunderbolt inter-domain** — both ends stay hosts, the USB4 router fabric carries a peer-to-peer protocol | USB4 / TB3+ | mainline netdev (`thunderbolt_net`, "USB4NET") |

Excluded: gadget / dual-role (OTG, Type-C DRD, Intel xDCI, Apple Silicon
Macs' CDC-NCM mode). These all need a UDC on one side.

Not USB at all, but the practical fallback: **two USB↔Ethernet adapters and a
patch cable** (RTL8156 2.5G, AQC111 5G, both `cdc_ncm`/`r8152`/`aqc111` in
mainline). Boring. It works everywhere.

---

## 2. Option A — bridge cables ("data link" / "Easy Transfer" cables)

A small chip with **two USB device ports** sits in the middle of the cable.
Each PC sees a USB device and binds a usbnet minidriver. The chip shuttles
bulk data between its two device sides. Both PCs get a point-to-point `usbN`
netdev.

Drivers in mainline (verified with `modinfo` on this machine, 7.2.9):

| Chip | Speed | Driver | IDs |
|------|-------|--------|-----|
| Prolific PL-2301 / PL-2302 | USB 1.1 | `plusb` | 067b:0000, 067b:0001 |
| Prolific PL-25A1 (Belkin, NI) | USB 2.0 | `plusb` | 067b:25a1, 050d:258a, 3923:7825 |
| **Prolific PL-27A1** | **USB 3.0** | `plusb` | 067b:27a1 |
| NetChip NET1080 | USB 1.1 | `net1080` | 0525:1080, 06d0:0622 |
| Genesys GL620A | USB 1.1 | `gl620a` | 05e3:0502 |
| Anchor EZ-Link (AN2720), Belkin F5U104, … | USB 1.1 | `cdc_subset` | various |

PL-27A1 notes:
- Same register model as PL-25A1 with a USB 3.0 PHY, so plusb gained
  support just by adding the ID (patches 2016 and 2017).
- The 2016 patch email reports "about 1.5Gbps using iperf".
- `plusb` is still marked "experimental" in Kconfig because of handshake,
  init and reset problems inherited from the original PL-2301 code.
- Retail cables with this chip: goobay "Active USB 3.0 Data Link", Unitek
  Y-3501, ThinkPenguin TPE-3TRANCBL, Plugable USB3-TRAN, and others.
- Many "Easy Transfer" cables carry an SPI flash and boot into a
  mass-storage/"KM switch" personality first. Check the ID they present
  before buying.

Pros: zero software work, works on USB 2.0, real netdev, works through
hubs.

Cons:
- The protocol is Prolific's, documented only by `plusb.c`.
- Throughput tops out around 1.5 Gb/s at best.
- Every frame takes a store-and-forward hop through a cheap chip.

**This is the only option for bare USB 2.0.** DbC needs SuperSpeed, and USB4
is its own thing.

---

## 3. Option B — xHCI Debug Capability (DbC)

The interesting option, and the one that fits "no gadget mode" most
literally.

### What it is

xHCI spec §7.6 defines an optional extended capability (ext-cap ID 10).
- When it is enabled, the controller hands one root-hub **SuperSpeed** port
  over to a small built-in *device* engine. That port then enumerates on the
  far host as a USB device: by default `1d6b:0010` "Linux USB Debug Target".
- The engine is in the host controller silicon. It is not a UDC, and no
  gadget stack is involved.
- Most Intel xHCI controllers have it. AMD, ASMedia, Renesas and VIA
  coverage is unknown (see "Status on this workstation" below).

Only the **target** side needs a DbC-capable controller. The other end is any
ordinary USB 3 port.

### What it exposes (from `drivers/usb/host/xhci-dbgcap.{c,h}`)

- One configuration, one interface, **exactly two endpoints: bulk OUT + bulk
  IN** (`struct dbc_ep eps[2]`). There is no interrupt endpoint and no class
  control requests.
- Max packet size is hardwired to 1024 (`DBC_MAX_PACKET`), with
  SuperSpeed bursts.
- The descriptors can be tuned through sysfs, but only while DbC is
  disabled:
  - `dbc_idVendor`, `dbc_idProduct`, `dbc_bcdDevice` and
    `dbc_bInterfaceProtocol` (Mar 2023).
  - `dbc_serial`, `dbc_product` and `dbc_manufacturer` (Jan 2026).
  - `dbc_poll_interval_ms` (Feb 2024).
- Kernel plumbing: there is a function-driver hook
  (`struct dbc_driver { configure; disconnect; }`, `dbc->driver`,
  `xhci_alloc_dbc(dev, base, driver)`). However, `xhci_create_dbc_dev()`
  calls `xhci_dbc_tty_probe()` directly, so the tty is the only function and
  it is hardcoded.

**Consequence: DbC cannot impersonate CDC-NCM/ECM.** Those classes need a
communications interface with an interrupt notification endpoint, a data
interface with alternate settings, and class control requests. DbC has
none of them, so `cdc_ncm` will never bind. "NCM over DbC" can at most
mean borrowing NCM's NTB framing over a raw bulk pair.

### Performance

Concrete DbC numbers:
- **Celadon (Intel's Android-on-x86): ADB over DbC measured ~28 MB/s.**
  This predates the 6.14 work.
- **Linux 6.14 DbC performance series** (Mathias Nyman, Dec 2024, tested by
  Łukasz Bartosik): adb push 210/118 MB/s max/min, pull 171/133 MB/s, on
  300 MB–9 GB files. That is roughly 1.0–1.7 Gb/s, comparable to the PL-27A1
  but with no chip in the middle.
- Both figures go through the ttyDBC path. `xhci-dbgcap.h` defines
  `DBC_WRITE_BUF_SIZE 8192`, presumably the tty's write FIFO (in
  `xhci-dbgtty.c`, which I haven't read). The tty layer is the overhead to
  remove, and a native netdev with large transfers should do better. That
  still needs to be measured.

### Prior art

- **Microsoft KDNET over USB**: Windows kernel debugging runs a *packet*
  transport over DbC. You need an A-A crossover cable with no VBUS, or a
  Type-A-to-Type-C adapter on the target. This is the closest existing
  "network over DbC" in production.
- **Celadon ADB over DbC**:
  - Target: ttyDBC-backed adbd.
  - Host: `usb_debug` unloaded, adb talks straight to the bulk endpoints
    via usbfs.
  - So the host side needs no kernel driver at all.
- **`xue`** (github.com/connojd/xue): a header-only, OS-agnostic DbC driver
  used by Xen and Bareflank for early console.
- **`earlyprintk=xdbc`** (`drivers/usb/early/xhci-dbc.c`): the kernel's
  boot-time DbC console.
- **None found**: no Linux netdev over DbC, either upstream or in a public
  out-of-tree project. This is the gap a `usb-net` project could fill.

### Hard constraints

- **SuperSpeed only.** Celadon notes that a USB 2.0 A-A cable does not work.
- **Cable**: a USB 3.0 A-to-A *debug* cable. Its SS pairs are crossed, and
  it has no VBUS and no D+/D-. **Never use a generic A-A cable with VBUS
  connected.**
  - USB-C targets have worked through an A-to-C adapter (per Microsoft).
  - C-to-C between two DRP ports depends on which side ends up as UFP. This
    is untested.
- The port has to be on the **root hub** and connected directly, with no
  hubs or docks.
- While DbC is enabled, that root port belongs to DbC and normal devices on
  it stop working.
- No runtime PM: disable autosuspend on the debug host side.

### Status on this workstation

- The running kernel has `CONFIG_USB_XHCI_DBGCAP is not set`, so there is
  no `/sys/bus/pci/devices/*/dbc`. Turning it on means a NixOS kernel
  rebuild: `boot.kernelPatches` with `extraStructuredConfig.USB_XHCI_DBGCAP
  = lib.kernel.yes`.
- Controllers present:
  - AMD 500-series chipset xHCI (1022:43ee)
  - 2× AMD Renoir/Cezanne xHCI (1022:1639)
  - NEC/Renesas uPD720200 (1033:0194)
- **Whether any of these implement DbC is unknown.** You can find out
  without a rebuild, because `CONFIG_DEBUG_FS=y`. xhci's debugfs creates a
  `reg-ext-dbc:NN` register dump only for controllers that have the
  capability:

      sudo sh -c 'ls -d /sys/kernel/debug/usb/xhci/*/reg-ext-*'

  Fallback: `scripts/probe-dbc.py` walks the extended-capability list
  itself.
  - This kernel has `CONFIG_IO_STRICT_DEVMEM=y`, and `xhci_hcd` has claimed
    every BAR, so mapping will most likely fail unless the controller is
    unbound or you boot with `iomem=relaxed`.
  - The register walk has not yet been run as root.
- Only one side needs DbC. A laptop with Intel xHCI is the safest target.

---

## 4. Option C — USB4 / Thunderbolt networking

Both ends stay hosts. The USB4 routers set up an inter-domain link, and
`thunderbolt_net` provides a netdev. On Windows 11 this is called "USB4NET",
and the two interoperate.
- Reported real-world throughput is about 10–20 Gb/s, with direction-
  dependent asymmetry and MTU sensitivity.
- It is the standardized answer to this exact question, but it needs USB4 or
  TB3+ silicon on both ends.
- `lspci` shows none on this machine.

---

## 5. Can a QEMU VM play the device side?

**No.** Whether a physical port behaves as host or device is decided by the
controller silicon and the PHY. A VM only changes software, so nothing
running in a guest can make a host-only port answer tokens on the wire.

- **`dummy_hcd`** is the module you were thinking of. It is a software
  loopback that connects a gadget and a host controller *inside the same
  kernel* and has no connection to a physical port.
- **QEMU** emulates *host* controllers for its guests: xHCI, EHCI, and
  `dwc2` in host mode only (its gadget registers are stubbed: "Gadget-mode
  registers, just return 0 for now"). `usb-host` passthrough hands a
  *physical device* to a guest, which is the opposite direction.
  QEMU's emulated xHCI advertises only Supported-Protocol capabilities, not
  DbC.
- **USB/IP with `usbip-vudc`** can export a gadget from one machine to
  another's host stack, but only over an *existing* IP network, so it is
  circular here.
- If a machine *does* have a UDC (e.g. a firmware-enabled Intel xDCI
  bound by `dwc3-pci`), you could pass it into a VM. At that point you are
  back to gadget mode and gain nothing from the VM.

**`dummy_hcd` is still useful as a development rig, and needs no VM.** This
kernel ships `dummy_hcd` and `raw_gadget` as modules. Load
`dummy_hcd is_super_speed=1` and write a tiny userspace raw_gadget that
presents **exactly DbC's descriptors**:
- `1d6b:0010` (or our custom PID)
- one interface with one bulk IN and one bulk OUT, 1024-byte packets

The local host stack then enumerates it. That lets us develop and fuzz the
*host-side* usbnet minidriver and the framing with no DbC hardware and no
special cable.
- `is_super_speed=1` matters. Without it the bus is high-speed with 512-byte
  bulk packets, so the impostor wouldn't match DbC's 1024 and the ZLP edge
  case would never be exercised.
- A VM only helps by keeping a crashing in-development driver off the
  workstation.
- The target-side DbC netdev still needs real hardware.

---

## 6. Recommendation

1. **Right now, no code:** buy a PL-27A1 cable (~1.5 Gb/s, `plusb`, works
   anywhere), or use two 2.5G USB-Ethernet adapters. If both machines have
   USB4, use `thunderbolt_net`.
2. **The project worth building is "usb-net" = a network link over xHCI
   DbC.** No chip in the middle, ~1–2 Gb/s+, with the target needing only a
   DbC-capable xHCI and a debug cable. Phases:
   1. **Probe**: confirm DbC exists on a candidate target (debugfs
      `reg-ext-dbc`), enable `CONFIG_USB_XHCI_DBGCAP`, and get
      `/dev/ttyDBC0` ↔ `/dev/ttyUSB0` echoing.
   2. **Userspace prototype, zero kernel code**:
      - Target: TUN ↔ `/dev/ttyDBC0` in raw mode.
      - Host: TUN ↔ libusb bulk endpoints, as Celadon does.
      - Set a custom `dbc_idProduct` before enabling DbC. Then `usb_debug`
        never binds, so there is nothing to unbind.
      - A tty loses transfer boundaries, so frame packets with a length
        prefix (or COBS).
      - Measure with iperf3 against the 6.14 numbers.
   3. **Host side in kernel**: a `cdc_subset`-style usbnet minidriver
      (bulk-only, `FLAG_POINTTOPOINT`) matched on a custom `dbc_idProduct`.
      Develop it against the dummy_hcd/raw_gadget DbC impostor (§5).
   4. **Target side in kernel**: a `dbc_net` function next to
      `xhci-dbgtty.c` behind the existing `struct dbc_driver` hook. This
      needs a small upstream change so the function can be selected instead
      of the hardcoded `xhci_dbc_tty_probe()`. Batch frames NTB-style into
      large transfers, and send a ZLP when a transfer is an exact multiple
      of 1024.
3. **Pure USB 2.0 with no special cable is impossible.** Something has to be
   a device. On USB 2.0 that something can only be a bridge chip, since DbC
   is SuperSpeed-only and EHCI's debug port needs an external debug device.

---

## 7. Sources

Local:
- `modinfo plusb net1080 gl620a cdc_subset usb_debug thunderbolt_net`
- `/proc/config.gz` on Linux 7.2.9

Kernel:
- [`drivers/usb/host/xhci-dbgcap.h`](https://github.com/torvalds/linux/blob/master/drivers/usb/host/xhci-dbgcap.h)
  and [`xhci-dbgcap.c`](https://github.com/torvalds/linux/blob/master/drivers/usb/host/xhci-dbgcap.c)
- [sysfs ABI: `sysfs-bus-pci-drivers-xhci_hcd`](https://kernel.org/doc/Documentation/ABI/testing/sysfs-bus-pci-drivers-xhci_hcd)
- [USB3 debug port guide](https://kernel.org/doc/html/v6.4/driver-api/usb/usb3-debug-port.html)
- [`drivers/usb/early/xhci-dbc.c`](https://github.com/torvalds/linux/blob/master/drivers/usb/early/xhci-dbc.c)
- [DbC performance series, patch 2/3](https://patchwork.kernel.org/project/linux-usb/patch/20241227120142.1035206-2-mathias.nyman@linux.intel.com/mbox)
  and [series mbox](https://patchwork.kernel.org/series/921107/mbox)
- [plusb PL-27A1 patch, 2016 (1.5 Gbps iperf)](https://lists.openwall.net/linux-kernel/2016/02/21/38)
  and [2017 patch](https://lists.openwall.net/linux-kernel/2017/04/20/401)
- [USB4 and Thunderbolt admin guide](https://www.kernel.org/doc/html/latest/admin-guide/thunderbolt.html)

Prior art:
- [Celadon: ADB over DbC](https://projectceladon.github.io/celadon-documentation/tutorials/dbc.html)
- [Microsoft: KDNET over USB](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/setting-up-a-kdnet-usb-connection)
- [xue DbC driver](https://github.com/connojd/xue)
- [Microsoft: USB4 inter-domain connections](https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/usb4-interdomain-connections)
- [Framework forum: USB4 networking throughput reports](https://community.frame.work/t/how-come-no-one-seems-to-have-tried-using-thunderbolt-for-40gbps-networking/75470?page=2)
- [Prolific PL27A1](https://prolificusa.com/product/pl27a1-super-speed-usb-host-host-bridge-controller)
- [ThinkPenguin TPE-3TRANCBL](https://www.thinkpenguin.com/gnu-linux/usb-30-data-transfer-cable-gnulinux-tpe-3trancbl)
- [mac-windows-usbc-bridge](https://github.com/Manveer-Singh-99/mac-windows-usbc-bridge):
  Mac in DRD gadget mode. Not host-to-host; cited only to rule it out.
- [QEMU `hw/usb/hcd-dwc2.c`](https://github.com/qemu/qemu/blob/master/hw/usb/hcd-dwc2.c)
  (host-only) and [`hcd-xhci.c`](https://github.com/qemu/qemu/blob/master/hw/usb/hcd-xhci.c)
