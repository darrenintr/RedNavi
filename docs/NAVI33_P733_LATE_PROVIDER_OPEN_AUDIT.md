# Navi33 P7.33 — delayed kernel IOService host-open audit (0.0.51)

## Why this exists

P7.32 `navi33-owner-evidence.txt` from **2026-11-13 09:24:00,
09:24:03 and 09:24:06 +08:00** confirmed the stable GFX0 tree:

```text
GFX0@0 <IOPCIDevice>
  RedNaviProbe
  AMDSupport
  .Display_boot <IONDRVFramebuffer>
    AppleMCCSControlModule
    AppleUpstreamUserClientDriver
    display0 / AppleDisplay
    IOFramebufferUserClient
    IOFramebufferSharedUserClient
```

`org.rednavi.Navi33P4 (0.0.50)` and `com.apple.kext.AMDSupport
(7.0.0)` were loaded. P7.28's early `direct_children_observed=1`
and no siblings were **timing artifacts**: it takes a snapshot inside
`RedNaviProbe::start()`, before matching has settled.

A user-space IORegistry snapshot reports service *attachment* only.
It cannot query the kernel's `IOService::isOpen(forClient)`.

## P7.33 implementation

**New delayed, read-only kernel host-open observation:**

- For the Navi33 path only, schedule an `IOTimerEventSource` after
  `registerService()` returns.
- At approximately **5 seconds** and **13 seconds** afterward, enumerate
  `pci->getChildIterator(gIOServicePlane)`, then check
  `pci->isOpen()`, `pci->isOpen(this)` and
  `pci->isOpen(child)` for the actual `AMDSupport` and
  `.Display_boot` sibling `IOService` instances.
- Publish `navi33-p7-late-provider-audit-1` and
  `navi33-p7-late-provider-audit-2` to the
  `RedNaviProbe` IORegistry node.
- Report `navi33-p7-late-provider-audit-scheduled` boolean so the
  device is not silently considered sampled when a workloop could not
  schedule the observer.
- Cancel/disable/remove/release the timer and retained workloop
  before device teardown in `stop()`.
- No extra boot arguments or user clients.
- Version bump to experimental P4 **0.0.51**.

**No new PCI or GPU MMIO operations:** the new code does not open or
close the PCI provider, enable PCI memory, map BARs, touch C2PMSG,
submit an SMU command, or enable RLC, MES, display, DMA or Metal.

An `IOService::isOpen(child)` result only proves a matching host
client **currently has its provider open** (and a negative result
only means this host API did not see such an open at the sampled
instant). It does NOT establish that it wrote an MP1 mailbox or
that no other kernel/firmware actor can access BAR5.

The fields `foreign_mailbox_writer_excluded=0`,
`mailbox_exclusive_proven=0` and `may_issue_smu_command=0`
are intentionally immutable fail-closed policy.

P7.27 retains `p727MailboxLeaseHeld=false`.

## Tests

`tests/host/navi33_late_provider_open_audit_test.cpp`
checks the difference between children and provider opens as well
as irreversible no-ownership inference. Python tests verify the
observer is scheduled after `registerService()`, canceled in
`stop()`, and that the observer body invokes no GPU MMIO or
firmware submission functions.

```sh
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
```

## On a *recovery-capable* experimental EFI only

After completing CI/local builds and backing up the working 0.0.50
EFI, deploy 0.0.51 with unchanged boot arguments. About 15 seconds
after login, inspect:

```sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe |
  grep -E '"navi33-p7-late-provider-audit'
```

Expected: both snapshot dictionaries present and
`amd_support_present=1`, `boot_framebuffer_present=1`
as previously seen in the post-boot IOService tree.
**Do not pre-judge** `amd_support_provider_open` or
`boot_framebuffer_provider_open`: the point of P7.33 is to
observe them on real hardware.

If `navi33-p7-late-provider-audit-scheduled=0` or snapshots are
missing, investigate workloop scheduling and avoid interpreting
missing evidence as a zero provider-open state.

The next gate to *live* SMU commands remains independent device/
firmware mailbox ownership and a robust recovery strategy. No
change to native graphics, Metal, or RLC readiness is implied.
