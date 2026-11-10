// SPDX-License-Identifier: BSD-3-Clause
// PSP fw_pri DMA owner policy tests; no IOKit and no GPU I/O.
#include "navi33_fw_pri_dma_owner.h"
#include <cassert>
#include <cstdio>

namespace o = rn_navi33::fw_pri_owner;

static o::Input good() {
    o::Input in{};
    in.enabled = true;
    in.bufferAllocated = true;
    in.dmaPrepared = true;
    in.pagesVerified = true;
    in.bytes = o::kBytes;
    in.pageCount = o::kPages;
    return in;
}

int main() {
    static_assert(o::kBytes == (1ull << 20), "fw_pri must be 1 MiB");
    static_assert(o::kPages == 256u, "fw_pri must have 256 4 KiB pages");

    auto in = good();
    auto r = o::inspect(in);
    assert(r.error == o::Error::Ok);
    assert(r.ownerVerified);
    assert(!r.mayInstallGartPtes);

    in = good(); in.enabled = false;
    assert(o::inspect(in).error == o::Error::Disabled);
    in = good(); in.bufferAllocated = false;
    assert(o::inspect(in).error == o::Error::NoBuffer);
    in = good(); in.dmaPrepared = false;
    assert(o::inspect(in).error == o::Error::DmaNotPrepared);
    in = good(); in.pagesVerified = false;
    assert(o::inspect(in).error == o::Error::PagesNotVerified);
    in = good(); in.bytes -= 4096;
    assert(o::inspect(in).error == o::Error::BadLength);
    in = good(); in.pageCount = 255;
    assert(o::inspect(in).error == o::Error::BadPageCount);

    std::puts("Navi33 PSP fw_pri DMA ownership tests passed (NO GPU I/O)");
    return 0;
}
