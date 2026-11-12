#include "navi33_smu_mailbox_admission.h"
#include <cassert>
using namespace rn_navi33::smu_mailbox_admission;

static Input ready() {
    Input x{};
    x.requested = x.explicitOptIn = x.unsafeBuildEnabled = true;
    x.exactNavi33 = x.mailboxLeaseHeld = true;
    x.mp1Major = 13; x.mp1Minor = 0; x.mp1Revision = 7;
    x.pspReady = x.pmfwAccepted = true;
    x.priorFenceExpected = x.priorFenceObserved = kExpectedPriorFence;
    x.pspTransportHealthy = x.addressesVerified = true;
    x.responseBefore = kResponseOk;
    return x;
}
static void rejects(Input x, Error e) {
    const auto r = inspect(x);
    assert(!r.maySubmit && r.error == e);
}
int main() {
    static_assert(kMsgRegisterDword == 0x282u, "Linux SMU13 MP1 message register");
    static_assert(kParamRegisterDword == 0x292u, "Linux SMU13 MP1 param register");
    static_assert(kResponseRegisterDword == 0x29au, "Linux SMU13 MP1 response register");
    static_assert(kGetSmuVersionMessage == 2u, "Navi33 GetSmuVersion");
    static_assert(kResponseOk == 1u, "SMU response OK");
    assert(inspect(ready()).maySubmit);
    { auto x=ready(); x.requested=false; rejects(x, Error::NotRequested); }
    { auto x=ready(); x.explicitOptIn=false; rejects(x, Error::MissingOptIn); }
    { auto x=ready(); x.unsafeBuildEnabled=false; rejects(x, Error::UnsafeBuildDisabled); }
    { auto x=ready(); x.exactNavi33=false; rejects(x, Error::WrongAsic); }
    { auto x=ready(); x.mp1Revision=10; rejects(x, Error::WrongMp1Revision); }
    { auto x=ready(); x.mailboxLeaseHeld=false; rejects(x, Error::NoMailboxLease); }
    { auto x=ready(); x.pspReady=false; rejects(x, Error::PspNotReady); }
    { auto x=ready(); x.pmfwAccepted=false; rejects(x, Error::PmfwNotAccepted); }
    { auto x=ready(); x.priorFenceObserved=22; rejects(x, Error::PspFenceMismatch); }
    { auto x=ready(); x.pspTransportHealthy=false; rejects(x, Error::PspTransportUnhealthy); }
    { auto x=ready(); x.addressesVerified=false; rejects(x, Error::NoVerifiedRegisters); }
    { auto x=ready(); x.responseBefore=kInvalid; rejects(x, Error::MailboxNotIdle); }

    Transcript transcript{};
    transcript.ownedByThisDriver = true;
    transcript.responseClearedToZeroObserved = true;
    transcript.commandSubmitted = true;
    transcript.commandId = kGetSmuVersionMessage;
    transcript.responseTransitionObserved = true;
    transcript.responseAfter = kResponseOk;
    transcript.argumentRead = true;
    transcript.versionResult = 0x00526000u;
    transcript.polls = 12u;
    assert(verifiedVersionHandshake(transcript));

    { auto x=transcript; x.ownedByThisDriver=false; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.responseClearedToZeroObserved=false; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.commandId=1; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.responseTransitionObserved=false; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.responseAfter=kResponseBusy; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.responseAfter=kInvalid; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.argumentRead=false; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.versionResult=0; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.versionResult=kInvalid; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.polls=0; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.polls=kMaxResponsePolls+1; assert(!verifiedVersionHandshake(x)); }
    { auto x=transcript; x.timedOut=true; assert(!verifiedVersionHandshake(x)); }

    // Actual macOS 0.0.40 log: smu_response=1 proves a readable register,
    // but there is no mailbox lease and no fresh request/response record.
    {
        auto x=ready();
        x.mailboxLeaseHeld=false;
        rejects(x, Error::NoMailboxLease);
        auto t=transcript;
        t.responseClearedToZeroObserved=false;
        t.commandSubmitted=false;
        assert(!verifiedVersionHandshake(t));
    }
    return 0;
}
