// SPDX-License-Identifier: BSD-3-Clause
#include "navi33_p75_sdma.h"
#include <cassert>
using namespace rn_navi33::p75_sdma;

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
    in.previousFenceExpected = 3;
    in.previousFenceObserved = 3;
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
    in.firmwareBytes = 16384;
    in.stageBytes = 262144;
    in.stageGpuVa = 0x7fff00103000ull;
    in.gartStart = 0x7fff00000000ull;
    in.gartEnd = in.gartStart + (1ull << 29) - 1;
    return in;
}

static void rejects(Input in, Error why) {
    const Report r = inspect(in);
    assert(r.error == why);
    assert(!r.mayLoad);
    for (uint32_t v : r.words) assert(v == 0u);
}

int main() {
    static_assert(kCommandId == 6u, "LOAD_IP_FW = 0x06");
    static_assert(kFirmwareType == 71u, "Navi33 SDMA Context Thread = 71");
    static_assert(kPriorFence == 3u && kExpectedFence == 4u, "fence progression");
    {
        const auto r = inspect(ready());
        assert(r.error == Error::Ok && r.mayLoad);
        assert(r.words[0] == 0x00103000u);
        assert(r.words[1] == 0x00007fffu);
        assert(r.words[2] == 16384u);
        assert(r.words[3] == 71u);
    }
    { auto in=ready(); in.requested=false; rejects(in,Error::Disabled); }
    { auto in=ready(); in.explicitOptIn=false; rejects(in,Error::NoExplicitOptIn); }
    { auto in=ready(); in.identityVerified=false; rejects(in,Error::IdentityUnverified); }
    { auto in=ready(); in.observedSdmaRevision=3; rejects(in,Error::SdmaIpMismatch); }
    { auto in=ready(); in.pspRingReady=false; rejects(in,Error::NoPspRing); }
    { auto in=ready(); in.tmrAccepted=false; rejects(in,Error::TmrNotAccepted); }
    { auto in=ready(); in.previousFenceExpected=2; rejects(in,Error::PreviousFenceMismatch); }
    { auto in=ready(); in.previousFenceObserved=2; rejects(in,Error::PreviousFenceMismatch); }
    { auto in=ready(); in.transportHealthy=false; rejects(in,Error::TransportUnhealthy); }
    { auto in=ready(); in.stageOwnerRetained=false; rejects(in,Error::NoStagingOwner); }
    { auto in=ready(); in.stageMapped=false; rejects(in,Error::NoStageMapping); }
    { auto in=ready(); in.stagePteVerified=false; rejects(in,Error::StagePteUnverified); }
    { auto in=ready(); in.firmwarePresent=false; rejects(in,Error::FirmwareMissing); }
    { auto in=ready(); in.firmwareMetadataVerified=false; rejects(in,Error::FirmwareMetadataUnverified); }
    { auto in=ready(); in.firmwareSourceVerified=false; rejects(in,Error::FirmwareSourceUnverified); }
    { auto in=ready(); in.firmwareIpRevision=3; rejects(in,Error::FirmwareTargetMismatch); }
    { auto in=ready(); in.firmwareBytes=0; rejects(in,Error::EmptyFirmware); }
    { auto in=ready(); in.firmwareBytes=8193; rejects(in,Error::EmptyFirmware); }
    { auto in=ready(); in.stageBytes=8192; rejects(in,Error::StageTooSmall); }
    { auto in=ready(); in.stageBytes=kMaxStageBytes+4096; rejects(in,Error::StageTooLarge); }
    { auto in=ready(); in.stageGpuVa+=1; rejects(in,Error::InvalidStageAddress); }
    { auto in=ready(); in.gartEnd=in.stageGpuVa + 0x1000; rejects(in,Error::InvalidStageAddress); }
    { auto in=ready(); in.gartStart=in.stageGpuVa+4096; rejects(in,Error::InvalidStageAddress); }
    return 0;
}
