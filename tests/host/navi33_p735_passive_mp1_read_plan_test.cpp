#include "navi33_p735_passive_mp1_read_plan.h"
#include <cassert>
#include <cstdint>

namespace p = rn_navi33::p735_passive_mp1_read_plan;

static void assertNeverGrantsHardwareAccess(const p::Report &r) {
    assert(!r.liveReadPermitted);
    assert(!r.registerReadSideEffectsExcluded);
    assert(!r.foreignWritersExcluded);
    assert(!r.firmwareWritersExcluded);
    assert(!r.mailboxExclusiveProven);
    assert(!r.mayIssueSmuCommand);
    assert(!r.requiresRsmuIndexWrite);
    assert(!r.requiresVramIndexWrite);
}
int main() {
    p::Input good{};
    good.exactNavi33 = true;
    good.mp1RevisionVerified = true;
    good.bar5DirectMappingReady = true;
    good.mp1BaseDword = 90112u; // physical P7.33 MP1 register base
    good.bar5MappedBytes = 1048576ull; // physical RX 7600 BAR5

    const auto mapped = p::plan(good);
    assert(mapped.refusal == p::Refusal::None);
    assert(mapped.offsetsVerifiedInDirectMappedBar);
    assert(mapped.msgByteOffset == 363016u);
    assert(mapped.paramByteOffset == 363080u);
    assert(mapped.responseByteOffset == 363112u);
    assertNeverGrantsHardwareAccess(mapped);

    { auto bad = good; bad.exactNavi33 = false;
      const auto r = p::plan(bad);
      assert(r.refusal == p::Refusal::WrongAsic);
      assert(!r.offsetsVerifiedInDirectMappedBar);
      assertNeverGrantsHardwareAccess(r); }

    { auto bad = good; bad.mp1RevisionVerified = false;
      const auto r = p::plan(bad);
      assert(r.refusal == p::Refusal::WrongMp1Revision);
      assert(!r.offsetsVerifiedInDirectMappedBar);
      assertNeverGrantsHardwareAccess(r); }

    { auto bad = good; bad.bar5DirectMappingReady = false;
      const auto r = p::plan(bad);
      assert(r.refusal == p::Refusal::NoBarMapping);
      assert(!r.offsetsVerifiedInDirectMappedBar);
      assertNeverGrantsHardwareAccess(r); }

    { auto bad = good; bad.bar5MappedBytes = 363112u;
      const auto r = p::plan(bad); // final response dword out of BAR
      assert(r.refusal == p::Refusal::BadRegisterAddress);
      assertNeverGrantsHardwareAccess(r); }

    { auto bad = good; bad.mp1BaseDword = UINT32_MAX;
      const auto r = p::plan(bad);
      assert(r.refusal == p::Refusal::BadRegisterAddress);
      assertNeverGrantsHardwareAccess(r); }

    { auto bad = good; bad.mp1BaseDword = 0u;
      const auto r = p::plan(bad);
      assert(r.refusal == p::Refusal::BadRegisterAddress);
      assertNeverGrantsHardwareAccess(r); }

    { auto bad = good; bad.bar5MappedBytes = 0u;
      const auto r = p::plan(bad);
      assert(r.refusal == p::Refusal::BadRegisterAddress);
      assertNeverGrantsHardwareAccess(r); }

    { auto bad = good; bad.bar5MappedBytes = 1048575ull;
      const auto r = p::plan(bad);
      assert(r.refusal == p::Refusal::None);
      assertNeverGrantsHardwareAccess(r); }

    return 0;
}
