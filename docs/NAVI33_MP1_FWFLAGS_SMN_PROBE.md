# Navi33 MP1 firmware-flags SMN probe (P7.26, experimental 0.0.42)

0.0.41 is the RX7600 *hardware-proven admission-only baseline*: it
published `index_written=0`, `gate_error=2`, `snapshot_valid=0`,
and maintained PSP Fence 23/23 with the RLC engine disabled.

0.0.42 adds an independent, **mandatory NBIO IP revision gate**
to the opt-in SMN path. It preserves the default-off behavior; it
does **not** turn on MMIO index writes automatically.

This child branch builds on experimental P4 0.0.40 (real RX7600
PSP Fence23/23 and GC/MP1 BAR5 read-only snapshot). The known-good
0.0.40 USB must be preserved; DO NOT automatically replace a rescue EFI.

## Why this new path is needed

Linux Navi33 SMU13.0.7-specific \`smu_v13_0_7_check_fw_status()\` reads the
**SMN** (System Management Network) register
\`MP1_Public | smnMP1_FIRMWARE_FLAGS = 0x03B10028\`, and checks
\`MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK = 0x00000001\`.

This is **NOT** the MP1 C2PMSG mailbox BAR5 address and **not**
a direct \`MP1 IP base + 0xBEE009\` BAR5 offset.
Linux uses `RREG32_PCIE` for it. Generic SMU13 reads
`0x03B10024`, but Navi33-specific SMU 13.0.7 reads
`0x03B10028`. The legacy RedNavi `smuProbe()` uses the
NBIF segment 1 index/data aperture for the **generic**
`0x03B10024` and is not a verified Navi33-specific probe.
The dedicated Step 27 path selects `0x03B10028`.

**Important:** Although the firmware flags themselves are read,
accessing SMN through this pair *writes to the indirect index register*.
This is a potential shared-device side effect. Neither simple
\`ioreg\` reads nor the no-opt-in boot path do that write. The
manual opt-in is explicitly required and no guarantee of conflict-free
ownership is implied.

## Step27 opt-in behavior

New boot argument, **default OFF**:
\`rednavi_navi33_p726_mp1_fwflags=1\`

### 0.0.42: NBIO version gate

Linux `amdgpu_discovery.c` maps `NBIO_HWIP` to
`NBIF_HWID` (108) in IP Discovery, and selects
`nbio_v7_11_funcs` for **NBIO 7.11.0–7.11.3**.
The Linux `nbio_v7_11.c` RSMU PF1 index/data register names are:
`regBIF_BX_PF1_RSMU_INDEX` and
`regBIF_BX_PF1_RSMU_DATA`.
In `nbio_7_11_0_offset.h`, both use **segment base index 1**
with register DWORD offsets `0` and `1`.

0.0.42 extracts the actual NBIF version from discovery instead
of inferring v7.11 from PCI device ID or from the observed segment1
base `20` (BAR5 `0x50/0x54`). The older synthetic
`navi33_bootstrap_test.cpp` fixture hardcodes `4.0.0` and
`0x14`; that **is not independent confirmation** of live 0.0.41
hardware's NBIO version. We must inspect 0.0.42 telemetry.

- Supported PF1 RSMU layout: NBIO `7.11.0` through `7.11.3`.
- All other versions, including `4.0.0`, `7.7.x`,
  `7.11.4+`, and unknown `0.0.0`, are fail-closed.
- This protection applies even with the manual boot opt-in.
- `navi33-nbio-major/minor/revision` and
  `navi33-p7-mp1-fwflags.nbio_major/minor/revision`
  are **passive** IP Discovery observations; a default-off boot
  can collect them without any new SMN index write.

Preconditions enforced in `navi33_mp1_fwflags_smn_probe.h`:
- \`rednavi_navi33_p7_step=27\` exactly;
- exact GC11.0.2 / Navi33 and MP1 revision 7;
- PMFW accepted and PSP Fence23/23;
- healthy PSP submit transport;
- NBIO 7.11.0–7.11.3 observed in NBIF IP Discovery;
- NBIF IP discovery segment 1 present;
- both NBIF index/data byte offsets within the BAR5 mapped aperture.

If no opt-in, the policy returns \`gate_error=2\`, does **not**
touch NBIF index/data, and publishes its admission report.

If permitted:
1. Read the old NBIF index once, refuse the operation if unreadable
   (\`0xffffffff\`), so we do not lose its value.
2. Write \`0x03B10028\` to the index **once**.
3. Read NBIF data **once**.
4. Restore the old index **once**, read back and verify restoration.
5. Publish raw bits. Interpret bit0 only if read/restore succeeded.

No SMU command, \`GetSmuVersion\`, \`AUTOLOAD_RLC\`, MP1 reset,
PSP firmware load beyond prior baseline, or graphics queue setup is
introduced. No new hardware-ready state is inferred: existing
\`smu_handshake_verified=0\` and \`may_autoload=0\` remain unchanged.

**Safety:** This is a manual opt-in experiment. Index register
restore does not establish exclusive SMN ownership against other
agents. Avoid performing it on the sole available display/boot
route without independent recovery EFI.

## IORegistry key

\`navi33-p7-mp1-fwflags\`:

- \`step\`, \`requested\`, \`explicit_opt_in\`, \`gate_error\`, \`may_probe\`
- \`nbif_found\`, \`nbif_base_dword\`, \`nbio_major\`, \`nbio_minor\`,
  \`nbio_revision\`, \`nbio_pf1_layout_supported\`, \`bar5_bytes\`
- \`smn_address\` (SMN byte address \`0x03B10028\`)
- \`index_byte_offset\`, \`data_byte_offset\`: MMIO BAR5 byte offsets
- \`index_before\`, \`index_written\`, \`index_after\`, \`index_restored\`
- \`fw_flags\`, \`snapshot_valid\`, \`interrupts_enabled\`
- \`smu_handshake_verified=0\`, \`smu_command_sent=0\`,
  \`rlc_autoload_submitted=0\`.

If there was no opt-in, \`fw_flags=4294967295\`,
\`snapshot_valid=0\`, \`interrupts_enabled=0\` **mean not sampled**,
not that firmware interrupts are actually disabled.

## Test / build (from Mac; no new USB writes automatically)

\`\`\`sh
cd ~/RedNavi
git fetch origin
git switch --track origin/feature/navi33-mp1-fwflags-smn-probe
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
\`\`\`

Check resulting \`build/Release/RedNaviNavi33P4.kext\` version 0.0.42.
Do **not** use \`NAVI33_RLC_AUTOLOAD_UNSAFE=1\`.
Build success is not real-hardware test success.

For a **separate experimental USB** boot with the existing opt-ins
including Step27, do not append the new flag until the no-opt-in
admission report has been reviewed.

\`\`\`sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe |
  grep -E '"navi33-p7-(mp1-fwflags|rlc-preflight|rlc-autoload-runtime)"'
\`\`\`

Expected no-opt-in: `step=27`, `explicit_opt_in=0`,
`gate_error=2`, `index_written=0`, `snapshot_valid=0`,
plus the **live** `nbio_major/minor/revision` and
`nbio_pf1_layout_supported` values. Report those before any
opt-in. If the layout is unsupported, `gate_error=6` on a hypothetical
opt-in and index writes remain blocked.
Expected opt-in only after a separate boot: \`index_written=1\`,
\`index_restored=1\`, \`snapshot_valid=1\`; \`fw_flags\` varies by
hardware. On any unexpected or all-ones read, stop and preserve logs.

## Why this is NOT SMU Ready

The Linux firmware-flags interrupt-enabled bit, PSP PMFW acceptance
and old C2PMSG_90 response=1 are all distinct signals. No fresh
owned SMU GetSmuVersion request/response has been performed.
Even if bit0=1, \`post_psp_error=13\` may legitimately persist and
RLC autoload must remain blocked.

The following P7.27 step still needs mailbox-exclusive ownership and
a one-shot SMU version transaction, plus separately corrected Linux
firmware loading order, before attempting any engine-start command.
