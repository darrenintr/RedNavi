// Deterministic tests for the Navi 33 offline scheduler plug-in.
#include "navi33_scheduler_plugin.h"
#include <cassert>
#include <cstdio>

using namespace rn_navi33::scheduler;
static_assert(!kHardwareWritesEnabled, "Scheduler model must not perform GPU I/O");

struct FakeBackend {
    Dispatch submitted[128]{};
    size_t count = 0;
    bool busy = false;
    bool fatal = false;
    static SubmitStatus submit(void *p, const Dispatch &d) {
        FakeBackend &f = *static_cast<FakeBackend *>(p);
        if (f.fatal) return SubmitStatus::Fatal;
        if (f.busy) return SubmitStatus::Busy;
        assert(f.count < 128);
        f.submitted[f.count++] = d;
        return SubmitStatus::Accepted;
    }
    Backend plugin() { return {this, submit}; }
};

static void basicOrderingAndDependencies() {
    FakeBackend fake;
    Scheduler s(fake.plugin());
    assert(s.configure(0, QueueType::Compute) == Result::Ok);
    assert(s.configure(1, QueueType::Graphics) == Result::Ok);
    assert(s.configure(0, QueueType::Compute) == Result::AlreadyConfigured);
    assert(s.configure(8, QueueType::Compute) == Result::InvalidQueue);
    auto a = s.enqueue(0, 100, 10);
    auto b = s.enqueue(0, 101, 10);
    auto c = s.enqueue(1, 200, 10, a.fence);
    assert(a.result == Result::Ok && b.result == Result::Ok && c.result == Result::Ok);
    assert(a.fence.sequence == 1 && b.fence.sequence == 2);
    assert(s.pump(100) == Result::Ok);
    assert(fake.count == 1 && fake.submitted[0].commandTag == 100);
    assert(s.status(c.fence) == JobState::Queued);
    assert(s.complete(b.fence, true) == Result::NotInFlight);
    assert(s.complete(a.fence, true) == Result::Ok);
    assert(s.complete(a.fence, true) == Result::NotInFlight);
    assert(s.pump(101) == Result::Ok);
    assert(fake.count == 3);
    assert(fake.submitted[1].commandTag == 101);
    assert(fake.submitted[2].commandTag == 200);
    assert(fake.submitted[2].type == QueueType::Graphics);
    assert(s.complete(c.fence, true) == Result::Ok);
    assert(s.complete(b.fence, true) == Result::Ok);
    assert(s.pump(100) == Result::BadClock);
    assert(s.retire(a.fence) == Result::Ok);
    assert(s.enqueue(1, 201, 20, a.fence).result == Result::BadDependency);
    assert(s.status(a.fence) == JobState::Unknown);
}

static void backpressureTimeoutAndRecovery() {
    FakeBackend fake;
    Scheduler s(fake.plugin());
    assert(s.configure(2, QueueType::Sdma) == Result::Ok);
    auto a = s.enqueue(2, 10, 5);
    auto b = s.enqueue(2, 11, 5);
    fake.busy = true;
    assert(s.pump(1) == Result::Ok && fake.count == 0);
    assert(s.status(a.fence) == JobState::Queued);
    fake.busy = false;
    assert(s.pump(2) == Result::Ok && fake.count == 1);
    assert(s.pump(6) == Result::Ok && !s.queueStatus(2).faulted);
    assert(s.pump(7) == Result::Ok && s.queueStatus(2).faulted);
    assert(s.status(a.fence) == JobState::TimedOut);
    assert(s.status(b.fence) == JobState::Queued);
    assert(s.complete(a.fence, true) == Result::NotInFlight); // stale completion
    assert(s.acknowledgeRecovery(2, false) == Result::RecoveryNotConfirmed);
    assert(s.pump(8) == Result::Ok && fake.count == 1);
    assert(s.acknowledgeRecovery(2, true) == Result::Ok);
    assert(s.queueStatus(2).generation == 1);
    assert(s.pump(9) == Result::Ok && fake.count == 2);
    assert(fake.submitted[1].commandTag == 11 && fake.submitted[1].generation == 1);
    assert(s.complete(b.fence, true) == Result::Ok);
    assert(s.retire(a.fence) == Result::Ok);
    assert(s.retire(b.fence) == Result::Ok);
}

static void failedDependencyAndBackendFatal() {
    FakeBackend fake;
    Scheduler s(fake.plugin());
    assert(s.configure(0, QueueType::Compute) == Result::Ok);
    assert(s.configure(1, QueueType::Graphics) == Result::Ok);
    auto a = s.enqueue(0, 20, 100);
    auto b = s.enqueue(1, 21, 100, a.fence);
    auto c = s.enqueue(1, 22, 100);
    assert(s.retire(a.fence) == Result::NotTerminal);
    assert(s.pump(0) == Result::Ok && fake.count == 1);
    assert(s.complete(a.fence, false) == Result::Ok);
    assert(s.queueStatus(0).faulted);
    assert(s.retire(a.fence) == Result::DependencyInUse);
    assert(s.pump(1) == Result::Ok);
    assert(s.status(b.fence) == JobState::DependencyFailed);
    assert(s.status(c.fence) == JobState::InFlight);
    assert(fake.submitted[1].commandTag == 22);
    assert(s.complete(c.fence, true) == Result::Ok);
    assert(s.retire(a.fence) == Result::Ok);
    assert(s.acknowledgeRecovery(0, true) == Result::Ok);
    fake.fatal = true;
    auto d = s.enqueue(0, 23, 100);
    assert(d.result == Result::Ok);
    assert(s.pump(2) == Result::Ok);
    assert(s.status(d.fence) == JobState::Failed);
    assert(s.queueStatus(0).faulted);
    assert(s.enqueue(0, 24, 100).result == Result::Faulted);
}

static void invalidInputAndBoundedStorage() {
    FakeBackend fake;
    Scheduler s(fake.plugin());
    assert(s.enqueue(0, 1, 1).result == Result::InvalidQueue);
    assert(s.configure(0, QueueType::Compute) == Result::Ok);
    assert(s.enqueue(0, 1, 0).result == Result::BadTimeout);
    assert(s.enqueue(0, 1, 1, {0, 20}).result == Result::BadDependency);
    assert(s.enqueue(0, 1, 1, {0, 0}).result == Result::BadDependency);
    Fence unknown{0, 42};
    assert(s.complete(unknown, true) == Result::InvalidTicket);
    assert(s.retire(unknown) == Result::InvalidTicket);
    Fence all[64]{};
    for (size_t i = 0; i < 64; ++i) {
        auto r = s.enqueue(0, 1000 + i, 100);
        assert(r.result == Result::Ok);
        all[i] = r.fence;
    }
    assert(s.enqueue(0, 999, 100).result == Result::Full);
    assert(s.pump(0) == Result::Ok);
    assert(s.complete(all[0], true) == Result::Ok);
    assert(s.retire(all[0]) == Result::Ok);
    assert(s.enqueue(0, 2000, 100).result == Result::Ok);
}

static void failsClosedWithoutBackend() {
    Scheduler s({});
    assert(s.configure(0, QueueType::Compute) == Result::Ok);
    auto a = s.enqueue(0, 1, 10);
    assert(a.result == Result::Ok);
    assert(s.pump(1) == Result::NoBackend);
    assert(s.status(a.fence) == JobState::Queued);
}

int main() {
    basicOrderingAndDependencies();
    backpressureTimeoutAndRecovery();
    failedDependencyAndBackendFatal();
    invalidInputAndBoundedStorage();
    failsClosedWithoutBackend();
    puts("Navi33 scheduler plugin host tests passed (no GPU I/O)");
}
