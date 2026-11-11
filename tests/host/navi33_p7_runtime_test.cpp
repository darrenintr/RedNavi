#include "navi33_p7_runtime.h"
#include <cassert>
using namespace rn_navi33::p7_runtime;

static Input ready(uint32_t step) {
    Input in{};
    in.step = step;
    in.identityVerified = true;
    in.memoryVerified = true;
    in.ledgerOwned = true;
    in.carveVerified = true;
    in.runtimeQuiescent = true;
    in.dmaOwnerVerified = true;
    in.gartOwnerVerified = true;
    in.firmwareMetadataVerified = true;
    in.firmwareDigestTrusted = true;
    return in;
}

int main() {
    {
        assert(sanitizeStep(0) == 0u);
        assert(sanitizeStep(1) == 1u);
        assert(sanitizeStep(3) == 3u);
        assert(sanitizeStep(4) == 4u);
        assert(sanitizeStep(5) == 5u);
        assert(sanitizeStep(6) == 6u);
        assert(sanitizeStep(7) == 7u);
        assert(sanitizeStep(8) == 0u);
    }
    {
        auto r = inspect(Input{});
        assert(r.error == Error::Disabled);
        assert(!r.mayProgramGart);
    }
    {
        auto in = ready(1);
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayProgramGart);
        assert(!r.mayIssuePspBootloader);
    }
    {
        auto in = ready(2);
        in.firmwareDigestTrusted = false;
        auto r = inspect(in);
        assert(r.error == Error::FirmwareDigestUntrusted);
        assert(r.mayProgramGart);
        assert(!r.mayCopyFirmware);
    }
    {
        auto in = ready(2);
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayProgramGart && r.mayCopyFirmware && r.mayIssuePspBootloader);
        assert(!r.mayStartQueues);
    }
    {
        auto in = ready(3);
        auto r = inspect(in);
        assert(r.error == Error::SecureOsNotAlive);
        assert(!r.mayStartQueues);
        in.secureOsAlive = true;
        r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(4);
        auto r = inspect(in);
        assert(r.error == Error::SecureOsNotAlive);
        in.secureOsAlive = true;
        r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayProgramGart);
        assert(r.mayIssuePspBootloader);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(5);
        auto r = inspect(in);
        assert(r.error == Error::SecureOsNotAlive);
        in.secureOsAlive = true;
        r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayProgramGart);
        assert(r.mayIssuePspBootloader);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(6);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(7);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(3);
        in.runtimeQuiescent = false;
        auto r = inspect(in);
        assert(r.error == Error::RuntimeBusy);
        assert(!r.mayProgramGart);
    }
    return 0;
}
