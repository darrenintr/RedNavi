// P7.25 read-only policy tests: no hardware I/O.
#include "navi33_p725_rlc_autoload.h"
#include <cassert>
using namespace rn_navi33::p725_rlc_autoload;

static Input ready() {
    Input i{};
    i.requested = i.explicitOptIn = i.asicVerified = true;
    i.gcMajor = 11; i.gcMinor = 0; i.gcRevision = 2;
    i.pspRingReady = i.tmrAccepted = i.pmfwAccepted = true;
    i.sdmaBothAccepted = i.pfpMeMecAccepted = true;
    i.rlcGAccepted = i.imuIAccepted = i.imuDAccepted = true;
    i.mesSchedulerCodeAccepted = i.mesSchedulerDataAccepted = true;
    i.mesKiqCodeAccepted = i.mesKiqDataAccepted = true;
    i.previousFenceExpected = i.previousFenceObserved = kPriorFence;
    i.transportHealthy = i.runtimeOwnerRetained = true;
    return i;
}
static void rejects(Input i, Error e) {
    auto r = inspect(i);
    assert(r.error == e && !r.mayAutoload);
}
int main() {
    static_assert(kCommandId == 0x21u, "Linux GFX_CMD_ID_AUTOLOAD_RLC");
    static_assert(kExpectedFence == 24u, "autoload PSP command fence");
    assert(hardwareBootComplete(0u, 0x80000000u));
    assert(!hardwareBootComplete(1u, 0x80000000u));
    assert(!hardwareBootComplete(0u, 0x7fffffffu));
    assert(!hardwareBootComplete(0u, 0xffffffffu));
    assert(!hardwareBootComplete(0xffffffffu, 0xffffffffu));
    assert(inspect(ready()).mayAutoload);
    { auto i=ready(); i.requested=false; rejects(i,Error::Disabled); }
    { auto i=ready(); i.explicitOptIn=false; rejects(i,Error::NoExplicitOptIn); }
    { auto i=ready(); i.asicVerified=false; rejects(i,Error::WrongAsic); }
    { auto i=ready(); i.gcRevision=3; rejects(i,Error::WrongGcVersion); }
    { auto i=ready(); i.pspRingReady=false; rejects(i,Error::NoPspRing); }
    { auto i=ready(); i.tmrAccepted=false; rejects(i,Error::NoTmr); }
    { auto i=ready(); i.pmfwAccepted=false; rejects(i,Error::NoPmfw); }
    { auto i=ready(); i.sdmaBothAccepted=false; rejects(i,Error::SdmaMissing); }
    { auto i=ready(); i.pfpMeMecAccepted=false; rejects(i,Error::PfpMeMecMissing); }
    { auto i=ready(); i.rlcGAccepted=false; rejects(i,Error::RlcMissing); }
    { auto i=ready(); i.imuDAccepted=false; rejects(i,Error::ImuMissing); }
    { auto i=ready(); i.mesSchedulerCodeAccepted=false; rejects(i,Error::MesSchedulerMissing); }
    { auto i=ready(); i.mesSchedulerDataAccepted=false; rejects(i,Error::MesSchedulerMissing); }
    { auto i=ready(); i.mesKiqCodeAccepted=false; rejects(i,Error::MesKiqMissing); }
    { auto i=ready(); i.mesKiqDataAccepted=false; rejects(i,Error::MesKiqMissing); }
    { auto i=ready(); i.previousFenceExpected=22; rejects(i,Error::PriorFenceMismatch); }
    { auto i=ready(); i.previousFenceObserved=22; rejects(i,Error::PriorFenceMismatch); }
    { auto i=ready(); i.transportHealthy=false; rejects(i,Error::TransportUnhealthy); }
    { auto i=ready(); i.runtimeOwnerRetained=false; rejects(i,Error::NoRuntimeOwner); }
    return 0;
}
