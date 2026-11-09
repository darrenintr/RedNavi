// SPDX-License-Identifier: BSD-3-Clause
#ifndef REDNAVI_MES_WORKSPACE_H
#define REDNAVI_MES_WORKSPACE_H

#include <stddef.h>
#include <stdint.h>

// A retained, already mapped VMID0 allocation used by the native MES backend.
// Mapping and DMA ownership belong to the caller; these types never create a
// GPU mapping or establish hardware readiness from a caller-provided flag.
namespace rn_mes {

struct GpuVa {
    uint64_t value;
    explicit constexpr GpuVa(uint64_t v = 0) : value(v) {}
};
struct VramOffset {
    uint64_t value;
    explicit constexpr VramOffset(uint64_t v = 0) : value(v) {}
};

enum class Backing : uint8_t { None, Host, Vram };
enum class Error : uint32_t {
    Ok, NoBacking, MissingCpuMapping, BadAlignment, InsufficientBytes,
    AddressOverflow, OutsideWorkspace, OverlappingRegions, InvalidRingSize,
    UnsupportedAsic, UnsupportedProcessPath, AlreadyConfigured
};

constexpr uint64_t kWorkspaceBytes = 0xB0000;
constexpr uint64_t kClearBytes = 0xA0000;
constexpr uint64_t kKiqBlock = 0;
constexpr uint64_t kSchedulerBlock = 0x20000;
constexpr uint64_t kSharedBlock = 0x30000;
constexpr uint64_t kComputeBlock = 0x40000;
constexpr uint64_t kEventLog = 0x50000;
constexpr uint64_t kEventLogBytes = 0x44000; // 16 KiB log + 256 KiB scratch
constexpr uint64_t kGraphicsBlock = 0x9C000;
constexpr uint64_t kRingAt = 0x5000;
constexpr uint64_t kRptrAt = 0x1000;
constexpr uint64_t kWptrAt = 0x1008;
constexpr uint64_t kMqdAt = 0x2000;
constexpr uint64_t kEopAt = 0x4000;
constexpr uint64_t kMqdBytes = 2048;
constexpr uint64_t kEopBytes = 2048;
constexpr uint32_t kKiqRingBytes = 0x10000;
constexpr uint32_t kSchedulerRingBytes = 0x8000;
constexpr uint32_t kComputeRingBytes = 0x2000;
constexpr uint32_t kGraphicsRingBytes = 0x8000;
constexpr uint64_t kGpuAddressMax = (1ull << 48) - 1;

inline bool contains(uint64_t total, uint64_t offset, uint64_t bytes) {
    return bytes && offset <= total && bytes <= total - offset;
}
inline bool aligned(uint64_t value, uint64_t alignment) {
    return alignment && !(alignment & (alignment - 1)) &&
           !(value & (alignment - 1));
}

struct Region {
    uint64_t offset = 0;
    uint64_t bytes = 0;
    GpuVa gpu{};
};
struct Queue {
    Region ring{}, rptr{}, wptr{}, mqd{}, eop{};
};

struct Workspace {
    Backing backing = Backing::None;
    GpuVa gpuBase{};
    VramOffset vramBase{};
    volatile uint8_t *cpuBase = nullptr;
    uint64_t bytes = 0;

    static Workspace host(GpuVa gpu, volatile void *cpu, uint64_t size) {
        Workspace w;
        w.backing = Backing::Host;
        w.gpuBase = gpu;
        w.cpuBase = static_cast<volatile uint8_t *>(cpu);
        w.bytes = size;
        return w;
    }
    static Workspace vram(GpuVa gpu, VramOffset offset, uint64_t size) {
        Workspace w;
        w.backing = Backing::Vram;
        w.gpuBase = gpu;
        w.vramBase = offset;
        w.bytes = size;
        return w;
    }

    Error validate() const {
        if (backing != Backing::Host && backing != Backing::Vram)
            return Error::NoBacking;
        if (bytes < kWorkspaceBytes) return Error::InsufficientBytes;
        if (!aligned(gpuBase.value, 4096) || !aligned(bytes, 4096))
            return Error::BadAlignment;
        if (gpuBase.value > kGpuAddressMax ||
            bytes - 1 > kGpuAddressMax - gpuBase.value)
            return Error::AddressOverflow;
        if (backing == Backing::Host) {
            if (!cpuBase) return Error::MissingCpuMapping;
            const uintptr_t base = reinterpret_cast<uintptr_t>(cpuBase);
            if (!aligned(base, 4)) return Error::BadAlignment;
            if (bytes - 1 > UINTPTR_MAX - base) return Error::AddressOverflow;
        } else {
            if (!aligned(vramBase.value, 4096)) return Error::BadAlignment;
            if (bytes - 1 > UINT64_MAX - vramBase.value)
                return Error::AddressOverflow;
        }
        return Error::Ok;
    }
    bool valid() const { return validate() == Error::Ok; }

    Error region(uint64_t offset, uint64_t length, uint64_t alignment,
                 Region *out) const {
        const Error error = validate();
        if (error != Error::Ok) return error;
        if (!out || !contains(bytes, offset, length))
            return Error::OutsideWorkspace;
        if (!aligned(offset, alignment) ||
            !aligned(gpuBase.value + offset, alignment))
            return Error::BadAlignment;
        *out = {offset, length, GpuVa(gpuBase.value + offset)};
        return Error::Ok;
    }

    Error queue(uint64_t block, uint32_t ringBytes, Queue *out) const {
        if (!out) return Error::OutsideWorkspace;
        if (ringBytes < 8 || (ringBytes & (ringBytes - 1)))
            return Error::InvalidRingSize;
        if (!contains(bytes, block, kRingAt)) return Error::OutsideWorkspace;
        Queue q;
        Error error = region(block + kRingAt, ringBytes, 256, &q.ring);
        if (error != Error::Ok) return error;
        if ((error = region(block + kRptrAt, 8, 8, &q.rptr)) != Error::Ok ||
            (error = region(block + kWptrAt, 8, 8, &q.wptr)) != Error::Ok ||
            (error = region(block + kMqdAt, kMqdBytes, 4096, &q.mqd)) != Error::Ok ||
            (error = region(block + kEopAt, kEopBytes, 256, &q.eop)) != Error::Ok)
            return error;
        *out = q;
        return Error::Ok;
    }

    // These callbacks only address VRAM offsets. Invalid host accesses never
    // fall through to them, even if the numeric offset looks like VRAM.
    using VramRead = uint32_t (*)(void *, VramOffset);
    using VramWrite = void (*)(void *, VramOffset, uint32_t);
    Error read32(uint64_t offset, uint32_t *value, VramRead read,
                 void *context) const {
        Region r;
        const Error error = region(offset, 4, 4, &r);
        if (error != Error::Ok) return error;
        if (!value) return Error::OutsideWorkspace;
        if (backing == Backing::Host) {
            *value = *reinterpret_cast<volatile uint32_t *>(cpuBase + offset);
        } else {
            if (!read) return Error::NoBacking;
            *value = read(context, VramOffset(vramBase.value + offset));
        }
        return Error::Ok;
    }
    Error write32(uint64_t offset, uint32_t value, VramWrite write,
                  void *context) const {
        Region r;
        const Error error = region(offset, 4, 4, &r);
        if (error != Error::Ok) return error;
        if (backing == Backing::Host) {
            *reinterpret_cast<volatile uint32_t *>(cpuBase + offset) = value;
        } else {
            if (!write) return Error::NoBacking;
            write(context, VramOffset(vramBase.value + offset), value);
        }
        return Error::Ok;
    }
};

inline bool overlaps(const Region &a, const Region &b) {
    // Regions supplied by Workspace::region cannot overflow.
    return a.offset < b.offset + b.bytes && b.offset < a.offset + a.bytes;
}

// Validate the exact native layout before any MMIO, CPU memory store or
// firmware packet. The same regions are used by initial setup and resets.
inline Error validateLayout(const Workspace &w) {
    const Error error = w.validate();
    if (error != Error::Ok) return error;
    const uint64_t blocks[] = {kKiqBlock, kSchedulerBlock, kComputeBlock, kGraphicsBlock};
    const uint32_t sizes[] = {kKiqRingBytes, kSchedulerRingBytes, kComputeRingBytes, kGraphicsRingBytes};
    Region regions[22];
    unsigned count = 0;
    for (unsigned i = 0; i < 4; ++i) {
        Queue queue;
        const Error qe = w.queue(blocks[i], sizes[i], &queue);
        if (qe != Error::Ok) return qe;
        regions[count++] = queue.ring;
        regions[count++] = queue.rptr;
        regions[count++] = queue.wptr;
        regions[count++] = queue.mqd;
        regions[count++] = queue.eop;
    }
    Error re = w.region(kSharedBlock, 0x4000, 4096, &regions[count++]);
    if (re != Error::Ok) return re;
    re = w.region(kEventLog, kEventLogBytes, 4096, &regions[count++]);
    if (re != Error::Ok) return re;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = i + 1; j < count; ++j)
            if (overlaps(regions[i], regions[j])) return Error::OverlappingRegions;
    return Error::Ok;
}

} // namespace rn_mes
#endif
