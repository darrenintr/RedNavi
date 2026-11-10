#ifndef REDNAVI_NAVI33_P7_POSTPSP_H
#define REDNAVI_NAVI33_P7_POSTPSP_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P7.1 post-PSP dependency gate for Navi33. Pure policy only: it does not
// allocate memory, write PTEs, create the PSP ring, reserve TMR, load IP
// firmware, touch SMU/MES registers, or submit a graphics queue.
#include <stdint.h>

namespace rn_navi33 { namespace p7_postpsp {

constexpr uint64_t kPage = 4096ull;
constexpr uint32_t kRuntimePages = 3u; // ring, command/response, fence
constexpr uint32_t kRingPage = 256u;   // immediately after 1 MiB fw_pri
constexpr uint32_t kCmdPage = 257u;
constexpr uint32_t kFencePage = 258u;

enum class Error : uint8_t {
    Ok = 0,
    PspNotAlive,
    GartNotReady,
    NoRuntimeDmaOwner,
    BadRuntimeAddress,
    RingNotCreated,
    TocNotAccepted,
    BadTmrSize,
    NoTmrOwnership,
    TmrOutsideVram,
    TmrNotAccepted,
    IpFirmwareIncomplete,
    SmuNotReady,
    MesFirmwareIncomplete,
};

struct Input {
    bool pspAlive = false;
    bool gartReady = false;

    bool runtimeDmaOwner = false;
    uint64_t gartStart = 0;
    uint64_t gartEnd = 0;
    uint64_t ringGpuVa = 0;
    uint64_t cmdGpuVa = 0;
    uint64_t fenceGpuVa = 0;

    bool ringCreated = false;
    bool tocAccepted = false;
    uint64_t tmrBytes = 0;
    bool tmrOwnershipVerified = false;
    uint64_t tmrMc = 0;
    uint64_t fbStart = 0;
    uint64_t fbBytes = 0;
    bool tmrAccepted = false;

    bool gcFirmwareLoaded = false;
    bool sdmaFirmwareLoaded = false;
    bool smuFirmwareLoaded = false;
    bool smuReady = false;
    bool mesSchedulerLoaded = false;
    bool mesKiqLoaded = false;
};

struct Report {
    Error error = Error::PspNotAlive;
    bool mayMapRuntimePages = false;
    bool mayCreateRing = false;
    bool mayLoadToc = false;
    bool maySetupTmr = false;
    bool mayLoadIpFirmware = false;
    bool mayInitSmu = false;
    bool mayStartMes = false;
    bool mayCreateGraphicsQueue = false;
};

constexpr bool aligned(uint64_t v) { return (v & (kPage - 1u)) == 0; }
constexpr bool spanInside(uint64_t start, uint64_t bytes,
                          uint64_t begin, uint64_t endInclusive) {
    if (!bytes || begin > endInclusive || start < begin || start > endInclusive)
        return false;
    return bytes <= endInclusive - start + 1u;
}

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.pspAlive) return r;
    if (!in.gartReady) { r.error = Error::GartNotReady; return r; }
    if (!in.runtimeDmaOwner) { r.error = Error::NoRuntimeDmaOwner; return r; }

    const uint64_t va[3] = {in.ringGpuVa, in.cmdGpuVa, in.fenceGpuVa};
    for (unsigned i = 0; i < 3; ++i) {
        if (!aligned(va[i]) ||
            !spanInside(va[i], kPage, in.gartStart, in.gartEnd)) {
            r.error = Error::BadRuntimeAddress; return r;
        }
        for (unsigned j = 0; j < i; ++j)
            if (va[i] == va[j]) {
                r.error = Error::BadRuntimeAddress; return r;
            }
    }
    r.mayMapRuntimePages = true;
    r.mayCreateRing = true;

    if (!in.ringCreated) { r.error = Error::RingNotCreated; return r; }
    r.mayLoadToc = true;
    if (!in.tocAccepted) { r.error = Error::TocNotAccepted; return r; }

    if (!in.tmrBytes || !aligned(in.tmrBytes)) {
        r.error = Error::BadTmrSize; return r;
    }
    if (!in.tmrOwnershipVerified) {
        r.error = Error::NoTmrOwnership; return r;
    }
    if (!in.fbStart || !in.fbBytes ||
        in.fbStart > UINT64_MAX - (in.fbBytes - 1u) ||
        !aligned(in.tmrMc) ||
        !spanInside(in.tmrMc, in.tmrBytes, in.fbStart,
                    in.fbStart + in.fbBytes - 1u)) {
        r.error = Error::TmrOutsideVram; return r;
    }
    r.maySetupTmr = true;
    if (!in.tmrAccepted) { r.error = Error::TmrNotAccepted; return r; }

    r.mayLoadIpFirmware = true;
    if (!in.gcFirmwareLoaded || !in.sdmaFirmwareLoaded ||
        !in.smuFirmwareLoaded) {
        r.error = Error::IpFirmwareIncomplete; return r;
    }

    r.mayInitSmu = true;
    if (!in.smuReady) { r.error = Error::SmuNotReady; return r; }
    if (!in.mesSchedulerLoaded || !in.mesKiqLoaded) {
        r.error = Error::MesFirmwareIncomplete; return r;
    }

    r.mayStartMes = true;
    r.mayCreateGraphicsQueue = true;
    r.error = Error::Ok;
    return r;
}

}} // namespace rn_navi33::p7_postpsp
#endif
