# Adding the xHCI Debug Capability to QEMU: scoping

Read against QEMU 11.1.2 `hw/usb/hcd-xhci.c` and Linux 7.3.4
`drivers/usb/host/xhci-dbgcap.{c,h}` on 2026-11-20. Nothing here is built
yet.

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
