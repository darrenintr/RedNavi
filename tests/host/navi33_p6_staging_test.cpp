// SPDX-License-Identifier: BSD-3-Clause
// Navi33 P6.2 PSP GART/DMA staging checks; host only, no GPU I/O.
#include "navi33_p6_psp_staging.h"
#include <cassert>
#include <cstdio>

namespace st = rn_navi33::p6_staging;
namespace boot = rn_navi33::p6_boot;

static st::Input good() {
    st::Input in{};
    in.p6PreflightPassed = true;
    in.firmwareBytesVerified = true;
    in.firmwareMetadataVerified = true;
    in.maxFirmwareComponentBytes = 384u << 10;

    in.gartOwnershipVerified = true;
    in.gartStart = 0x7fff00000000ull;
    in.gartEnd = 0x7fff1fffffffull;
    in.fwPriGpuVa = in.gartStart;
    in.fwPriBytes = boot::kStageBytes;

    in.dmaMappingRetained = true;
    in.dmaPagesVerified = true;
    in.dmaBytes = boot::kStageBytes;
    return in;
}

int main() {
    auto in = good();
    auto r = st::validate(in);
    assert(r.error == st::Error::Ok);
    assert(r.fwPriGpuVa == 0x7fff00000000ull);
    assert(r.fwPriBytes == boot::kStageBytes);
    assert(!r.mayCopyFirmware);
    assert(!r.mayIssuePspCommands);

    in = good(); in.p6PreflightPassed = false;
    assert(st::validate(in).error == st::Error::NoPreflight);
    in = good(); in.firmwareBytesVerified = false;
    assert(st::validate(in).error == st::Error::NoFirmwareProof);
    in = good(); in.firmwareMetadataVerified = false;
    assert(st::validate(in).error == st::Error::NoFirmwareProof);
    in = good(); in.maxFirmwareComponentBytes = boot::kStageBytes + 1u;
    assert(st::validate(in).error == st::Error::BadFirmwareLength);
    in = good(); in.gartOwnershipVerified = false;
    assert(st::validate(in).error == st::Error::NoGartOwnership);
    in = good(); in.gartStart = 0;
    assert(st::validate(in).error == st::Error::BadGartWindow);
    in = good(); in.fwPriGpuVa += st::kPage;
    assert(st::validate(in).error == st::Error::BadFwPriAddress);
    in = good(); in.fwPriGpuVa = in.gartEnd - boot::kStageBytes + 2u;
    assert(st::validate(in).error == st::Error::BadFwPriAddress);
    in = good(); in.dmaMappingRetained = false;
    assert(st::validate(in).error == st::Error::NoDmaOwnership);
    in = good(); in.dmaPagesVerified = false;
    assert(st::validate(in).error == st::Error::NoDmaOwnership);
    in = good(); in.dmaBytes -= st::kPage;
    assert(st::validate(in).error == st::Error::BadDmaSpan);

    std::puts("Navi33 P6.2 PSP GART/DMA staging tests passed (NO GPU I/O)");
    return 0;
}
