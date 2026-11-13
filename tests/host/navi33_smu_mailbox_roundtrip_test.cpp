#include "navi33_smu_mailbox_roundtrip.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>

using namespace rn_navi33;
namespace smu = smu_mailbox_admission;
namespace rt = smu_mailbox_roundtrip;

struct Fake {
    rt::RegisterPlan reg;
    smu_lifetime_latch::AttemptLatch claim{};
    smu_lifetime_latch::AttemptLatch *sharedClaim = &claim;
    uint32_t response = smu::kResponseOk;
    uint32_t parameter = 99u;
    uint32_t message = 0u;
    uint32_t reply = smu::kResponseOk;
    uint32_t version = 0x00526000u;
    uint32_t writes = 0u, messageWrites = 0u;
    uint32_t pauses = 0u, pollsAfterSend = 0u, clockReads = 0u;
    uint32_t replyAt = 3u;
    uint64_t tickNs = 100000000ull; // arbitrary monotonic clock origin
    uint64_t pauseNs = 25000ull;    // 25 us
    uint64_t timeOnWriteNs = 0ull;
    uint64_t timeOnReadParamNs = 0ull;
    bool ignoreResponseClear = false, ignoreParameterClear = false;
    bool regressOnPause = false;

    explicit Fake(rt::RegisterPlan p) : reg(p) {}
    uint32_t read(uint32_t offset) {
        if (offset == reg.responseByteOffset) {
            if (message == smu::kGetSmuVersionMessage &&
                ++pollsAfterSend >= replyAt) {
                response = reply;
                parameter = version;
            }
            return response;
        }
        assert(offset == reg.parameterByteOffset);
        tickNs += timeOnReadParamNs;
        return parameter;
    }
    void write(uint32_t offset, uint32_t v) {
        ++writes;
        tickNs += timeOnWriteNs;
        if (offset == reg.responseByteOffset) {
            if (!ignoreResponseClear) response = v;
        } else if (offset == reg.parameterByteOffset) {
            if (!ignoreParameterClear) parameter = v;
        } else {
            assert(offset == reg.messageByteOffset);
            assert(v == smu::kGetSmuVersionMessage);
            message = v;
            ++messageWrites;
        }
    }
    void pause() {
        ++pauses;
        if (regressOnPause) tickNs -= 1000000ull;
        else tickNs += pauseNs;
    }
    uint64_t now() { ++clockReads; return tickNs; }
};

smu::Report admitted() {
    smu::Report r{};
    r.error = smu::Error::Ok;
    r.maySubmit = true;
    return r;
}
rt::Result run(Fake &f, rt::SessionState &session,
               smu::Report gate = admitted(), bool leased = true) {
    return rt::execute(gate, f.reg, leased, session, *f.sharedClaim,
        [&](uint32_t o) { return f.read(o); },
        [&](uint32_t o, uint32_t v) { f.write(o, v); },
        [&]() { f.pause(); },
        [&]() { return f.now(); });
}

int main() {
    const auto reg = rt::plan(0x16000u, 1024u * 1024u);
    assert(reg.valid && reg.messageByteOffset == 363016u &&
           reg.parameterByteOffset == 363080u &&
           reg.responseByteOffset == 363112u);
    assert(!rt::plan(0u, 1024u * 1024u).valid);
    assert(!rt::plan(0x16000u, 363112u).valid);
    assert(!rt::plan(0xffffffffu, 1024u * 1024u).valid);

    { // Verified command transition; state persists for every later call.
        Fake f(reg); rt::SessionState session{};
        const auto result = run(f, session);
        assert(result.verified && result.error == rt::Error::Ok);
        assert(result.responseBefore == 1u &&
               result.responseAfterClear == 0u &&
               result.parameterAfterClear == 0u);
        assert(result.transcript.responseAfter == 1u &&
               result.transcript.versionResult == f.version);
        assert(result.transcript.polls == 3u && f.messageWrites == 1u &&
               f.writes == 3u && result.monotonicChecks > 0u);
        assert(session.consumed && session.verified && !session.quarantined);
        auto second = run(f, session);
        assert(second.error == rt::Error::AlreadyAttempted);
        assert(f.messageWrites == 1u && f.writes == 3u);
        assert(smu_lifetime_latch::isClaimed(f.claim));
    }
    { // A new service instance has a fresh local session, but must
      // still be rejected by the same global Kext attempt latch.
        Fake first(reg), next(reg);
        next.sharedClaim = &first.claim;
        rt::SessionState local1{}, local2{};
        auto a = run(first, local1);
        assert(a.verified && smu_lifetime_latch::isClaimed(first.claim));
        auto b = run(next, local2);
        assert(b.error == rt::Error::CrossServiceAttemptAlreadyClaimed);
        assert(!local2.consumed && !local2.quarantined);
        assert(next.writes == 0u && next.clockReads == 0u);
    }
    { // Default-off admission must not consume session or even read clock.
        Fake f(reg); rt::SessionState session{};
        auto deny = admitted(); deny.maySubmit = false;
        auto first = run(f, session, deny);
        assert(first.error == rt::Error::NotAdmitted &&
               !session.consumed && f.writes == 0u && f.clockReads == 0u);
        auto second = run(f, session, admitted(), false);
        assert(second.error == rt::Error::NotAdmitted &&
               !session.consumed && f.writes == 0u && f.clockReads == 0u);
        assert(!smu_lifetime_latch::isClaimed(f.claim));
    }
    { // Invalid BAR mapping rejects before consuming the session.
        auto wrong = rt::plan(0x16000u, 16u);
        Fake f(wrong); rt::SessionState session{};
        auto result = run(f, session);
        assert(result.error == rt::Error::InvalidAddress &&
               !session.consumed && f.writes == 0u);
    }
    { // Unexpected pre-submit value must permanently quarantine.
        Fake f(reg); rt::SessionState session{};
        f.response = 0u;
        auto result = run(f, session);
        assert(result.error == rt::Error::ResponseChangedBeforeSubmit &&
               session.consumed && session.quarantined && f.writes == 0u);
        const auto again = run(f, session);
        assert(again.error == rt::Error::Quarantined && f.writes == 0u);
        Fake reprobe(reg); reprobe.sharedClaim = &f.claim;
        rt::SessionState newServiceSession{};
        auto blocked = run(reprobe, newServiceSession);
        assert(blocked.error == rt::Error::CrossServiceAttemptAlreadyClaimed);
        assert(reprobe.writes == 0u && reprobe.clockReads == 0u);
    }
    { // Failed clear readback cannot be treated as a fresh response.
        Fake f(reg); rt::SessionState session{};
        f.ignoreResponseClear = true;
        auto result = run(f, session);
        assert(result.error == rt::Error::ClearNotObserved &&
               session.quarantined && f.writes == 1u && f.messageWrites == 0u);
    }
    {
        Fake f(reg); rt::SessionState session{};
        f.ignoreParameterClear = true;
        auto result = run(f, session);
        assert(result.error == rt::Error::ParameterClearNotObserved &&
               session.quarantined && f.writes == 2u && f.messageWrites == 0u);
    }
    { // The independent poll cap also works if the clock stops advancing.
        Fake f(reg); rt::SessionState session{};
        f.replyAt = smu::kMaxResponsePolls + 1u;
        f.pauseNs = 0u;
        auto result = run(f, session);
        assert(result.error == rt::Error::TimedOut &&
               result.transcript.timedOut &&
               result.transcript.polls == smu::kMaxResponsePolls);
        assert(session.quarantined && f.messageWrites == 1u);
    }
    { // Real elapsed monotonic budget is enforced before 2000 polls.
        Fake f(reg); rt::SessionState session{};
        f.replyAt = smu::kMaxResponsePolls + 1u;
        f.pauseNs = 20000000ull;
        auto result = run(f, session);
        assert(result.error == rt::Error::DeadlineExpired &&
               result.deadlineExpired && result.transcript.timedOut);
        assert(result.transcript.polls < smu::kMaxResponsePolls &&
               result.elapsedNanoseconds >= rt::kDeadlineNanoseconds);
        assert(session.consumed && session.quarantined && f.messageWrites == 1u);
        auto again = run(f, session);
        assert(again.error == rt::Error::Quarantined && f.messageWrites == 1u);
    }
    { // A deadline reached between parameter clearing and submission
      // must *not* issue the irreversible message.
        Fake f(reg); rt::SessionState session{};
        f.timeOnWriteNs = 30000000ull; // 30 ms per MMIO write
        auto result = run(f, session);
        assert(result.error == rt::Error::DeadlineExpired &&
               result.deadlineExpired && !result.transcript.commandSubmitted &&
               f.messageWrites == 0u && f.writes == 2u &&
               session.quarantined);
    }
    { // A backwards clock must fail closed, never wrap to a success.
        Fake f(reg); rt::SessionState session{};
        f.replyAt = smu::kMaxResponsePolls + 1u;
        f.regressOnPause = true;
        auto result = run(f, session);
        assert(result.error == rt::Error::ClockRegressed &&
               result.clockRegressed && session.quarantined &&
               f.messageWrites == 1u);
    }
    { // Firmware error and invalid version quarantine the lease.
        Fake f(reg); rt::SessionState session{};
        f.reply = smu::kResponseBusy;
        auto result = run(f, session);
        assert(result.error == rt::Error::FirmwareRejected &&
               result.transcript.responseAfter == smu::kResponseBusy &&
               session.quarantined);
    }
    for (uint32_t invalid : {0u, 0xffffffffu}) {
        Fake f(reg); rt::SessionState session{};
        f.version = invalid;
        auto result = run(f, session);
        assert(result.error == rt::Error::InvalidVersion &&
               !result.verified && session.quarantined &&
               f.messageWrites == 1u);
    }
}
