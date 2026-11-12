#ifndef REDNAVI_NAVI33_SMU_PROVIDER_OWNER_AUDIT_H
#define REDNAVI_NAVI33_SMU_PROVIDER_OWNER_AUDIT_H
// SPDX-License-Identifier: BSD-3-Clause
//
// Read-only host PCI provider ownership snapshot for Navi33 P7.28.
// An IOKit child service is not necessarily an IOPCIDevice open client.
// IOPCIDevice::isOpen() does not establish exclusive access to the SMU
// mailbox across firmware, other PCI functions, or raw BAR5 mappings.
// This file contains no hardware I/O and cannot authorize SMU commands.
#include <stdint.h>

namespace rn_navi33 { namespace smu_provider_owner_audit {

enum class ProviderState : uint8_t {
    NotRequested = 0,
    NoObservedOpen = 1,
    OpenByOtherClient = 2,
    OpenByRedNavi = 3,
    ContradictorySnapshot = 4,
};

struct Input {
    bool requested = false;
    bool providerOpenByAny = false;
    bool providerOpenByRedNavi = false;
    bool childSnapshotAvailable = false;
    uint32_t directChildrenObserved = 0;
    bool amdSupportSibling = false;
    bool bootFramebufferSibling = false;
    // The audited P7.27 implementation does not serialize all external
    // clients of BAR5, and the provider was never opened by RedNavi.
    bool allRedNaviSmcSendersSerialized = false;
    bool foreignMailboxWriterExcluded = false;
};

struct Report {
    ProviderState providerState = ProviderState::NotRequested;
    bool otherOpenOwnerObserved = false;
    bool ownProviderOpenObserved = false;
    bool mailboxExclusiveProven = false; // intentionally cannot be inferred
    bool mayIssueSmuCommand = false;     // independent P7.27 admission gate
};

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (in.providerOpenByRedNavi && !in.providerOpenByAny) {
        r.providerState = ProviderState::ContradictorySnapshot;
        return r;
    }
    if (in.providerOpenByRedNavi) {
        r.providerState = ProviderState::OpenByRedNavi;
        r.ownProviderOpenObserved = true;
    } else if (in.providerOpenByAny) {
        r.providerState = ProviderState::OpenByOtherClient;
        r.otherOpenOwnerObserved = true;
    } else {
        r.providerState = ProviderState::NoObservedOpen;
    }
    // A successful IOPCIDevice open would only establish host-provider
    // arbitration. It is not enough to certify all potential mailbox
    // writers, so P7.28 is ALWAYS observation-only.
    return r;
}

}} // namespace rn_navi33::smu_provider_owner_audit
#endif
