// SPDX-License-Identifier: BSD-3-Clause
// Host-only ordering and probe status checks; no physical GPU execution.
#include "navi33_linux_boot_parity.h"
#include <assert.h>

using namespace rn_navi33::linux_boot;
static Settings validSettings(uint32_t version = 0x92u) {
    Settings s{};
    s.vendor = 0x1002u; s.device = 0x7480u;
    s.gcMajor = 11u; s.gcMinor = 0u; s.gcRevision = 2u;
    s.pspFirmware = true; s.mesKiq = true; s.asyncGfx = true;
    s.mesVersionObserved = true; s.mesSchedulerVersion = version;
    return s;
}
int main() {
    static_assert(!kHardwareWritesEnabled, "Do not enable GPU writes");
    Settings s = validSettings();
    Plan p = makePlan(s);
    assert(p.valid() && p.count == 28u);
    assert(p.stages[13] == Step::MesKiqStart);
    assert(p.stages[14] == Step::MesKiqQueue);
    assert(p.stages[15] == Step::MesSchedulerMap);
    assert(p.stages[16] == Step::MesSetResources);
    assert(p.stages[17] == Step::MesSetResources1);
    assert(p.stages[18] == Step::MesQueryStatus);
    assert(p.stages[19] == Step::MesOwnershipTransfer);
    assert(p.stages[21] == Step::KcqKiqSetResources);
    assert(p.stages[22] == Step::KcqMesMap);
    assert(p.stages[24] == Step::KgqMesMap);
    assert(p.stages[26] == Step::GfxFence);

    bool proved[kMaxSteps]{};
    for (unsigned i=0;i<p.count;++i) proved[i]=true;
    assert(checkProgress(p,p.stages,proved,0).nextStep == Step::Discovery);
    assert(checkProgress(p,p.stages,proved,16).nextStep == Step::MesSetResources);
    const Progress all=checkProgress(p,p.stages,proved,p.count);
    assert(all.done && all.error == Error::AlreadyComplete);
    Step wrong[kMaxSteps]{};
    for(unsigned i=0;i<p.count;++i) wrong[i]=p.stages[i];
    wrong[15]=Step::KgqMesMap;
    const Progress swapped=checkProgress(p,wrong,proved,p.count);
    assert(swapped.error == Error::WrongOrder && swapped.nextIndex==15u);
    proved[5]=false;
    const Progress missing=checkProgress(p,p.stages,proved,p.count);
    assert(missing.error == Error::MissingEvidence && missing.nextIndex==5u);
    assert(checkProgress(p,nullptr,nullptr,1).error == Error::WrongOrder);

    s=validSettings(0x47u);
    p=makePlan(s);
    assert(p.valid() && p.count==27u);
    assert(p.stages[17]==Step::MesQueryStatus);
    s=validSettings(0x51u);
    assert(makePlan(s).count==27u);
    s=validSettings(0x52u);
    assert(makePlan(s).count==28u);

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
