#ifndef REDNAVI_NAVI33_GC_QUEUE_BRINGUP_H
#define REDNAVI_NAVI33_GC_QUEUE_BRINGUP_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Clear separation between PSP firmware acknowledgement, running RLC,
// initialized GC/GFXHUB and a PROVEN, actual graphics queue. Pure policy;
// this file never reads/writes MMIO, schedules hardware, or allocates BOs.
// This path currently only publishes its fail-closed diagnostic report.
#include <stdint.h>

namespace rn_navi33 { namespace gc_queue_bringup {

enum class Error : uint8_t {
    Ok = 0,
    RlcNotBooted,
    GfxHubNotInitialized,
    Rs64NotConfigured,
    GcRegistersNotInitialized,
    MesSchedulerNotRunning,
    MesKiqNotRunning,
    NoQueueMemoryOwner,
    NoDoorbellMapping,
    NoInterruptOwner,
    NoScratchGpuFence,
};

struct Input {
    bool rlcBootComplete = false;
    bool gfxHubInitialized = false;
    bool rs64Configured = false;
    bool gcRegistersInitialized = false;
    bool mesSchedulerRunning = false;
    bool mesKiqRunning = false;
    bool queueMemoryOwnerVerified = false;
    bool doorbellMapped = false;
    bool interruptOwnerVerified = false;
    bool scratchGpuFenceCompleted = false;
};

struct Report {
    Error blocker = Error::RlcNotBooted;
    bool mayCreateGraphicsQueue = false;
};

inline Report inspect(const Input &i) {
    Report r{};
    if (!i.rlcBootComplete) return r;
    if (!i.gfxHubInitialized) {
        r.blocker = Error::GfxHubNotInitialized; return r;
    }
    if (!i.rs64Configured) {
        r.blocker = Error::Rs64NotConfigured; return r;
    }
    if (!i.gcRegistersInitialized) {
        r.blocker = Error::GcRegistersNotInitialized; return r;
    }
    if (!i.mesSchedulerRunning) {
        r.blocker = Error::MesSchedulerNotRunning; return r;
    }
    if (!i.mesKiqRunning) {
        r.blocker = Error::MesKiqNotRunning; return r;
    }
    if (!i.queueMemoryOwnerVerified) {
        r.blocker = Error::NoQueueMemoryOwner; return r;
    }
    if (!i.doorbellMapped) {
        r.blocker = Error::NoDoorbellMapping; return r;
    }
    if (!i.interruptOwnerVerified) {
        r.blocker = Error::NoInterruptOwner; return r;
    }
    if (!i.scratchGpuFenceCompleted) {
        r.blocker = Error::NoScratchGpuFence; return r;
    }
    r.blocker = Error::Ok;
    r.mayCreateGraphicsQueue = true;
    return r;
}

}} // namespace rn_navi33::gc_queue_bringup
#endif
