// SPDX-License-Identifier: BSD-3-Clause
// P6.2 PSP13.0.7 boot-firmware metadata contract; host only.
#include "navi33_p6_psp_boot_plan.h"
#include <cassert>
#include <cstdio>

namespace p = rn_navi33::p6_boot;

static p::Evidence good() {
    p::Evidence e{};
    e.major = 13;
    e.minor = 0;
    e.revision = 7;
    e.exactTargetName = true;
    e.sourceDigestPresent = true;
    e.sourceBytes = 2u << 20;
    e.componentMask = p::SysDrv | p::Sos | p::Kdb | p::SocDrv;
    e.maxComponentBytes = 512u << 10;
    return e;
}

int main() {
    static_assert(p::kStageBytes == (1u << 20), "Linux PSP fw_pri is 1 MiB");

    auto e = good();
    auto r = p::inspect(e);
    assert(r.error == p::Error::Ok);
    assert(r.metadataVerified);
    assert(!r.mayLoadPsp);

    e = good(); e.revision = 10;
    assert(p::inspect(e).error == p::Error::WrongTarget);
    e = good(); e.exactTargetName = false;
    assert(p::inspect(e).error == p::Error::WrongTarget);
    e = good(); e.sourceDigestPresent = false;
    assert(p::inspect(e).error == p::Error::MissingSourceDigest);
    e = good(); e.sourceBytes = 0;
    assert(p::inspect(e).error == p::Error::BadContainerSize);
    e = good(); e.sourceBytes = p::kMaxContainerBytes + 1u;
    assert(p::inspect(e).error == p::Error::BadContainerSize);
    e = good(); e.componentMask &= ~p::SysDrv;
    assert(p::inspect(e).error == p::Error::MissingSystemDriver);
    e = good(); e.componentMask &= ~p::Sos;
    assert(p::inspect(e).error == p::Error::MissingSecureOs);
    e = good(); e.maxComponentBytes = p::kStageBytes + 1u;
    assert(p::inspect(e).error == p::Error::ComponentTooLarge);

    std::puts("Navi33 P6.2 PSP boot metadata tests passed (NO GPU I/O)");
    return 0;
}
