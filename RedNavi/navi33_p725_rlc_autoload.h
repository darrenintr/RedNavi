#ifndef REDNAVI_NAVI33_P725_RLC_AUTOLOAD_H
#define REDNAVI_NAVI33_P725_RLC_AUTOLOAD_H
// SPDX-License-Identifier: BSD-3-Clause
//
// GC11 Navi33 PSP GFX_CMD_ID_AUTOLOAD_RLC (0x21).
// This is NOT another LOAD_IP_FW. It can start RLC/IMU execution!
// The only permitted runtime path is behind a separately built *unsafe*
// feature AND an explicit opt-in boot argument after all prerequisite
// firmware loads have been independently accepted in this same boot.
//
// Linux: amdgpu_psp.c -> psp_rlc_autoload_start(), then
// gfx_v11_0_wait_for_rlc_autoload_complete() verifies CP_STAT=0 and
// RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE (bit31).
// No GC/MMHUB register writes, no graphics queues here.
#include <stdint.h>

namespace rn_navi33 { namespace p725_rlc_autoload {

constexpr uint32_t kCommandId = 0x21u;
constexpr uint32_t kPriorFence = 23u;
constexpr uint32_t kExpectedFence = 24u;
constexpr uint32_t kBootCompleteBit = 0x80000000u;

enum class Error : uint8_t {
    Ok = 0,
    Disabled,
    NoExplicitOptIn,
    WrongAsic,
    WrongGcVersion,
    NoPspRing,
    NoTmr,
    NoPmfw,
    SdmaMissing,
    PfpMeMecMissing,
    RlcMissing,
    ImuMissing,
    MesSchedulerMissing,
    MesKiqMissing,
    PriorFenceMismatch,
    TransportUnhealthy,
    NoRuntimeOwner,
};

struct Input {
    bool requested = false;
    bool explicitOptIn = false;
    bool asicVerified = false;
    uint8_t gcMajor = 0;
    uint8_t gcMinor = 0;
    uint8_t gcRevision = 0;
    bool pspRingReady = false;
    bool tmrAccepted = false;
    bool pmfwAccepted = false;
    bool sdmaBothAccepted = false;
    bool pfpMeMecAccepted = false;
    bool rlcGAccepted = false;
    bool imuIAccepted = false;
    bool imuDAccepted = false;
    bool mesSchedulerCodeAccepted = false;
    bool mesSchedulerDataAccepted = false;
    bool mesKiqCodeAccepted = false;
    bool mesKiqDataAccepted = false;
    uint32_t previousFenceExpected = 0;
    uint32_t previousFenceObserved = 0;
    bool transportHealthy = false;
    bool runtimeOwnerRetained = false;
};

struct Report {
    Error error = Error::Disabled;
    bool mayAutoload = false;
};

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.explicitOptIn) { r.error = Error::NoExplicitOptIn; return r; }
    if (!in.asicVerified) { r.error = Error::WrongAsic; return r; }
    if (in.gcMajor != 11 || in.gcMinor != 0 || in.gcRevision != 2) {
        r.error = Error::WrongGcVersion; return r;
    }
    if (!in.pspRingReady) { r.error = Error::NoPspRing; return r; }
    if (!in.tmrAccepted) { r.error = Error::NoTmr; return r; }
    if (!in.pmfwAccepted) { r.error = Error::NoPmfw; return r; }
    if (!in.sdmaBothAccepted) { r.error = Error::SdmaMissing; return r; }
    if (!in.pfpMeMecAccepted) {
        r.error = Error::PfpMeMecMissing; return r;
    }
    if (!in.rlcGAccepted) { r.error = Error::RlcMissing; return r; }
    if (!in.imuIAccepted || !in.imuDAccepted) {
        r.error = Error::ImuMissing; return r;
    }
    if (!in.mesSchedulerCodeAccepted || !in.mesSchedulerDataAccepted) {
        r.error = Error::MesSchedulerMissing; return r;
    }
    if (!in.mesKiqCodeAccepted || !in.mesKiqDataAccepted) {
        r.error = Error::MesKiqMissing; return r;
    }
    if (in.previousFenceExpected != kPriorFence ||
        in.previousFenceObserved != kPriorFence) {
        r.error = Error::PriorFenceMismatch; return r;
    }
    if (!in.transportHealthy) {
        r.error = Error::TransportUnhealthy; return r;
    }
    if (!in.runtimeOwnerRetained) {
        r.error = Error::NoRuntimeOwner; return r;
    }
    r.error = Error::Ok;
    r.mayAutoload = true;
    return r;
}

inline bool hardwareBootComplete(uint32_t cpStat, uint32_t rlcStatus) {
    return cpStat == 0u && rlcStatus != 0xffffffffu &&
           (rlcStatus & kBootCompleteBit) != 0u;
}

}} // namespace rn_navi33::p725_rlc_autoload
#endif
