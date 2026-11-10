# Navi33 Cross-Driver Host Lab (RX4Darwin / nullarc / Cobalt64)

Branch: feature/navi33-crossdriver-lab
Base: darrenintr/RedNavi main at 64817f1b.
Status: **HOST-SIDE PROTOTYPE, NOT A WORKING RX 7600 DRIVER**.

## Three experiments

1. RX4Darwin-inspired ordered evidence checks in RedNavi/navi33_stage_gate.h.
Discovery -> PSP/Firmware -> GPUVM -> SDMA -> MES/Graphics Queue -> physical Compute -> RADV -> Metal.
Each stage requires previously verified evidence; the model never grants GPU write permission.
No GFX12 registers are copied into GFX11.0.2 code.

2. nullarc-inspired fixed-size topology wire format in RedNavi/navi33_topology_wire.h.
It proposes a 128-byte interface containing PCI identity, shader engines,
shader arrays, CU bitmaps and RB mask. It DOES NOT define a working IOUserClient
selector or a production Mesa consumer. Tests only use synthetic data.
Actual Navi33 CU and RB details must come from authenticated Linux amdgpu
capture or live hardware measurements. Do not use Navi32 constants as fallback.
A set runtime-ready flag without verified physical compute is insufficient.

3. Cobalt64-inspired IOAcceleratorFamily2 ABI checks in
tools/navi33_ioaccel_abi_audit.py. It compares a locally generated
Cobalt64 ioaccel-layout model.json against RedNavi's older macOS 15 baseline.
Structural agreement cannot confirm actual C++ vtable linkage: run the
upstream Cobalt64 check_layout.py compile-time vtable test on the target
macOS version. Mismatches require review; do not guess or patch blindly.

## Host tests (no GPU)

    make test

Or:

    mkdir -p build
    c++ -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_crossdriver_test.cpp -o build/navi33_crossdriver_test
    ./build/navi33_crossdriver_test
    python3 -m unittest discover -s tests/host -p navi33_ioaccel_abi_audit_test.py -v

CI also runs the C++ tests with GCC/Clang and ASan/UBSan.
No evidence of RX 7600 hardware bring-up is implied by a passing host test.

## macOS-only ABI study (user's own kernel collections)

Follow Cobalt64/tools/ioaccel-layout/README.md to prepare the correct
Boot/System Kernel Collections and locally extracted IOAcceleratorFamily2
and AppleParavirtGPU binaries; Cobalt64 does not ship a kext extractor.

    export C64_WORK="$HOME/c64-work"
    export C64_LAYOUT_OUT="$HOME/c64-layout-out"
    export C64_MKSDK="$HOME/MacKernelSDK"
    cd /path/to/Cobalt64/tools/ioaccel-layout
    python3 dump_vtables.py
    python3 gen_header.py
    python3 /path/to/RedNavi/tools/navi33_ioaccel_abi_audit.py --model "$C64_LAYOUT_OUT/model.json"
    python3 check_layout.py

Do NOT commit model.json, vtable dumps, extracted kernel collections or
Apple-derived generated headers. Baseline counts and size references
come from RedNavi docs/M6_ACCEL_ABI.md and RedNaviAccel2/kern_accel2.cpp.

## Safety constraints

- No changes to production kext, Mesa bridge, Metal plugin or OpenCore.
- No real firmware load, MMIO writes, BAR remapping, new GPUVM page tables,
  doorbell writes, MES queue takeover, reset, or shader dispatch.
- Existing P4/P5/P6 fail-closed Navi33 checks remain unchanged.
- Keep separate: BAR physical aperture, mapped CPU virtual address, GPU
  MC physical VRAM address, per-VMID GPU virtual address, MMIO DWORD offsets.
- With the RX 7600 as sole display GPU, live PSP/MES tests must await
  an independently recoverable test setup.

## Next steps to integrate after evidence is captured

1. Verify actual board CU bitmap and RB mask from Linux amdgpu.
2. Add a versioned read-only Navi33 UserClient topology producer and
   a strict Mesa/RADV consumer without Navi32 fallback.
3. Record PSP, GART page table, SDMA fence, MES/MQD, physical compute
   readback evidence independently on real hardware.
4. Independently validate each macOS build's IOAccelerator ABI and review
   RedNaviAccel2 before enabling the new path.

## Inspirations (architecture only, new code BSD-3-Clause)

RX4Darwin: https://github.com/RX4Darwin/AMDRadeonRDNA4.kext
nullarc: https://github.com/luuumi-dev/nullarc
Cobalt64: https://github.com/Almosst-DEV/Cobalt64
RedNavi: https://github.com/Helemen7/RedNavi

No third-party source or noncommercial code has been copied. Examine
licenses and attribution before future direct source integration.
