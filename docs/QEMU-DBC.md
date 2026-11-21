# Adding the xHCI Debug Capability to QEMU

Read against QEMU 11.1.2 `hw/usb/hcd-xhci.c` and Linux 7.3.4
`drivers/usb/host/xhci-dbgcap.{c,h}` on 2026-11-20. The scoping below was
written first; the status section records what was then built.

## Status

Built as design A below, in [`qemu/xhci-dbc.patch`](../qemu/xhci-dbc.patch)
against QEMU 11.1.2 (`nix/qemu-dbc.nix`). It adds about 1,000 lines, close
to the estimate below:
- a new `hw/usb/hcd-xhci-dbc.c` (~900 lines), holding the register block, event ring,
  transfer engine and the `usb-dbc-peer` device;
- hooks in `hcd-xhci.c` for the capability chain, realize/unrealize and
  device reset, plus a `dbc` property, default off;
- trace points `usb_xhci_dbc_*`.

```
-device qemu-xhci,id=dbct,dbc=on          # the target's controller
-device qemu-xhci,id=dbch                  # the debug host's controller
-device usb-dbc-peer,id=cable,dbc=dbct,bus=dbch.0
```

The peer stays detached while the guest has DbC disabled. On enable it
enumerates with the guest's DCDDI identity and its info-context strings.
`device_del cable` unplugs it; the guest sees CCS drop.

`nix/vm-dbc.nix` runs the guest's own DbC driver against the emulation,
with milestones 1 and 2 of the plan:
- ttyDBC0 ↔ ttyUSB0 carries 1 MiB each way intact.
- The `vm-impostor` checks run unchanged against ttyDBC: frames of
  64–9004 bytes both ways and iperf3 both ways. A target that stops
  reading for 4 s is checked too.
  - `dbcnet-tty` (`src/dbcnet-tty.c`) bridges ttyDBC to TUN on the target.
- The ZLP and padding cases, and reconnects after DbC disable/enable and
  after a cable unplug.

iperf3 through ttyDBC and `dbcnet-tty` gave 165–229 Mb/s, against about
600–715 Mb/s for the impostor. The likely cause is ttyDBC: 16 requests
of 1 KiB, a tty layer and a polled event loop, where the impostor moves
32 KiB per read. That is not profiled yet. It is the case for milestone
3's in-kernel `dbc_net`. Both ends share one VM, so the numbers are only
indicative.

### What building it found

Three bugs (one each in the emulation's first cut, Linux and dbcnet) and
one property the scoping predicted.

1. **The emulation dropped events when the event ring looked full.**
   - Linux's DbC event loop consumes events by cycle bit, and writes ERDP
     only after the loop ends.
   - Under steady traffic each completion leads to a requeue, a doorbell
     and another completion, so the loop can run for hundreds of events on
     a stale ERDP.
   - The first cut checked for "full" against that ERDP and dropped the
     event, and the guest then waited forever for those TRBs.
   - It showed only under host load: iperf stalled about 2 s in.
   - Now a TRB is taken only when the ring has room for its completion,
     the other ring's TRB in progress and a port status change, and an ERDP
     write resumes both rings. Real hardware also holds off rather than
     lose an event.

2. **A Linux regression: the DbC transfer rings fill after one lap.**
   - 6.12 and earlier kept their own `num_trbs_free` counter in
     `xhci-dbgcap.c`, which the transfer-event handler incremented.
   - Later kernels check free space with the shared
     `xhci_num_trbs_free()`, which compares enqueue with `ring->dequeue`.
     Nothing in the DbC driver advances `dequeue`.
   - So after about 254 TRBs per direction (~254 KiB through ttyDBC),
     every queue fails with `failed to queue trbs` and ttyDBC goes quiet.
   - Seen on 6.18.64; 7.3.4 has the same code, so real hardware should hit
     it too.
   - [`nix/linux-dbc-ring-dequeue.patch`](../nix/linux-dbc-ring-dequeue.patch)
     advances `dequeue` past each completed TRB, and the test kernel
     carries it. It is a candidate for upstream.
3. **A dbcnet bug: abandoning a bulk OUT desynchronises the stream.**
   - `dbcnet-host` used to give up on a bulk OUT after 2 s. By then the
     target had already taken part of it, as real hardware does, packet by
     packet.
   - The target's decoder, now mid-frame, read the next batch as the rest
     of that frame. With sparse traffic that looked like a dead link.
   - Bug 1 stalls tripped that timeout. Pausing the target process (SIGSTOP
     on `dbcnet-tty`, so ttyDBC throttles) reproduces it every time.
   - Fix: bulk OUT has no timeout, so a stalled target NAKs until it
     resumes, and an unplug still ends the transfer.
   - `vm-dbc` now pauses the target for 4 s mid-iperf and then requires
     exact-multiple frames both ways. The impostor can't take that check:
     stopping it interrupts raw_gadget's ioctls, which drop partly filled
     requests.
4. **DbC never sends a zero-length packet**, as predicted below.
   `dbcnet-tty` appends a zero-length frame whenever a batch ends on a
   1024-byte boundary. `--no-pad` shows the reply arriving only at the
   host's read timeout.

Not done:
- milestone 3, the in-kernel `dbc_net`;
- migration (a blocker is registered);
- endpoint halt (DbC never stalls, and `CLEAR_FEATURE(ENDPOINT_HALT)` is
  acked).

## Why

`nix/vm-impostor.nix` already tests the debug-host side and the framing.
But `dbc-impostor` is a stand-in: the target side of a real link is Linux's
own DbC driver plus, later, our `dbc_net` function. A guest can only run
that code if its xHCI advertises extended capability 10.

QEMU's xHCI does not. `xhci_cap_read()` serves only the two Supported
Protocol capabilities at 0x20–0x3f, and every other offset reads 0.

## What the Linux driver actually uses

The guest driver is the spec's only consumer we care about, and it uses a
small subset of the spec. The driver is polled (a delayed work item every
`dbc_poll_interval_ms`), and DbC has no interrupts, so neither needs
emulating.

| Piece | Linux usage | Emulation |
|-------|-------------|-----------|
| `DCID` | reads ERST max | constant |
| `DCDB` doorbell | target 0 = OUT, 1 = IN | kick the transfer engine |
| `DCERSTSZ`/`DCERSTBA`/`DCERDP` | one single-segment event ring | own event writer; `xhci_write_event()` is tied to interrupters, ~60 lines |
| `DCCTRL` | DCE (enable), DCR (run), port enable, HOT/HIT (halts), DRC, max burst | small state machine |
| `DCPORTSC` | CCS, PED, CSC/PRC/PLC/CEC change bits, write-1-to-clear | mirrors peer attach/reset |
| `DCCP` | info context (4 string-descriptor pointers + lengths), OUT and IN endpoint contexts (TR dequeue + DCS) | DMA reads at enable / doorbell |
| `DCDDI1/2` | VID, protocol, PID, bcdDevice | device descriptor fields |
| TRBs consumed | Normal (IOC), Link (toggle cycle), No-op (cancelled requests) | **reuse `xhci_ring_fetch()`** as-is |
| Events produced | Transfer Event (SUCCESS / SHORT_PACKET / STALL, residual, EP ID, TRB ptr), Port Status Change | two event constructors |

Linux's state machine is DISABLED → ENABLED → CONNECTED (CCS=1) →
CONFIGURED (DCR=1).
- If DCR doesn't rise within 2 s of connecting, Linux toggles DCE and
  retries.
- A cable unplug (CCS=0, PED=0) or a port reset (PRC) drops it back to
  ENABLED and flushes the rings.

## The other end of the cable

The DbC needs a USB host to enumerate it. There are two designs, and the
first is enough for one VM:

**A. In-process loopback device (recommended first).**
- Add a new `usb-dbc-peer` USBDevice that is linked to the xHCI that owns
  the DbC, and plug it into a USB bus: a second `qemu-xhci` stands in for
  the debug host.
- The guest setting DCE attaches it, and clearing DCE detaches it.
- It answers enumeration as DbC hardware does: device descriptor from
  DCDDI, strings DMA-read from the info context, a fixed config with one
  interface and EP1 bulk OUT/IN at 1024 with SS companions, and a BOS.
  QEMU's `desc.c` already supports SuperSpeed descriptors.
- SET_CONFIGURATION sets DCR and queues a Port Status Change event.
- Bulk `USBPacket`s are handled asynchronously (`USB_RET_ASYNC`) and
  matched against Normal TRBs on the DbC rings. A packet with no TRB
  posted NAKs until the doorbell rings.
- Both ends then run real kernel code in one guest:
  - target: `xhci-dbgcap` + ttyDBC (later `dbc_net`)
  - debug host: `usb_debug` or `dbcnet-host`
- This is the emulated version of the loopback-cable setup people use on
  real hardware.

**B. usbredir backend, for two VMs.**
- The DbC side speaks the usbredir protocol in the "host" (device-owner)
  role over a chardev.
- The second VM uses stock `-device usb-redir,chardev=…`.
- It reuses everything from A except the transport. Do it only if
  single-VM loopback turns out to hide something.

## Semantics to get right

- **Packetisation.** QEMU hands a device whole transfers, not 1024-byte
  packets. Emulate packet boundaries:
  - An OUT TRB completes when it is full, or when a short packet arrives
    (completion code SHORT_PACKET plus the residual).
  - An IN `USBPacket` completes when its buffer is full or a TRB's data ends
    on a short packet.
- **ZLPs.** Linux's DbC driver never queues a zero-length TRB. An IN write
  of exactly k×1024 bytes therefore never terminates a larger host read.
  - ttyDBC gets away with it because `usb_debug` reads one packet at a time.
  - **`dbcnet-host` reads 32 KiB, so against real ttyDBC such a write only
    surfaces at its 200 ms read timeout.** The `vm-impostor` subtest "target
    to host without a ZLP" measures this: a 1024-byte frame arrived after
    183 ms instead of under 1 ms. The cancelled read keeps its partial data,
    so the frame is late, not lost.
  - Fix it in the protocol or in `dbc_net`: queue a zero-length Normal TRB,
    assuming DbC emits a ZLP for one as host-mode xHCI does (unverified),
    or pad frames.
  - The emulation should mirror whatever real silicon does here, so the VM
    test catches it.
- **Halts.** HOT/HIT and the peer's CLEAR_FEATURE(ENDPOINT_HALT) can come
  later; Linux handles them, but nothing in the data path needs them.
- **Placement.** The xECP chain's 8-bit next pointer reaches about 1 KiB
  past 0x30. The DbC block (0x40 bytes) can sit in the reserved operational
  space below the port registers (0x80–0x43f), or behind a larger
  `LEN_CAP`, gated by a `dbc=on` property, default off, so existing
  machines and migration are untouched.
- **Migration.** Start as unmigratable (a migration blocker). Add vmstate
  only if upstreaming.

## Size and cost

| Part | Lines (rough) |
|------|---------------|
| Register block, ext-cap chaining, `dbc` property | ~200 |
| Event ring writer + context parsing | ~150 |
| Transfer engine (doorbell → TRBs ↔ pending packets, packetisation, events) | ~300 |
| `usb-dbc-peer` device (descriptors, control, async bulk, attach/detach) | ~300 |
| **Total** | **~1,000 lines of C**, mostly a new `hcd-xhci-dbc.c` plus a few hooks/exports in `hcd-xhci.c` |

Build cost on top of the existing test:
- A patched QEMU for the VM test via `virtualisation.qemu.package`.
- A guest kernel with `CONFIG_USB_XHCI_DBGCAP=y`, which NixOS kernels lack.
  That is one kernel compile, cached afterwards.

**Assessment: moderate, and well bounded.** The guest driver's demands are
small and fully readable, and QEMU's ring fetch already does the hard part.
The risk is in packetisation and ZLP semantics, which are exactly the
things the test exists to pin down. Upstream QEMU has no DbC emulation, so
this is also a plausible upstream contribution once it works.

## Plan

1. Build `hcd-xhci-dbc.c` against the patched QEMU from a flake input or a
   patch, with a NixOS test variant: kernel with DBGCAP, two `qemu-xhci`,
   and `usb-dbc-peer`.
2. Milestone 1: `echo enable > …/dbc` → guest log shows "DbC configured" →
   `/dev/ttyDBC0` ↔ `/dev/ttyUSB0` echo.
3. Milestone 2: `dbcnet-host` against the real ttyDBC through a userspace
   tty↔TUN bridge, with the same pings and iperf as `vm-impostor`.
4. Milestone 3: the in-kernel `dbc_net` replaces the tty bridge.
