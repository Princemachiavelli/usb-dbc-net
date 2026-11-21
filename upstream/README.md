# Upstream patches (not sent)

Generated with `git format-patch` from the same tree as
[`nix/linux-dbc-ring-dequeue.patch`](../nix/linux-dbc-ring-dequeue.patch)
and [`nix/linux-dbc-net.patch`](../nix/linux-dbc-net.patch). Applied in
order, they give exactly the code the checks test. Base: Linux 6.18.64,
`drivers/usb/host`.

- `ring-fix/` is a standalone fix, meant for stable: DbC transfer rings
  fill after one lap since 7ee645963075 ("usb: xhci: remove legacy
  'num_trbs_free' tracking").
- `dbc-net/` is an RFC: a `remove` op for DbC functions, then `dbc_net`
  with the Kconfig split. It applies on top of the fix.

Before sending:
- **Rebase onto `usb-next`** (Greg KH's usb tree) and rebuild. They were
  made against 6.18.64, not mainline.
- **Read them, and check the From: address and sign-off.** They carry
  `Signed-off-by:` with the local git identity, which certifies the DCO
  for you.
- **Choose the trailer for the AI assistance.** These use
  `Co-Authored-By:`. checkpatch prefers `Co-authored-by:`, and the kernel
  may have its own rule for tool-assisted patches, so check
  `Documentation/process/` first.
- **Run `checkpatch.pl` in a full git tree**, so that it can resolve the
  `Fixes:` id.
- **Find the recipients** with `scripts/get_maintainer.pl` (xHCI: Mathias
  Nyman; linux-usb@vger.kernel.org).
- **Consider testing on real hardware first,** as the cover letter
  promises before a non-RFC posting.
