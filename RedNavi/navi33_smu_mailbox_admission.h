#ifndef REDNAVI_NAVI33_SMU_MAILBOX_ADMISSION_H
#define REDNAVI_NAVI33_SMU_MAILBOX_ADMISSION_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Navi33 MP1 13.0.7 GetSmuVersion admission and response-validation policy.
// NO hardware I/O is implemented in this header or enabled by this branch.
//
// Linux references:
//  smu_v13_0.c: C2PMSG_66=0x282 (message), _82=0x292 (argument),
//                _90=0x29a (response), all *DWORD* offsets from MP1 base.
//  smu_v13_0_7_ppsmc.h: GetSmuVersion=0x02, PPSMC_Result_OK=0x01.
//  smu_cmn_get_smc_version(): obtains version via an actual SMU message.
//
// Reading a stale C2PMSG_90 value 1 without an owned request/response
// transition NEVER establishes that the running SMU can process messages.
#include <stdint.h>

namespace rn_navi33 { namespace smu_mailbox_admission {

constexpr uint32_t kMsgRegisterDword = 0x0282u;
constexpr uint32_t kParamRegisterDword = 0x0292u;
constexpr uint32_t kResponseRegisterDword = 0x029au;
constexpr uint32_t kGetSmuVersionMessage = 0x02u;
constexpr uint32_t kResponseOk = 0x01u;
constexpr uint32_t kResponseBusy = 0xfcu;
constexpr uint32_t kInvalid = 0xffffffffu;
constexpr uint32_t kExpectedPriorFence = 23u;
constexpr uint32_t kMaxResponsePolls = 2000u; // Proposed bound, not used for MMIO here.

enum class Error : uint8_t {
    Ok = 0,
    NotRequested,
    MissingOptIn,
    UnsafeBuildDisabled,
    WrongAsic,
    WrongMp1Revision,
    NoMailboxLease,
    PspNotReady,
    PmfwNotAccepted,
    PspFenceMismatch,
    PspTransportUnhealthy,
    NoVerifiedRegisters,
    MailboxNotIdle,
};

struct Input {
    bool requested = false;
    bool explicitOptIn = false;
    bool unsafeBuildEnabled = false;
    bool exactNavi33 = false;
    uint8_t mp1Major = 0;
    uint8_t mp1Minor = 0;
    uint8_t mp1Revision = 0;
    // A unique device/SMU mailbox owner MUST be established by kernel
    // resource arbitration, not by a boot argument or an idle status bit.
    bool mailboxLeaseHeld = false;
    bool pspReady = false;
    bool pmfwAccepted = false;
    uint32_t priorFenceExpected = 0;
    uint32_t priorFenceObserved = 0;
    bool pspTransportHealthy = false;
    bool addressesVerified = false;
    uint32_t responseBefore = kInvalid;
};

struct Report {
    Error error = Error::NotRequested;
    bool maySubmit = false;
};

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.explicitOptIn) { r.error = Error::MissingOptIn; return r; }
    if (!in.unsafeBuildEnabled) {
        r.error = Error::UnsafeBuildDisabled; return r;
    }
    if (!in.exactNavi33) { r.error = Error::WrongAsic; return r; }
    if (in.mp1Major != 13u || in.mp1Minor != 0u || in.mp1Revision != 7u) {
        r.error = Error::WrongMp1Revision; return r;
    }
    if (!in.mailboxLeaseHeld) { r.error = Error::NoMailboxLease; return r; }
    if (!in.pspReady) { r.error = Error::PspNotReady; return r; }
    if (!in.pmfwAccepted) { r.error = Error::PmfwNotAccepted; return r; }
    if (in.priorFenceExpected != kExpectedPriorFence ||
        in.priorFenceObserved != kExpectedPriorFence) {
        r.error = Error::PspFenceMismatch; return r;
    }
    if (!in.pspTransportHealthy) {
        r.error = Error::PspTransportUnhealthy; return r;
    }
    if (!in.addressesVerified) {
        r.error = Error::NoVerifiedRegisters; return r;
    }
    if (in.responseBefore != kResponseOk) {
        r.error = Error::MailboxNotIdle; return r;
    }
    r.error = Error::Ok;
    r.maySubmit = true;
    return r;
}

// A transaction must supply independent observations of all three phases.
// The response register already containing 1 is not a fresh reply.
// The implementation must clear response, observe zero, send exactly
// GetSmuVersion, poll within a deadline, read the output parameter, and
// permanently invalidate any mailbox lease on timeout or inconsistency.
struct Transcript {
    bool ownedByThisDriver = false;
    bool responseClearedToZeroObserved = false;
    bool commandSubmitted = false;
    uint32_t commandId = 0;
    bool responseTransitionObserved = false;
    uint32_t responseAfter = kInvalid;
    bool argumentRead = false;
    uint32_t versionResult = kInvalid;
    uint32_t polls = 0;
    bool timedOut = false;
};

inline bool verifiedVersionHandshake(const Transcript &t) {
    return t.ownedByThisDriver &&
           t.responseClearedToZeroObserved &&
           t.commandSubmitted &&
           t.commandId == kGetSmuVersionMessage &&
           t.responseTransitionObserved &&
           t.responseAfter == kResponseOk &&
           t.argumentRead &&
           t.versionResult != 0u &&
           t.versionResult != kInvalid &&
           t.polls > 0u && t.polls <= kMaxResponsePolls &&
           !t.timedOut;
}

}} // namespace rn_navi33::smu_mailbox_admission
#endif
