# The QEMU the NixOS tests run, with xHCI Debug Capability emulation: the
# series in upstream/qemu, developed against the same QEMU release.
{ qemu_test }:
qemu_test.overrideAttrs (old: {
  pname = "qemu-dbc";
  patches = (old.patches or [ ]) ++ [
    ../upstream/qemu/0001-hw-usb-xHCI-Debug-Capability-emulation-and-the-usb-d.patch
    ../upstream/qemu/0002-hw-usb-hcd-xhci-dbc-migrate-the-Debug-Capability.patch
    ../upstream/qemu/0003-tests-qtest-usb-hcd-xhci-test-split-starting-QEMU-fr.patch
    ../upstream/qemu/0004-tests-qtest-usb-hcd-xhci-test-test-the-xHCI-Debug-Ca.patch
    ../upstream/qemu/0005-docs-system-devices-usb-document-the-xHCI-Debug-Capa.patch
  ];
})
