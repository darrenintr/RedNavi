# Navi33 P7 runtime bring-up

This document covers the first native RX 7600 hardware-execution checkpoint
after the verified P6.6 / experimental bundle 0.0.10 state.

## Pinned PSP13.0.7 boot firmware

P7 must not trust a firmware image merely because the local generator computed
a SHA-256 for it. The accepted reference is the PSP 13.0.7 image present after
the upstream linux-firmware update:

- upstream-style commit: `1d4fdf2cbdc443ae1233e3516421e1bd7e84d304`
- commit date: 2026-02-25
- commit subject: `amdgpu: update PSP 13.0.7 firmware`
- file: `amdgpu/psp_13_0_7_sos.bin`
- bytes: `248496`
- Git blob: `94ae312e171957873fff74761c11af3e9849e020`
- SHA-256: `94db505fa6482f258c33a0a8d412050f6d843ab4ada368252e988f82f8a26fa8`

The byte stream and both hashes were independently cross-checked at the same
commit in the Texas Instruments and Cirrus Logic linux-firmware mirrors. Their
current default branches need not contain the same firmware revision, so do
not use a moving `main` digest as the P7 trust root.

Generate the local, uncommitted header with:

```sh
python3 tools/gen_psp_fw_header.py \
  /path/to/psp_13_0_7_sos.bin RedNavi/psp_fw.h \
  --expected-sha256 94db505fa6482f258c33a0a8d412050f6d843ab4ada368252e988f82f8a26fa8
```

A mismatch aborts header generation. Omitting `--expected-sha256` still
produces a development header, but sets `RN_PSP_FW_DIGEST_TRUSTED=0` and P7
will refuse the PSP hardware load.

## Execution levels

P7 remains disabled unless the existing experimental P4 build/runtime gate and
the P6 ownership probes have all succeeded.

Required existing boot arguments:

```text
rednavi_navi33_unsafe_p4=1
rednavi_navi33_dma_owner_probe=1
rednavi_navi33_vram_owner_probe=1
rednavi_navi33_allocator_carve_probe=1
```

Then add exactly one P7 execution level:

- `rednavi_navi33_p7_step=1`: first real hardware checkpoint. Populate the
  owned 1 MiB VMID0 PTE table with the exact retained 256-page IODMA list,
  verify PTE readback, program **MMHUB only**, acquire MMHUB invalidate engine
  17's semaphore, flush VMID0, then release it. GFXHUB is deliberately
  deferred until the graphics block is powered later in bring-up. No PSP
  firmware command is allowed.
- `rednavi_navi33_p7_step=2`: requires step-1 gates plus independently trusted
  PSP firmware metadata. Copy one PSP component at a time into the retained
  1 MiB host-DMA `fw_pri`, pass its GART VA through C2PMSG_36, and execute
  the PSP13 bootloader chain. For first hardware admission, add
  `rednavi_navi33_p7_psp_limit=N` to cap the number of bootloader commands.
  This makes KDB/SPL/SYS/SOS independently observable before allowing the
  whole chain to reach C2PMSG_81.
- `rednavi_navi33_p7_step=3`: after the verified PSP boot chain, map three
  retained host-DMA pages for the PSP KM/GPCOM ring, command/response buffer
  and fence, create the ring, submit LOAD_TOC, and record the PSP-calculated
  TMR size. It stops before claiming or programming TMR.
- `rednavi_navi33_p7_step=4`: P7.2 software-only TMR ownership checkpoint.
  Reuse the P6.6 quiescent visible-VRAM carve and the LOAD_TOC size to claim a
  naturally aligned TMR range. No PMFW, SETUP_TMR, IP firmware, SMU, MES or
  graphics queue command is issued.

Do not jump directly to step 2 or step 3 on a new build. Validate step 1 and
capture its IORegistry state first.

## Step-1 expected properties

`navi33-p7-gart-runtime` should show:

```text
step = 1
gate_error = 0
may_program_gart = 1
pte_verified = 1
bad_pte = 0xffffffff
flush_rc = 0
gart_ready = 1
flush_hub = 1
gfxhub_deferred = 1
mmhub_last_ack & 1 = 1
mmhub_last_sem & 1 = 1
gart_start = 0x7fff00000000
fw_pri = 0x7fff00000000
pt_offset = 0x0ff00000
```

P6 ownership and carve reports must remain green. A PTE mismatch or failed
invalidate blocks all later stages.

### MMHUB 3.0.2 register map

Navi33 advertises MMHUB 3.0.2. Real-hardware 0.0.13 testing exposed that the
native bring-up header was still generated from mmhub_3_0_0 offsets: PTE
backing itself verified, but ENG17 semaphore read zero because the driver was
accessing the 3.0.0 register locations. P7 now generates every programmed
MMHUB register from mmhub_3_0_2_offset.h / _sh_mask.h. This includes the
system aperture, L1/L2 controls, context 0 page-table registers, fault
registers, and invalidate engine 17.

### Why Step 1 uses MMHUB, not GFXHUB

Real-hardware 0.0.12 testing proved that all 256 SYSTEM PTEs were written and
read back correctly, but the GFXHUB engine-17 invalidate never acknowledged.
This matches Linux GMC11 ordering: early `gmc_v11_0_gart_enable()` brings up
MMHUB and flushes VMID0 there, while GFXHUB is owned by the later graphics
bring-up. Linux also skips a GFXHUB register flush when the graphics block is
not powered. P7 therefore treats MMHUB as the only early GART authority and
does not touch GFXHUB until PSP/SMU/GC power-on has been established.

## Step-2 expected properties

The first 0.0.15 hardware run should use:

```text
rednavi_navi33_p7_step=2
rednavi_navi33_p7_psp_limit=1
```

That deliberately executes only the first generated bootloader command (KDB).
A successful limited checkpoint should report:

```text
firmware_metadata_verified = 1
firmware_digest_trusted = 1
gart_ready = 1
may_issue_psp_bootloader = 1
chain_total = 4
chain_target = 1
chain_limited = 1
chain_done = 1
failed_index = 0xffffffff
failure_reason = 0
last_cmd = 0x00080000
last_c2pmsg35_before = 0x80000000
last_c2pmsg35_after = 0x80000000
sos_after = 0
psp_ok = 0
```

`psp_ok=0` is expected while the chain is deliberately limited. Increase the
limit only after the previous checkpoint is accepted:

```text
1 = KDB
2 = KDB + PSP13 SPL-table command
3 = KDB + SPL-table + SYS_DRV
4 = full pinned chain ending in SOS
```

For PSP13, Linux's SPL callback is intentionally unusual: `psp_hw_start()`
requires the SPL descriptor to be present, but
`psp_v13_0_bootloader_load_spl()` copies the KDB payload again before issuing
`PSP_BL__LOAD_TOS_SPL_TABLE`. P7 mirrors that behavior. The standalone SPL
descriptor bytes remain available in the generated header but are not sent by
that command.

The pinned PSP13.0.7 image therefore generates the executable sequence:

```text
KDB      -> LOAD_KEY_DATABASE      (KDB bytes)
SPL      -> LOAD_TOS_SPL_TABLE     (KDB bytes again)
SYS_DRV  -> LOAD_SYSDRV            (SYS_DRV bytes)
SOS      -> LOAD_SOSDRV            (SOS bytes)
```

Linux also allocates the ordinary bare-metal `fw_pri` in GTT/system memory
unless SR-IOV, an explicit debug VRAM option, or XGMI requires VRAM. P7's
retained host-DMA `fw_pri` behind the verified MMHUB VMID0 GART therefore
matches the normal dGPU placement model.

Step-2 failure diagnostics use:

```text
failure_reason = 0  no failure
                 1  bootloader was not ready before the command
                 2  component exceeded the 1 MiB fw_pri
                 3  non-SOS command never returned C2PMSG_35=0x80000000
                 4  SOS command did not change C2PMSG_81
                 5  MP0 base or fw_pri CPU buffer was unavailable
```

A complete four-command run should finally reach:

```text
chain_done = chain_total = 4
sos_before = 0
sos_after != 0
psp_ok = 1
```

## Step-3 / Step-4 expected properties

RX 7600 real hardware has now validated Step 3 with:

```text
runtime_dma_prepared = 1
runtime_mapped = 1
ring_created = 1
toc_accepted = 1
toc_status = 0
tmr_bytes = 0x01300000
post_gate_error = 8     # NoTmrOwnership, expected at Step 3
```

Step 4 remains software-only. For the validated 256 MiB visible aperture and
P6.6 free range, the 19 MiB TMR planner should choose:

```text
tmr_bytes = 0x01300000
offset    = 0x0d100000
mc        = 0x800d100000
pa        = 0x0d100000
```

The offset is both 1 MiB aligned and an exact multiple of the 19 MiB TMR size.
It fits inside the already-audited RedNavi visible-VRAM tail, does not overlap
the boot console or scratch/dummy/VMID0 page table reservation, and stays
inside CPU-visible VRAM.

The `pa` value follows Linux `amdgpu_gmc_vram_pa()` semantics:

```text
pa = tmr_mc - gmc.vram_start + vm_manager.vram_base_offset
```

On the validated card `MMMC_VM_FB_OFFSET == 0`, therefore `pa` is the VRAM
offset `0x0d100000`. It is **not** `BAR0 CPU physical + offset`.

A successful Step 4 should publish:

```text
navi33-p7-tmr-plan:
  error = 0
  requested = 1
  toc_accepted = 1
  carve_verified = 1
  allocator_quiescent = 1
  tmr_bytes = 19922944
  offset = 0x0d100000
  mc = 0x800d100000
  pa = 0x0d100000
  natural_aligned = 1
  inside_visible_vram = 1
  no_overlap = 1
  ownership_verified = 1
  may_write_vram = 0
  may_issue_setup_tmr = 0

navi33-p7-postpsp-runtime:
  post_gate_error = 10     # PmfwNotLoaded
  may_load_pmfw = 1
  may_setup_tmr = 0
  may_load_ip_fw = 0
  may_start_mes = 0
```

This encodes Linux's PMFW-centralized C-state ordering: ownership first, then
PMFW, then SETUP_TMR.

## Step-5 PMFW-only checkpoint

After RX 7600 real hardware validated Step 4 TMR ownership, Step 5 admits
exactly one additional PSP command: `GFX_CMD_ID_LOAD_IP_FW` for
`GFX_FW_TYPE_SMU` (18). It deliberately does **not** send SETUP_TMR, does
not send an SMU mailbox message, and does not load GC/SDMA/MES firmware.

The local PMFW source is `smu_13_0_10.bin`. RedNavi pins the current
linux-firmware source used for this checkpoint to:

```text
linux-firmware commit:
0a6871b19abf5d6e024b5d208b101ae53e7fa0de

smu_13_0_10.bin SHA-256:
9376ae64149e6b0b684898ffbc12c2230f8c50a2e9447dc7dafc95c0c16b5227

Reference-manifest BLAKE2b-256:
d6c5a9fa211985ab00438955931adff5eaf456794d38b198ce92ff39d283dc26
```

`tools/gen_smu_fw_header.py` extracts only the common AMD firmware header's
ucode payload, matching Linux's `AMDGPU_UCODE_ID_SMC` PSP load path. The
generated header marks its digest trusted only when the supplied expected
SHA-256 matches exactly.

Step 5 uses a **dedicated** GTT-style firmware staging allocation, matching
Linux's separate `firmware.fw_buf` model. It must not reuse PSP `fw_pri`.

The PMFW payload occupies 64 retained system pages and is mapped immediately
after the three PSP runtime pages:

```text
fw_pri pages     = 0..255
PSP ring/cmd/fence = 256..258
PMFW staging     = 259..322
PMFW stage VA    = 0x7fff00103000
command          = GFX_CMD_ID_LOAD_IP_FW (0x06)
fw_type          = GFX_FW_TYPE_SMU (18)
```

Every PMFW staging PTE is read back and MMHUB VMID0 is invalidated before the
command is submitted.

Real-hardware 0.0.18 testing reached the PMFW command but timed out
waiting for its fence while LOAD_TOC remained healthy. 0.0.19 then proved
that a dedicated Linux-style PMFW staging buffer is correctly mapped:

```text
stage_prepared = 1
stage_mapped = 1
stage_page_count = 64
stage_pte_verified = 1
submit_wptr_before = 16
submit_wptr_after = 32
fence_expected = 2
fence_observed = 0
fence_polls = 6000
```

That result verified host-side staging and PTE readback, but did NOT
validate the PMFW's target ASIC or prove that PSP consumed its contents.
The 0.0.19 trace cannot exclude a wrong-MP1 firmware image. Two remaining Linux-parity differences are
corrected in 0.0.20:

1. Linux creates and initializes `firmware.fw_buf` before PSP `hw_init`,
   therefore before the GPCOM ring is created. RedNavi now maps PMFW staging
   pages together with the PSP runtime pages and performs a single VMID0
   invalidate before ring creation. No GART mutation occurs between LOAD_TOC
   and LOAD_IP_FW.
2. Linux clears the PSP fence buffer once before ring initialization and
   preserves the previous fence value across submissions. RedNavi no longer
   clears the fence page for every command and records
   `fence_before -> fence_expected -> fence_observed`.

The 6-second P7.3 wait remains in place. Linux initializes
`psp_timeout=20000` and polls at roughly 150–300 us, which gives an
effective several-second timeout window.

A successful Step 5 should publish `navi33-p7-pmfw-runtime` with:

```text
step = 5
error = 0
requested = 1
psp_ready = 1
toc_accepted = 1
tmr_ownership_verified = 1
firmware_present = 1
metadata_verified = 1
digest_trusted = 1
fence_ok = 1
status = 0
accepted = 1
stage_prepared = 1
stage_mapped = 1
stage_page_count = 64
stage_pte_verified = 1
stage_bad_pte = 0xffffffff
stage_mapping_error = 0
stage_va = 0x7fff00103000
fence_before = 1
fence_expected = 2
fence_observed = 2
fence_ok = 1
status = 0
accepted = 1
may_setup_tmr = 0
```

The PSP response's `fw_addr_lo/fw_addr_hi` is recorded as evidence but is not
used as the source address and is not required to match the future TMR
placement.

After PMFW acceptance, the post-PSP policy should advance from
`PmfwNotLoaded` to `TmrNotAccepted`:

```text
navi33-p7-postpsp-runtime:
  may_load_pmfw = 1
  may_setup_tmr = 1
  may_load_ip_fw = 0
  may_start_mes = 0
```

`may_setup_tmr=1` is admission for the **next** checkpoint only; Step 5 never
issues SETUP_TMR.

## Render-pipeline boundary

For MP0 13.0.7, Linux marks PMFW centralized C-state management as enabled and
`boot_time_tmr=false`. The relevant order after the bootloader chain is:

1. create the PSP GPCOM ring;
2. LOAD_TOC to let PSP calculate the required TMR size;
3. claim an owned, naturally aligned TMR range; the validated RedNavi planner
   uses the visible-VRAM tail rather than hard-coding Linux's allocator result;
4. load SMU/PMFW through PSP **before** SETUP_TMR;
5. SETUP_TMR using MC and GPU-physical VRAM addresses derived from verified
   FB_LOCATION / FB_OFFSET geometry;
6. load the remaining GC/SDMA/MES/DMUB firmware through PSP;
7. initialize SMU and the graphics blocks;
8. bring up MES scheduler + KIQ;
9. map the first graphics queue and prove a completed fence;
10. only then expose the path to RADV/Metal translation.

The native Linux evidence also pins the 512 MiB GART at
`0x7fff00000000..0x7fff1fffffff`. P7 does not treat sOS-alive by itself as a
rendering pass.


### 0.0.21: GPCOM RPTR diagnostic checkpoint

This diagnostic build records MP0 C2PMSG_68 (GPCOM RPTR) around the
second ring submission without writing the read pointer or changing the
command/fence protocol. New `navi33-p7-pmfw-runtime` fields:

- `submit_rptr_before`: RPTR immediately before publishing frame 2
- `submit_rptr_after_kick`: RPTR immediately after C2PMSG_67 WPTR write
- `submit_rptr_final`: RPTR after fence completion or timeout

Expected frame indices: first LOAD_TOC frame occupies DWORDs 0..15,
second LOAD_IP_FW frame occupies DWORDs 16..31. Thus WPTR=32 after the
second kick; RPTR=32 would indicate the PSP has advanced past frame 2.
RPTR=16 suggests it has not retired frame 2; RPTR=0 suggests even the
first frame's pointer state is not as expected. An RPTR advance does
**not** prove successful firmware execution; inspect fence and response
status as separate evidence.

Linux PSP13 uses C2PMSG_67 for GPCOM WPTR, while C2PMSG_68 is the
GPCOM RPTR defined by `psp_gfx_ctrl`. Do not write C2PMSG_68. Do not
issue extra PSP commands on timeout. The firmware payload generator
already extracts `ucode_size_bytes` at `ucode_array_offset_bytes`,
matching Linux; `buf_size` and `buf_version` are also zero-initialized
in Linux's command path, so do not change them as an unproven fix.

Real-hardware validation remains pending. Compile-only CI does not
establish RPTR/fence progress on RX 7600.


### 0.0.22: Navi33 PMFW target binding and PSP submission hardening

**Confirmed wrong-source bug:** the previous SMU header generator only accepted
`smu_13_0_10.bin` (Navi32), while this RX7600's recorded IP Discovery
shows MP1 13.0.7. Linux selects `smu_13_0_7.bin` for MP1 13.0.7.
The two firmware images have the same extracted ucode length, 261632 bytes;
staging length, PTE readback and a self-asserted digest therefore could
not detect this error. It is a high-priority root-cause hypothesis for
the second-PSP-command fence timeout, not yet confirmed by hardware.

Generator usage (firmware and generated bytes stay local):

```sh
python3 tools/gen_smu_fw_header.py \
  build/firmware/smu_13_0_7.bin RedNavi/smu_fw.h \
  --target navi33 \
  --expected-sha256 68ec18bd605e680085c927ff72c609f8c771aff0718d0cfab58a3744dff8e5b7
make navi33-p4
```

The canonical source is linux-firmware commit
`7819871f1ff3d2928e434ee47673700ee500a9c2`.
Its full-image SHA-256 was checked to correspond to MP1 13.0.7;
the emitted payload is the common-header ucode slice, excluding
other trailing sections. A matching user-provided hash without a
reviewed firmware-to-target binding sets `target_verified=0`.

The runtime rejects metadata V1, missing target verification, and any
V2 header whose MP1 major/minor/revision differs from *actual*
Navi33 IP Discovery; it rejects before PMFW DMA allocation and
again before LOAD_IP_FW. New IORegistry fields:
`target_verified`, `target_match`, `firmware_mp1_revision`,
`hardware_mp1_revision`, `submitted`, `response_valid`,
`transport_error`, `transport_quarantined`.

The new transport state prevents reuse of a shared command buffer
after an unconfirmed PSP submission. A timeout has
`transport_error=8`, `response_valid=0`, response words left as
`0xffffffff`, and `transport_quarantined=1`, even if a fence completes
late. Restart from a fresh boot after timeout. PSP13 ring creation now
waits 20 ms before polling its response; an additional fence sample
runs after the final sleep. x86 DMA publish/response checks use
`__sync_synchronize()`; `OSSynchronizeIO()` is a no-op on x86.
Pages exposed by partial GART writes are retained through teardown
even when readback or invalidate fails.

A successful Step 5 should demonstrate `target_match=1`,
`fence_expected=2`, `fence_observed=2`, `response_valid=1`,
`transport_error=0`, and `accepted=1`. Host tests and compiling
cannot establish that the real RX7600 has completed the PSP command.
The following work is a retained TMR reservation and SETUP_TMR;
Step 6 is not implemented and cannot be enabled by a boot argument.



## 0.0.23 — P7.4 Step 6: experimental SETUP_TMR

The RX 7600 P7.3 test on 2026-11-11 confirmed all four PSP13
bootloader steps, a successful LOAD_TOC, accepted Navi33 PMFW,
a fence transition 1 -> 2, ring RPTR 16 -> 32, and the post-PSP
`may_setup_tmr=1` admission. The TMR plan from that card is:
`tmr_bytes=0x01300000`, VRAM offset `0x0d100000`,
GPU MC `0x800d100000`, GPU VRAM physical `0x0d100000`.

Linux references:
- `drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c`
  `psp_prep_tmr_cmd_buf`: `GFX_CMD_ID_SETUP_TMR=0x05`,
  `buf_phy_addr_lo/hi=tmr_mc`, `buf_size=tmr_bo.size`,
  `virt_phy_addr=1`, `system_phy_addr_lo/hi=amdgpu_gmc_vram_pa()`.
- `drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h`
  `struct psp_gfx_cmd_setup_tmr` has exactly six DWORDs,
  with `virt_phy_addr` at flag **bit 1** (`0x2`).

These addresses are **different address domains**: MC is the GPU
framebuffer-MC address, PA is the GPU VRAM physical offset translated
from FB_OFFSET, and neither field is a CPU BAR0 physical address.

Step 6 is a separate opt-in:
```text
rednavi_navi33_p7_step=6
```
This **alone does not issue SETUP_TMR**. It publishes the read-only
`navi33-p7-tmr-runtime` admission report with `gate_error=2`
(`NoExplicitOptIn`). To execute the PSP command, add:
```text
rednavi_navi33_p74_setup_tmr=1
```

The new pure policy verifies exact IP Discovery, completed PSP sOS,
GPCOM ring, prior PMFW acceptance, previous fence 2, quiescence, full
TMR ownership, 1 MiB size/alignment, exact Linux MC/PA translations,
and containment in the exclusive free left-tail carve. Any failure
blocks the third GPCOM write. This experiment does not automatically
start GC, SDMA, MES, or create graphics queues. Keep a bootable
0.0.22 fallback EFI; a failed PSP transaction may hang the only GPU.

If SETUP_TMR succeeds:
```text
navi33-p7-tmr-runtime:
  step = 6
  gate_error = 0
  may_issue = 1
  command_id = 5
  tmr_size = 19922944
  tmr_mc_lo = 219152384
  tmr_mc_hi = 128
  tmr_pa_lo = 219152384
  tmr_pa_hi = 0
  tmr_flags = 2
  prior_fence_expected = 2
  prior_fence_observed = 2
  submitted = 1
  fence_expected = 3
  fence_observed = 3
  response_valid = 1
  fence_ok = 1
  status = 0
  accepted = 1
```

After acceptance `navi33-p7-postpsp-runtime` advances from
`post_gate_error=11` (TmrNotAccepted) to `12`
(IpFirmwareIncomplete), and `may_load_ip_fw=1`. This only
authorizes **future development**, not immediate graphics firmware
execution. Unexpected timeouts quarantine the shared PSP ring,
and no further commands may be sent before reboot.

Note: the current visible-VRAM free-range ledger is a software
ownership contract, not an implementation of a full operating-system
VRAM allocator. Do not run the experiment if another GPU owner or
another driver may concurrently allocate the candidate region.



## 0.0.24: P7.5 SDMA6.0.2 Context Thread, one-PSP-command checkpoint

The 2026-11-11 RX 7600 hardware report confirmed Step 6 SETUP_TMR:
`fence_expected=3`, `fence_observed=3`, `response_valid=1`,
`status=0`, `accepted=1`, and `may_load_ip_fw=1`.
The third PSP frame used WPTR 32→48; its RPTR remained 32 at the
final snapshot despite a valid fence/status. Do not use RPTR alone as
success or failure; observe it along with the fence on Step 7.

The upstream kernel uses `sdma_6_0_2.bin` for Navi33 SDMA 6.0.2.
`struct sdma_firmware_header_v2_0` encodes Context Thread at
`ucode_array_offset_bytes`, length `ctx_ucode_size_bytes`, and
Control Thread separately at `ctl_ucode_offset`, length
`ctl_ucode_size_bytes`. PSP `GFX_FW_TYPE_SDMA_UCODE_TH0=71`,
`GFX_CMD_ID_LOAD_IP_FW=6`. This checkpoint **loads only TH0**;
it does not load TH1, start SDMA engines, or claim an operational ring.

Avoid repeating the earlier Navi32 PMFW bug: the generator requires the
exact separately reviewed 2025-04-15 linux-firmware mirror snapshot,
even when the caller has another image with the right filename.
Pin (file itself is not committed):
- source: `beagleboard/mirror-linux-firmware`;
- commit: `2f411c10e4570d03426d3cb095471aa1120114ab`;
- file: `amdgpu/sdma_6_0_2.bin`, 34,560 bytes;
- reviewed Git blob SHA-1: `28dd03efcb3a134edcf066d0820d0c2051879525`.

On macOS, download the exact source and generate an ignored local
compile-time header:

```sh
cd ~/RedNavi
mkdir -p ~/RedNavi-firmware
curl -fL --retry 3 \
  'https://raw.githubusercontent.com/beagleboard/mirror-linux-firmware/2f411c10e4570d03426d3cb095471aa1120114ab/amdgpu/sdma_6_0_2.bin' \
  -o ~/RedNavi-firmware/sdma_6_0_2.bin

python3 tools/gen_navi33_sdma_fw_header.py \
  ~/RedNavi-firmware/sdma_6_0_2.bin RedNavi/navi33_sdma_fw.h

make clean
make test
make navi33-p4
```

The generator verifies the *Git blob object hash* over the exact
source bytes (Git's `blob <length>\\0<contents>` form), SDMA v2.0
header, IP6.0 version, and both thread offsets. The generated
header is Git-ignored and must not be pushed.

Safety: Step 7 without a second opt-in is a read-only P7.5
admission report after the already validated Step 6.
First boot:
```text
rednavi_navi33_p7_step=7
rednavi_navi33_p74_setup_tmr=1
# do not add the P75 opt-in yet
```
Expect `navi33-p7-sdma-runtime.gate_error=2`,
`submitted=0`. After verifying that phase, in a separate
boot add:
```text
rednavi_navi33_p75_sdma_th0=1
```

Step 7 refuses wrong IP Discovery (must be SDMA 6.0.2), unreviewed
firmware, missing PMFW/TMR acceptance, fence3 mismatch,
quarantined ring, invalid GART VA, unmapped stage, or too-large
firmware. Only after all gates pass does it reuse the **existing**
PSP-mapped PMFW stage, copy TH0, publish an x86 DMA memory barrier,
and submit one LOAD_IP_FW(type=71). No new PTEs/GART changes.
The host DMA backing is retained until reboot.

Expected Step 7 success:
```text
navi33-p7-sdma-runtime:
  gate_error = 0
  may_load = 1
  firmware_ip_revision = 2
  observed_ip_revision = 2
  source_verified = 1
  cmd_id = 6
  fw_type = 71
  prior_fence_expected = 3
  prior_fence_observed = 3
  submitted = 1
  fence_expected = 4
  fence_observed = 4
  fence_ok = 1
  response_valid = 1
  status = 0
  accepted = 1
  sdma_engine_started = 0
```

PSP acceptance **does not prove** a running SDMA engine or graphics
acceleration; next steps are SDMA Control Thread and then the
GC11.0.2 firmware path. Keep a known-bootable 0.0.23 EFI.



## 0.0.25 P7.6: Navi33 SDMA Control Thread (TH1), fifth PSP fence

Real RX 7600 Step 7 checkpoint (0.0.24) succeeded 2026-11-11:
`firmware_ip_revision=observed_ip_revision=2`,
`source_verified=1`, `fw_type=71`,
`firmware_bytes=17408`, `fence_before=3`,
`fence_expected=fence_observed=4`, `status=0`, `accepted=1`,
`transport_error=0`. GPCOM `WPTR` and final `RPTR`
advanced `48 -> 64`. This establishes a strong checkpoint for
issuing a *fifth* PSP command: SDMA6.0.2 **Control Thread** TH1.

This branch contains **no new vendor firmware**; it uses the same
locally generated, Git-blob-verified `navi33_sdma_fw.h`, where the
already pinned `sdma_6_0_2.bin` has exactly 17,408 context
bytes and 16,896 control bytes. Linux `psp_gfx_if.h` defines
`GFX_FW_TYPE_SDMA_UCODE_TH1=72` and
`GFX_CMD_ID_LOAD_IP_FW=6`. Only the TH1 payload is submitted
in the fifth GPCOM frame. Both TH0 and TH1 PSP acceptance mean
**firmware loaded**, not an initialized SDMA engine.

### Build

```sh
cd ~/RedNavi
git fetch origin
git switch -c feature/navi33-p76-sdma-th1 --track origin/feature/navi33-p76-sdma-th1
# Existing generated PSP, SMU and SDMA headers are required.
test -f RedNavi/psp_fw.h &&
test -f RedNavi/smu_fw.h &&
test -f RedNavi/navi33_sdma_fw.h
make clean
make test && make navi33-p4
```

### Boot protocol

Retain previous explicit opt-ins for P7.4 and P7.5:
`rednavi_navi33_p74_setup_tmr=1` and
`rednavi_navi33_p75_sdma_th0=1`.

First, set `rednavi_navi33_p7_step=8`, but **do not enable**
`rednavi_navi33_p76_sdma_th1=1`. The prior steps *still*
execute as before (PMFW, TMR, SDMA TH0), but new
`navi33-p7-sdma-th1-runtime` should show:
```text
step=8 gate_error=2 explicit_opt_in=0 submitted=0 accepted=0
prior_fence_expected=4 prior_fence_observed=4 th0_accepted=1
```

On a second boot, only after confirming Step 8 first-round safety
and keeping 0.0.24 fallback EFI, enable the separate
`rednavi_navi33_p76_sdma_th1=1` arg. Expected fifth-command
telemetry:
```text
navi33-p7-sdma-th1-runtime:
  gate_error=0 may_load=1 th0_accepted=1
  firmware_bytes=16896 fw_type=72 command_id=6
  submitted=1
  submit_wptr_before=64 submit_wptr_after=80
  fence_before=4 fence_expected=5 fence_observed=5
  response_valid=1 status=0 accepted=1
  transport_error=0 transport_quarantined=0
  sdma_firmware_both_loaded=1 sdma_engine_started=0
```
RPTR must be observed but a transient lag by itself is not a
failure if fence/status completed successfully. The fifth
submission reuses the existing retained PSP-mapped host-DMA
firmware staging after TH0 fence=4, preserving GART mappings.
The source stays validated against the same pinned Linux firmware
blob, and every previous gate must succeed again in this boot.

No VM/SDMA queues are created; no SDMA register programming or GC
firmware loading occurs in Step 8. `may_start_mes` must remain 0.
The `navi33-p7-postpsp-runtime` report gains
`sdma_firmware_both_loaded`, while `post_gate_error=12`
remains expected until GC firmware is also ready.

In case of a timeout or driver hang, do **not** reissue a PSP
command in the same session. Boot known-good 0.0.24 USB EFI.



## 0.0.26 P7.7: GC11.0.2 RS64 PFP instruction microcode (sixth PSP fence)

The real RX 7600 on 2026-11-11 verified P7.6:
`sdma_firmware_both_loaded=1`, TH1 type=72,
`fence_before=4`, `fence_expected=5`,
`fence_observed=5`, `status=0`, `accepted=1`,
`transport_error=0`, and both ring pointers 64→80. This
checkpoint starts **GC11.0.2 firmware** with **only RS64 PFP
instruction microcode**, not PFP data/stack, ME/MEC, RLC, IMU,
MES, SDMA ring init or graphics acceleration.

The Linux kernel `gfx_v11_0.c` enables RS64 for GC11.0.2 when
`gfx_firmware_header_v2_0` is present. `amdgpu_psp.c` maps
`AMDGPU_UCODE_ID_CP_RS64_PFP` to
`GFX_FW_TYPE_RS64_PFP=87`. PSP `GFX_CMD_ID_LOAD_IP_FW=6`.
The pinned source image `gc_11_0_2_pfp.bin` has:
- full file 230,496 bytes
- common header v2.0, GC IP11.0 (Navi33 exact rev2 pinned by hash)
- RS64 PFP code offset 256, code length 90,976 bytes
- RS64 PFP stack/data offset 91,232, size 139,264 bytes
- only the code part is staged, within existing 262,144-byte DMA area.

Firmware is not committed to the repository. Source is pinned to
`beagleboard/mirror-linux-firmware` commit
`2f411c10e4570d03426d3cb095471aa1120114ab`;
reviewed binary Git blob SHA-1
`4d18151728ccc513fbabddec25f58dcccc8eee2a`.
The generator refuses files with the wrong name, hash, header,
code/data offsets, IP, or stage limit.

### Local build

```sh
cd ~/RedNavi
git fetch origin
git switch --track origin/feature/navi33-p77-gc-pfp
mkdir -p ~/RedNavi-firmware
curl -fL --retry 3 \
  'https://raw.githubusercontent.com/beagleboard/mirror-linux-firmware/2f411c10e4570d03426d3cb095471aa1120114ab/amdgpu/gc_11_0_2_pfp.bin' \
  -o ~/RedNavi-firmware/gc_11_0_2_pfp.bin
python3 tools/gen_navi33_gc_pfp_fw_header.py \
  ~/RedNavi-firmware/gc_11_0_2_pfp.bin RedNavi/navi33_gc_pfp_fw.h
make clean
make test && make navi33-p4
```

### Separate safety checkpoint

First boot `rednavi_navi33_p7_step=9` with all **previous**
opt-ins set (SETUP_TMR, SDMA TH0, SDMA TH1), but **omit**
`rednavi_navi33_p77_gc_pfp=1`. Previously successful PSP
commands still run; P7.7 is read-only and expected to publish
`navi33-p7-gc-pfp-runtime.gate_error=2`, `submitted=0`,
`sdma_both_accepted=1`, previous fence expected/observed 5.
Keep a separate bootable 0.0.25 EFI and a backup of config.plist.

Only after verifying Step 9 admission and the known-good EFI,
explicitly opt-in with `rednavi_navi33_p77_gc_pfp=1` in a
second fresh boot. The policy requires exact IP Discovery GC11.0.2,
the pinned, locally generated PFP source, both SDMA threads
accepted during this boot, previous PSP fence 5, retained/still
mapped host-DMA firmware stage, and a healthy PSP ring.

Expected outcome:
```text
navi33-p7-gc-pfp-runtime:
  gate_error=0 may_load=1
  sdma_both_accepted=1 source_verified=1
  observed_gc_revision=2 firmware_gc_revision=2
  command_id=6 fw_type=87 firmware_bytes=90976
  prior_fence_expected=5 prior_fence_observed=5
  submitted=1
  submit_wptr_before=80 submit_wptr_after=96
  fence_before=5 fence_expected=6 fence_observed=6
  fence_ok=1 response_valid=1 status=0
  accepted=1 gc_pfp_firmware_loaded=1 gc_engine_started=0
```

The new code does not set post-PSP `gcFirmwareLoaded`: one PFP
instruction image is not complete GC firmware. Consequently,
`post_gate_error=12` (IpFirmwareIncomplete) and
`may_start_mes=0` are still expected. On PSP timeout or
hang, no more PSP ring commands may be issued until reboot.



## 0.0.27 P7.8: RS64 PFP P0_STACK, isolated seventh PSP fence

The 2026-11-11 RX 7600 Step 9 trace proved that the Navi33
GC11.0.2 RS64 PFP instruction microcode (type=87, 90,976 B)
was PSP-accepted at `fence=6/6`, `status=0`,
`response_valid=1` with ring WPTR/RPTR 80→96.
Both SDMA threads were still accepted and transport was healthy.

Linux `amdgpu_ucode.c` maps both RS64 PFP P0_STACK and P1_STACK
to the **same data slice** of `gc_11_0_2_pfp.bin`:
`gfx_firmware_header_v2_0.data_offset_bytes=91232`,
`data_size_bytes=139264` in the separately reviewed pinned image.
However, `psp_gfx_if.h` defines distinct PSP firmware types:
`GFX_FW_TYPE_RS64_PFP_P0_STACK=90` and
`GFX_FW_TYPE_RS64_PFP_P1_STACK=91`.
**Step 10 submits only P0_STACK (type=90); P1 is next.**

The existing reviewed source remains pinned to Git blob
`4d18151728ccc513fbabddec25f58dcccc8eee2a`
from `beagleboard/mirror-linux-firmware` commit
`2f411c10e4570d03426d3cb095471aa1120114ab`.
The generator now emits both code and data arrays into the
ignored, locally built `RedNavi/navi33_gc_pfp_fw.h`.
Old generated headers *must be regenerated* after switching
to this branch or the Step 10 code will not compile.

### Build on macOS

```sh
cd ~/RedNavi
git fetch origin
git switch --track origin/feature/navi33-p78-gc-pfp-p0
python3 tools/gen_navi33_gc_pfp_fw_header.py \
  ~/RedNavi-firmware/gc_11_0_2_pfp.bin RedNavi/navi33_gc_pfp_fw.h
make clean
make test && make navi33-p4
```

Keep a verified 0.0.26 USB OpenCore EFI backup before installing 0.0.27.

### Safe two-boot test

First boot must retain all four prior PSP opt-ins:
`rednavi_navi33_p74_setup_tmr=1`,
`rednavi_navi33_p75_sdma_th0=1`,
`rednavi_navi33_p76_sdma_th1=1`,
`rednavi_navi33_p77_gc_pfp=1`.
Set `rednavi_navi33_p7_step=10` but do NOT set the new
`rednavi_navi33_p78_pfp_p0=1` argument yet. Expect:
```text
navi33-p7-gc-pfp-p0-runtime:
  step=10 gate_error=2 explicit_opt_in=0 submitted=0 accepted=0
  pfp_accepted=1 prior_fence_expected=6 prior_fence_observed=6
  firmware_bytes=139264 fw_type=90
```

After real hardware read-only acceptance, start a second fresh boot
with `rednavi_navi33_p78_pfp_p0=1`. The seventh PSP command is
now admitted only if the current boot's RS64 PFP firmware load at
fence 6 succeeded, the exact GC IP 11.0.2 and pinned source match,
all prior SDMA/PMFW/TMR prerequisites succeeded, the unchanged
host-DMA staging PTEs remain valid, and PSP transport is healthy.

Expected fifth? No — this is the **seventh PSP GPCOM command**:
```text
navi33-p7-gc-pfp-p0-runtime:
  gate_error=0 may_load=1
  firmware_bytes=139264 command_id=6 fw_type=90
  submitted=1
  submit_wptr_before=96 submit_wptr_after=112
  fence_before=6 fence_expected=7 fence_observed=7
  status=0 response_valid=1 accepted=1
  gc_pfp_p0_loaded=1 gc_engine_started=0
```
The PFP P0_STACK load does not start any graphics engines,
complete GC firmware, or enable MES. In particular,
`post_gate_error=12` and `may_start_mes=0` remain expected.
Do not retry a stalled command without reboot.
