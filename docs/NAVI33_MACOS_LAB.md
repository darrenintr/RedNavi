# Navi33 native macOS lab (RX 7600 / Sequoia)

This branch is an experimental, **non-invasive diagnostic and build baseline** for a
native Sequoia install booting through the RX 7600's firmware framebuffer.

## Why the native SSD test is useful

A booted Sequoia desktop on the RX 7600 is meaningful progress, but it does
**not** show that macOS has a working Navi33 Metal driver. Firmware/GOP
framebuffer scanout and CPU-rendered UI are independent of GFX11 queue
initialization. SSD vs HDD responsiveness measures storage latency as well as
the rendering path; it is not evidence of GPU acceleration.

The next acceptance criteria are: exact macOS and PCI identity, display/Metal
reports, a clean MacKernelSDK build, all host tests, and a tested rollback path.
Only afterwards is a hardware probe considered. The P4/P5 read path writes
MM_INDEX and may enable PCI memory decode; it is **not** write-free.

## First run: no sudo, no driver install

From a Terminal on the native Mac:

```sh
xcode-select --install                   # only if Command Line Tools are missing
git clone --branch feature/navi33-macos-lab \
  https://github.com/darrenintr/RedNavi.git
cd RedNavi
sh scripts/navi33-macos-lab.sh
```

The script performs:

1. `python3 tools/navi33_macos_baseline.py navi33-macos-baseline.json`:
   records a **whitelist** of macOS version, CPU core count, RX 7600 PCI
   `1002:7480` visibility, display/Metal description, volume summary,
   relevant loaded kexts, security status and presence (not contents) of
   relevant boot arguments. No serial number, EDID, VBIOS or raw IORegistry
   dump is collected.
2. `make test`: host-only test suite (no GPU writes).
3. `sh scripts/bootstrap.sh`, only if headers have not been fetched.
4. `make DEBUG=1`: compile development Kext bundles, **do not load them**.
5. `codesign --verify build/Debug/RedNavi.kext`: check signatures only.

This creates local `build/` and `MacKernelSDK/`; it does not change the
OpenCore EFI, SIP, NVRAM, GPU firmware, system extensions, or system partitions.
The produced Kext may match the RX 7600, but **must not** be installed merely
because it compiled.

## Inspect results

```sh
cat navi33-macos-baseline.json
ioreg -l -w0 -c IOPCIDevice | grep -i -m 3 '7480'
system_profiler SPDisplaysDataType
diskutil info /
```

The `ioreg` example prints raw information to the local terminal only; avoid
publishing unredacted output. The baseline tool is the preferred report to
share. A working desktop with `-radvesa` does not imply GPU acceleration.

## Before any Navi33 Kext hardware testing

- Preserve a known-good **USB OpenCore EFI without RedNavi** and a Windows
  recovery path. Keep the bootable system usable without loading the driver.
- If RX 7600 is the sole working display, do not enable the Navi32 PSP/MES,
  GART, DCN replay, `rednavi_fb`, `rednavi_accel2`, or firmware-loader
  boot arguments. A GPU reset can remove the only display and force a reboot.
- Do not disable SIP or modify the auxiliary kernel collection until an
  isolated hardware-test setup and rollback plan exist.
- Map Navi33's GC11.0.2 / SDMA6.0.2 / PSP13.0.7 / MMHUB3.0.2 using the
  actual discovery data. Never use Navi32's fixed VRAM address assumptions.
- On supported recoverable setups, begin with P4/P5/P6 preflight and examine
  individual `navi33-*` IORegistry results. Neither compile success nor
  `sos_alive` is an acceleration pass.

## Hardware milestone sequence (not completed)

1. P4 IP discovery and P5 memory-window/firmware readiness with a
   recoverable test machine.
2. Validate PSP13.0.7 firmware ownership and signed boot handshakes.
3. Own and verify VRAM/GART allocations and GPU PTE invalidation.
4. Bring up MES/KIQ, verify mapped HQD and completed doorbell/fence.
5. Confirm a shader-produced marker and completed fence; then test RADV
   compute, Vulkan graphics and Metal translation.

See `docs/NAVI33_P4_RUNTIME.md`, `docs/NAVI33_P5_COMPUTE.md` and
`docs/INIT_RESOURCES.md`.

**Hardware status:** Navi33 native Metal/compute are not yet validated on RX 7600.


## Verified macOS 15.8.2 baseline from the user's hardware (2026-11-09)

System: ASUS TUF B450M-PLUS GAMING / Ryzen 5 3600 / RX 7600
(PCI `1002:7480`). GPU appears as `GFX0`, OS PCI class `0x030000`.
The firmware framebuffer is `IONDRVFramebuffer` with `IOFBMemorySize =
8294400` bytes (`1920 * 1080 * 4`). `MTLCopyAllDevices()` returns
**zero** devices. The working desktop is not proof of Metal/3D acceleration.

The macOS IORegistry `assigned-addresses` array (five 20-byte PCI records)
and its matching `IODeviceMemory` descriptors yield:

| Region | CPU physical base | Size | Meaning |
| --- | ---: | ---: | --- |
| BAR0 | `0x830000000` | 256 MiB | CPU-mapped VRAM aperture |
| BAR2 | `0x840000000` | 2 MiB | Doorbell aperture |
| BAR4 | `0xE000` | 256 bytes | PCI I/O port resource |
| BAR5 | `0xFC900000` | 1 MiB | MMIO register aperture |
| ROM | `0xFCA00000` | 128 KiB | Expansion ROM resource |

The same physical RX 7600 under Linux had BAR0 **8192 MiB**, BAR2
**256 MiB**, and BAR5 **1 MiB** (see `NAVI33_REAL_LINUX_20261108.md`).
These are CPU physical resources, not GPU MC addresses. A smaller macOS
BAR0 does not reduce the GPU's physical VRAM from 8 GiB. Do not set
`RN_VRAM_MC_BASE`, a GPU PTE backing, or a GPU VA from any CPU BAR
physical address. `IOPCIExpressLinkStatus = 0x1084` is recorded as an
OS-reported field only; the motherboard's effective PCIe link speed should
not be inferred from it without upstream/root-port confirmation.

### Updated no-GPU-I/O report

```sh
cd ~/RedNavi
git pull --ff-only
python3 tools/navi33_macos_baseline.py navi33-macos-baseline.json
python3 -m unittest discover -s tests/host -p 'navi33_pci_resources_test.py'
make test
```

Inspect `rx7600.matching_devices[0].pci_resources` in the new report.
`p4_p5_resource_gate = pass` means only the OS PCI address-array structure
is coherent and minimum BAR sizes are present. It is **not** permission to
map BAR5 or load the probe; existing P4/P5 code writes MM_INDEX while reading
discovery and may change PCI memory decode state. No firmware, MES, DCN or
accelerator is enabled by this report.

The host tests pin the two observed CPU aperture sizes and verify that the
existing Navi33 `makeMemoryWindow` and typed address conversion logic can
handle 256 MiB BAR0, 2 MiB doorbell BAR2, and full 8 GiB Linux ReBAR
without treating a CPU-visible window as total VRAM. The live MMHUB
FB_LOCATION and FB_OFFSET values are *not yet captured on macOS*.

### Next hardware gate

Before touching MM_INDEX, prepare a genuine out-of-band recovery path and a
known-good USB EFI without RedNavi. The RX 7600 is currently the only
functional display; a soft SSH session alone does not protect against a GPU
reset, a wedged PCIe bus, or system panic. Isolate an intentionally bounded,
non-resetting probe and audit every read/write in it before any test run.


### P4 probe admission now fails closed by default

The default `RedNavi.kext` build is **not a hardware test**. The
`RedNaviProbe::start()` Navi33-specific early gate runs before
`setMemoryEnable(true)`, BAR5 mapping, IP Discovery, and any MM_INDEX
writes. With ordinary Debug/Release builds the gate returns
`DisabledInBuild` even if an old boot-argument was left behind.

An experimental compilation flag `RN_ENABLE_NAVI33_UNSAFE_P4` AND
the exact `rednavi_navi33_unsafe_p4=1` boot argument would both be needed
to reach the legacy risky path. **Do not enable either on the sole-display
Sequoia machine.** This is an accidental-I/O gate, not a promise that
matching a Kext to an active GPU is harmless. The legacy `RedNavi.kext` personality now intentionally **excludes**
`1002:7480` from `IOFramebuffer` matching. The new
`RedNaviNavi33Diag.kext` has its own `RedNaviDiagnostic` category and only
matches `1002:7480`. A distinct matching category reduces framebuffer
driver competition, but does **not** prove attaching a Kext is safe.

Host test: `tests/host/navi33_probe_admission_test.cpp` checks that a
normal build cannot opt in and a hypothetical experimental build still
rejects missing or unexpected boot-argument values. These tests perform
no hardware I/O.

Next bring-up task is to audit register-access/read-side effects and
prepare an independent recovery method before any GPU hardware read.


### Independent diagnostic Kext: compile only, never load by default

The experimental metadata observer uses `IOPCIDevice::configRead16` and
`getDeviceMemoryWithRegister` to read **PCI resource metadata only**.
It does not map BAR5, write PCI command flags, modify MM_INDEX, load
firmware, configure GPU VM, reset the GPU, or claim the framebuffer.
It is excluded from `make all` and is not packaged in CI artifacts.

```sh
cd ~/RedNavi
git pull --ff-only
make test
make DEBUG=1 diag
codesign --verify build/Debug/RedNaviNavi33Diag.kext
```

**Do not run** `kmutil install`, `kextload`, or inject this Kext into
OpenCore EFI on a sole-display RX 7600 yet. This component has not been
loaded on the real machine. CI compilation is not GPU runtime validation.


## Existing RedNavi Navi33 hardware path: opt-in P4 build (not a new port)

Cobalt64's proven RX7600 parameters and much of the Navi33 host
implementation were already integrated into RedNavi. The next blocker
is **runtime wiring**, not more parameter copying.

The current `RedNaviProbe::start()` has a separate `1002:7480` case
which performs GPU IP Discovery, MMHUB/FB_LOCATION readback, PSP/SMU
mailbox preflight, and publishes `navi33-*` IORegistry properties.
It then returns **before the legacy Navi32 PSP, GART, SDMA, DCN or MES
runtime**. This is intentionally not a Navi33 compute/Metal implementation.

The production `RedNavi.kext` does not match RX7600. A distinct
**explicitly experimental** bundle reuses the **same** RedNavi source
without importing any Cobalt64 code or rerunning parameter migration:

```sh
cd ~/RedNavi
git pull --ff-only
make test
make DEBUG=1 navi33-p4
codesign --verify build/Debug/RedNaviNavi33P4.kext
```

- The separate bundle has `org.rednavi.Navi33P4` and only matches
  `1002:7480` with `IOMatchCategory=RedNaviP4Experimental`, **not**
  `IOFramebuffer`.
- Its `kern_probe.cpp` is compiled with
  `RN_ENABLE_NAVI33_UNSAFE_P4`, but the runtime still rejects GPU
  hardware access unless **the exact boot argument**
  `rednavi_navi33_unsafe_p4=1` is also present.
- Unlike the simple metadata observer, this path **modifies PCI
  memory-enable state and writes MM_INDEX/MM_INDEX_HI** during
  discovery. It can hang or disturb the sole video card.
- The build target is **not** in `make all`, normal Kext artifacts,
  the boot EFI or any automatic install routine.
- CI compiles both Debug and Release experimental bundles, but does
  not publish or inject them. The host tests assert compile/boot
  admission and separate matching category.
- GitHub Actions does **not** install or inject P4. On **2026-11-09**
  the user explicitly booted the P4 USB on native Sequoia; actual
  `org.rednavi.Navi33P4 (0.0.1)` loaded and `RedNaviProbe` registered.
  This is a P4 IP Discovery proof, NOT compute or Metal acceleration.

### First native RX7600 P4/P5/P6 evidence (2026-11-09)

The boot argument `rednavi_navi33_unsafe_p4=1` was present. The live
IORegistry reported `navi33-identity-verified=Yes`,
`navi33-discovery-error=0`, 6 required IP blocks,
`navi33-ip-bases-ready=Yes` and `vram_mib=8176`.
P5 initially read all three MMHUB FB registers as `0xFFFFFFFF`;
`navi33-memory-preflight.preflight_error=2` (BadLocation) and
`navi33-linux-boot-preflight.blocker=2` (MissingMemory).

Root cause in the probe source: **wrong version-specific MMHUB register
offsets**, copied from `mmhub_3_0_0_offset.h` rather than the Navi33
`mmhub_3_0_2_offset.h` that Linux's `mmhub_v3_0_2.c` uses.
Correct native RX7600 dword offsets from MMHUB segment 0 are
`FB_LOCATION_BASE=0x086c`, `TOP=0x086d`, and `FB_OFFSET=0x0857`;
the incorrect 3.0.0 values were `0x08ec`, `0x08ed`, `0x08d7`.
The corrected P5 readback **still requires real-device retesting**.
Do not replace Navi32's separate MMHUB 3.0.0 register table globally.

Raw P6 mailbox reads were `IFWI=0x80000000` (ready bit set),
`sOS=0`, `SMU response=1`. But P6 gate `status=3` means
**MemoryUnverified**, and does not establish firmware initialization,
sOS ownership, GPU VM setup, or PSP-ring command readiness.
All PSP/GART/MES GPU writes remain disabled.

### Second native P5 readback (2026-11-10)

P4 bundle `0.0.2` confirmed the MMHUB 3.0.2 offset fix on the same
RX7600. Live values were `FB_LOCATION_BASE=0x8000`,
`FB_LOCATION_TOP=0x81ff`, `FB_OFFSET=0`,
`RCC_CONFIG_MEMSIZE=8176 MiB`, and macOS BAR0 remained 256 MiB.
The resulting MMHUB location span is 8192 MiB, exactly one 16 MiB unit
larger than the NBIO/RCC memsize.

This is not an MMIO-read failure. Linux GMC11 sets `mc_vram_size` from
NBIO `get_memsize()` and uses MMHUB `get_fb_location()` as the VRAM
base; RedNavi's old gate incorrectly required the FB_LOCATION span to
never exceed memsize. P4 `0.0.3` therefore accepts a one-unit
difference in either direction but exposes only
`min(FB_LOCATION span, RCC memsize)` as usable VRAM. For this native
macOS capture that is 8176 MiB, with CPU-visible VRAM clipped to the
256 MiB BAR0.

The corrected memory geometry still needs a third native USB boot before
P5 can be declared passed. Expected next state: `preflight_error=0`,
`memory-window-verified=Yes`, `address-geometry-verified=Yes`.
With `IFWI=0x80000000` and `sOS=0`, P6 should then advance from
`MemoryUnverified` to the fail-closed `NeedsVerifiedFirmware` state;
that still does not authorize PSP commands.

Do not add the P4 bundle to the known-good OpenCore EFI. Use only a
separate, recoverable **test EFI**, and keep the clean boot path
available. Never co-install the normal RedNavi and P4 bundles; both
contain `RedNaviProbe` runtime classes. On the sole-display RX7600
you should also have an independently proven recovery procedure
before taking this risk.

### Next verified hardware checkpoint

After rebuilding P4 with corrected MMHUB 3.0.2 offsets, reboot
the isolated test USB and collect the P4/P5/P6 IORegistry fields
including exact readback from
`navi33-memory-preflight`, `navi33-address-space-audit`,
`navi33-firmware-preflight`, `navi33-linux-boot-preflight`.
Only after those checks can we wire the existing Navi33 PSP/GART/MES
implementation into the RedNavi runtime. The historical Cobalt64
compute proof was in a specific RX7600 GPU-passthrough environment;
its absolute GPU VM/VRAM ownership must be revalidated on bare-metal
Sequoia.
