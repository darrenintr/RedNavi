#include "navi33_p736_mp1_response_sampler.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

namespace p = rn_navi33::p736_response_sampler;

static p::Config validConfig() {
    p::Config x{};
    x.explicitOptIn = true;
    x.exactNavi33 = true;
    x.mp1Is13_0_7 = true;
    x.directBar5MappingReady = true;
    x.mp1BaseDword = 90112u;
    x.bar5MappedBytes = 1048576ull;
    return x;
}
static void neverSubmit(const p::Report &r) {
    assert(!r.mailboxExclusiveProven);
    assert(!r.handshakeVerified);
    assert(!r.mayIssueSmuCommand);
}

int main() {
    {
        p::State s{};
        p::KextBudget budget{};
        auto x = validConfig();
        x.explicitOptIn = false;
        assert(p::configure(s, x) == p::Error::Disabled);
        int reads = 0;
        auto r = p::sample(s, budget, true, true, x.bar5MappedBytes,
                           [&](uint32_t) { ++reads; return 1u; });
        assert(r.error == p::Error::Disabled);
        assert(reads == 0 && p::readsClaimed(budget) == 0u);
        neverSubmit(r);
    }
    {
        auto x = validConfig();
        p::State s{};
        x.exactNavi33 = false;
        assert(p::configure(s, x) == p::Error::WrongAsic);
        x = validConfig(); x.mp1Is13_0_7 = false;
        assert(p::configure(s, x) == p::Error::WrongMp1Revision);
        x = validConfig(); x.directBar5MappingReady = false;
        assert(p::configure(s, x) == p::Error::MissingBarMapping);
        x = validConfig(); x.mp1BaseDword = UINT32_MAX;
        assert(p::configure(s, x) == p::Error::BadDirectRegisterAddress);
        x = validConfig(); x.bar5MappedBytes = 363112ull;
        assert(p::configure(s, x) == p::Error::BadDirectRegisterAddress);
        x = validConfig(); x.mp1BaseDword = 0u;
        assert(p::configure(s, x) == p::Error::BadDirectRegisterAddress);
    }
    {
        p::State s{};
        p::KextBudget budget{};
        const auto config = validConfig();
        assert(p::configure(s, config) == p::Error::Armed);
        assert(s.responseByteOffset == 363112u);
        int reads = 0;
        auto reader = [&](uint32_t off) {
            assert(off == 363112u);
            ++reads;
            return reads == 1 ? 1u : 0u;
        };
        auto inactive = p::sample(s, budget, false, true,
                                  config.bar5MappedBytes, reader);
        assert(inactive.error == p::Error::InactiveService);
        assert(!inactive.readPerformed);
        auto lost = p::sample(s, budget, true, false,
                              config.bar5MappedBytes, reader);
        assert(lost.error == p::Error::MappingLost);
        auto shortBar = p::sample(s, budget, true, true, 363112u, reader);
        assert(shortBar.error == p::Error::MappingLost);
        assert(reads == 0 && p::readsClaimed(budget) == 0u);

        auto a = p::sample(s, budget, true, true,
                           config.bar5MappedBytes, reader);
        assert(a.readPerformed && a.response == 1u);
        assert(a.globalSlotConsumed);
        assert(!a.previousValueKnown && !a.newValueDiffers);
        assert(a.sampleCount == 1u);
        neverSubmit(a);

        auto b = p::sample(s, budget, true, true,
                           config.bar5MappedBytes, reader);
        assert(b.readPerformed && b.response == 0u);
        assert(b.previousValueKnown && b.newValueDiffers);
        assert(b.sampleCount == 2u);
        neverSubmit(b);

        auto c = p::sample(s, budget, true, true,
                           config.bar5MappedBytes, reader);
        assert(c.error == p::Error::ServiceReadBudgetExhausted);
        assert(!c.readPerformed && !c.globalSlotConsumed);
        assert(reads == 2 && p::readsClaimed(budget) == 2u);
        p::stop(s);
        auto stopped = p::sample(s, budget, true, true,
                                 config.bar5MappedBytes, reader);
        assert(stopped.error == p::Error::Stopped);
        assert(reads == 2 && p::readsClaimed(budget) == 2u);
        neverSubmit(stopped);
    }
    {
        // Shared image cap survives multiple services; local reconfiguration
        // or a service stop cannot replenish it.
        p::KextBudget budget{};
        auto x = validConfig();
        p::State first{}, second{}, reprobe{};
        assert(p::configure(first, x) == p::Error::Armed);
        assert(p::configure(second, x) == p::Error::Armed);
        int reads = 0;
        auto fake = [&](uint32_t) { ++reads; return 0xffffffffu; };
        const auto a = p::sample(first, budget, true, true, x.bar5MappedBytes, fake);
        const auto b = p::sample(second, budget, true, true, x.bar5MappedBytes, fake);
        assert(a.readPerformed && b.readPerformed);
        // A response of 0xffffffff is a *value*, not a verified reply.
        neverSubmit(a); neverSubmit(b);
        p::stop(first);
        p::stop(second);
        assert(p::configure(reprobe, x) == p::Error::Armed);
        const auto after = p::sample(reprobe, budget, true, true,
                                      x.bar5MappedBytes, fake);
        assert(after.error == p::Error::KextReadBudgetExhausted);
        assert(!after.readPerformed && reads == 2);
        neverSubmit(after);
    }
    {
        // Concurrent services cannot cross the two-read image cap.
        p::KextBudget budget{};
        constexpr int kWorkers = 32;
        std::atomic<uint32_t> reads{0u};
        std::atomic<uint32_t> permitted{0u};
        std::vector<std::thread> threads;
        threads.reserve(kWorkers);
        for (int i = 0; i < kWorkers; ++i) {
            threads.emplace_back([&] {
                p::State s{};
                const auto x = validConfig();
                assert(p::configure(s, x) == p::Error::Armed);
                const auto r = p::sample(
                    s, budget, true, true, x.bar5MappedBytes,
                    [&](uint32_t byteOffset) {
                        assert(byteOffset == 363112u);
                        reads.fetch_add(1u, std::memory_order_relaxed);
                        return 1u;
                    });
                if (r.readPerformed)
                    permitted.fetch_add(1u, std::memory_order_relaxed);
                neverSubmit(r);
            });
        }
        for (auto &t : threads) t.join();
        assert(reads.load() == 2u);
        assert(permitted.load() == 2u);
        assert(p::readsClaimed(budget) == 2u);
    }
    return 0;
}
