#include "navi33_xnu_access_evidence.h"
#include <cassert>
#include <stdint.h>

namespace ev = rn_navi33::xnu_access_evidence;

static ev::Fact tree(bool yes) {
    return {yes ? ev::Truth::ObservedYes : ev::Truth::ObservedNo,
            ev::Origin::IOServiceTree};
}
static ev::Fact open(bool yes) {
    return {yes ? ev::Truth::ObservedYes : ev::Truth::ObservedNo,
            ev::Origin::ProviderOpenQuery};
}
static ev::Fact mp1(bool yes) {
    return {yes ? ev::Truth::ObservedYes : ev::Truth::ObservedNo,
            ev::Origin::PassiveMp1MmioRead};
}
static ev::Fact traced(bool yes) {
    return {yes ? ev::Truth::ObservedYes : ev::Truth::ObservedNo,
            ev::Origin::InstrumentedWriteTrace};
}
static bool has(uint32_t value, ev::Blocker bit) {
    return (value & static_cast<uint32_t>(bit)) != 0u;
}
static void assertBlocked(const ev::Report &r) {
    assert(has(r.blockers, ev::ExternalHostWritersNotExcluded));
    assert(has(r.blockers, ev::FirmwareWritersNotExcluded));
    assert(has(r.blockers, ev::NoHardwareMailboxLease));
    assert(!r.externalHostWritersExcluded);
    assert(!r.firmwareWritersExcluded);
    assert(!r.mailboxExclusiveProven);
    assert(!r.mayIssueSmuCommand);
}
int main() {
    {   // No evidence is NOT equivalent to an explicit false.
        ev::Report r = ev::inspect({});
        assert(r.validHostSamples == 0u);
        assert(!r.latestOpenQueryValid);
        assert(!r.latestAnyHostOpenReportedNo);
        assert(has(r.blockers, ev::MissingLateHostQuery));
        assertBlocked(r);
    }
    {   // Physical P7.33: first sample at ~5 s sees only RedNavi;
        // second sample at ~13 s sees RedNavi, AMDSupport, Display_boot.
        // All observed host-open queries were false, NOT an MP1 lease.
        ev::Input in{};
        in.first.captured = true;
        in.first.amdSupportAttached = tree(false);
        in.first.bootFramebufferAttached = tree(false);
        in.first.providerOpenByAny = open(false);
        in.first.providerOpenByRedNavi = open(false);
        in.first.amdSupportProviderOpen = open(false);
        in.first.bootFramebufferProviderOpen = open(false);
        in.second.captured = true;
        in.second.amdSupportAttached = tree(true);
        in.second.bootFramebufferAttached = tree(true);
        in.second.providerOpenByAny = open(false);
        in.second.providerOpenByRedNavi = open(false);
        in.second.amdSupportProviderOpen = open(false);
        in.second.bootFramebufferProviderOpen = open(false);
        const auto r = ev::inspect(in);
        assert(r.validHostSamples == 2u);
        assert(r.latestOpenQueryValid);
        assert(r.latestAnyHostOpenReportedNo);
        assert(r.anyAppleServiceAttached);
        assert(!r.anyNamedAppleHostOpen);
        assert(!r.hostSnapshotInconsistent);
        assert(!has(r.blockers, ev::HostOpenObserved));
        assertBlocked(r);
    }
    {   // A provider-open observation is not a mailbox owner record.
        ev::Input in{};
        in.second.captured = true;
        in.second.amdSupportAttached = tree(true);
        in.second.providerOpenByAny = open(true);
        in.second.amdSupportProviderOpen = open(true);
        const auto r = ev::inspect(in);
        assert(r.anyNamedAppleHostOpen);
        assert(has(r.blockers, ev::HostOpenObserved));
        assertBlocked(r);
    }
    {   // A contradiction between child open and global open is not
        // an ownership proof; fail closed.
        ev::Input in{};
        in.second.captured = true;
        in.second.providerOpenByAny = open(false);
        in.second.amdSupportProviderOpen = open(true);
        const auto r = ev::inspect(in);
        assert(r.hostSnapshotInconsistent);
        assert(has(r.blockers, ev::HostSnapshotInconsistent));
        assertBlocked(r);
    }
    {   // An open at T1 and no open at T2 is NOT contradictory.
        ev::Input in{};
        in.first.captured = true;
        in.first.providerOpenByAny = open(true);
        in.first.amdSupportProviderOpen = open(true);
        in.second.captured = true;
        in.second.providerOpenByAny = open(false);
        in.second.amdSupportProviderOpen = open(false);
        const auto r = ev::inspect(in);
        assert(r.anyNamedAppleHostOpen);
        assert(!r.hostSnapshotInconsistent);
        assert(r.latestAnyHostOpenReportedNo);
        assert(has(r.blockers, ev::HostOpenObserved));
        assertBlocked(r);
    }
    {   // Sampling idle MP1 registers does NOT exclude an off-window
        // host or firmware writer.
        ev::Input in{};
        in.second.captured = true;
        in.second.providerOpenByAny = open(false);
        in.mp1ValueTransition = mp1(false);
        const auto r = ev::inspect(in);
        assert(!r.sampledMp1ValueTransition);
        assertBlocked(r);
    }
    {   // Seeing a changing mailbox value DOES NOT attribute the write.
        ev::Input in{};
        in.mp1ValueTransition = mp1(true);
        const auto r = ev::inspect(in);
        assert(r.sampledMp1ValueTransition);
        assert(!r.observedAttributedMp1Write);
        assertBlocked(r);
    }
    {   // Even an instrumented trace of one writer doesn't exclude
        // hidden writers in other drivers, PCI functions or firmware.
        ev::Input in{};
        in.attributedMp1Write = traced(true);
        const auto r = ev::inspect(in);
        assert(r.observedAttributedMp1Write);
        assertBlocked(r);
    }
    {   // Typing a host-open query as a service-tree observation must
        // be detected, rather than trusted as 'open=false'.
        ev::Input in{};
        in.second.captured = true;
        in.second.providerOpenByAny = tree(false);
        const auto r = ev::inspect(in);
        assert(r.factProvenanceInvalid);
        assert(!r.latestOpenQueryValid);
        assert(has(r.blockers, ev::InvalidFactProvenance));
        assertBlocked(r);
    }
    {   // No amount of optimistic synthetic evidence opens the
        // separate hardware/firmware admission gate.
        ev::Input in{};
        in.first.captured = true;
        in.second.captured = true;
        in.second.providerOpenByAny = open(false);
        in.second.amdSupportAttached = tree(false);
        in.second.bootFramebufferAttached = tree(false);
        in.mp1ValueTransition = mp1(false);
        in.attributedMp1Write = traced(false);
        const auto r = ev::inspect(in);
        assertBlocked(r);
    }
    return 0;
}
