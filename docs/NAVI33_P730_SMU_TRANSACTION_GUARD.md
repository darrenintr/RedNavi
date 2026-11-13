# Navi33 P7.30 — fail-closed SMU transaction safety (0.0.49)

Status: **host simulator / kernel build validation, live MP1 GetSmuVersion STILL BLOCKED**.

## Baseline from real RX 7600, 2026-11-13

The physical P7.29 (0.0.48) USB boot was successful.

- `host_mutex_initialized=1`, `host_mutex_locked_at_snapshot=0`
- `legacy_sender_mutex_guarded=1`,
  `p727_sender_mutex_guarded=1`
- `registers_verified=1`, `response_before=1`
- `external_mailbox_writers_excluded=0`,
  `mailbox_exclusive_proven=0`
- `command_sent=0`, `handshake_verified=0`
- P7.28 IORegistry child listing was sampled during
  `RedNaviProbe::start()` before other GFX0 children registered.

The old response `1` is **NOT** evidence of a new command.

## Found and fixed: host mutex aliases

The legacy Navi32 `RedNaviProbe::smuProbe()` creates a *temporary*
`rn_device` to access the same GPU BAR5 as the persistent
`RedNaviProbe::gpu`. Before P7.30, its `rn_smu13_send()` locked
the temporary device's mutex, while P7.27 locked
`gpu.smu_msg_lock`. Two host software aliases could therefore
serialize independently while writing identical C2PMSG registers.

Now the temporary `rn_device` explicitly sets
`smu_msg_lock_override=&gpu.smu_msg_lock`, and
`rn_smu13_send()` resolves the shared mutex through
`rn_smu_message_mutex(adev)`.

The old *Navi32-only* timeout recovery write to C2PMSG_90 is also
guarded by the **same resolved mutex**. It writes a synthetic
response value of 1 and **must never** be treated as firmware
acknowledgement or used for Navi33. This legacy path needs a
separate follow-up review before enabling any other command.

The host mutex does not grant ownership against Apple `AMDSupport`,
firmware, non-cooperating drivers or other PCI functions.

## P7.30 one-shot and deadline behavior

The Roundtrip handler now accepts a `SessionState` retained inside
the RedNaviProbe instance (not a stack-local bool):

- `consumed`: set *before* the first observed MMIO operation;
  no second attempt within that service instance
- `quarantined`: set on unexpected response, failed zero-clear,
  wrong output, busy/error response, monotonic clock regression,
  expiration or poll timeout
- `verified`: set only after the complete fresh version response

An independently checked monotonic deadline spans the complete
transaction: `kDeadlineNanoseconds=50,000,000` (nominal 50 ms).
The existing 2,000-poll cap remains a second backstop when the
clock does not advance. This is a *bounded polling algorithm*, not
a real-time scheduling guarantee: a kernel stall can exceed the
budget until the next clock check.

The new signature injects `nowNs()`, permitting synthetic
clock tests without any hardware I/O. The kernel uses
`rn_now_ns()` (XNU monotonic uptime conversion).

**Limit:** the session is bound to the `RedNaviProbe` service
instance, not retained across kext unload/reload or a complete
service reprobe. A hardware-life-cycle guarantee requires a
separate, audited GPU identity keyed latch. The unsafe live path
must remain disabled until that is resolved.

## Source-level writer audit

The new host Python test scans RedNavi and the compatibility C++
translation units for direct
`WREG32_SOC15(RN_MP1, ..., mmMP1_SMN_C2PMSG_{66,82,90})`.

Reviewed explicit sites:

1. `RedNavi/smu13.cpp`: three writes in `rn_smu13_send()`,
   now serialized using the shared mutex
2. `RedNavi/kern_probe.cpp`: one Navi32-only synthetic
   response recovery write, now locked using the same alias mutex
3. Navi33 P7.27: MMIO callbacks wrapping `rd()/wr()`, fully
   serialized by `gpu.smu_msg_lock`, gated off in hardware

The test can detect unexpected **named direct writes**.
It **cannot** prove the absence of numeric-offset, computed-offset,
inline assembly, third-party, or firmware mailbox writers.
Consequently `external_mailbox_writers_excluded=0` and
`mailbox_exclusive_proven=0` remain valid.

## Telemetry

New property: `navi33-p7-smu-transaction-guard`.

- `session_scope_service_instance=1`
- `session_consumed=0`, `session_quarantined=0`,
  `session_verified=0` on a default-off USB boot
- `deadline_ns_lo=50000000`, `deadline_ns_hi=0`
- `elapsed_ns_lo=0`, `elapsed_ns_hi=0`, `monotonic_checks=0`
  when the SMU request has not been admitted
- `legacy_alias_shared_mutex=1`
- `unreviewed_external_writers_remain=1`
- `live_smu_command_enabled=0`

Existing P7.27 telemetry remains unchanged in meaning.

## Verification and deployment

```sh
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
```

Keep the last known-good **0.0.48** USB Kext and a separately
bootable rescue EFI while CI or tests are running.

After all checks pass and a recoverable experimental USB has been
updated to 0.0.49, boot with the existing arguments **without**
`rn727smu=1` and without enabling RLC. Check:

```sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe \
  | grep -E '"navi33-p7-(smu-transaction-guard|smu-host-serializer|smu-roundtrip)"'
```

Expected: `live_smu_command_enabled=0`, `command_sent=0`,
`handshake_verified=0`, `session_consumed=0`.

**Do not** turn the P7.27 `p727MailboxLeaseHeld=false` constant
into `true` based on these results. The first real GetSmuVersion
response remains a future, separately reviewed milestone; firmware
submission ordering and RLC autoload are independently unresolved.
