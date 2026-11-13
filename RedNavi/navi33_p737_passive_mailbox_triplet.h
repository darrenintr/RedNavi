#ifndef REDNAVI_NAVI33_P737_PASSIVE_MAILBOX_TRIPLET_H
#define REDNAVI_NAVI33_P737_PASSIVE_MAILBOX_TRIPLET_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P7.37: OPTIONAL, BOUNDED, host-direct MMIO READ-ONLY evidence.
// Requires a same-tick P7.36 C2PMSG_90 response read; performs exactly
// two ADDITIONAL mapped BAR5 dword reads (C2PMSG_66 / C2PMSG_82).
//
// This is NOT an atomic three-register snapshot. The response was read
// *before* these two registers. A changed message/parameter/response is
// not evidence of which actor wrote the register or whether a new
// firmware transaction completed. No write/queue/PSP/SMU API here.
//
// Linux smu_cmn.c uses RREG32 to poll response, read SMU out-args,
// and inspect msg/arg in error diagnostics; that is a reason to test,
// not a guarantee of harmless reads in every device power state.

#include <stdint.h>
#include "navi33_smu_mailbox_roundtrip.h"

namespace rn_navi33 { namespace p737_passive_mailbox_triplet {

constexpr uint32_t kInvalid = 0xffffffffu;
constexpr uint32_t kMaxTripletsPerService = 2u;
constexpr uint32_t kReadsPerTriplet = 2u;
constexpr uint32_t kMaxAdditionalDwordReads = 4u;

enum class Status : uint8_t {
    Disabled = 0,
    RequiresP736OptIn,
    WrongAsic,
    WrongMp1Revision,
    NoBarMapping,
    InvalidDirectOffsets,
    Armed,
    Stopped,
    InactiveService,
    MappingLost,
    MissingPairedResponse,
    ServiceBudgetExhausted,
    ImageBudgetExhausted,
    Sampled
};

enum class Evidence : uint8_t {
    Unobserved = 0,
    FirstSnapshot,
    StableBetweenSamples,
    UnattributedRegisterChange
};

struct Config {
    bool explicitOptIn = false;
    bool p736ResponseArmed = false;
    bool exactNavi33 = false;
    bool mp1Is13_0_7 = false;
    bool directBar5MappingReady = false;
    uint32_t mp1BaseDword = 0u;
    uint64_t bar5MappedBytes = 0ull;
};

struct State {
    bool requested = false;
    bool armed = false;
    bool stopped = false;
    uint32_t messageByteOffset = kInvalid;
    uint32_t parameterByteOffset = kInvalid;
    uint32_t responseByteOffset = kInvalid;
    uint32_t samples = 0u;
    uint32_t lastTick = 0u;
    uint32_t lastMessage = kInvalid;
    uint32_t lastParameter = kInvalid;
    uint32_t lastResponse = kInvalid;
};

struct ImageBudget {
    // CAS across every service in the loaded Kext image. Never reset
    // during stop(), re-probe or IOService::start().
    alignas(uint32_t) uint32_t claimedDwordReads = 0u;
};

struct Report {
    Status status = Status::Disabled;
    Evidence evidence = Evidence::Unobserved;
    bool readPerformed = false;
    bool pairedResponseFromThisTick = false;
    bool previousTripletAvailable = false;
    bool messageChanged = false;
    bool parameterChanged = false;
    bool responseChanged = false;
    uint32_t message = kInvalid;
    uint32_t parameter = kInvalid;
    uint32_t response = kInvalid;
    uint32_t messageByteOffset = kInvalid;
    uint32_t parameterByteOffset = kInvalid;
    uint32_t responseByteOffset = kInvalid;
    uint32_t serviceTriplets = 0u;
    uint32_t additionalReadsThisSample = 0u;
    // Absolutely no transaction attribution / SMU admission here.
    bool rednaviCommandSubmittedProven = false;
    bool appleOrOtherHostWriterAttributed = false;
    bool firmwareResponseToNewCommandProven = false;
    bool mailboxExclusiveProven = false;
    bool mayIssueSmuCommand = false;
};

inline Status configure(State &s, const Config &cfg) {
    s = State{};
    s.requested = cfg.explicitOptIn;
    if (!cfg.explicitOptIn) return Status::Disabled;
    if (!cfg.p736ResponseArmed) return Status::RequiresP736OptIn;
    if (!cfg.exactNavi33) return Status::WrongAsic;
    if (!cfg.mp1Is13_0_7) return Status::WrongMp1Revision;
    if (!cfg.directBar5MappingReady) return Status::NoBarMapping;
    const auto offsets = smu_mailbox_roundtrip::plan(
        cfg.mp1BaseDword, cfg.bar5MappedBytes);
    if (!offsets.valid) return Status::InvalidDirectOffsets;
    s.messageByteOffset = offsets.messageByteOffset;
    s.parameterByteOffset = offsets.parameterByteOffset;
    s.responseByteOffset = offsets.responseByteOffset;
    s.armed = true;
    return Status::Armed;
}

inline uint32_t claimedReads(const ImageBudget &budget) {
    return __atomic_load_n(&budget.claimedDwordReads, __ATOMIC_ACQUIRE);
}

inline bool claimPair(ImageBudget &budget) {
    uint32_t prior = claimedReads(budget);
    while (prior <= kMaxAdditionalDwordReads - kReadsPerTriplet) {
        const uint32_t desired = prior + kReadsPerTriplet;
        if (__atomic_compare_exchange_n(
                &budget.claimedDwordReads, &prior, desired, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

inline bool inRange(uint32_t byteOffset, uint64_t length) {
    return byteOffset != kInvalid && (byteOffset & 3u) == 0u &&
           length >= 4u &&
           static_cast<uint64_t>(byteOffset) <= length - 4u;
}

template <typename Read>
inline Report sample(State &state, ImageBudget &budget,
                     uint32_t tick, bool serviceActive, bool mappingPresent,
                     uint64_t mappedBarBytes,
                     uint32_t lastP736ResponseTick,
                     uint32_t lastP736Response,
                     uint32_t p736ResponseByteOffset,
                     Read read) {
    Report r{};
    r.messageByteOffset = state.messageByteOffset;
    r.parameterByteOffset = state.parameterByteOffset;
    r.responseByteOffset = state.responseByteOffset;
    r.serviceTriplets = state.samples;
    if (state.stopped) { r.status = Status::Stopped; return r; }
    if (!state.armed) { r.status = Status::Disabled; return r; }
    if (!serviceActive) { r.status = Status::InactiveService; return r; }
    if (!mappingPresent ||
        !inRange(state.messageByteOffset, mappedBarBytes) ||
        !inRange(state.parameterByteOffset, mappedBarBytes) ||
        !inRange(state.responseByteOffset, mappedBarBytes)) {
        r.status = Status::MappingLost;
        return r;
    }
    if (tick == 0u || tick != lastP736ResponseTick ||
        p736ResponseByteOffset != state.responseByteOffset ||
        (state.samples != 0u && tick == state.lastTick)) {
        r.status = Status::MissingPairedResponse;
        return r;
    }
    r.pairedResponseFromThisTick = true;
    if (state.samples >= kMaxTripletsPerService) {
        r.status = Status::ServiceBudgetExhausted;
        return r;
    }
    // Reserve BOTH additional dword reads in one atomic image-wide CAS.
    // On budget failure the callback invokes ZERO MMIO reads.
    if (!claimPair(budget)) {
        r.status = Status::ImageBudgetExhausted;
        return r;
    }
    const uint32_t message = read(state.messageByteOffset);
    const uint32_t parameter = read(state.parameterByteOffset);
    r.readPerformed = true;
    r.additionalReadsThisSample = kReadsPerTriplet;
    r.message = message;
    r.parameter = parameter;
    r.response = lastP736Response;
    r.previousTripletAvailable = state.samples != 0u;
    if (r.previousTripletAvailable) {
        r.messageChanged = state.lastMessage != message;
        r.parameterChanged = state.lastParameter != parameter;
        r.responseChanged = state.lastResponse != lastP736Response;
        r.evidence = r.messageChanged || r.parameterChanged ||
                     r.responseChanged
                         ? Evidence::UnattributedRegisterChange
                         : Evidence::StableBetweenSamples;
    } else {
        r.evidence = Evidence::FirstSnapshot;
    }
    state.lastTick = tick;
    state.lastMessage = message;
    state.lastParameter = parameter;
    state.lastResponse = lastP736Response;
    ++state.samples;
    r.serviceTriplets = state.samples;
    r.status = Status::Sampled;
    return r;
}

inline void stop(State &s) {
    s.stopped = true;
    s.armed = false;
    // Keep the image-wide read budget consumed after service removal.
}

}} // namespace rn_navi33::p737_passive_mailbox_triplet
#endif
