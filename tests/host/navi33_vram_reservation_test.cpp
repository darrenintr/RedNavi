// SPDX-License-Identifier: BSD-3-Clause
// Read-only Navi33 VRAM reservation planner tests.
#include "navi33_vram_reservation.h"
#include <cassert>
#include <cstdio>

namespace v = rn_navi33::vram_reservation;

int main() {
    v::Input in{};
    in.barPhys = 0x830000000ull;
    in.barBytes = 256ull << 20;
    in.cpuVisibleBytes = 256ull << 20;
    in.consolePhys = in.barPhys;
    in.consoleRowBytes = 1920ull * 4ull;
    in.consoleHeight = 1080;

    auto p = v::inspect(in);
    assert(p.error == v::Error::Ok);
    assert(p.consoleInsideBar);
    assert(p.console.offset == 0);
    assert(p.console.bytes == 8294400ull + 128ull);
    assert(p.pageTable.offset == 255ull << 20);
    assert(p.pageTable.bytes == 1ull << 20);
    assert(p.scratch.offset == (255ull << 20) - 8192ull);
    assert(p.dummy.offset == (255ull << 20) - 4096ull);
    assert(p.candidateNonOverlapping);
    assert(!p.reservationOwned);
    assert(!p.mayWriteVram);

    in.consolePhys = in.barPhys + (255ull << 20);
    in.consoleRowBytes = 4096;
    in.consoleHeight = 512;
    assert(v::inspect(in).error == v::Error::ConsoleOutsideBar);

    in.consolePhys = in.barPhys + (254ull << 20);
    in.consoleRowBytes = 4096;
    in.consoleHeight = 256;
    assert(v::inspect(in).error == v::Error::CandidateOverlap);

    in = {};
    in.barPhys = 0x830000000ull;
    in.barBytes = 256ull << 20;
    in.cpuVisibleBytes = 512ull << 10;
    in.consolePhys = in.barPhys;
    in.consoleRowBytes = 4096;
    in.consoleHeight = 1;
    assert(v::inspect(in).error == v::Error::TooLittleVisibleVram);

    std::puts("Navi33 VRAM reservation planner tests passed (NO VRAM WRITE)");
    return 0;
}
