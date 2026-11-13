#ifndef REDNAVI_NAVI33_P735_PASSIVE_MP1_READ_PLAN_H
#define REDNAVI_NAVI33_P735_PASSIVE_MP1_READ_PLAN_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P7.35 HOST-ONLY plan for a future passive MP1 register monitor.
//
// Direct BAR5 read means the *host instruction* does not issue a MMIO
// register-index write. It does NOT prove the target register has no
// read-to-clear or other hardware read side effect. Consequently this
// policy never authorizes a new live read and never opens an SMU lease.
//
// rn_rreg32 may write RN_RSMU_INDEX when outside direct MMIO range;
// rn_vram_rd32/vramRead write MM_INDEX even to obtain a read result.
// Neither API is suitable for a strict "zero MMIO writes" monitor.
#include <stdint.h>
#include "navi33_smu_mailbox_roundtrip.h"

namespace rn_navi33 { namespace p735_passive_mp1_read_plan {

enum class Refusal : uint8_t {
    None = 0,
    WrongAsic,
    WrongMp1Revision,
    NoBarMapping,
    BadRegisterAddress,
};

struct Input {
    bool exactNavi33 = false;
    bool mp1RevisionVerified = false; // MP1 13.0.7
    bool bar5DirectMappingReady = false;
    uint32_t mp1BaseDword = 0u;
    uint64_t bar5MappedBytes = 0ull;
};

struct Report {
    Refusal refusal = Refusal::WrongAsic;
    bool offsetsVerifiedInDirectMappedBar = false;
    bool requiresRsmuIndexWrite = false; // no fallback is allowed
    bool requiresVramIndexWrite = false; // no fallback is allowed
    bool registerReadSideEffectsExcluded = false; // not reviewed here
    bool liveReadPermitted = false; // always false
    bool foreignWritersExcluded = false; // always false
    bool firmwareWritersExcluded = false; // always false
    bool mailboxExclusiveProven = false; // always false
    bool mayIssueSmuCommand = false; // always false
    uint32_t msgByteOffset = 0xffffffffu;
    uint32_t paramByteOffset = 0xffffffffu;
    uint32_t responseByteOffset = 0xffffffffu;
};

inline Report plan(const Input &in) {
    Report r{};
    if (!in.exactNavi33) return r;
    if (!in.mp1RevisionVerified) {
        r.refusal = Refusal::WrongMp1Revision;
        return r;
    }
    if (!in.bar5DirectMappingReady) {
        r.refusal = Refusal::NoBarMapping;
        return r;
    }
    const auto reg = smu_mailbox_roundtrip::plan(
        in.mp1BaseDword, in.bar5MappedBytes);
    if (!reg.valid) {
        r.refusal = Refusal::BadRegisterAddress;
        return r;
    }
    r.refusal = Refusal::None;
    r.offsetsVerifiedInDirectMappedBar = true;
    r.msgByteOffset = reg.messageByteOffset;
    r.paramByteOffset = reg.parameterByteOffset;
    r.responseByteOffset = reg.responseByteOffset;
    // This is an address plan, NOT an instruction to read hardware.
    // Read-to-clear semantics and time-of-use resource lifetime need
    // independent review. No runtime reads/writes are introduced.
    return r;
}

}} // namespace rn_navi33::p735_passive_mp1_read_plan
#endif
