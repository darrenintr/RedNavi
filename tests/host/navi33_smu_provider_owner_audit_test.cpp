#include "navi33_smu_provider_owner_audit.h"
#include <cassert>
using namespace rn_navi33::smu_provider_owner_audit;

int main() {
    Input in{};
    auto r=inspect(in);
    assert(r.providerState==ProviderState::NotRequested);
    assert(!r.mailboxExclusiveProven && !r.mayIssueSmuCommand);
    in.requested = true;
    r=inspect(in);
    assert(r.providerState==ProviderState::NoObservedOpen);
    assert(!r.otherOpenOwnerObserved && !r.ownProviderOpenObserved);
    in.providerOpenByAny = true;
    r=inspect(in);
    assert(r.providerState==ProviderState::OpenByOtherClient);
    assert(r.otherOpenOwnerObserved && !r.ownProviderOpenObserved);
    in.providerOpenByRedNavi = true;
    r=inspect(in);
    assert(r.providerState==ProviderState::OpenByRedNavi);
    assert(!r.otherOpenOwnerObserved && r.ownProviderOpenObserved);
    // Even opening the PCI provider is insufficient to prove that no
    // firmware/other-function/direct-register writer can interleave.
    assert(!r.mailboxExclusiveProven && !r.mayIssueSmuCommand);
    in.providerOpenByAny = false;
    r=inspect(in);
    assert(r.providerState==ProviderState::ContradictorySnapshot);
    assert(!r.ownProviderOpenObserved);
    // Sibling existence does not imply an open owner or mailbox writer.
    in.providerOpenByAny=true;
    in.providerOpenByRedNavi=false;
    in.amdSupportSibling=true;
    in.bootFramebufferSibling=true;
    in.childSnapshotAvailable=true;
    in.directChildrenObserved=3;
    r=inspect(in);
    assert(r.providerState==ProviderState::OpenByOtherClient);
    assert(!r.mailboxExclusiveProven && !r.mayIssueSmuCommand);
    in.providerOpenByAny=false;
    r=inspect(in);
    assert(r.providerState==ProviderState::NoObservedOpen);
    assert(!r.mailboxExclusiveProven && !r.mayIssueSmuCommand);
}
