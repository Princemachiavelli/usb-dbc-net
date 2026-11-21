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
  dbcPatches = import ./mainline-patches.nix;
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
