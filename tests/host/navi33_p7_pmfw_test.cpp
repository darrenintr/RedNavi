#include "navi33_p7_pmfw.h"
#include <cassert>
using namespace rn_navi33::p7_pmfw;

static Input good() {
    Input in{};
    in.requested = true;
    in.pspReady = true;
    in.tocAccepted = true;
    in.tmrOwnershipVerified = true;
    in.firmwarePresent = true;
    in.metadataVerified = true;
    in.digestTrusted = true;
    in.firmwareBytes = 0x40000;
    in.stageBytes = 0x100000;
    return in;
}

int main() {
    {
        auto r = inspect(good());
        assert(r.error == Error::Ok);
        assert(r.mayStage && r.mayLoadPmfw);
    }
    {
        auto in = good(); in.requested = false;
        assert(inspect(in).error == Error::Disabled);
    }
    {
        auto in = good(); in.pspReady = false;
        assert(inspect(in).error == Error::PspNotReady);
    }
    {
        auto in = good(); in.tocAccepted = false;
        assert(inspect(in).error == Error::TocNotAccepted);
    }
    {
        auto in = good(); in.tmrOwnershipVerified = false;
        assert(inspect(in).error == Error::NoTmrOwnership);
    }
    {
        auto in = good(); in.firmwarePresent = false;
        assert(inspect(in).error == Error::FirmwareMissing);
    }
    {
        auto in = good(); in.metadataVerified = false;
        assert(inspect(in).error == Error::MetadataUnverified);
    }
    {
        auto in = good(); in.digestTrusted = false;
        assert(inspect(in).error == Error::DigestUntrusted);
    }
    {
        auto in = good(); in.firmwareBytes = 0;
        assert(inspect(in).error == Error::EmptyFirmware);
    }
    {
        auto in = good(); in.firmwareBytes = 0x100001;
        assert(inspect(in).error == Error::FirmwareTooLarge);
    }
    return 0;
}
