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
  # The full tree also takes the documentation patch, which the module
  # builds, having only drivers/usb/host, cannot.
  kconfig-split = callPackage ./kconfig-split.nix {
    linux = linux_testing;
    dbcPatches = dbcPatches ++ [
      ../upstream/dbc-net/0004-Documentation-usb-describe-dbc_net-the-DbC-network-f.patch
    ];
    hostPatch = ../upstream/dbcnet-host/0001-net-usb-add-dbcnet-the-debug-host-s-end-of-a-DbC-net.patch;
  };
  xhci-hcd-dbc = linuxPackages_testing.callPackage ./xhci-hcd-dbc.nix { inherit dbcPatches; };
  xhci-hcd-dbc-tty = linuxPackages_testing.callPackage ./xhci-hcd-dbc.nix {
    inherit dbcPatches;
    tty = true;
  };
}
