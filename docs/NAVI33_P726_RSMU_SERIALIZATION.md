# Navi33 P7.26 — shared RSMU transaction serialization (0.0.44)

## Starting evidence (2026-11-12)

RX 7600 real macOS boot with 0.0.43:
- Kext `0.0.43` loaded; actual NBIO IP `4.3.1`, NBIF segment1 base `0x14` DWORD.
- `nbio_pf0_layout_supported=1`, `nbio_pf1_layout_supported=0`.
- Both P7.26 opt-ins were off; `index_written=0`, `data_read=0`, `snapshot_valid=0`, `gate_error=2`.
- `smu_handshake_verified=0`, `firmware_order_matches_linux=0`, RLC autoload blocked.

This validates **passive PF0 version admission**, not an actual RSMU firmware-flags measurement.

## Why change the lock

`compat/src/rn_reg.cpp` already serializes indirect RSMU accesses using
`gpu.mmio_idx_lock` when invoked with the main device. Previously, P7.26 in
`kern_probe.cpp` manipulated PF0 INDEX/DATA directly with `rd`/`wr`
without that lock. The full save/select/read/restore sequence must use the
**same lock**, rather than an unrelated new lock.

The new `sampleSerialized` transaction performs these operations while
the caller holds `gpu.mmio_idx_lock`:
1. Admission gate check **before** lock and MMIO.
2. Lock; read original INDEX; reject all-ones sentinel without writing.
3. Select `0x03B10028`; read INDEX back to flush and verify.
4. Read DATA once only if selected INDEX matches.
5. Restore original INDEX even on failed selection; verify the restore.
6. Unlock; compute validity only from the complete transcript.

Callbacks do not sleep or call other indirect helpers under the lock.
The driver-local lock does **not** serialize unrelated PCI functions,
firmware, other kexts, or any legacy access bypassing `gpu.mmio_idx_lock`.
In particular, the separate opt-in `smuProbe()` historical diagnostic
has a direct INDEX write and uses a different address (`0x03B10024`);
it must **not** be enabled concurrently for Navi33. That diagnostic path
is outside the Navi33 early-return branch but remains unvalidated.

## Fail-closed scope

No new boot arguments, no automatic RSMU read, no PSP command, and no
RLC autoload in this change. P7.26 retains the two existing mandatory
opt-ins, disabled by default:

```text
rednavi_navi33_p726_mp1_fwflags=1
rednavi_navi33_p726_nbio43_pf0=1
```

**Do not enable the two flags in the working EFI.** The new 0.0.44 build
is intended first for *default-off* host/CI and recoverable-EFI testing.

## Host tests

`tests/host/navi33_mp1_fwflags_smn_probe_test.cpp` now verifies:
- Lock is held for every INDEX/DATA access, including restore.
- On success, readback matches; original INDEX is restored and validated.
- Closed admission does not acquire lock or touch MMIO.
- Failed INDEX selection never reads DATA and still restores INDEX.
- Unreadable original INDEX produces zero writes.

Run:

```sh
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' \
  build/Release/RedNaviNavi33P4.kext/Contents/Info.plist
```

Expected version: `0.0.44`. Compile/tests are not real-hardware proof.

## First real-hardware check (default OFF)

Preserve working 0.0.43 EFI and a *separate* bootable rescue EFI.
Only replace the kext on a recoverable experimental OpenCore route.
Do not add either P7.26 opt-in or enable unsafe RLC autoload.
After boot:

```sh
kmutil showloaded | grep -i navi33
ioreg -r -l -w0 -c RedNaviProbe |
  grep -E '"navi33-(nbio-|p7-mp1-fwflags|p7-rlc-preflight)'
```

Expected:
- Kext `0.0.44`, NBIO 4.3.1 and `nbio_pf0_layout_supported=1`.
- `gate_error=2`, `index_written=0`, `snapshot_valid=0`.
- `smu_handshake_verified=0`, `may_autoload=0`.

Even a valid future `MP1_FIRMWARE_FLAGS.INTERRUPTS_ENABLED` sample
will **not** certify the SMU mailbox handshake, Linux firmware load order,
or a graphics queue. Do not bypass those gates.
