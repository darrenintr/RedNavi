#include "navi33_p7_postpsp.h"
#include <cassert>
using namespace rn_navi33::p7_postpsp;

static Input base() {
    Input i{};
    i.pspAlive = true;
    i.gartReady = true;
    i.runtimeDmaOwner = true;
    i.gartStart = 0x7fff00000000ull;
    i.gartEnd = 0x7fff1fffffffull;
    i.ringGpuVa = i.gartStart + (uint64_t)kRingPage * kPage;
    i.cmdGpuVa = i.gartStart + (uint64_t)kCmdPage * kPage;
    i.fenceGpuVa = i.gartStart + (uint64_t)kFencePage * kPage;
    return i;
}

int main() {
    {
        auto r = inspect(Input{});
        assert(r.error == Error::PspNotAlive);
        assert(!r.mayCreateRing);
    }
    {
        auto i = base();
        auto r = inspect(i);
        assert(r.error == Error::RingNotCreated);
        assert(r.mayMapRuntimePages && r.mayCreateRing);
    }
    {
        auto i = base();
        i.ringCreated = true;
        auto r = inspect(i);
        assert(r.error == Error::TocNotAccepted);
        assert(r.mayLoadToc);
    }
    {
        auto i = base();
        i.ringCreated = i.tocAccepted = true;
        i.tmrBytes = 0x1300000ull;
        i.tmrOwnershipVerified = true;
        i.fbStart = 0x8000000000ull;
        i.fbBytes = 0x1ff000000ull;
        i.tmrMc = 0x81fc000000ull;
        auto r = inspect(i);
        assert(r.error == Error::PmfwNotLoaded);
        assert(r.mayLoadPmfw);
        assert(!r.maySetupTmr);
    }
    {
        auto i = base();
        i.ringCreated = i.tocAccepted = true;
        i.tmrBytes = 0x1300000ull;
        i.tmrOwnershipVerified = true;
        i.fbStart = 0x8000000000ull;
        i.fbBytes = 0x1ff000000ull;
        i.tmrMc = 0x800d100000ull;
        i.pmfwLoaded = true;
        auto r = inspect(i);
        assert(r.error == Error::TmrNotAccepted);
        assert(r.mayLoadPmfw && r.maySetupTmr);
        assert(!r.mayLoadIpFirmware);
    }
    {
        auto i = base();
        i.ringCreated = i.tocAccepted = i.tmrOwnershipVerified =
            i.tmrAccepted = true;
        i.tmrBytes = 0x1300000ull;
        i.fbStart = 0x8000000000ull;
        i.fbBytes = 0x1ff000000ull;
        i.tmrMc = 0x81fc000000ull;
        i.pmfwLoaded = true;
        i.gcFirmwareLoaded = i.sdmaFirmwareLoaded = true;
        i.smuFirmwareLoaded = true;
        i.smuReady = true;
        i.mesSchedulerLoaded = i.mesKiqLoaded = true;
        auto r = inspect(i);
        assert(r.error == Error::Ok);
        assert(r.mayLoadPmfw && r.maySetupTmr);
        assert(r.mayLoadIpFirmware && r.mayInitSmu);
        assert(r.mayStartMes && r.mayCreateGraphicsQueue);
    }
    {
        auto i = base();
        i.cmdGpuVa = i.ringGpuVa;
        auto r = inspect(i);
        assert(r.error == Error::BadRuntimeAddress);
    }
    return 0;
}
