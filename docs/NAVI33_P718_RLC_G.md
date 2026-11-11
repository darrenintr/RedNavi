# Navi33 P7.18: RLC-G PSP firmware checkpoint

**This is experimental.** Real RX 7600 P7.17 verified:
PSP fence 16/16, LOAD_IP_FW type 97 / MEC P3 accepted,
status=0, healthy ring RPTR=256, GART stage 270336 bytes,
and no graphics engine execution. Preserve version 0.0.36 EFI
as a separate bootable recovery image. Never overwrite it.

## Linux reference and provenance

Linux drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c at
commit 7475ea8b58616b33c04afb976cf0fbaad9dd63fb
requests amdgpu/gc_11_0_2_rlc.bin for GC11.0.2.
psp_gfx_if.h defines GFX_FW_TYPE_RLC_G = 8.
The PSP RLC-G payload is the **common-header ucode
region**, not the whole .bin firmware container.

Pinned source: beagleboard/mirror-linux-firmware
commit 2f411c10e4570d03426d3cb095471aa1120114ab:
* amdgpu/gc_11_0_2_rlc.bin
* Git blob 062f6da3e670b256d537ef52ee9b9ad44f1e4b8c
* source size 178848 bytes

No proprietary binary or generated source header is committed.
The local header generator verifies the entire source blob,
then extracts the RLC-G executable payload and its metadata.

## Build

From ~/RedNavi on the macOS machine, preserving all previous
locally-generated PSP, SDMA, PFP, ME and MEC headers:

~~~sh
git fetch origin
git switch --track origin/feature/navi33-p718-rlc-g

curl -fL --retry 3 \
  "https://raw.githubusercontent.com/beagleboard/mirror-linux-firmware/2f411c10e4570d03426d3cb095471aa1120114ab/amdgpu/gc_11_0_2_rlc.bin" \
  -o /tmp/gc_11_0_2_rlc.bin

python3 tools/gen_navi33_gc_rlc_fw_header.py \
  /tmp/gc_11_0_2_rlc.bin RedNavi/navi33_gc_rlc_fw.h

make test
make check-kernel-psp-firmware
make navi33-p4
~~~

Stop if SHA, size, firmware header, host tests, syntax or
compile fail. Verify the locally built kext version is
0.0.37. GitHub CI cannot ship real vendor firmware.

## Two-boot hardware validation, separate experimental EFI

1. Only after successful builds, install local 0.0.37 kext
   on the experimental EFI (not the separate recovery EFI).
   Keep all verified P7.17 opt-ins including
   rednavi_navi33_p717_mec_p3=1. Set
   rednavi_navi33_p7_step=20, but do NOT enable
   rednavi_navi33_p718_rlc_g=1. The RLC-G IORegistry
   property should report:
   step=20, gate_error=2, explicit_opt_in=0, submitted=0,
   mec_p3_accepted=1, prior_fence_expected=16,
   prior_fence_observed=16, source_verified=1,
   firmware_present=1, metadata_verified=1,
   fw_type=8, stage_pte_verified=1.
2. Only after that, do a **fresh power-cycle** and add
   rednavi_navi33_p718_rlc_g=1. If successful,
   fence_expected=17, fence_observed=17,
   response_valid=1, status=0, accepted=1,
   gc_rlc_g_loaded=1, transport_error=0,
   transport_quarantined=0.
   RPTR is expected to reach 272 on uninterrupted
   16-byte PSP command frames, but measure rather than
   assume this is proof of success.

A timeout or quarantined ring means no retries in the same
boot; revert to the known-good separate EFI on failure.

## Explicit non-goals and graphics proof

A PSP firmware acceptance does NOT start the RLC, IMU,
GC11 graphics engines, MES Scheduler/KIQ or queues.
The post-PSP error and may_start_mes=0 remain expected.
Next:
1. Pin/validate gc_11_0_2_imu.bin, IMU I/D sections,
   RLC autoload prerequisite records.
2. GFXHUB/doorbells/golden registers and owned queue memory.
3. MES KIQ and Scheduler, a real GFX scratch test.
4. Executed GPU writeback/IB fence, offscreen pixel readback.
5. Metal compositor and WindowServer engagement.
Never report GPU desktop rendering based on PSP fences alone.
