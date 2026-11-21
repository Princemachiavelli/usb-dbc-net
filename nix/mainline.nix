# upstream/'s series as it would be sent, on mainline as nixpkgs' linux_testing
# has it: the Kconfig check in a full tree, with the host driver, and xhci-hcd
# built with and without ttyDBC. When linux_testing moves on and a patch stops
# applying, that is the signal to rebase upstream/.
{
  callPackage,
  linkFarm,
  linux_testing,
  linuxPackages_testing,
}:
let
  dbcPatches = [
    ../upstream/fixes/0001-usb-xhci-dbc-advance-the-transfer-ring-s-dequeue-poi.patch
    ../upstream/fixes/0002-usb-xhci-dbc-stop-the-DbC-before-taking-its-requests.patch
    ../upstream/dbc-net/0001-usb-xhci-dbc-let-the-function-undo-its-own-probe.patch
    ../upstream/dbc-net/0002-usb-xhci-dbc-give-ttyDBC-its-own-Kconfig-option.patch
    ../upstream/dbc-net/0003-usb-xhci-dbc-add-a-network-function-dbc_net.patch
  ];
in
linkFarm "dbc-mainline" {
  kconfig-split = callPackage ./kconfig-split.nix {
    linux = linux_testing;
    inherit dbcPatches;
    hostPatch = ../upstream/dbcnet-host/0001-net-usb-add-dbcnet-the-debug-host-s-end-of-a-DbC-net.patch;
  };
  xhci-hcd-dbc = linuxPackages_testing.callPackage ./xhci-hcd-dbc.nix { inherit dbcPatches; };
  xhci-hcd-dbc-tty = linuxPackages_testing.callPackage ./xhci-hcd-dbc.nix {
    inherit dbcPatches;
    tty = true;
  };
}
