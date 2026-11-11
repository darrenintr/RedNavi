# Navi33 P7.19/P7.20 IMU I/D PSP firmware checkpoint

Real RX 7600 / RedNavi 0.0.37 has successfully loaded RLC-G,
PSP firmware type 8, 25088 bytes, fence 17/17, status 0,
ring RPTR=272. This is PSP acceptance, not GPU execution.

Linux reference: torvalds/linux commit
7475ea8b58616b33c04afb976cf0fbaad9dd63fb,
drivers/gpu/drm/amd/amdgpu/imu_v11_0.c, amdgpu_ucode.c,
amdgpu_psp.c, gfx_v11_0.c. The IMU is split into I (type 68)
and D (type 69), with PSP payloads following the IMU header
v1.0 common array offset. Separately, RLC autoload, IMU
setup, GFX CP initialization and MES remain unimplemented.

Pinned IMU file:
- Repo beagleboard/mirror-linux-firmware
- Commit 2f411c10e4570d03426d3cb095471aa1120114ab
- Path amdgpu/gc_11_0_2_imu.bin
- Whole file 132352 bytes
- Git blob SHA-1 7d0941dadc1f8ac1584ee763b357604503fca6d9
- Header 1.0, firmware version 186730240
- IMU_I 66048 bytes, IMU_D 66048 bytes
- Both fit 270336-byte 66-page DMA stage.

The experimental order loads I/D *after* previously verified
RLC-G. This is not a claim to reproduce all the selected
Linux PSP autoload and initialization ordering.

## Local build

Keep all local generated source headers and an independent,
working older EFI; never overwrite the only bootable recovery
USB. Run on the Mac from the project directory:

~~~sh
cd ~/RedNavi
git fetch origin
git switch --track origin/feature/navi33-p719-p720-imu
curl -fL --retry 3 \
  "https://raw.githubusercontent.com/beagleboard/mirror-linux-firmware/2f411c10e4570d03426d3cb095471aa1120114ab/amdgpu/gc_11_0_2_imu.bin" \
  -o /tmp/gc_11_0_2_imu.bin
python3 tools/gen_navi33_gc_imu_fw_header.py \
  /tmp/gc_11_0_2_imu.bin RedNavi/navi33_gc_imu_fw.h
make test
make check-kernel-psp-firmware
make navi33-p4
~~~

Stop on source mismatch, host test or build failure. Only
the local firmware-enabled 0.0.38 build is suitable for
experimentation; CI artifacts omit proprietary firmware.

## Four-boot testing protocol, no combined first attempt

1. P7.19 admission-only: leave every successful prior
   boot-arg (including rednavi_navi33_p718_rlc_g=1).
   Change rednavi_navi33_p7_step=20 to step=21.
   DO NOT add rednavi_navi33_p719_imu_i=1. Expected:
   gc-imu-i-runtime gate_error=2, explicit_opt_in=0,
   submitted=0, rlc_g_accepted=1, prior fence 17/17,
   fw_type=68, firmware_bytes=66048, source_verified=1.
2. Fresh power cycle with rednavi_navi33_p719_imu_i=1:
   expect I accepted at fence 18/18, fw_type=68,
   gc_imu_i_loaded=1, status=0, transport_error=0,
   ring RPTR from 272 to 288.
3. P7.20 admission-only, fresh power cycle:
   retain all prior opt-ins, set
   rednavi_navi33_p7_step=22, do NOT add
   rednavi_navi33_p720_imu_d=1. Expect D gate_error=2,
   submitted=0, imu_i_accepted=1, prior fence 18/18,
   fw_type=69, firmware_bytes=66048.
4. P7.20 fresh power cycle with
   rednavi_navi33_p720_imu_d=1: expect fence 19/19,
   status=0, gc_imu_d_loaded=1, transport_error=0,
   ring RPTR normally from 288 to 304.

Check using:
~~~sh
sysctl kern.bootargs
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe \
 | grep -E '"navi33-p7-(gc-imu-[id]-runtime|gc-rlc-g-runtime|postpsp-runtime|render-pipeline)"'
~~~

Never resubmit a timed-out or quarantined PSP ring command
inside one boot. For a hang or kernel panic, restore your
independently bootable known-good EFI.

## Not yet accomplished

RLC/IMU executing: NO. RLC autoload started: NO.
GC11.0.2 engine start: NO. MES Scheduler/KIQ: NO.
Graphics queue: NO. Metal/WindowServer acceleration: NO.
A successful PSP ring fence is not a graphics workload fence.
Next work is RLC autoload/IMU status, then GFXHUB and
command processor init, followed by a real scratch register
ring test, GPU writeback/IB fence, and offscreen pixel readback.
