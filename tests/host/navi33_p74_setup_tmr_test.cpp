// SPDX-License-Identifier: BSD-3-Clause
#include "navi33_p74_setup_tmr.h"
#include <cassert>
#include <cstdint>
using namespace rn_navi33::p74_tmr;

static Input good() {
    Input in{};
    in.requested = true;
    in.explicitOptIn = true;
    in.identityVerified = true;
    in.pspReady = true;
    in.postPspMaySetupTmr = true;
    in.pmfwAccepted = true;
    in.transportHealthy = true;
    in.previousFenceExpected = 2u;
    in.previousFenceObserved = 2u;
    in.tmrOwnershipVerified = true;
    in.allocatorQuiescent = true;
    in.carveVerified = true;
    in.naturalAligned = true;
    in.noOverlap = true;
    in.tocBytes = 0x01300000ull;
    in.tmrBytes = 0x01300000ull;
    in.tmrOffset = 0x0d100000ull;
    in.mcStart = 0x8000000000ull;
    in.physBase = 0;
    in.mc = 0x800d100000ull;
    in.pa = 0x0d100000ull;
    in.visibleBytes = 0x10000000ull;
    in.carveStart = 0x0d000000ull;
    in.carveBytes = 0x02efe000ull;
    return in;
}

static void reject(Input in, Error expected) {
    const auto report = inspect(in);
    assert(report.error == expected);
    assert(!report.mayIssue);
    for (auto w : report.words) assert(w == 0u);
}

int main() {
    static_assert(kCommandId == 5u, "Linux PSP GFX_CMD_ID_SETUP_TMR");
    static_assert(kArgCount == 6u, "PSP TMR has six DWORD fields");
    static_assert(kVirtPhyAddr == 2u, "bit 1 is virt_phy_addr");
    {
        auto r = inspect(good());
        assert(r.error == Error::Ok && r.mayIssue);
        // Verified RX 7600 TMR MC/PA; NEVER substitute PCI BAR physical addr.
        assert(r.words[0] == 0x0d100000u);
        assert(r.words[1] == 0x00000080u);
        assert(r.words[2] == 0x01300000u);
        assert(r.words[3] == 0x00000002u);
        assert(r.words[4] == 0x0d100000u);
        assert(r.words[5] == 0u);
    }
    { auto in=good(); in.requested=false; reject(in, Error::Disabled); }
    { auto in=good(); in.explicitOptIn=false; reject(in, Error::NoExplicitOptIn); }
    { auto in=good(); in.identityVerified=false; reject(in, Error::IdentityUnverified); }
    { auto in=good(); in.pspReady=false; reject(in, Error::PspNotReady); }
    { auto in=good(); in.postPspMaySetupTmr=false; reject(in, Error::PostPspNotAdmitted); }
    { auto in=good(); in.pmfwAccepted=false; reject(in, Error::PmfwNotAccepted); }
    { auto in=good(); in.transportHealthy=false; reject(in, Error::TransportUnhealthy); }
    { auto in=good(); in.previousFenceExpected=0; reject(in, Error::PreviousFenceMismatch); }
    { auto in=good(); in.previousFenceObserved=0; reject(in, Error::PreviousFenceMismatch); }
    { auto in=good(); in.tmrOwnershipVerified=false; reject(in, Error::NoTmrOwnership); }
    { auto in=good(); in.allocatorQuiescent=false; reject(in, Error::AllocatorNotQuiescent); }
    { auto in=good(); in.carveVerified=false; reject(in, Error::CarveNotVerified); }
    { auto in=good(); in.tocBytes+=4096; reject(in, Error::BadTocSize); }
    { auto in=good(); in.tmrBytes=0; in.tocBytes=0; reject(in, Error::BadTmrSize); }
    { auto in=good(); in.tmrBytes=0x01280000; in.tocBytes=in.tmrBytes; reject(in, Error::BadTmrSize); }
    { auto in=good(); in.naturalAligned=false; reject(in, Error::BadAlignment); }
    { auto in=good(); in.tmrOffset+=0x100000ull; in.mc+=0x100000ull; in.pa+=0x100000ull; reject(in, Error::BadAlignment); }
    { auto in=good(); in.mc+=4096; reject(in, Error::BadGeometry); }
    { auto in=good(); in.pa+=4096; reject(in, Error::BadGeometry); }
    { auto in=good(); in.mc=in.pa; reject(in, Error::BadGeometry); }
    { auto in=good(); in.visibleBytes=0x0e000000ull; reject(in, Error::OutsideVisibleVram); }
    { auto in=good(); in.carveBytes=0x01000000ull; reject(in, Error::OutsideCarvedRegion); }
    { auto in=good(); in.noOverlap=false; reject(in, Error::Overlap); }
    {
        auto in=good();
        in.physBase=0x200000000ull;
        in.pa=in.physBase+in.tmrOffset;
        auto r=inspect(in);
        assert(r.mayIssue && r.words[4] == 0x0d100000u);
        assert(r.words[5] == 2u);
    }
    return 0;
}
