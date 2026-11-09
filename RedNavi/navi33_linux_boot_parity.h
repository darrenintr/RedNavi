#ifndef REDNAVI_NAVI33_LINUX_BOOT_PARITY_H
#define REDNAVI_NAVI33_LINUX_BOOT_PARITY_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Navi33 (gfx1102): host-testable Linux amdgpu GPU-init dependency contract.
// This is a routing/diagnostic component, NOT a firmware or queue loader.
// It contains no register writes, no GPU accesses, and cannot enable hardware.
//
// Linux baseline: torvalds/linux 7475ea8b58616b33c04afb976cf0fbaad9dd63fb.
// Branch: PSP firmware loading, GC 11.0.2, enable_mes_kiq=1,
// amdgpu_async_gfx_ring=1, MES legacy mapping with firmware >= 0x47.
#include <stddef.h>
#include <stdint.h>

namespace rn_navi33 { namespace linux_boot {

constexpr bool kHardwareWritesEnabled = false;
constexpr uint32_t kMesLegacyVersion = 0x47u;
constexpr uint32_t kMesResources1Version = 0x52u;
constexpr uint32_t kMesVersionMask = 0xfffu;
constexpr unsigned kMaxSteps = 32u;

enum class Step : uint8_t {
    Discovery, CommonHw, GmcHw, IhHw, PspFw, SmuHw,
    GfxRlcAutoloadWait, GfxHubEnable, GfxGolden, GfxConstants,
    NbioDoorbells, GfxRlcResume, CpDoorbellRanges,
    MesKiqStart, MesKiqQueue, MesSchedulerMap,
    MesSetResources, MesSetResources1, MesQueryStatus,
    MesOwnershipTransfer, KcqMqd, KcqKiqSetResources,
    KcqMesMap, KgqMqd, KgqMesMap,
    GfxStart, GfxFence, ComputeFence
};

enum class Error : uint8_t {
    Ok, UnsupportedAsic, WrongBootPath, UnknownMesVersion,
    UnsupportedMesVersion, LegacyQueueConflict,
    WrongOrder, MissingEvidence, AlreadyComplete, InvalidPlan
};

struct Settings {
    uint16_t vendor = 0, device = 0;
    uint16_t gcMajor = 0, gcMinor = 0, gcRevision = 0;
    bool pspFirmware = false; // AMDGPU_FW_LOAD_PSP
    bool mesKiq = false;       // adev->enable_mes_kiq
    bool asyncGfx = false;     // amdgpu_async_gfx_ring
    bool mesVersionObserved = false;
    uint32_t mesSchedulerVersion = 0;
    bool directG34G42Active = false; // existing Cobalt64-style route is exclusive
};

struct Plan {
    Error error = Error::InvalidPlan;
    Step stages[kMaxSteps]{};
    unsigned count = 0;
    bool valid() const { return error == Error::Ok; }
};

inline bool navi33(const Settings &s) {
    return s.vendor == 0x1002u && s.device == 0x7480u &&
           s.gcMajor == 11u && s.gcMinor == 0u && s.gcRevision == 2u;
}
inline Plan makePlan(const Settings &s) {
    Plan p{};
    if (!navi33(s)) { p.error = Error::UnsupportedAsic; return p; }
    if (!s.pspFirmware || !s.mesKiq || !s.asyncGfx) {
        p.error = Error::WrongBootPath; return p;
    }
    if (s.directG34G42Active) { p.error = Error::LegacyQueueConflict; return p; }
    if (!s.mesVersionObserved) { p.error = Error::UnknownMesVersion; return p; }
    const uint32_t ver = s.mesSchedulerVersion & kMesVersionMask;
    if (ver < kMesLegacyVersion) { p.error = Error::UnsupportedMesVersion; return p; }

    const Step prefix[] = {
        Step::Discovery, Step::CommonHw, Step::GmcHw, Step::IhHw,
        Step::PspFw, Step::SmuHw,
        Step::GfxRlcAutoloadWait, Step::GfxHubEnable, Step::GfxGolden,
        Step::GfxConstants, Step::NbioDoorbells, Step::GfxRlcResume,
        Step::CpDoorbellRanges, Step::MesKiqStart, Step::MesKiqQueue,
        Step::MesSchedulerMap, Step::MesSetResources
    };
    const Step suffix[] = {
        Step::MesQueryStatus, Step::MesOwnershipTransfer,
        Step::KcqMqd, Step::KcqKiqSetResources, Step::KcqMesMap,
        Step::KgqMqd, Step::KgqMesMap, Step::GfxStart,
        Step::GfxFence, Step::ComputeFence
    };
    for (const Step stage : prefix) p.stages[p.count++] = stage;
    if (ver >= kMesResources1Version)
        p.stages[p.count++] = Step::MesSetResources1;
    for (const Step stage : suffix) p.stages[p.count++] = stage;
    p.error = Error::Ok;
    return p;
}

// This guard checks *reported* stage order. A caller-supplied "evidence"
// boolean is NEVER sufficient to establish a real GPU fence or ownership.
// Do not use it to authorize MMIO, DMA, firmware or GPU submissions.
struct Progress {
    Error error = Error::InvalidPlan;
    unsigned nextIndex = 0;
    Step nextStep = Step::Discovery;
    bool done = false;
};
inline Progress checkProgress(const Plan &plan, const Step *events,
                              const bool *verified, unsigned count) {
    Progress out{};
    if (!plan.valid() || plan.count > kMaxSteps) return out;
    if (count > plan.count || (count && (!events || !verified))) {
        out.error = Error::WrongOrder; return out;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (events[i] != plan.stages[i]) {
            out.error = Error::WrongOrder; out.nextIndex = i;
            out.nextStep = plan.stages[i]; return out;
        }
        if (!verified[i]) {
            out.error = Error::MissingEvidence; out.nextIndex = i;
            out.nextStep = plan.stages[i]; return out;
        }
    }
    out.nextIndex = count;
    out.done = count == plan.count;
    out.error = out.done ? Error::AlreadyComplete : Error::Ok;
    if (!out.done) out.nextStep = plan.stages[count];
    return out;
}

// Read-only status from *actual* RedNavi Navi33 probe. This preflight never
// claims that COMMON/GMC/PSP completed their Linux hw_init callbacks;
// register visibility and sOS sign-of-life are only observations.
enum class Blocker : uint8_t {
    Discovery, MissingBases, MissingMemory, PspMailboxUnreadable,
    PspIfwiNotReady, NoVerifiedGpuOwner
};
struct Snapshot {
    bool identityVerified = false;
    bool basesVerified = false;
    bool memoryWindowVerified = false;
    bool pspMailboxRead = false;
    bool ifwiReady = false;
    bool sosAlive = false;
};
struct Preflight {
    Blocker blocker = Blocker::Discovery;
    uint32_t observationMask = 0u;
    bool hardwareInitAllowed = false;
};
inline Preflight inspect(const Snapshot &s) {
    Preflight p{};
    if (s.identityVerified) p.observationMask |= 1u;
    else return p;
    if (s.basesVerified) p.observationMask |= 2u;
    else { p.blocker = Blocker::MissingBases; return p; }
    if (s.memoryWindowVerified) p.observationMask |= 4u;
    else { p.blocker = Blocker::MissingMemory; return p; }
    if (s.pspMailboxRead) p.observationMask |= 8u;
    else { p.blocker = Blocker::PspMailboxUnreadable; return p; }
    if (s.ifwiReady) p.observationMask |= 16u;
    else { p.blocker = Blocker::PspIfwiNotReady; return p; }
    if (s.sosAlive) p.observationMask |= 32u; // observation, never PSP ring proof
    p.blocker = Blocker::NoVerifiedGpuOwner;
    return p;
}

} } // namespace rn_navi33::linux_boot
#endif
