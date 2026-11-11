#ifndef REDNAVI_NAVI33_P713_STAGE_EXPAND_H
#define REDNAVI_NAVI33_P713_STAGE_EXPAND_H
// SPDX-License-Identifier: BSD-3-Clause
// P7.13: pure software plan for MEC instruction staging expansion.
// Required because GC11.0.2 RS64 MEC code is 267008B (66 4KiB pages),
// two pages larger than the current 262144B PMFW stage.
// This policy never allocates, maps, invalidates, or submits any PSP command.
#include <stdint.h>

namespace rn_navi33 { namespace p713_stage_expand {
constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kMaxStageBytes = 1ull << 20;
constexpr uint32_t kFirstPage = 259u;
constexpr uint32_t kReviewedMecCodeBytes = 267008u;
constexpr uint64_t pageAlign(uint64_t n) {
    return (n + (kPage - 1u)) & ~(kPage - 1u);
}

enum class Error : uint8_t {
    Ok = 0, Disabled, NoExplicitOptIn, IdentityUnverified,
    FirmwareUnverified, NoGartOwner, BadFirmwareSize,
    BadPmfwSize, StageTooLarge, InvalidGartRange,
    NoExpansionNeeded
};

struct Input {
    bool requested = false;
    bool explicitOptIn = false;
    bool identityVerified = false;
    bool firmwareVerified = false;
    bool gartOwned = false;
    uint32_t firmwareBytes = 0;
    uint32_t pmfwBytes = 0;
    uint64_t gartStart = 0;
    uint64_t gartEnd = 0;
};

struct Report {
    Error error = Error::Disabled;
    bool mayExpand = false;
    uint64_t defaultStageBytes = 0;
    uint64_t targetStageBytes = 0;
    uint32_t targetPages = 0;
};

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.explicitOptIn) {
        r.error = Error::NoExplicitOptIn; return r;
    }
    if (!in.identityVerified) {
        r.error = Error::IdentityUnverified; return r;
    }
    if (!in.firmwareVerified) {
        r.error = Error::FirmwareUnverified; return r;
    }
    if (!in.gartOwned) {
        r.error = Error::NoGartOwner; return r;
    }
    if (in.firmwareBytes != kReviewedMecCodeBytes) {
        r.error = Error::BadFirmwareSize; return r;
    }
    if (!in.pmfwBytes || (in.pmfwBytes & 3u)) {
        r.error = Error::BadPmfwSize; return r;
    }
    r.defaultStageBytes = pageAlign(in.pmfwBytes);
    const uint64_t maximum = in.pmfwBytes > in.firmwareBytes ?
        in.pmfwBytes : in.firmwareBytes;
    r.targetStageBytes = pageAlign(maximum);
    if (r.targetStageBytes > kMaxStageBytes) {
        r.error = Error::StageTooLarge; return r;
    }
    if (r.targetStageBytes <= r.defaultStageBytes) {
        r.error = Error::NoExpansionNeeded; return r;
    }
    // The stage begins at GART PTE index 259. Enforce the
    // complete span is inside GART and cannot overlap fw_pri
    // (indices 0..255) or runtime ring/cmd/fence (256..258).
    if (in.gartEnd < in.gartStart ||
        (in.gartStart & (kPage-1u)) ||
        ((in.gartEnd-in.gartStart+1u) & (kPage-1u)) ||
        in.gartEnd-in.gartStart+1u <
            (static_cast<uint64_t>(kFirstPage)*kPage +
             r.targetStageBytes)) {
        r.error = Error::InvalidGartRange; return r;
    }
    r.targetPages = static_cast<uint32_t>(r.targetStageBytes / kPage);
    r.mayExpand = true;
    r.error = Error::Ok;
    return r;
}
}} // namespace rn_navi33::p713_stage_expand
#endif
