# Initialization resources: DMA pages and MES workspace

This change prepares the shared native backend for Navi33 while fixing memory
contracts in the existing Navi32 execution path. **It does not enable Navi33
hardware initialization.** The Navi33 PSP/GMC adapter and physical RX 7600
validation remain outstanding. No XNU source or kernel-loading policy changes
are required for these driver-side changes.

## Host BO mapping is validated before publication

`RedNavi/rn_dma_pages.h` consumes `IODMACommand::Segment64` batches and expands
them into a complete list of device-visible DMA page addresses. These addresses
are independent of CPU pointers, CPU physical addresses and GPU virtual
addresses. The collector checks:

- capacity, positive progress and exact byte coverage;
- segment count and cursor consistency across batches;
- 4 KiB alignment and nonzero page-multiple lengths;
- address overflow and the GMC11 44-bit DMA limit, including each final byte.

DMA IOVA zero is valid. The collector clears the page addresses it populated
on failure and performs no GPU access. IOKit remains the trusted producer of
the segment array; validating its returned metadata cannot contain a producer
that writes beyond the capacity passed to it.

`RedNaviProbe::createHostBo()` now requests a 44-bit, page-aligned DMA mapping
and validates **all** pages before writing any GART PTE. A failed enumeration
does not advance the GART cursor or leave a partial mapping. The existing
four-page `rednavi_gart_test` diagnostic uses the same validation and additionally
requires four contiguous DMA pages, since its packets use `base + page offset`.
Its readiness check uses the validated length rather than a nonzero base address.

After PTE installation, the VMID0 invalidation must acknowledge. If it times
out, the new PTEs are cleared and invalidation is retried. A successful rollback
releases the unexposed BO. A second timeout reserves the slot and VA range,
charges the normal BO budget, and retains the DMA mapping and backing until
reboot. The caller receives allocation failure. `destroyBo()` also retains a
GART BO if its unmap invalidation fails; `stop()` does not release those
quarantined allocations. This bounded retention prevents a stale device
translation from pointing at recycled memory.

There is no verified engine shutdown/drain yet. If `stop()` follows a live GPU
session, it also retains all published host BO DMA mappings and the standalone
diagnostic host allocation until reboot. It cancels callbacks before clearing
the workspace and locks. This deliberately trades retained memory for avoiding
DMA into freed pages; the provider's existing allocation budget remains the
bound. Inactive, non-quarantined allocations can still be released.

This is not a complete GPU teardown implementation. Ordinary userspace VM
invalidation error handling and engine quiescence before unloading remain
separate work. A software workspace reset is not proof that the GPU has stopped,
and unloading/reloading an active GPU remains unsupported.

## MES has one explicit memory description

Previously the host workspace used `gpuVa - vramMcBase` as a synthetic VRAM
offset. A global macro redirected some VRAM accessors into the host buffer;
an out-of-range host access could fall back to real VRAM access. This also
made later queue submissions and resets depend on an implicit address convention.

`RedNavi/rn_mes_workspace.h` supplies separate `GpuVa` and `VramOffset` types and
one retained `Workspace`:

```cpp
auto host = rn_mes::Workspace::host(
    rn_mes::GpuVa(bo.gpuVa), bo.md->getBytesNoCopy(), bo.size);

auto vram = rn_mes::Workspace::vram(
    rn_mes::GpuVa(vramMcBase + offset),
    rn_mes::VramOffset(offset), rn_mes::kWorkspaceBytes);
```

All MES offsets are relative to this allocation. GPU packet and MQD addresses
come from validated GPU regions. CPU access uses the supplied host mapping or
the explicit VRAM offset, including checks for the complete four-byte access.
An invalid host access never invokes the VRAM callbacks.

The current layout and packet formats are preserved:

| Region | Relative offset | Size / use |
| --- | ---: | --- |
| MES KIQ block | `0x00000` | ring at `+0x5000`, 64 KiB |
| MES scheduler block | `0x20000` | ring at `+0x5000`, 32 KiB |
| Shared context and completion words | `0x30000` | 16 KiB |
| Kernel compute block | `0x40000` | ring at `+0x5000`, 8 KiB |
| Event log and scratch | `0x50000` | `0x44000` bytes |
| Graphics block | `0x9C000` | ring at `+0x5000`, 32 KiB |
| Required allocation | `0x00000` | `0xB0000` bytes total |

The validator checks mapping presence, size, alignment, address overflow and
overlap of the active regions. Initial setup, late scheduler frames, queue
resets, compute submission and graphics submission share this description.
The legacy direct-KIQ VRAM path retains its own explicit access path.

Ring emission now reports failure to its caller. A missing doorbell returns
`-ENODEV`; a workspace access fault returns `-EIO`. Ring-space and completion
polls stop on an access fault instead of waiting for a timeout. Software write
counters and the last graphics VMID advance only after a successful doorbell
emission, so a rejected submission is not reported as accepted work.

The host workspace remains the default. `rednavi_mes_host=0` explicitly selects
VRAM. If the requested host allocation fails, MES reports an error instead of
silently selecting VRAM. `mes-start.workspace_error` exposes the numeric
`rn_mes::Error` value; zero means no workspace error, **not** successful GPU
initialization. The usual stage, firmware status and hardware observations
remain necessary.

`rednavi_mes_step >= 7` is rejected as `UnsupportedProcessPath` before allocating
a MES workspace or entering MES setup. That parked process-queue experiment
assumed VRAM-backed GPUVM tables and cannot safely share the host workspace.
Stages 1 through 6 remain available for the existing native path. Navi33
continues to reject this native MES entry point as `UnsupportedAsic`.

## Initialization order contract

The host-only Navi33 plan now models software resource readiness, PSP transport,
TMR allocation, SMU firmware loading, TMR installation and graphics firmware
loading separately. It requires GC 11.0.2 and PSP/SMU 13.0.7. The selected MES
firmware gates and the initialization-time KIQ `SET_RESOURCES` exception are
retained. Scratch-register tests are named as such, and a later MES hardware
callback closes the plan. Empty, truncated or fabricated plans cannot report
successful completion.

See [NAVI33_LINUX_BOOT_PARITY.md](NAVI33_LINUX_BOOT_PARITY.md) for the source
mapping and limits. The plan is not connected to an executor that performs
Navi33 GPU writes and cannot establish hardware readiness from caller flags.

## Verification

```sh
make test                         # macOS / Clang
make test HOST_CXX=g++             # Linux / GCC
make check-kernel-firmware         # macOS + MacKernelSDK
```

`make test` includes the DMA and MES workspace regressions, alongside the
existing host suite. Two additional recording-stub tests compile the actual
`gfx11.cpp` implementation and exercise startup rejection and submission
failure propagation. The submit test includes that implementation directly
to inject internal queue-offset and polling faults; it is not a second copy
of the implementation. The initialization-resources CI job runs these tests
and the DMA, workspace and boot-order tests with GCC and Clang, AddressSanitizer and
UndefinedBehaviorSanitizer. Cases include multi-batch and discontiguous DMA,
short/oversized mappings, IOVA zero, address-width boundaries, GPU addresses
below the VRAM MC base, the last valid dword and rejected overrun accesses.
Runtime cases include zero-I/O startup rejections, a successful synthetic
doorbell write, missing doorbells, invalid queue offsets, faults during
ring-space polling and stale completion words after a failed scheduler emit.
The stubs provide no-op locks/delays and record I/O; they do not emulate GPU
firmware, concurrent execution or cache coherence.

Native Debug and Release builds compile the changed driver. A separate
`-fsyntax-only` target enables `RN_HAVE_GC_FW` to compile the normally excluded
MES caller. Its fixture contains declarations only, no firmware definitions or
bytes, and is never linked into the uploaded kexts. This checks the optional
native API boundary without making a hardware or firmware-validity claim.

These checks do not reproduce IOMMU behavior, GPU cache invalidation, queue
execution or active-device unload on physical hardware. Existing Navi32 smoke
tests and a staged Navi33 hardware bring-up are still required before enabling
new hardware paths.

## Source basis

- [Linux GMC11 DMA setup](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c)
- [Linux MES11 initialization](https://github.com/torvalds/linux/blob/7475ea8b58616b33c04afb976cf0fbaad9dd63fb/drivers/gpu/drm/amd/amdgpu/mes_v11_0.c)
- [XNU IODMACommand interface](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IODMACommand.h)
- [XNU IOBufferMemoryDescriptor implementation](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOBufferMemoryDescriptor.cpp)
