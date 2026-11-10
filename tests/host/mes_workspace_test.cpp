// SPDX-License-Identifier: BSD-3-Clause
#include "rn_mes_workspace.h"

#include <cassert>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <vector>

using namespace rn_mes;
static_assert(!std::is_convertible<uint64_t, GpuVa>::value, "GPU VAs are explicit");
static_assert(!std::is_convertible<VramOffset, GpuVa>::value, "VRAM is not GPU VA");
static_assert(!std::is_convertible<GpuVa, VramOffset>::value, "GPU VA is not VRAM");

struct VramRecorder {
    unsigned reads = 0, writes = 0;
    uint64_t offset = 0;
    uint32_t value = 0;
    static uint32_t read(void *context, VramOffset offset) {
        auto &r = *static_cast<VramRecorder *>(context);
        ++r.reads;
        r.offset = offset.value;
        return r.value;
    }
    static void write(void *context, VramOffset offset, uint32_t value) {
        auto &r = *static_cast<VramRecorder *>(context);
        ++r.writes;
        r.offset = offset.value;
        r.value = value;
    }
};

int main() {
    std::vector<uint32_t> memory(kWorkspaceBytes / 4, 0);
    VramRecorder vram;
    // The shipped Navi32 GART window is below its VRAM MC aperture. The old
    // GPUVA-minus-MC "offset" wrapped here; this mapping needs no subtraction.
    const Workspace host = Workspace::host(GpuVa(0x2000100000ull), memory.data(), kWorkspaceBytes);
    assert(host.valid());
    assert(validateLayout(host) == Error::Ok);
    Queue kiq, scheduler, compute, graphics;
    assert(host.queue(kKiqBlock, kKiqRingBytes, &kiq) == Error::Ok);
    assert(host.queue(kSchedulerBlock, kSchedulerRingBytes, &scheduler) == Error::Ok);
    assert(host.queue(kComputeBlock, kComputeRingBytes, &compute) == Error::Ok);
    assert(host.queue(kGraphicsBlock, kGraphicsRingBytes, &graphics) == Error::Ok);
    assert(kiq.ring.gpu.value == 0x2000105000ull);
    assert(scheduler.mqd.gpu.value == 0x2000122000ull);
    assert(compute.ring.gpu.value == 0x2000145000ull);
    assert(compute.wptr.gpu.value == 0x2000141008ull);
    assert(graphics.mqd.gpu.value == 0x200019E000ull);
    assert(graphics.ring.gpu.value == 0x20001A1000ull);
    assert(graphics.ring.offset + graphics.ring.bytes == 0xA9000);
    assert(!overlaps(kiq.ring, scheduler.ring));
    assert(!overlaps(compute.ring, graphics.ring));
    Region log;
    assert(host.region(kEventLog, kEventLogBytes, 4096, &log) == Error::Ok);
    assert(log.offset + log.bytes == 0x94000);
    assert(!overlaps(log, graphics.mqd));

    // Last dword is valid. An invalid host range must never escape to VRAM.
    assert(host.write32(kWorkspaceBytes - 4, 0xA55A1234, VramRecorder::write, &vram) == Error::Ok);
    uint32_t value = 0;
    assert(host.read32(kWorkspaceBytes - 4, &value, VramRecorder::read, &vram) == Error::Ok);
    assert(value == 0xA55A1234 && memory.back() == value);
    const auto before = memory;
    const uint64_t invalid[] = {kWorkspaceBytes, kWorkspaceBytes - 2, 1, UINT64_MAX, UINT64_MAX - 3};
    for (const uint64_t offset : invalid) {
        assert(host.write32(offset, 0xBAD, VramRecorder::write, &vram) != Error::Ok);
        value = 0xDEADBEEF;
        assert(host.read32(offset, &value, VramRecorder::read, &vram) != Error::Ok);
        assert(value == 0xDEADBEEF);
    }
    assert(memory == before && vram.reads == 0 && vram.writes == 0);
    assert(host.queue(UINT64_MAX - 0x1000, kKiqRingBytes, &kiq) != Error::Ok);
    assert(host.queue(kGraphicsBlock, 0x10000, &graphics) != Error::Ok);
    assert(host.queue(kComputeBlock, 3, &compute) == Error::InvalidRingSize);
    assert(host.queue(kComputeBlock + 4, kComputeRingBytes, &compute) == Error::BadAlignment);

    // The old, validated Navi32 VRAM placement has exactly the same GPU
    // fields while CPU accesses use its VRAM offset, never the MC address.
    const Workspace local = Workspace::vram(GpuVa(0x800C400000ull), VramOffset(0x0C400000), kWorkspaceBytes);
    assert(validateLayout(local) == Error::Ok);
    assert(local.queue(kComputeBlock, kComputeRingBytes, &compute) == Error::Ok);
    assert(compute.mqd.gpu.value == 0x800C442000ull);
    assert(compute.ring.gpu.value >> 8 == 0x800C4450ull);
    assert(local.write32(compute.ring.offset, 0xC0310800, VramRecorder::write, &vram) == Error::Ok);
    assert(vram.writes == 1 && vram.offset == 0x0C445000);
    assert(local.read32(compute.ring.offset, &value, VramRecorder::read, &vram) == Error::Ok);
    assert(vram.reads == 1 && value == 0xC0310800);
    assert(local.write32(kWorkspaceBytes, 0, VramRecorder::write, &vram) != Error::Ok);
    assert(vram.writes == 1);

    // Every rejection is checked before the backing callback is reached.
    Workspace bad[] = {
        {},
        Workspace::host(GpuVa(0x2000100000ull), nullptr, kWorkspaceBytes),
        Workspace::host(GpuVa(0x2000100004ull), memory.data(), kWorkspaceBytes),
        Workspace::host(GpuVa(0x2000100000ull), memory.data(), kWorkspaceBytes - 4096),
        Workspace::host(GpuVa((1ull << 48) - 4096), memory.data(), kWorkspaceBytes),
        Workspace::host(GpuVa(0x2000100000ull), reinterpret_cast<void *>(UINTPTR_MAX - 3), kWorkspaceBytes),
        Workspace::vram(GpuVa(0x800C400000ull), VramOffset(UINT64_MAX - 4095), kWorkspaceBytes),
        Workspace::vram(GpuVa(0x800C400000ull), VramOffset(3), kWorkspaceBytes)
    };
    for (const auto &w : bad) {
        assert(!w.valid());
        assert(validateLayout(w) != Error::Ok);
        assert(w.write32(0, 0, VramRecorder::write, &vram) != Error::Ok);
        assert(w.read32(0, &value, VramRecorder::read, &vram) != Error::Ok);
    }
    assert(vram.reads == 1 && vram.writes == 1 && memory == before);

    // The final valid 48-bit GPU byte is accepted; a further page is not.
    Workspace high = Workspace::host(GpuVa((1ull << 48) - kWorkspaceBytes), memory.data(), kWorkspaceBytes);
    assert(high.valid());
    Region last;
    assert(high.region(kWorkspaceBytes - 4, 4, 4, &last) == Error::Ok);
    assert(last.gpu.value == (1ull << 48) - 4);
    high.bytes += 4096;
    assert(high.validate() == Error::AddressOverflow);

    std::puts("MES workspace: address domains, native layout and bounded backing access PASS");
}
