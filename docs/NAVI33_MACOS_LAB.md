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
