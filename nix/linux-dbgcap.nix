# The NixOS default kernel with the xHCI Debug Capability driver (xhci-dbgcap
# and ttyDBC, built into xhci_hcd), which NixOS kernels leave out.
#
# Since the DbC driver moved from its own free-TRB counter to
# xhci_num_trbs_free() (after 6.12), nothing advances its transfer rings'
# dequeue pointer, so each ring takes one lap of TRBs (~254 KiB through
# ttyDBC) and then every queue fails with "failed to queue trbs".
# linux-dbc-ring-dequeue.patch advances it on each transfer event.
{ lib, linuxPackages }:
linuxPackages.kernel.override {
  structuredExtraConfig = with lib.kernel; {
    USB_XHCI_DBGCAP = yes;
  };
  kernelPatches = linuxPackages.kernel.kernelPatches ++ [
    {
      name = "xhci-dbc-ring-dequeue";
      patch = ./linux-dbc-ring-dequeue.patch;
    }
  ];
}
