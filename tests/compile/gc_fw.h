// SPDX-License-Identifier: BSD-3-Clause
// Declaration-only fixture for make check-kernel-firmware. There are no
// firmware bytes or definitions here, and this target never links a kext.
#ifndef RN_COMPILE_ONLY_FIRMWARE
#error "This header is only for the firmware-enabled syntax check"
#endif

struct RNGcFw {
    const char *name;
    unsigned int type, offset, size;
};
extern const unsigned int kRNGcFwCount;
extern const RNGcFw kRNGcFw[];
extern const unsigned char kRNGcFwBlob[];
extern const unsigned int kRNGcPfpStartLo, kRNGcPfpStartHi;
extern const unsigned int kRNGcMeStartLo, kRNGcMeStartHi;
extern const unsigned int kRNGcMecStartLo, kRNGcMecStartHi;
extern const unsigned int kRNMes0StartLo, kRNMes0StartHi;
extern const unsigned int kRNMes1StartLo, kRNMes1StartHi;
