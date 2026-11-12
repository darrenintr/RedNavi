#ifndef REDNAVI_NAVI33_MP1_FWFLAGS_SMN_PROBE_H
#define REDNAVI_NAVI33_MP1_FWFLAGS_SMN_PROBE_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Opt-in Navi33 SMU 13.0.7 firmware-flags observational probe.
// MP1_FIRMWARE_FLAGS is SMN address 0x03B10028, NOT MP1 BAR5 base+offset.
// Linux NBIO 4.3.1 maps PF0 RSMU INDEX/DATA to NBIF segment 1
// DWORD offsets 0 and 1. A read requires one index MMIO write;
// the index write must be posted/flushed by a readback before data.
// This policy checks boundaries and prerequisites only;
// no SMN, GPU, SMU command or PCI writes occur in this header.
#include <stdint.h>

namespace rn_navi33 { namespace mp1_fwflags_smn_probe {

constexpr uint32_t kSmnMp1FirmwareFlags = 0x03b10028u;
constexpr uint32_t kNbifIndexDword = 0u;
constexpr uint32_t kNbifDataDword = 1u;
constexpr uint32_t kNbifIndexHiDword = 2u; // 32-bit SMN path leaves HI alone.
constexpr uint32_t kInterruptsEnabledMask = 0x00000001u;
constexpr uint32_t kInvalid = 0xffffffffu;
constexpr uint32_t kPriorFence = 23u;

// Short alternative boot-args for the strict two-key P7.26 opt-in.
// Avoid appending two 30+ character strings to an almost full XNU
// CommandLine buffer. Keep legacy spellings for existing experiments.
constexpr const char kShortFwFlagsBootArg[] = "rn726fw";
constexpr const char kShortPf0BootArg[] = "rn726pf0";

// Require an explicit value of 1 and fail closed when a legacy and
// short spelling conflict. No substring/prefix matching is allowed.
inline bool optInFromEither(bool legacyPresent, uint32_t legacyValue,
                            bool shortPresent, uint32_t shortValue) {
    if (legacyPresent && legacyValue != 1u) return false;
    if (shortPresent && shortValue != 1u) return false;
    return legacyPresent || shortPresent;
}


enum class Error : uint8_t {
    Ok = 0,
    NotRequested,
    MissingOptIn,
    WrongStep,
    WrongAsic,
    WrongMp1Revision,
    UnsupportedNbioVersion,
    PmfwNotAccepted,
    PriorFenceMismatch,
    TransportUnhealthy,
    MissingNbif,
    InvalidNbifWindow,
    MissingPf0OptIn, // append: preserve existing IORegistry numeric errors
};
struct Input {
    bool requested = false;
    bool explicitOptIn = false;
    // Second explicit opt-in for the verified NBIO4.3 PF0 path.
    // Prevents an old 0.0.42 flag from silently enabling writes on update.
    bool pf0ExplicitOptIn = false;
    uint32_t step = 0;
    bool exactNavi33 = false;
    uint32_t mp1Revision = 0;
    // Real NBIF_HWID 108 IP Discovery version, not assumed from PCI ID.
    uint32_t nbioMajor = 0;
    uint32_t nbioMinor = 0;
    uint32_t nbioRevision = 0;
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

// Linux amdgpu_discovery.c selects nbio_v4_3_funcs for v4.3.0/1.
// nbio_v4_3.c uses regBIF_BX_PF0_RSMU_INDEX/DATA. The generated
// nbio_4_3_0_offset.h assigns those register DWORDs 0/1 to NBIF
// segment base index 1. RX7600 0.0.42 observed NBIO 4.3.1.
inline bool supportedPf0RsmuVersion(uint32_t major, uint32_t minor,
                                   uint32_t revision) {
    return major == 4u && minor == 3u && revision <= 1u;
}

// Legacy PF1 diagnostic only: Linux 7.11.0..3 has an index/data
// pair at similar offsets, but it is NOT enabled for Navi33 0.0.43.
inline bool supportedPf1RsmuVersion(uint32_t major, uint32_t minor,
                                   uint32_t revision) {
    return major == 7u && minor == 11u && revision <= 3u;
}

struct Transcript {
    uint32_t originalIndex = kInvalid;
    uint32_t selectedReadback = kInvalid;
    uint32_t firmwareFlags = kInvalid;
    uint32_t restoredIndex = kInvalid;
    bool indexWritten = false;
    bool dataRead = false;
    bool restoreWritten = false;
};

// Linux amdgpu_device_indirect_rreg() always readl(index) after the
// index writel and before readl(data). Reject snapshots missing this
// verified selection or a verified restore of the original index.
inline bool validTranscript(const Transcript &t) {
    return t.originalIndex != kInvalid &&
        t.indexWritten &&
        t.selectedReadback == kSmnMp1FirmwareFlags &&
        t.dataRead && t.firmwareFlags != kInvalid &&
        t.restoreWritten &&
        t.restoredIndex == t.originalIndex;
}

// The entire INDEX/DATA/restore transaction must share the device's
// indirect-register lock with rn_rreg32/rn_wreg32. Callers provide the
// same lock; this header stays usable by pure host tests. No sleeping,
// waiting, or nested indirect-register access inside these callbacks.
// A driver-local lock cannot serialize firmware or another PCI function.
template <typename Acquire, typename Release, typename Read, typename Write>
inline Transcript sampleSerialized(const Report &gate, Acquire acquire,
                                   Release release, Read read, Write write) {
    Transcript t{};
    if (!gate.mayProbe || gate.error != Error::Ok ||
        gate.indexByteOffset == kInvalid ||
        gate.dataByteOffset == kInvalid ||
        gate.indexByteOffset == gate.dataByteOffset) return t;

    acquire();
    t.originalIndex = read(gate.indexByteOffset);
    if (t.originalIndex != kInvalid) {
        write(gate.indexByteOffset, kSmnMp1FirmwareFlags);
        t.indexWritten = true;
        // Flush posted MMIO index write before reading indirect data.
        t.selectedReadback = read(gate.indexByteOffset);
        if (t.selectedReadback == kSmnMp1FirmwareFlags) {
            t.firmwareFlags = read(gate.dataByteOffset);
            t.dataRead = true;
        }
        // Restore even if selection failed; verify under the same lock.
        write(gate.indexByteOffset, t.originalIndex);
        t.restoreWritten = true;
        t.restoredIndex = read(gate.indexByteOffset);
    }
    release();
    return t;
}

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.explicitOptIn) { r.error = Error::MissingOptIn; return r; }
    if (in.step != 27u) { r.error = Error::WrongStep; return r; }
    if (!in.exactNavi33) { r.error = Error::WrongAsic; return r; }
    if (in.mp1Revision != 7u) { r.error = Error::WrongMp1Revision; return r; }
    if (!supportedPf0RsmuVersion(in.nbioMajor, in.nbioMinor,
                                 in.nbioRevision)) {
        r.error = Error::UnsupportedNbioVersion; return r;
    }
    if (!in.pmfwAccepted) { r.error = Error::PmfwNotAccepted; return r; }
    if (in.priorFenceExpected != kPriorFence ||
        in.priorFenceObserved != kPriorFence) {
        r.error = Error::PriorFenceMismatch; return r;
    }
    if (!in.transportHealthy) { r.error = Error::TransportUnhealthy; return r; }
    if (!in.pf0ExplicitOptIn) { r.error = Error::MissingPf0OptIn; return r; }
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
