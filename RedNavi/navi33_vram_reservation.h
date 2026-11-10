#ifndef REDNAVI_NAVI33_VRAM_RESERVATION_H
#define REDNAVI_NAVI33_VRAM_RESERVATION_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Read-only Navi33 VRAM reservation candidate planner.
// It converts the current boot console's CPU physical BAR0 address into a
// VRAM aperture offset and proposes VMID0/GART bookkeeping storage near the
// top of the independently proven CPU-visible aperture.
//
// IMPORTANT: a non-overlapping candidate is NOT ownership. This header never
// reserves memory and never authorizes a write.
#include <stdint.h>

namespace rn_navi33 { namespace vram_reservation {

constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kPageTableBytes = 1ull << 20;
constexpr uint64_t kAuxBytes = kPage;
constexpr bool kMayWriteVram = false;

enum class Error : uint8_t {
    Ok,
    BadBar,
    BadConsole,
    ConsoleOutsideBar,
    ConsoleOverflow,
    TooLittleVisibleVram,
    CandidateOverlap,
};

struct Input {
    uint64_t barPhys = 0;
    uint64_t barBytes = 0;
    uint64_t cpuVisibleBytes = 0;

    uint64_t consolePhys = 0;
    uint64_t consoleRowBytes = 0;
    uint32_t consoleHeight = 0;
};

struct Range {
    uint64_t offset = 0; // VRAM offset from BAR0 aperture base
    uint64_t bytes = 0;
};

struct Plan {
    Error error = Error::BadBar;
    Range console{};
    Range pageTable{};
    Range scratch{};
    Range dummy{};
    uint64_t consoleBytes = 0;
    bool consoleInsideBar = false;
    bool candidateNonOverlapping = false;
    bool reservationOwned = false; // ALWAYS false in this planner.
    bool mayWriteVram = false;     // ALWAYS false.
};

constexpr uint64_t alignDown(uint64_t v, uint64_t a) {
    return a ? (v & ~(a - 1u)) : 0;
}
constexpr bool validRange(uint64_t off, uint64_t bytes, uint64_t limit) {
    return bytes && off <= limit && bytes <= limit - off;
}
constexpr bool overlaps(const Range &a, const Range &b) {
    return a.bytes && b.bytes && a.offset < b.offset + b.bytes &&
           b.offset < a.offset + a.bytes;
}

inline Plan inspect(const Input &in) {
    Plan p{};
    if (!in.barPhys || !in.barBytes || !in.cpuVisibleBytes ||
        in.cpuVisibleBytes > in.barBytes) {
        return p;
    }
    if (!in.consolePhys || !in.consoleRowBytes || !in.consoleHeight) {
        p.error = Error::BadConsole;
        return p;
    }
    if (in.consoleRowBytes > UINT64_MAX / in.consoleHeight) {
        p.error = Error::ConsoleOverflow;
        return p;
    }

    // IONDRVFramebuffer::getApertureRange() adds 128 bytes to
    // bytesPerRow * activeHeight. Mirror that exact safety tail.
    const uint64_t pixelsBytes = in.consoleRowBytes * in.consoleHeight;
    if (pixelsBytes > UINT64_MAX - 128u) {
        p.error = Error::ConsoleOverflow;
        return p;
    }
    p.consoleBytes = pixelsBytes + 128u;

    if (in.consolePhys < in.barPhys) {
        p.error = Error::ConsoleOutsideBar;
        return p;
    }
    p.console.offset = in.consolePhys - in.barPhys;
    p.console.bytes = p.consoleBytes;
    if (!validRange(p.console.offset, p.console.bytes, in.barBytes)) {
        p.error = Error::ConsoleOutsideBar;
        return p;
    }
    p.consoleInsideBar = true;

    // Put the 1 MiB table at the highest 1 MiB-aligned location in the
    // CPU-visible aperture. Scratch/dummy occupy the two pages immediately
    // below it. This is only a candidate; ownership is not inferred.
    if (in.cpuVisibleBytes < kPageTableBytes + 2u * kPage) {
        p.error = Error::TooLittleVisibleVram;
        return p;
    }
    const uint64_t pt = alignDown(in.cpuVisibleBytes - kPageTableBytes,
                                  kPageTableBytes);
    if (pt < 2u * kPage) {
        p.error = Error::TooLittleVisibleVram;
        return p;
    }
    p.pageTable = {pt, kPageTableBytes};
    p.scratch = {pt - 2u * kPage, kPage};
    p.dummy = {pt - kPage, kPage};

    if (!validRange(p.pageTable.offset, p.pageTable.bytes,
                    in.cpuVisibleBytes) ||
        !validRange(p.scratch.offset, p.scratch.bytes,
                    in.cpuVisibleBytes) ||
        !validRange(p.dummy.offset, p.dummy.bytes,
                    in.cpuVisibleBytes)) {
        p.error = Error::TooLittleVisibleVram;
        return p;
    }

    if (overlaps(p.console, p.pageTable) ||
        overlaps(p.console, p.scratch) ||
        overlaps(p.console, p.dummy)) {
        p.error = Error::CandidateOverlap;
        return p;
    }

    p.candidateNonOverlapping = true;
    p.error = Error::Ok;
    return p;
}

}} // namespace rn_navi33::vram_reservation
#endif
