#ifndef REDNAVI_NAVI33_XNU_ACCESS_EVIDENCE_H
#define REDNAVI_NAVI33_XNU_ACCESS_EVIDENCE_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P7.34: provenance-aware, HOST-ONLY evidence interpretation.
// This header deliberately contains no IOKit calls, BAR mapping, MMIO,
// firmware access, or command-authorizing path.
//
// Three independent levels must never be conflated:
//   IORegistry child attachment != IOPCIDevice open(client)
//   IOPCIDevice open(client) != exclusive BAR5/MMIO ownership
//   C2PMSG value transition != attribution to any particular writer
//
// NullMoth NVRM uses PCI config and BAR operations without fPCI->open();
// Cobalt64 Navi33 separates DMA/IOVA ownership from GPU execution.
// These are *design observations*, not assumptions about Apple AMDSupport.

#include <stdint.h>

namespace rn_navi33 { namespace xnu_access_evidence {

enum class Truth : uint8_t {
    Unknown = 0,
    ObservedNo = 1,
    ObservedYes = 2,
};

enum class Origin : uint8_t {
    None = 0,
    IOServiceTree = 1,
    ProviderOpenQuery = 2,
    PassiveMp1MmioRead = 3,
    InstrumentedWriteTrace = 4,
};

struct Fact {
    Truth truth = Truth::Unknown;
    Origin origin = Origin::None;
};

struct HostSample {
    bool captured = false;
    Fact amdSupportAttached {};
    Fact bootFramebufferAttached {};
    Fact providerOpenByAny {};
    Fact providerOpenByRedNavi {};
    Fact amdSupportProviderOpen {};
    Fact bootFramebufferProviderOpen {};
};

struct Input {
    HostSample first {};
    HostSample second {};
    Fact mp1ValueTransition {};
    Fact attributedMp1Write {};
};

enum Blocker : uint32_t {
    MissingLateHostQuery = 1u << 0,
    HostOpenObserved = 1u << 1,
    HostSnapshotInconsistent = 1u << 2,
    InvalidFactProvenance = 1u << 3,
    ExternalHostWritersNotExcluded = 1u << 4,
    FirmwareWritersNotExcluded = 1u << 5,
    NoHardwareMailboxLease = 1u << 6,
};

struct Report {
    uint32_t validHostSamples = 0u;
    uint32_t blockers = ExternalHostWritersNotExcluded |
                        FirmwareWritersNotExcluded | NoHardwareMailboxLease;
    bool latestOpenQueryValid = false;
    bool latestAnyHostOpenReportedNo = false;
    bool anyAppleServiceAttached = false;
    bool anyNamedAppleHostOpen = false;
    bool sampledMp1ValueTransition = false;
    bool observedAttributedMp1Write = false;
    bool hostSnapshotInconsistent = false;
    bool factProvenanceInvalid = false;
    // Never promote these without an independently reviewed hardware
    // arbitration model. THIS MODEL CANNOT ISSUE COMMANDS.
    bool externalHostWritersExcluded = false;
    bool firmwareWritersExcluded = false;
    bool mailboxExclusiveProven = false;
    bool mayIssueSmuCommand = false;
};

inline bool valid(const Fact &f, Origin expected) {
    return f.truth != Truth::Unknown && f.origin == expected;
}
inline bool yes(const Fact &f, Origin expected) {
    return valid(f, expected) && f.truth == Truth::ObservedYes;
}
inline bool no(const Fact &f, Origin expected) {
    return valid(f, expected) && f.truth == Truth::ObservedNo;
}
inline bool illTyped(const Fact &f, Origin expected) {
    return f.truth != Truth::Unknown && f.origin != expected;
}

inline void includeHostSample(const HostSample &s, Report &r) {
    if (!s.captured) return;
    ++r.validHostSamples;
    const Origin tree = Origin::IOServiceTree;
    const Origin open = Origin::ProviderOpenQuery;
    if (illTyped(s.amdSupportAttached, tree) ||
        illTyped(s.bootFramebufferAttached, tree) ||
        illTyped(s.providerOpenByAny, open) ||
        illTyped(s.providerOpenByRedNavi, open) ||
        illTyped(s.amdSupportProviderOpen, open) ||
        illTyped(s.bootFramebufferProviderOpen, open))
        r.factProvenanceInvalid = true;
    if (yes(s.amdSupportAttached, tree) ||
        yes(s.bootFramebufferAttached, tree))
        r.anyAppleServiceAttached = true;
    if (yes(s.amdSupportProviderOpen, open) ||
        yes(s.bootFramebufferProviderOpen, open))
        r.anyNamedAppleHostOpen = true;
    if (yes(s.providerOpenByRedNavi, open) &&
        no(s.providerOpenByAny, open))
        r.hostSnapshotInconsistent = true;
    if (r.anyNamedAppleHostOpen &&
        no(s.providerOpenByAny, open))
        r.hostSnapshotInconsistent = true;
}

inline Report inspect(const Input &in) {
    Report r{};
    includeHostSample(in.first, r);
    includeHostSample(in.second, r);
    // A last observed 'false' is only a time-local host API sample.
    // Do not claim no other actors can map a BAR or submit a message.
    const HostSample &last = in.second.captured ? in.second : in.first;
    r.latestOpenQueryValid =
        last.captured &&
        valid(last.providerOpenByAny, Origin::ProviderOpenQuery);
    r.latestAnyHostOpenReportedNo =
        r.latestOpenQueryValid &&
        no(last.providerOpenByAny, Origin::ProviderOpenQuery);
    if (yes(in.first.providerOpenByAny, Origin::ProviderOpenQuery) ||
        yes(in.second.providerOpenByAny, Origin::ProviderOpenQuery) ||
        r.anyNamedAppleHostOpen)
        r.blockers |= HostOpenObserved;
    r.sampledMp1ValueTransition =
        yes(in.mp1ValueTransition, Origin::PassiveMp1MmioRead);
    r.observedAttributedMp1Write =
        yes(in.attributedMp1Write, Origin::InstrumentedWriteTrace);
    if (illTyped(in.mp1ValueTransition, Origin::PassiveMp1MmioRead) ||
        illTyped(in.attributedMp1Write, Origin::InstrumentedWriteTrace))
        r.factProvenanceInvalid = true;
    if (!r.latestOpenQueryValid) r.blockers |= MissingLateHostQuery;
    if (r.hostSnapshotInconsistent)
        r.blockers |= HostSnapshotInconsistent;
    if (r.factProvenanceInvalid)
        r.blockers |= InvalidFactProvenance;
    // Even a perfectly stable or changing mailbox value, or an
    // attributed write from *one* actor, cannot exclude *all* others.
    // Hardware ownership admission continues to require an unrelated,
    // deliberately disabled P7.27 gate.
    return r;
}

}} // namespace rn_navi33::xnu_access_evidence
#endif
