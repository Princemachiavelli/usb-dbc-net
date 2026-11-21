# xhci-hcd.ko, built out of tree for a stock kernel with the xHCI Debug
# Capability turned on and our DbC patches applied. Distribution kernels
# often build xhci_hcd as a module with CONFIG_USB_XHCI_DBGCAP off; nothing
# outside the module depends on that option, so a replacement module is
# enough and the kernel itself needs no rebuild. It installs under updates/,
# which depmod prefers over the kernel's own copy.
#
# By default DbC's only function is dbc_net and the module has no tty code.
# With tty = true, ttyDBC is built too and xhci_hcd.dbc_net picks one.
#
# Use with boot.kernelPackages.callPackage, so it matches the running kernel.
{
  lib,
  stdenv,
  kernel,
  kernelModuleMakeFlags,
  ripgrep,
  tty ? false,
  # The DbC series for this kernel; nix/mainline.nix passes upstream/'s.
  dbcPatches ? [
    ./linux-dbc-ring-dequeue.patch
    ./linux-dbc-stop.patch
    ./linux-dbc-net.patch
  ],
}:
let
  build = "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build";
in
stdenv.mkDerivation {
  pname = "xhci-hcd-dbc" + lib.optionalString tty "-tty";
  inherit (kernel) version src;

  # Only the xHCI sources; the tree the module builds against is kernel.dev.
  # Release kernels come as a tarball, rc kernels as a source tree.
  unpackPhase = ''
    runHook preUnpack
    mkdir source
    if [ -d $src ]; then
      mkdir -p source/drivers/usb
      cp -r $src/drivers/usb/host source/drivers/usb/
      chmod -R u+w source
    else
      tar xf $src -C source --strip-components=1 --wildcards '*/drivers/usb/host/*'
    fi
    cd source
    runHook postUnpack
  '';

  patches = dbcPatches;

  # Just xhci-hcd from the directory's Makefile, with DbC forced on. The
  # kernel's config has no say here, so the functions are set as flags too.
  postPatch = ''
    cat > drivers/usb/host/Kbuild <<'EOF'
    CFLAGS_xhci-trace.o := -I$(src)
    ccflags-y += -DCONFIG_USB_XHCI_DBGCAP -DCONFIG_USB_XHCI_DBGCAP_NET
    xhci-hcd-y := xhci.o xhci-mem.o xhci-ext-caps.o
    xhci-hcd-y += xhci-ring.o xhci-hub.o xhci-dbg.o xhci-trace.o
    xhci-hcd-y += xhci-dbgcap.o xhci-dbgnet.o
    ${lib.optionalString tty ''
      ccflags-y += -DCONFIG_USB_XHCI_DBGCAP_TTY
      xhci-hcd-y += xhci-dbgtty.o
    ''}
    xhci-hcd-$(CONFIG_DEBUG_FS) += xhci-debugfs.o
    xhci-hcd-$(CONFIG_USB_XHCI_SIDEBAND) += xhci-sideband.o
    obj-m := xhci-hcd.o
    EOF
  '';

  nativeBuildInputs = kernel.moduleBuildDependencies ++ [ ripgrep ];

  makeFlags = kernelModuleMakeFlags ++ [
    "-C"
    build
    "M=$(PWD)/drivers/usb/host"
  ];

  buildFlags = [ "modules" ];

  # Without the tty function, the module must not reach the tty layer at all.
  postBuild = lib.optionalString (!tty) ''
    if $NM -u drivers/usb/host/xhci-hcd.ko | rg -w 'tty_\w+'; then
      echo "xhci-hcd.ko still references the tty layer" >&2
      exit 1
    fi
  '';

  installFlags = [
    "INSTALL_MOD_PATH=${placeholder "out"}"
    "INSTALL_MOD_DIR=updates"
    "INSTALL_MOD_STRIP=1"
    "DEPMOD=true"
  ];
  installTargets = [ "modules_install" ];

  meta = {
    description =
      "xhci-hcd with the xHCI Debug Capability, its ring fix, and dbc_net"
      + lib.optionalString tty " beside ttyDBC";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}
