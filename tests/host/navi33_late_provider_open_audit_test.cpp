#include "navi33_late_provider_open_audit.h"
#include <cassert>

namespace audit = rn_navi33::late_provider_open_audit;

int main() {
    audit::Input input{};
    auto missing = audit::inspect(input);
    assert(!missing.sampled && !missing.mailboxExclusiveProven &&
           !missing.mayIssueSmuCommand);

    input.sampled = true;
    input.childIteratorAvailable = true;
    input.directChildrenObserved = 3;
    input.amdSupportPresent = true;
    input.bootFramebufferPresent = true;
    auto attachedOnly = audit::inspect(input);
    assert(attachedOnly.sampled && attachedOnly.amdSupportPresent &&
           attachedOnly.bootFramebufferPresent &&
           !attachedOnly.providerOpenByAny && !attachedOnly.knownSiblingProviderOpen &&
           !attachedOnly.foreignMailboxWriterExcluded &&
           !attachedOnly.mailboxExclusiveProven &&
           !attachedOnly.mayIssueSmuCommand);

    input.providerOpenByAny = true;
    input.amdSupportProviderOpen = true;
    auto amdOpened = audit::inspect(input);
    assert(amdOpened.knownSiblingProviderOpen &&
           amdOpened.otherHostOpenWithoutRedNavi &&
           !amdOpened.mailboxExclusiveProven &&
           !amdOpened.mayIssueSmuCommand);

    input.amdSupportProviderOpen = false;
    input.providerOpenByRedNavi = true;
    auto rednaviOpened = audit::inspect(input);
    assert(!rednaviOpened.otherHostOpenWithoutRedNavi &&
           !rednaviOpened.knownSiblingProviderOpen &&
           !rednaviOpened.mailboxExclusiveProven &&
           !rednaviOpened.mayIssueSmuCommand);

    input.bootFramebufferProviderOpen = true;
    auto bootOpened = audit::inspect(input);
    assert(bootOpened.knownSiblingProviderOpen &&
           !bootOpened.mailboxExclusiveProven &&
           !bootOpened.mayIssueSmuCommand);

    // Parent reports an open client, but it is not one of the two
    // recognized named siblings. This must not be treated as proof
    // that those siblings wrote the SMU mailbox.
    input.providerOpenByRedNavi = false;
    input.bootFramebufferProviderOpen = false;
    auto unknownOpen = audit::inspect(input);
    assert(unknownOpen.otherHostOpenWithoutRedNavi);
    assert(!unknownOpen.knownSiblingProviderOpen);
    assert(!unknownOpen.foreignMailboxWriterExcluded);
}
