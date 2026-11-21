# The target: its xHCI's Debug Capability becomes a network interface, dbc0,
# through dbc_net. Enable DbC with `echo enable > /sys/bus/pci/devices/<xHCI>/dbc`.
{ config, lib, ... }:
let
  cfg = config.hardware.dbc-net;
in
{
  options.hardware.dbc-net.tty = lib.mkEnableOption ''
    ttyDBC beside dbc_net. dbc_net stays the default; to switch a controller
    to the tty, write 0 to /sys/module/xhci_hcd/parameters/dbc_net and rebind
    the controller. Without this, the module has no tty code at all
  '';

  options.hardware.dbc-net.patches = lib.mkOption {
    type = lib.types.nullOr (lib.types.listOf lib.types.path);
    default = null;
    description = ''
      The DbC series for xhci-hcd, if not the 6.18 one in nix/: for
      another kernel, such as mainline with nix/mainline-patches.nix.
    '';
  };

  config = {
    boot.extraModulePackages = [
      (config.boot.kernelPackages.callPackage ./xhci-hcd-dbc.nix (
        { inherit (cfg) tty; } // lib.optionalAttrs (cfg.patches != null) { dbcPatches = cfg.patches; }
      ))
    ];
    boot.kernelParams = lib.optional cfg.tty "xhci_hcd.dbc_net=1";
  };
}
