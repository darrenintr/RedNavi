// SPDX-License-Identifier: BSD-3-Clause
// Host-only synthetic fixtures. Nothing communicates with a GPU.
#include "navi33_stage_gate.h"
#include "navi33_topology_wire.h"
#include <cassert>
#include <cstdio>
using namespace rn_navi33::lab;

static void test_stage_gates() {
    static_assert(!kHardwareWritesEnabled, "GPU writes must remain disabled");
    Evidence e{};
    auto r = assess(e);
    assert(r.highest == Stage::None && r.blockedBy == Missing::Discovery);
    assert(!r.authorizesWrites);
    e.gpuWrittenMarker = e.gpuFenceDone = e.computeReadback = true;
    assert(assess(e).highest == Stage::None);
    // Restore baseline after proving that later-stage signals cannot bypass P4/P5.
    e.gpuWrittenMarker = e.gpuFenceDone = e.computeReadback = false;
    e.ipDiscovery = e.p5Memory = e.p6PspPreflight = true;
    assert(assess(e).highest == Stage::Discovery);
    e.signedFirmwareHashes = e.pspResponse = true;
    assert(assess(e).highest == Stage::Discovery); // SMU response required
    e.smuResponse = true;
    assert(assess(e).highest == Stage::Firmware);
    e.exclusiveVramOwner = e.pageTableReadback = true;
    assert(assess(e).highest == Stage::Firmware); // address-domain evidence missing
    e.vaDomainsSeparated = true;
    assert(assess(e).highest == Stage::GpuVm);
    e.sdmaGpuFence = true;
    assert(assess(e).highest == Stage::Sdma);
    e.mesFirmwareAck = e.mesMapQueuesAck = true;
    assert(assess(e).highest == Stage::Sdma);
    e.gfxQueueMappedReadback = true;
    assert(assess(e).highest == Stage::Mes);
    e.gpuWrittenMarker = e.computeReadback = true;
    e.gpuFenceDone = false;
    assert(assess(e).highest == Stage::Mes);
    e.gpuFenceDone = true;
    assert(assess(e).highest == Stage::Compute);
    e.capturedTopologyValidated = e.radvKernelPassed = true;
    assert(assess(e).highest == Stage::Radv);
    e.ioaccelCompiledLayoutVerified = e.windowServerGpuFrame = true;
    r = assess(e);
    assert(r.highest == Stage::Metal && r.blockedBy == Missing::None);
    assert(!r.authorizesWrites);
}

static rn_navi33_topology_wire make_synthetic() {
    rn_navi33_topology_wire t{};
    t.version = RN_NAVI33_TOPOLOGY_VERSION;
    t.flags = RN_NAVI33_TOPOLOGY_CAPTURED;
    t.vendor = 0x1002u;
    t.device = 0x7480u;
    t.revision = 0xc0u; // synthetic; NOT a verified RX 7600 revision
    t.num_shader_engines = 1u;
    t.num_shader_arrays_per_engine = 2u;
    t.num_cu_per_sh = 8u;
    t.num_rb_pipes = 4u;
    t.enabled_rb_pipes_mask = 0xfu;
    t.cu_bitmap[0][0] = 0xffu;
    t.cu_bitmap[0][1] = 0xffu;
    t.cu_active_number = 16u;
    return t;
}

static void test_topology() {
    auto t = make_synthetic();
    assert(sizeof(t) == RN_NAVI33_TOPOLOGY_BYTES);
    assert(rn_navi33_topology_valid(&t, sizeof(t)));
    assert(!rn_navi33_topology_radv_ready(&t, sizeof(t), 1));
    t.flags |= RN_NAVI33_TOPOLOGY_RUNTIME_READY;
    assert(!rn_navi33_topology_radv_ready(&t, sizeof(t), 0));
    assert(rn_navi33_topology_radv_ready(&t, sizeof(t), 1));
    assert(!rn_navi33_topology_valid(nullptr, sizeof(t)));
    assert(!rn_navi33_topology_valid(&t, sizeof(t)-1));
    t.device = 0x747eu;
    assert(!rn_navi33_topology_valid(&t, sizeof(t)));
    t = make_synthetic(); t.cu_active_number = 15u;
    assert(!rn_navi33_topology_valid(&t, sizeof(t)));
    t = make_synthetic(); t.num_shader_engines = 0u;
    assert(!rn_navi33_topology_valid(&t, sizeof(t)));
    t = make_synthetic(); t.cu_bitmap[0][0] |= 0x100u;
    assert(!rn_navi33_topology_valid(&t, sizeof(t)));
    t = make_synthetic(); t.enabled_rb_pipes_mask = 0x10u;
    assert(!rn_navi33_topology_valid(&t, sizeof(t)));
    t = make_synthetic(); t.reserved[0] = 1u;
    assert(!rn_navi33_topology_valid(&t, sizeof(t)));
    t = make_synthetic(); t.flags = 0u;
    assert(!rn_navi33_topology_valid(&t, sizeof(t)));
    t = make_synthetic(); t.num_cu_per_sh = 32u;
    assert(rn_navi33_topology_valid(&t, sizeof(t)));
}

int main() {
    test_stage_gates();
    test_topology();
    std::puts("Navi33 cross-driver host tests PASS (synthetic, no GPU I/O)");
}
