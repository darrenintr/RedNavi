#ifndef REDNAVI_NAVI33_P7_TMR_PLAN_H
#define REDNAVI_NAVI33_P7_TMR_PLAN_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P7.2 software-only PSP TMR ownership planner for Navi33.
//
// This planner consumes the already-verified P6.6 visible-VRAM carve and the
// TMR size returned by PSP LOAD_TOC.  It deliberately does not write VRAM,
// submit SETUP_TMR, load PMFW, or mutate an allocator.
//
// Linux allocates the TMR with 1 MiB alignment and notes that a naturally
// aligned address is preferred.  We therefore choose the first address inside
// the free RedNavi visible-VRAM range that is an integer multiple of the TMR
// size itself.  The PSP-provided size must also be 1 MiB aligned.
//
// Address domains are kept explicit:
//   offset  = byte offset inside the VRAM aperture
//   mc      = GPU MC address (FB_LOCATION_BASE + offset)
//   pa      = GPU-physical VRAM address used by SETUP_TMR
//             (FB_OFFSET + offset), matching amdgpu_gmc_vram_pa() semantics.
// None of these values is a CPU virtual pointer or PCI BAR CPU physical
// address.

#include <stdint.h>

namespace rn_navi33 { namespace p7_tmr {

constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kAlignment = 1ull << 20; // PSP_TMR_ALIGNMENT
constexpr bool kHardwareWritesEnabled = false;

enum class Error : uint8_t {
    Ok = 0,
    Disabled,
    TocNotAccepted,
    NoAllocatorCarve,
    AllocatorBusy,
    BadTmrSize,
    BadGeometry,
    BadFreeRange,
    NoNaturalFit,
    OutsideVisibleVram,
    Overlap,
};

struct Range {
    uint64_t offset = 0;
    uint64_t bytes = 0;
};

struct Input {
    bool requested = false;
    bool tocAccepted = false;
    bool allocatorCarveVerified = false;
    bool allocatorQuiescent = false;

    uint64_t tmrBytes = 0;
    uint64_t cpuVisibleBytes = 0;
    uint64_t mcStart = 0;
    uint64_t vramPhysBase = 0; // MMHUB FB_OFFSET << 24

    Range freeRange{};
    Range console{};
    Range priorReservation{}; // scratch + dummy + VMID0 page table
};

struct Report {
    Error error = Error::Disabled;
    Range candidate{};
    uint64_t mc = 0;
    uint64_t pa = 0;
    bool naturalAligned = false;
    bool insideVisibleVram = false;
    bool noOverlap = false;
    bool ownershipVerified = false;
    bool mayWriteVram = false;
    bool mayIssueSetupTmr = false;
};

constexpr bool addOk(uint64_t a, uint64_t b) {
    return a <= UINT64_MAX - b;
}
constexpr uint64_t end(const Range &r) {
    return r.offset + r.bytes;
}
constexpr bool valid(const Range &r) {
    return r.bytes && addOk(r.offset, r.bytes);
}
constexpr bool overlaps(const Range &a, const Range &b) {
    return a.bytes && b.bytes &&
           a.offset < end(b) && b.offset < end(a);
}
constexpr bool aligned(uint64_t v, uint64_t a) {
    return a && !(v % a);
}
constexpr uint64_t ceilMultiple(uint64_t v, uint64_t quantum) {
    if (!quantum) return 0;
    const uint64_t rem = v % quantum;
    if (!rem) return v;
    const uint64_t add = quantum - rem;
    return addOk(v, add) ? v + add : 0;
}

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.tocAccepted) {
        r.error = Error::TocNotAccepted;
        return r;
    }
    if (!in.allocatorCarveVerified) {
        r.error = Error::NoAllocatorCarve;
        return r;
    }
    if (!in.allocatorQuiescent) {
        r.error = Error::AllocatorBusy;
        return r;
    }
    if (!in.tmrBytes || !aligned(in.tmrBytes, kAlignment)) {
        r.error = Error::BadTmrSize;
        return r;
    }
    if (!in.cpuVisibleBytes || !in.mcStart) {
        r.error = Error::BadGeometry;
        return r;
    }
    if (!valid(in.freeRange) || end(in.freeRange) > in.cpuVisibleBytes) {
        r.error = Error::BadFreeRange;
        return r;
    }

    const uint64_t start = ceilMultiple(in.freeRange.offset, in.tmrBytes);
    if (!start || !addOk(start, in.tmrBytes) ||
        start < in.freeRange.offset ||
        start + in.tmrBytes > end(in.freeRange)) {
        r.error = Error::NoNaturalFit;
        return r;
    }
    r.candidate = {start, in.tmrBytes};
    r.naturalAligned = aligned(start, in.tmrBytes) &&
                       aligned(start, kAlignment);

    if (!r.naturalAligned ||
        end(r.candidate) > in.cpuVisibleBytes) {
        r.error = Error::OutsideVisibleVram;
        return r;
    }
    r.insideVisibleVram = true;

    if (overlaps(r.candidate, in.console) ||
        overlaps(r.candidate, in.priorReservation)) {
        r.error = Error::Overlap;
        return r;
    }
    r.noOverlap = true;

    if (!addOk(in.mcStart, start) ||
        !addOk(in.vramPhysBase, start)) {
        r.error = Error::BadGeometry;
        return r;
    }
    r.mc = in.mcStart + start;
    r.pa = in.vramPhysBase + start;

    r.ownershipVerified = true;
    r.error = Error::Ok;
    return r;
}

}} // namespace rn_navi33::p7_tmr
#endif
