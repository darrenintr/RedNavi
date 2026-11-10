#ifndef REDNAVI_NAVI33_TOPOLOGY_WIRE_H
#define REDNAVI_NAVI33_TOPOLOGY_WIRE_H
// SPDX-License-Identifier: BSD-3-Clause
// nullarc-inspired 128-byte topology ABI prototype, C/C++ and host-only.
// No live IOUserClient selector exists yet. Do not use Navi32 constants.
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RN_NAVI33_TOPOLOGY_VERSION 1u
#define RN_NAVI33_TOPOLOGY_CAPTURED 1u
#define RN_NAVI33_TOPOLOGY_RUNTIME_READY 2u
#define RN_NAVI33_TOPOLOGY_BYTES 128u
struct rn_navi33_topology_wire {
    uint32_t version, flags, vendor, device, revision, subvendor, subdevice;
    uint32_t num_shader_engines, num_shader_arrays_per_engine, num_cu_per_sh;
    uint32_t num_rb_pipes, enabled_rb_pipes_mask, cu_active_number;
    uint32_t cu_bitmap[4][4], reserved[3];
};
static inline unsigned rn_navi33_popcnt(uint32_t bits) {
    unsigned n = 0u;
    while (bits) { bits &= bits - 1u; ++n; }
    return n;
}
// Aligned struct, exact size. Copy unaligned raw bytes into a local struct first.
static inline int rn_navi33_topology_valid(const struct rn_navi33_topology_wire *p, size_t bytes) {
    if (!p || bytes != RN_NAVI33_TOPOLOGY_BYTES ||
        p->version != RN_NAVI33_TOPOLOGY_VERSION ||
        !(p->flags & RN_NAVI33_TOPOLOGY_CAPTURED) ||
        (p->flags & ~(RN_NAVI33_TOPOLOGY_CAPTURED | RN_NAVI33_TOPOLOGY_RUNTIME_READY)) ||
        p->vendor != 0x1002u || p->device != 0x7480u || p->revision > 0xffu ||
        p->subvendor > 0xffffu || p->subdevice > 0xffffu ||
        p->num_shader_engines == 0u || p->num_shader_engines > 4u ||
        p->num_shader_arrays_per_engine == 0u || p->num_shader_arrays_per_engine > 4u ||
        p->num_cu_per_sh == 0u || p->num_cu_per_sh > 32u ||
        p->num_rb_pipes == 0u || p->num_rb_pipes > 32u ||
        p->enabled_rb_pipes_mask == 0u || p->cu_active_number == 0u ||
        p->reserved[0] || p->reserved[1] || p->reserved[2]) return 0;
    uint32_t cmask = p->num_cu_per_sh == 32u ? UINT32_MAX : (UINT32_C(1) << p->num_cu_per_sh) - 1u;
    uint32_t rmask = p->num_rb_pipes == 32u ? UINT32_MAX : (UINT32_C(1) << p->num_rb_pipes) - 1u;
    if (p->enabled_rb_pipes_mask & ~rmask) return 0;
    unsigned active = 0u;
    for (unsigned se = 0; se < 4u; ++se)
        for (unsigned sa = 0; sa < 4u; ++sa) {
            uint32_t bits = p->cu_bitmap[se][sa];
            if ((bits & ~cmask) || ((se >= p->num_shader_engines ||
                sa >= p->num_shader_arrays_per_engine) && bits)) return 0;
            active += rn_navi33_popcnt(bits);
        }
    return active == p->cu_active_number;
}
// Both the hardware queue/compute AND the runtime flag must be proven.
static inline int rn_navi33_topology_radv_ready(const struct rn_navi33_topology_wire *p,
    size_t bytes, int verifiedPhysicalCompute) {
    return verifiedPhysicalCompute && rn_navi33_topology_valid(p, bytes) &&
        (p->flags & RN_NAVI33_TOPOLOGY_RUNTIME_READY) != 0u;
}
#ifdef __cplusplus
}
static_assert(sizeof(rn_navi33_topology_wire) == RN_NAVI33_TOPOLOGY_BYTES,
              "Navi33 topology wire ABI must be 128 bytes");
#endif
#endif
