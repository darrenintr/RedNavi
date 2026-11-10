# Navi33: RedNavi Linux `amdgpu` boot-parity path

Status: **read-only Navi33 runtime preflight + host-only initialization order contract**.
The original boot-parity work followed `feature/navi33-scheduler-plugin`.
The current `feature/navi33-init-resources` work builds on
`feature/navi33-m4-gc1102` and still does not start Navi33 firmware,
program its GART, enable MES KIQ, map hardware queues or advertise
acceleration. The shared native path used by RX 7800 XT now has explicit
MES workspace addressing and DMA-page validation; see
[Initialization resources](INIT_RESOURCES.md) for that separate runtime
refactor and its verification limits.
The read-only `navi33-linux-boot-preflight` IORegistry property is
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
   callbacks. COMMON software state precedes COMMON hardware init.
   GMC software init creates the memory manager and GART table; its
   early hardware init enables **MMHUB**, then initializes writeback
   storage. This is not the later GFXHUB enable operation.
2. `amdgpu_device_ip_hw_init_phase1()` initializes COMMON/IH
   where applicable, but only after all IP software init has completed,
   including MES ring/MQD resources, the IB pool and firmware BO.
   `amdgpu_device_fw_loading()` handles the PSP firmware path described
   below, then `amdgpu_device_ip_hw_init_phase2()` runs remaining
   hardware init callbacks. `SmuHw` means SMU hardware/power-management
   setup, not the earlier transfer of SMU firmware through PSP.
3. `gfx_v11_0_hw_init()` waits for RLC autoload, enables
   GFXHUB, initializes golden registers and constants, NBIO GC
   doorbells, and resumes RLC and CP (plus branch-specific steps).
4. `gfx_v11_0_cp_resume()` programs CP doorbell ranges and,
   on this async-GFX branch, enables compute/GFX CP engines before
   calling `amdgpu_mes_kiq_hw_init()`.
5. `mes_v11_0_kiq_hw_init()` starts/checks MES
   scheduler+KIQ firmware, configures KIQ as ME3/Pipe1, initializes
   its ring and MQD. For scheduler firmware >= 0x47 it calls
   `mes_v11_0_hw_init()`.
6. `mes_v11_0_hw_init()` initializes the Scheduler
   ME3/Pipe0, maps its MQD via the MES KIQ, submits
   `SET_HW_RESOURCES`, optionally
   `SET_HW_RESOURCES_1` for firmware >= 0x52,
   runs `QUERY_SCHEDULER_STATUS` and makes the scheduler ready for
   kernel queue mapping. This readiness is not an irreversible
   prohibition of all later initialization-time KIQ operations.
7. `gfx_v11_0_kcq_resume()` constructs KCQ MQDs and
   `amdgpu_gfx_mes_enable_kcq()` performs its
   KIQ SET_RESOURCES step and maps compute queues through MES.
8. `gfx_v11_0_cp_async_gfx_ring_resume()` constructs
   graphics MQD(s), calls `amdgpu_gfx_enable_kgq()`,
   which uses `amdgpu_mes_map_legacy_queue()` /
   MES ADD_QUEUE on the >= 0x47 path, then starts the GFX CP
   and runs kernel ring tests. These tests write `SCRATCH_REG0`;
   they are named `GfxScratchTest` and `ComputeScratchTest` in the
   contract, not GPU-memory fence tests.
9. The main IP hardware-init loop later reaches MES again, after the
   intervening hardware callbacks. Because the scheduler is already
   ready, `mes_v11_0_hw_init()` takes its `out` path and sets KIQ
   `sched.ready` to false again. This is `MesHwFinalize`.

On GC11 the `KcqKiqSetResources` stage remains after early scheduler
readiness: `amdgpu_gfx_mes_enable_kcq()` explicitly sends KIQ
`SET_RESOURCES` when `enable_uni_mes` is false. Its ring test can set
KIQ `sched.ready` back to true; the later MES callback clears it again.
`amdgpu_ring_alloc()` does not gate that direct submission on
`sched.ready`. Preserve this source-version-specific exception rather
than deleting it on the strength of an ownership comment.

The stage list models the dependencies needed by this bring-up path.
It does not expand every display, SDMA, multimedia, optional firmware
feature or late-init callback. Completing this reported plan therefore
does not certify full Linux driver initialization or acceleration.

### PSP13.0.7: allocation and installation are separate

For this ASIC, `psp_check_pmfw_centralized_cstate_management()` selects
the PMFW-before-TMR-install branch. The expanded milestones are:

| Stage | Linux operation and dependency |
|---|---|
| `PspRingReady` | Complete the necessary bootloader/SOS startup and create the PSP command ring; an SOS sign-of-life observation alone is insufficient. |
| `PspTmrAllocated` | `psp_tmr_init()` allocates/pins the TMR BO. This allocation occurs before SMU firmware loading. |
| `PspSmuFirmware` | `psp_load_smu_fw()` transfers SMU firmware using the working PSP transport. |
| `PspTmrInstalled` | `psp_tmr_load()` installs the TMR after PMFW has loaded. This is the ordering rule specific to this PSP branch. |
| `PspGraphicsFirmware` | Load the applicable CP, MES, IMU and RLC records, including firmware-header-dependent sections, through `RLC_G`. |
| `PspRlcAutoloadStart` | Issue the RLC autoload start after PSP has received the graphics firmware records. |
| `PspFwComplete` | Complete the remaining PSP firmware-init work before moving to later IP hardware init. Autoload completion is checked separately by `GfxRlcAutoloadWait`. |

PSP13.0.10 is not selected by the same centralized-Cstate branch.
The planner therefore checks both discovered PSP and SMU versions
against **13.0.7**, instead of inferring them from a GC11.0.2 match.
The existing offline transfer planner's SMU-first record order does
not by itself represent PSP transport readiness or TMR installation.

### Ring progress is different from a GPU writeback fence

`gfx_v11_0_ring_test_ring()` checks a scratch register transition from
`0xCAFEDEAD` to `0xDEADBEEF`. This proves that the selected ring processed
the test command. It does not exercise an IB writing to an owned memory
buffer, the interrupt-driven completion path, or a `dma_fence` wait.
Those require separate IB/writeback/fence validation, such as the later
`amdgpu_ib_ring_tests()` path. The boot planner never upgrades reported
scratch success to any of these proofs.

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
  non-13.0.7 PSP/SMU, incompatible driver settings and G34/G42 conflicts.
  Firmware version >= 0x52 adds SET_HW_RESOURCES_1. The plan keeps the
  input settings and validates its complete canonical sequence; empty,
  truncated, reordered or version-inconsistent plans are invalid even
  if a caller sets `error = Ok`. Complete progress returns
  `nextIndex == plan.count`, `nextStep == None` and `done == true`;
  it does not restart at Discovery or authorize hardware access.
- `RedNavi/kern_probe.cpp`: an additional **read-only**
  result after existing P6.1 PSP mailbox inspection, exposing blocker
  and observation mask through IORegistry. Even an apparently ready
  PSP/GC snapshot returns NoVerifiedGpuOwner and cannot trigger
  hardware initialization.
- `tests/host/navi33_linux_boot_parity_test.cpp`:
  software/hardware dependency ordering, PSP SMU/TMR order, nested MES
  and KIQ exception, malformed-plan rejection, terminal progress,
  firmware branching and read-only preflight regression cases;
  built in `make test`.

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

The boot-parity model and Navi33 preflight perform no GPU writes and are
BSD-3-Clause as the RedNavi repository requires. Linux (GPL) is used as an upstream behaviour
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
- [amdgpu_psp.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c)
- [gmc_v11_0.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c)
- [amdgpu_ring.c](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c)
