#ifndef REDNAVI_NAVI33_LINUX_BOOT_PARITY_H
#define REDNAVI_NAVI33_LINUX_BOOT_PARITY_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Navi33 (gfx1102): host-testable Linux amdgpu GPU-init dependency contract.
// This is a routing/diagnostic component, NOT a firmware or queue loader.
// It contains no register writes, no GPU accesses, and cannot enable hardware.
//
// Linux baseline: torvalds/linux 7475ea8b58616b33c04afb976cf0fbaad9dd63fb.
// Branch: PSP firmware loading, GC 11.0.2, PSP/SMU 13.0.7, enable_mes_kiq=1,
// amdgpu_async_gfx_ring=1, MES legacy mapping with firmware >= 0x47.
#include <stddef.h>
#include <stdint.h>

namespace rn_navi33 { namespace linux_boot {

constexpr bool kHardwareWritesEnabled = false;
constexpr uint32_t kMesLegacyVersion = 0x47u;
constexpr uint32_t kMesResources1Version = 0x52u;
constexpr uint32_t kMesVersionMask = 0xfffu;
constexpr unsigned kMaxSteps = 48u;

enum class Step : uint8_t {
    Discovery, CommonSwReady, CommonHw, GmcSwReady, GmcMmhubHw,
    WritebackReady, IpSoftwareReady, IbPoolReady, FirmwareBoReady, IhHw,
    PspRingReady, PspTmrAllocated, PspSmuFirmware, PspTmrInstalled,
    PspGraphicsFirmware, PspRlcAutoloadStart, PspFwComplete, SmuHw,
    GfxRlcAutoloadWait, GfxHubEnable, GfxGolden, GfxConstants,
    NbioDoorbells, GfxRlcResume, CpDoorbellRanges, CpEnginesEnable,
    MesKiqStart, MesKiqQueue, MesSchedulerMap,
    MesSetResources, MesSetResources1, MesQueryStatus,
    MesSchedulerReady, KcqMqd, KcqKiqSetResources,
    KcqMesMap, KgqMqd, KgqMesMap,
    GfxStart, GfxScratchTest, ComputeScratchTest, MesHwFinalize,
    None = 0xffu // no next stage; invalid or complete, as indicated by Progress
};

enum class Error : uint8_t {
    Ok, UnsupportedAsic, WrongBootPath, UnknownMesVersion,
    UnsupportedMesVersion, LegacyQueueConflict,
    WrongOrder, MissingEvidence, AlreadyComplete, InvalidPlan,
    UnsupportedIpVersion
};

struct Settings {
    uint16_t vendor = 0, device = 0;
    uint16_t gcMajor = 0, gcMinor = 0, gcRevision = 0;
    uint16_t pspMajor = 0, pspMinor = 0, pspRevision = 0;
    uint16_t smuMajor = 0, smuMinor = 0, smuRevision = 0;
    bool pspFirmware = false; // AMDGPU_FW_LOAD_PSP
    bool mesKiq = false;       // adev->enable_mes_kiq
    bool asyncGfx = false;     // amdgpu_async_gfx_ring
    bool mesVersionObserved = false;
    uint32_t mesSchedulerVersion = 0;
    bool directG34G42Active = false; // existing Cobalt64-style route is exclusive
};

struct Plan {
    Error error = Error::InvalidPlan;
    Settings settings{};
    Step stages[kMaxSteps]{};
    unsigned count = 0;
    bool valid() const;
};

inline bool navi33(const Settings &s) {
    return s.vendor == 0x1002u && s.device == 0x7480u &&
           s.gcMajor == 11u && s.gcMinor == 0u && s.gcRevision == 2u;
}
namespace detail {
inline Error settingsError(const Settings &s) {
    if (!navi33(s)) return Error::UnsupportedAsic;
    // The PMFW-before-TMR-install rule is PSP13.0.7-specific. A GC match
    // alone must not select that rule for another discovered MP0/MP1 IP.
    if (s.pspMajor != 13u || s.pspMinor != 0u || s.pspRevision != 7u ||
        s.smuMajor != 13u || s.smuMinor != 0u || s.smuRevision != 7u)
        return Error::UnsupportedIpVersion;
    if (!s.pspFirmware || !s.mesKiq || !s.asyncGfx) {
        return Error::WrongBootPath;
    }
    if (s.directG34G42Active) return Error::LegacyQueueConflict;
    if (!s.mesVersionObserved) return Error::UnknownMesVersion;
    const uint32_t ver = s.mesSchedulerVersion & kMesVersionMask;
    if (ver < kMesLegacyVersion) return Error::UnsupportedMesVersion;
    return Error::Ok;
}

// This is the dependency projection for the selected boot branch, not a
// list of every callback (display, SDMA and multimedia are not expanded).
// Sources: amdgpu_device.c::amdgpu_device_ip_init,
// amdgpu_psp.c::psp_hw_start/psp_load_non_psp_fw and gfx_v11_0_cp_resume.
inline constexpr Step kPrefix[] = {
    Step::Discovery, Step::CommonSwReady, Step::CommonHw,
    Step::GmcSwReady, Step::GmcMmhubHw, Step::WritebackReady,
    Step::IpSoftwareReady, Step::IbPoolReady, Step::FirmwareBoReady,
    Step::IhHw, Step::PspRingReady, Step::PspTmrAllocated,
    Step::PspSmuFirmware, Step::PspTmrInstalled,
    Step::PspGraphicsFirmware, Step::PspRlcAutoloadStart,
    Step::PspFwComplete, Step::SmuHw,
    Step::GfxRlcAutoloadWait, Step::GfxHubEnable, Step::GfxGolden,
    Step::GfxConstants, Step::NbioDoorbells, Step::GfxRlcResume,
    Step::CpDoorbellRanges, Step::CpEnginesEnable,
    Step::MesKiqStart, Step::MesKiqQueue,
    Step::MesSchedulerMap, Step::MesSetResources
};
inline constexpr Step kSuffix[] = {
    Step::MesQueryStatus, Step::MesSchedulerReady,
    // Even after the early scheduler init, GC11's non-unified MES path
    // sends KIQ SET_RESOURCES before mapping kernel compute queues.
    Step::KcqMqd, Step::KcqKiqSetResources, Step::KcqMesMap,
    Step::KgqMqd, Step::KgqMesMap, Step::GfxStart,
    // These Linux ring tests modify SCRATCH_REG0. They do NOT prove an
    // IB's GPU memory writeback or a dma_fence completed successfully.
    Step::GfxScratchTest, Step::ComputeScratchTest,
    // The later MES IP hw_init takes its already-ready path and resets
    // KIQ sched.ready=false after the initialization-only KIQ use above.
    Step::MesHwFinalize
};
constexpr unsigned kPrefixCount = sizeof(kPrefix) / sizeof(kPrefix[0]);
constexpr unsigned kSuffixCount = sizeof(kSuffix) / sizeof(kSuffix[0]);
static_assert(kPrefixCount + 1u + kSuffixCount <= kMaxSteps,
              "Linux boot stages exceed plan capacity");

inline bool needsResources1(const Settings &s) {
    return (s.mesSchedulerVersion & kMesVersionMask) >= kMesResources1Version;
}
inline unsigned stageCount(const Settings &s) {
    return kPrefixCount + kSuffixCount + (needsResources1(s) ? 1u : 0u);
}
inline Step stageAt(const Settings &s, unsigned index) {
    if (index < kPrefixCount) return kPrefix[index];
    index -= kPrefixCount;
    if (needsResources1(s)) {
        if (!index) return Step::MesSetResources1;
        --index;
    }
    return index < kSuffixCount ? kSuffix[index] : Step::None;
}
} // namespace detail

inline bool Plan::valid() const {
    if (error != Error::Ok || detail::settingsError(settings) != Error::Ok ||
        count != detail::stageCount(settings) || count > kMaxSteps)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (stages[i] != detail::stageAt(settings, i)) return false;
    return true;
}

inline Plan makePlan(const Settings &s) {
    Plan p{};
    p.settings = s;
    p.error = detail::settingsError(s);
    if (p.error != Error::Ok) return p;
    p.count = detail::stageCount(s);
    for (unsigned i = 0; i < p.count; ++i)
        p.stages[i] = detail::stageAt(s, i);
    return p;
}

// This guard checks *reported* stage order. A caller-supplied "evidence"
// boolean is NEVER sufficient to establish a real GPU fence or ownership.
// Do not use it to authorize MMIO, DMA, firmware or GPU submissions.
struct Progress {
    Error error = Error::InvalidPlan;
    unsigned nextIndex = 0;
    Step nextStep = Step::None;
    bool done = false; // complete report, not permission or proof of acceleration
};
inline Progress checkProgress(const Plan &plan, const Step *events,
                              const bool *verified, unsigned count) {
    Progress out{};
    if (!plan.valid()) return out;
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
