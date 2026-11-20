# usb-net

IP networking between two ordinary PCs over a USB cable, with no USB gadget
mode (no UDC) on either side.

- [`docs/RESEARCH.md`](docs/RESEARCH.md) covers the options: bridge cables,
  xHCI Debug Capability, USB4. It also covers prior art, why a VM can't play
  the device side, and the plan.
- To check whether a controller has the Debug Capability, list xhci's
  debugfs; a `reg-ext-dbc:NN` entry means DbC is present:
  `sudo sh -c 'ls -d /sys/kernel/debug/usb/xhci/*/reg-ext-*'`.
  [`scripts/probe-dbc.py`](scripts/probe-dbc.py) is a fallback that walks
  the registers directly. It needs the BAR to be mappable, which strict
  devmem blocks while `xhci_hcd` is bound.
