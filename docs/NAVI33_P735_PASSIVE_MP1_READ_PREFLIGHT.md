# Navi33 P7.35 — passive MP1 monitoring preflight (host only)

**This phase has no runtime GPU reads or writes.** It adds source-based
checks and a C++ address-plan model, not a live Kext sampler. Working
USB version **0.0.51** stays unchanged.

## Grounded P7.34 evidence

The two user-supplied P7.34 JSON reports confirmed:

- Host source inventory: **4** named direct MP1 writes and **3**
  indirect P7.27 dynamic write sites, exactly matching the reviewed
  baseline; the latter are gated by the still-hardcoded
  `p727MailboxLeaseHeld=false`.
- P7.33 sample 1: GFX0 direct children=1, no Apple siblings.
- P7.33 sample 2: children=3, `AMDSupport` and `.Display_boot`
  attached, but `provider_open_by_any=0`, named-sibling
  `isOpen(child)=0`, no contradictory snapshot.
- `smu_command_sent_telemetry=0`, `handshake_verified=0`.
- The input tools explicitly **do not** exclude raw/third-party BAR5
  writers or firmware actors, and cannot prove hardware ownership.

## The important register-wrapper trap

The C++ utility name or driver's intent to "read" is not a proof that
no GPU MMIO writes occur:

| Source path | What it actually does | Suitable as a *zero-write* sampler? |
| --- | --- | --- |
| `RedNaviProbe::rd(byteOffset)` | One CPU load from mapped BAR5 | Candidate, **only** for separately reviewed register semantics and valid mapping lifetime |
| `rn_rreg32(adev,regDword)` (within mapped BAR) | Direct `readl` | Conditional candidate |
| `rn_rreg32(adev,regDword)` (outside mapped BAR) | `writel(RN_RSMU_INDEX)`, readback, `readl(RN_RSMU_DATA)` | **No**: index *write* |
| `RedNaviProbe::vramRead(pos)` | Writes `MM_INDEX` and `MM_INDEX_HI`, then reads `MM_DATA` | **No** |
| `rn_vram_rd32()` | Writes `RN_MM_INDEX` and `RN_MM_INDEX_HI`, then reads `RN_MM_DATA` | **No** |

Sources:
- [RedNavi `kern_probe.cpp`](https://github.com/darrenintr/RedNavi/blob/feature/navi33-p734-xnu-access-evidence-model/RedNavi/kern_probe.cpp) (helpers `rd`, `wr`, `vramRead`);
- [RedNavi `compat/src/rn_reg.cpp`](https://github.com/darrenintr/RedNavi/blob/feature/navi33-p734-xnu-access-evidence-model/compat/src/rn_reg.cpp) (`rn_rreg32`, `rn_vram_rd32`).

**Even direct reads can have hardware side effects** if the register is
read-to-clear or otherwise reacts to reads; P7.35 does not establish
register-specific read safety. A source-level "no writes" test covers
only the chosen host wrapper, not bus/firmware side effects.

## What P7.35 actually verifies

`navi33_p735_passive_mp1_read_plan.h` consumes:

- An exact Navi33 identity check;
- Verified MP1 **13.0.7** revision;
- Already-mapped direct BAR5 length;
- MP1 base in dwords.

It reuses the existing validated P7.27 register offset math
(`smu_mailbox_roundtrip::plan`) and rejects missing/out-of-range
bases and BAR lengths.

The P7.33 RX 7600 physical values were:

```text
mp1_base_dword        = 90112
bar5_mapped_bytes     = 1048576
C2PMSG_66 byte offset = 363016
C2PMSG_82 byte offset = 363080
C2PMSG_90 byte offset = 363112
```

All three lie within the validated 1 MiB BAR5 window. This proves
only a feasible **direct address plan**, not that performing a late
read is harmless. The plan always returns:

```text
liveReadPermitted                  = false
registerReadSideEffectsExcluded   = false
foreignWritersExcluded            = false
firmwareWritersExcluded           = false
mailboxExclusiveProven            = false
mayIssueSmuCommand                = false
```

No P7.35 header is included in `kern_probe.cpp` and no timer,
boot argument, user client, kernel IORegistry property or GPU MMIO
operation was added. Experimental Kext version remains **0.0.51**.

## Validation

```sh
cd ~/RedNavi
git fetch origin
git switch feature/navi33-p735-passive-mp1-read-plan 2>/dev/null || git switch --track origin/feature/navi33-p735-passive-mp1-read-plan
git pull --ff-only
make test
```

New tests check physical offsets, incomplete/overflowed BAR mappings,
identity gates, and hardcoded no-live-read policy. Source tests
deliberately detect MMIO-index writes inside read-named helper
functions and reject accidental integration of the host-only model
into the Kext.

## Before implementing a real passive runtime monitor

1. Confirm from Linux amdgpu code and available AMD register semantics
   that `C2PMSG_66/82/90` are safe to sample after firmware setup;
   the P7.34 source model does **not** prove this.
2. Verify the BAR5 mapping lifetime and read synchronization across
   `start()`, timer callback, `stop()` and device sleep/wake.
3. Limit frequency and count; do not attempt new RSMU-index or
   MM_INDEX reads in the passive callback.
4. Treat all snapshots as **time-local**: unchanged values don't
   exclude off-sample writers. A changed value doesn't name its author.
5. Keep P7.27 `p727MailboxLeaseHeld=false`, RLC and MES gating
   unchanged. Never make mailbox ownership depend on a passive probe.

P7.35 is a practical hardware-monitor design review, **not** another
"fence completed" milestone.
