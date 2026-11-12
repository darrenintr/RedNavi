# Navi33 P7.28 — read-only GFX0 PCI-provider ownership audit (0.0.47)

## Trigger and evidence

On a physical RX 7600 boot with RedNaviNavi33P4 **0.0.46**:

- P7.27: `registers_verified=1`, `response_before=1`.
- `mailbox_lease_held=0`, `command_sent=0`,
  `handshake_verified=0`, `admission_error=2` (missing opt-in).
- Under the same GFX0 IOPCIDevice, IOService tree listed siblings
  `RedNaviProbe`, `AMDSupport` and `.Display_boot` with
  `IONDRVFramebuffer` attached to the boot display.

The presence of an `AMDSupport` **service** does not establish that
it opened GFX0 as an exclusive client, writes SMU C2PMSG registers,
or provides hardware acceleration. The existing RedNaviProbe never
calls `pci->open(this)`, so it does not hold a proven PCI-provider
open lease today.

## What the audit does

Pure C++17 `navi33_smu_provider_owner_audit.h` classifies the
host provider's momentary `isOpen()` and `isOpen(this)` snapshot
without turning a child-service listing into an ownership claim.

`kern_probe.cpp` publishes
`navi33-p7-mailbox-provider-audit` during the existing Step 27:

- `provider_open_by_any` / `provider_open_by_rednavi`;
- `provider_state`:
  0 not requested, 1 no observed open, 2 opened by another client,
  3 opened by RedNavi, 4 contradictory state;
- `child_snapshot_available`, `direct_children_observed`,
  `amd_support_sibling`, `boot_framebuffer_sibling`;
- independent, **still false** `all_rednavi_senders_serialized`,
  `foreign_mailbox_writer_excluded`,
  `mailbox_exclusive_proven`, `may_issue_smu_command`.

The class count and sibling inspection are a point-in-time IORegistry
snapshot, not a stable inventory or a security boundary.

No `pci->open` or `pci->close` is called. No PCI command register
changes, SMU mailbox write, PSP command, RLC autoload, MES or graphics
queue are introduced. Existing P7.27 `mailbox_lease_held=false`
is **unchanged**, even if someone sets unsafe build/boot arguments.

## Why the PCI provider state matters

IOService `open` is an arbitration mechanism; its details depend
on the provider class. The public IOPCIDevice implementation
`handleOpen` delegates to the superclass's single-owner semantics.
However this arbitration does **not** serialize firmware,
non-cooperating register writers, or other PCI functions. It is
not interchangeable with a proven MP1 mailbox lease.

Apple XNU headers define `IOService::isOpen()` as detecting any
client's open state, or a specific client's open state. No user-space
`ioreg` property directly substitutes for these calls.

## Test (no new opt-in needed)

```sh
git fetch origin
git switch --track origin/feature/navi33-p728-provider-owner-audit
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
```

On an independently recoverable experimental EFI, ensure default-off
boot arguments. After boot:

```sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe |
  grep -E '"navi33-p7-(smu-roundtrip|mailbox-provider-audit)"'
```

Expected version: 0.0.47. Even if `provider_state=1` (no opened
client) this is NOT proof of mailbox exclusivity. Do not turn on
`rn727smu=1` and do not change P7.27's hardcoded lease gate.

## Next engineering step

Audit and serialize **every RedNavi host SMU sender**, including the
legacy `rn_smu13_send` path, with a sleeping mailbox mutex; separate
host arbitration from protocol-transcript validity. Design a
manual, one-shot real-hardware experiment only when it cannot
interfere with the boot-display pipeline and a recovery path exists.
Do not unblock RLC while the Linux firmware submission order mismatches.
