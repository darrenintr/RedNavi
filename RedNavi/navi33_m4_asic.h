#ifndef REDNAVI_NAVI33_M4_ASIC_H
#define REDNAVI_NAVI33_M4_ASIC_H
// SPDX-License-Identifier: BSD-3-Clause
// Shared M4 ASIC-profile selection, not a second GPU driver.
// No IOKit, GPU writes or queue initialization. 7800 XT remains unchanged.
// The live Navi33 path must not call M4 until real PSP/GMC/BO/MES proofs.
#include <stddef.h>
#include <stdint.h>
#include "navi33_address_spaces.h"
#include "navi33_linux_boot_parity.h"
#include "navi33_runtime.h"

namespace rn_navi33 { namespace m4 {

enum class Family : uint8_t { Unknown, Navi32, Navi33 };
enum class Gfxhub : uint8_t { Unknown, V3_0_3, V3_0 };
enum class Mmhub : uint8_t { Unknown, V3_0, V3_0_2 };
enum class Error : uint8_t {
    Ok, UnsupportedAsic, BadIpVersion, MissingGeometry,
    MissingFirmwareProof, MissingMemoryOwner, MissingVmMapping,
    InconsistentAddress, OverlappingBuffers, MissingQueueResources,
    InvalidDoorbell
};
struct Profile {
    Error error = Error::UnsupportedAsic;
    Family family = Family::Unknown;
    Gfxhub gfxhub = Gfxhub::Unknown;
    Mmhub mmhub = Mmhub::Unknown;
    uint16_t pciDevice = 0;
    uint8_t gcMajor = 0, gcMinor = 0, gcRevision = 0;
    uint8_t pspMajor = 0, pspMinor = 0, pspRevision = 0;
    uint32_t gfxhubL2Cntl3Default = 0;
    uint32_t mesKiqDoorbellDword = 0x18u;
    uint32_t mesSchedulerDoorbellDword = 0x16u;
    uint32_t gfxRingDoorbellDword = 0x116u;
    bool gpuWritesEnabled = false;
    bool valid() const { return error == Error::Ok; }
};
inline Profile select(uint16_t vendor, uint16_t device,
                      uint8_t gcMajor, uint8_t gcMinor, uint8_t gcRev,
                      uint8_t mmMajor, uint8_t mmMinor, uint8_t mmRev,
                      uint8_t pspMajor, uint8_t pspMinor, uint8_t pspRev) {
    Profile p{};
    if (vendor != 0x1002u) return p;
    if (device == 0x747eu) {
        // Verified M4 RX7800XT path. Matching register version does not
        // certify any particular firmware image was successfully loaded.
        if (gcMajor != 11 || gcMinor != 0 || gcRev != 3 ||
            mmMajor != 3 || mmMinor != 0 || mmRev != 0 ||
            pspMajor != 13 || pspMinor != 0 || pspRev != 10) {
            p.error=Error::BadIpVersion; return p;
        }
        p.family=Family::Navi32; p.gfxhub=Gfxhub::V3_0_3;
        p.mmhub=Mmhub::V3_0;
        p.gfxhubL2Cntl3Default=0x80100007u;
    } else if (device == 0x7480u) {
        if (gcMajor != 11 || gcMinor != 0 || gcRev != 2 ||
            mmMajor != 3 || mmMinor != 0 || mmRev != 2 ||
            pspMajor != 13 || pspMinor != 0 || pspRev != 7) {
            p.error=Error::BadIpVersion; return p;
        }
        // Linux gmc_v11_0_set_gfxhub_funcs and set_mmhub_funcs:
        // GC11.0.2 uses gfxhub_v3_0, MMHUB3.0.2 uses mmhub_v3_0_2.
        p.family=Family::Navi33; p.gfxhub=Gfxhub::V3_0;
        p.mmhub=Mmhub::V3_0_2;
        p.gfxhubL2Cntl3Default=0x80120007u;
    } else return p;
    p.pciDevice=device; p.gcMajor=gcMajor; p.gcMinor=gcMinor;
    p.gcRevision=gcRev; p.pspMajor=pspMajor;
    p.pspMinor=pspMinor; p.pspRevision=pspRev;
    p.error=Error::Ok;
    return p;
}
inline Profile forNavi33(const RuntimeReport &r) {
    if (!r.eligibleForComputeBringup()) return {};
    return select(r.observed.vendor,r.observed.device,
                  r.observed.gc.major,r.observed.gc.minor,r.observed.gc.revision,
                  r.observed.mmhub.major,r.observed.mmhub.minor,r.observed.mmhub.revision,
                  r.observed.psp.major,r.observed.psp.minor,r.observed.psp.revision);
}

// A future M4 adapter receives strong-typed GPU virtual addresses only.
// These are NOT VRAM offsets or MC addresses; they must be produced by a
// verified GART/PTE mapping in navi33_address_spaces.h.
struct MesQueues {
    address::GpuVa kiqRing{};
    address::GpuVa kiqMqd{};
    address::GpuVa kiqRptr{};
    address::GpuVa kiqWptr{};
    address::GpuVa schedRing{};
    address::GpuVa schedMqd{};
    address::GpuVa schedRptr{};
    address::GpuVa schedWptr{};
    address::GpuVa schedulerStatus{};
    address::GpuVa gfxRing{};
    address::GpuVa gfxMqd{};
    address::GpuVa gfxFence{};
    bool queuesOwned = false;
    bool gfxhubVmid0Live = false;
    bool mmhubVmid0Live = false;
    bool gpuVmInvalidationAck = false;
    bool doorbellBarRetained = false;
};
struct MesPreflight {
    Error error = Error::MissingQueueResources;
    bool canProgramQueue = false; // ALWAYS false until real backend ownership
};
inline bool within48(address::GpuVa addr, uint64_t bytes, uint64_t align) {
    return address::alignOk(addr.value,align) &&
           address::validSpan(addr.value,bytes,address::kAddressMax48);
}
inline MesPreflight inspectMesQueues(const Profile& p,
                                    const address::Geometry& geometry,
                                    const MesQueues& q,
                                    uint64_t doorbellBarBytes) {
    MesPreflight out{};
    if (!p.valid() || p.family!=Family::Navi33)
        { out.error=Error::UnsupportedAsic; return out; }
    if (!geometry.verified) { out.error=Error::MissingGeometry; return out; }
    if (!q.queuesOwned) { out.error=Error::MissingMemoryOwner; return out; }
    if (!q.gfxhubVmid0Live || !q.mmhubVmid0Live || !q.gpuVmInvalidationAck)
        { out.error=Error::MissingVmMapping; return out; }
    if (!q.doorbellBarRetained ||
        !address::doorbellBarOffset(p.mesKiqDoorbellDword,doorbellBarBytes).ok() ||
        !address::doorbellBarOffset(p.mesSchedulerDoorbellDword,doorbellBarBytes).ok() ||
        !address::doorbellBarOffset(p.gfxRingDoorbellDword,doorbellBarBytes).ok())
        { out.error=Error::InvalidDoorbell; return out; }
    const address::GpuVa addr[] = {
        q.kiqRing,q.kiqMqd,q.kiqRptr,q.kiqWptr,q.schedRing,q.schedMqd,
        q.schedRptr,q.schedWptr,q.schedulerStatus,q.gfxRing,q.gfxMqd,q.gfxFence
    };
    // Minimum reserved regions. The real BO owner must provide the full
    // verified, non-aliasing extents before a hardware writer can be enabled.
    const uint64_t lengths[] = {
        0x1000,0x1000,8,8,0x1000,0x1000,8,8,8,0x1000,0x1000,8
    };
    const uint64_t align[] = {
        256,4096,8,8,256,4096,8,8,8,256,4096,8
    };
    constexpr size_t n=sizeof(addr)/sizeof(addr[0]);
    for(size_t i=0;i<n;i++) {
        if (!within48(addr[i],lengths[i],align[i]))
            { out.error=Error::InconsistentAddress; return out; }
        for(size_t j=0;j<i;j++) {
            if(addr[i].value < addr[j].value+lengths[j] &&
               addr[j].value < addr[i].value+lengths[i])
                { out.error=Error::OverlappingBuffers; return out; }
        }
    }
    out.error=Error::Ok;
    // A synthetic owner assertion and passing host validation do NOT prove
    // that the GPU can fetch any queue. Hardware submission is never armed
    // by this planning component.
    return out;
}

}} // rn_navi33::m4
#endif
