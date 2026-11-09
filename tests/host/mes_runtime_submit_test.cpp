// SPDX-License-Identifier: BSD-3-Clause
// Compile the actual driver in this translation unit so failures can be
// injected into its private queue state without adding test hooks to the kext.
// This tests software error propagation; it does not emulate GPU execution.
#include <cassert>
#include <cstdio>
#include <vector>
#include "../../RedNavi/gfx11.cpp"

static unsigned reads, writes, delays;
static bool poisonOnDelay;
extern "C" {
IOLock *IOLockAlloc(void) { return nullptr; }
void IOLockFree(IOLock *) {}
void IOLockLock(IOLock *) {}
void IOLockUnlock(IOLock *) {}
void IOLog(const char *, ...) {}
void rn_delay_us(unsigned long) { ++delays; if (poisonOnDelay) gMesMemoryError = rn_mes::Error::OutsideWorkspace; }
void rn_sleep_ms(unsigned long) { ++delays; if (poisonOnDelay) gMesMemoryError = rn_mes::Error::OutsideWorkspace; }
u32 rn_rreg32(rn_device *, u32) { ++reads; return 0; }
void rn_wreg32(rn_device *, u32, u32) { ++writes; }
u32 rn_vram_rd32(rn_device *, u64) { assert(false); return 0; }
void rn_vram_wr32(rn_device *, u64, u32) { assert(false); }
}

int main() {
    std::vector<uint32_t> memory(rn_mes::kWorkspaceBytes / 4);
    std::vector<uint64_t> doorbells(4096);
    rn_device device{};
    gMesMemory = rn_mes::Workspace::host(rn_mes::GpuVa(0x2000100000ull), memory.data(), rn_mes::kWorkspaceBytes);
    gGfxBlk = rn_mes::kGraphicsBlock;
    rn_set_doorbell(doorbells.data(), 6);
    assert(rn_gfx_submit_ib(&device, 0x2000200000ull, 16, 0x2000201000ull, 1, 2) == 0);
    assert(gGfxWptr == 24 && doorbells[kGfxDoorbell / 2] == 24 && gGfxLastVmid == 2);
    const auto previous = memory;
    const auto previousDoorbells = doorbells;
    auto gfxSubmit = [&]() { return rn_gfx_submit_ib(&device, 0x2000200000ull, 16, 0x2000201000ull, 1, 3); };
    rn_set_doorbell(nullptr, 6);
    assert(gfxSubmit() == -ENODEV);
    rn_set_doorbell(doorbells.data(), 6);
    gMesMemoryError = rn_mes::Error::OutsideWorkspace;
    assert(gfxSubmit() == -EIO);
    assert(gGfxWptr == 24 && gGfxLastVmid == 2 && memory == previous && doorbells == previousDoorbells);
    // Fail a read after entering the ring-space wait: return EIO after one
    // delay instead of spending 200000 polls and returning EAGAIN.
    gMesMemoryError = rn_mes::Error::Ok;
    gGfxWptr = kGfxRingBytes / 4 - 1;
    poisonOnDelay = true;
    delays = 0;
    assert(gfxSubmit() == -EIO && delays == 1);
    assert(gGfxWptr == kGfxRingBytes / 4 - 1 && gGfxLastVmid == 2 && doorbells == previousDoorbells);
    // A structurally valid allocation with an invalid runtime queue offset
    // causes the first backing read to fail, without a delay or doorbell.
    poisonOnDelay = false;
    gMesMemoryError = rn_mes::Error::Ok;
    gGfxBlk = rn_mes::kWorkspaceBytes;
    delays = 0;
    assert(gfxSubmit() == -EIO && delays == 0 && doorbells == previousDoorbells);
    gGfxBlk = rn_mes::kGraphicsBlock;
    // The emitter can fail after a valid rptr read. Its result must reach
    // the public submit even when the ring had room.
    gMesMemoryError = rn_mes::Error::Ok;
    gGfxBlk = rn_mes::kWorkspaceBytes - 0x2000;
    gGfxWptr = 0;
    assert(gfxSubmit() == -EIO && gGfxWptr == 0 && gGfxLastVmid == 2 && doorbells == previousDoorbells);
    gMapped = true;
    gMappedUsesMes = true;
    gMappedRingAt = rn_mes::kRingAt;
    gMappedRingBytes = rn_mes::kComputeRingBytes;
    gMappedWptr = gMappedRingBytes / 4 - 1;
    gMesMemoryError = rn_mes::Error::Ok;
    poisonOnDelay = true;
    delays = 0;
    assert(rn_compute_submit_ib(&device, 0, rn_mes::kComputeBlock, 4096, 0, 16, 0, 1, 0) == -EIO);
    assert(delays == 1 && gMappedWptr == gMappedRingBytes / 4 - 1 && doorbells == previousDoorbells);
    rn_set_doorbell(nullptr, 6);
    gMesMemoryError = rn_mes::Error::Ok;
    assert(rn_compute_submit_ib(&device, 0, rn_mes::kComputeBlock, 4096, 0, 16, 0, 1, 0) == -ENODEV);
    // Late frames reject a bad scheduler queue before waiting, even if the
    // old fence and status would otherwise look completed.
    rn_set_doorbell(doorbells.data(), 6);
    poisonOnDelay = false;
    gMesMemoryError = rn_mes::Error::Ok;
    gMesCtx = {};
    gMesCtx.valid = true;
    gMesCtx.schedOff = rn_mes::kWorkspaceBytes;
    gMesCtx.apiStatusOff = rn_mes::kSharedBlock + 0x100;
    gMesCtx.ringFenceOff = rn_mes::kSharedBlock + 0x20;
    memory[gMesCtx.ringFenceOff / 4] = 0xFFFF;
    u32 status = 0xFFFF;
    union MESAPI__ADD_QUEUE frame{};
    delays = 0;
    assert(!mesSendLate(&device, &frame, offsetof(union MESAPI__ADD_QUEUE, api_status) / 4, &status));
    assert(status == 0 && delays == 0 && gMesCtx.schedWptr == 0 && doorbells == previousDoorbells);
    std::puts("Actual runtime: submit errors, no false success/kick, immediate poll abort, late-frame rejection PASS");
}
