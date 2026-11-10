#ifndef REDNAVI_NAVI33_ADDRESS_SPACES_H
#define REDNAVI_NAVI33_ADDRESS_SPACES_H
// SPDX-License-Identifier: BSD-3-Clause
// GC11.0.2 address-space contract. Does not read/write GPU state.
// Every conversion requires an explicit mapping; the same uint64_t bit
// pattern in different address spaces is NOT interchangeable.
#include <stddef.h>
#include <stdint.h>
#include "navi33_bootstrap.h"

namespace rn_navi33 { namespace address {

template <typename Tag> struct Address {
    uint64_t value;
    constexpr Address() : value(0) {}
    explicit constexpr Address(uint64_t v) : value(v) {}
};
struct VramOffsetTag {};
struct GpuMcTag {};        // FB_LOCATION_BASE + VRAM offset; GPU MC aperture
struct GpuPteTag {};       // FB_OFFSET + VRAM offset; VRAM page physical in PTEs
struct GpuVaTag {};        // translated by GFXHUB/MMHUB + VMID; NOT MC
struct DmaIovaTag {};      // IOMMU/bus-mapped HOST memory, NOT CPU physical
struct CpuPhysicalTag {};
struct CpuKvaTag {};
struct BarByteTag {};
using VramOffset = Address<VramOffsetTag>;
using GpuMc = Address<GpuMcTag>;
using GpuPte = Address<GpuPteTag>;
using GpuVa = Address<GpuVaTag>;
using DmaIova = Address<DmaIovaTag>;
using CpuPhysical = Address<CpuPhysicalTag>;
using CpuKva = Address<CpuKvaTag>;
using BarByte = Address<BarByteTag>;
constexpr uint64_t kAddressMax48 = (1ull << 48u) - 1ull;

enum class Error : uint8_t {
    Ok, NoMemoryProof, InvalidSize, Overflow, OutsideVram,
    CpuWindowUnverified, OutsideBar, BadAlignment, UnmappedGpuVa,
    WrongVmid, UnownedMapping, NoDmaMapping, BadBar
};
template <typename Tag> struct Result {
    Error error = Error::NoMemoryProof;
    Address<Tag> address{};
    bool ok() const { return error == Error::Ok; }
};
inline bool validSpan(uint64_t start, uint64_t bytes, uint64_t limit) {
    return bytes != 0 && start <= limit && bytes <= limit - start;
}
inline bool alignOk(uint64_t v, uint64_t align) {
    return align && !(align & (align - 1u)) && (v & (align - 1u)) == 0;
}

// FB_LOCATION_BASE/TOP describe a GPU MC address range (via MMHUB).
// FB_OFFSET gives the physical base to use for a local VRAM PTE. Linux
// gmc_v11_0_vram_gtt_location obtains vram_base_offset through MMHUB's
// get_mc_fb_offset(); this is NOT interchangeable with FB_LOCATION_BASE.
// Linux uses gfxhub_v3_0 for GC11.0.2, gfxhub_v3_0_3 for GC11.0.3;
// Navi33 MMHUB 3.0.2 requires mmhub_v3_0_2.
struct Geometry {
    GpuMc mcStart{};
    GpuPte pteBase{};
    uint64_t usableVramBytes = 0;
    uint64_t cpuVisibleBytes = 0;
    bool verified = false;
};
struct GeometryResult {
    Error error = Error::NoMemoryProof;
    Geometry geometry{};
    bool ok() const { return error == Error::Ok; }
};
inline GeometryResult makeGeometry(const rn_navi33::MemoryWindow& window,
                                   uint32_t gfxFbOffsetRaw,
                                   bool gfxFbOffsetVerified) {
    GeometryResult out{};
    if (!window.valid() || !gfxFbOffsetVerified ||
        window.fbStart > kAddressMax48 ||
        !validSpan(window.fbStart, window.fbBytes, kAddressMax48) ||
        !window.cpuVisibleBytes || window.cpuVisibleBytes > window.fbBytes)
        return out;
    // Both hubs must agree before one PTE backing address can be used.
    if (gfxFbOffsetRaw != window.fbOffsetRegister)
        return out;
    if (gfxFbOffsetRaw > 0xffffffu ||
        !validSpan(static_cast<uint64_t>(gfxFbOffsetRaw) << 24u,
                   window.fbBytes, kAddressMax48))
        { out.error = Error::Overflow; return out; }
    out.geometry.mcStart = GpuMc(window.fbStart);
    out.geometry.pteBase = GpuPte(static_cast<uint64_t>(gfxFbOffsetRaw) << 24u);
    out.geometry.usableVramBytes = window.fbBytes;
    out.geometry.cpuVisibleBytes = window.cpuVisibleBytes;
    out.geometry.verified = true;
    out.error = Error::Ok;
    return out;
}
inline Result<GpuMcTag> toMc(const Geometry& g, VramOffset off,
                           uint64_t bytes, uint64_t align = 1) {
    if (!g.verified) return {};
    if (!alignOk(off.value, align)) return {Error::BadAlignment, {}};
    if (!validSpan(off.value, bytes, g.usableVramBytes))
        return {Error::OutsideVram, {}};
    return {Error::Ok, GpuMc(g.mcStart.value + off.value)};
}
inline Result<GpuPteTag> toPtePhysical(const Geometry& g, VramOffset off,
                                      uint64_t bytes, uint64_t align = 4096) {
    if (!g.verified) return {};
    if (!alignOk(off.value, align)) return {Error::BadAlignment, {}};
    if (!validSpan(off.value, bytes, g.usableVramBytes))
        return {Error::OutsideVram, {}};
    if (!alignOk(g.pteBase.value, align)) return {Error::BadAlignment, {}};
    return {Error::Ok, GpuPte(g.pteBase.value + off.value)};
}

// A CPU BAR mapping is a CPU virtual pointer, never a GPU address.
// bar0VramStart must be determined from the actual, owned CPU aperture;
// the size of BAR0 alone cannot prove where that aperture begins.
struct CpuVramAperture {
    CpuKva cpuBase{};
    VramOffset bar0VramStart{};
    uint64_t mappedBytes = 0;
    bool independentlyVerified = false;
};
inline Result<CpuKvaTag> vramToCpu(const Geometry& g,
                                  const CpuVramAperture& a,
                                  VramOffset off, uint64_t bytes) {
    if (!g.verified || !a.independentlyVerified || !a.cpuBase.value)
        return {Error::CpuWindowUnverified, {}};
    if (!validSpan(off.value, bytes, g.usableVramBytes) ||
        off.value < a.bar0VramStart.value ||
        !validSpan(off.value - a.bar0VramStart.value, bytes, a.mappedBytes))
        return {Error::OutsideBar, {}};
    const uint64_t d = off.value - a.bar0VramStart.value;
    if (a.cpuBase.value > UINT64_MAX - d) return {Error::Overflow, {}};
    return {Error::Ok, CpuKva(a.cpuBase.value + d)};
}

// A GFXHUB GPU-VA needs an independent VM mapping and completed TLB
// invalidation. VMID0 is special: its aperture translation must also be
// explicitly demonstrated; never infer VMID0 VA from an MC address.
struct VramVmMapping {
    uint32_t vmid = 0;
    GpuVa vaStart{};
    VramOffset backing{};
    uint64_t bytes = 0;
    bool externallyOwned = false;
    bool pageTablesLive = false;
    bool tlbInvalidationAck = false;
};
inline Result<GpuVaTag> toGpuVa(const Geometry& g, VramOffset off,
                               uint64_t bytes, uint32_t vmid,
                               const VramVmMapping& m) {
    if (!g.verified) return {};
    if (!m.externallyOwned || !m.pageTablesLive || !m.tlbInvalidationAck)
        return {Error::UnmappedGpuVa, {}};
    if (m.vmid != vmid) return {Error::WrongVmid, {}};
    if (!validSpan(m.backing.value, m.bytes, g.usableVramBytes) ||
        !validSpan(off.value, bytes, g.usableVramBytes) ||
        off.value < m.backing.value ||
        !validSpan(off.value - m.backing.value, bytes, m.bytes))
        return {Error::OutsideVram, {}};
    const uint64_t d = off.value - m.backing.value;
    if (!validSpan(m.vaStart.value, m.bytes, kAddressMax48) ||
        !validSpan(m.vaStart.value + d, bytes, kAddressMax48))
        return {Error::Overflow, {}};
    return {Error::Ok, GpuVa(m.vaStart.value + d)};
}

// Host RAM: IOKit DMA/IOMMU must supply an actual device-visible IOVA.
// A CPU physical address or CPU pointer NEVER establishes a DMA mapping.
struct DmaSegment {
    DmaIova bus{};
    uint64_t bytes = 0;
    bool memoryWired = false;
    bool dmaMappingConfirmed = false;
    bool ownerRetainsMapping = false;
};
inline Result<DmaIovaTag> hostDmaAddress(const DmaSegment& s,
                                        uint64_t offset, uint64_t bytes,
                                        uint64_t align = 4096) {
    if (!s.memoryWired || !s.dmaMappingConfirmed || !s.ownerRetainsMapping)
        return {Error::NoDmaMapping, {}};
    if (!validSpan(offset, bytes, s.bytes) ||
        !validSpan(s.bus.value, s.bytes, kAddressMax48))
        return {Error::OutsideBar, {}};
    if (!alignOk(s.bus.value + offset, align))
        return {Error::BadAlignment, {}};
    return {Error::Ok, DmaIova(s.bus.value + offset)};
}

// GART-mapped HOST memory is a GPU VA only if its DMA IOVA has a
// programmed, owned GART PTE mapping. No identity mapping is assumed.
struct HostVmMapping {
    uint32_t vmid = 0;
    GpuVa vaStart{};
    DmaIova dmaBacking{};
    uint64_t bytes = 0;
    bool externallyOwned = false;
    bool gartPtesLive = false;
    bool tlbInvalidationAck = false;
};
inline Result<GpuVaTag> hostToGpuVa(const DmaSegment& dma, uint64_t offset,
                                    uint64_t bytes, uint32_t vmid,
                                    const HostVmMapping& vm) {
    if (!dma.memoryWired || !dma.dmaMappingConfirmed ||
        !dma.ownerRetainsMapping) return {Error::NoDmaMapping, {}};
    if (!vm.externallyOwned || !vm.gartPtesLive || !vm.tlbInvalidationAck)
        return {Error::UnmappedGpuVa, {}};
    if (vm.vmid != vmid) return {Error::WrongVmid, {}};
    if (dma.bus.value != vm.dmaBacking.value ||
        !validSpan(vm.dmaBacking.value, vm.bytes, kAddressMax48) ||
        !validSpan(offset, bytes, dma.bytes) ||
        !validSpan(offset, bytes, vm.bytes) ||
        !validSpan(vm.vaStart.value, vm.bytes, kAddressMax48))
        return {Error::OutsideVram, {}};
    return {Error::Ok, GpuVa(vm.vaStart.value + offset)};
}

// MMIO and BAR2 doorbells are BYTE offsets in a CPU BAR mapping. IP
// Discovery and register definitions instead identify DWORD offsets.
inline Result<BarByteTag> gcRegisterBarOffset(uint32_t segmentBaseDword,
                                            uint32_t regDword,
                                            uint64_t bar5Bytes) {
    uint32_t byteOffset = 0;
    if (!rn_navi33::checkedBarDword(segmentBaseDword, regDword,
                                    bar5Bytes, &byteOffset))
        return {Error::BadBar, {}};
    return {Error::Ok, BarByte(byteOffset)};
}
inline Result<BarByteTag> doorbellBarOffset(uint32_t indexDword,
                                           uint64_t bar2Bytes) {
    const uint64_t off = static_cast<uint64_t>(indexDword) * 4u;
    // MES doorbells are written as 64-bit WPTRs.
    if (!alignOk(off, 8u) || !validSpan(off, 8u, bar2Bytes))
        return {Error::BadBar, {}};
    return {Error::Ok, BarByte(off)};
}
}} // rn_navi33::address
#endif
