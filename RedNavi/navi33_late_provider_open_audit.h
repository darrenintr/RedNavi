#ifndef REDNAVI_NAVI33_LATE_PROVIDER_OPEN_AUDIT_H
#define REDNAVI_NAVI33_LATE_PROVIDER_OPEN_AUDIT_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P7.33: interpret an after-registerService IOPCIDevice host-open snapshot.
// This is observational IOKit host state, not SMU firmware arbitration.
// IOService children can exist without opening the provider, and kernel/
// firmware or outside BAR5 clients may access the mailbox independently.
#include <stdint.h>

namespace rn_navi33 { namespace late_provider_open_audit {

struct Input {
    bool sampled = false;
    bool childIteratorAvailable = false;
    uint32_t directChildrenObserved = 0u;
    bool providerOpenByAny = false;
    bool providerOpenByRedNavi = false;
    bool amdSupportPresent = false;
    bool amdSupportProviderOpen = false;
    bool bootFramebufferPresent = false;
    bool bootFramebufferProviderOpen = false;
};

struct Report {
    bool sampled = false;
    bool knownSiblingProviderOpen = false;
    bool otherHostOpenWithoutRedNavi = false;
    bool providerOpenByAny = false;
    bool providerOpenByRedNavi = false;
    bool amdSupportPresent = false;
    bool bootFramebufferPresent = false;
    // Never infer any real mailbox ownership from IOService::isOpen().
    bool foreignMailboxWriterExcluded = false;
    bool mailboxExclusiveProven = false;
    bool mayIssueSmuCommand = false;
};

inline Report inspect(const Input &s) {
    Report r{};
    r.sampled = s.sampled;
    if (!s.sampled) return r;
    r.providerOpenByAny = s.providerOpenByAny;
    r.providerOpenByRedNavi = s.providerOpenByRedNavi;
    r.amdSupportPresent = s.amdSupportPresent;
    r.bootFramebufferPresent = s.bootFramebufferPresent;
    r.knownSiblingProviderOpen =
        (s.amdSupportPresent && s.amdSupportProviderOpen) ||
        (s.bootFramebufferPresent && s.bootFramebufferProviderOpen);
    r.otherHostOpenWithoutRedNavi =
        s.providerOpenByAny && !s.providerOpenByRedNavi;
    // No ownership conclusion is safe even when there is no open client.
    return r;
}

}} // namespace rn_navi33::late_provider_open_audit
#endif
