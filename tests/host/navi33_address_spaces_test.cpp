// SPDX-License-Identifier: BSD-3-Clause
// All tests are offline: no device, MMIO, firmware or GPU queues.
#include "navi33_address_spaces.h"
#include <assert.h>
#include <stdint.h>
#include <type_traits>

using namespace rn_navi33;
using namespace rn_navi33::address;
static_assert(!std::is_convertible<VramOffset, GpuMc>::value, "VRAM != MC");
static_assert(!std::is_convertible<GpuMc, GpuVa>::value, "MC != VA");
static_assert(!std::is_convertible<CpuKva, GpuVa>::value, "CPU KVA != GPU VA");
static_assert(!std::is_convertible<CpuPhysical, DmaIova>::value, "CPU physical != IOVA");
static_assert(!std::is_convertible<DmaIova, GpuPte>::value, "IOVA != VRAM PTE");
static_assert(!std::is_convertible<GpuPte, GpuMc>::value, "PTE offset != MC");
static_assert(!std::is_convertible<BarByte, VramOffset>::value, "BAR != VRAM");
static_assert(!std::is_convertible<uint64_t, GpuMc>::value, "no implicit integer MC");
static_assert(!std::is_convertible<GpuMc, uint64_t>::value, "no implicit MC integer");

static void geometryTests() {
    const uint32_t fbBase=0x8000u;
    const uint32_t fbTop=fbBase+511u-1u; // 8176MiB (511 * 16MiB)
    MemoryWindow w=makeMemoryWindow(fbBase, fbTop, 0u, 8192u, 8ull<<30);
    assert(w.valid());
    assert(w.fbStart == 0x8000000000ull);
    assert(w.fbBytes == 8176ull*(1u<<20));
    auto g=makeGeometry(w,0,true);
    assert(g.ok());
    assert(g.geometry.mcStart.value == w.fbStart);
    assert(g.geometry.pteBase.value == 0); // FB_OFFSET != FB_LOCATION
    auto m=toMc(g.geometry,VramOffset(0x123000),0x1000,0x1000);
    assert(m.ok() && m.address.value==w.fbStart+0x123000);
    auto p=toPtePhysical(g.geometry,VramOffset(0x123000),0x1000);
    assert(p.ok() && p.address.value==0x123000);
    assert(m.address.value!=p.address.value);
    assert(toMc(g.geometry,VramOffset(8176ull<<20),1).error==Error::OutsideVram);
    assert(toMc(g.geometry,VramOffset(0x123001),4096,4096).error==Error::BadAlignment);
    assert(toPtePhysical(g.geometry,VramOffset(0x123000),0).error==Error::OutsideVram);
    assert(makeGeometry(w,1,true).error==Error::NoMemoryProof);
    assert(makeGeometry(w,0,false).error==Error::NoMemoryProof);

    CpuVramAperture a{};
    a.cpuBase=CpuKva(0xffff000000000000ull);
    a.mappedBytes=256ull<<20;
    a.independentlyVerified=true;
    auto cpu=vramToCpu(g.geometry,a,VramOffset(0x123000),4096);
    assert(cpu.ok() && cpu.address.value==a.cpuBase.value+0x123000);
    assert(vramToCpu(g.geometry,a,VramOffset(256ull<<20),4096).error==Error::OutsideBar);
    a.bar0VramStart=VramOffset(64ull<<20);
    assert(vramToCpu(g.geometry,a,VramOffset(0x123000),4096).error==Error::OutsideBar);
    a.independentlyVerified=false;
    assert(vramToCpu(g.geometry,a,VramOffset(64ull<<20),4096).error==Error::CpuWindowUnverified);

    VramVmMapping vm{};
    vm.vmid=8;
    vm.vaStart=GpuVa(0x30000000ull);
    vm.backing=VramOffset(0x100000);
    vm.bytes=0x20000;
    vm.externallyOwned=true;
    vm.pageTablesLive=true;
    vm.tlbInvalidationAck=true;
    auto va=toGpuVa(g.geometry,VramOffset(0x101000),4096,8,vm);
    assert(va.ok() && va.address.value==0x30001000ull);
    assert(toGpuVa(g.geometry,VramOffset(0x101000),4096,1,vm).error==Error::WrongVmid);
    assert(toGpuVa(g.geometry,VramOffset(0x130000),4096,8,vm).error==Error::OutsideVram);
    vm.tlbInvalidationAck=false;
    assert(toGpuVa(g.geometry,VramOffset(0x101000),4096,8,vm).error==Error::UnmappedGpuVa);
    vm.tlbInvalidationAck=true;
    vm.externallyOwned=false;
    assert(toGpuVa(g.geometry,VramOffset(0x101000),4096,8,vm).error==Error::UnmappedGpuVa);

    Geometry fake{};
    assert(toMc(fake,VramOffset(0),4096).error==Error::NoMemoryProof);
    assert(toPtePhysical(fake,VramOffset(0),4096).error==Error::NoMemoryProof);
}
static void dmaTests() {
    DmaSegment d{};
    d.bus=DmaIova(0x2000000000ull);
    d.bytes=8192;
    d.memoryWired=true;
    d.dmaMappingConfirmed=true;
    d.ownerRetainsMapping=true;
    auto a=hostDmaAddress(d,4096,4096);
    assert(a.ok() && a.address.value==0x2000001000ull);
    assert(hostDmaAddress(d,4096,8192).error==Error::OutsideBar);
    assert(hostDmaAddress(d,4097,100).error==Error::BadAlignment);
    d.bus=DmaIova(UINT64_MAX);
    assert(hostDmaAddress(d,4096,4096).error==Error::OutsideBar);
    d.bus=DmaIova(0x2000000000ull);
    d.dmaMappingConfirmed=false;
    assert(hostDmaAddress(d,0,4096).error==Error::NoDmaMapping);
}
static void barTests() {
    auto gc=gcRegisterBarOffset(0x1260,0x1688,0x100000);
    assert(gc.ok() && gc.address.value==uint64_t(0x1260+0x1688)*4u);
    assert(gcRegisterBarOffset(0xffffffffu,0xffffffffu,0x100000).error==Error::BadBar);
    assert(gcRegisterBarOffset(0x0,0x1000,0x1000).error==Error::BadBar);
    for(uint32_t index : {0x16u,0x18u,0x116u}) {
        auto db=doorbellBarOffset(index,4096);
        assert(db.ok() && db.address.value==index*4u);
    }
    assert(doorbellBarOffset(0x15u,4096).error==Error::BadBar);
    assert(doorbellBarOffset(0x200u,4096).error==Error::BadBar);
}
int main() { geometryTests(); dmaTests(); barTests(); }
