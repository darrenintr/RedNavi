#ifndef REDNAVI_NAVI33_VRAM_LEDGER_H
#define REDNAVI_NAVI33_VRAM_LEDGER_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P6.5 software-only VRAM reservation ledger.
// "Owned" here means RedNavi has exclusively claimed these offsets in its own
// runtime bookkeeping after checking the active boot framebuffer. It does NOT
// mean the bytes were touched, zeroed, mapped by the GPU, or proven unused by
// every firmware agent.
//
// No VRAM read/write, PTE write, GART programming or PSP command occurs here.
#include <stdint.h>

namespace rn_navi33 { namespace vram_ledger {

constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kPageTableBytes = 1ull << 20;
constexpr bool kHardwareWritesEnabled = false;

enum class Error : uint8_t {
    Ok,
    Disabled,
    BadGeometry,
    ConsoleUnverified,
    AllocatorActive,
    BadCandidate,
    Overlap,
};

struct Range {
    uint64_t offset = 0;
    uint64_t bytes = 0;
};

struct Input {
    bool requested = false;
    bool geometryVerified = false;
    bool consoleVerified = false;
    bool allocatorInactive = false;
    uint64_t cpuVisibleBytes = 0;
    Range console{};
    Range pageTable{};
    Range scratch{};
    Range dummy{};
};

struct Report {
    Error error = Error::Disabled;
    uint64_t reservedBytes = 0;
    bool ledgerOwned = false;
    bool allocatorCarveRequired = false;
    bool mayWriteVram = false;
    bool mayInstallGartPtes = false;
};

constexpr bool aligned(uint64_t v, uint64_t a) {
    return a && !(a & (a - 1u));
}
constexpr bool validRange(const Range &r, uint64_t limit) {
    return r.bytes && r.offset <= limit && r.bytes <= limit - r.offset;
}
constexpr bool overlaps(const Range &a, const Range &b) {
    return a.bytes && b.bytes &&
           a.offset < b.offset + b.bytes &&
           b.offset < a.offset + a.bytes;
}

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.geometryVerified || !in.cpuVisibleBytes) {
        r.error = Error::BadGeometry;
        return r;
    }
    if (!in.consoleVerified || !validRange(in.console, in.cpuVisibleBytes)) {
        r.error = Error::ConsoleUnverified;
        return r;
    }
    if (!in.allocatorInactive) {
        r.error = Error::AllocatorActive;
        return r;
    }
    if (!aligned(in.pageTable.offset, kPageTableBytes) ||
        in.pageTable.bytes != kPageTableBytes ||
        !aligned(in.scratch.offset, kPage) ||
        !aligned(in.dummy.offset, kPage) ||
        in.scratch.bytes != kPage || in.dummy.bytes != kPage ||
        !validRange(in.pageTable, in.cpuVisibleBytes) ||
        !validRange(in.scratch, in.cpuVisibleBytes) ||
        !validRange(in.dummy, in.cpuVisibleBytes) ||
        in.scratch.offset + kPage != in.dummy.offset ||
        in.dummy.offset + kPage != in.pageTable.offset) {
        r.error = Error::BadCandidate;
        return r;
    }
    if (overlaps(in.console, in.pageTable) ||
        overlaps(in.console, in.scratch) ||
        overlaps(in.console, in.dummy) ||
        overlaps(in.pageTable, in.scratch) ||
        overlaps(in.pageTable, in.dummy) ||
        overlaps(in.scratch, in.dummy)) {
        r.error = Error::Overlap;
        return r;
    }

    r.error = Error::Ok;
    r.reservedBytes = kPageTableBytes + 2u * kPage;
    r.ledgerOwned = true;
    // The future visible-VRAM allocator currently treats the top part of the
    // 256 MiB aperture as free. Integration MUST carve this owned range out
    // before that allocator becomes active.
    r.allocatorCarveRequired = true;
    return r;
}

}} // namespace rn_navi33::vram_ledger
#endif
