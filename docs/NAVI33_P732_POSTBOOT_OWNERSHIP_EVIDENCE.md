# P7.32 — post-boot IOService ownership evidence (user space only)

The physical RX 7600 with RedNaviNavi33P4 0.0.50 has verified
P7.31 default-off state:

- `one_slot_all_navi33_in_kext=1`
- `attempt_slot_claimed=0`
- `survives_service_reprobe=1`
- `survives_kext_reload=0`
- `live_smu_command_enabled=0`, `command_sent=0`,
  `handshake_verified=0`, `mailbox_exclusive_proven=0`

**No SMU command has been sent on Navi33.** A BAR5
`C2PMSG_90=1` value read before a new command is not a firmware
roundtrip.

## Why observe later?

The P7.28 `navi33-p7-mailbox-provider-audit` is constructed
inside `RedNaviProbe::start()`, before the Navi33 path calls
`registerService()`. It observed one GFX0 child and neither
`AMDSupport` nor `.Display_boot`. An *earlier full booted-system*
`ioreg -t` dump had both services attached. This is a timing
mismatch, not proof that those children were absent after boot.

The ownership question is not resolved by creating more synthetic
locks or by repeatedly rebooting a single-GPU machine.

## New observer: NO new Kext / hardware I/O

`tools/navi33_owner_snapshot.py` captures multiple snapshots of:

1. `ioreg -p IOService -w0 -t` and the current `GFX0` subtree;
2. `ioreg -p IOService -r -l -w0 -c RedNaviProbe` P7 properties;
3. `kmutil showloaded` matching AMD/RedNavi Kext names.

It is run after macOS finishes booting and can be repeated without
replacing EFI, reloading a Kext, or opening any PCI user client.
It has **no `sudo`**, does not read physical registers, and does
not execute `ioreg -w` (the option `-w0` is output width, not
a hardware write).

Run on the **existing** 0.0.50 system:

```sh
cd ~/RedNavi
git fetch origin
git switch --track origin/feature/navi33-p732-userland-owner-observer
python3 -m unittest discover -s tests/host -p navi33_owner_snapshot_test.py
python3 tools/navi33_owner_snapshot.py --samples 3 --interval 3 \
    --output "$HOME/Desktop/navi33-owner-evidence.txt"
```

The resulting text can be sent back for review. The file is created
exclusively: it will not overwrite a previous evidence file.

## Interpretation

- `AMDSupport` and `.Display_boot` appearing beneath GFX0
  confirms *service attachment at the sample time*, not PCI
  `open()` ownership or SMU C2PMSG writes.
- `kmutil showloaded` confirms Kext code is loaded, not that
  it is bound to the GPU or owns its mailbox.
- An unchanged tree in multiple samples is NOT proof that SMU
  response/parameter/message registers were idle.
- `IOService::isOpen()` is a host client-open check. A late
  userspace IORegistry tree does not expose the true result of
  `pci->isOpen()`, and neither result can exclude firmware or
  clients bypassing provider arbitration.
- Do not use a matched `AMDSupport` instance to claim Apple is
  sending SMU commands without additional direct evidence.
- Apple legacy `IONDRVFramebuffer` represents a boot display path;
  it is not the native accelerated RX 7600 driver.

The next gate to a *live* SMU version query requires review of
all participating host drivers and firmware, and an explicit
recovery design. This tool only gathers supporting evidence. It
does not change the P7.27 hardcoded `mailboxLeaseHeld=false`,
not does it permit RLC/MES/Metal.

## Validation

```sh
python3 -m unittest discover -s tests/host -p navi33_owner_snapshot_test.py
make test
```

The existing 0.0.50 Kext is unchanged in this P7.32 branch.
