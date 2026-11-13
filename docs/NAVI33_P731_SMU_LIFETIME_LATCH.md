# Navi33 P7.31 — cross-service one-shot latch (0.0.50)

## Physical baseline

P7.30 Kext **0.0.49** loaded successfully on an RX 7600 on
2026-11-13. Recorded evidence:

```text
navi33-p7-smu-transaction-guard:
  session_scope_service_instance=1
  session_consumed=0
  session_quarantined=0
  legacy_alias_shared_mutex=1
  deadline_ns_lo=50000000
  live_smu_command_enabled=0

navi33-p7-smu-roundtrip:
  registers_verified=1
  response_before=1
  command_sent=0
  handshake_verified=0
  mailbox_lease_held=0
```

The stale C2PMSG response `1` is **not** a new SMU acknowledgement.

## What was missing

P7.30's `SessionState` belongs to one `RedNaviProbe` instance. A
provider stop/reprobe creates a new instance with a fresh state. If
hardware command admission were opened later, repeated probes could
defeat a service-local one-shot limit.

## P7.31 implementation

`navi33_smu_lifetime_latch.h` defines an atomic 32-bit attempt latch
using acquire/release compare-and-exchange.

- A **single static latch in `kern_probe.cpp`** is shared among all
  Navi33 `RedNaviProbe` objects linked into this *same loaded Kext*.
- `smu_mailbox_roundtrip::execute()` now **requires** a reference to
  that latch. Once other admission gates and address checks pass, it
  atomically claims the latch **before any BAR5 MMIO observation**.
- If another service already claimed it,
  `CrossServiceAttemptAlreadyClaimed` is returned with **zero MMIO**.
  The new service's own local state remains unconsumed.
- Once claimed, the latch is **never reset**, even if a command fails,
  the owning `RedNaviProbe` stops, or the provider is rematched.
- Existing per-instance `SessionState` continues to quarantine
  inconsistent/missing firmware replies. The 50ms monotonic deadline,
  2,000-poll cap, per-device host mutex and alias mutex continue to
  apply.
- This one-slot policy is intentionally *more restrictive* than a
  per-PCI-device keyed table: multiple Navi33 cards managed by the
  same Kext share one slot.

This guard is **not** persisted across Kext unload/reload, operating
system reboot, a separate Kext build, or other drivers; it cannot be
interpreted as physical-device lifetime ownership. An atomic attempt
latch also does not coordinate the host mutex of *different*
RedNaviProbe instances, a separate issue before live enablement.

**P7.27 `p727MailboxLeaseHeld=false` remains hardcoded.**
Even unsafe build switches and `rn727smu=1` cannot enable a live
`GetSmuVersion` through this P7.31 path.

## P7.31 IORegistry snapshot

New key `navi33-p7-smu-lifetime-latch`:

- `one_slot_all_navi33_in_kext=1`
- `survives_service_reprobe=1` (within same loaded Kext image)
- `survives_kext_reload=0`
- `attempt_slot_claimed=0` on default-off initial boot
- `this_service_claim_acquired=0` on default-off initial boot
- `service_instance_consumed=0`
- `unreviewed_external_writers_remain=1`
- `mailbox_exclusive_proven=0`
- `live_smu_command_enabled=0`

This is a start-time diagnostic snapshot, not a continuously updated
property. `AMDSupport` and `.Display_boot` may attach later.

## Validation

Host tests cover a 32-thread atomic attempt race, two independent
service-local `SessionState` objects sharing one global latch, and
reprobe after timeout/quarantine. Static writer audit requires the
new guard and unchanged hardcoded hardware block.

```sh
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
```

Do not replace a working 0.0.49 experimental USB before all CI and
local checks pass. Maintain a separate rescue EFI.

When 0.0.50 is built and validated, **without** adding new unsafe
boot arguments:

```sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe \
  | grep -E '"navi33-p7-(smu-lifetime-latch|smu-transaction-guard|smu-roundtrip)"'
```

Expected `attempt_slot_claimed=0` and `command_sent=0`.

## Remaining blockers before first live GetSmuVersion

1. An external mailbox-writer/ownership strategy for physical RX
   7600 with Apple services, boot framebuffer, and firmware actors.
   `IOPCIDevice::isOpen()=false` alone does not prove exclusivity.
2. Audited synchronization covering *all* host clients and concurrent
   probe instances that can access one physical MP1 mailbox.
3. A well-defined recovery and re-arming policy across complete Kext
   reload/reboot (not provided by this experiment).
4. Independent review of the existing Navi32-only synthetic timeout
   recovery write. It is not a valid Navi33 PMFW response.
5. Firmware ordering remains unresolved; keep RLC autoload blocked.

P7.31 is deliberately not a GPU acceleration or SMU command-enabling
milestone. It narrows a correctness gap while remaining fail-closed.
