# Navi33 P7.27: one-shot SMU 13.0.7 GetSmuVersion transaction

Status: **implementation and host simulation; hardware submission blocked**.
Version 0.0.46. This stage does not start RLC, MES, graphics queues, or Metal.

## Hardware baseline

RX 7600, MP1 13.0.7, NBIO 4.3.1. PSP PMFW and firmware
loading were acknowledged through fence 23/23. P7.26 read
`MP1_FIRMWARE_FLAGS = 1` through PF0 RSMU; this proves only an
SMN snapshot, NOT a fresh SMU mailbox command/response.
P7.25 observed old C2PMSG_90 response `1` but
`smu_handshake_verified=0`. The Linux-equivalent RLC firmware
loading order remains unresolved.

## Implemented

- New `navi33_smu_mailbox_roundtrip.h`: checked MP1 BAR5 offset
  mapping; one-shot `GetSmuVersion=0x02` command, with response
  clear-readback, argument clear-readback, explicit command submission,
  bounded response polling, output argument reading, and transcript
  validation using the existing P7.25 `navi33_smu_mailbox_admission.h`.
- Stops on an unexpected pre-submit status, failed readback,
  firmware rejection, all-ones/zero version, or timeout.
  No automatic retry; the attempt is consumed even on failure.
  The maximum 2000 polls and 25 us delay define a nominal 50 ms
  polling budget (not a hard wall-clock deadline).
- Host tests simulate fresh acknowledgement, missing lease,
  stale pre-submit state, blocked response/argument clearing,
  busy, timeout, invalid results, and duplicate submission.
- P7.27 is wired to `kern_probe.cpp` as a **fail-closed** diagnostic
  `navi33-p7-smu-roundtrip` with register offsets, admission state,
  request/response transcript and error fields.
- Optional build switch `NAVI33_SMU_MAILBOX_UNSAFE=1` and boot
  opt-in `rn727smu=1` exist, but neither authorizes a transaction.

## Critical blocker: real mailbox ownership

The live device code sets `p727MailboxLeaseHeld=false` deliberately,
even if both unsafe build and boot opt-in are enabled. The existing
driver does not prove exclusive ownership of the MP1 mailbox across
other kernel services, PCI functions or platform/SMU firmware users.
Having a BAR5 mapping, retaining a PSP ring, acquiring a local spinlock,
or calling `IOPCIDevice::open` is not by itself sufficient.

**No new SMU C2PMSG MMIO writes can occur from P7.27 in 0.0.46.**
The transport is exercised via fake MMIO in host tests only.
Never change this constant to true based on firmware flags or an
old `PPSMC_Result_OK=1` snapshot; implement and audit device-wide
mailbox arbitration first.

P7.27 transcript is NOT fed to the P7.25 RLC preflight. Both
`smu_handshake_verified=0` and
`firmware_order_matches_linux=0` remain unchanged on hardware.

## Register layout

All offsets are DWORD-relative to the discovered MP1 base:
- C2PMSG_66 message: `0x282`
- C2PMSG_82 input/output parameter: `0x292`
- C2PMSG_90 response: `0x29a`

For observed MP1 base `0x16000` DWORD, BAR5 bytes are
363016, 363080 and 363112 respectively, subject to BAR bounds.

## Verification on host (no GPU I/O)

```sh
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
```

Do not install this branch merely to make the new unsafe switch work.
On an independently recoverable experimental EFI, first boot with
**no new flags** and inspect:

```sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe | grep -E '"navi33-p7-(smu-roundtrip|rlc-preflight)'
```

Expected on Step 27: P7.27 `attempted=0`, `may_submit=0`,
`handshake_verified=0` and `mailbox_lease_held=0`. A correct
read-only diagnostic is not a verified SMU command response.

## Remaining milestone

1. Audit and implement an exclusive MP1 mailbox ownership mechanism
   that covers all real claimants; implement lease invalidation and
   a recovery plan after timeout.
2. Prove full kernel compile and boot on a rescue-capable test EFI.
3. Conduct exactly one manually opted-in, bounded hardware
   GetSmuVersion transaction and capture zero-before-command,
   response transition and version output.
4. Correct the independent PSP firmware ordering mismatch before
   considering RLC autoload.

Reference: Linux `smu_v13_0.c`, `smu_cmn.c`,
`smu_v13_0_7_ppsmc.h`; internal
`docs/NAVI33_SMU_MAILBOX_ADMISSION.md`.
