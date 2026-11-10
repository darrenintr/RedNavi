// SPDX-License-Identifier: BSD-3-Clause
// P6.5 software-only VRAM reservation ledger tests.
#include "navi33_vram_ledger.h"
#include <cassert>
#include <cstdio>

namespace l = rn_navi33::vram_ledger;

static l::Input good() {
    l::Input in{};
    in.requested = true;
    in.geometryVerified = true;
    in.consoleVerified = true;
    in.allocatorInactive = true;
    in.cpuVisibleBytes = 256ull << 20;
    in.console = {0, 8294528ull};
    in.scratch = {0x0fefe000ull, l::kPage};
    in.dummy = {0x0feff000ull, l::kPage};
    in.pageTable = {0x0ff00000ull, l::kPageTableBytes};
    return in;
}

int main() {
    auto in = good();
    auto r = l::inspect(in);
    assert(r.error == l::Error::Ok);
    assert(r.reservedBytes == (1ull << 20) + 8192ull);
    assert(r.ledgerOwned);
    assert(r.allocatorCarveRequired);
    assert(!r.mayWriteVram);
    assert(!r.mayInstallGartPtes);

    in = good(); in.requested = false;
    assert(l::inspect(in).error == l::Error::Disabled);
    in = good(); in.geometryVerified = false;
    assert(l::inspect(in).error == l::Error::BadGeometry);
    in = good(); in.consoleVerified = false;
    assert(l::inspect(in).error == l::Error::ConsoleUnverified);
    in = good(); in.allocatorInactive = false;
    assert(l::inspect(in).error == l::Error::AllocatorActive);
    in = good(); in.pageTable.offset += l::kPage;
    assert(l::inspect(in).error == l::Error::BadCandidate);
    in = good(); in.dummy.offset += l::kPage;
    assert(l::inspect(in).error == l::Error::BadCandidate);
    in = good(); in.console = {0x0feff000ull, 8192ull};
    assert(l::inspect(in).error == l::Error::Overlap);

    std::puts("Navi33 P6.5 VRAM ledger tests passed (NO VRAM WRITE)");
    return 0;
}
