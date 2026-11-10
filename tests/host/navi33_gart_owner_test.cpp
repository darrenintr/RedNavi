// SPDX-License-Identifier: BSD-3-Clause
// Navi33 GMC11/GART ownership planner tests; no GPU I/O.
#include "navi33_gart_owner.h"
#include <cassert>
#include <cstdio>

namespace g = rn_navi33::gart_owner;

static g::Input good() {
    g::Input in{};
    in.fbStart = 0x8000000000ull;
    in.fbBytes = 8176ull << 20;
    in.cpuVisibleBytes = 256ull << 20;
    in.gartBytes = 512ull << 20;

    // Illustrative caller-owned offsets only. The planner does not allocate.
    in.pageTable = {0x02000000ull, g::kDefaultPtBytes};
    in.scratch = {0x02100000ull, g::kPage};
    in.dummy = {0x02101000ull, g::kPage};
    in.vramOwnershipVerified = true;
    in.reservationMapVerified = true;

    in.dmaBytes = g::kPspFwPriBytes;
    in.dmaPageCount = static_cast<uint32_t>(g::kPspFwPriBytes / g::kPage);
    in.dmaMappingRetained = true;
    in.dmaPagesVerified = true;
    return in;
}

int main() {
    static_assert(g::kDefaultPtBytes == (1ull << 20),
                  "512 MiB GART needs a 1 MiB 64-bit PTE table");
    assert(g::linuxHighGartStart(512ull << 20) == 0x7fff00000000ull);

    auto in = good();
    auto p = g::inspect(in);
    assert(p.error == g::Error::Ok);
    assert(p.gartStart == 0x7fff00000000ull);
    assert(p.gartEnd == 0x7fff1fffffffull);
    assert(p.pageTableBytes == 1ull << 20);
    assert(p.fwPriGpuAddress == p.gartStart);
    assert(p.fwPriPageCount == 256);
    assert(p.ownershipVerified);
    assert(!p.mayProgramGart);

    in = good(); in.vramOwnershipVerified = false;
    assert(g::inspect(in).error == g::Error::NoVramOwnership);
    in = good(); in.reservationMapVerified = false;
    assert(g::inspect(in).error == g::Error::NoReservationMap);
    in = good(); in.pageTable.bytes -= g::kPage;
    assert(g::inspect(in).error == g::Error::BadPtRange);
    in = good(); in.pageTable.offset = (256ull << 20) - (512ull << 10);
    assert(g::inspect(in).error == g::Error::PtNotCpuVisible);
    in = good(); in.scratch.offset = in.pageTable.offset;
    assert(g::inspect(in).error == g::Error::AuxOverlap);
    in = good(); in.dummy.bytes = 2 * g::kPage;
    assert(g::inspect(in).error == g::Error::BadAuxPage);
    in = good(); in.dmaMappingRetained = false;
    assert(g::inspect(in).error == g::Error::NoDmaOwnership);
    in = good(); in.dmaPageCount = 255;
    assert(g::inspect(in).error == g::Error::BadDmaSpan);
    in = good(); in.gartBytes = 511ull << 20;
    // Page-aligned but Linux high-placement PTE table no longer equals the
    // default 1 MiB caller reservation, so the supplied range is rejected.
    assert(g::inspect(in).error == g::Error::BadPtRange);

    std::puts("Navi33 GMC11/GART ownership planner tests passed (NO GPU I/O)");
    return 0;
}
