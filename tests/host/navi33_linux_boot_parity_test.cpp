// SPDX-License-Identifier: BSD-3-Clause
// Host-only ordering and probe status checks; no physical GPU execution.
#include "navi33_linux_boot_parity.h"
#include <assert.h>

using namespace rn_navi33::linux_boot;
static Settings validSettings(uint32_t version = 0x92u) {
    Settings s{};
    s.vendor = 0x1002u; s.device = 0x7480u;
    s.gcMajor = 11u; s.gcMinor = 0u; s.gcRevision = 2u;
    s.pspMajor = 13u; s.pspMinor = 0u; s.pspRevision = 7u;
    s.smuMajor = 13u; s.smuMinor = 0u; s.smuRevision = 7u;
    s.pspFirmware = true; s.mesKiq = true; s.asyncGfx = true;
    s.mesVersionObserved = true; s.mesSchedulerVersion = version;
    return s;
}

static unsigned stageIndex(const Plan &p, Step step) {
    for (unsigned i = 0; i < p.count; ++i)
        if (p.stages[i] == step) return i;
    return p.count;
}

static void before(const Plan &p, Step first, Step second) {
    const unsigned a = stageIndex(p, first), b = stageIndex(p, second);
    assert(a < p.count && b < p.count && a < b);
}

static void rejectReorderedReport(const Plan &p, Step first, Step second) {
    Step events[kMaxSteps]{};
    bool reported[kMaxSteps]{};
    for (unsigned i = 0; i < p.count; ++i) {
        events[i] = p.stages[i]; reported[i] = true;
    }
    const unsigned a = stageIndex(p, first), b = stageIndex(p, second);
    assert(a < b && b < p.count);
    events[a] = second; events[b] = first;
    const Progress result = checkProgress(p, events, reported, p.count);
    assert(result.error == Error::WrongOrder && result.nextIndex == a);
    assert(result.nextStep == first && !result.done);
}

static void rejectMalformedPlan(const Plan &p) {
    assert(!p.valid());
    const Progress result = checkProgress(p, nullptr, nullptr, 0);
    assert(result.error == Error::InvalidPlan && !result.done);
    assert(result.nextStep == Step::None);
}

int main() {
    static_assert(!kHardwareWritesEnabled, "Do not enable GPU writes");
    Settings s = validSettings();
    Plan p = makePlan(s);
    assert(p.valid() && p.count == 42u);
    before(p, Step::CommonSwReady, Step::CommonHw);
    before(p, Step::GmcSwReady, Step::GmcMmhubHw);
    before(p, Step::GmcMmhubHw, Step::WritebackReady);
    before(p, Step::WritebackReady, Step::IpSoftwareReady);
    before(p, Step::IpSoftwareReady, Step::FirmwareBoReady);
    before(p, Step::IbPoolReady, Step::FirmwareBoReady);
    before(p, Step::FirmwareBoReady, Step::PspRingReady);
    before(p, Step::IhHw, Step::PspRingReady);
    before(p, Step::PspRingReady, Step::PspTmrAllocated);
    before(p, Step::PspTmrAllocated, Step::PspSmuFirmware);
    before(p, Step::PspSmuFirmware, Step::PspTmrInstalled);
    before(p, Step::PspTmrInstalled, Step::PspGraphicsFirmware);
    before(p, Step::PspGraphicsFirmware, Step::PspRlcAutoloadStart);
    before(p, Step::PspRlcAutoloadStart, Step::PspFwComplete);
    before(p, Step::PspFwComplete, Step::SmuHw);
    before(p, Step::GfxRlcAutoloadWait, Step::GfxHubEnable);
    before(p, Step::CpEnginesEnable, Step::MesKiqStart);
    before(p, Step::MesKiqQueue, Step::MesSchedulerMap);
    before(p, Step::MesSetResources, Step::MesSetResources1);
    before(p, Step::MesSetResources1, Step::MesQueryStatus);
    before(p, Step::MesSchedulerReady, Step::KcqKiqSetResources);
    before(p, Step::KcqKiqSetResources, Step::KcqMesMap);
    before(p, Step::KcqMesMap, Step::KgqMesMap);
    before(p, Step::GfxStart, Step::GfxScratchTest);
    before(p, Step::GfxScratchTest, Step::ComputeScratchTest);
    assert(p.stages[p.count - 1u] == Step::MesHwFinalize);

    rejectReorderedReport(p, Step::PspSmuFirmware, Step::PspTmrInstalled);
    rejectReorderedReport(p, Step::GmcMmhubHw, Step::PspRingReady);
    rejectReorderedReport(p, Step::GfxRlcAutoloadWait, Step::GfxHubEnable);
    rejectReorderedReport(p, Step::MesSchedulerReady, Step::KgqMesMap);
    rejectReorderedReport(p, Step::KcqKiqSetResources, Step::KcqMesMap);

    bool proved[kMaxSteps]{};
    for (unsigned i=0;i<p.count;++i) proved[i]=true;
    assert(checkProgress(p,p.stages,proved,0).nextStep == Step::Discovery);
    for (unsigned count = 0; count < p.count; ++count) {
        const Progress partial = checkProgress(p, p.stages, proved, count);
        assert(partial.error == Error::Ok && !partial.done);
        assert(partial.nextIndex == count && partial.nextStep == p.stages[count]);
    }
    const Progress all=checkProgress(p,p.stages,proved,p.count);
    assert(all.done && all.error == Error::AlreadyComplete);
    assert(all.nextIndex == p.count && all.nextStep == Step::None);
    const unsigned unproved = stageIndex(p, Step::PspTmrInstalled);
    proved[unproved]=false;
    const Progress missing=checkProgress(p,p.stages,proved,p.count);
    assert(missing.error == Error::MissingEvidence && missing.nextIndex == unproved);
    assert(missing.nextStep == Step::PspTmrInstalled && !missing.done);
    assert(checkProgress(p,nullptr,nullptr,1).error == Error::WrongOrder);
    assert(checkProgress(p,nullptr,nullptr,p.count+1u).error == Error::WrongOrder);

    Plan malformed{};
    malformed.error = Error::Ok;
    rejectMalformedPlan(malformed); // Empty plans cannot report completion.
    malformed = p; malformed.count = 0;
    rejectMalformedPlan(malformed);
    malformed = p; malformed.count = kMaxSteps + 1u;
    rejectMalformedPlan(malformed);
    malformed = p; --malformed.count;
    rejectMalformedPlan(malformed); // Final callback cannot be dropped.
    malformed = p; malformed.stages[unproved] = Step::PspSmuFirmware;
    rejectMalformedPlan(malformed);
    malformed = p; malformed.stages[unproved] = Step::None;
    rejectMalformedPlan(malformed);
    malformed = p; malformed.settings.mesSchedulerVersion = 0x47u;
    rejectMalformedPlan(malformed); // Version and optional stages must agree.
    malformed = p; malformed.settings.device = 0x747eu;
    rejectMalformedPlan(malformed);

    s=validSettings(0x47u);
    p=makePlan(s);
    assert(p.valid() && p.count==41u);
    assert(stageIndex(p, Step::MesSetResources1) == p.count);
    s=validSettings(0x51u);
    assert(makePlan(s).count==41u);
    s=validSettings(0x52u);
    assert(makePlan(s).count==42u);
    s=validSettings(0x00020092u); // API-version bits are outside the version mask.
    assert(makePlan(s).valid() && makePlan(s).count==42u);

    s=validSettings();
    s.mesVersionObserved=false;
    assert(makePlan(s).error == Error::UnknownMesVersion);
    s=validSettings(0x46u);
    assert(makePlan(s).error == Error::UnsupportedMesVersion);
    s=validSettings();
    s.directG34G42Active=true;
    assert(makePlan(s).error == Error::LegacyQueueConflict);
    s=validSettings();
    s.asyncGfx=false;
    assert(makePlan(s).error == Error::WrongBootPath);
    s=validSettings();
    s.device=0x747Eu;
    assert(makePlan(s).error == Error::UnsupportedAsic);
    s=validSettings();
    s.gcRevision=3u;
    assert(makePlan(s).error == Error::UnsupportedAsic);
    s=validSettings();
    s.pspRevision=10u;
    assert(makePlan(s).error == Error::UnsupportedIpVersion);
    s=validSettings();
    s.smuRevision=10u;
    assert(makePlan(s).error == Error::UnsupportedIpVersion);

    Snapshot snap{};
    auto obs=inspect(snap);
    assert(obs.blocker==Blocker::Discovery && !obs.hardwareInitAllowed);
    snap.identityVerified=true;
    assert(inspect(snap).blocker==Blocker::MissingBases);
    snap.basesVerified=true;
    assert(inspect(snap).blocker==Blocker::MissingMemory);
    snap.memoryWindowVerified=true;
    assert(inspect(snap).blocker==Blocker::PspMailboxUnreadable);
    snap.pspMailboxRead=true;
    assert(inspect(snap).blocker==Blocker::PspIfwiNotReady);
    snap.ifwiReady=true;
    obs=inspect(snap);
    assert(obs.blocker==Blocker::NoVerifiedGpuOwner && obs.observationMask==31u);
    assert(!obs.hardwareInitAllowed);
    snap.sosAlive=true;
    obs=inspect(snap);
    assert(obs.observationMask==63u && !obs.hardwareInitAllowed);
}
