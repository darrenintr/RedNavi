# Navi33 P7.26 — NBIO 4.3.1 PF0 RSMU, experimental 0.0.43

## Actual RX7600 evidence

The 2026-11-12 macOS real-hardware run of Kext **0.0.42** showed:

- \`navi33-nbio-major=4\`, \`navi33-nbio-minor=3\`, \`navi33-nbio-revision=1\`.
- \`navi33-p7-mp1-fwflags.nbio_pf1_layout_supported=0\` (correct:
  NBIO 4.3.1 is PF0, **not** NBIO 7.11 PF1).
- NBIF segment 1 base DWORD 20 (\`0x14\`).
- \`index_written=0\`, \`gate_error=2\`, \`snapshot_valid=0\`:
  no new indirect SMN I/O; PSP Fence 23/23 remained accepted.
- \`smu_handshake_verified=0\`, \`may_autoload=0\`.

Version 0.0.43 adds a Linux-matched **PF0** read policy and
a second opt-in, without changing the normal boot's hardware I/O.

## Exact Linux map, not inferred from the PCI ID

Reference: Linux \`torvalds/linux\` commit
\`7475ea8b58616b33c04afb976cf0fbaad9dd63fb\`.

- \`drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c\` selects
  \`nbio_v4_3_funcs\` for NBIO \`4.3.0\` or \`4.3.1\`.
- \`drivers/gpu/drm/amd/amdgpu/nbio_v4_3.c\`
  \`nbio_v4_3_get_pcie_index_offset()\` uses
  \`regBIF_BX_PF0_RSMU_INDEX\` and its data counterpart
  uses \`regBIF_BX_PF0_RSMU_DATA\`.
- \`drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_4_3_0_offset.h\`:
  \`regBIF_BX_PF0_RSMU_INDEX = 0\`,
  \`regBIF_BX_PF0_RSMU_INDEX_BASE_IDX = 1\`,
  \`regBIF_BX_PF0_RSMU_DATA = 1\`,
  \`regBIF_BX_PF0_RSMU_DATA_BASE_IDX = 1\`;
  \`regBIF_BX_PF0_RSMU_INDEX_HI = 2\`, same segment.
- With the physical NBIF segment1 base \`0x14\` DWORD:
  index BAR5 byte \`0x50\`, data \`0x54\`,
  index-hi \`0x58\`. The **32-bit** SMN read below never writes
  or relies on the INDEX_HI register.
- \`drivers/gpu/drm/amd/amdgpu/amdgpu_reg_access.c\`:
  \`amdgpu_device_indirect_rreg()\` uses a driver-owned PCIE spinlock,
  writes the index, **reads it back to flush**, then reads data.
  The macOS experimental probe now also requires an exact index
  readback before touching data, and finally restores and checks
  the previous index. Restoration does **not** prove exclusive
  ownership of an index register potentially shared with other agents.
- \`drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0_7_ppt.c\`:
  \`smu_v13_0_7_check_fw_status()\` reads SMN \`0x03B10028\`
  through \`RREG32_PCIE\` and checks bit0
  (\`INTERRUPTS_ENABLED\`). This is **NOT** the generic
  SMU13 \`0x03B10024\` value.

## Fail-closed opt-in protocol (0.0.43)

The 0.0.43 branch supports only the verified PF0 access family:
NBIO \`4.3.0\` or \`4.3.1\`. An NBIO \`7.11.x\` value remains
denied by **this Navi33 PF0 code path**, even though the earlier
0.0.42 branch tested PF1 mapping.

**Default OFF**. No new index writes are performed with no new
boot arguments, and prior \`rednavi_navi33_p726_mp1_fwflags=1\`
alone is now insufficient to trigger a read. Active sampling requires
*both* flags, in addition to the Step27, ASIC, PMFW, PSP fence,
transport, BAR5/segment1, and NBIO gating:

\`\`\`text
rednavi_navi33_p726_mp1_fwflags=1
rednavi_navi33_p726_nbio43_pf0=1
\`\`\`

**Do not add either flag to the working EFI now.** Only attempt
active sampling on an independently recoverable experimental boot
route after reviewing the 0.0.43 default-off telemetry.

When admitted:
1. Save the original RSMU index. If unreadable (all ones), refuse.
2. Write 32-bit SMN index \`0x03B10028\` once.
3. Read back the index; **abort data read if it differs**.
4. If selection matches, read the RSMU data once.
5. Restore previous index and read it back, even if step 3 failed.
6. Accept a snapshot only if selected readback, valid data, and
   restored index all match the pure-host \`Transcript\` policy.

This does not send a SMU command, declare a live GetSmuVersion
handshake, start RLC autoload, or enable a graphics queue.

## IORegistry

\`navi33-p7-mp1-fwflags\` now reports:
\`pf0_explicit_opt_in\`,
\`nbio_pf0_layout_supported\`,
\`nbio_pf1_layout_supported\` (legacy informational field),
\`index_selected_readback\`,
\`index_selection_verified\`, \`data_read\`,
\`restore_written\` and original 0.0.42 fields
(\`gate_error\`, \`may_probe\`, \`index_written\`,
\`index_before\`, \`index_after\`, \`index_restored\`,
\`fw_flags\`, \`snapshot_valid\`, \`interrupts_enabled\`).

For the first **default-off** 0.0.43 boot, expected:
- Kext \`0.0.43\`, \`nbio_major=4\`, \`nbio_minor=3\`,
  \`nbio_revision=1\`.
- \`nbio_pf0_layout_supported=1\`,
  \`nbio_pf1_layout_supported=0\`.
- \`explicit_opt_in=0\`, \`pf0_explicit_opt_in=0\`,
  \`gate_error=2\`, \`may_probe=0\`, \`index_written=0\`,
  \`data_read=0\`, \`snapshot_valid=0\`,
  \`smu_command_sent=0\`, \`rlc_autoload_submitted=0\`.

The value \`fw_flags=0xFFFFFFFF\` with
\`snapshot_valid=0\` is a *not-sampled sentinel*, not evidence
that hardware firmware is broken.

## Build / first test

\`\`\`sh
cd ~/RedNavi
git fetch origin
git switch --track origin/feature/navi33-nbio43-pf0-fwflags
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' \
  build/Release/RedNaviNavi33P4.kext/Contents/Info.plist
\`\`\`

Output should be \`0.0.43\`. Preserve the known-good 0.0.42
and another independently bootable recovery EFI; never
auto-inject or enable unsafe P7.25 RLC.
After an **admission-only** experiment boot, collect:

\`\`\`sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe |
  grep -E '"navi33-(nbio-|p7-mp1-fwflags|p7-rlc-preflight)'
\`\`\`

Full host tests plus real hardware test are distinct.
No 0.0.43 real-hardware firmware-flags measurement is claimed.

## Deferred prerequisites

Even if \`MP1_FIRMWARE_FLAGS.INTERRUPTS_ENABLED\` is 1,
the driver still needs an independently verified owned SMU mailbox
GetSmuVersion transcript, Linux-consistent firmware PSP load ordering,
and a valid RLC / graphics queue start. Do not promote
\`smu_handshake_verified\` or \`may_autoload\` from this snapshot.
