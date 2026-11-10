// Host-only IP register-map and VRAM window checks for P5 compute bring-up.
#include "navi33_bootstrap.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace rn_navi33;
static void put16(std::vector<uint8_t>& b, size_t o, uint16_t v) {
    b[o] = static_cast<uint8_t>(v);
    b[o + 1] = static_cast<uint8_t>(v >> 8);
}
static void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    put16(b, o, static_cast<uint16_t>(v));
    put16(b, o + 2, static_cast<uint16_t>(v >> 16));
}
static void finish(std::vector<uint8_t>& b) {
    uint32_t sum = 0;
    for (size_t i = 10; i < b.size(); ++i) sum += b[i];
    put16(b, 8, static_cast<uint16_t>(sum));
}
static std::vector<uint8_t> makeDiscovery(bool version2 = false, bool optional = true) {
    std::vector<uint8_t> b(1024, 0);
    put32(b, 0, 0x28211407);
    put16(b, 4, version2 ? 2 : 1);
    put16(b, 10, static_cast<uint16_t>(b.size()));
    const size_t list = version2 ? 16 : 12;
    if (version2) put16(b, 12, 1);
    put16(b, list, 0x60);
    put32(b, 0x60, 0x53445049);
    put16(b, 0x64, 4);
    put16(b, 0x66, 0x1c0);
    put16(b, 0x6c, 1);
    put16(b, 0x70, 0x100);
    const uint16_t ids[] = {11, 42, 255, 1, 271, 34, 40, 108};
    // Observed on 2026-11-08, Sapphire RX 7600 / rev CF, Linux 7.0:
    // Crucially MMHUB 3.0.2 has TWO segments (not five).
    const uint8_t counts[] = {4, 4, 5, 5, 5, 2, 2, 6};
    const uint32_t liveBases[8][6] = {
        {0x1260, 0xA000, 0x1C000, 0x2402C00},
        {0x1260, 0xA000, 0x1C000, 0x2402C00},
        {0x16000, 0xDC0000, 0xE00000, 0xE40000, 0x243FC00},
        {0x16000, 0xDC0000, 0xE00000, 0xE40000, 0x243FC00},
        {0x12, 0xC0, 0x34C0, 0x9000, 0x2403C00},
        {0x1A000, 0x2408800},
        {0x10A0, 0x240A000},
        {0, 0x14, 0xD20, 0x10400, 0x241B000, 0x4040000},
    };
    const IpVersion versions[] = {kReference.gc, kReference.sdma, kReference.psp,
        kReference.smu, kReference.dcn, kReference.mmhub, {1, 0, 0}, {4, 0, 0}};
    unsigned count = optional ? 8 : 6;
    put16(b, 0x102, static_cast<uint16_t>(count));
    size_t pos = 0x104;
    for (unsigned i = 0; i < count; ++i) {
        put16(b, pos, ids[i]);
        b[pos + 2] = 0;
        b[pos + 3] = counts[i];
        b[pos + 4] = versions[i].major;
        b[pos + 5] = versions[i].minor;
        b[pos + 6] = versions[i].revision;
        for (unsigned j = 0; j < counts[i]; ++j)
            put32(b, pos + 8u + 4u * j, liveBases[i][j]);
        pos += 8u + 4u * counts[i];
    }
    finish(b);
    return b;
}

int main() {
    for (bool v2 : {false, true}) {
        auto b = makeDiscovery(v2);
        const auto r = inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size());
        assert(r.error == RuntimeError::Ok);
        assert(hasComputeRegisterBases(r));
        assert(r.bases.gc.count == 4 && r.bases.gc.dwords[3] == 0x2402c00u);
        assert(r.bases.mmhub.count == 2 && r.bases.mmhub.dwords[1] == 0x2408800u);
        assert(r.bases.nbif.count == 6 && r.bases.nbif.dwords[5] == 0x4040000u);
        b = makeDiscovery(v2, false);
        const auto missingOptional = inspectRuntimeDiscovery(0x1002, 0x7480, b.data(), b.size());
        assert(missingOptional.error == RuntimeError::Ok);
        assert(!hasComputeRegisterBases(missingOptional));
    }

    // Physical RX7600 P4 (2026-11-09): MMHUB 3.0.0 offsets returned
    // 0xFFFFFFFF on MMHUB 3.0.2. Protect the Navi33-specific offsets.
    static_assert(mmhub_v3_0_2::kFbLocationBaseDword == 0x086cu,
                  "Navi33 MMHUB v3.0.2 FB_LOCATION_BASE offset changed");
    static_assert(mmhub_v3_0_2::kFbLocationTopDword == 0x086du,
                  "Navi33 MMHUB v3.0.2 FB_LOCATION_TOP offset changed");
    static_assert(mmhub_v3_0_2::kFbOffsetDword == 0x0857u,
                  "Navi33 MMHUB v3.0.2 FB_OFFSET offset changed");
    constexpr uint32_t kNavi33Mmhub0 = 0x1a000u; // real IP Discovery base
    uint32_t off = 0;
    assert(checkedBarDword(kNavi33Mmhub0, mmhub_v3_0_2::kFbLocationBaseDword,
                           1u << 20, &off));
    assert(off == 0x6a1b0u); // CPU BAR5 byte offset, not GPU MC address
    assert(checkedBarDword(kNavi33Mmhub0, mmhub_v3_0_2::kFbLocationTopDword,
                           1u << 20, &off));
    assert(off == 0x6a1b4u);
    assert(checkedBarDword(kNavi33Mmhub0, mmhub_v3_0_2::kFbOffsetDword,
                           1u << 20, &off));
    assert(off == 0x6a15cu);
    assert(checkedBarDword(0x100u, 0x8ecu, 0x4000, &off));
    assert(off == (0x100u + 0x8ecu) * 4u);
    assert(!checkedBarDword(0x100u, 0x8ecu, 0x2000, &off));
    assert(!checkedBarDword(UINT32_MAX, 0, 0x100000u, &off));
    assert(!checkedBarDword(0, 0, 4096, nullptr));

    assert(makeMemoryWindow(0xffffffffu, 0xffffffffu, 0xffffffffu,
                            8176, 256ull << 20).error ==
           MemoryError::BadLocation);
    const auto window = makeMemoryWindow(0x8000, 0x81ff, 0, 8192, 0x10000000ull);
    assert(window.valid());
    assert(window.fbStart == 0x8000000000ull);
    assert(window.locationBytes == 8ull * 1024 * 1024 * 1024);
    assert(window.fbBytes == 8ull * 1024 * 1024 * 1024);
    assert(window.cpuVisibleBytes == 256ull * 1024 * 1024);

    // Native macOS P4 v0.0.2 capture, 2026-11-10:
    // FB_LOCATION=0x8000..0x81ff (8192 MiB), RCC_CONFIG_MEMSIZE=8176 MiB,
    // BAR0=256 MiB. Linux GMC11 treats memsize as mc_vram_size and the
    // location register as the base, so the conservative usable span is
    // 8176 MiB rather than rejecting the one-unit disagreement.
    const auto observedMac = makeMemoryWindow(0x8000, 0x81ff, 0, 8176,
                                               256ull << 20);
    assert(observedMac.valid());
    assert(observedMac.locationBytes == 8192ull << 20);
    assert(observedMac.vramBytes == 8176ull << 20);
    assert(observedMac.fbBytes == 8176ull << 20);
    assert(observedMac.fbEnd == 0x81feffffffull);
    assert(observedMac.cpuVisibleBytes == 256ull << 20);
    assert(observedMac.fbStart == 0x8000000000ull);

    // Native Linux reference: FB_LOCATION span 8176 MiB while memsize/BAR0
    // span 8192 MiB. The inverse 16 MiB disagreement is accepted too.
    const auto captured = makeMemoryWindow(0x8000, 0x81fe, 0, 8192,
                                           8192ull * 1024 * 1024);
    assert(captured.valid());
    assert(captured.locationBytes == 8176ull * 1024 * 1024);
    assert(captured.fbBytes == 8176ull * 1024 * 1024);
    assert(captured.cpuVisibleBytes == 8176ull * 1024 * 1024);
    assert(makeMemoryWindow(0x8000, 0x81fd, 0, 8192,
                            8192ull * 1024 * 1024).error ==
           MemoryError::WindowTooSmall);
    assert(makeMemoryWindow(0x8000, 0x81ff, 0, 8160,
                            256ull << 20).error ==
           MemoryError::WindowTooSmall);
    assert(makeMemoryWindow(0x8000, 0x81fe, 0, 8192,
                            8192ull * 1024 * 1024 + 4096ull).error ==
           MemoryError::InvalidBar);
    assert(!makeMemoryWindow(0x8000, 0x81ff, 0, 0, 0x10000000).valid());
    assert(makeMemoryWindow(0x8001, 0x8000, 0, 8192, 0x10000000).error == MemoryError::BadLocation);
    assert(makeMemoryWindow(0x8000, 0x80ff, 0, 8192, 0x10000000).error == MemoryError::WindowTooSmall);
    assert(makeMemoryWindow(0x8000, 0x81ff, 0, 8192, 1024).error == MemoryError::InvalidBar);
    std::puts("Navi33 P5 compute preflight tests passed (no GPU commands)");
    return 0;
}
