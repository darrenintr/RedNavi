// SPDX-License-Identifier: BSD-3-Clause
// Navi33 experimental metadata observer for native macOS.
// Important: no BAR mapping, no GPU MMIO, no MM_INDEX, no PCI config writes,
// no firmware, no DCN take-over, no GPU reset. Do not install on the
// sole-display RX7600 without an out-of-band recovery path.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

class RedNaviNavi33Diag : public IOService {
    OSDeclareDefaultStructors(RedNaviNavi33Diag)

public:
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(RedNaviNavi33Diag, IOService)

bool RedNaviNavi33Diag::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    auto *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;

    // Independent check even if the Info.plist personality is altered.
    const uint16_t vendor = pci->configRead16(kIOPCIConfigVendorID);
    const uint16_t device = pci->configRead16(kIOPCIConfigDeviceID);
    if (vendor != 0x1002u || device != 0x7480u) {
        IOLog("RedNaviDiag: refusing non-Navi33 PCI %04x:%04x\n",
              vendor, device);
        return false;
    }

    // PCI reads only: never change Command/Status or BAR values.
    setProperty("rednavi-navi33-identity", true);
    setProperty("rednavi-diag-version", static_cast<uint64_t>(1), 32);
    setProperty("rednavi-pci-vendor", static_cast<uint64_t>(vendor), 32);
    setProperty("rednavi-pci-device", static_cast<uint64_t>(device), 32);
    setProperty("rednavi-pci-command",
                static_cast<uint64_t>(pci->configRead16(0x04)), 32);

    // Existing OS-assigned PCI resource metadata, never mapDeviceMemory().
    // BAR0/2/5 are 64-bit/memory resources on the observed RX7600; querying
    // only the base register descriptors avoids assumptions about the
    // serialized array order. A missing resource remains missing.
    static const uint8_t kBars[] = {0, 2, 5};
    static const char *const kLengthNames[] = {
        "rednavi-bar0-cpu-aperture-bytes",
        "rednavi-bar2-doorbell-bytes",
        "rednavi-bar5-register-bytes"
    };
    static const char *const kPhysicalNames[] = {
        "rednavi-bar0-cpu-physical-base",
        "rednavi-bar2-cpu-physical-base",
        "rednavi-bar5-cpu-physical-base"
    };
    bool allPresent = true;
    for (unsigned i = 0; i < sizeof(kBars) / sizeof(kBars[0]); ++i) {
        IODeviceMemory *mem = pci->getDeviceMemoryWithRegister(
            kIOPCIConfigBaseAddress0 + 4u * kBars[i]);
        if (!mem) { allPresent = false; continue; }
        setProperty(kLengthNames[i],
                    static_cast<uint64_t>(mem->getLength()), 64);
        setProperty(kPhysicalNames[i],
                    static_cast<uint64_t>(mem->getPhysicalAddress()), 64);
    }

    // This is only a presence/identity report; no kernel GPU I/O readiness,
    // no GPU MC address proof, and no Metal acceleration implied.
    setProperty("rednavi-all-pci-descriptors-present", allPresent);
    setProperty("rednavi-gpu-writes-enabled", false);
    setProperty("rednavi-hardware-readback-performed", false);
    setProperty("rednavi-metal-acceleration-ready", false);
    IOLog("RedNaviDiag: RX7600 PCI metadata observed; no GPU MMIO or config writes\n");
    registerService();
    return true;
}
