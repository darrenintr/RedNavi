# Navi33 P7.38 — offline SMU mailbox ownership review

P7.37 physical RX 7600: both paired triplets read Message=0, Parameter=0, Response=1; the second was classified stable. This is not an owned SMU handshake or an exclusive mailbox lease.

## Changes

P7.38 adds only tools/navi33_p738_ownership_audit.py and host regression tests. It does not change the Kext, boot arguments, MMIO accesses, PSP, RLC, MES or GPU queues.

The offline report joins P7.33 IOService snapshots, P7.36/P7.37 MP1 receipts and the P7.34 known RedNavi writer inventory. It checks missing or duplicate samples, response-to-triplet agreement, value/timestamp order, BAR5 offsets, read budgets, evidence classes, and unsupported ownership claims. Static scanning cannot exclude Apple, firmware or dynamically computed writes.

## Usage

Capture on macOS: ioreg -r -l -w0 -c RedNaviProbe > "$HOME/Desktop/RedNavi-P738-ioreg.txt"

Analyze from the checked-out P7.38 branch: python3 tools/navi33_p738_ownership_audit.py --input "$HOME/Desktop/RedNavi-P738-ioreg.txt" --repo . --output "$HOME/Desktop/RedNavi-P738-report.json"

Run tests: python3 -m unittest discover -s tests/host -p navi33_p738_ownership_audit_test.py

Exit code 2 means incomplete or inconsistent observed evidence; exit 0 only means the saved evidence is internally consistent, never that SMU commands are authorized. The script itself performs no GPU access.

## Remaining blockers

1. No independently demonstrated exclusive platform-level MP1 mailbox ownership.
2. Provider-open and IOService attachment are not BAR5/MMIO writer arbitration.
3. Apple/third-party and firmware writers cannot be excluded by saved snapshots.
4. Sequential sample values are not an atomic transaction.
5. No attributed RedNavi GetSmuVersion request with a fresh firmware response.

The report always sets decision.may_issue_smu_command=false, decision.enable_p727_get_smu_version=false and decision.enable_rlc_mes_or_graphics_queue=false. No new boot-args or EFI changes are required.