#include "navi33_p737_passive_mailbox_triplet.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

namespace p = rn_navi33::p737_passive_mailbox_triplet;

static p::Config validConfig() {
    p::Config c{};
    c.explicitOptIn = true;
    c.p736ResponseArmed = true;
    c.exactNavi33 = true;
    c.mp1Is13_0_7 = true;
    c.directBar5MappingReady = true;
    c.mp1BaseDword = 90112u;
    c.bar5MappedBytes = 1048576ull;
    return c;
}

static void neverAttribute(const p::Report &r) {
    assert(!r.rednaviCommandSubmittedProven);
    assert(!r.appleOrOtherHostWriterAttributed);
    assert(!r.firmwareResponseToNewCommandProven);
    assert(!r.mailboxExclusiveProven);
    assert(!r.mayIssueSmuCommand);
}

int main() {
    {
        p::State s{};
        p::ImageBudget b{};
        p::Config c = validConfig();
        c.explicitOptIn = false;
        assert(p::configure(s, c) == p::Status::Disabled);
        uint32_t reads = 0u;
        const auto r = p::sample(
            s,b,1u,true,true,c.bar5MappedBytes,1u,1u,363112u,
            [&](uint32_t) { ++reads; return 1u; });
        assert(r.status == p::Status::Disabled);
        assert(reads == 0u && p::claimedReads(b) == 0u);
        neverAttribute(r);
    }
    {
        p::State s{};
        p::Config c = validConfig();
        c.p736ResponseArmed = false;
        assert(p::configure(s, c) == p::Status::RequiresP736OptIn);
        c = validConfig(); c.exactNavi33 = false;
        assert(p::configure(s,c) == p::Status::WrongAsic);
        c = validConfig(); c.mp1Is13_0_7 = false;
        assert(p::configure(s,c) == p::Status::WrongMp1Revision);
        c = validConfig(); c.directBar5MappingReady = false;
        assert(p::configure(s,c) == p::Status::NoBarMapping);
        c = validConfig(); c.mp1BaseDword = 0u;
        assert(p::configure(s,c) == p::Status::InvalidDirectOffsets);
        c = validConfig(); c.mp1BaseDword = UINT32_MAX;
        assert(p::configure(s,c) == p::Status::InvalidDirectOffsets);
        c = validConfig(); c.bar5MappedBytes = 363110u;
        assert(p::configure(s,c) == p::Status::InvalidDirectOffsets);
        assert(!s.armed);
    }
    {
        p::State s{};
        p::ImageBudget b{};
        const auto c = validConfig();
        assert(p::configure(s,c) == p::Status::Armed);
        assert(s.messageByteOffset == 363016u);
        assert(s.parameterByteOffset == 363080u);
        assert(s.responseByteOffset == 363112u);
        uint32_t reads = 0u;
        const auto reader = [&](uint32_t off) {
            ++reads;
            if (off == 363016u) return reads <= 2u ? 0x31u : 0x32u;
            assert(off == 363080u);
            return 0x1234u;
        };
        const auto idle = p::sample(s,b,1u,false,true,
            c.bar5MappedBytes,1u,1u,363112u,reader);
        assert(idle.status == p::Status::InactiveService);
        const auto unmapped = p::sample(s,b,1u,true,false,
            c.bar5MappedBytes,1u,1u,363112u,reader);
        assert(unmapped.status == p::Status::MappingLost);
        const auto small = p::sample(s,b,1u,true,true,
            363112u,1u,1u,363112u,reader);
        assert(small.status == p::Status::MappingLost);
        const auto noResponse = p::sample(s,b,1u,true,true,
            c.bar5MappedBytes,0u,1u,363112u,reader);
        assert(noResponse.status == p::Status::MissingPairedResponse);
        const auto mismatchedOffset = p::sample(s,b,1u,true,true,
            c.bar5MappedBytes,1u,1u,1234u,reader);
        assert(mismatchedOffset.status == p::Status::MissingPairedResponse);
        assert(reads == 0u && p::claimedReads(b) == 0u);

        const auto first = p::sample(s,b,1u,true,true,
            c.bar5MappedBytes,1u,1u,363112u,reader);
        assert(first.status == p::Status::Sampled && first.readPerformed);
        assert(first.message == 0x31u && first.parameter == 0x1234u);
        assert(first.response == 1u);
        assert(first.evidence == p::Evidence::FirstSnapshot);
        assert(first.pairedResponseFromThisTick);
        assert(!first.previousTripletAvailable);
        assert(first.additionalReadsThisSample == 2u);
        assert(reads == 2u && p::claimedReads(b) == 2u);
        neverAttribute(first);

        const auto duplicate = p::sample(s,b,1u,true,true,
            c.bar5MappedBytes,1u,1u,363112u,reader);
        assert(duplicate.status == p::Status::MissingPairedResponse);
        assert(reads == 2u);
        neverAttribute(duplicate);

        const auto second = p::sample(s,b,2u,true,true,
            c.bar5MappedBytes,2u,1u,363112u,reader);
        assert(second.status == p::Status::Sampled);
        assert(second.message == 0x32u && second.parameter == 0x1234u);
        assert(second.response == 1u);
        assert(second.evidence == p::Evidence::UnattributedRegisterChange);
        assert(second.messageChanged && !second.parameterChanged);
        assert(!second.responseChanged);
        assert(second.previousTripletAvailable);
        assert(second.serviceTriplets == 2u);
        assert(reads == 4u && p::claimedReads(b) == 4u);
        neverAttribute(second);

        const auto exhausted = p::sample(s,b,3u,true,true,
            c.bar5MappedBytes,3u,1u,363112u,reader);
        assert(exhausted.status == p::Status::ServiceBudgetExhausted);
        assert(!exhausted.readPerformed && reads == 4u);
        p::stop(s);
        const auto stopped = p::sample(s,b,3u,true,true,
            c.bar5MappedBytes,3u,1u,363112u,reader);
        assert(stopped.status == p::Status::Stopped);
        assert(reads == 4u);
        neverAttribute(stopped);
    }
    {
        p::ImageBudget b{};
        p::State s{};
        const auto c = validConfig();
        assert(p::configure(s,c) == p::Status::Armed);
        const auto read = [](uint32_t off) {
            return off == 363016u ? 0x13u : 0x77u;
        };
        auto first = p::sample(s,b,1u,true,true,c.bar5MappedBytes,
                               1u,1u,363112u,read);
        auto second = p::sample(s,b,2u,true,true,c.bar5MappedBytes,
                                2u,1u,363112u,read);
        assert(first.evidence == p::Evidence::FirstSnapshot);
        assert(second.evidence == p::Evidence::StableBetweenSamples);
        assert(!second.messageChanged && !second.parameterChanged);
        assert(!second.responseChanged);
        neverAttribute(second);
        p::stop(s);
        p::State newService{};
        assert(p::configure(newService,c) == p::Status::Armed);
        const auto denied = p::sample(newService,b,1u,true,true,
                                     c.bar5MappedBytes,1u,1u,363112u,read);
        assert(denied.status == p::Status::ImageBudgetExhausted);
        assert(!denied.readPerformed);
        neverAttribute(denied);
    }
    {
        // All 32 simulated services contend for the same image budget.
        p::ImageBudget b{};
        constexpr int kWorkers = 32;
        std::atomic<uint32_t> totalReads{0u};
        std::atomic<uint32_t> snapshots{0u};
        std::vector<std::thread> threads;
        for (int i = 0; i < kWorkers; ++i) {
            threads.emplace_back([&]() {
                p::State s{};
                const auto c = validConfig();
                assert(p::configure(s,c) == p::Status::Armed);
                const auto r = p::sample(
                    s,b,1u,true,true,c.bar5MappedBytes,1u,1u,363112u,
                    [&](uint32_t off) {
                        assert(off == 363016u || off == 363080u);
                        totalReads.fetch_add(1u, std::memory_order_relaxed);
                        return 1u;
                    });
                if (r.readPerformed)
                    snapshots.fetch_add(1u, std::memory_order_relaxed);
                neverAttribute(r);
            });
        }
        for (auto &t : threads) t.join();
        assert(snapshots.load() == 2u);
        assert(totalReads.load() == 4u);
        assert(p::claimedReads(b) == 4u);
    }
    return 0;
}
