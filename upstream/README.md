# Upstream patches (not sent)

Generated with `git format-patch` from the same tree as the patches in
`nix/`. Applied in order, they give exactly the code the checks test.
Base: Linux 6.18.64.

| Directory | For | Contents |
|---|---|---|
| `fixes/` | linux-usb, stable | Two DbC core fixes: rings that fill after one lap (since 7ee645963075), and a request leak when DbC is disabled |
| `dbc-net/` | linux-usb, RFC | On top of the fixes: a `remove` op, ttyDBC under its own Kconfig option, then `dbc_net`, which asks for a product ID (0012 provisional) |
| `dbcnet-host/` | netdev, RFC | The debug host's usbnet driver, `drivers/net/usb/dbcnet.c` |

Each step of `fixes/` and `dbc-net/` builds. `checks.kconfig-split`
builds the result, plus `dbcnet-host/`, in a full kernel tree.

Not needed: a `usb_debug` change. With `dbc_net` on its own product ID,
`usb_debug`'s match on 0010 and 0011 never sees it.

Not here: QEMU's DbC emulation, on branch `dbc` of `~/srcs/qemu-dbc`. QEMU
can't take it as written: `docs/devel/code-provenance.rst` says the project
declines contributions that include or derive from AI-generated content,
naming Claude. It stays a local test fixture.

Before sending:
- **Rebase onto `usb-next`** (Greg KH's usb tree) and `net-next`, and
  rebuild. They were made against 6.18.64, not mainline.
- **Read them, and check the From: address and sign-off.** They carry
  `Signed-off-by:` with the local git identity, which certifies the DCO
  for you.
- **Choose the trailer for the AI assistance.** These use
  `Co-Authored-By:`. checkpatch prefers `Co-authored-by:`, and the kernel
  may have its own rule for tool-assisted patches, so check
  `Documentation/process/` first.
- **Add a `Fixes:` tag to the stop fix.** Find the commit that introduced
  the flush-before-stop order in `xhci_dbc_stop()` with `git blame`.
- **Run `checkpatch.pl` in a full git tree**, so that it can resolve the
  `Fixes:` ids.
- **Decide on a MAINTAINERS entry** for `dbcnet.c` and `xhci-dbgnet.c`.
  One would name you.
- **Find the recipients** with `scripts/get_maintainer.pl`: xHCI is Mathias
  Nyman, on linux-usb@vger.kernel.org; the usbnet driver goes to
  netdev@vger.kernel.org.
- **Consider testing on real hardware first,** as the cover letter
  promises before a non-RFC posting.
