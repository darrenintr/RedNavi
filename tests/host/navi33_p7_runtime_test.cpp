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
        assert(sanitizeStep(8) == 8u);
        assert(sanitizeStep(9) == 9u);
        assert(sanitizeStep(10) == 10u);
        assert(sanitizeStep(11) == 11u);
        assert(sanitizeStep(12) == 12u);
        assert(sanitizeStep(13) == 13u);
        assert(sanitizeStep(14) == 14u);
        assert(sanitizeStep(15) == 15u);
        assert(sanitizeStep(16) == 16u);
        assert(sanitizeStep(17) == 17u);
        assert(sanitizeStep(18) == 18u);
        assert(sanitizeStep(19) == 19u);
        assert(sanitizeStep(20) == 20u);
        assert(sanitizeStep(21) == 0u);
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
        auto in = ready(8);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(9);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(10);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(11);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(12);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(13);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(14);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(15);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(16);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    {
        auto in = ready(17);
        in.secureOsAlive = true;
        auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayStartQueues);
    }
    for (uint32_t step = 18u; step <= 20u; ++step) {
        auto in = ready(step);
        in.secureOsAlive = true;
        const auto r = inspect(in);
        assert(r.error == Error::Ok);
        assert(r.mayIssuePspBootloader);
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
