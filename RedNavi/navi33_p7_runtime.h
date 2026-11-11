#ifndef REDNAVI_NAVI33_P7_RUNTIME_H
#define REDNAVI_NAVI33_P7_RUNTIME_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Navi33 P7 execution gate. This is a pure policy contract: no MMIO, DMA,
// firmware copy, allocator mutation or queue submission occurs here.
//
// step 0: observation only
// step 1: install the already-owned VMID0/GART mapping
// step 2: additionally allow the verified PSP13.0.7 boot chain
// step 3: PSP KM/GPCOM ring + LOAD_TOC after sOS is alive
// step 4: software-only TMR ownership checkpoint
// step 5: PMFW-only LOAD_IP_FW checkpoint; SETUP_TMR remains blocked
// step 6: optionally submit SETUP_TMR after a second explicit boot opt-in
// step 7: optional SDMA6.0.2 Context Thread PSP firmware checkpoint
// step 8: optional SDMA6.0.2 Control Thread TH1 PSP firmware checkpoint
// step 9: optional GC11.0.2 RS64 PFP instruction-only PSP checkpoint
// step 10: optional RS64 PFP P0_STACK data-only PSP checkpoint
// step 11: optional RS64 PFP P1_STACK data-only PSP checkpoint
// step 12: optional RS64 ME instruction-only PSP checkpoint (type 88)
// step 13: optional RS64 ME P0_STACK PSP checkpoint (type 92)
// step 14: optional RS64 ME P1_STACK PSP checkpoint (type 93)
// step 15: independently consented MEC Code stage enlargement + separate PSP type 89
// step 16: independently consented MEC P0_STACK data-only PSP checkpoint (type 94)
// step 17: independently consented MEC P1_STACK data-only PSP checkpoint (type 95)
// step 18: independently consented MEC P2_STACK data-only PSP checkpoint (type 96)
// step 19: independently consented MEC P3_STACK data-only PSP checkpoint (type 97)
#include <stdint.h>

namespace rn_navi33 { namespace p7_runtime {

constexpr uint32_t kMaxExecutionStep = 19u;

constexpr uint32_t sanitizeStep(uint32_t raw) {
    return raw <= kMaxExecutionStep ? raw : 0u;
}

enum class Error : uint8_t {
    Ok = 0,
    Disabled,
    IdentityUnverified,
    MemoryUnverified,
    NoLedger,
    CarveUnverified,
    RuntimeBusy,
    NoDmaOwner,
    NoGartOwner,
    FirmwareMetadataUnverified,
    FirmwareDigestUntrusted,
    SecureOsNotAlive,
};

struct Input {
    uint32_t step = 0;
    bool identityVerified = false;
    bool memoryVerified = false;
    bool ledgerOwned = false;
    bool carveVerified = false;
    bool runtimeQuiescent = false;
    bool dmaOwnerVerified = false;
    bool gartOwnerVerified = false;
    bool firmwareMetadataVerified = false;
    bool firmwareDigestTrusted = false;
    bool secureOsAlive = false;
};

struct Report {
    Error error = Error::Disabled;
    uint32_t step = 0;
    bool mayProgramGart = false;
    bool mayCopyFirmware = false;
    bool mayIssuePspBootloader = false;
    bool mayStartQueues = false;
};

inline Report inspect(const Input &in) {
    Report r{};
    r.step = in.step;
    if (in.step == 0) return r;
    if (!in.identityVerified) { r.error = Error::IdentityUnverified; return r; }
    if (!in.memoryVerified) { r.error = Error::MemoryUnverified; return r; }
    if (!in.ledgerOwned) { r.error = Error::NoLedger; return r; }
    if (!in.carveVerified) { r.error = Error::CarveUnverified; return r; }
    if (!in.runtimeQuiescent) { r.error = Error::RuntimeBusy; return r; }
    if (!in.dmaOwnerVerified) { r.error = Error::NoDmaOwner; return r; }
    if (!in.gartOwnerVerified) { r.error = Error::NoGartOwner; return r; }

    r.mayProgramGart = true;
    if (in.step == 1) { r.error = Error::Ok; return r; }

    if (!in.firmwareMetadataVerified) {
        r.error = Error::FirmwareMetadataUnverified; return r;
    }
    if (!in.firmwareDigestTrusted) {
        r.error = Error::FirmwareDigestUntrusted; return r;
    }
    r.mayCopyFirmware = true;
    r.mayIssuePspBootloader = true;

    if (in.step == 2) { r.error = Error::Ok; return r; }
    if (!in.secureOsAlive) {
        r.error = Error::SecureOsNotAlive; return r;
    }
    r.mayStartQueues = true;
    r.error = Error::Ok;
    return r;
}

}} // namespace rn_navi33::p7_runtime
#endif
