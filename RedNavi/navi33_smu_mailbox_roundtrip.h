#ifndef REDNAVI_NAVI33_SMU_MAILBOX_ROUNDTRIP_H
#define REDNAVI_NAVI33_SMU_MAILBOX_ROUNDTRIP_H
// SPDX-License-Identifier: BSD-3-Clause
//
// One-shot Navi33 MP1 13.0.7 GetSmuVersion mailbox transport.
// Hardware access is supplied by the caller. The default P4 build must
// never enable the unsafe kernel call site. Host-only tests use fake MMIO.
// A PCI provider open arbitrates host clients, but it cannot prove that
// firmware or another PCI function never touches the SMU mailbox.
#include <stdint.h>
#include "navi33_smu_mailbox_admission.h"

namespace rn_navi33 { namespace smu_mailbox_roundtrip {

namespace admission = rn_navi33::smu_mailbox_admission;
constexpr uint32_t kPauseMicroseconds = 25u;

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
};

struct Result {
    Error error = Error::NotAdmitted;
    bool attempted = false;
    bool quarantineLease = false;
    bool responseClearWritten = false;
    bool parameterClearWritten = false;
    bool verified = false;
    uint32_t responseBefore = admission::kInvalid;
    uint32_t responseAfterClear = admission::kInvalid;
    uint32_t parameterAfterClear = admission::kInvalid;
    admission::Transcript transcript{};
};

// The caller must have exclusive host-IOPCIDevice access for this entire
// function. Once attempted, do not retry, including after BUSY or timeout.
// A response already equal to 1 cannot count as a fresh handshake: it
// must be cleared, observed as zero, and transition after message 0x02.
// Writes are never made unless admission, BAR5, and host lease all pass.
// read/write use verified BAR5 byte offsets. pause is a bounded delay.
template <typename Read, typename Write, typename Pause>
inline Result execute(const admission::Report &gate,
                      const RegisterPlan &reg,
                      bool hostLeaseHeld, bool &oneShotConsumed,
                      Read read, Write write, Pause pause) {
    Result r{};
    if (!gate.maySubmit || !hostLeaseHeld) return r;
    if (!reg.valid) { r.error = Error::InvalidAddress; return r; }
    if (oneShotConsumed) { r.error = Error::AlreadyAttempted; return r; }
    oneShotConsumed = true;
    r.attempted = true;
    auto &t = r.transcript;
    t.ownedByThisDriver = true;

    // Recheck under the owner's lease; an earlier snapshot may be stale.
    r.responseBefore = read(reg.responseByteOffset);
    if (r.responseBefore != admission::kResponseOk) {
        r.error = Error::ResponseChangedBeforeSubmit;
        r.quarantineLease = true;
        return r;
    }
    write(reg.responseByteOffset, 0u);
    r.responseClearWritten = true;
    r.responseAfterClear = read(reg.responseByteOffset);
    if (r.responseAfterClear != 0u) {
        r.error = Error::ClearNotObserved;
        r.quarantineLease = true;
        return r;
    }
    t.responseClearedToZeroObserved = true;

    write(reg.parameterByteOffset, 0u);
    r.parameterClearWritten = true;
    r.parameterAfterClear = read(reg.parameterByteOffset);
    if (r.parameterAfterClear != 0u) {
        r.error = Error::ParameterClearNotObserved;
        r.quarantineLease = true;
        return r;
    }

    write(reg.messageByteOffset, admission::kGetSmuVersionMessage);
    t.commandSubmitted = true;
    t.commandId = admission::kGetSmuVersionMessage;
    for (uint32_t i = 0u; i < admission::kMaxResponsePolls; ++i) {
        const uint32_t response = read(reg.responseByteOffset);
        t.polls = i + 1u;
        if (response != 0u) {
            t.responseTransitionObserved = true;
            t.responseAfter = response;
            break;
        }
        pause();
    }
    if (!t.responseTransitionObserved) {
        t.timedOut = true;
        r.error = Error::TimedOut;
        r.quarantineLease = true;
        return r;
    }
    if (t.responseAfter != admission::kResponseOk) {
        r.error = Error::FirmwareRejected;
        r.quarantineLease = true;
        return r;
    }
    t.versionResult = read(reg.parameterByteOffset);
    t.argumentRead = true;
    r.verified = admission::verifiedVersionHandshake(t);
    r.error = r.verified ? Error::Ok : Error::InvalidVersion;
    r.quarantineLease = !r.verified;
    return r;
}

}} // namespace rn_navi33::smu_mailbox_roundtrip
#endif
