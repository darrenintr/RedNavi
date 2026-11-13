#ifndef REDNAVI_NAVI33_SMU_MAILBOX_ROUNDTRIP_H
#define REDNAVI_NAVI33_SMU_MAILBOX_ROUNDTRIP_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P7.30: bounded, one-shot Navi33 MP1 13.0.7 GetSmuVersion transport.
// Pure C++ testable algorithm; the kernel caller supplies checked BAR5 MMIO,
// a monotonic nanosecond clock and an exclusive cooperating host mutex.
//
// This is NOT a hardware ownership grant. The real Navi33 caller must keep
// p727MailboxLeaseHeld=false until external ownership is reviewed.
// No PSP/RLC/MES/graphics engine commands are issued here.
#include <stdint.h>
#include "navi33_smu_mailbox_admission.h"
#include "navi33_smu_lifetime_latch.h"

namespace rn_navi33 { namespace smu_mailbox_roundtrip {

namespace admission = rn_navi33::smu_mailbox_admission;
constexpr uint32_t kPauseMicroseconds = 25u;
constexpr uint64_t kDeadlineNanoseconds = 50000000ull; // 50 ms monotonic budget

struct RegisterPlan {
    bool valid = false;
    uint32_t messageByteOffset = admission::kInvalid;
    uint32_t parameterByteOffset = admission::kInvalid;
    uint32_t responseByteOffset = admission::kInvalid;
};

inline bool checkedOffset(uint32_t baseDword, uint32_t regDword,
                          uint64_t barBytes, uint32_t *offset) {
    if (!baseDword || !offset || barBytes < sizeof(uint32_t)) return false;
    const uint64_t byte =
        (static_cast<uint64_t>(baseDword) + regDword) * 4ull;
    if (byte > UINT32_MAX || byte > barBytes - sizeof(uint32_t)) return false;
    *offset = static_cast<uint32_t>(byte);
    return true;
}

inline RegisterPlan plan(uint32_t mp1BaseDword, uint64_t barBytes) {
    RegisterPlan r{};
    const bool msg = checkedOffset(mp1BaseDword,
        admission::kMsgRegisterDword, barBytes, &r.messageByteOffset);
    const bool param = checkedOffset(mp1BaseDword,
        admission::kParamRegisterDword, barBytes, &r.parameterByteOffset);
    const bool resp = checkedOffset(mp1BaseDword,
        admission::kResponseRegisterDword, barBytes, &r.responseByteOffset);
    r.valid = msg && param && resp;
    return r;
}

enum class Error : uint8_t {
    Ok = 0,
    NotAdmitted,
    InvalidAddress,
    AlreadyAttempted,
    ResponseChangedBeforeSubmit,
    ClearNotObserved,
    ParameterClearNotObserved,
    TimedOut,
    FirmwareRejected,
    InvalidVersion,
    Quarantined,
    DeadlineExpired,
    ClockRegressed,
    CrossServiceAttemptAlreadyClaimed,
};

// One session is retained by the RedNaviProbe instance rather than
// re-created on every call. A separate atomic Kext-global claim prevents a
// second service instance from retrying after the first consumes its slot.
// A Kext reload can reset that global slot and is NOT covered.
// The caller serializes access to this state with gpu.smu_msg_lock.
struct SessionState {
    bool consumed = false;
    bool quarantined = false;
    bool verified = false;
};

struct Result {
    Error error = Error::NotAdmitted;
    bool attempted = false;
    bool globalAttemptClaimAcquired = false;
    bool quarantineLease = false;
    bool responseClearWritten = false;
    bool parameterClearWritten = false;
    bool verified = false;
    bool deadlineExpired = false;
    bool clockRegressed = false;
    uint32_t monotonicChecks = 0u;
    uint64_t elapsedNanoseconds = 0ull;
    uint32_t responseBefore = admission::kInvalid;
    uint32_t responseAfterClear = admission::kInvalid;
    uint32_t parameterAfterClear = admission::kInvalid;
    admission::Transcript transcript{};
};

// The caller MUST hold the common host SMU mutex for the complete
// transaction. hostLeaseHeld must be independently proven, never inferred
// from an idle response or IOPCIDevice::isOpen snapshot.
//
// The monotonic deadline covers response clearing, argument clearing,
// message write, response wait, and version read. The poll count is an
// additional fail-safe (avoids spinning if a supplied clock stalls).
// Rejected admission performs zero MMIO operations and consumes no attempt.
// The Kext-global claim is atomically consumed *before* any BAR5 observation.
// Once MMIO observation starts, *any* inconsistency quarantines the session.
//
// nowNs returns a monotonically increasing 64-bit nanosecond count.
// read/write use validated BAR5 byte offsets. pause is a bounded delay.
template <typename Read, typename Write, typename Pause, typename Now>
inline Result execute(const admission::Report &gate,
                      const RegisterPlan &reg, bool hostLeaseHeld,
                      SessionState &state,
                      smu_lifetime_latch::AttemptLatch &globalClaim,
                      Read read, Write write,
                      Pause pause, Now nowNs) {
    Result r{};
    if (!gate.maySubmit || !hostLeaseHeld) return r;
    if (!reg.valid) { r.error = Error::InvalidAddress; return r; }
    if (state.quarantined) { r.error = Error::Quarantined; return r; }
    if (state.consumed) { r.error = Error::AlreadyAttempted; return r; }
    // The atomic Kext-global latch stops a second RedNaviProbe instance
    // even if its own SessionState was newly constructed. A failed claim
    // does not read the clock, touch registers, or consume local state.
    if (!smu_lifetime_latch::claimOnce(globalClaim)) {
        r.error = Error::CrossServiceAttemptAlreadyClaimed;
        return r;
    }
    r.globalAttemptClaimAcquired = true;

    // Consume before the first MMIO observation; retries after partial
    // progress may misinterpret a delayed firmware response as a new one.
    state.consumed = true;
    r.attempted = true;
    auto &t = r.transcript;
    t.ownedByThisDriver = true;

    auto fail = [&](Error error) {
        r.error = error;
        r.quarantineLease = true;
        state.quarantined = true;
    };
    const uint64_t startedNs = nowNs();
    uint64_t lastNs = startedNs;
    auto deadlineReached = [&]() -> bool {
        const uint64_t currentNs = nowNs();
        ++r.monotonicChecks;
        if (currentNs < lastNs) {
            r.clockRegressed = true;
            fail(Error::ClockRegressed);
            return true;
        }
        lastNs = currentNs;
        r.elapsedNanoseconds = currentNs - startedNs;
        if (r.elapsedNanoseconds >= kDeadlineNanoseconds) {
            r.deadlineExpired = true;
            t.timedOut = true;
            fail(Error::DeadlineExpired);
            return true;
        }
        return false;
    };

    if (deadlineReached()) return r;
    r.responseBefore = read(reg.responseByteOffset);
    if (deadlineReached()) return r;
    if (r.responseBefore != admission::kResponseOk) {
        fail(Error::ResponseChangedBeforeSubmit);
        return r;
    }

    write(reg.responseByteOffset, 0u);
    r.responseClearWritten = true;
    if (deadlineReached()) return r;
    r.responseAfterClear = read(reg.responseByteOffset);
    if (deadlineReached()) return r;
    if (r.responseAfterClear != 0u) {
        fail(Error::ClearNotObserved);
        return r;
    }
    t.responseClearedToZeroObserved = true;

    write(reg.parameterByteOffset, 0u);
    r.parameterClearWritten = true;
    if (deadlineReached()) return r;
    r.parameterAfterClear = read(reg.parameterByteOffset);
    if (deadlineReached()) return r;
    if (r.parameterAfterClear != 0u) {
        fail(Error::ParameterClearNotObserved);
        return r;
    }

    // A deadline check immediately before the irreversible command write.
    if (deadlineReached()) return r;
    write(reg.messageByteOffset, admission::kGetSmuVersionMessage);
    t.commandSubmitted = true;
    t.commandId = admission::kGetSmuVersionMessage;

    for (uint32_t i = 0u; i < admission::kMaxResponsePolls; ++i) {
        if (deadlineReached()) return r;
        const uint32_t response = read(reg.responseByteOffset);
        t.polls = i + 1u;
        if (deadlineReached()) return r;
        if (response != 0u) {
            t.responseTransitionObserved = true;
            t.responseAfter = response;
            break;
        }
        if (i + 1u < admission::kMaxResponsePolls) pause();
    }
    if (!t.responseTransitionObserved) {
        t.timedOut = true;
        fail(Error::TimedOut);
        return r;
    }
    if (t.responseAfter != admission::kResponseOk) {
        fail(Error::FirmwareRejected);
        return r;
    }

    if (deadlineReached()) return r;
    t.versionResult = read(reg.parameterByteOffset);
    t.argumentRead = true;
    if (deadlineReached()) return r;
    r.verified = admission::verifiedVersionHandshake(t);
    if (!r.verified) {
        fail(Error::InvalidVersion);
        return r;
    }
    r.error = Error::Ok;
    state.verified = true;
    return r;
}

}} // namespace rn_navi33::smu_mailbox_roundtrip
#endif
