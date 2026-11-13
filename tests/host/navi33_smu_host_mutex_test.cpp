// P7.29: exercise the actual rn_smu13_send() sender with mock MMIO.
// Two threads must never issue overlapping MP1 C2PMSG transactions on
// one rn_device. No real GPU registers or firmware are accessed.
#include <atomic>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
// Bring in C++ standard library types before LinuxKPI swap/min macros.
#include "smu13.h"

namespace {
constexpr u32 kBase = 0x100u;
constexpr u32 kMsg = kBase + mmMP1_SMN_C2PMSG_66;
constexpr u32 kParam = kBase + mmMP1_SMN_C2PMSG_82;
constexpr u32 kResp = kBase + mmMP1_SMN_C2PMSG_90;

struct Fake {
    std::mutex lock;
    u32 response = PPSMC_Result_OK;
    u32 parameter = 0u;
    u32 result = 0x00505600u;
    u32 nextResponse = PPSMC_Result_OK;
    unsigned polls = 0u;
    unsigned active = 0u;
    unsigned maxActive = 0u;
    unsigned messages = 0u;
    unsigned complete = 0u;
} bus;
} // namespace

extern "C" u32 rn_rreg32(struct rn_device *, u32 reg) {
    if (reg == kParam) {
        std::lock_guard<std::mutex> guard(bus.lock);
        return bus.parameter;
    }
    assert(reg == kResp);
    bool delay = false;
    u32 answer = 0;
    {
        std::lock_guard<std::mutex> guard(bus.lock);
        if (bus.active) {
            delay = true;
            if (++bus.polls >= 4) {
                --bus.active;
                bus.response = bus.nextResponse;
                bus.parameter = bus.result;
                ++bus.complete;
            }
        }
        answer = bus.response;
    }
    // Yield *outside* the fake MMIO state lock so a missing driver lock
    // would allow the competing thread's message to overlap.
    if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return answer;
}

extern "C" void rn_wreg32(struct rn_device *, u32 reg, u32 value) {
    std::lock_guard<std::mutex> guard(bus.lock);
    if (reg == kResp) { bus.response = value; return; }
    if (reg == kParam) { bus.parameter = value; return; }
    assert(reg == kMsg && value == PPSMC_MSG_GetSmuVersion);
    ++bus.active;
    if (bus.active > bus.maxActive) bus.maxActive = bus.active;
    ++bus.messages;
    bus.polls = 0;
}

int main() {
    rn_device d{};
    mutex_init(&d.smu_msg_lock);
    d.reg_offset[RN_MP1_HWIP][0][0] = kBase;
    // A second software rn_device can target the same BAR5 (legacy
    // smuProbe does exactly this). Both MUST use one host mutex.
    rn_device legacy{};
    mutex_init(&legacy.smu_msg_lock);
    legacy.reg_offset[RN_MP1_HWIP][0][0] = kBase;
    legacy.smu_msg_lock_override = &d.smu_msg_lock;
    assert(rn_smu_message_mutex(&legacy) == &d.smu_msg_lock);
    assert(rn_smu_message_mutex(&d) == &d.smu_msg_lock);
    // Independent service instance: no override and a different device mutex.
    rn_device other{};
    mutex_init(&other.smu_msg_lock);
    other.reg_offset[RN_MP1_HWIP][0][0] = kBase;
    assert(rn_smu_message_mutex(&other) != rn_smu_message_mutex(&d));
    std::atomic<bool> go{false};
    int rc1 = -1, rc2 = -1, rc3 = -1;
    u32 version1 = 0, version2 = 0, version3 = 0, resp1 = 0, resp2 = 0, resp3 = 0;
    std::thread first([&]() {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        rc1 = rn_smu13_send(&d, PPSMC_MSG_GetSmuVersion, 0,
                            &version1, &resp1, 500000);
    });
    std::thread second([&]() {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        rc2 = rn_smu13_send(&legacy, PPSMC_MSG_GetSmuVersion, 0,
                            &version2, &resp2, 500000);
    });
    std::thread third([&]() {
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        rc3 = rn_smu13_send(&other, PPSMC_MSG_GetSmuVersion, 0,
                            &version3, &resp3, 500000);
    });
    go.store(true, std::memory_order_release);
    first.join();
    second.join();
    third.join();
    assert(rc1 == 0 && rc2 == 0 && rc3 == 0);
    assert(resp1 == PPSMC_Result_OK && resp2 == PPSMC_Result_OK && resp3 == PPSMC_Result_OK);
    assert(version1 == bus.result && version2 == bus.result && version3 == bus.result);
    assert(bus.messages == 3 && bus.complete == 3 && bus.maxActive == 1);
    assert(!mutex_is_locked(rn_smu_host_coordinator()));
    assert(!mutex_is_locked(&d.smu_msg_lock));

    // Missing previous response: time out without issuing any new
    // command; all early return paths must release the mutex.
    {
        std::lock_guard<std::mutex> guard(bus.lock);
        bus.response = 0;
    }
    u32 timedResp = 0xffffffffu;
    const int timedOut = rn_smu13_send(&d, PPSMC_MSG_GetSmuVersion, 0,
                                     nullptr, &timedResp, 1000);
    assert(timedOut != 0 && timedResp == 0);
    assert(bus.messages == 3 && !mutex_is_locked(&d.smu_msg_lock));
    assert(!mutex_is_locked(rn_smu_host_coordinator()));

    // Firmware BUSY is not success; the mutex is still released.
    {
        std::lock_guard<std::mutex> guard(bus.lock);
        bus.response = PPSMC_Result_OK;
        bus.nextResponse = PPSMC_Result_CmdRejectedBusy;
    }
    u32 busyResp = 0;
    const int busy = rn_smu13_send(&d, PPSMC_MSG_GetSmuVersion, 0,
                                   nullptr, &busyResp, 500000);
    assert(busy != 0 && busyResp == PPSMC_Result_CmdRejectedBusy);
    assert(bus.messages == 4 && bus.maxActive == 1);
    assert(!mutex_is_locked(rn_smu_host_coordinator()));
    assert(!mutex_is_locked(&d.smu_msg_lock));
}
