# The kernel link: dbc_net and dbcnet.ko

Both ends of the link now run in the kernel, with no tty and no userspace
in the data path:
- **Target:** `dbc_net`, a network interface on the xHCI Debug Capability's
  rings, in place of ttyDBC. It is a patch to `xhci-hcd`,
  [`nix/linux-dbc-net.patch`](../nix/linux-dbc-net.patch).
- **Debug host:** `dbcnet.ko`, a usbnet minidriver,
  [`kmod/dbcnet.c`](../kmod/dbcnet.c).

The framing on the wire is unchanged from the userspace prototype, so
`dbcnet-host` still drives a `dbc_net` target and `dbcnet.ko` drives the
impostor. The userspace pieces remain as fallbacks: `dbcnet-host` for hosts
without the module, and `dbcnet-tty` for targets whose kernel lacks
`dbc_net`.

Everything here is tested only against the QEMU emulation
([`QEMU-DBC.md`](QEMU-DBC.md)), not yet on real DbC hardware.

## Using it

On NixOS, import the flake's modules:
- `nixosModules.target` on the target. It replaces `xhci_hcd` with one
  whose only DbC function is `dbc_net`, with no tty code. To keep ttyDBC
  as an alternative, set `hardware.dbc-net.tty = true`. The module then
  carries both, and sets `xhci_hcd.dbc_net=1`.
- `nixosModules.debug-host` on the debug host. It installs `dbcnet.ko`,
  which loads by modalias when a `dbc_net` target enumerates.

Then, on the target:

```
echo 1 > /sys/bus/pci/devices/<xHCI>/dbc_poll_interval_ms   # optional, see below
echo enable > /sys/bus/pci/devices/<xHCI>/dbc
ip addr add 10.77.0.1/24 dev dbc0 && ip link set dbc0 mtu 9000 up
```

On the host, `dbc0` appears when the cable connects:

```
ip addr add 10.77.0.2/24 dev dbc0 && ip link set dbc0 mtu 9000 up
```

### Why `xhci-hcd` is rebuilt rather than the kernel

DbC code lives entirely inside `xhci-hcd.ko`, and distribution kernels often
build that as a module with `CONFIG_USB_XHCI_DBGCAP` off. Nothing outside
the module depends on that option: `struct xhci_hcd` keeps its `dbc` field
either way. So [`nix/xhci-hcd-dbc.nix`](../nix/xhci-hcd-dbc.nix) compiles
just `xhci-hcd` from the running kernel's source, with:
- DbC forced on;
- the ring fix ([`nix/linux-dbc-ring-dequeue.patch`](../nix/linux-dbc-ring-dequeue.patch)),
  without which a DbC ring fills after one lap on kernels after 6.12;
- `dbc_net`, and ttyDBC only with `tty = true`.

Without the tty, the build checks that the module references no `tty_*`
symbol. The tty build references 14.

It installs under `updates/`, which depmod prefers. That takes about a
minute, against a full kernel build. The tests use the stock nixpkgs
kernel.

## The target: dbc_net

### Choosing it

The DbC core hard-wired ttyDBC: `USB_XHCI_DBGCAP` depended on `TTY`, and
module init registered the tty driver. The patch:
- splits the functions into Kconfig options. `USB_XHCI_DBGCAP_TTY`
  defaults to y, so existing configs keep ttyDBC; `USB_XHCI_DBGCAP_NET` is
  new. `USB_XHCI_DBGCAP` itself now needs only `TTY || NET`.
- adds a `remove` op to `struct dbc_driver`, so the core can tear down
  whichever function it set up;
- with both functions built, adds a module parameter, `xhci_hcd.dbc_net`,
  that picks `dbc_net` when a controller is set up. With one built, there
  is no parameter.

The parameter is read at probe time. To switch a running controller, write
the parameter and then unbind and rebind the controller; `vm-dbc` does that
to reach the tty.

An upstream version would more likely be a sysfs attribute beside `dbc`.
The parameter was the smallest change that works out of tree.

### The interface

- `dbcN` is registered when the controller is set up, not when a debug host
  connects. It has no carrier until DbC is configured.
- So the interface, its namespace and its addresses outlive disconnects,
  DbC disable and cable unplugs. Only the carrier changes.
- It is raw IP: `ARPHRD_NONE`, point-to-point, with no ARP. The MTU defaults
  to 1500 and can go up to 65528.

### Its USB identity

- `dbc_net` presents the DbC as `1d6b:0012`. ttyDBC is `1d6b:0010`, and
  the early printk console is `1d6b:0011`; each Linux DbC function has its
  own product ID.
  - 0012 is unused but not assigned. Linux Foundation product IDs are
    reserved by the USB maintainers, so the RFC asks for one.
- The interface protocol is 0, "vendor defined" in the xHCI spec. ttyDBC
  uses 1, "GNU Remote Debug Command".
- The protocol can't identify the function, which an earlier version
  relied on. ttyDBC users can set it to 0 or 1 through
  `dbc_bInterfaceProtocol`, so a ttyDBC target at protocol 0 would have
  been claimed by `dbcnet.ko`.
- With its own product ID, `usb_debug` (which matches 0010 and 0011)
  never sees `dbc_net`. No udev rule, load-order trick or `usb_debug`
  patch is needed.
- `dbcnet-host` looks for 0012, then 0010, unless `--pid` says otherwise.

### Transmit

- Packets are copied into one of four 64 KiB buffers. The copy also fills in
  partial checksums, so the interface offers `HW_CSUM` and `SG`, and TCP
  hands over GSO trains.
- A buffer goes out when the stack has nothing more queued (`xmit_more`),
  when it fills, or when one of the three transfers in flight completes. So
  light traffic goes out one packet per transfer, and heavy traffic is
  batched.
- DbC cannot send a ZLP (Linux's DbC core rejects zero-length requests), so
  a transfer that would end on a 1024-byte boundary gets an empty frame
  appended.

### Receive

- Eight 64 KiB requests stay queued. Each 64 KiB buffer is one TRB: it is
  naturally aligned, so it crosses no 64 KiB line.
- Completions arrive from the DbC event work with interrupts off, so they
  only queue the request and schedule NAPI. That runs in process context,
  where `raise_softirq_irqoff()` wakes ksoftirqd, so the receive softirq is
  not left pending; the runs logged no "NOHZ tick-stop error".
- NAPI runs the stream decoder, delivers frames through GRO, and requeues
  the request.
- Because a receive TRB is 64 KiB, it completes only when it is full or on
  a short packet. The debug host must therefore end a transfer that is an
  exact multiple of 1024 bytes with a ZLP. Both `dbcnet.ko` and
  `dbcnet-host` do; `vm-dbc` checks that `dbcnet-host --no-zlp` stalls.

### Latency

DbC has no interrupts, so Linux polls its event ring:
- continuously while transfers are recent or a transmit is pending;
- otherwise every `dbc_poll_interval_ms`, 64 ms by default.

So the first packet after an idle spell can wait up to 64 ms. The tests set
1 ms. This is the DbC core's behaviour and applies to ttyDBC alike.

## The debug host: dbcnet.ko

- **Matching:** it binds `1d6b:0012` with interface class `dc`, and is
  autoloaded by modalias. `vm-dbc` checks that `usb_debug` can't be bound
  to a `dbc_net` target.
- **Interface:** usbnet's Ethernet device is turned into raw IP, the same
  way `qmi_wwan`'s raw-IP mode does it, and named `dbcN`.
- **Receive:** 16 KiB URBs, several in flight. The stream decoder keeps its
  state across URBs, since frames span them.
- **Transmit:** packets are collected while `xmit_more` says more follow, up
  to 32 KiB, and go out as one URB. `FLAG_SEND_ZLP` ends exact multiples.
  - Without batching, every packet was its own transfer and cost the target
    a TRB completion. Host → target at MTU 1500 was then 93 Mb/s.
- **MTU:** usbnet refuses an MTU that is a multiple of the packet size, so
  that a full-size frame needs no ZLP. That rule does nothing here, since
  every frame carries a header and the ZLP is sent anyway. `dbcnet` uses its
  own `ndo_change_mtu`, and `vm-dbc` checks 8192.

## Results

These come from one run, on 2026-11-21, of `vm-dbc` and `vm-impostor`
together. The two VMs shared the host's CPUs, and each VM holds both ends
of its link. iperf3 ran for 5 s, and the figures are the receiver's, in
Mb/s. To show the spread, the previous identical run is in brackets where
it differed by more than a third:

| Target | Debug host | MTU | host → target | target → host |
|---|---|---|---|---|
| dbc_net | dbcnet.ko | 9000 | 1520 (1009) | 1455 |
| dbc_net | dbcnet.ko | 1500 | 1285 | 658 |
| dbc_net | dbcnet-host | 9000 | 1022 (1442) | 642 |
| dbc_net | dbcnet-host | 1500 | 1085 | 179 |
| dbcnet-tty (ttyDBC) | dbcnet-host | 9000 | 208 | 231 |
| dbcnet-tty (ttyDBC) | dbcnet-host | 1500 | 112 | 171 |
| PPP on ttyDBC | PPP on ttyUSB | 1500 | 67 | 59 |
| dbc-impostor (dummy_hcd) | dbcnet.ko | 9000 | 601 | 1063 |

What the table shows:
- **Dropping the tty** is the big step on the target: 3.8–11× the ttyDBC
  path, depending on direction and MTU.
- **`dbcnet-host` limits target → host:** it keeps one synchronous 32 KiB
  read outstanding. `dbcnet.ko` keeps several URBs queued and gets 2–4× more
  in that direction.
- **Host → target, the two debug hosts are within run-to-run noise.** Both
  batch up to about 32 KiB per transfer.
- **The numbers are indicative only.** A single run can swing by a third or
  more. Real hardware will differ too: the emulation moves data with memcpy
  and no link-rate limit, while real DbC is a 5 Gb/s link with real DMA.
  Each VM has two vCPUs.

## USB/IP over the link

DbC can't be a USB gadget: its descriptors and its single pair of bulk
endpoints are fixed. Over IP, though, USB/IP gives much of what gadget
mode would. A USB device plugged into one machine appears on the other,
with no new code:

```
# target (where the device is plugged in)
modprobe usbip-host && usbip bind -b <busid> && usbipd -D
# debug host
modprobe vhci-hcd && usbip attach -r 10.77.0.1 -b <busid>
```

Exporting the other way works the same, with `usbipd` on the debug host.

`vm-dbc` and `vm-dbc-notty` check this with an emulated USB stick on a
third xHCI in the guest, assigned to the target. They:
- export it from the target's namespace;
- attach it on the debug host over `dbc0`;
- read all 256 MiB through `vhci_hcd`, and compare 64 MiB with the
  expected contents;
- detach it.

The read ran at 122–140 MB/s (about 1 Gb/s), close to the link's TCP
throughput.

## Not done

- **Real hardware.** The next step is the probe in the plan
  (`RESEARCH.md` §6), on boards such as the UP 4000 or UP 7000.
- **Other kernels.** Both patches are tested only on 6.18.64, the nixpkgs
  default, though `nixosModules.target` builds for whatever kernel is
  configured.
  - The ring fix's context differs on 6.12 and earlier. Those kernels don't
    need the fix, but the patch will fail to apply.
  - `dbc_net` uses 6.12+ interfaces, such as `<linux/unaligned.h>`.
- **Upstreaming.** The ring fix is ready to send. `dbc_net` would need a
  sysfs selector rather than a module parameter, documentation, an
  assigned product ID, and agreement on the framing. `upstream/` has the
  series. The Kconfig split has been
  through real kernel configs (`checks.kconfig-split`), building
  `drivers/usb/host` in the full tree three ways:
  - existing configs (`USB_XHCI_DBGCAP=y` alone) still get ttyDBC and no
    `dbc_net`;
  - both functions together;
  - a `TTY=n` kernel, which before could not have DbC at all, gets
    `dbc_net` alone, with no tty references.

  It has been booted only as the out-of-tree module.
- **A race in the DbC core, analysed but not fixed.** It is not seen in
  any test.
  - **The window:** `xhci_dbc_stop()` (`disable`, suspend, removal) gives
    back the pending requests while the state is still `DS_CONFIGURED`.
    Only then does it call the function's `disconnect`, and later set
    `DS_DISABLED`.
  - **What can slip in:** in between, `dbc_ep_queue()` still accepts
    requests. For `dbc_net`, that's NAPI requeueing a receive buffer or
    `xmit` submitting one. ttyDBC's write path can do the same.
  - **The effect:** such a request is never given back, so its DMA mapping
    leaks: an IOVA under an IOMMU, or a bounce buffer under swiotlb.
    Nothing is corrupted: the endpoint lists are reinitialised at the next
    enable, and `dbc_net` rebuilds its pools from its own arrays.
  - **Why the obvious fixes fail:**
    - Flushing after `disconnect` would hand completions to ttyDBC after
      it has freed its port.
    - Setting `DS_ENABLED` before the flush, as the cable-unplug path
      does, lets the still-running event work walk back up to
      `DS_CONFIGURED` and call `configure` again.
  - **A fix** wants a stopping state, which the event work and
    `dbc_ep_queue()` both refuse, set under the lock before the flush.
    That's a core change, best proposed alongside the RFC.
- **Statistics.** These use `net_device.stats` without per-CPU counters.
