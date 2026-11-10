#ifndef REDNAVI_NAVI33_BOOTSTRAP_H
#define REDNAVI_NAVI33_BOOTSTRAP_H
// SPDX-License-Identifier: BSD-3-Clause
// Navi 33 live hardware preflight helpers. No GPU I/O occurs in this header.
// Values are derived from checked IP Discovery and the device's own MMHUB
// registers; none are copied from Navi 32's hard-coded 16 GiB address map.
#include <stdint.h>
#include "navi33_runtime.h"

namespace rn_navi33 {

// RX 7600 (Navi33) reports MMHUB IP 3.0.2, whose register layout is NOT
// identical to Navi32's MMHUB 3.0.0. All offsets are DWORDs relative to
// MMHUB segment 0 from the device's validated IP Discovery table.
// Source: Linux asic_reg/mmhub/mmhub_3_0_2_offset.h.
namespace mmhub_v3_0_2 {
constexpr uint32_t kFbLocationBaseDword = 0x086cu;
constexpr uint32_t kFbLocationTopDword  = 0x086du;
constexpr uint32_t kFbOffsetDword       = 0x0857u;
} // namespace mmhub_v3_0_2

enum class MemoryError : uint8_t {
    Ok, NoVram, BadLocation, WindowTooSmall, InvalidBar
};
struct MemoryWindow {
    MemoryError error = MemoryError::NoVram;
    uint64_t fbStart = 0;
    uint64_t fbEnd = 0; // inclusive
    uint64_t locationBytes = 0; // raw MMHUB FB_LOCATION span
    uint64_t fbBytes = 0;       // conservative usable span
    uint64_t vramBytes = 0;     // NBIO/RCC_CONFIG_MEMSIZE
    uint64_t cpuVisibleBytes = 0;
    uint32_t fbOffsetRegister = 0; // keep raw; FB_OFFSET is not a VRAM allocation address
    bool valid() const { return error == MemoryError::Ok; }
};
constexpr uint64_t kFbUnit = 1ull << 24; // FB_LOCATION_* in 16 MiB units
inline MemoryWindow makeMemoryWindow(uint32_t fbBase, uint32_t fbTop,
                                     uint32_t fbOffset, uint32_t vramMib,
                                     uint64_t bar0Bytes) {
    MemoryWindow out{};
    out.fbOffsetRegister = fbOffset;
    if (!vramMib || vramMib > 65536u) return out;
    out.vramBytes = static_cast<uint64_t>(vramMib) << 20;
    if ((fbBase & 0xff000000u) || (fbTop & 0xff000000u) || fbTop < fbBase) {
        out.error = MemoryError::BadLocation;
        return out;
    }
    const uint64_t units = static_cast<uint64_t>(fbTop) - fbBase + 1u;
    out.fbStart = static_cast<uint64_t>(fbBase) * kFbUnit;
    out.locationBytes = units * kFbUnit;

    // Linux GMC11 uses NBIO get_memsize() (RCC_CONFIG_MEMSIZE) as
    // mc_vram_size and uses MMHUB get_fb_location() for the base address.
    // Real Navi33 captures show the two size observations can differ by one
    // 16 MiB unit in either direction:
    //   Linux:  FB_LOCATION span 8176 MiB, memsize/BAR0 8192 MiB
    //   macOS:  FB_LOCATION span 8192 MiB, memsize 8176 MiB, BAR0 256 MiB
    // Treat that one-unit disagreement as a reserved edge, but never expose
    // more usable VRAM than both independent observations agree on.
    constexpr uint64_t kMaxNavi33ReservedTail = 16ull << 20;
    const uint64_t larger = out.locationBytes > out.vramBytes ?
        out.locationBytes : out.vramBytes;
    const uint64_t smaller = out.locationBytes < out.vramBytes ?
        out.locationBytes : out.vramBytes;
    if (larger - smaller > kMaxNavi33ReservedTail) {
        out.error = MemoryError::WindowTooSmall;
        return out;
    }
    out.fbBytes = smaller;
    out.fbEnd = out.fbStart + out.fbBytes - 1u;

    // BAR0 is a CPU aperture and may include the same 16 MiB reserved edge.
    // The usable CPU-visible span is still clipped to conservative fbBytes.
    if (bar0Bytes < 4096u || bar0Bytes > larger) {
        out.error = MemoryError::InvalidBar;
        return out;
    }
    out.cpuVisibleBytes = bar0Bytes < out.fbBytes ? bar0Bytes : out.fbBytes;
    out.error = MemoryError::Ok;
    return out;
}

// Registers in BAR5 are addressed in DWORDs. Do not turn a malformed IP
// discovery segment into an unchecked MMIO access.
inline bool checkedBarDword(uint32_t ipBaseDword, uint32_t registerDword,
                            uint64_t mappedBarBytes, uint32_t *byteOffset) {
    if (!byteOffset) return false;
    const uint64_t off = (static_cast<uint64_t>(ipBaseDword) + registerDword) * 4ull;
    if (off > UINT32_MAX || off > mappedBarBytes ||
        mappedBarBytes - off < 4u) return false;
    *byteOffset = static_cast<uint32_t>(off);
    return true;
}

inline bool hasComputeRegisterBases(const RuntimeReport &r) {
    return r.eligibleForComputeBringup() &&
        r.bases.gc.count >= 4 && r.bases.mp0.count >= 1 &&
        r.bases.mp1.count >= 1 && r.bases.sdma0.count >= 1 &&
        r.bases.mmhub.count >= 2 && r.bases.osssys.count >= 2 &&
        r.bases.nbif.count >= 6;
}
} // namespace rn_navi33
#endif
