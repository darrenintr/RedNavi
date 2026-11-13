# Navi33 P7.34 — XNU ownership and MP1 access evidence model

This milestone adds **host-only policy/testing and offline review tools**.
It intentionally makes **no change to the runtime RX 7600 Kext**,
the OpenCore EFI, boot arguments or the P7.27 command admission gate.
The current verified USB Kext remains **0.0.51**.

## Physical baseline (2026-11-13, RX 7600)

P7.33 on real macOS returned:

| P7.33 observation | sample 1 (~5 s) | sample 2 (~13 s) |
| --- | ---: | ---: |
| `direct_children_observed` | 1 | 3 |
| `amd_support_present` | 0 | 1 |
| `boot_framebuffer_present` | 0 | 1 |
| `amd_support_provider_open` | 0 | 0 |
| `boot_framebuffer_provider_open` | 0 | 0 |
| `provider_open_by_any` | 0 | 0 |
| `mailbox_exclusive_proven` | 0 | 0 |

P7.32 additionally confirmed `RedNaviProbe`, `AMDSupport` and
`.Display_boot` were direct GFX0 IOService children after macOS
had finished device matching.

**These observations are time-local and say nothing conclusive
about raw BAR5 MMIO or asynchronous PSP/PMFW activity.**

## Three upstream source models reviewed

### NullMoth — native XNU GPU integration

- [`NVRM.cpp`, `startGPU`](https://github.com/nullmoth/nvidia-macos-driver/blob/main/kexts/NVRM/NVRM.cpp#L688-L839)
  uses `IOPCIDevice` config reads/writes and resource controls without
  requiring a preceding `fPCI->open()` call in that path. Therefore
  `IOService::isOpen()==false` is **not** equivalent to no PCI/MMIO
  activity.
- [`os-xnu.cpp`](https://github.com/nullmoth/nvidia-macos-driver/blob/main/kexts/NVRM/os-xnu.cpp#L237-L303)
  retains the PCI object in the device registry, then releases the
  registry on teardown. A provider pointer, retained device handle
  and an exclusive mailbox lease are **three distinct concepts**.
- [`os-xnu.cpp`, map table](https://github.com/nullmoth/nvidia-macos-driver/blob/main/kexts/NVRM/os-xnu.cpp#L330-L357)
  separates IOLock-protected writers from interrupt-path readers and
  uses atomic publication. Do not use a sleeping mutex inside
  primary interrupt context.
- [`NVRM.cpp`, `stop`](https://github.com/nullmoth/nvidia-macos-driver/blob/main/kexts/NVRM/NVRM.cpp#L1948-L1966)
  drains/stops relevant device work before teardown, protecting
  against callbacks after state release.

This is a *different GPU vendor* and its GSP protocol **must not** be
treated as AMD SMU/PSP documentation. No NullMoth code is copied.

### Cobalt64 — Navi33 native staging

- [`Navi33Probe.cpp`](https://github.com/darrenintr/Cobalt64/blob/main/src/navi33-bringup/src/Navi33Probe.cpp#L73-L160)
  explicitly distinguishes PCI inventory and passive preflight from
  active GPU register access.
- [`Navi33DarwinStage.cpp`](https://github.com/darrenintr/Cobalt64/blob/main/src/navi33-bringup/src/Navi33DarwinStage.cpp#L25-L55)
  separates temporary IOVA resources from GPU-owned active DMA. If the
  GPU may still access a mapping, the code quarantines it rather than
  issuing a premature `complete()` / free.
- [`Navi33DarwinDma.cpp`](https://github.com/darrenintr/Cobalt64/blob/main/src/navi33-bringup/src/Navi33DarwinDma.cpp)
  uses `IOMapper::copyMapperForDevice` and `IODMACommand` to
  validate host-side IOVA staging. An OS IOMMU map does **not** mean a
  hardware GPU queue has run.

### RedNavi upstream — display/accelerator architecture

- [`kern_probe.cpp`](https://github.com/Helemen7/RedNavi/blob/main/RedNavi/kern_probe.cpp)
  contains native `IOPCIDevice`, `IOService`, workloop, SMU and
  graphics queue paths.
- [`RedNaviAccel2`](https://github.com/Helemen7/RedNavi/tree/main/RedNaviAccel2),
  [`RedNaviFB`](https://github.com/Helemen7/RedNavi/tree/main/RedNaviFB)
  provide *structural* reference for an eventual display/accelerator
  chain. Neither implies that RX 7600 Metal is already functional.

## Evidence hierarchy

1. **Service attachment:** IORegistry tree says an `IOService`
   child exists. Does not show a PCI open, BAR map or mailbox write.
2. **Host provider-open query:** `pci->isOpen([client])` says whether
   IOKit sees an open state *at a particular observation time*.
   It does not control every possible MMIO access path.
3. **BAR/MMIO access:** a known mapped BAR and a read of a register
   establish access/observation, not exclusive ownership. A changed
   C2PMSG value cannot attribute its writer without separate trace.
4. **Host transaction attribution:** an instrumented write can name
   **one** host sender. It cannot exclude hidden host or firmware
   writers by itself.
5. **Firmware/device ownership:** independent protocol and
   arbitration proof plus a recovery policy, which we do **not** have.
   Only after independent review could a separate command gate be
   designed. P7.34 never attempts this.

## New files

- `RedNavi/navi33_xnu_access_evidence.h`:
  pure, provenance-typed model with `Truth` and `Origin`.
  It catches contradictory and incorrectly typed observations.
  Unknown observations are not silently converted to `false`.
  It **always** returns
  `externalHostWritersExcluded=false`,
  `firmwareWritersExcluded=false`,
  `mailboxExclusiveProven=false`,
  `mayIssueSmuCommand=false`.
- `tests/host/navi33_xnu_access_evidence_test.cpp`:
  P7.33 physical sample fixture, time-varying host-open, contradictory
  query, stale/changed MMIO, attributed write, missing and invalid
  provenance.
- `tools/navi33_xnu_access_review.py`:
  offline parser of P7.33 IORegistry text. Outputs JSON with
  explicit nulls for missing evidence.
- `tools/navi33_mp1_source_inventory.py`:
  static inventory of known named and indirect MP1 C2PMSG host writers.
  A different count or re-enabled hardware mailbox lease fails the
  known-sites baseline, **but passing cannot prove completeness**.
- Companion Python tests, all invoked by `make test`.

## Important correction to the earlier writer count

P7.30's source audit counted **four named direct**
`WREG32_SOC15(RN_MP1,0,C2PMSG_66/82/90,...)` sites:

- Three in `RedNavi/smu13.cpp` (`C2PMSG_90/82/66`);
- One legacy **Navi32-only** synthetic timeout-response write in
  `RedNavi/kern_probe.cpp` (`C2PMSG_90=1`).
  This is **not** an actual firmware acknowledgment.

But Navi33 P7.27 also contains **three indirect byte-offset writes**
in `navi33_smu_mailbox_roundtrip.h`:
`write(reg.responseByteOffset, 0)`,
`write(reg.parameterByteOffset, 0)` and
`write(reg.messageByteOffset, GetSmuVersion)`.
The callback ultimately invokes `wr(offset,value)` in
`kern_probe.cpp`. The **hardcoded**
`constexpr bool p727MailboxLeaseHeld = false;` keeps these
operations unreachable in the current hardware build.

This inventory is deliberately not called a *complete MP1 writer
audit*. Other dynamic offsets, direct physical mappings, Apple drivers
and GPU firmware remain unverified.

A separate subtlety: the legacy `smuProbe` uses
`wr((nbif+0)*4, 0x03b10024)` to select a firmware-flags SMN register
before reading the data port. This is an INDEX **write** even though
the intended SMN data operation is a read. It is not itself a
`C2PMSG_66/82/90` command. Keep such paths distinct when later
evaluating passive probes.

## Local verification on the Mac (no Kext/USB change)

```sh
cd ~/RedNavi
git fetch origin
git switch feature/navi33-p734-xnu-access-evidence-model 2>/dev/null || git switch --track origin/feature/navi33-p734-xnu-access-evidence-model
git pull --ff-only
make test
python3 tools/navi33_mp1_source_inventory.py --output "$HOME/Desktop/navi33-p734-writers.json"
ioreg -r -l -w0 -c RedNaviProbe > "$HOME/Desktop/navi33-p734-ioreg.txt"
python3 tools/navi33_xnu_access_review.py --input "$HOME/Desktop/navi33-p734-ioreg.txt" --output "$HOME/Desktop/navi33-p734-access.json"
```

All tools are host-only or offline. The existing **0.0.51**
Kext remains on the USB, and boot arguments must remain unchanged.

## Remaining blocker

Neither absence of host provider opens, absence of sampled MMIO
transitions, nor a known host sender can establish an exclusive AMD
MP1 mailbox lease. Do **not** enable the P7.27 SMU query or P7.25 RLC
autoload based on this report. Future passive MP1 reads require
explicit validation that the registers are safe to read and that
the sampling workloop cannot disturb initialization; even then,
read-only observations do not confer a lease.
