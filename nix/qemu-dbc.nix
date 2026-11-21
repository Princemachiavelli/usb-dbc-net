# The QEMU the NixOS tests run, with xHCI Debug Capability emulation
# (qemu/xhci-dbc.patch, developed against the same QEMU release).
{ qemu_test }:
qemu_test.overrideAttrs (old: {
  pname = "qemu-dbc";
  patches = (old.patches or [ ]) ++ [ ../qemu/xhci-dbc.patch ];
})
