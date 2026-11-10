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
- `rednavi_navi33_p7_step=3`: currently only publishes render/queue admission
  after sOS is alive. It deliberately does not start MES or a graphics queue.

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

## Render-pipeline boundary

For MP0 13.0.7, Linux marks PMFW centralized C-state management as enabled and
`boot_time_tmr=false`. The relevant order after the bootloader chain is:

1. create the PSP GPCOM ring;
2. LOAD_TOC to let PSP calculate the required TMR size;
3. reserve the observed PSP TMR (same-card Linux capture: `0x1300000` bytes
   from GPU MC `0x81fc000000`);
4. load SMU/PMFW through PSP **before** SETUP_TMR;
5. SETUP_TMR;
6. load the remaining GC/SDMA/MES/DMUB firmware through PSP;
7. initialize SMU and the graphics blocks;
8. bring up MES scheduler + KIQ;
9. map the first graphics queue and prove a completed fence;
10. only then expose the path to RADV/Metal translation.

The native Linux evidence also pins the 512 MiB GART at
`0x7fff00000000..0x7fff1fffffff`. P7 does not treat sOS-alive by itself as a
rendering pass.
