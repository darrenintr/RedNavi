// SPDX-License-Identifier: BSD-3-Clause
// Link with RedNavi/gfx11.cpp. Recording I/O stubs check that rejected
// initialization never touches registers, VRAM, CPU backing or doorbells.
// This does not establish hardware readiness or emulate firmware.
#include <cassert>
#include <cstdio>
#include <vector>
#include <IOKit/IOLib.h>
#include "gfx11.h"

static unsigned reads, writes, vramReads, vramWrites;
extern "C" {
IOLock *IOLockAlloc(void) { return nullptr; }
void IOLockFree(IOLock *) {}
void IOLockLock(IOLock *) {}
void IOLockUnlock(IOLock *) {}
void IOLog(const char *, ...) {}
void rn_delay_us(unsigned long) {}
void rn_sleep_ms(unsigned long) {}
u32 rn_rreg32(rn_device *, u32) { ++reads; return 0; }
void rn_wreg32(rn_device *, u32, u32) { ++writes; }
u32 rn_vram_rd32(rn_device *, u64) { ++vramReads; return 0; }
void rn_vram_wr32(rn_device *, u64, u32) { ++vramWrites; }
}

int main() {
    std::vector<uint32_t> memory(rn_mes::kWorkspaceBytes / 4, 0xDEADBEEF);
    std::vector<uint64_t> doorbells(4096, 0xDEADBEEF);
    rn_set_doorbell(doorbells.data(), 6);
    const rn_mes::Workspace workspace = rn_mes::Workspace::host(
        rn_mes::GpuVa(0x2000100000ull), memory.data(), rn_mes::kWorkspaceBytes);
    rn_device device{};
    device.gfxhub_variant = RN_GFXHUB_NAVI32_V3_0_3;
    const u32 starts[4]{};
    rn_mes_result result{};
    auto rejectsWithoutIo = [&](const rn_mes::Workspace &w, u32 step, rn_mes::Error error) {
        const auto previous = memory;
        const auto previousDoorbells = doorbells;
        reads = writes = vramReads = vramWrites = 0;
        rn_mes_start(&device, w, nullptr, starts, step, nullptr, nullptr, &result);
        assert(result.workspace_error == static_cast<u32>(error));
        assert(result.step == 0 && result.add_ok == 0 && result.hw_res_ok == 0);
        assert(reads == 0 && writes == 0 && vramReads == 0 && vramWrites == 0);
        assert(memory == previous && doorbells == previousDoorbells);
    };
    rejectsWithoutIo({}, 5, rn_mes::Error::NoBacking);
    auto invalid = workspace;
    invalid.bytes -= 4096;
    rejectsWithoutIo(invalid, 5, rn_mes::Error::InsufficientBytes);
    invalid = workspace;
    invalid.gpuBase = rn_mes::GpuVa((1ull << 48) - 4096);
    rejectsWithoutIo(invalid, 5, rn_mes::Error::AddressOverflow);
    invalid = workspace;
    invalid.gpuBase = rn_mes::GpuVa(workspace.gpuBase.value + 4);
    rejectsWithoutIo(invalid, 5, rn_mes::Error::BadAlignment);
    rejectsWithoutIo(workspace, 7, rn_mes::Error::UnsupportedProcessPath);
    device.gfxhub_variant = RN_GFXHUB_NAVI33_V3_0;
    rejectsWithoutIo(workspace, 5, rn_mes::Error::UnsupportedAsic);
    device.gfxhub_variant = RN_GFXHUB_NAVI32_V3_0_3;
    rejectsWithoutIo(workspace, 0, rn_mes::Error::Ok);
    rn_mes_start(&device, workspace, nullptr, starts, 1, nullptr, nullptr, &result);
    assert(result.step == 1 && reads && writes && !vramReads && !vramWrites);
    rejectsWithoutIo(workspace, 5, rn_mes::Error::AlreadyConfigured);
    rn_mes_forget_workspace();
    assert(reads == 0 && writes == 0 && vramReads == 0 && vramWrites == 0);
    std::puts("Actual rn_mes_start: invalid resources, Navi33, process path and reuse reject before all I/O PASS");
}
