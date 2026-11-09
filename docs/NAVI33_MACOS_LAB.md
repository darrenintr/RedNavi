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
