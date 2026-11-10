#ifndef REDNAVI_NAVI33_PROBE_ADMISSION_H
#define REDNAVI_NAVI33_PROBE_ADMISSION_H
// SPDX-License-Identifier: BSD-3-Clause
// No IOKit or hardware access: explicit admission decision before any
// BAR mapping, PCI command writes, MM_INDEX touches, or GPU reset.
// Protects an RX7600 that is the only working macOS display.
#include <stdint.h>

namespace rn_navi33 {

enum class ProbeAdmission : uint8_t {
    OtherAsic,
    DisabledInBuild,
    MissingExplicitConsent,
    ExperimentalOnly
};

inline ProbeAdmission admitEarlyHardwareProbe(
    uint16_t vendor, uint16_t device, bool experimentalBuild,
    uint32_t explicitBootArg) {
    if (vendor != 0x1002u || device != 0x7480u)
        return ProbeAdmission::OtherAsic; // Legacy ASIC path unchanged.
    if (!experimentalBuild)
        return ProbeAdmission::DisabledInBuild;
    if (explicitBootArg != 1u)
        return ProbeAdmission::MissingExplicitConsent;
    return ProbeAdmission::ExperimentalOnly;
}

} // namespace rn_navi33
#endif
