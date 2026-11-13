# P7.36 — opt-in, two-read Navi33 MP1 C2PMSG_90 observer

**Experimental Navi33 P4 Kext version: 0.0.52**

P7.36 is the first **optional** post-boot MP1 response register
monitor. It is **off by default** and never issues an SMU command.
It reuses the existing P7.33 IOKit timer; NO new timer or UserClient
is created.

The host code performs exactly one **direct 32-bit BAR5 volatile
read** per accepted timer event, and only from the validated byte
offset of MP1 C2PMSG_90. It does NOT perform an RSMU index write,
MM_INDEX write, VRAM read, mailbox clear or message submission.

The original Navi33 P4 probing may perform other explicitly opted-in
hardware operations. "No new writes" means P7.36's *new sampler
code*, **not** the entire Kext.

## Why the response register, and not all three MP1 fields?

The Linux mainline SMU13 transport defines:
- `mmMP1_SMN_C2PMSG_66 = 0x0282` (message),
- `mmMP1_SMN_C2PMSG_82 = 0x0292` (parameter),
- `mmMP1_SMN_C2PMSG_90 = 0x029a` (response),

and `smu_cmn.c` repeatedly polls `RREG32(cfg->resp_reg)`
in its SMU message V1 response-wait loop. A *live Linux SMU*
transaction also writes response=0, argument, and message. We are
**not** implementing that transaction on RX 7600 in P7.36.

References:
- [Linux `smu_v13_0.c`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/pm/swsmu/smu13/smu_v13_0.c)
  (register config in `smu_v13_0_init_msg_ctl`)
- [Linux `smu_cmn.c`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c)
  (`__smu_msg_v1_poll_stat` and `__smu_msg_v1_send`)

**Warning:** Linux polling provides evidence that the response
register is *normally read*, not a comprehensive guarantee against
silicon/power-state read side effects. No amount of sampled idleness
proves mailbox exclusivity. This remains experimental on the only
macOS display adapter.

## Strictly bounded admission

A new read occurs **only if every precondition is true**:

1. `rednavi_navi33_p736_response_read=1` is explicitly supplied.
   Absent, zero or another value means no new P7.36 MMIO read.
2. Normal Navi33 unsafe-P4 build/admission already allowed the
   RedNavi service to start.
3. The Navi33 IP Discovery image has been validated and reports
   MP1 **13.0.7** (major=13, minor=0, revision=7).
4. BAR5 is directly mapped and the response, message and parameter
   addresses all fit inside the mapped BAR; NO RSMU fallback.
5. At callback time the provider and service are not inactive, the
   BAR map is still present, and the response byte offset remains
   in bounds.
6. **Two reads per loaded Kext image total**, with atomic CAS claim
   *before* any read. The same cap applies across multiple
   `RedNaviProbe` service instances/reprobes. A Kext reload resets
   the software cap; it does not imply a cold hardware reset.

P7.36 does not use or consume P7.31's separate SMU command latch.

### Physical RX 7600 offset validation

From previously collected P7.27 telemetry:

| Field | Value |
| --- | ---: |
| `mp1_base_dword` | 90112 |
| `bar5_bytes` | 1048576 |
| C2PMSG_66 byte offset | 363016 |
| C2PMSG_82 byte offset | 363080 |
| C2PMSG_90 byte offset | 363112 |

Address arithmetic reuses `smu_mailbox_roundtrip::plan()`.
The only live read is `RedNaviProbe::rd(responseByteOffset)`.
No `rn_rreg32()`, `vramRead()` or indexed access is used.

## Execution and IORegistry evidence

The existing P7.33 timer fires approximately 5 and 13 seconds after
`registerService()`. On each firing it still publishes the P7.33
IOService host-open audit. **If P7.36 was armed**, it also publishes:

- `navi33-p7-mp1-response-sample-1`
- `navi33-p7-mp1-response-sample-2`

Each dictionary includes `sample`, `requested`,
`read_performed`, `error`, `response`,
`response_byte_offset`, `previous_value_known`,
`changed_from_previous`, `service_read_count`,
`kext_read_count`, `global_read_slot_consumed`,
`max_kext_image_reads`, `timestamp_ns_lo/hi`,
`register_writes_in_new_path`,
`mailbox_exclusive_proven`, `handshake_verified`,
`may_issue_smu_command`.

Boot-time properties record:

```text
navi33-p7-mp1-response-monitor-requested
navi33-p7-mp1-response-monitor-armed
navi33-p7-mp1-response-monitor-config-error
navi33-p7-mp1-response-monitor-global-cap
```

If opt-in is absent, these indicate disabled but *no new sample
properties* are published. An absent sample in that situation does
not mean that response=0.

Even if `response` changes from 1 to 0, `handshake_verified=0`,
`mailbox_exclusive_proven=0`, `may_issue_smu_command=0`.
No new command or fresh firmware acknowledgment is inferred.

The existing `stop()` code cancels, disables and removes the
P7.33 timer before the BAR5 `regs` map is released; P7.36 then
marks the sampler stopped. It does not create an independent callback
that can outlive the timer. Source tests check this ordering, but do
not constitute a formal proof against all unexpected macOS service
termination races.

## Build verification

```sh
cd ~/RedNavi
git fetch origin
git switch feature/navi33-p736-optin-mp1-response-sampler 2>/dev/null || git switch --track origin/feature/navi33-p736-optin-mp1-response-sampler
git pull --ff-only
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' build/Release/RedNaviNavi33P4.kext/Contents/Info.plist
```

Expected `0.0.52` from the built Kext. This does **not**
automatically update an EFI.

### First boot safety recommendation

Keep the known-working **0.0.51** and an independent recovery EFI.
Only install `0.0.52` on a separate experimental USB/EFI once
CI/local builds pass. For the first boot of 0.0.52, **do not add**
`rednavi_navi33_p736_response_read=1`; verify the new code is
disabled by default, that the desktop remains usable and P7.33
existing telemetry is unchanged.

Only consider the read opt-in on a later test after confirming a
working recovery route and accepting the hardware risk. Do not
combine this with extra experimental SMU/PSP/GC/RLC boot arguments.

After an opted-in boot, once macOS reaches the desktop:

```sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe |
  grep -E '"navi33-p7-mp1-response-|\"navi33-p7-late-provider-audit'
```

Host tests verify the default-off behavior, all admission failures,
mapping size re-checks, one read per sample, both sample transitions,
two-read per-image cap under 32 concurrent simulated services, and
persistent budget after a simulated service reprobe.

P7.27 `p727MailboxLeaseHeld=false`, RLC autoload, MES and Metal
remain **blocked**. This is an observational experiment, not
native accelerated RX 7600 display support.
