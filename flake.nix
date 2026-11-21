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
      });

      checks = forAllSystems (
        pkgs:
        let
          inherit (self.packages.${pkgs.stdenv.hostPlatform.system}) dbcnet;
        in
        {
          # Builds and runs the framing unit tests.
          inherit dbcnet;
          vm-impostor = pkgs.callPackage ./nix/vm-impostor.nix { inherit dbcnet; };
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
