#ifndef REDNAVI_NAVI33_STAGE_GATE_H
#define REDNAVI_NAVI33_STAGE_GATE_H
// SPDX-License-Identifier: BSD-3-Clause
// RX4Darwin-inspired Navi33 bring-up gates: pure host-side evidence audit.
// NO GPU writes, queue setup, VRAM mapping or firmware load.
#include <stdint.h>
namespace rn_navi33 { namespace lab {
constexpr bool kHardwareWritesEnabled = false;
enum class Stage : uint8_t { None, Discovery, Firmware, GpuVm, Sdma, Mes, Compute, Radv, Metal };
enum class Missing : uint8_t { Discovery, Firmware, GpuVm, Sdma, Mes, Compute, Radv, Metal, None };
struct Evidence {
    bool ipDiscovery = false, p5Memory = false, p6PspPreflight = false;
    bool signedFirmwareHashes = false, pspResponse = false, smuResponse = false;
    bool exclusiveVramOwner = false, pageTableReadback = false, vaDomainsSeparated = false;
    bool sdmaGpuFence = false;
    bool mesFirmwareAck = false, mesMapQueuesAck = false, gfxQueueMappedReadback = false;
    bool gpuWrittenMarker = false, gpuFenceDone = false, computeReadback = false;
    bool capturedTopologyValidated = false, radvKernelPassed = false;
    bool ioaccelCompiledLayoutVerified = false, windowServerGpuFrame = false;
};
struct Report {
    Stage highest = Stage::None;
    Missing blockedBy = Missing::Discovery;
    // Never interpretable as permission to send a GPU command.
    bool authorizesWrites = false;
};
inline Report assess(const Evidence &e) {
    Report r{};
    if (!e.ipDiscovery || !e.p5Memory || !e.p6PspPreflight) return r;
    r.highest = Stage::Discovery; r.blockedBy = Missing::Firmware;
    if (!e.signedFirmwareHashes || !e.pspResponse || !e.smuResponse) return r;
    r.highest = Stage::Firmware; r.blockedBy = Missing::GpuVm;
    if (!e.exclusiveVramOwner || !e.pageTableReadback || !e.vaDomainsSeparated) return r;
    r.highest = Stage::GpuVm; r.blockedBy = Missing::Sdma;
    if (!e.sdmaGpuFence) return r;
    r.highest = Stage::Sdma; r.blockedBy = Missing::Mes;
    if (!e.mesFirmwareAck || !e.mesMapQueuesAck || !e.gfxQueueMappedReadback) return r;
    r.highest = Stage::Mes; r.blockedBy = Missing::Compute;
    if (!e.gpuWrittenMarker || !e.gpuFenceDone || !e.computeReadback) return r;
    r.highest = Stage::Compute; r.blockedBy = Missing::Radv;
    if (!e.capturedTopologyValidated || !e.radvKernelPassed) return r;
    r.highest = Stage::Radv; r.blockedBy = Missing::Metal;
    if (!e.ioaccelCompiledLayoutVerified || !e.windowServerGpuFrame) return r;
    r.highest = Stage::Metal; r.blockedBy = Missing::None;
    return r;
}
}} // namespace rn_navi33::lab
#endif
