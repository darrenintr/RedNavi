#ifndef REDNAVI_NAVI33_ALLOCATOR_CARVE_H
#define REDNAVI_NAVI33_ALLOCATOR_CARVE_H
// SPDX-License-Identifier: BSD-3-Clause
//
// P6.6: software-only visible-VRAM allocator carve + runtime quiescence audit.
// This models how the future visible allocator must exclude the P6.5 owned
// GART bookkeeping range. It does not mutate VramPool and performs no GPU I/O.
#include <stdint.h>

namespace rn_navi33 { namespace allocator_carve {

constexpr uint64_t kPage = 4096ull;
constexpr uint64_t kVisibleTailStart = 0x0d000000ull;
constexpr uint64_t kVisibleTailEnd = 0x10000000ull; // exclusive
constexpr bool kHardwareWritesEnabled = false;

enum class Error : uint8_t {
    Ok,
    Disabled,
    NoLedger,
    RuntimeBusy,
    BadPool,
    BadReservation,
    ReservationOutsidePool,
};

struct Range {
    uint64_t offset = 0;
    uint64_t bytes = 0;
};

struct Input {
    bool requested = false;
    bool ledgerOwned = false;

    bool gpuLive = false;
    bool vmReady = false;
    bool gfxQueueOk = false;
    bool rednaviFramebufferReady = false;
    uint32_t boCount = 0;
    uint32_t vmidUsed = 0;

    Range pool{kVisibleTailStart, kVisibleTailEnd - kVisibleTailStart};
    Range reservation{};
};

struct Report {
    Error error = Error::Disabled;
    Range left{};
    Range right{};
    uint32_t beforePages = 0;
    uint32_t reservedPages = 0;
    uint32_t afterPages = 0;
    bool quiescent = false;
    bool carveVerified = false;
    bool mayMutateAllocator = false;
    bool mayWriteVram = false;
};

constexpr bool aligned(uint64_t v) {
    return !(v & (kPage - 1u));
}
constexpr bool valid(const Range &r) {
    return r.bytes && aligned(r.offset) && aligned(r.bytes) &&
           r.offset <= UINT64_MAX - r.bytes;
}
constexpr uint64_t end(const Range &r) {
    return r.offset + r.bytes;
}

inline Report inspect(const Input &in) {
    Report r{};
    if (!in.requested) return r;
    if (!in.ledgerOwned) {
        r.error = Error::NoLedger;
        return r;
    }

    r.quiescent = !in.gpuLive && !in.vmReady && !in.gfxQueueOk &&
                  !in.rednaviFramebufferReady && in.boCount == 0 &&
                  in.vmidUsed == 0;
    if (!r.quiescent) {
        r.error = Error::RuntimeBusy;
        return r;
    }

    if (!valid(in.pool)) {
        r.error = Error::BadPool;
        return r;
    }
    if (!valid(in.reservation)) {
        r.error = Error::BadReservation;
        return r;
    }
    if (in.reservation.offset < in.pool.offset ||
        end(in.reservation) > end(in.pool)) {
        r.error = Error::ReservationOutsidePool;
        return r;
    }

    r.beforePages = static_cast<uint32_t>(in.pool.bytes / kPage);
    r.reservedPages = static_cast<uint32_t>(in.reservation.bytes / kPage);

    if (in.reservation.offset > in.pool.offset) {
        r.left = {
            in.pool.offset,
            in.reservation.offset - in.pool.offset
        };
    }
    if (end(in.reservation) < end(in.pool)) {
        r.right = {
            end(in.reservation),
            end(in.pool) - end(in.reservation)
        };
    }

    r.afterPages = static_cast<uint32_t>((r.left.bytes + r.right.bytes) / kPage);
    if (r.afterPages + r.reservedPages != r.beforePages) {
        r.error = Error::BadReservation;
        return r;
    }

    r.carveVerified = true;
    r.error = Error::Ok;
    return r;
}

}} // namespace rn_navi33::allocator_carve
#endif
