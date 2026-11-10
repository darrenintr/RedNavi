// SPDX-License-Identifier: BSD-3-Clause
#include "navi33_p7_tmr_plan.h"
#include <cassert>
#include <cstdio>

namespace t = rn_navi33::p7_tmr;

static t::Input good() {
    t::Input in{};
    in.requested = true;
    in.tocAccepted = true;
    in.allocatorCarveVerified = true;
    in.allocatorQuiescent = true;
    in.tmrBytes = 0x01300000ull;       // 19 MiB, observed on RX 7600
    in.cpuVisibleBytes = 0x10000000ull; // 256 MiB BAR-visible VRAM
    in.mcStart = 0x8000000000ull;
    in.vramPhysBase = 0;               // MMMC_VM_FB_OFFSET == 0
    in.freeRange = {0x0d000000ull, 0x02efe000ull};
    in.console = {0, 0x00800080ull};
    in.priorReservation = {0x0fefe000ull, 0x00102000ull};
    return in;
}

int main() {
    {
        const auto r = t::inspect(good());
        assert(r.error == t::Error::Ok);
        assert(r.candidate.offset == 0x0d100000ull);
        assert(r.candidate.bytes == 0x01300000ull);
        assert(r.mc == 0x800d100000ull);
        assert(r.pa == 0x0d100000ull);
        assert(r.naturalAligned);
        assert(r.insideVisibleVram);
        assert(r.noOverlap);
        assert(r.ownershipVerified);
        assert(!r.mayWriteVram);
        assert(!r.mayIssueSetupTmr);
    }
    {
        auto in = good();
        in.requested = false;
        assert(t::inspect(in).error == t::Error::Disabled);
    }
    {
        auto in = good();
        in.tocAccepted = false;
        assert(t::inspect(in).error == t::Error::TocNotAccepted);
    }
    {
        auto in = good();
        in.allocatorCarveVerified = false;
        assert(t::inspect(in).error == t::Error::NoAllocatorCarve);
    }
    {
        auto in = good();
        in.allocatorQuiescent = false;
        assert(t::inspect(in).error == t::Error::AllocatorBusy);
    }
    {
        auto in = good();
        in.tmrBytes = 0x01280000ull;
        assert(t::inspect(in).error == t::Error::BadTmrSize);
    }
    {
        auto in = good();
        in.freeRange = {0x0f000000ull, 0x00efe000ull};
        assert(t::inspect(in).error == t::Error::NoNaturalFit);
    }
    {
        auto in = good();
        in.console = {0x0d000000ull, 0x02000000ull};
        assert(t::inspect(in).error == t::Error::Overlap);
    }
    {
        auto in = good();
        in.vramPhysBase = 0x200000000ull;
        const auto r = t::inspect(in);
        assert(r.error == t::Error::Ok);
        assert(r.pa == 0x20d100000ull);
    }

    std::puts("Navi33 P7.2 TMR ownership planner tests passed (NO GPU I/O)");
    return 0;
}
