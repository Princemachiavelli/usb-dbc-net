# usb-net

IP networking between two ordinary PCs over a USB cable, with no USB gadget
mode (no UDC) on either side. The approach is a network link over the xHCI
Debug Capability (DbC).

## Docs

- [`docs/RESEARCH.md`](docs/RESEARCH.md) covers the options: bridge cables,
  xHCI DbC, USB4. It also covers prior art, why a VM can't play the device
  side, and the plan.
- [`docs/QEMU-DBC.md`](docs/QEMU-DBC.md) scopes adding DbC emulation to QEMU,
  so the real target-side kernel code can be tested in a VM.

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
- Both use the framing in `src/frame.h`:
  - a 4-byte header (magic, length) per IP packet, as a byte stream, so it
    also works through `/dev/ttyDBC0`
  - batched up to 32 KiB per transfer
  - a ZLP whenever a transfer ends on a packet boundary

## Checks

```
nix build .#checks.x86_64-linux.dbcnet        # build + framing unit tests
nix build -L .#checks.x86_64-linux.vm-impostor
```

`vm-impostor` runs in one NixOS VM:
- `dummy_hcd is_super_speed=1` + `raw_gadget`, with the impostor in a
  `target` network namespace and `dbcnet-host` in the default namespace.
- It checks:
  - SuperSpeed enumeration, with `usb_debug` binding as it does on real DbC.
  - Pings both ways at frame sizes 64–9004 bytes, including exact multiples
    of 1024.
  - iperf3 in both directions, reported but not asserted.
  - That turning the host's ZLP off breaks a 1024-byte frame, so the test
    really exercises the edge.
  - That the host side reconnects after the device disappears.

## Hardware probe

To check whether a controller has the Debug Capability:
`sudo sh -c 'ls -d /sys/kernel/debug/usb/xhci/*/reg-ext-*'`. A
`reg-ext-dbc:NN` entry means DbC is present.
[`scripts/probe-dbc.py`](scripts/probe-dbc.py) is a fallback that walks the
registers directly. It needs the BAR to be mappable, which strict devmem
blocks while `xhci_hcd` is bound.
