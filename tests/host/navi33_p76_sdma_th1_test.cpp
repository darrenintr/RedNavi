// SPDX-License-Identifier: BSD-3-Clause
#include "navi33_p76_sdma_th1.h"
#include <cassert>
using namespace rn_navi33::p76_sdma;

static Input ready() {
    Input in{};
    in.requested = true;
    in.explicitOptIn = true;
    in.identityVerified = true;
    in.observedSdmaMajor = 6;
    in.observedSdmaMinor = 0;
    in.observedSdmaRevision = 2;
    in.pspRingReady = true;
    in.tmrAccepted = true;
    in.th0Accepted = true;
    in.previousFenceExpected = 4;
    in.previousFenceObserved = 4;
    in.transportHealthy = true;
    in.stageOwnerRetained = true;
    in.stageMapped = true;
    in.stagePteVerified = true;
    in.firmwarePresent = true;
    in.firmwareMetadataVerified = true;
    in.firmwareSourceVerified = true;
    in.firmwareIpMajor = 6;
    in.firmwareIpMinor = 0;
    in.firmwareIpRevision = 2;
    in.firmwareBytes = 16896;
    in.stageBytes = 262144;
    in.stageGpuVa = 0x7fff00103000ull;
    in.gartStart = 0x7fff00000000ull;
    in.gartEnd = in.gartStart + (1ull << 29) - 1;
    return in;
}

static void rejects(Input in, Error reason) {
    auto report = inspect(in);
    assert(report.error == reason);
    assert(!report.mayLoad);
    for (auto w : report.words) assert(w == 0u);
}

int main() {
    static_assert(kCommandId == 6u, "Linux LOAD_IP_FW");
    static_assert(kFirmwareType == 72u, "Linux SDMA Control Thread");
    static_assert(kPriorFence == 4u && kExpectedFence == 5u,
                  "Fifth PSP command must follow completed TH0 fence");
    {
        auto r = inspect(ready());
        assert(r.error == Error::Ok && r.mayLoad);
        assert(r.words[0] == 0x00103000u);
        assert(r.words[1] == 0x00007fffu);
        assert(r.words[2] == 16896u);
        assert(r.words[3] == 72u);
    }
    { auto a=ready(); a.requested=false; rejects(a,Error::Disabled); }
    { auto a=ready(); a.explicitOptIn=false; rejects(a,Error::NoExplicitOptIn); }
    { auto a=ready(); a.identityVerified=false; rejects(a,Error::IdentityUnverified); }
    { auto a=ready(); a.observedSdmaMajor=5; rejects(a,Error::SdmaIpMismatch); }
    { auto a=ready(); a.observedSdmaMinor=1; rejects(a,Error::SdmaIpMismatch); }
    { auto a=ready(); a.observedSdmaRevision=3; rejects(a,Error::SdmaIpMismatch); }
    { auto a=ready(); a.pspRingReady=false; rejects(a,Error::NoPspRing); }
    { auto a=ready(); a.tmrAccepted=false; rejects(a,Error::TmrNotAccepted); }
    { auto a=ready(); a.th0Accepted=false; rejects(a,Error::Th0NotAccepted); }
    { auto a=ready(); a.previousFenceExpected=3; rejects(a,Error::PreviousFenceMismatch); }
    { auto a=ready(); a.previousFenceObserved=3; rejects(a,Error::PreviousFenceMismatch); }
    { auto a=ready(); a.transportHealthy=false; rejects(a,Error::TransportUnhealthy); }
    { auto a=ready(); a.stageOwnerRetained=false; rejects(a,Error::NoStagingOwner); }
    { auto a=ready(); a.stageMapped=false; rejects(a,Error::NoStageMapping); }
    { auto a=ready(); a.stagePteVerified=false; rejects(a,Error::StagePteUnverified); }
    { auto a=ready(); a.firmwarePresent=false; rejects(a,Error::FirmwareMissing); }
    { auto a=ready(); a.firmwareMetadataVerified=false; rejects(a,Error::FirmwareMetadataUnverified); }
    { auto a=ready(); a.firmwareSourceVerified=false; rejects(a,Error::FirmwareSourceUnverified); }
    { auto a=ready(); a.firmwareIpRevision=3; rejects(a,Error::FirmwareTargetMismatch); }
    { auto a=ready(); a.firmwareBytes=0; rejects(a,Error::EmptyFirmware); }
    { auto a=ready(); a.firmwareBytes=16897; rejects(a,Error::EmptyFirmware); }
    { auto a=ready(); a.stageBytes=4096; rejects(a,Error::StageTooSmall); }
    { auto a=ready(); a.stageBytes=kMaxStageBytes+4096; rejects(a,Error::StageTooLarge); }
    { auto a=ready(); a.stageGpuVa+=1; rejects(a,Error::InvalidStageAddress); }
    { auto a=ready(); a.gartEnd=a.stageGpuVa+4096; rejects(a,Error::InvalidStageAddress); }
    { auto a=ready(); a.gartStart=a.stageGpuVa+4096; rejects(a,Error::InvalidStageAddress); }
    return 0;
}
