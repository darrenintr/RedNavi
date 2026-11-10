#ifndef REDNAVI_NAVI33_GART_OWNER_H
#define REDNAVI_NAVI33_GART_OWNER_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Navi33 / GMC11 GART ownership and placement contract.
// Host-only: computes addresses and validates caller-supplied ownership facts.
// It never allocates memory, writes page tables, programs GFXHUB/MMHUB or
// flushes a GPU TLB.
//
// Linux GC11 uses a 512 MiB GART and AMDGPU_GART_PLACEMENT_HIGH. With the
// 47-bit pre-hole MC ceiling this produces the native RX7600 reference:
//   0x7fff00000000 .. 0x7fff1fffffff
// after 4 GiB alignment.
#include <stdint.h>

namespace rn_navi33 { namespace gart_owner {

constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kFourGiB = 1ull << 32;
constexpr uint64_t kHoleStart = 1ull << 47;
constexpr uint64_t kDefaultGartBytes = 512ull << 20;
constexpr uint64_t kPteBytesPerPage = 8ull;
constexpr uint64_t kDefaultPtBytes =
    (kDefaultGartBytes / kPage) * kPteBytesPerPage; // 1 MiB
constexpr uint64_t kPspFwPriBytes = 1ull << 20;
constexpr bool kHardwareWritesEnabled = false;

enum class Error : uint8_t {
    Ok,
    BadGeometry,
    BadGartSize,
    PlacementOverlap,
    NoVramOwnership,
    NoReservationMap,
    BadPtRange,
    PtNotCpuVisible,
    BadAuxPage,
    AuxOverlap,
    NoDmaOwnership,
    BadDmaSpan,
};

struct VramRange {
    uint64_t offset = 0; // from P5 VRAM base, not CPU physical / GPU VA
    uint64_t bytes = 0;
};

struct Input {
    uint64_t fbStart = 0;       // P5 GPU MC base
    uint64_t fbBytes = 0;       // P5 conservative usable VRAM
    uint64_t cpuVisibleBytes = 0;
    uint64_t gartBytes = kDefaultGartBytes;

    VramRange pageTable{};      // caller-owned VMID0 PTE storage
    VramRange scratch{};        // caller-owned default page
    VramRange dummy{};          // caller-owned fault page
    bool vramOwnershipVerified = false;
    bool reservationMapVerified = false;

    // PSP fw_pri host backing: actual IODMACommand mapping has to remain
    // retained for the entire GPU use. The page addresses are validated
    // separately by rn_dma::collectPages.
    uint64_t dmaBytes = 0;
    uint32_t dmaPageCount = 0;
    bool dmaMappingRetained = false;
    bool dmaPagesVerified = false;
};

struct Plan {
    Error error = Error::BadGeometry;
    uint64_t gartStart = 0;
    uint64_t gartEnd = 0;
    uint64_t pageTableBytes = 0;
    uint64_t fwPriGpuAddress = 0; // GART VA, not DMA IOVA and not CPU pointer
    uint32_t fwPriFirstPage = 0;
    uint32_t fwPriPageCount = 0;
    bool ownershipVerified = false;
    bool mayProgramGart = false; // ALWAYS false in this planning stage.
};

constexpr bool aligned(uint64_t v, uint64_t a) {
    return a && !(a & (a - 1u)) && !(v & (a - 1u));
}
constexpr bool validRange(uint64_t off, uint64_t bytes, uint64_t limit) {
    return bytes && off <= limit && bytes <= limit - off;
}
constexpr bool overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) {
    return a < b + bn && b < a + an;
}

inline uint64_t linuxHighGartStart(uint64_t gartBytes) {
    if (!gartBytes || gartBytes > kHoleStart) return 0;
    const uint64_t maxMc = kHoleStart - 1u;
    const uint64_t raw = maxMc - gartBytes + 1u;
    return raw & ~(kFourGiB - 1u);
}

inline Plan inspect(const Input &in) {
    Plan p{};
    if (!in.fbStart || !in.fbBytes || in.cpuVisibleBytes > in.fbBytes ||
        in.fbStart > UINT64_MAX - (in.fbBytes - 1u)) {
        return p;
    }
    if (!in.gartBytes || !aligned(in.gartBytes, kPage) ||
        in.gartBytes > kHoleStart) {
        p.error = Error::BadGartSize; return p;
    }

    p.gartStart = linuxHighGartStart(in.gartBytes);
    if (!p.gartStart || in.gartBytes > UINT64_MAX - p.gartStart) {
        p.error = Error::BadGartSize; return p;
    }
    p.gartEnd = p.gartStart + in.gartBytes - 1u;

    const uint64_t fbEnd = in.fbStart + in.fbBytes - 1u;
    if (!(p.gartEnd < in.fbStart || p.gartStart > fbEnd)) {
        p.error = Error::PlacementOverlap; return p;
    }

    const uint64_t pages = in.gartBytes / kPage;
    if (pages > UINT64_MAX / kPteBytesPerPage) {
        p.error = Error::BadGartSize; return p;
    }
    p.pageTableBytes = pages * kPteBytesPerPage;

    if (!in.vramOwnershipVerified) {
        p.error = Error::NoVramOwnership; return p;
    }
    if (!in.reservationMapVerified) {
        p.error = Error::NoReservationMap; return p;
    }

    if (!aligned(in.pageTable.offset, kPage) ||
        in.pageTable.bytes != p.pageTableBytes ||
        !validRange(in.pageTable.offset, in.pageTable.bytes, in.fbBytes)) {
        p.error = Error::BadPtRange; return p;
    }
    // RedNavi's first native owner will populate PTEs from the CPU, so keep
    // the table inside the independently proven BAR0-visible span.
    if (!validRange(in.pageTable.offset, in.pageTable.bytes,
                    in.cpuVisibleBytes)) {
        p.error = Error::PtNotCpuVisible; return p;
    }

    const VramRange aux[2] = {in.scratch, in.dummy};
    for (unsigned i = 0; i < 2; ++i) {
        if (!aligned(aux[i].offset, kPage) || aux[i].bytes != kPage ||
            !validRange(aux[i].offset, aux[i].bytes, in.fbBytes)) {
            p.error = Error::BadAuxPage; return p;
        }
        if (overlaps(aux[i].offset, aux[i].bytes,
                     in.pageTable.offset, in.pageTable.bytes)) {
            p.error = Error::AuxOverlap; return p;
        }
    }
    if (overlaps(in.scratch.offset, in.scratch.bytes,
                 in.dummy.offset, in.dummy.bytes)) {
        p.error = Error::AuxOverlap; return p;
    }

    if (!in.dmaMappingRetained || !in.dmaPagesVerified) {
        p.error = Error::NoDmaOwnership; return p;
    }
    if (in.dmaBytes != kPspFwPriBytes ||
        in.dmaPageCount != kPspFwPriBytes / kPage ||
        in.dmaBytes > in.gartBytes) {
        p.error = Error::BadDmaSpan; return p;
    }

    p.fwPriGpuAddress = p.gartStart;
    p.fwPriFirstPage = 0;
    p.fwPriPageCount = in.dmaPageCount;
    p.ownershipVerified = true;
    p.error = Error::Ok;
    return p;
}

}} // namespace rn_navi33::gart_owner
#endif
