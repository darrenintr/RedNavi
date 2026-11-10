// SPDX-License-Identifier: BSD-3-Clause
// P6.6 allocator carve + quiescence audit tests; no GPU I/O.
#include "navi33_allocator_carve.h"
#include <cassert>
#include <cstdio>

namespace a = rn_navi33::allocator_carve;

static a::Input good() {
    a::Input in{};
    in.requested = true;
    in.ledgerOwned = true;
    in.reservation = {0x0fefe000ull, 0x00102000ull};
    return in;
}

int main() {
    auto in = good();
    auto r = a::inspect(in);
    assert(r.error == a::Error::Ok);
    assert(r.quiescent);
    assert(r.carveVerified);
    assert(r.beforePages == 0x3000u);
    assert(r.reservedPages == 0x102u);
    assert(r.afterPages == 0x2efeu);
    assert(r.left.offset == 0x0d000000ull);
    assert(r.left.bytes == 0x02efe000ull);
    assert(r.right.bytes == 0);
    assert(!r.mayMutateAllocator);
    assert(!r.mayWriteVram);

    in = good(); in.requested = false;
    assert(a::inspect(in).error == a::Error::Disabled);
    in = good(); in.ledgerOwned = false;
    assert(a::inspect(in).error == a::Error::NoLedger);

    in = good(); in.gpuLive = true;
    assert(a::inspect(in).error == a::Error::RuntimeBusy);
    in = good(); in.vmReady = true;
    assert(a::inspect(in).error == a::Error::RuntimeBusy);
    in = good(); in.gfxQueueOk = true;
    assert(a::inspect(in).error == a::Error::RuntimeBusy);
    in = good(); in.rednaviFramebufferReady = true;
    assert(a::inspect(in).error == a::Error::RuntimeBusy);
    in = good(); in.boCount = 1;
    assert(a::inspect(in).error == a::Error::RuntimeBusy);
    in = good(); in.vmidUsed = 1;
    assert(a::inspect(in).error == a::Error::RuntimeBusy);

    in = good(); in.pool.bytes -= 1;
    assert(a::inspect(in).error == a::Error::BadPool);
    in = good(); in.reservation.bytes -= 1;
    assert(a::inspect(in).error == a::Error::BadReservation);
    in = good(); in.reservation = {0x0cfff000ull, 0x2000ull};
    assert(a::inspect(in).error == a::Error::ReservationOutsidePool);
    in = good(); in.reservation = {0x0e000000ull, 0x100000ull};
    r = a::inspect(in);
    assert(r.error == a::Error::Ok);
    assert(r.left.bytes == 0x01000000ull);
    assert(r.right.offset == 0x0e100000ull);
    assert(r.right.bytes == 0x01f00000ull);

    std::puts("Navi33 P6.6 allocator carve tests passed (NO GPU I/O)");
    return 0;
}
