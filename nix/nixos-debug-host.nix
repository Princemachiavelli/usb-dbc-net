# The debug host: dbcnet.ko presents a dbc_net target as a network interface,
# dbc0. It loads by modalias when the target enumerates.
{ config, ... }:
{
  boot.extraModulePackages = [ (config.boot.kernelPackages.callPackage ./dbcnet-kmod.nix { }) ];
}
