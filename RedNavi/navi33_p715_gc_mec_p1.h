#ifndef REDNAVI_NAVI33_P715_GC_MEC_P1_H
#define REDNAVI_NAVI33_P715_GC_MEC_P1_H
// SPDX-License-Identifier: BSD-3-Clause
// P7.15: Navi33 GC11.0.2 RS64 MEC P1_STACK data microcode.
// Linux uses the identical pinned GFX v2.0 MEC data region for
// each MEC P0..P7 stack with a distinct PSP firmware type.
// This stage permits only LOAD_IP_FW(type 95) after MEC P0_STACK
// type 94 was accepted at prior fence 13 and the retained
// explicitly expanded DMA stage remains mapped and verified.
// No MEC P2..P7, RLC, MES or graphics queues start.
// Pure host-testable admission policy (no hardware I/O).
#include <stdint.h>

namespace rn_navi33 { namespace p715_gc_mec_p1 {

constexpr uint32_t kCommandId = 0x06u;
constexpr uint32_t kFirmwareType = 95u; // GFX_FW_TYPE_RS64_MEC_P1_STACK
constexpr uint32_t kArgumentCount = 4u;
constexpr uint32_t kPriorFence = 13u;
constexpr uint32_t kExpectedFence = 14u;
constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kStageMaxBytes = 1ull << 20;

enum class Error : uint8_t {
    Ok = 0,
    Disabled,
    NoExplicitOptIn,
    IdentityUnverified,
    GcIpMismatch,
    NoPspRing,
    TmrNotAccepted,
    SdmaNotAccepted,
    PfpNotAccepted,
    P0NotAccepted,
    P1NotAccepted,
    MeNotAccepted,
    MeP0NotAccepted,
    MeP1NotAccepted,
    MecNotAccepted,
    MecP0NotAccepted,
    NoStageExpansion,
    PreviousFenceMismatch,
    TransportUnhealthy,
    NoStagingOwner,
    NoStageMapping,
    StagePteUnverified,
    FirmwareMissing,
    FirmwareMetadataUnverified,
    FirmwareSourceUnverified,
    FirmwareTargetMismatch,
    EmptyFirmware,
    StageTooSmall,
    StageTooLarge,
    InvalidStageAddress,
};

struct Input {
    bool requested = false;
    bool explicitOptIn = false;
    bool identityVerified = false;
    uint8_t observedGcMajor = 0;
    uint8_t observedGcMinor = 0;
    uint8_t observedGcRevision = 0;
    bool pspRingReady = false;
    bool tmrAccepted = false;
    bool sdmaBothAccepted = false;
    bool pfpAccepted = false;
    bool p0Accepted = false;
    bool p1Accepted = false;
    bool meAccepted = false;
    bool meP0Accepted = false;
    bool meP1Accepted = false;
    bool mecAccepted = false;
    bool mecP0Accepted = false;
    bool stageExpansionVerified = false;
    uint32_t previousFenceExpected = 0;
    uint32_t previousFenceObserved = 0;
    bool transportHealthy = false;
    bool stageOwnerRetained = false;
    bool stageMapped = false;
    bool stagePteVerified = false;
    bool firmwarePresent = false;
    bool firmwareMetadataVerified = false;
    bool firmwareSourceVerified = false;
    uint32_t firmwareIpMajor = 0;
    uint32_t firmwareIpMinor = 0;
    uint32_t firmwareIpRevision = 0;
    uint64_t firmwareBytes = 0;
    uint64_t stageBytes = 0;
    uint64_t stageGpuVa = 0;
    uint64_t gartStart = 0;
    uint64_t gartEnd = 0;
};

struct Report {
    Error error = Error::Disabled;
    bool mayLoad = false;
    uint32_t words[kArgumentCount]{};
};

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.explicitOptIn) {
        r.error = Error::NoExplicitOptIn; return r;
    }
    if (!in.identityVerified) {
        r.error = Error::IdentityUnverified; return r;
    }
    if (in.observedGcMajor != 11 || in.observedGcMinor != 0 ||
        in.observedGcRevision != 2) {
        r.error = Error::GcIpMismatch; return r;
    }
    if (!in.pspRingReady) {
        r.error = Error::NoPspRing; return r;
    }
    if (!in.tmrAccepted) {
        r.error = Error::TmrNotAccepted; return r;
    }
    if (!in.sdmaBothAccepted) {
        r.error = Error::SdmaNotAccepted; return r;
    }
    if (!in.pfpAccepted) {
        r.error = Error::PfpNotAccepted; return r;
    }
    if (!in.p0Accepted) {
        r.error = Error::P0NotAccepted; return r;
    }
    if (!in.p1Accepted) {
        r.error = Error::P1NotAccepted; return r;
    }
    if (!in.meAccepted) {
        r.error = Error::MeNotAccepted; return r;
    }
    if (!in.meP0Accepted) {
        r.error = Error::MeP0NotAccepted; return r;
    }
    if (!in.meP1Accepted) {
        r.error = Error::MeP1NotAccepted; return r;
    }
    if (!in.mecAccepted) {
        r.error = Error::MecNotAccepted; return r;
    }
    if (!in.mecP0Accepted) {
        r.error = Error::MecP0NotAccepted; return r;
    }
    if (!in.stageExpansionVerified) {
        r.error = Error::NoStageExpansion; return r;
    }
    if (in.previousFenceExpected != kPriorFence ||
        in.previousFenceObserved != kPriorFence) {
        r.error = Error::PreviousFenceMismatch; return r;
    }
    if (!in.transportHealthy) {
        r.error = Error::TransportUnhealthy; return r;
    }
    if (!in.stageOwnerRetained) {
        r.error = Error::NoStagingOwner; return r;
    }
    if (!in.stageMapped) {
        r.error = Error::NoStageMapping; return r;
    }
    if (!in.stagePteVerified) {
        r.error = Error::StagePteUnverified; return r;
    }
    if (!in.firmwarePresent) {
        r.error = Error::FirmwareMissing; return r;
    }
    if (!in.firmwareMetadataVerified) {
        r.error = Error::FirmwareMetadataUnverified; return r;
    }
    if (!in.firmwareSourceVerified) {
        r.error = Error::FirmwareSourceUnverified; return r;
    }
    if (in.firmwareIpMajor != in.observedGcMajor ||
        in.firmwareIpMinor != in.observedGcMinor ||
        in.firmwareIpRevision != in.observedGcRevision) {
        r.error = Error::FirmwareTargetMismatch; return r;
    }
    if (!in.firmwareBytes || in.firmwareBytes > UINT32_MAX ||
        (in.firmwareBytes & 3u)) {
        r.error = Error::EmptyFirmware; return r;
    }
    if (in.firmwareBytes > in.stageBytes) {
        r.error = Error::StageTooSmall; return r;
    }
    if (in.stageBytes > kStageMaxBytes) {
        r.error = Error::StageTooLarge; return r;
    }
    if (!in.stageGpuVa || (in.stageGpuVa & (kPage - 1u)) ||
        in.gartEnd < in.gartStart ||
        in.stageGpuVa < in.gartStart ||
        in.stageGpuVa > in.gartEnd ||
        !in.stageBytes ||
        in.stageBytes - 1u > in.gartEnd - in.stageGpuVa) {
        r.error = Error::InvalidStageAddress; return r;
    }
    r.words[0] = static_cast<uint32_t>(in.stageGpuVa);
    r.words[1] = static_cast<uint32_t>(in.stageGpuVa >> 32);
    r.words[2] = static_cast<uint32_t>(in.firmwareBytes);
    r.words[3] = kFirmwareType;
    r.error = Error::Ok;
    r.mayLoad = true;
    return r;
}

}} // namespace rn_navi33::p715_gc_mec_p1
#endif
