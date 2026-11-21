# usb-net

IP networking between two ordinary PCs over a USB cable, with no USB gadget
mode (no UDC) on either side. The approach is a network link over the xHCI
Debug Capability (DbC).

## Docs

- [`docs/RESEARCH.md`](docs/RESEARCH.md) covers the options: bridge cables,
  xHCI DbC, USB4. It also covers prior art, why a VM can't play the device
  side, and the plan.
- [`docs/QEMU-DBC.md`](docs/QEMU-DBC.md) scopes DbC emulation for QEMU, so
  the real target-side kernel code can be tested in a VM. It also records
  the result: the emulation is built and running, and the bugs found while
  building it.

## Userspace prototype (`src/`)

- **`dbcnet-host`** is the debug-host end.
  - It finds the DbC device by VID:PID (default `1d6b:0010`), takes the
    interface from `usb_debug` with libusb, and bridges its bulk endpoints
    to a TUN interface.
  - It reconnects when the device comes back.
- **`dbc-impostor`** is a raw_gadget device that copies DbC's descriptors:
  `1d6b:0010`, one interface, and EP1 bulk OUT/IN at 1024 bytes with
  SuperSpeed companions. It bridges to TUN the same way. It stands in for
  real DbC hardware on `dummy_hcd`.
- **`dbcnet-tty`** is the target end on real DbC.
  - It bridges the kernel's `/dev/ttyDBC0` to TUN, and reopens the tty
    after each disconnect.
  - DbC cannot send a ZLP, so a batch ending on a 1024-byte boundary gets
    a zero-length frame appended.
- Both use the framing in `src/frame.h`:
  - a 4-byte header (magic, length) per IP packet, as a byte stream, so it
    also works through `/dev/ttyDBC0`
  - batched up to 32 KiB per transfer
  - a ZLP whenever a transfer ends on a packet boundary

## Checks

```
nix build .#checks.x86_64-linux.dbcnet        # build + framing unit tests
nix build -L .#checks.x86_64-linux.vm-impostor
nix build -L .#checks.x86_64-linux.vm-dbc     # patched QEMU + guest kernel
```

Both VM tests run in one NixOS VM, with the target end in a `target`
network namespace and `dbcnet-host` in the default one. They share their
checks (`nix/vm-common.py`):
- pings both ways at frame sizes 64–9004 bytes, including exact multiples
  of 1024
- iperf3 in both directions, reported but not asserted
- that the host side reconnects after the device disappears

`vm-impostor` uses `dummy_hcd is_super_speed=1` + `raw_gadget`, with
`dbc-impostor` as the target. It also checks that:
- the device enumerates at SuperSpeed, with `usb_debug` binding as it does
  on real DbC;
- turning the host's ZLP off breaks a 1024-byte frame, so the test really
  exercises the edge.

`vm-dbc` runs the guest kernel's own DbC driver on emulated hardware:
- QEMU with [`qemu/xhci-dbc.patch`](qemu/xhci-dbc.patch), which adds
  `qemu-xhci,dbc=on` and a `usb-dbc-peer` cable end.
- A kernel with `CONFIG_USB_XHCI_DBGCAP` and
  [`nix/linux-dbc-ring-dequeue.patch`](nix/linux-dbc-ring-dequeue.patch),
  a fix for DbC rings that fill after one lap on kernels after 6.12.
- The target is ttyDBC bridged by `dbcnet-tty`.
- It also checks:
  - a raw ttyDBC0 ↔ ttyUSB0 transfer;
  - a target that stops reading for 4 s mid-iperf, which must lose nothing;
  - the padding that replaces the ZLP DbC cannot send;
  - reconnects after DbC disable and after a cable unplug.

See [`docs/QEMU-DBC.md`](docs/QEMU-DBC.md) for what the emulation found.

## Hardware probe

To check whether a controller has the Debug Capability:
`sudo sh -c 'ls -d /sys/kernel/debug/usb/xhci/*/reg-ext-*'`. A
`reg-ext-dbc:NN` entry means DbC is present.
[`scripts/probe-dbc.py`](scripts/probe-dbc.py) is a fallback that walks the
registers directly. It needs the BAR to be mappable, which strict devmem
blocks while `xhci_hcd` is bound.
