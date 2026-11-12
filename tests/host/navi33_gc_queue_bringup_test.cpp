#include "navi33_gc_queue_bringup.h"
#include <cassert>
using namespace rn_navi33::gc_queue_bringup;

int main() {
    Input i{};
    auto a = inspect(i);
    assert(!a.mayCreateGraphicsQueue && a.blocker == Error::RlcNotBooted);
    i.rlcBootComplete = true;
    assert(inspect(i).blocker == Error::GfxHubNotInitialized);
    i.gfxHubInitialized = true;
    assert(inspect(i).blocker == Error::Rs64NotConfigured);
    i.rs64Configured = true;
    assert(inspect(i).blocker == Error::GcRegistersNotInitialized);
    i.gcRegistersInitialized = true;
    assert(inspect(i).blocker == Error::MesSchedulerNotRunning);
    i.mesSchedulerRunning = true;
    assert(inspect(i).blocker == Error::MesKiqNotRunning);
    i.mesKiqRunning = true;
    assert(inspect(i).blocker == Error::NoQueueMemoryOwner);
    i.queueMemoryOwnerVerified = true;
    assert(inspect(i).blocker == Error::NoDoorbellMapping);
    i.doorbellMapped = true;
    assert(inspect(i).blocker == Error::NoInterruptOwner);
    i.interruptOwnerVerified = true;
    assert(inspect(i).blocker == Error::NoScratchGpuFence);
    i.scratchGpuFenceCompleted = true;
    assert(inspect(i).mayCreateGraphicsQueue);
    i.mesKiqRunning = false;
    assert(!inspect(i).mayCreateGraphicsQueue);
    return 0;
}
