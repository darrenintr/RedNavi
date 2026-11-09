#ifndef REDNAVI_NAVI33_SCHEDULER_PLUGIN_H
#define REDNAVI_NAVI33_SCHEDULER_PLUGIN_H
// SPDX-License-Identifier: BSD-3-Clause
// Offline, single-threaded software scheduling model for Navi 33.
// No IOKit, MES MMIO, GPU addresses, allocation, or hardware ownership.
// This header is NOT included in the production kext.
#include <stddef.h>
#include <stdint.h>

namespace rn_navi33 { namespace scheduler {

constexpr bool kHardwareWritesEnabled = false;
constexpr uint8_t kInvalidQueue = 0xffu;
constexpr size_t kMaxQueues = 8;
constexpr size_t kMaxJobs = 64;

enum class QueueType : uint8_t { Graphics, Compute, Sdma };
enum class SubmitStatus : uint8_t { Accepted, Busy, Fatal };
enum class Result : uint8_t {
    Ok, InvalidQueue, AlreadyConfigured, BadTimeout, BadDependency,
    Full, Faulted, NoBackend, BadClock, InvalidTicket,
    NotInFlight, RecoveryNotConfirmed, NotTerminal, DependencyInUse
};
enum class JobState : uint8_t {
    Unknown, Queued, InFlight, Succeeded, Failed, TimedOut, DependencyFailed
};

struct Fence {
    uint8_t queue = kInvalidQueue;
    uint64_t sequence = 0;
    bool valid() const { return queue != kInvalidQueue && sequence != 0; }
};
inline bool equal(Fence a, Fence b) {
    return a.queue == b.queue && a.sequence == b.sequence;
}

struct Dispatch {
    Fence signal{};
    uint64_t commandTag = 0; // Opaque identifier, not a GPU virtual address.
    QueueType type = QueueType::Compute;
    uint32_t generation = 0; // Changes only after confirmed external reset.
};
struct Backend {
    void *context = nullptr;
    SubmitStatus (*submit)(void *context, const Dispatch &request) = nullptr;
};
struct EnqueueResult {
    Result result = Result::InvalidQueue;
    Fence fence{};
};
struct QueueStatus {
    bool configured = false;
    bool faulted = false;
    QueueType type = QueueType::Compute;
    uint64_t nextSequence = 1;
    uint32_t generation = 0;
};

class Scheduler {
public:
    explicit Scheduler(Backend backend) : backend_(backend) {}

    Result configure(uint8_t id, QueueType type) {
        if (id >= kMaxQueues) return Result::InvalidQueue;
        if (queues_[id].configured) return Result::AlreadyConfigured;
        queues_[id].configured = true;
        queues_[id].type = type;
        return Result::Ok;
    }

    EnqueueResult enqueue(uint8_t id, uint64_t commandTag,
                          uint32_t timeoutMs, Fence dependency = {}) {
        if (id >= kMaxQueues || !queues_[id].configured)
            return {Result::InvalidQueue, {}};
        Queue &q = queues_[id];
        if (q.faulted) return {Result::Faulted, {}};
        if (!timeoutMs) return {Result::BadTimeout, {}};
        if (dependency.valid()) {
            if (dependency.queue >= kMaxQueues ||
                !queues_[dependency.queue].configured ||
                dependency.sequence >= queues_[dependency.queue].nextSequence ||
                !find(dependency))
                return {Result::BadDependency, {}};
        } else if (dependency.queue != kInvalidQueue || dependency.sequence != 0) {
            return {Result::BadDependency, {}};
        }
        if (q.nextSequence == UINT64_MAX) return {Result::Full, {}};
        Job *slot = nullptr;
        for (auto &job : jobs_) {
            if (job.state == JobState::Unknown) { slot = &job; break; }
        }
        if (!slot) return {Result::Full, {}};
        const Fence fence{id, q.nextSequence++};
        *slot = {fence, dependency, commandTag, timeoutMs, 0, JobState::Queued};
        return {Result::Ok, fence};
    }

    // A deterministic scheduler tick. The caller supplies a monotonic
    // millisecond clock; neither this class nor the backend starts a thread.
    Result pump(uint64_t nowMs) {
        if (timeKnown_ && nowMs < lastMs_) return Result::BadClock;
        lastMs_ = nowMs;
        timeKnown_ = true;
        if (!backend_.submit) return Result::NoBackend;

        for (auto &job : jobs_) {
            if (job.state != JobState::InFlight) continue;
            if (nowMs - job.startedMs >= job.timeoutMs) {
                job.state = JobState::TimedOut;
                queues_[job.fence.queue].faulted = true;
            }
        }
        for (uint8_t id = 0; id < kMaxQueues; ++id) {
            Queue &q = queues_[id];
            if (!q.configured || q.faulted) continue;
            // A queue has at most one in-flight command. There is no
            // presumption that the GPU serializes separate hardware queues.
            if (hasInFlight(id)) continue;
            // Re-evaluate if a dependency is terminally failed; that command
            // never reaches the backend, and following commands may advance.
            for (size_t step = 0; step < kMaxJobs; ++step) {
                Job *job = earliestQueued(id);
                if (!job) break;
                if (job->dependency.valid()) {
                    const Job *parent = find(job->dependency);
                    if (!parent) {
                        job->state = JobState::DependencyFailed;
                        continue; // A retired dependency fails closed.
                    }
                    if (parent->state == JobState::Failed ||
                        parent->state == JobState::TimedOut ||
                        parent->state == JobState::DependencyFailed) {
                        job->state = JobState::DependencyFailed;
                        continue;
                    }
                    if (parent->state != JobState::Succeeded) break;
                }
                const Dispatch dispatch{job->fence, job->tag, q.type, q.generation};
                const SubmitStatus rc = backend_.submit(backend_.context, dispatch);
                if (rc == SubmitStatus::Busy) break;
                if (rc == SubmitStatus::Fatal) {
                    job->state = JobState::Failed;
                    q.faulted = true;
                    break;
                }
                job->state = JobState::InFlight;
                job->startedMs = nowMs;
                break;
            }
        }
        return Result::Ok;
    }

    // Called only for a backend-reported completion. A stale, duplicated,
    // unknown, or out-of-order event never advances the queue.
    Result complete(Fence fence, bool success) {
        Job *job = find(fence);
        if (!job) return Result::InvalidTicket;
        if (job->state != JobState::InFlight) return Result::NotInFlight;
        job->state = success ? JobState::Succeeded : JobState::Failed;
        if (!success) queues_[fence.queue].faulted = true;
        return Result::Ok;
    }

    // Must be called only after an external hardware recovery/reset has
    // been verified. The scheduler itself never touches the GPU or MES.
    Result acknowledgeRecovery(uint8_t id, bool externallyConfirmed) {
        if (id >= kMaxQueues || !queues_[id].configured)
            return Result::InvalidQueue;
        if (!externallyConfirmed) return Result::RecoveryNotConfirmed;
        if (hasInFlight(id)) return Result::NotInFlight;
        if (queues_[id].faulted) {
            queues_[id].faulted = false;
            ++queues_[id].generation;
        }
        return Result::Ok;
    }

    JobState status(Fence fence) const {
        const Job *job = find(fence);
        return job ? job->state : JobState::Unknown;
    }
    QueueStatus queueStatus(uint8_t id) const {
        if (id >= kMaxQueues) return {};
        const Queue &q = queues_[id];
        return {q.configured, q.faulted, q.type, q.nextSequence, q.generation};
    }
    // Frees bounded offline-model history, never a live command. Do not
    // retire a terminal fence while any queued job still depends on it.
    Result retire(Fence fence) {
        Job *job = find(fence);
        if (!job) return Result::InvalidTicket;
        if (!isTerminal(job->state)) return Result::NotTerminal;
        for (const auto &other : jobs_) {
            if (other.state == JobState::Queued && equal(other.dependency, fence))
                return Result::DependencyInUse;
        }
        *job = {};
        return Result::Ok;
    }

private:
    struct Queue {
        bool configured = false;
        bool faulted = false;
        QueueType type = QueueType::Compute;
        uint64_t nextSequence = 1;
        uint32_t generation = 0;
    };
    struct Job {
        Fence fence{};
        Fence dependency{};
        uint64_t tag = 0;
        uint32_t timeoutMs = 0;
        uint64_t startedMs = 0;
        JobState state = JobState::Unknown;
    };
    static bool isTerminal(JobState s) {
        return s == JobState::Succeeded || s == JobState::Failed ||
               s == JobState::TimedOut || s == JobState::DependencyFailed;
    }
    Job *find(Fence f) {
        if (!f.valid()) return nullptr;
        for (auto &job : jobs_)
            if (job.state != JobState::Unknown && equal(job.fence, f)) return &job;
        return nullptr;
    }
    const Job *find(Fence f) const {
        if (!f.valid()) return nullptr;
        for (const auto &job : jobs_)
            if (job.state != JobState::Unknown && equal(job.fence, f)) return &job;
        return nullptr;
    }
    bool hasInFlight(uint8_t id) const {
        for (const auto &job : jobs_)
            if (job.state == JobState::InFlight && job.fence.queue == id) return true;
        return false;
    }
    Job *earliestQueued(uint8_t id) {
        Job *best = nullptr;
        for (auto &job : jobs_) {
            if (job.state != JobState::Queued || job.fence.queue != id) continue;
            if (!best || job.fence.sequence < best->fence.sequence) best = &job;
        }
        return best;
    }

    Backend backend_{};
    Queue queues_[kMaxQueues]{};
    Job jobs_[kMaxJobs]{};
    uint64_t lastMs_ = 0;
    bool timeKnown_ = false;
};

}} // namespace rn_navi33::scheduler
#endif // REDNAVI_NAVI33_SCHEDULER_PLUGIN_H
