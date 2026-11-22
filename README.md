# usb-net

IP networking between two ordinary PCs over a USB cable, with no USB gadget
mode (no UDC) on either side. The approach is a network link over the xHCI
Debug Capability (DbC).

## Docs

- [`docs/RESEARCH.md`](docs/RESEARCH.md) covers the options: bridge cables,
  xHCI DbC, USB4. It also covers prior art, why a VM can't play the device
  side, and the plan.
- [`docs/QEMU-DBC.md`](docs/QEMU-DBC.md) covers DbC emulation for QEMU,
  which lets the target's kernel code run in a VM: the scoping, the
  result, and the bugs found while building it.
- [`docs/KERNEL.md`](docs/KERNEL.md) covers the in-kernel link (`dbc_net`
  on the target, `dbcnet.ko` on the debug host): its design, how to use
  it, and throughput against the tty and PPP paths.

## The kernel link

- **Target: `dbc_net`.** The DbC becomes a network interface, `dbc0`, in
  place of ttyDBC.
  - It is a patch to `xhci-hcd` ([`nix/linux-dbc-net.patch`](nix/linux-dbc-net.patch)).
    The patch also splits ttyDBC into its own Kconfig option, so DbC no
    longer needs the tty layer.
  - [`nix/xhci-hcd-dbc.nix`](nix/xhci-hcd-dbc.nix) builds just that module
    for a stock kernel, with DbC on and the ring fix. No kernel rebuild is
    needed.
  - By default `dbc_net` is the module's only DbC function. `tty = true`
    adds ttyDBC back, selectable with `xhci_hcd.dbc_net`.
- **Debug host: `dbcnet.ko`** ([`kmod/dbcnet.c`](kmod/dbcnet.c)), a usbnet
  minidriver.
  - `dbc_net` presents itself as `1d6b:0012` (provisional) where ttyDBC is
    `1d6b:0010`. So `dbcnet.ko` autoloads for it, and `usb_debug` never
    claims it.
- **On NixOS:** use `nixosModules.target` on the target and
  `nixosModules.debug-host` on the host. `hardware.dbc-net.tty = true`
  keeps ttyDBC available on the target.

## Userspace tools (`src/`)

These are the prototype, now the fallbacks:
- **`dbcnet-host`** is the debug-host end without `dbcnet.ko`.
  - It finds the DbC device by VID:PID (by default `1d6b:0012`, `dbc_net`,
    or `1d6b:0010`, ttyDBC), takes the
    interface from whichever driver holds it with libusb, and bridges its
    bulk endpoints to a TUN interface.
  - It reconnects when the device comes back.
- **`dbcnet-tty`** is the target end on a kernel without `dbc_net`.
  - It bridges the kernel's `/dev/ttyDBC0` to TUN, and reopens the tty
    after each disconnect.
  - DbC cannot send a ZLP, so a batch ending on a 1024-byte boundary gets
    a zero-length frame appended.
- **`dbc-impostor`** is a raw_gadget device that copies DbC's descriptors:
  `1d6b:0010`, one interface, and EP1 bulk OUT/IN at 1024 bytes with
  SuperSpeed companions.
  - It bridges to TUN the same way, standing in for real DbC hardware on
    `dummy_hcd`.
  - `--pid 0012 --protocol 0` makes it look like `dbc_net`.
- All of them, and both kernel ends, use the same framing (`src/frame.h`):
  - a 4-byte header (magic, length) per IP packet, as a byte stream, so it
    also works through `/dev/ttyDBC0`;
  - batched up to 32 KiB per transfer;
  - every transfer ends on a short packet: the debug host sends a ZLP, and
    the target, which cannot send one, pads.

## Checks

```
nix build .#checks.x86_64-linux.dbcnet        # build + framing unit tests
nix build -L .#checks.x86_64-linux.vm-impostor
nix build -L .#checks.x86_64-linux.vm-dbc     # patched QEMU, stock kernel
nix build -L .#checks.x86_64-linux.vm-dbc-notty
nix build -L .#checks.x86_64-linux.kconfig-split  # the patch's Kconfig, in-tree
```

Each VM test runs both ends in one NixOS VM: the target in a `target`
network namespace, the debug host in the default one. Every link they bring
up gets the shared checks (`nix/vm-common.py`):
- pings both ways at frame sizes 64–9004 bytes, including exact multiples
  of 1024;
- iperf3 in both directions, reported but not asserted.

`vm-dbc` runs the guest's DbC on emulated hardware, using QEMU with
the QEMU series in [`upstream/qemu/`](upstream/qemu). Its target module carries
both functions. It checks, in order:
1. **`dbc_net` + `dbcnet.ko`**, through the two NixOS modules on a stock
   kernel:
   - the interface exists before DbC is enabled, without carrier;
   - enumeration at SuperSpeed;
   - `dbcnet.ko` autoloads for the device, and `usb_debug` can't bind it;
   - reconnects after DbC disable and after a cable unplug.
2. **`dbc_net` + `dbcnet-host`:** the same target from userspace, and that
   the target needs the host's ZLP.
3. **The tty path**, after rebinding the controller with `dbc_net` off:
   - ttyDBC0 ↔ ttyUSB0 carries 1 MiB each way;
   - `dbcnet-tty` works, and a target that stops reading for 4 s mid-iperf
     loses nothing;
   - the padding that replaces DbC's missing ZLP is needed;
   - stock PPP over ttyDBC0 ↔ ttyUSB0 runs, as a baseline.

Between parts 2 and 3, both `vm-dbc` variants also carry a USB stick on
the target to the debug host with USB/IP over `dbc0`, and read it back
(see [`docs/KERNEL.md`](docs/KERNEL.md)).

`vm-dbc-notty` runs parts 1 and 2 on the default, tty-free module. It
also checks that no tty function exists: there is no `dbc_serial` tty
driver and no `dbc_net` parameter, and rebinding the controller brings
`dbc_net` back, not ttyDBC.

`vm-impostor` uses `dummy_hcd is_super_speed=1` + `raw_gadget`, with
`dbc-impostor` as the target. It checks:
- enumeration at SuperSpeed, with `usb_debug` binding as it does on real
  DbC;
- that turning the host's ZLP off breaks a 1024-byte frame;
- reconnects;
- `dbcnet.ko` against the impostor presenting as `dbc_net` (`1d6b:0012`).

## Hardware probe

To check whether a controller has the Debug Capability:
`sudo sh -c 'ls -d /sys/kernel/debug/usb/xhci/*/reg-ext-*'`. A
`reg-ext-dbc:NN` entry means DbC is present.
[`scripts/probe-dbc.py`](scripts/probe-dbc.py) is a fallback that walks the
registers directly. It needs the BAR to be mappable, which strict devmem
blocks while `xhci_hcd` is bound.
