# The Kconfig side of linux-dbc-net.patch, which the out-of-tree module build
# bypasses with -D flags. On a full kernel tree, three configs:
#   default  USB_XHCI_DBGCAP alone, as existing configs have it: ttyDBC is
#            still built and dbc_net is not;
#   both     both functions;
#   notty    a kernel without the tty layer (TTY=n), where DbC used to be
#            impossible: dbc_net alone, and nothing that needs a tty.
# Each builds drivers/usb/host and checks which DbC objects came out.
{
  lib,
  stdenv,
  linux,
  bc,
  bison,
  flex,
  perl,
  openssl,
  elfutils,
  pkg-config,
  ripgrep,
}:
stdenv.mkDerivation {
  name = "dbc-kconfig-split";
  inherit (linux) src;

  patches = [
    ./linux-dbc-ring-dequeue.patch
    ./linux-dbc-net.patch
  ];

  nativeBuildInputs = [
    bc
    bison
    flex
    perl
    openssl
    elfutils
    pkg-config
    ripgrep
  ];

  postPatch = ''
    patchShebangs scripts
  '';

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild

    # config <name> <scripts/config args...>
    config() {
      local o=build-$1
      shift
      make O=$o x86_64_defconfig >/dev/null
      scripts/config --file $o/.config -d DEBUG_INFO_BTF -e USB_XHCI_HCD "$@"
      make O=$o olddefconfig >/dev/null
    }

    # state <name> <symbol> <expected>
    state() {
      local got
      got=$(scripts/config --file build-$1/.config --state $2)
      # "is not set" and absent mean the same here.
      [ "$got" = undef ] && got=n
      echo "$1: $2=$got"
      [ "$got" = "$3" ] || { echo "$1: expected $2=$3" >&2; exit 1; }
    }

    # objs <name> <built...> -- <absent...>
    objs() {
      local o=build-$1 absent=
      shift
      make O=$o -j$NIX_BUILD_CORES drivers/usb/host/ >/dev/null
      for f in "$@"; do
        if [ "$f" = -- ]; then absent=1; continue; fi
        if [ -n "$absent" ]; then
          [ ! -e $o/drivers/usb/host/$f ] || { echo "$o: $f was built" >&2; exit 1; }
        else
          [ -e $o/drivers/usb/host/$f ] || { echo "$o: $f missing" >&2; exit 1; }
        fi
      done
    }

    config default -e USB_XHCI_DBGCAP
    state default USB_XHCI_DBGCAP_TTY y
    state default USB_XHCI_DBGCAP_NET n
    objs default xhci-dbgcap.o xhci-dbgtty.o -- xhci-dbgnet.o

    config both -e USB_XHCI_DBGCAP -e USB_XHCI_DBGCAP_NET
    state both USB_XHCI_DBGCAP_TTY y
    state both USB_XHCI_DBGCAP_NET y
    objs both xhci-dbgcap.o xhci-dbgtty.o xhci-dbgnet.o

    config notty -e EXPERT -d TTY -e USB_XHCI_DBGCAP -e USB_XHCI_DBGCAP_NET
    state notty TTY n
    state notty USB_XHCI_DBGCAP y
    state notty USB_XHCI_DBGCAP_TTY n
    state notty USB_XHCI_DBGCAP_NET y
    objs notty xhci-dbgcap.o xhci-dbgnet.o -- xhci-dbgtty.o
    if nm -u build-notty/drivers/usb/host/xhci-dbgcap.o | rg -w 'dbc_tty_\w+|tty_\w+'; then
      echo "notty: the DbC core still references the tty function" >&2
      exit 1
    fi

    runHook postBuild
  '';

  installPhase = ''
    touch $out
  '';

  meta.platforms = [ "x86_64-linux" ];
}
