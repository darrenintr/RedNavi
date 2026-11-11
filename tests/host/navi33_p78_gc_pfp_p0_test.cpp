// SPDX-License-Identifier: BSD-3-Clause
#include "navi33_p78_gc_pfp_p0.h"
#include <cassert>
using namespace rn_navi33::p78_gc_pfp_p0;

static Input ready() {
    Input in{};
    in.requested = true;
    in.explicitOptIn = true;
    in.identityVerified = true;
    in.observedGcMajor = 11;
    in.observedGcMinor = 0;
    in.observedGcRevision = 2;
    in.pspRingReady = true;
    in.tmrAccepted = true;
    in.sdmaBothAccepted = true;
    in.pfpAccepted = true;
    in.previousFenceExpected = 6;
    in.previousFenceObserved = 6;
    in.transportHealthy = true;
    in.stageOwnerRetained = true;
    in.stageMapped = true;
    in.stagePteVerified = true;
    in.firmwarePresent = true;
    in.firmwareMetadataVerified = true;
    in.firmwareSourceVerified = true;
    in.firmwareIpMajor = 11;
    in.firmwareIpMinor = 0;
    in.firmwareIpRevision = 2;
    in.firmwareBytes = 139264;
    in.stageBytes = 262144;
    in.stageGpuVa = 0x7fff00103000ull;
    in.gartStart = 0x7fff00000000ull;
    in.gartEnd = in.gartStart + (1ull << 29) - 1;
    return in;
}

static void rejects(Input in, Error reason) {
    const auto r = inspect(in);
    assert(r.error == reason);
    assert(!r.mayLoad);
    for (const auto v : r.words) assert(v == 0u);
}

int main() {
    static_assert(kCommandId == 6u, "Linux LOAD_IP_FW=6");
    static_assert(kFirmwareType == 90u, "PFP P0_STACK=90");
    static_assert(kPriorFence == 6u && kExpectedFence == 7u,
                  "P78 must execute as seventh ring command");
    {
        auto r = inspect(ready());
        assert(r.error == Error::Ok && r.mayLoad);
        assert(r.words[0] == 0x00103000u);
        assert(r.words[1] == 0x00007fffu);
        assert(r.words[2] == 139264u);
        assert(r.words[3] == 90u);
    }
    { auto a=ready(); a.requested=false; rejects(a,Error::Disabled); }
    { auto a=ready(); a.explicitOptIn=false; rejects(a,Error::NoExplicitOptIn); }
    { auto a=ready(); a.identityVerified=false; rejects(a,Error::IdentityUnverified); }
    { auto a=ready(); a.observedGcMajor=12; rejects(a,Error::GcIpMismatch); }
    { auto a=ready(); a.observedGcMinor=1; rejects(a,Error::GcIpMismatch); }
    { auto a=ready(); a.observedGcRevision=3; rejects(a,Error::GcIpMismatch); }
    { auto a=ready(); a.pspRingReady=false; rejects(a,Error::NoPspRing); }
    { auto a=ready(); a.tmrAccepted=false; rejects(a,Error::TmrNotAccepted); }
    { auto a=ready(); a.sdmaBothAccepted=false; rejects(a,Error::SdmaNotAccepted); }
    { auto a=ready(); a.pfpAccepted=false; rejects(a,Error::PfpNotAccepted); }
    { auto a=ready(); a.previousFenceExpected=5; rejects(a,Error::PreviousFenceMismatch); }
    { auto a=ready(); a.previousFenceObserved=5; rejects(a,Error::PreviousFenceMismatch); }
    { auto a=ready(); a.transportHealthy=false; rejects(a,Error::TransportUnhealthy); }
    { auto a=ready(); a.stageOwnerRetained=false; rejects(a,Error::NoStagingOwner); }
    { auto a=ready(); a.stageMapped=false; rejects(a,Error::NoStageMapping); }
    { auto a=ready(); a.stagePteVerified=false; rejects(a,Error::StagePteUnverified); }
    { auto a=ready(); a.firmwarePresent=false; rejects(a,Error::FirmwareMissing); }
    { auto a=ready(); a.firmwareMetadataVerified=false; rejects(a,Error::FirmwareMetadataUnverified); }
    { auto a=ready(); a.firmwareSourceVerified=false; rejects(a,Error::FirmwareSourceUnverified); }
    { auto a=ready(); a.firmwareIpRevision=3; rejects(a,Error::FirmwareTargetMismatch); }
    { auto a=ready(); a.firmwareBytes=0; rejects(a,Error::EmptyFirmware); }
    { auto a=ready(); a.firmwareBytes=90977; rejects(a,Error::EmptyFirmware); }
    { auto a=ready(); a.stageBytes=4096; rejects(a,Error::StageTooSmall); }
    { auto a=ready(); a.stageBytes=kStageMaxBytes+1; rejects(a,Error::StageTooLarge); }
    { auto a=ready(); a.stageGpuVa+=1; rejects(a,Error::InvalidStageAddress); }
    { auto a=ready(); a.gartEnd=a.stageGpuVa+4096; rejects(a,Error::InvalidStageAddress); }
    { auto a=ready(); a.gartStart=a.stageGpuVa+4096; rejects(a,Error::InvalidStageAddress); }
    return 0;
}
