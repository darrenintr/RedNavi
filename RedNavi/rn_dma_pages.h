#ifndef REDNAVI_RN_DMA_PAGES_H
#define REDNAVI_RN_DMA_PAGES_H
// SPDX-License-Identifier: BSD-3-Clause

#include <stdint.h>

// IOKit DMA segments describe device-visible I/O addresses. They are not CPU
// pointers, CPU physical addresses, GPU virtual addresses, or VRAM offsets.
// Validate the complete list before a caller installs any GPU page-table entry.
namespace rn_dma {

constexpr uint64_t kPageBytes = 4096;
constexpr uint32_t kSegmentBatch = 8;
constexpr uint32_t kGmc11DmaAddressBits = 44;

enum class Error : uint8_t {
    Ok,
    InvalidRequest,
    ProviderError,
    InvalidSegmentCount,
    NoProgress,
    OffsetMismatch,
    BadAlignment,
    InvalidLength,
    AddressRange,
    TooManyBytes,
};

struct Result {
    Error error;
    uint32_t pageCount;
    uint64_t bytes;
    bool ok() const { return error == Error::Ok; }
};

// Segment must expose fIOVMAddr and fLength, as IODMACommand::Segment64 does.
// fetch(offset, segments, count) returns true on success. offset is the byte
// position before/after the batch; count is capacity in and segment count out.
// This matches gen64IOVMSegments's contract without depending on IOKit headers.
//
// No PTEs are written here. On failure, any entries populated by this operation
// are cleared and pageCount/bytes are zero. The caller MUST check ok() before
// publishing the list and retain the DMA mapping for its entire GPU lifetime.
// A zero I/O address is allowed; zero alone is not an invalid-address marker.
template <typename Segment, typename Fetch>
Result collectPages(uint64_t *pages, uint32_t pageCapacity, uint64_t bytes,
                    Fetch fetch, uint32_t addressBits = kGmc11DmaAddressBits) {
    if (!pages || !bytes || (bytes & (kPageBytes - 1)) ||
        bytes / kPageBytes > pageCapacity || !addressBits || addressBits > 64)
        return {Error::InvalidRequest, 0, 0};

    const uint32_t wanted = static_cast<uint32_t>(bytes / kPageBytes);
    const uint64_t mask = addressBits == 64 ? UINT64_MAX : (uint64_t(1) << addressBits) - 1;
    for (uint32_t i = 0; i < wanted; ++i) pages[i] = 0;

    uint32_t filled = 0;
    uint64_t covered = 0;
    uint64_t offset = 0;
    auto fail = [&](Error error) -> Result {
        for (uint32_t i = 0; i < filled; ++i) pages[i] = 0;
        return {error, 0, 0};
    };

    while (covered < bytes) {
        Segment segments[kSegmentBatch] = {};
        uint32_t count = kSegmentBatch;
        const uint64_t before = offset;
        if (!fetch(&offset, segments, &count)) return fail(Error::ProviderError);
        if (!count || count > kSegmentBatch) return fail(Error::InvalidSegmentCount);
        if (offset <= before) return fail(Error::NoProgress);
        if (offset > bytes) return fail(Error::OffsetMismatch);

        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t address = segments[i].fIOVMAddr;
            const uint64_t length = segments[i].fLength;
            if (address & (kPageBytes - 1)) return fail(Error::BadAlignment);
            if (!length || (length & (kPageBytes - 1))) return fail(Error::InvalidLength);
            if (length > bytes - covered) return fail(Error::TooManyBytes);
            // Check the final byte before adding: both uint64_t wrapping and
            // truncation by a narrower GPU DMA/PTE address field are rejected.
            if (address > mask || length - 1 > mask - address)
                return fail(Error::AddressRange);

            for (uint64_t page = 0; page < length / kPageBytes; ++page)
                pages[filled++] = address + page * kPageBytes;
            covered += length;
        }
        if (offset != covered) return fail(Error::OffsetMismatch);
    }

    return {Error::Ok, filled, covered};
}

} // namespace rn_dma
#endif
