# Navi33: RedNavi Linux `amdgpu` boot-parity path

Status: **read-only runtime preflight + host-only initialization order contract**.
This branch is stacked on `feature/navi33-scheduler-plugin` and does not
start Navi33 firmware, program its GART, enable MES KIQ, map hardware queues
or advertise acceleration. The existing RX 7800 XT M4/M5/M6 path is unchanged.
The new read-only `navi33-linux-boot-preflight` IORegistry property is
only published after RedNavi's existing Navi33 Discovery/P5/P6 inspection.

## Correct target and why

RedNavi is the primary hardware backend. Its existing M4 RX 7800 XT path
already implements PSP, GART, MES KIQ, Scheduler, compute queue, graphics
queue, RADV and display integration. Cobalt64 is **evidence/reference only**:
its Navi33 PSP/autostart/compute work ran on physical hardware, but its
G40 Graphics Queue and G42 direct-MEC mapping stalled. Do **not** fork the
Cobalt64 kext into RedNavi or attach its G34/G42 queue to the live MES
scheduler.

Linux baseline: [torvalds/linux @ 7475ea8b](https://github.com/torvalds/linux/tree/7475ea8b58616b33c04afb976cf0fbaad9dd63fb).
Target `1002:7480`, gfx1102 / GC 11.0.2, PSP/SMU 13.0.7,
SDMA 6.0.2, DCN 3.2.1. This is the **PSP-loaded, MES-KIQ enabled,
async-GFX** Linux branch. Do not treat the plan as universal for all
Linux kernel versions, firmware versions or runtime module parameters.

## Actual Linux call path

1. `amdgpu_device_ip_early_init()` calls IP Discovery and early_init
   functions. `amdgpu_device_ip_init()` invokes the software init
   callbacks and early COMMON and GMC hardware init; GMC is needed for
   GPU BO allocations and writeback storage.
2. `amdgpu_device_ip_hw_init_phase1()` initializes COMMON/IH
   where applicable, `amdgpu_device_fw_loading()` handles the
   PSP firmware path, then `amdgpu_device_ip_hw_init_phase2()`
   runs remaining hardware init callbacks.
3. `gfx_v11_0_hw_init()` waits for RLC autoload, enables
   GFXHUB, initializes golden registers and constants, NBIO GC
   doorbells, and resumes RLC and CP (plus branch-specific steps).
4. `gfx_v11_0_cp_resume()` programs CP doorbell ranges and,
   on the enabled MES-KIQ path, calls `amdgpu_mes_kiq_hw_init()`.
5. `mes_v11_0_kiq_hw_init()` starts/checks MES
   scheduler+KIQ firmware, configures KIQ as ME3/Pipe1, initializes
   its ring and MQD. For scheduler firmware >= 0x47 it calls
   `mes_v11_0_hw_init()`.
6. `mes_v11_0_hw_init()` initializes the Scheduler
   ME3/Pipe0, maps its MQD via the MES KIQ, submits
   `SET_HW_RESOURCES`, optionally
   `SET_HW_RESOURCES_1` for firmware >= 0x52,
   runs `QUERY_SCHEDULER_STATUS` and hands ordinary
   queue ownership over to MES. The KIQ has special remaining
   internal use in some upstream code; do not emulate arbitrary
   external KIQ submissions.
7. `gfx_v11_0_kcq_resume()` constructs KCQ MQDs and
   `amdgpu_gfx_mes_enable_kcq()` performs its
   KIQ SET_RESOURCES step and maps compute queues through MES.
8. `gfx_v11_0_cp_async_gfx_ring_resume()` constructs
   graphics MQD(s), calls `amdgpu_gfx_enable_kgq()`,
   which uses `amdgpu_mes_map_legacy_queue()` /
   MES ADD_QUEUE on the >= 0x47 path, then starts the GFX CP
   and runs kernel ring tests.

**GFX boot is nested:** Linux does not complete the whole MES lifecycle
as a separate top-level IP block before GFX. It does so *inside*
`gfx_v11_0_cp_resume()`. A successful
`CP_MES_CNTL` start alone does not validate queue
ownership or Scheduler command processing.

## RedNavi component mapping

| Linux component | Existing RedNavi | Navi33 gap |
|---|---|---|
| IP discovery | `navi33_runtime.h`, `kern_probe.cpp` P4/P5 | Real silicon confirmation and recovery |
| COMMON/PSP/SMU | `kern_probe.cpp` Navi32 PSP loader, Navi33 P1/P6 planners | Dedicated Navi33 firmware loader + independent ownership |
| GMC/GFXHUB | `gfx11.cpp` and `navi33_vm_plan.h` | GC11.0.2-specific offset map and owned GPU BOs |
| MES KIQ/Scheduler | `gfx11.cpp::rn_mes_start`, `navi33_mes_plan.h` | Parameterize for gfx1102; verify HQD and two real fences |
| KCQ/KGQ | RedNavi M4 `rn_mes_start` and GFX queue / M5 RADV | Exact KIQ SET_RESOURCES then MES ADD_QUEUE, no G34/G42 |
| User queues | `rn_uapi.h` and Mesa bridge | Query real gfx1102 properties; capability bits require executed proof |
| Rendering | RADV test programs + Metal M6 | First real Navi33 GFX fence and offscreen pixel readback |

Keep the Navi32 constant `RN_VRAM_MC_BASE` out of the new
Navi33 path. Use validated per-device GFXHUB/MMHUB bases and owned VRAM
allocations. The existing 7800 XT display/Metal path is not a shortcut to
Navi33 display or performance claims.

## Added implementation

- `RedNavi/navi33_linux_boot_parity.h`: guarded ordered
  call-path model for the GC11.0.2 PSP/MES-KIQ/async-GFX branch. The plan
  refuses unknown MES firmware version, non-0x7480/GC11.0.2 identity,
  incompatible driver settings and G34/G42 conflicts. Firmware
  version >= 0x52 adds SET_HW_RESOURCES_1.
- `RedNavi/kern_probe.cpp`: an additional **read-only**
  result after existing P6.1 PSP mailbox inspection, exposing blocker
  and observation mask through IORegistry. Even an apparently ready
  PSP/GC snapshot returns NoVerifiedGpuOwner and cannot trigger
  hardware initialization.
- `tests/host/navi33_linux_boot_parity_test.cpp`:
  stage-order, refusal, firmware branching and read-only preflight
  regression cases; built in `make test`.

This is a first integration stage, **not** a fully ported Linux
hardware bootstrap. A host test only validates decisions and metadata,
not that physical GPU queues work.

## Next implementation in this branch family

1. Introduce a separate Navi33 device owner and bounded GPU BO lease
   compatible with the existing RedNavi M4 backend; preserve the sole
   display's firmware console and independent recovery.
2. Replace hardcoded Navi32 IP register assumptions with GC11.0.2
   discovery-derived registers and validate GPUVM translations.
3. Wire PSP13.0.7 autoload/SMU into the new state machine with
   independent live-fence proofs. Do not copy signed firmware into Git.
4. Parameterize the native M4 MES bootstrap for GC11.0.2, including
   scheduler/kiq MQDs, separate ring buffers, NOP alignment, HDP,
   doorbell 0x58/0x60, and scheduler log buffer size.
5. Accept a stage only with silicon evidence: KIQ fence, Scheduler API
   and sequence fences, KGQ mapped/active, RPTR/WPTR movement and
   actual GPU output. Record the first discrepancy relative to Linux.
6. Only after real hardware verification: extend gfx1102 RADV user-space
   metadata and Metal (the latter is explicitly not ready today).

## Safety and licensing

The new code performs no GPU writes and is BSD-3-Clause as the RedNavi
repository requires. Linux (GPL) is used as an upstream behaviour
reference; no Linux driver bodies or noncommercial driver code are copied.
A new hardware-write stage on the sole display GPU requires a
dedicated bounded allowlist, full power-off/recovery plan, verified
owner/retention and explicit approval. Never write VBIOS.

Sources:
- [amdgpu_device.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c)
- [amdgpu_discovery.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c)
- [gfx_v11_0.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c)
- [mes_v11_0.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/mes_v11_0.c)
- [amdgpu_gfx.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c)
