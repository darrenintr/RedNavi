# Navi33 P7.37 — passive MP1 triplet / transaction evidence (0.0.53)

P7.36 real RX 7600: both direct C2PMSG_90 readings were
`0x00000001`, about 8 seconds apart. P7.36 `read_performed=1`
for both, image budget reached 2/2, and `handshake_verified=0`.
Apple `AMDSupport` and `.Display_boot` were attached by the second
IOService snapshot but neither appeared as a PCI provider-open client.
**These observations do not constitute any new owned SMU transaction.**

## Goal

Gather two matching *passive* snapshots of:

- **MP1 C2PMSG_90** — response, already read by P7.36.
- **MP1 C2PMSG_66** — message, a *new optional* P7.37 direct load.
- **MP1 C2PMSG_82** — parameter, a *new optional* P7.37 direct load.

This is deliberately **not** an atomic snapshot. The response is read
before message and parameter; other actors may modify the registers
between CPU loads. A mismatch/change of values cannot identify an
Apple Kext, other host software or PSP/SMU firmware as writer.

In current Linux mainline
[`smu_cmn.c`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/pm/swsmu/smu_cmn.c),
the V1 SMU code polls Response via RREG32, reads output-argument
registers after a successful transaction, and reads Message/Parameter
in error diagnostics. On sending commands, Linux writes Response=0,
Parameter and Message in that order. The transport is an
architecture reference, **not a guarantee** against special
read-side effects in every macOS power state.

## Two independent consent gates

P7.37 has two explicit runtime requirements:

```text
rednavi_navi33_p736_response_read=1
rednavi_navi33_p737_triplet_read=1
```

**Default without both:** P7.37 adds zero GPU MMIO reads.
`requested`, `armed` and `config-status` are published.
P7.37 cannot be armed if P7.36's response sampler is not armed.

Both models require verified Navi33 MP1 **13.0.7** and direct BAR5
offset validation. On the physical RX 7600:

| Register | BAR5 byte offset | New in P7.37? |
| --- | ---: | --- |
| Message C2PMSG_66 | 363016 | Yes |
| Parameter C2PMSG_82 | 363080 | Yes |
| Response C2PMSG_90 | 363112 | No, reused from P7.36 |
| BAR5 window | 1048576 bytes | Existing mapping |

### Budgets / lifetime

P7.36 still consumes a maximum of **2** direct response dword reads
per loaded Kext image. P7.37 separately consumes a maximum of **4**
additional direct dword reads (two message/parameter pairs), using a
single atomic CAS claim **before** either extra read. No response read
for the current timer tick means **zero** new P7.37 reads.

The exact maximum when both are enabled is therefore **6 host
MMIO reads per Kext image**, of which P7.37 adds at most four.
This does not count other older P4/P7 diagnostic accesses.

P7.37 uses the existing P7.33 timer at approximately 5 and 13 seconds
after `registerService()`; no new timer, workloop, IOMemoryMap,
DMA allocation, SMU message, PSP command, MM_INDEX write or
RSMU INDEX write is introduced by P7.37.

`stop()` removes the P7.33 event source before releasing BAR5 and
then disarms P7.36 and P7.37. Budgets remain consumed through any
service reprobe in one loaded Kext image; a full Kext reload resets
these software counters.

## New IORegistry diagnostics

```text
navi33-p7-mp1-triplet-requested
navi33-p7-mp1-triplet-armed
navi33-p7-mp1-triplet-config-status
navi33-p7-mp1-triplet-extra-read-cap
navi33-p7-mp1-triplet-sample-1
navi33-p7-mp1-triplet-sample-2
```

Each sample reports message/parameter/response, byte offsets,
same-tick pairing, source-evidence class, whether values changed
between the two observations, budget consumption and timestamp.

Evidence enum:
- **0**: unobserved or refused.
- **1**: first passive triplet, no earlier comparison.
- **2**: stable across two accepted triplets. **Not** proof that
  no off-window transaction occurred.
- **3**: register values changed, writer **unattributed**.

The following fields remain **0** for every outcome:

```text
rednavi_command_submitted_proven
external_host_writer_attributed
firmware_response_to_new_command_proven
mailbox_exclusive_proven
may_issue_smu_command
```

P7.27's `p727MailboxLeaseHeld=false` remains hardcoded. A response
of 1, even with changed message/parameter, does not authorize a
GetSmuVersion, PSP submission or RLC/MES/GPU queue start.

### Offline evidence analysis

Save IORegistry text and run this Python parser:

```sh
ioreg -r -l -w0 -c RedNaviProbe > "$HOME/Desktop/RedNavi-P737-ioreg.txt"
python3 "$HOME/RedNavi/tools/navi33_p737_transaction_evidence.py" --input "$HOME/Desktop/RedNavi-P737-ioreg.txt" --output "$HOME/Desktop/RedNavi-P737-evidence.json"
```

The parser also accepts P7.36-only logs, describing the missing
triplet fields as unknown/null. It cannot establish any writer
ownership and it performs **no** hardware access.

## Build and deploy considerations

**First boot:** retain a separate recoverable EFI and a known-good
`0.0.52` Kext backup, install `0.0.53` on the *experimental*
USB only, and boot WITHOUT the new P7.37 argument. Confirm
`requested=No`, `armed=No`, and the earlier P7.36 behavior.
Only consider enabling P7.37 on another controlled boot after
confirming the working fallback; it adds two MMIO register reads
per timer callback on the sole display GPU.

Host tests cover: default-off, missing P7.36 consent, invalid
Navi33/MP1/BAR states, same-tick P7.36 response pairing, sequential
two-read interpretation, no false handshake attribution, existing
response=1, concurrent 32 simulated service callbacks, global
budgets, source no-write audit and stop/unmap order.

## Source-based future audit for Apple AMDSupport

Provider-open state is not a raw BAR owner. P7.37 never assumes Apple
uses C2PMSG. An actual external-writer attribution requires validated
observation of the specific writer/instruction, MAC/kernel component
and target MMIO address; reading different register values is
insufficient. This PR intentionally does **not** attempt invasive
patching of Apple's Kexts, MMIO traps or unproven firmware control.
