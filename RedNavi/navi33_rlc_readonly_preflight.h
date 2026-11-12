#ifndef REDNAVI_NAVI33_RLC_READONLY_PREFLIGHT_H
#define REDNAVI_NAVI33_RLC_READONLY_PREFLIGHT_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Read-only GC11/SMU evidence before any PSP AUTOLOAD_RLC command.
// In particular, an old SMU mailbox result is NOT a verified live SMU
// round-trip, and PSP accepting RLC/MES microcode is NOT engine execution.
// This policy does not access MMIO or send any command.
#include <stdint.h>

namespace rn_navi33 { namespace rlc_readonly_preflight {

constexpr uint32_t kInvalid = 0xffffffffu;
// Linux gfx_v11_0_wait_for_rlc_autoload_complete(), GC11.0.2.
// Bases and register numbers are DWORD indices; converted to BAR5 bytes.
constexpr uint32_t kCpStatDword = 0x0f40u;          // GC segment 0
constexpr uint32_t kRlcBootloadDword = 0x4e82u;     // GC segment 1
constexpr uint32_t kImuScratchDword = 0x4068u;      // GC segment 1
constexpr uint32_t kSmuResponseDword = 0x029au;     // MP1 segment 0

enum class Error : uint8_t {
    Ok = 0,
    NotRequested,
    BadRegisterAddress,
    UnreadableGcSnapshot,
    PmfwNotAccepted,
    SmuMailboxUnreadable,
    SmuHandshakeUnverified,
    FirmwareLoadOrderMismatch,
};

struct Input {
    bool requested = false;
    bool basesDiscovered = false;
    uint32_t gc0Base = 0;
    uint32_t gc1Base = 0;
    uint32_t mp1Base = 0;
    uint64_t bar5Bytes = 0;

    bool gcSamplesRead = false;
    uint32_t cpStat = kInvalid;
    uint32_t rlcBootloadStatus = kInvalid;
    uint32_t imuScratch0 = kInvalid;

    bool pmfwAccepted = false;
    bool smuMailboxRead = false;
    uint32_t smuResponse = kInvalid;
    // Only a completed, owned SMU mailbox command/response can set this.
    // Read-only sampling (including a stale PPSMC_Result_OK) must NOT.
    bool smuHandshakeVerified = false;

    // PSP submission fences, not an inferred ordering from source names.
    uint32_t mesSchedCodeFence = 0;
    uint32_t mesSchedDataFence = 0;
    uint32_t mesKiqCodeFence = 0;
    uint32_t mesKiqDataFence = 0;
    uint32_t imuIFence = 0;
    uint32_t imuDFence = 0;
    uint32_t rlcGFence = 0;
};

struct Report {
    Error error = Error::NotRequested;
    bool gcAddressesValid = false;
    bool gcSnapshotReadable = false;
    bool pmfwAccepted = false;
    bool smuMailboxReadable = false;
    bool smuHandshakeVerified = false;
    bool linuxFirmwareOrder = false;
    bool mayAutoload = false;
    uint32_t cpStatByteOffset = kInvalid;
    uint32_t rlcBootloadByteOffset = kInvalid;
    uint32_t imuScratchByteOffset = kInvalid;
    uint32_t smuResponseByteOffset = kInvalid;
};

// Convert a discovered IP-base + register DWORD to a *bounded* BAR5 byte
// offset, rejecting zero bases, integer overflow and out-of-aperture reads.
inline bool checkedRegister(uint32_t baseDwords, uint32_t regDwords,
                            uint64_t barBytes, uint32_t *out) {
    if (!out || !baseDwords || barBytes < sizeof(uint32_t)) return false;
    const uint64_t byteOffset =
        (static_cast<uint64_t>(baseDwords) + regDwords) * 4ull;
    if (byteOffset > UINT32_MAX ||
        byteOffset > barBytes - sizeof(uint32_t)) return false;
    *out = static_cast<uint32_t>(byteOffset);
    return true;
}

// Linux amdgpu_ucode.h order: MES scheduler / KIQ, IMU I/D, then RLC_G.
// PSP triggers AUTOLOAD_RLC immediately after loading RLC_G. Do not
// accept the legacy RedNavi ordering (RLC_G before IMU and MES).
inline bool matchesLinuxOrder(const Input &in) {
    const uint32_t fences[] = {
        in.mesSchedCodeFence, in.mesSchedDataFence,
        in.mesKiqCodeFence, in.mesKiqDataFence,
        in.imuIFence, in.imuDFence, in.rlcGFence
    };
    if (!fences[0]) return false;
    for (unsigned i = 1; i < sizeof(fences) / sizeof(fences[0]); ++i) {
        if (!fences[i] || fences[i] <= fences[i - 1]) return false;
    }
    return true;
}

inline Report inspect(const Input &in) {
    Report r{};
    if (in.basesDiscovered) {
        r.gcAddressesValid =
            checkedRegister(in.gc0Base, kCpStatDword, in.bar5Bytes,
                            &r.cpStatByteOffset) &&
            checkedRegister(in.gc1Base, kRlcBootloadDword, in.bar5Bytes,
                            &r.rlcBootloadByteOffset) &&
            checkedRegister(in.gc1Base, kImuScratchDword, in.bar5Bytes,
                            &r.imuScratchByteOffset) &&
            checkedRegister(in.mp1Base, kSmuResponseDword, in.bar5Bytes,
                            &r.smuResponseByteOffset);
    }
    r.gcSnapshotReadable = r.gcAddressesValid && in.gcSamplesRead &&
        in.cpStat != kInvalid && in.rlcBootloadStatus != kInvalid &&
        in.imuScratch0 != kInvalid;
    r.pmfwAccepted = in.pmfwAccepted;
    r.smuMailboxReadable = r.gcAddressesValid && in.smuMailboxRead &&
        in.smuResponse != kInvalid;
    r.smuHandshakeVerified = r.pmfwAccepted && r.smuMailboxReadable &&
        in.smuHandshakeVerified;
    r.linuxFirmwareOrder = matchesLinuxOrder(in);

    if (!in.requested) return r;
    if (!r.gcAddressesValid) {
        r.error = Error::BadRegisterAddress; return r;
    }
    if (!r.gcSnapshotReadable) {
        r.error = Error::UnreadableGcSnapshot; return r;
    }
    if (!r.pmfwAccepted) {
        r.error = Error::PmfwNotAccepted; return r;
    }
    if (!r.smuMailboxReadable) {
        r.error = Error::SmuMailboxUnreadable; return r;
    }
    if (!r.smuHandshakeVerified) {
        r.error = Error::SmuHandshakeUnverified; return r;
    }
    if (!r.linuxFirmwareOrder) {
        r.error = Error::FirmwareLoadOrderMismatch; return r;
    }
    r.error = Error::Ok;
    r.mayAutoload = true;
    return r;
}

}} // namespace rn_navi33::rlc_readonly_preflight
#endif
