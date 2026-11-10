#ifndef REDNAVI_NAVI33_P6_PSP_STAGING_H
#define REDNAVI_NAVI33_P6_PSP_STAGING_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Navi33 P6.2 PSP firmware-staging contract.
// Host-only: validates that a verified PSP13.0.7 component can be copied into
// a retained 1 MiB host DMA buffer that has an explicitly owned GART VA.
// No allocation, memcpy, PTE write, MMIO, ring creation or PSP command.
//
// Address spaces are deliberately separate:
//   dmaIova      = device-visible host-memory backing from IODMACommand
//   fwPriGpuVa   = VMID0/GART address programmed into PSP boot mailbox
// Neither is a CPU pointer, CPU physical address, VRAM offset, or GPU MC VRAM.
#include <stdint.h>
#include "navi33_p6_psp_boot_plan.h"

namespace rn_navi33 { namespace p6_staging {

constexpr bool kHardwareWritesEnabled = false;
constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kOneMiB = 1ull << 20;

enum class Error : uint8_t {
    Ok,
    NoPreflight,
    NoFirmwareProof,
    BadFirmwareLength,
    NoGartOwnership,
    BadGartWindow,
    BadFwPriAddress,
    NoDmaOwnership,
    BadDmaSpan,
};

struct Input {
    bool p6PreflightPassed = false;
    bool firmwareBytesVerified = false;
    bool firmwareMetadataVerified = false;
    uint32_t maxFirmwareComponentBytes = 0;

    bool gartOwnershipVerified = false;
    uint64_t gartStart = 0;
    uint64_t gartEnd = 0;
    uint64_t fwPriGpuVa = 0;
    uint64_t fwPriBytes = 0;

    bool dmaMappingRetained = false;
    bool dmaPagesVerified = false;
    uint64_t dmaBytes = 0;
};

struct Result {
    Error error = Error::NoPreflight;
    uint64_t fwPriGpuVa = 0;
    uint64_t fwPriBytes = 0;
    bool mayCopyFirmware = false;   // remains false until a future execution phase
    bool mayIssuePspCommands = false; // ALWAYS false in P6.2
};

constexpr bool aligned(uint64_t value, uint64_t align) {
    return align && !(align & (align - 1u)) && !(value & (align - 1u));
}
constexpr bool spanInside(uint64_t start, uint64_t bytes,
                          uint64_t begin, uint64_t endInclusive) {
    if (!bytes || begin > endInclusive || start < begin) return false;
    const uint64_t available = endInclusive - start + 1u;
    return start <= endInclusive && bytes <= available;
}

inline Result validate(const Input &in) {
    Result out{};
    if (!in.p6PreflightPassed) return out;
    if (!in.firmwareBytesVerified || !in.firmwareMetadataVerified) {
        out.error = Error::NoFirmwareProof; return out;
    }
    if (!in.maxFirmwareComponentBytes ||
        in.maxFirmwareComponentBytes > p6_boot::kStageBytes) {
        out.error = Error::BadFirmwareLength; return out;
    }
    if (!in.gartOwnershipVerified) {
        out.error = Error::NoGartOwnership; return out;
    }
    if (!in.gartStart || in.gartStart > in.gartEnd) {
        out.error = Error::BadGartWindow; return out;
    }
    if (in.fwPriBytes != p6_boot::kStageBytes ||
        !aligned(in.fwPriGpuVa, kOneMiB) ||
        !spanInside(in.fwPriGpuVa, in.fwPriBytes,
                    in.gartStart, in.gartEnd)) {
        out.error = Error::BadFwPriAddress; return out;
    }
    if (!in.dmaMappingRetained || !in.dmaPagesVerified) {
        out.error = Error::NoDmaOwnership; return out;
    }
    if (in.dmaBytes != in.fwPriBytes ||
        in.dmaBytes != p6_boot::kStageBytes) {
        out.error = Error::BadDmaSpan; return out;
    }

    out.error = Error::Ok;
    out.fwPriGpuVa = in.fwPriGpuVa;
    out.fwPriBytes = in.fwPriBytes;
    return out;
}

}} // namespace rn_navi33::p6_staging
#endif
