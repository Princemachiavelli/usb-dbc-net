# dbcnet.ko, the debug host's kernel driver (kmod/dbcnet.c), out of tree.
# Use with boot.kernelPackages.callPackage, so it matches the running kernel.
{
  lib,
  stdenv,
  kernel,
  kernelModuleMakeFlags,
}:
stdenv.mkDerivation {
  pname = "dbcnet-kmod";
  inherit (kernel) version;

  src = lib.fileset.toSource {
    root = ../kmod;
    fileset = ../kmod;
  };

  nativeBuildInputs = kernel.moduleBuildDependencies;

  makeFlags = kernelModuleMakeFlags ++ [
    "-C"
    "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "M=$(PWD)"
  ];

  buildFlags = [ "modules" ];

  installFlags = [
    "INSTALL_MOD_PATH=${placeholder "out"}"
    "INSTALL_MOD_DIR=extra"
    "INSTALL_MOD_STRIP=1"
    "DEPMOD=true"
  ];
  installTargets = [ "modules_install" ];

  meta = {
    description = "usbnet driver for the debug host's end of a dbc_net link";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}
