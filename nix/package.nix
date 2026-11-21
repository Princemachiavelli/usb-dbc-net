{
  lib,
  stdenv,
  pkg-config,
  libusb1,
}:
stdenv.mkDerivation {
  pname = "dbcnet";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ../src;
    fileset = ../src;
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libusb1 ];

  makeFlags = [ "PREFIX=${placeholder "out"}" ];
  env.NIX_CFLAGS_COMPILE = "-Werror";
  doCheck = true;

  meta = {
    description = "Userspace dbcnet link: DbC impostor gadget and libusb debug-host side";
    platforms = lib.platforms.linux;
    mainProgram = "dbcnet-host";
  };
}
