#ifndef REDNAVI_NAVI33_P74_SETUP_TMR_H
#define REDNAVI_NAVI33_P74_SETUP_TMR_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Navi33 P7.4 SETUP_TMR admission and Linux PSP13 command-word encoding.
// Pure host-testable policy: inspect() never reads registers or writes VRAM.
// STEP 6 is a dangerous GPU-write experiment: require two independent
// boot-arg opt-ins and a freshly completed Step-5 PMFW command.

#include <stdint.h>

namespace rn_navi33 { namespace p74_tmr {

constexpr uint32_t kCommandId = 0x05u; // GFX_CMD_ID_SETUP_TMR
constexpr uint32_t kArgCount = 6u;
constexpr uint32_t kVirtPhyAddr = 1u << 1; // bit 1, NOT bit 0 (SR-IOV)
constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kAlignment = 1ull << 20;
constexpr uint32_t kExpectedPriorFence = 2u;

enum class Error : uint8_t {
    Ok = 0,
    Disabled,
    NoExplicitOptIn,
    IdentityUnverified,
    PspNotReady,
    PostPspNotAdmitted,
    PmfwNotAccepted,
    TransportUnhealthy,
    PreviousFenceMismatch,
    NoTmrOwnership,
    AllocatorNotQuiescent,
    CarveNotVerified,
    BadTocSize,
    BadTmrSize,
    BadAlignment,
    BadGeometry,
    OutsideVisibleVram,
    OutsideCarvedRegion,
    Overlap,
};

struct Input {
    bool requested = false;
    bool explicitOptIn = false;
    bool identityVerified = false;
    bool pspReady = false;
    bool postPspMaySetupTmr = false;
    bool pmfwAccepted = false;
    bool transportHealthy = false;
    uint32_t previousFenceExpected = 0;
    uint32_t previousFenceObserved = 0;
    bool tmrOwnershipVerified = false;
    bool allocatorQuiescent = false;
    bool carveVerified = false;
    bool naturalAligned = false;
    bool noOverlap = false;
    uint64_t tocBytes = 0;
    uint64_t tmrBytes = 0;
    uint64_t tmrOffset = 0;
    uint64_t mc = 0;
    uint64_t pa = 0;
    uint64_t mcStart = 0;
    uint64_t physBase = 0;
    uint64_t visibleBytes = 0;
    uint64_t carveStart = 0;
    uint64_t carveBytes = 0;
};

struct Report {
    Error error = Error::Disabled;
    bool mayIssue = false;
    // psp_gfx_cmd_setup_tmr union at byte offset 28 of the command buffer.
    // [mc_lo, mc_hi, bytes, flags, physical_lo, physical_hi]
    uint32_t words[kArgCount]{};
};

constexpr bool addOk(uint64_t a, uint64_t b) {
    return a <= UINT64_MAX - b;
}
constexpr bool aligned(uint64_t value, uint64_t size) {
    return size && value % size == 0;
}
constexpr bool inside(uint64_t start, uint64_t size,
                      uint64_t regionStart, uint64_t regionSize) {
    return size && regionSize &&
           addOk(start, size) && addOk(regionStart, regionSize) &&
           start >= regionStart && start + size <= regionStart + regionSize;
}

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.explicitOptIn) {
        r.error = Error::NoExplicitOptIn; return r;
    }
    if (!in.identityVerified) {
        r.error = Error::IdentityUnverified; return r;
    }
    if (!in.pspReady) {
        r.error = Error::PspNotReady; return r;
    }
    if (!in.postPspMaySetupTmr) {
        r.error = Error::PostPspNotAdmitted; return r;
    }
    if (!in.pmfwAccepted) {
        r.error = Error::PmfwNotAccepted; return r;
    }
    if (!in.transportHealthy) {
        r.error = Error::TransportUnhealthy; return r;
    }
    if (in.previousFenceExpected != kExpectedPriorFence ||
        in.previousFenceObserved != kExpectedPriorFence) {
        r.error = Error::PreviousFenceMismatch; return r;
    }
    if (!in.tmrOwnershipVerified) {
        r.error = Error::NoTmrOwnership; return r;
    }
    if (!in.allocatorQuiescent) {
        r.error = Error::AllocatorNotQuiescent; return r;
    }
    if (!in.carveVerified) {
        r.error = Error::CarveNotVerified; return r;
    }
    if (in.tocBytes != in.tmrBytes) {
        r.error = Error::BadTocSize; return r;
    }
    if (!in.tmrBytes || in.tmrBytes > UINT32_MAX ||
        !aligned(in.tmrBytes, kAlignment)) {
        r.error = Error::BadTmrSize; return r;
    }
    if (!in.naturalAligned || !aligned(in.tmrOffset, in.tmrBytes) ||
        !aligned(in.tmrOffset, kAlignment) ||
        !aligned(in.mc, kPage) || !aligned(in.pa, kPage)) {
        r.error = Error::BadAlignment; return r;
    }
    if (!in.mcStart || !addOk(in.mcStart, in.tmrOffset) ||
        !addOk(in.physBase, in.tmrOffset) ||
        in.mc != in.mcStart + in.tmrOffset ||
        in.pa != in.physBase + in.tmrOffset) {
        r.error = Error::BadGeometry; return r;
    }
    if (!inside(in.tmrOffset, in.tmrBytes, 0, in.visibleBytes)) {
        r.error = Error::OutsideVisibleVram; return r;
    }
    if (!inside(in.tmrOffset, in.tmrBytes,
                in.carveStart, in.carveBytes)) {
        r.error = Error::OutsideCarvedRegion; return r;
    }
    if (!in.noOverlap) {
        r.error = Error::Overlap; return r;
    }
    r.words[0] = static_cast<uint32_t>(in.mc);
    r.words[1] = static_cast<uint32_t>(in.mc >> 32);
    r.words[2] = static_cast<uint32_t>(in.tmrBytes);
    r.words[3] = kVirtPhyAddr;
    r.words[4] = static_cast<uint32_t>(in.pa);
    r.words[5] = static_cast<uint32_t>(in.pa >> 32);
    r.error = Error::Ok;
    r.mayIssue = true;
    return r;
}

}} // namespace rn_navi33::p74_tmr
#endif
