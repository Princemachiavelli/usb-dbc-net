# The debug host: dbcnet.ko presents a dbc_net target as a network interface,
# dbc0. Loaded at boot so it is registered before usb_debug; the udev rule
# covers the case where usb_debug got there first.
{ config, pkgs, ... }:
{
  boot.extraModulePackages = [ (config.boot.kernelPackages.callPackage ./dbcnet-kmod.nix { }) ];
  boot.kernelModules = [ "dbcnet" ];
  services.udev.packages = [
    (pkgs.runCommand "dbcnet-udev-rules" { } ''
      install -D -m 444 ${../udev/90-dbcnet.rules} $out/lib/udev/rules.d/90-dbcnet.rules
      substituteInPlace $out/lib/udev/rules.d/90-dbcnet.rules --replace-fail /bin/sh ${pkgs.runtimeShell}
    '')
  ];
}
