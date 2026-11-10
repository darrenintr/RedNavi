#ifndef REDNAVI_NAVI33_FW_PRI_DMA_OWNER_H
#define REDNAVI_NAVI33_FW_PRI_DMA_OWNER_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Policy contract for the Navi33 PSP13 fw_pri host allocation.
// This does not allocate/map memory. It only validates facts reported by the
// IOKit owner. A verified DMA owner is still NOT a GPU/GART mapping.
#include <stdint.h>

namespace rn_navi33 { namespace fw_pri_owner {

constexpr uint64_t kBytes = 1ull << 20;
constexpr uint32_t kPages = static_cast<uint32_t>(kBytes / 4096ull);
constexpr uint32_t kDmaAddressBits = 44u;
constexpr bool kHardwareWritesEnabled = false;

enum class Error : uint8_t {
    Ok,
    Disabled,
    NoBuffer,
    DmaNotPrepared,
    PagesNotVerified,
    BadLength,
    BadPageCount,
};

struct Input {
    bool enabled = false;
    bool bufferAllocated = false;
    bool dmaPrepared = false;
    bool pagesVerified = false;
    uint64_t bytes = 0;
    uint32_t pageCount = 0;
};

struct Report {
    Error error = Error::Disabled;
    bool ownerVerified = false;
    bool mayInstallGartPtes = false;
};

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.enabled) return r;
    if (!in.bufferAllocated) {
        r.error = Error::NoBuffer; return r;
    }
    if (!in.dmaPrepared) {
        r.error = Error::DmaNotPrepared; return r;
    }
    if (!in.pagesVerified) {
        r.error = Error::PagesNotVerified; return r;
    }
    if (in.bytes != kBytes) {
        r.error = Error::BadLength; return r;
    }
    if (in.pageCount != kPages) {
        r.error = Error::BadPageCount; return r;
    }
    r.error = Error::Ok;
    r.ownerVerified = true;
    return r;
}

}} // namespace rn_navi33::fw_pri_owner
#endif
