#ifndef REDNAVI_NAVI33_P6_PSP_BOOT_PLAN_H
#define REDNAVI_NAVI33_P6_PSP_BOOT_PLAN_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Navi33 / PSP 13.0.7 boot-firmware evidence contract.
// Host-only policy: this validates metadata produced from a local
// psp_13_0_7_sos.bin. It performs no hashing, allocation, MMIO or PSP command.
//
// Linux PSP13 uses a reusable 1 MiB fw_pri staging buffer and copies one
// bootloader component at a time. psp_hw_start conditionally loads the
// components present in the SOS package, then requires SYS_DRV + SOS before
// a useful sOS/ring can exist. This header never authorizes those writes.
#include <stdint.h>

namespace rn_navi33 { namespace p6_boot {

constexpr uint8_t kTargetMajor = 13u;
constexpr uint8_t kTargetMinor = 0u;
constexpr uint8_t kTargetRevision = 7u;
constexpr uint32_t kStageBytes = 1u << 20; // Linux PSP_1_MEG
constexpr uint32_t kMaxContainerBytes = 16u << 20;

enum ComponentBit : uint32_t {
    Sos       = 1u << 0,
    SysDrv    = 1u << 1,
    Kdb       = 1u << 2,
    Toc       = 1u << 3,
    Spl       = 1u << 4,
    Rl        = 1u << 5,
    SocDrv    = 1u << 6,
    IntfDrv   = 1u << 7,
    DbgDrv    = 1u << 8,
    RasDrv    = 1u << 9,
    IpKeyMgr  = 1u << 10,
    SpdmDrv   = 1u << 11,
};

constexpr uint32_t kRequiredMask = Sos | SysDrv;

enum class Error : uint8_t {
    Ok,
    WrongTarget,
    MissingSourceDigest,
    BadContainerSize,
    MissingSystemDriver,
    MissingSecureOs,
    MissingComponents,
    ComponentTooLarge,
};

struct Evidence {
    uint8_t major = 0, minor = 0, revision = 0;
    bool exactTargetName = false;
    bool sourceDigestPresent = false;
    uint32_t sourceBytes = 0;
    uint32_t componentMask = 0;
    uint32_t maxComponentBytes = 0;
};

struct Report {
    Error error = Error::WrongTarget;
    uint32_t requiredMask = kRequiredMask;
    uint32_t presentMask = 0;
    uint32_t stageBytes = kStageBytes;
    bool metadataVerified = false;
    bool mayLoadPsp = false; // ALWAYS false: ownership + hardware opt-in are separate.
};

inline Report inspect(const Evidence &e) {
    Report r{};
    r.presentMask = e.componentMask;
    if (e.major != kTargetMajor || e.minor != kTargetMinor ||
        e.revision != kTargetRevision || !e.exactTargetName)
        return r;
    if (!e.sourceDigestPresent) {
        r.error = Error::MissingSourceDigest;
        return r;
    }
    if (e.sourceBytes != kExpectedSourceBytes ||
        e.sourceBytes > kMaxContainerBytes) {
        r.error = Error::BadContainerSize;
        return r;
    }
    if (!(e.componentMask & SysDrv)) {
        r.error = Error::MissingSystemDriver;
        return r;
    }
    if (!(e.componentMask & Sos)) {
        r.error = Error::MissingSecureOs;
        return r;
    }
    if ((e.componentMask & kRequiredMask) != kRequiredMask) {
        r.error = Error::MissingComponents;
        return r;
    }
    if (!e.maxComponentBytes || e.maxComponentBytes > kStageBytes) {
        r.error = Error::ComponentTooLarge;
        return r;
    }
    r.error = Error::Ok;
    r.metadataVerified = true;
    return r;
}

}} // namespace rn_navi33::p6_boot
#endif
