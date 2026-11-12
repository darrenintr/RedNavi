# Navi33 SMU 13.0.7: safe GetSmuVersion admission policy

This follow-up is stacked on the experimentally verified
\`feature/navi33-rlc-readonly-preflight\` branch (RedNavi P4 0.0.40).
**Policy + host tests only.** It deliberately does not issue an SMU
mailbox command, call \`rn_smu13_send\`, change OpenCore boot args,
activate RLC Autoload, or adjust the known-working PSP firmware sequence.

## Hardware snapshot (2026-11-12, RX 7600)

- PSP accepted PMFW; PSP Fence 2/2, overall Fence 23/23.
- MP1 response register readback = \`1\`, before/after PMFW load as well
  as in the read-only P7.25 snapshot.
- BAR5 = 1048576 bytes. Discovered MP1 base DWORD = 90112 (\`0x16000\`).
  Existing response register byte offset = 363112 (\`0x58a68\`).
- GC0 CP_STAT=0, GC1 RLC bootload status=0, IMU scratch=0.
- \`smu_handshake_verified=0\`, \`firmware_order_matches_linux=0\`,
  \`may_autoload=0\`. RLC engine/graphics queues not started.

This is insufficient to infer a **live SMU request/response transaction**.

## Linux reference

For Navi33 SMU 13.0.7:
- \`drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c\`
  defines MP1 C2PMSG_66 DWORD \`0x282\` (message),
  C2PMSG_82 DWORD \`0x292\` (parameter/output),
  C2PMSG_90 DWORD \`0x29a\` (response).
- \`drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu_v13_0_7_ppsmc.h\`:
  \`PPSMC_MSG_GetSmuVersion=0x02\`,
  \`PPSMC_Result_OK=0x01\`, busy \`0xFC\`.
- \`smu_cmn_get_smc_version()\` calls SMU_MSG_GetSmuVersion to retrieve
  the version from a **fresh** command/response, not a stale status read.

Given currently discovered MP1 base 0x16000, the *theoretical* BAR5 byte
addresses are:
- C2PMSG_66: \`(0x16000+0x282)*4 = 363016\`;
- C2PMSG_82: \`(0x16000+0x292)*4 = 363080\`;
- C2PMSG_90: \`(0x16000+0x29a)*4 = 363112\` (read-only confirmed).

## What is implemented

\`RedNavi/navi33_smu_mailbox_admission.h\` is pure C++17 policy:
- Refuses a request without explicit manual opt-in AND a separately
  enabled unsafe build.
- Requires exact Navi33 MP1 revision 13.0.7, PSP/PMFW accepted, retained
  healthy PSP transport and expected Fence23/23.
- Requires a **genuine exclusive MP1 mailbox lease** proven independently
  by the driver, not simply by an opt-in or an idle response value.
- Requires verified address bounds and an idle mailbox response code.
- Defines response transcript validation: owned operation, observed
  clear-to-zero, GetSmuVersion command submitted, observed response
  transition to PPSMC_OK, nonzero non-all-ones version output, bounded
  completed polls, no timeout.
- Test vectors reject stale \`PPSMC_OK\`, wrong command, timeouts, all-ones
  response, arbitrary version and absent ownership.

No actual mailbox ownership logic or MMIO sending path is implemented:
**\`mailboxLeaseHeld\` cannot currently be asserted on Navi33.**
Existing \`post_psp_error=13\` intentionally remains. Passing the
synthetic host test is not a GPU firmware readiness proof.

## Before the first hardware mailbox message

1. Implement and independently audit true exclusive MP1 mailbox ownership
   (including potential firmware/other-driver use of the mailbox).
2. Implement bounded deadline and logging; only a single GetSmuVersion
   request, never retry automatically on busy/timeout.
3. Explicit compile-time experimental gating and step-specific boot opt-in,
   disabled in normal release/USB.
4. Prove correct byte offsets from real IP discovery and BAR5 bounds.
5. Ensure recovery EFI is separate; do not enable on the sole boot/display
   device until safe recovery is available.
6. Independently correct/validate Linux-equivalent PSP firmware submission
   order (MES scheduler/KIQ, IMU, RLC_G) before AUTOLOAD_RLC.
7. Do not let this probe set \`RLC boot complete\`, \`MES running\`,
   \`Graphics Queue running\` or \`Metal available\`.

## Build/test (no device interaction)

\`\`\`sh
git fetch origin
git switch --track origin/feature/navi33-smu-mailbox-admission
make test
\`\`\`

No USB modification is necessary or recommended from this policy-only
branch. It is intended to inform the later implementation.
