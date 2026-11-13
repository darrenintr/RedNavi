#ifndef REDNAVI_NAVI33_P736_MP1_RESPONSE_SAMPLER_H
#define REDNAVI_NAVI33_P736_MP1_RESPONSE_SAMPLER_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P7.36: ONLY an explicitly requested, bounded, DIRECT BAR5 read of
// MP1 C2PMSG_90. Host-only policy; kernel supplies the one read callback.
// The default boot executes NO new reads. No RSMU/MM_INDEX fallback and
// no mailbox message, parameter, or response WRITE is present here.
//
// The Linux SMU13 V1 transport polls its response register with RREG32.
// This is positive evidence for normal software polling, NOT proof that
// all hardware revisions/power states have no read side effects.
// Neither a repeated nor a changed response indicates message ownership.
//
#include <stdint.h>
#include "navi33_smu_mailbox_roundtrip.h"

namespace rn_navi33 { namespace p736_response_sampler {

constexpr uint32_t kMaxKextImageReads = 2u;
constexpr uint32_t kInvalid = 0xffffffffu;

enum class Error : uint8_t {
    Disabled = 0,
    WrongAsic,
    WrongMp1Revision,
    MissingBarMapping,
    BadDirectRegisterAddress,
    Armed,
    Stopped,
    InactiveService,
    MappingLost,
    KextReadBudgetExhausted,
    ServiceReadBudgetExhausted,
    Sampled
};

struct Config {
    bool explicitOptIn = false;
    bool exactNavi33 = false;
    bool mp1Is13_0_7 = false;
    bool directBar5MappingReady = false;
    uint32_t mp1BaseDword = 0u;
    uint64_t bar5MappedBytes = 0u;
};

struct State {
    bool requested = false;
    bool armed = false;
    bool stopped = false;
    uint32_t responseByteOffset = kInvalid;
    uint32_t samples = 0u;
    uint32_t lastValue = kInvalid;
};

struct KextBudget {
    // Atomic CAS across all Navi33 services in the same loaded Kext.
    // Never reset on an IOService re-probe; resets upon Kext reload.
    alignas(uint32_t) uint32_t claimed = 0u;
};

struct Report {
    Error error = Error::Disabled;
    bool readPerformed = false;
    bool newValueDiffers = false;
    bool previousValueKnown = false;
    bool globalSlotConsumed = false;
    uint32_t response = kInvalid;
    uint32_t sampleCount = 0u;
    uint32_t responseByteOffset = kInvalid;
    // SMU ownership, firmware state and roundtrip remain unproven.
    bool mailboxExclusiveProven = false;
    bool handshakeVerified = false;
    bool mayIssueSmuCommand = false;
};

inline Error configure(State &s, const Config &in) {
    // Configured once from validated Navi33 device discovery, not from an
    // IORegistry property or a user client. Does not perform GPU I/O.
    s = State{};
    s.requested = in.explicitOptIn;
    if (!in.explicitOptIn) return Error::Disabled;
    if (!in.exactNavi33) return Error::WrongAsic;
    if (!in.mp1Is13_0_7) return Error::WrongMp1Revision;
    if (!in.directBar5MappingReady) return Error::MissingBarMapping;
    const auto offsets = smu_mailbox_roundtrip::plan(
        in.mp1BaseDword, in.bar5MappedBytes);
    if (!offsets.valid) return Error::BadDirectRegisterAddress;
    s.responseByteOffset = offsets.responseByteOffset;
    s.armed = true;
    return Error::Armed;
}

inline bool claimOne(KextBudget &budget) {
    uint32_t prior = __atomic_load_n(&budget.claimed, __ATOMIC_ACQUIRE);
    while (prior < kMaxKextImageReads) {
        const uint32_t next = prior + 1u;
        if (__atomic_compare_exchange_n(&budget.claimed, &prior,
                                        next, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

inline uint32_t readsClaimed(const KextBudget &budget) {
    return __atomic_load_n(&budget.claimed, __ATOMIC_ACQUIRE);
}

template <typename Read>
inline Report sample(State &s, KextBudget &budget, bool serviceActive,
                     bool mappingValid, uint64_t currentMappedBarBytes,
                     Read read) {
    Report r{};
    r.sampleCount = s.samples;
    r.responseByteOffset = s.responseByteOffset;
    if (s.stopped) { r.error = Error::Stopped; return r; }
    if (!s.armed) { r.error = Error::Disabled; return r; }
    if (!serviceActive) { r.error = Error::InactiveService; return r; }
    if (!mappingValid || s.responseByteOffset == kInvalid ||
        currentMappedBarBytes < sizeof(uint32_t) ||
        static_cast<uint64_t>(s.responseByteOffset) >
            currentMappedBarBytes - sizeof(uint32_t)) {
        r.error = Error::MappingLost;
        return r;
    }
    if (s.samples >= kMaxKextImageReads) {
        r.error = Error::ServiceReadBudgetExhausted;
        return r;
    }
    // Consume the shared Kext-image slot before any live device read.
    // A reset or second service cannot exceed this budget in one image.
    if (!claimOne(budget)) {
        r.error = Error::KextReadBudgetExhausted;
        return r;
    }
    r.globalSlotConsumed = true;
    const uint32_t value = read(s.responseByteOffset);
    r.readPerformed = true;
    r.previousValueKnown = s.samples != 0u;
    r.newValueDiffers = r.previousValueKnown && s.lastValue != value;
    r.response = value;
    s.lastValue = value;
    ++s.samples;
    r.sampleCount = s.samples;
    r.error = Error::Sampled;
    // Neither an existing 1 nor a change into 1 is a message handshake.
    return r;
}

inline void stop(State &s) {
    s.stopped = true;
    s.armed = false;
    // Never replenish the Kext budget here.
}

}} // namespace rn_navi33::p736_response_sampler
#endif
