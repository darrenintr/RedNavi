#include "navi33_smu_lifetime_latch.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

namespace life = rn_navi33::smu_lifetime_latch;

int main() {
    // A Kext-scope latch exists independently of individual service objects.
    life::AttemptLatch global{};
    assert(!life::isClaimed(global));

    constexpr unsigned kThreads = 32;
    std::atomic<bool> go{false};
    std::atomic<unsigned> winners{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (unsigned n = 0u; n < kThreads; ++n) {
        threads.emplace_back([&]() {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (life::claimOnce(global))
                winners.fetch_add(1u, std::memory_order_relaxed);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto &thread : threads) thread.join();
    assert(winners.load() == 1u);
    assert(life::isClaimed(global));

    // A simulated fresh RedNaviProbe still sees the consumed Kext slot.
    for (unsigned i = 0; i < kThreads; ++i)
        assert(!life::claimOnce(global));
    assert(life::isClaimed(global));

    // This separate latch models a new Kext image. It is intentionally
    // NOT persisted or shared across Kext unload/reload or reboots.
    life::AttemptLatch separateImage{};
    assert(!life::isClaimed(separateImage));
    assert(life::claimOnce(separateImage));
    assert(life::isClaimed(separateImage));
}
