#ifndef REDNAVI_NAVI33_MP1_FWFLAGS_SMN_PROBE_H
#define REDNAVI_NAVI33_MP1_FWFLAGS_SMN_PROBE_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Opt-in Navi33 SMU 13.0.7 firmware-flags observational probe.
// MP1_FIRMWARE_FLAGS is SMN address 0x03B10028, NOT MP1 BAR5 base+offset.
// Reading requires one write to NBIF RSMU_IND_INDEX before reading
// RSMU_IND_DATA. This policy checks boundaries and prerequisites only;
// no SMN, GPU, SMU command or PCI writes occur in this header.
#include <stdint.h>

namespace rn_navi33 { namespace mp1_fwflags_smn_probe {

constexpr uint32_t kSmnMp1FirmwareFlags = 0x03b10028u;
constexpr uint32_t kNbifIndexDword = 0u;
constexpr uint32_t kNbifDataDword = 1u;
constexpr uint32_t kInterruptsEnabledMask = 0x00000001u;
constexpr uint32_t kInvalid = 0xffffffffu;
constexpr uint32_t kPriorFence = 23u;

enum class Error : uint8_t {
    Ok = 0,
    NotRequested,
    MissingOptIn,
    WrongStep,
    WrongAsic,
    WrongMp1Revision,
    PmfwNotAccepted,
    PriorFenceMismatch,
    TransportUnhealthy,
    MissingNbif,
    InvalidNbifWindow,
};
struct Input {
    bool requested = false;
    bool explicitOptIn = false;
    uint32_t step = 0;
    bool exactNavi33 = false;
    uint32_t mp1Revision = 0;
    bool pmfwAccepted = false;
    uint32_t priorFenceExpected = 0;
    uint32_t priorFenceObserved = 0;
    bool transportHealthy = false;
    bool nbifBaseFound = false;
    uint32_t nbifBaseDword = 0;
    uint64_t bar5Bytes = 0;
};
struct Report {
    Error error = Error::NotRequested;
    bool mayProbe = false;
    uint32_t indexByteOffset = kInvalid;
    uint32_t dataByteOffset = kInvalid;
};

inline bool checkedNbifOffsets(uint32_t baseDword, uint64_t bar5Bytes,
                               uint32_t *indexByteOffset,
                               uint32_t *dataByteOffset) {
    if (!baseDword || !indexByteOffset || !dataByteOffset ||
        bar5Bytes < 8u) return false;
    const uint64_t index =
        (static_cast<uint64_t>(baseDword) + kNbifIndexDword) * 4ull;
    const uint64_t data =
        (static_cast<uint64_t>(baseDword) + kNbifDataDword) * 4ull;
    if (index > UINT32_MAX || data > UINT32_MAX ||
        index > bar5Bytes - sizeof(uint32_t) ||
        data > bar5Bytes - sizeof(uint32_t)) return false;
    *indexByteOffset = static_cast<uint32_t>(index);
    *dataByteOffset = static_cast<uint32_t>(data);
    return true;
}

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.explicitOptIn) { r.error = Error::MissingOptIn; return r; }
    if (in.step != 27u) { r.error = Error::WrongStep; return r; }
    if (!in.exactNavi33) { r.error = Error::WrongAsic; return r; }
    if (in.mp1Revision != 7u) { r.error = Error::WrongMp1Revision; return r; }
    if (!in.pmfwAccepted) { r.error = Error::PmfwNotAccepted; return r; }
    if (in.priorFenceExpected != kPriorFence ||
        in.priorFenceObserved != kPriorFence) {
        r.error = Error::PriorFenceMismatch; return r;
    }
    if (!in.transportHealthy) { r.error = Error::TransportUnhealthy; return r; }
    if (!in.nbifBaseFound) { r.error = Error::MissingNbif; return r; }
    if (!checkedNbifOffsets(in.nbifBaseDword, in.bar5Bytes,
                            &r.indexByteOffset, &r.dataByteOffset)) {
        r.error = Error::InvalidNbifWindow; return r;
    }
    r.error = Error::Ok;
    r.mayProbe = true;
    return r;
}

inline bool interruptsEnabled(uint32_t flags, bool snapshotValid) {
    return snapshotValid && (flags & kInterruptsEnabledMask) != 0u;
}

// Even a valid firmware-flags bit 0 is not an independently verified SMU
// command/response. It MUST NOT set smuHandshakeVerified / postInput.smuReady.
}} // namespace rn_navi33::mp1_fwflags_smn_probe
#endif
