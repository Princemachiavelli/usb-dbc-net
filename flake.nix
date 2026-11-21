{
  description = "IP networking between two hosts over USB, with no gadget mode on either side";

  inputs.nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.zst";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      # raw_gadget, dummy_hcd, TUN and DbC are all Linux.
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: rec {
        dbcnet = pkgs.callPackage ./nix/package.nix { };
        default = dbcnet;
        # For the default kernel; NixOS configurations build their own.
        xhci-hcd-dbc = pkgs.linuxPackages.callPackage ./nix/xhci-hcd-dbc.nix { };
        dbcnet-kmod = pkgs.linuxPackages.callPackage ./nix/dbcnet-kmod.nix { };
        qemu-dbc = pkgs.callPackage ./nix/qemu-dbc.nix { };
      });

      checks = forAllSystems (
        pkgs:
        let
          inherit (self.packages.${pkgs.stdenv.hostPlatform.system}) dbcnet qemu-dbc;
        in
        {
          # Builds and runs the framing unit tests.
          inherit dbcnet;
          vm-impostor = pkgs.callPackage ./nix/vm-impostor.nix { inherit dbcnet; };
          kconfig-split = pkgs.callPackage ./nix/kconfig-split.nix { };
          vm-dbc = pkgs.callPackage ./nix/vm-dbc.nix { inherit dbcnet qemu-dbc; };
          vm-dbc-notty = pkgs.callPackage ./nix/vm-dbc.nix {
            inherit dbcnet qemu-dbc;
            tty = false;
          };
        }
      );

      nixosModules = {
        target = ./nix/nixos-target.nix;
        debug-host = ./nix/nixos-debug-host.nix;
      };

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.dbcnet ];
          packages = [ pkgs.clang-tools ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
