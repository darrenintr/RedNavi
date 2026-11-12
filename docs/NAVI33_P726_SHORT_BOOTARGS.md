# Navi33 P7.26 — short double opt-in boot arguments (experimental 0.0.45)

## Evidence, 2026-11-12

RX 7600 had a validated 0.0.44 **default-off** boot:
- `RedNaviProbe` existed, P7.26 PF0 support = 1, NBIO 4.3.1.
- Both long opt-ins off; no INDEX write, no SMN sample.

After appending two long P7.26 arguments, the user observed:
- `kmutil showloaded` listed 0.0.44.
- `sysctl kern.bootargs` returned an empty string.
- `ioreg -c RedNaviProbe` returned no service.
- The OpenCore EFI plist and IODeviceTree NVRAM entry contained the two flags.
- `kernelmanager_helper` logged a missing identifier notification in both
  the working and nonworking boots; it is not proven causal.

The complete configured line was **1014 ASCII bytes**; removing the two
P7.26 long flags leaves **947 bytes**. A/B recovery on 2026-11-12 at ~21:14: after removing the two long flags
and rebooting the unchanged 0.0.44 Kext, `sysctl kern.bootargs` returned
the full existing string and `ioreg -c RedNaviProbe` reported
`navi33-nbio-major=4` again. This strongly supports a boot-argument
length/forwarding issue, but does not yet identify which layer or buffer
caused the failure.
Some XNU boot-argument structures have a 1024-byte command line buffer;
1014 bytes alone is not mathematically greater than 1024.

## Backward-compatible short flags

```text
rn726fw=1
rn726pf0=1
```

The first is equivalent in intent to
`rednavi_navi33_p726_mp1_fwflags=1`, the second to
`rednavi_navi33_p726_nbio43_pf0=1`. Both remain independently required.

For P7.26 no direct hardware accesses occur unless:
- `rednavi_navi33_p7_step=27`,
- RX7600 exact ASIC / MP1 13.0.7, NBIO 4.3.0 or 4.3.1,
- accepted PMFW, PSP fence 23/23, healthy transport,
- NBIF segment1 / BAR5 range validated,
- **both** firmware-flags and PF0 opt-in categories have a value of 1.

The code accepts old or short names for compatibility, but when both
spellings for one category are set, a conflict (e.g. one value 0) is
rejected. No prefix/substring shortcut.

Using two short flags instead of the two old flags reduces the reported
line from 1014 to approximately **968 bytes**. **Replace**, do not append to, the two
long flags. The 0.0.44 Kext does not understand these new flags.

## Test plan / failure containment

1. **Done on 2026-11-12:** restored default-off 0.0.44 boot args; both
   `sysctl kern.bootargs` and `RedNaviProbe` reappeared. This is an
   A/B correlation, not proof of the precise underlying overflow.
2. Build and host-test 0.0.45 on this feature branch.
3. Keep a known-good USB and an independent bootable rescue EFI.
4. Test 0.0.45 with *no P7.26 opt-ins* first: no index write, no RLC.
5. Only with known-good recovery and an explicit second test EFI, replace
   both old flags with `rn726fw=1 rn726pf0=1`.
6. Read `sysctl kern.bootargs`, `kmutil showloaded` and
   `ioreg -r -l -w0 -c RedNaviProbe` telemetry.

If there is no `RedNaviProbe`, do NOT interpret missing
`navi33-p7-mp1-fwflags` as a failed hardware transaction.

Even a valid firmware-flags snapshot does not establish an SMU
GetSmuVersion handshake or permission to autoload RLC. This branch
does not change the RLC gating or launch any graphics engine.

## Host build

```sh
make test
make check-kernel-psp-firmware
make check-kernel-rlc-autoload
make clean
make navi33-p4
/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' \
  build/Release/RedNaviNavi33P4.kext/Contents/Info.plist
```

Expected: `0.0.45`. CI and host tests cannot substitute for a recoverable
real-hardware test.
