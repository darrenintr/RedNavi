# Navi33 RLC / SMU / GC read-only preflight (experimental 0.0.40)

Parent baseline: [feature/navi33-rlc-gc-mes-startup](../RedNaviP4/Info.plist.in)
0.0.39 verified on RX 7600 (2026-11-12): Scheduler Code/Data fences 20/21,
MES KIQ Code/Data fences 22/23; PSP accepted, RPTR reached 368.
**No RLC, MES engine, graphics queue, WindowServer acceleration or Metal was started.**

This child branch adds read-only evidence publication and an extra **fail-closed
policy gate**. It does **not** silently change the PSP loading sequence or
issue any new SMU command. Keep the working 0.0.39 experimental EFI separate.

## Why Step 27 is still blocked

Linux reference pinned to torvalds/linux
\`7475ea8b58616b33c04afb976cf0fbaad9dd63fb\`:
- \`amdgpu_ucode.h\` orders MES Scheduler, MES KIQ, IMU I/D, then RLC_G.
- \`amdgpu_psp.c::psp_load_non_psp_fw\` submits \`AUTOLOAD_RLC\` as soon
  as RLC_G loads (assuming autoload is supported).
- \`gfx_v11_0.c::gfx_v11_0_wait_for_rlc_autoload_complete\` independently
  checks \`CP_STAT==0\` and RLC bootload status bit31.

Current RedNavi PSP fences are **RLC_G=17, IMU I/D=18/19, MES=20..23**,
not Linux-equivalent. All commands being acknowledged by PSP does not
prove their eventual engine-autoload semantics are equivalent.

SMU mailbox readback from the successful 0.0.39 hardware log was
\`smu_response_before=1\`, \`smu_response_after=1\`, and
\`smu_read_ok=1\`. This proves that MP1 reads returned 1, but does NOT
prove that an owned, fresh command/response cycle was completed.
The new policy never treats a stale \`PPSMC_Result_OK\` as SMU Ready.

## New IORegistry property

\`navi33-p7-rlc-preflight\`, emitted from the Step27 code path.

- \`gc0_base_dword\`, \`gc1_base_dword\`, \`mp1_base_dword\`: discovered
  GC/MP1 segment bases, **DWORD units**.
- \`bar5_bytes_lo/hi\`: register mapping length used for bounds checks.
- \`gc_addresses_verified\`: all four register offsets within BAR5.
- \`cp_stat_byte_offset\`, \`rlc_bootload_byte_offset\`,
  \`imu_scratch_byte_offset\`, \`smu_response_byte_offset\`: checked BAR5
  **byte offsets**. Invalid/unchecked => \`4294967295\`.
- \`cp_stat_readback\`, \`rlc_bootload_readback\`,
  \`imu_scratch0_readback\`, \`smu_response_readback\`: raw MMIO values,
  unchanged from the sample.
- \`gc_snapshot_readable\`: checked GC0/GC1 readback was possible, and
  none of the three GC registers returned \`0xffffffff\`. This is NOT
  proof that RLC is running.
- \`pmfw_firmware_accepted\`: PMFW PSP acknowledgement.
- \`smu_mailbox_readable\`: read-only MP1 response sample is accessible.
- \`smu_handshake_verified\`: **0** until a future independently
  controlled SMU command/response protocol validates a fresh reply.
- \`firmware_order_matches_linux\`: **0** with the current RLC_G-first
  fence sequence; requires an independently tested reordered sequence.
- \`blocker\`: policy-specific error enum (see header). 0 only when every
  requirement has been demonstrated, not automatically just because
  a user sets a boot arg.
- \`may_autoload\`: must remain **0** for this 0.0.40 branch.

The new preflight collects GC/MP1 MMIO samples only with \`p7_step>=27\`,
valid, in-BAR5 register offsets and previous MES KIQ Data accepted. It
does not write MMIO. The legacy \`post_psp_error=13\` stays in force until
SMU readiness is independently proved.

The \`navi33-p7-rlc-autoload-runtime\` gate remains
\`gate_error=2\` when Step27 has no opt-in. Even with a future
unsafe build and \`rednavi_navi33_p725_rlc_autoload=1\`, the new
\`ReadOnlyPreflightIncomplete\` guard prevents the PSP autoload command
until all evidence is valid. **Do not enable that flag on this branch.**

## Non-disruptive test

Preserve the 0.0.39 EFI and bootloader as independent recovery media.
To build 0.0.40 locally (Xcode CLT needed):

~~~sh
git fetch origin
git switch --track origin/feature/navi33-rlc-readonly-preflight
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
# Rebuild with all previously generated local firmware headers retained.
make navi33-p4
~~~

No new boot arg is required: keep \`rednavi_navi33_p7_step=27\` with
\`rednavi_navi33_p725_rlc_autoload\` **absent**. After deliberately
installing the separate test Kext (do not overwrite the recovery USB),
check:

~~~sh
kmutil showloaded | grep -i Navi33
ioreg -r -l -w0 -c RedNaviProbe | grep \
  -E '"navi33-p7-(rlc-preflight|rlc-autoload-runtime|postpsp-runtime)"'
~~~

Do not interpret all-ones snapshots as healthy values. If RLC registers
are not accessible pre-boot, that is a diagnostic finding, not a reason
to bypass the safety gate.

## Subsequent engineering tasks (not yet implemented)

1. Audit actual Linux GC11 register base/offset definitions against Navi33
   IP-discovery and validate the read-only BAR5 snapshot on physical RX7600.
2. Independently design and test a Linux-parity firmware submission order
   on a *separate* experimental branch; this changes previously tested
   fence numbers and must not silently replace the known working sequence.
3. Implement a bounded, device-owned SMU command/response probe with
   exact firmware revision, clean reply handshake, timeouts and no
   other owner; do not infer Ready from \`smu_response=1\` alone.
4. Only when 1-3 pass should we consider a dedicated RLC hardware-start
   build. PSP fence 24 by itself is not RLC boot success. Test the
   bootload-complete bit and CP_STAT before any GFXHUB/CP/MES queue writes.
