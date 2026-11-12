# Navi33 MP1 firmware-flags SMN probe (P7.26, experimental 0.0.41)

This child branch builds on experimental P4 0.0.40 (real RX7600
PSP Fence23/23 and GC/MP1 BAR5 read-only snapshot). The known-good
0.0.40 USB must be preserved; DO NOT automatically replace a rescue EFI.

## Why this new path is needed

Linux \`smu_v13_0_check_fw_status()\` reads the
**SMN** (System Management Network) register
\`MP1_Public | smnMP1_FIRMWARE_FLAGS = 0x03B10028\`, and checks
\`MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK = 0x00000001\`.

This is **NOT** the MP1 C2PMSG mailbox BAR5 address and **not**
a direct \`MP1 IP base + 0xBEE009\` BAR5 offset.
Linux uses \`RREG32_PCIE\` for it. The generic SMU13 register address is `0x03B10024`, but the
SMU 13.0.7 implementation uses `0x03B10028`. Existing RedNavi legacy
\`RedNaviProbe::smuProbe()\` uses the discovered NBIF segment 1
RSMU index/data pair (base + DWORD offsets 0/1) to select/read SMN
\`0x03B10028\`.

**Important:** Although the firmware flags themselves are read,
accessing SMN through this pair *writes to the indirect index register*.
This is a potential shared-device side effect. Neither simple
\`ioreg\` reads nor the no-opt-in boot path do that write. The
manual opt-in is explicitly required and no guarantee of conflict-free
ownership is implied.

## Step27 opt-in behavior

New boot argument, **default OFF**:
\`rednavi_navi33_p726_mp1_fwflags=1\`

Preconditions enforced in \`navi33_mp1_fwflags_smn_probe.h\`:
- \`rednavi_navi33_p7_step=27\` exactly;
- exact GC11.0.2 / Navi33 and MP1 revision 7;
- PMFW accepted and PSP Fence23/23;
- healthy PSP submit transport;
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
- \`nbif_found\`, \`nbif_base_dword\`, \`bar5_bytes\`
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

Check resulting \`build/Release/RedNaviNavi33P4.kext\` version 0.0.41.
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

Expected no-opt-in: \`step=27\`, \`explicit_opt_in=0\`,
\`gate_error=2\`, \`index_written=0\`, \`snapshot_valid=0\`.
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
