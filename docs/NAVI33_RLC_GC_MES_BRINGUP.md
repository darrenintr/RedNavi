# Navi33: PSP firmware -> RLC autoload -> real GC/MES queue

**Experimental, not for automatic injection.**
Last *hardware validated* baseline: RedNavi **0.0.38**, Step22,
RX7600 PSP IMU_D type69, fence19/19, RPTR304, status0, no
PSP transport failure. RLC/IMU/GC/MES engines and queues
NOT proven running. Keep the 0.0.38 recovery EFI on a
separate **bootable** device before changing an experimental
EFI. A desktop config backup is not an independent bootloader.

## Linux source / hardware semantics

Pinned Linux source: torvalds/linux
7475ea8b58616b33c04afb976cf0fbaad9dd63fb.

* amdgpu_mes.c on GC11 selects gc_11_0_2_mes_2.bin for
  MES Scheduler and gc_11_0_2_mes1.bin for KIQ.
* amdgpu_ucode.c extracts each MES v1.0 code/data offset;
  amdgpu_psp.c PSP fw types: MES code=33, MES stack=34,
  KIQ code=81, KIQ stack=82.
* amdgpu_psp.c::psp_rlc_autoload_start issues the actual
  GFX_CMD_ID_AUTOLOAD_RLC command **0x21**. This may start
  an engine: it is not another LOAD_IP_FW (0x06) request.
* gfx_v11_0_wait_for_rlc_autoload_complete tests
  CP_STAT == 0 and RLC_RLCS_BOOTLOAD_STATUS bit31 set.
  PSP fence completion alone is not RLC execution.
* After RLC autoload, Linux initializes GC GFXHUB/RS64,
  golden registers, RLC resume, CP resume, and MES.
  RedNavi currently does NONE of these later writes.

Important: the existing P7 sequence already PSP loaded RLC_G
at fence17 and IMU I/D at fences18/19, then stages MES.
Linux's autoload may occur when RLC_G is loaded after
earlier MES firmware registration. Matching individual
PSP acceptance is NOT proof of equivalent overall ordering.
Review this discrepancy before attempting actual RLC start.

## Firmware source

All vendor binaries are external. Pinned mirror:
beagleboard/mirror-linux-firmware at commit
2f411c10e4570d03426d3cb095471aa1120114ab.

| Image | Whole bytes | Git blob SHA-1 | Code bytes | Data bytes |
|---|---:|---|---:|---:|
| gc_11_0_2_mes_2.bin | 256288 | 693e5796e64f69d8c79e3ca8180be8c8928d9c26 | 124960 | 131072 |
| gc_11_0_2_mes1.bin | 218400 | e10f5a044e93acce3cef5038da4afaff6ad3e6f9 | 87072 | 131072 |

Each executable or stack segment fits the previously tested
270336-byte DMA stage. Generator verifies exact Git blob SHA
and sizes before extracting Linux's MES-specific spans.
No vendor payload is committed to Git.

Build locally with all prior generated PSP/PMFW/SDMA/PFP/
ME/MEC/RLC/IMU headers PRESERVED:

~~~sh
cd ~/RedNavi
git fetch origin
git switch --track origin/feature/navi33-rlc-gc-mes-startup

curl -fL --retry 3 \
 "https://raw.githubusercontent.com/beagleboard/mirror-linux-firmware/2f411c10e4570d03426d3cb095471aa1120114ab/amdgpu/gc_11_0_2_mes_2.bin" \
 -o /tmp/gc_11_0_2_mes_2.bin
curl -fL --retry 3 \
 "https://raw.githubusercontent.com/beagleboard/mirror-linux-firmware/2f411c10e4570d03426d3cb095471aa1120114ab/amdgpu/gc_11_0_2_mes1.bin" \
 -o /tmp/gc_11_0_2_mes1.bin

python3 tools/gen_navi33_mes_fw_header.py \
 /tmp/gc_11_0_2_mes_2.bin /tmp/gc_11_0_2_mes1.bin \
 RedNavi/navi33_mes_fw.h

make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make navi33-p4
/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' \
 build/Release/RedNaviNavi33P4.kext/Contents/Info.plist
~~~

Local version should be **0.0.39**. Do NOT install if
source verification, test, compiler or signing fails.
CI tests lack real private vendor firmware and do not
prove a firmware-enabled local build or GPU behavior.

## Four MES checkpoints: do not combine

For each checkpoint, independently boot once WITHOUT that
step's new opt-in, verify gate_error=2, submitted=0 and
the prior PSP fence. Then power-cycle, enable ONLY that
step's opt-in and verify PSP status=0, accepted=1,
response_valid=1, fence observed=expected and no transport
quarantine. Preserve all previous successful flags.

| Step | PSP load | Separate boot arg | Expected fence |
|---|---|---|---|
| 23 | MES Scheduler Code type33 | rednavi_navi33_p721_mes_sched_code=1 | 20 |
| 24 | MES Scheduler Data type34 | rednavi_navi33_p722_mes_sched_data=1 | 21 |
| 25 | MES KIQ Code type81 | rednavi_navi33_p723_mes_kiq_code=1 | 22 |
| 26 | MES KIQ Data type82 | rednavi_navi33_p724_mes_kiq_data=1 | 23 |

For steps23–26 inspect the corresponding
navi33-p7-mes-*-runtime property. STOP if the unexpected
status, fence, RPTR or quarantine suggests PSP ring trouble.
Do not infer that MES firmware acceptance starts MES engines.

## Step27: actual RLC autoload, hazardous

DEFAULT 0.0.39 KEXT COMPILE DISABLES AUTOLOAD_RLC
even if the opt-in boot argument is present.
With step=27 and no new boot flag, expect
navi33-p7-rlc-autoload-runtime:
  gate_error=2, explicit_opt_in=0, unsafe_build_enabled=0,
  submitted=0, prior_fence_expected=23,
  prior_fence_observed=23.

Only after validating all previous MES checkpoints and
independent hardware/firmware ordering and SMU readiness:
rebuild an intentionally unsafe test driver. Because Make
does NOT track compiler flag changes, a clean rebuild is
mandatory:

~~~sh
make clean
make NAVI33_RLC_AUTOLOAD_UNSAFE=1 navi33-p4
~~~

In a separately recoverable, deliberate real GPU test,
require BOTH the Step27 boot argument and the independent
opt-in:
  rednavi_navi33_p7_step=27
  rednavi_navi33_p725_rlc_autoload=1

Only if all PSP/GART/TMR/PMFW/SDMA/PFP/ME/MEC/RLC/IMU/
MES Scheduler/KIQ gates and prior fence23 succeed can
the driver issue PSP command 0x21. It is expected to
produce PSP fence24 if accepted; this is NOT hardware
success. Actual success requires
  rlc_bootload_complete=1
  cp_stat=0
  bootload_status bit31 set.
Driver polls these two read-only GC11 registers for at
most 250 ms after PSP acknowledged the command.

On black screen, kernel panic, PSP timeout or invalid
readback, recover with the separate known-good EFI.
Do NOT retry a timed-out or quarantined command in the
same boot. After receiving the first RLC boot status,
STOP and review it before adding GC/queue writes.

## What remains for a real graphics queue

The new navi33-p7-gc-queue-preflight strictly distinguishes:
- firmware load accepted;
- RLC hardware bootcomplete;
- GFXHUB + RS64 + GC golden register initialization;
- MES Scheduler and KIQ EXECUTING (not merely loaded);
- owned queue memory + doorbell + interrupt;
- completed real GPU scratch writeback fence.

A private diagnostic queue can only be attempted after
hardware readiness, and user-facing queue availability
requires the real scratch GPU fence. At this commit the
GC/GFXHUB/MES/queue writes remain OFF, no diagnostic ring
is submitted, no Metal device or WindowServer acceleration
can be claimed. Next engineering work: verify GC register
readbacks and owned GFXHUB state, bring up MES Scheduler
and KIQ, create one bounded queue, test a single scratch
writeback GPU fence, then test offscreen pixel output.
