// SPDX-License-Identifier: BSD-3-Clause
// Exercises the DMA segment validator used before publishing GPU PTEs.
#include "rn_dma_pages.h"
#include <assert.h>
#include <stdint.h>

namespace {
struct Segment { uint64_t fIOVMAddr, fLength; };
using rn_dma::Error;
constexpr uint64_t page = rn_dma::kPageBytes;
constexpr uint64_t sentinel = 0xfeedfacefeedfaceull;

void validLists() {
    uint64_t out[6] = {sentinel, sentinel, sentinel, sentinel, sentinel, sentinel};
    unsigned calls = 0;
    auto result = rn_dma::collectPages<Segment>(out, 5, 5 * page,
        [&](uint64_t *offset, Segment *segments, uint32_t *count) {
            assert(*count == rn_dma::kSegmentBatch);
            if (calls++ == 0) {
                assert(*offset == 0);
                segments[0] = {0x100000, 2 * page};
                segments[1] = {0x900000, page};
                *count = 2;
                *offset = 3 * page;
            } else {
                assert(*offset == 3 * page);
                segments[0] = {0, 2 * page}; // IOVA zero is valid.
                *count = 1;
                *offset = 5 * page;
            }
            return true;
        });
    assert(result.ok() && result.pageCount == 5 && result.bytes == 5 * page);
    assert(calls == 2);
    assert(out[0] == 0x100000 && out[1] == 0x101000 && out[2] == 0x900000);
    assert(out[3] == 0 && out[4] == page && out[5] == sentinel);
}

void rejectedRequests() {
    unsigned calls = 0;
    auto fetch = [&](uint64_t *, Segment *, uint32_t *) { ++calls; return false; };
    uint64_t out[2] = {sentinel, sentinel};
    assert(rn_dma::collectPages<Segment>(nullptr, 1, page, fetch).error == Error::InvalidRequest);
    assert(rn_dma::collectPages<Segment>(out, 1, 0, fetch).error == Error::InvalidRequest);
    assert(rn_dma::collectPages<Segment>(out, 1, page + 1, fetch).error == Error::InvalidRequest);
    assert(rn_dma::collectPages<Segment>(out, 1, 2 * page, fetch).error == Error::InvalidRequest);
    assert(rn_dma::collectPages<Segment>(out, 1, page, fetch, 0).error == Error::InvalidRequest);
    assert(rn_dma::collectPages<Segment>(out, 1, page, fetch, 65).error == Error::InvalidRequest);
    assert(calls == 0 && out[0] == sentinel && out[1] == sentinel);
}

rn_dma::Result oneSegment(uint64_t address, uint64_t length, uint64_t bytes,
                          uint64_t offset, uint32_t addressBits = 44) {
    uint64_t out[3] = {sentinel, sentinel, sentinel};
    auto result = rn_dma::collectPages<Segment>(out, 2, bytes,
        [&](uint64_t *cursor, Segment *segments, uint32_t *count) {
            segments[0] = {address, length};
            *count = 1;
            *cursor = offset;
            return true;
        }, addressBits);
    if (!result.ok()) assert(result.pageCount == 0 && result.bytes == 0 && out[0] == 0);
    assert(out[2] == sentinel);
    return result;
}

void malformedSegments() {
    assert(oneSegment(0x100001, page, page, page).error == Error::BadAlignment);
    assert(oneSegment(0x100000, 0, page, page).error == Error::InvalidLength);
    assert(oneSegment(0x100000, page + 1, 2 * page, 2 * page).error == Error::InvalidLength);
    assert(oneSegment(0x100000, 2 * page, page, page).error == Error::TooManyBytes);
    assert(oneSegment(0x100000, page, page, 0).error == Error::NoProgress);
    assert(oneSegment(0x100000, page, page, 2 * page).error == Error::OffsetMismatch);
    assert(oneSegment(0x100000, page, 2 * page, 2 * page).error == Error::OffsetMismatch);
}

void addressLimits() {
    const uint64_t top44 = uint64_t(1) << 44;
    assert(oneSegment(top44 - page, page, page, page).ok()); // last allowed byte
    assert(oneSegment(top44, page, page, page).error == Error::AddressRange);
    assert(oneSegment(top44 - page, 2 * page, 2 * page, 2 * page).error == Error::AddressRange);
    // The width is explicit, rather than truncating a 64-bit segment to 44 bits.
    assert(oneSegment(top44, page, page, page, 48).ok());
    assert(oneSegment(UINT64_MAX - page + 1, page, page, page, 64).ok());
    assert(oneSegment(UINT64_MAX - page + 1, 2 * page, 2 * page, 2 * page, 64).error == Error::AddressRange);
}

void failedProvider() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        uint64_t out[3] = {sentinel, sentinel, sentinel};
        unsigned calls = 0;
        unsigned published = 0;
        auto result = rn_dma::collectPages<Segment>(out, 2, 2 * page,
            [&](uint64_t *offset, Segment *segments, uint32_t *count) {
                if (calls++ == 0) {
                    segments[0] = {0x100000, page};
                    *count = 1;
                    *offset = page;
                    return true;
                }
                if (mode == 0) return false;          // late IOKit failure
                if (mode == 1) { *count = 0; return true; } // short mapping
                if (mode == 2) { *count = rn_dma::kSegmentBatch + 1; return true; }
                segments[0] = {0x200000, page};
                *count = 1;
                if (mode == 3) return true;           // provider made no progress
                *offset = 0;                         // provider moved backwards
                return true;
            });
        if (result.ok()) published += result.pageCount;
        assert(!result.ok() && published == 0 && calls == 2);
        assert(result.pageCount == 0 && result.bytes == 0);
        assert(out[0] == 0 && out[1] == 0 && out[2] == sentinel);
        assert(result.error == (mode == 0 ? Error::ProviderError :
               mode < 3 ? Error::InvalidSegmentCount : Error::NoProgress));
    }

    // A malformed final segment must also invalidate earlier valid entries from
    // the SAME batch: callers cannot leave partial live GART mappings behind.
    uint64_t out[3] = {sentinel, sentinel, sentinel};
    auto result = rn_dma::collectPages<Segment>(out, 2, 2 * page,
        [&](uint64_t *offset, Segment *segments, uint32_t *count) {
            segments[0] = {0x100000, page};
            segments[1] = {uint64_t(1) << 44, page};
            *count = 2;
            *offset = 2 * page;
            return true;
        });
    assert(result.error == Error::AddressRange && result.pageCount == 0);
    assert(out[0] == 0 && out[1] == 0 && out[2] == sentinel);
}
}

int main() {
    validLists();
    rejectedRequests();
    malformedSegments();
    addressLimits();
    failedProvider();
}
