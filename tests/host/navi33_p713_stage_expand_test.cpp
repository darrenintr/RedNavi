// SPDX-License-Identifier: BSD-3-Clause
#include "navi33_p713_stage_expand.h"
#include <cassert>
using namespace rn_navi33::p713_stage_expand;

static Input ready() {
    Input in{};
    in.requested = true;
    in.explicitOptIn = true;
    in.identityVerified = true;
    in.firmwareVerified = true;
    in.gartOwned = true;
    in.firmwareBytes = kReviewedMecCodeBytes;
    in.pmfwBytes = 262144;
    in.gartStart = 0x7fff00000000ull;
    in.gartEnd = in.gartStart + (512ull << 20) - 1ull;
    return in;
}
static void rejects(Input in, Error error) {
    const auto r = inspect(in);
    assert(r.error == error);
    assert(!r.mayExpand);
}

int main() {
    static_assert(pageAlign(267008) == 270336, "MEC needs 66 pages");
    static_assert(kFirstPage == 259u, "After fw_pri and PSP ring");
    {
        const auto r = inspect(ready());
        assert(r.mayExpand && r.error == Error::Ok);
        assert(r.defaultStageBytes == 262144);
        assert(r.targetStageBytes == 270336);
        assert(r.targetPages == 66);
    }
    { auto x=ready(); x.requested=false; rejects(x,Error::Disabled); }
    { auto x=ready(); x.explicitOptIn=false; rejects(x,Error::NoExplicitOptIn); }
    { auto x=ready(); x.identityVerified=false; rejects(x,Error::IdentityUnverified); }
    { auto x=ready(); x.firmwareVerified=false; rejects(x,Error::FirmwareUnverified); }
    { auto x=ready(); x.gartOwned=false; rejects(x,Error::NoGartOwner); }
    { auto x=ready(); x.firmwareBytes=267004; rejects(x,Error::BadFirmwareSize); }
    { auto x=ready(); x.pmfwBytes=0; rejects(x,Error::BadPmfwSize); }
    { auto x=ready(); x.pmfwBytes=262143; rejects(x,Error::BadPmfwSize); }
    { auto x=ready(); x.pmfwBytes=1048576; rejects(x,Error::NoExpansionNeeded); }
    { auto x=ready(); x.pmfwBytes=1048572; rejects(x,Error::NoExpansionNeeded); }
    { auto x=ready(); x.pmfwBytes=1048580; rejects(x,Error::StageTooLarge); }
    { auto x=ready(); x.gartEnd=x.gartStart+259ull*4096+270336-2; rejects(x,Error::InvalidGartRange); }
    { auto x=ready(); x.gartEnd=x.gartStart-1; rejects(x,Error::InvalidGartRange); }
    { auto x=ready(); x.gartStart++; rejects(x,Error::InvalidGartRange); }
    return 0;
}
