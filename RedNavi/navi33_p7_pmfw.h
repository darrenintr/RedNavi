#ifndef REDNAVI_NAVI33_P7_PMFW_H
#define REDNAVI_NAVI33_P7_PMFW_H
// SPDX-License-Identifier: BSD-3-Clause
// Pure P7.3 PMFW admission policy. No GPU I/O occurs here.

#include <stdint.h>

namespace rn_navi33 { namespace p7_pmfw {

enum class Error : uint8_t {
    Ok = 0,
    Disabled,
    PspNotReady,
    TocNotAccepted,
    NoTmrOwnership,
    FirmwareMissing,
    MetadataUnverified,
    DigestUntrusted,
    EmptyFirmware,
    FirmwareTooLarge,
};

struct Input {
    bool requested = false;
    bool pspReady = false;
    bool tocAccepted = false;
    bool tmrOwnershipVerified = false;
    bool firmwarePresent = false;
    bool metadataVerified = false;
    bool digestTrusted = false;
    uint64_t firmwareBytes = 0;
    uint64_t stageBytes = 0;
};

struct Report {
    Error error = Error::Disabled;
    bool mayStage = false;
    bool mayLoadPmfw = false;
};

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.pspReady) { r.error = Error::PspNotReady; return r; }
    if (!in.tocAccepted) { r.error = Error::TocNotAccepted; return r; }
    if (!in.tmrOwnershipVerified) {
        r.error = Error::NoTmrOwnership; return r;
    }
    if (!in.firmwarePresent) {
        r.error = Error::FirmwareMissing; return r;
    }
    if (!in.metadataVerified) {
        r.error = Error::MetadataUnverified; return r;
    }
    if (!in.digestTrusted) {
        r.error = Error::DigestUntrusted; return r;
    }
    if (!in.firmwareBytes) {
        r.error = Error::EmptyFirmware; return r;
    }
    if (!in.stageBytes || in.firmwareBytes > in.stageBytes) {
        r.error = Error::FirmwareTooLarge; return r;
    }
    r.mayStage = true;
    r.mayLoadPmfw = true;
    r.error = Error::Ok;
    return r;
}

}} // namespace rn_navi33::p7_pmfw
#endif
