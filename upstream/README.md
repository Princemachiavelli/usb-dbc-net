# Upstream patches (not sent)

Generated with `git format-patch`. The top level is what would be sent,
against mainline (7.4-rc3). `v6.18/` is the same series on Linux 6.18.64.
That's what `nix/` builds and the VM tests run.

| Directory | For | Contents |
|---|---|---|
| `fixes/` | linux-usb, stable | Two DbC core fixes: rings that fill after one lap (`Fixes: 7ee645963075`), and a request leak when DbC is disabled (`Fixes: efe3e3ae5a66`) |
| `dbc-net/` | linux-usb, RFC | On top of the fixes: a `remove` op, ttyDBC under its own Kconfig option, then `dbc_net`, which asks for a product ID (0012 provisional) |
| `dbcnet-host/` | netdev, RFC | The debug host's usbnet driver, `drivers/net/usb/dbcnet.c` |

How they're checked:
- **`checks.mainline`** applies the top level to nixpkgs' `linux_testing`
  (7.4-rc3, the same as mainline for every file the series touches). It:
  - runs the Kconfig check in a full tree, which builds `dbcnet`;
  - builds `xhci-hcd` both with and without ttyDBC.
  
  It fails once `linux_testing` moves on and a patch stops applying.
- **`checks.kconfig-split` and the VM tests** do the same for `v6.18/`.
- **`checks.vm-dbc-mainline` and `vm-dbc-mainline-notty`** boot 7.4-rc3
  with the top-level series and run the whole VM test on it, including
  DbC disabled and re-enabled under traffic. This is the stop fix exactly
  as it would be sent. The one thing not run there is `dbcnet.c`'s
  `kzalloc_obj()` variant; the tests load `kmod/dbcnet.c`, which differs
  from it only in that allocation.
- **Each step of the series builds on its own,** on both bases.
- **checkpatch (7.4-rc3)** has no errors. The remaining warnings are:
  - the `Co-Authored-By` trailers;
  - MAINTAINERS for the two new files;
  - "unknown commit id", only because it ran without a git tree. Both
    ids were confirmed on GitHub.

Differences between the two bases:
- **`xhci_dbc_stop()`:** mainline rewrote it (runtime PM, serialised
  enable and disable), so the stop fix there is a port with the same
  logic.
- **Strings:** mainline added configurable DbC strings beside the
  `dbc_net` parameter.
- **`kzalloc_obj()`:** `dbcnet.c` uses it on mainline, as checkpatch now
  asks. `kmod/dbcnet.c` and `v6.18/` keep `kzalloc()`.

Not needed: a `usb_debug` change. With `dbc_net` on its own product ID,
`usb_debug`'s match on 0010 and 0011 never sees it.

Not here: QEMU's DbC emulation, on branch `dbc` of `~/srcs/qemu-dbc`. QEMU
can't take it as written: `docs/devel/code-provenance.rst` says the project
declines contributions that include or derive from AI-generated content,
naming Claude. It stays a local test fixture.

The kernel's rules for AI-assisted patches are in 7.4-rc3's
`Documentation/process/coding-assistants.rst` and `generated-content.rst`.
They are followed here:
- **No `Signed-off-by:`.** An AI agent must not add one; only a human can
  certify the DCO.
- **Disclosure.** Each changelog says what the assistant did and how the
  patch was tested, and each cover letter has a "Tool use" paragraph:
  which tool, what the prompts were, which parts it wrote, how it was
  tested.
- **What couldn't be done is stated.** The stop fix has no reproducer.
  All testing was in emulation.
- **Classification.** Both fixes are regular bugs under `threat-model.rst`,
  not vulnerabilities. Only root (the `dbc` sysfs attribute), suspend or
  removal can trigger the stop race.

Before sending, which only you can do:
- **Review everything, then sign off.** `generated-content.rst`: "You are
  expected to understand and to be able to defend everything you submit."
  Add your `Signed-off-by:` only once you can, and check the From:
  address.
- **Edit the "Tool use" paragraphs.** They describe the work as the
  assistant saw it. Add your own part: what you reviewed, changed or
  directed.
- **Choose the AI trailer.**
  - These carry `Co-Authored-By: Claude …`, your usual choice. It isn't a
    kernel tag: `submitting-patches.rst` says such extra tags are "just
    ... ignored for now", and checkpatch warns about it.
  - `Co-developed-by:` doesn't fit, because it must be followed by that
    co-author's `Signed-off-by:`, which an AI may not add.
  - The disclosure the rules ask for is already in each changelog, so
    dropping the trailer would lose nothing.
- **Re-check against `usb-next` and `net-next` just before sending.** As of
  2026-11-21:
  - `usb-next` matches 7.4-rc3 byte for byte in every file `fixes/` and
    `dbc-net/` touch;
  - `dbcnet-host/` applies to `net-next` with line offsets only. `net-next`
    split the ax88179 driver.
- **Decide on a MAINTAINERS entry** for `dbcnet.c` and `xhci-dbgnet.c`.
  One would name you.
- **Find the recipients** with `scripts/get_maintainer.pl`:
  - xHCI is Mathias Nyman, on linux-usb@vger.kernel.org;
  - the usbnet driver goes to netdev@vger.kernel.org;
  - everything also goes to linux-kernel@vger.kernel.org.
  
  The cover letters and the host patch carry `base-commit:` (v7.4-rc3).
  Regenerate with `git format-patch --base` if you rebase.
- **Test on real hardware,** as the RFC's cover letter promises before a
  non-RFC posting.
