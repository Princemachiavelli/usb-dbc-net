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
        # Not callPackage: linuxPackagesFor needs the kernel's own .override.
        linux-dbgcap = import ./nix/linux-dbgcap.nix { inherit (pkgs) lib linuxPackages; };
        qemu-dbc = pkgs.callPackage ./nix/qemu-dbc.nix { };
      });

      checks = forAllSystems (
        pkgs:
        let
          inherit (self.packages.${pkgs.stdenv.hostPlatform.system}) dbcnet linux-dbgcap qemu-dbc;
        in
        {
          # Builds and runs the framing unit tests.
          inherit dbcnet;
          vm-impostor = pkgs.callPackage ./nix/vm-impostor.nix { inherit dbcnet; };
          vm-dbc = pkgs.callPackage ./nix/vm-dbc.nix { inherit dbcnet linux-dbgcap qemu-dbc; };
        }
      );

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.dbcnet ];
          packages = [ pkgs.clang-tools ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
