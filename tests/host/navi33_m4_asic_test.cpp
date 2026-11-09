// SPDX-License-Identifier: BSD-3-Clause
// M4 ASIC selection and MES buffer admission. Synthetic host-only tests.
#include "navi33_m4_asic.h"
#include <assert.h>

using namespace rn_navi33;
using namespace rn_navi33::m4;
using namespace rn_navi33::address;

static Profile n33() {
    return select(0x1002,0x7480,11,0,2,3,0,2,13,0,7);
}
static Geometry geometry() {
    MemoryWindow w=makeMemoryWindow(0x8000,0x81fe,0,8192,8ull<<30);
    assert(w.valid());
    auto g=makeGeometry(w,0,true);
    assert(g.ok());
    return g.geometry;
}
static MesQueues buffers() {
    const uint64_t base=0x2000000000ull; // illustrative GART VA, NOT owned
    MesQueues q{};
    q.kiqRing=GpuVa(base);
    q.kiqMqd=GpuVa(base+0x10000);
    q.kiqRptr=GpuVa(base+0x11000);
    q.kiqWptr=GpuVa(base+0x11008);
    q.schedRing=GpuVa(base+0x20000);
    q.schedMqd=GpuVa(base+0x28000);
    q.schedRptr=GpuVa(base+0x29000);
    q.schedWptr=GpuVa(base+0x29008);
    q.schedulerStatus=GpuVa(base+0x29010);
    q.gfxRing=GpuVa(base+0x30000);
    q.gfxMqd=GpuVa(base+0x38000);
    q.gfxFence=GpuVa(base+0x39000);
    q.queuesOwned=true;
    q.gfxhubVmid0Live=true;
    q.mmhubVmid0Live=true;
    q.gpuVmInvalidationAck=true;
    q.doorbellBarRetained=true;
    return q;
}
int main() {
    Profile n=n33();
    assert(n.valid());
    assert(n.family==Family::Navi33);
    assert(n.gfxhub==Gfxhub::V3_0 && n.mmhub==Mmhub::V3_0_2);
    assert(n.gfxhubL2Cntl3Default==0x80120007u);
    assert(!n.gpuWritesEnabled);
    Profile p=select(0x1002,0x747e,11,0,3,3,0,0,13,0,10);
    assert(p.valid() && p.family==Family::Navi32);
    assert(p.gfxhub==Gfxhub::V3_0_3 && p.mmhub==Mmhub::V3_0);
    assert(p.gfxhubL2Cntl3Default==0x80100007u);
    assert(select(0x1002,0x7480,11,0,3,3,0,2,13,0,7).error==rn_navi33::m4::Error::BadIpVersion);
    assert(select(0x1002,0x7480,11,0,2,3,0,0,13,0,7).error==rn_navi33::m4::Error::BadIpVersion);
    assert(select(0x1002,0x7480,11,0,2,3,0,2,13,0,10).error==rn_navi33::m4::Error::BadIpVersion);
    assert(select(0x1002,0x744c,11,0,2,3,0,2,13,0,7).error==rn_navi33::m4::Error::UnsupportedAsic);
    assert(select(0x1234,0x7480,11,0,2,3,0,2,13,0,7).error==rn_navi33::m4::Error::UnsupportedAsic);

    auto g=geometry();
    auto q=buffers();
    auto check=inspectMesQueues(n,g,q,0x1000);
    assert(check.error==rn_navi33::m4::Error::Ok && !check.canProgramQueue);
    q.queuesOwned=false;
    assert(inspectMesQueues(n,g,q,0x1000).error==rn_navi33::m4::Error::MissingMemoryOwner);
    q.queuesOwned=true;
    q.mmhubVmid0Live=false;
    assert(inspectMesQueues(n,g,q,0x1000).error==rn_navi33::m4::Error::MissingVmMapping);
    q.mmhubVmid0Live=true;
    q.gfxRing=q.kiqRing;
    assert(inspectMesQueues(n,g,q,0x1000).error==rn_navi33::m4::Error::OverlappingBuffers);
    q=buffers();
    q.schedMqd=GpuVa(q.schedRing.value+0x1000);
    assert(inspectMesQueues(n,g,q,0x1000).error==rn_navi33::m4::Error::OverlappingBuffers);
    q=buffers();
    q.gfxMqd=GpuVa(q.gfxMqd.value+1);
    assert(inspectMesQueues(n,g,q,0x1000).error==rn_navi33::m4::Error::InconsistentAddress);
    q=buffers();
    q.doorbellBarRetained=false;
    assert(inspectMesQueues(n,g,q,0x1000).error==rn_navi33::m4::Error::InvalidDoorbell);
    q=buffers();
    assert(inspectMesQueues(n,g,q,0x200).error==rn_navi33::m4::Error::InvalidDoorbell);
    g.verified=false;
    assert(inspectMesQueues(n,g,q,0x1000).error==rn_navi33::m4::Error::MissingGeometry);
}
