// SPDX-License-Identifier: BSD-3-Clause
// Pure host tests: no PCI, MMIO, IOKit, or GPU access.
#include "navi33_probe_admission.h"
#include <cassert>
using rn_navi33::ProbeAdmission;
using rn_navi33::admitEarlyHardwareProbe;

int main() {
    // A normal CI/debug release can never touch RX7600 BARs, even when
    // someone accidentally leaves a rednavi boot argument enabled.
    assert(admitEarlyHardwareProbe(0x1002,0x7480,false,0) ==
           ProbeAdmission::DisabledInBuild);
    assert(admitEarlyHardwareProbe(0x1002,0x7480,false,1) ==
           ProbeAdmission::DisabledInBuild);
    assert(admitEarlyHardwareProbe(0x1002,0x7480,false,0xffffffffu) ==
           ProbeAdmission::DisabledInBuild);
    // A custom experimental build still requires the exact opt-in value.
    assert(admitEarlyHardwareProbe(0x1002,0x7480,true,0) ==
           ProbeAdmission::MissingExplicitConsent);
    assert(admitEarlyHardwareProbe(0x1002,0x7480,true,2) ==
           ProbeAdmission::MissingExplicitConsent);
    assert(admitEarlyHardwareProbe(0x1002,0x7480,true,1) ==
           ProbeAdmission::ExperimentalOnly);
    // No change to the legacy Navi32 probe admission.
    assert(admitEarlyHardwareProbe(0x1002,0x747e,false,0) ==
           ProbeAdmission::OtherAsic);
    assert(admitEarlyHardwareProbe(0x1234,0x7480,false,1) ==
           ProbeAdmission::OtherAsic);
    return 0;
}
