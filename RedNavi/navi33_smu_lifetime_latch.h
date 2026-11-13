#ifndef REDNAVI_NAVI33_SMU_LIFETIME_LATCH_H
#define REDNAVI_NAVI33_SMU_LIFETIME_LATCH_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P7.31: a conservative, single-slot, atomic attempt latch shared by all
// Navi33 RedNaviProbe instances in ONE loaded Kext image.
//
// IMPORTANT: this does not survive unloading the Kext, a reboot, another
// driver taking over, or firmware resets. It does not provide MP1 mailbox
// ownership. Keep the real Navi33 command gate hardcoded OFF.
//
// Claim is intentionally never cleared. A second service reprobe cannot
// start another transaction after the first consumed the slot.
// All Navi33 GPUs in this Kext share one slot (conservative overblocking).
// This is an atomic 32-bit host memory operation, not GPU MMIO.
#include <stdint.h>

namespace rn_navi33 { namespace smu_lifetime_latch {

struct alignas(4) AttemptLatch {
    uint32_t claimed = 0u;
};

inline bool claimOnce(AttemptLatch &latch) {
    uint32_t expected = 0u;
    return __atomic_compare_exchange_n(
        &latch.claimed, &expected, 1u, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

inline bool isClaimed(const AttemptLatch &latch) {
    return __atomic_load_n(&latch.claimed, __ATOMIC_ACQUIRE) != 0u;
}

}} // namespace rn_navi33::smu_lifetime_latch
#endif
