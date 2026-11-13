# Navi33 P7.29 — host-side SMU mailbox serialization (0.0.48)

**Status:** software transaction serialization; no live SMU command submitted.

## Why

RX 7600 (0.0.47) P7.28 produced:

```text
provider_state=1 provider_open_by_any=0 provider_open_by_rednavi=0
direct_children_observed=1 amd_support_sibling=0
boot_framebuffer_sibling=0 mailbox_exclusive_proven=0
p727: response_before=1 command_sent=0 handshake_verified=0
```

This snapshot occurs inside `RedNaviProbe::start()` **before**
`registerService()`. The sibling count is therefore provisional.
Earlier complete IOService enumeration showed `AMDSupport` and
`.Display_boot` on GFX0. This is not contradictory.

`provider_state=1` means **no opened provider observed at that
instant**. Neither no opened IOPCIDevice clients nor a host mutex
certifies that firmware/other software cannot use MP1 C2PMSG.

## Changes

- `rn_device.smu_msg_lock` is a dedicated, per-device,
  sleeping compatibility `mutex`, initialized in `rn_device_init()`.
- `rn_smu13_send()` now holds the mutex over its entire transaction,
  including the previous-response wait, register writes, bounded
  response poll, firmware error handling and output read. A scoped
  guard releases it on every early return.
- The legacy Navi32-only timeout-recovery response register write is
  also protected by the same mutex. **It remains legacy behavior**
  and must never be used as a P7.27 proof of a real response.
- Navi33 P7.27 `execute()` takes the **same** mutex for its entire
  call, so future admission cannot interleave with cooperating
  `rn_smu13_send()` users of the same `rn_device`.
- `navi33-p7-smu-host-serializer` reports that both sender call
  paths are guarded. It also explicitly reports that hardware
  commands remain disabled and outside writers are not excluded.
- Existing `navi33-p7-mailbox-provider-audit` adds
  `sampled_during_start=1` and
  `sibling_snapshot_provisional=1`.
- Host test `navi33_smu_host_mutex_test` calls the **actual**
  `rn_smu13_send()` through mocked MMIO from two concurrent
  threads. It checks no overlapping command submissions, full
  response completion, lock release on timeout and BUSY rejection.

## Limitations

This mutex is a **host-driver-local mutual exclusion mechanism**,
not an ownership token from PSP/PMFW. It does not arbitrate
`AMDSupport`, `IONDRVFramebuffer`, firmware or
external BAR5 writers. The legacy Navi32 timeout recovery remains
architecturally distinct from Navi33.

P7.27's `p727MailboxLeaseHeld=false` remains hardcoded. It does
**not** send `GetSmuVersion`, regardless of unsafe build or
`rn727smu=1`. P7.25 RLC autoload remains blocked.

## Validation

```sh
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
```

After successful CI, test 0.0.48 only on a recovery-capable EFI and
without new unsafe boot arguments:

```sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe \
  | grep -E '"navi33-p7-(smu-host-serializer|mailbox-provider-audit|smu-roundtrip)"'
```

Expected:
- local mutex exists and is observed unlocked at the snapshot;
- both sender guard fields = 1;
- outside-writer exclusion = 0;
- hardware command enabled = 0;
- P7.27 `command_sent=0`, `handshake_verified=0`;
- P7.28 early snapshot flags = 1.

The next milestone requires **separate** review of possible external
mailbox writers, firmware readiness, a reboot-capable recovery
procedure and a single manually authorized live GetSmuVersion.
Do not use P7.29 as a reason to bypass P7.27's hardcoded guard.
