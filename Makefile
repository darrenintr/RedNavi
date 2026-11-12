# Builds build/<Config>/RedNavi.kext. Needs macOS + Xcode command-line tools and the
# MacKernelSDK (scripts/bootstrap.sh fetches it).
ifeq ($(DEBUG),1)
CONFIG := Debug
OPT := -O0 -g -DDEBUG
else
CONFIG := Release
OPT := -Os
endif

SDK := MacKernelSDK
OUT := build/$(CONFIG)
# Every kext is signed at build time (ad hoc by default, no Team ID: an ad hoc signature is all the test machines need with SIP and AMFI relaxed), so nothing has to be signed
# again where it is installed. RN_SIGN_ID=<identity> signs with a certificate instead (an Apple Development one gives a Team ID, but changes nothing for loading here).
RN_SIGN_ID ?= -
# iCloud Drive puts Finder attributes back on files of this folder at any moment, and codesign refuses them: strip and sign again until it takes (a copy made with rsync -a has none).
SIGN = for i in 1 2 3 4 5; do xattr -cr $(1); codesign -f -s "$(RN_SIGN_ID)" --timestamp=none $(1) 2>/dev/null && break; sleep 1; done; codesign --verify $(1) || { echo "cannot sign $(1)"; exit 1; }
KEXT := $(OUT)/RedNavi.kext
FBKEXT := $(OUT)/RedNaviFB.kext
ACCELKEXT := $(OUT)/RedNaviAccel.kext
ACCEL2KEXT := $(OUT)/RedNaviAccel2.kext
CXX := clang++
HOST_CXX ?= $(CXX)
ARCH := -arch x86_64
CXXFLAGS := $(ARCH) $(OPT) -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -mkernel \
	-nostdinc -isystem $(SDK)/Headers -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE \
	-DAPPLE -DNeXT -mmacosx-version-min=12.0 -Wall -Icompat/include
# Optional local firmware component (python3 tools/gen_psp_fw_header.py ... RedNavi/psp_fw.h).
ifneq ($(wildcard RedNavi/psp_fw.h),)
CXXFLAGS += -DRN_HAVE_PSP_FW
endif
ifneq ($(wildcard RedNavi/smu_fw.h),)
CXXFLAGS += -DRN_HAVE_SMU_FW
endif
# Local pinned Navi33 SDMA firmware. No blob is shipped in the repository.
ifneq ($(wildcard RedNavi/navi33_sdma_fw.h),)
CXXFLAGS += -DRN_HAVE_NAVI33_SDMA_FW
endif
# Git-blob-pinned Navi33 GC11.0.2 RS64 PFP instructions only.
ifneq ($(wildcard RedNavi/navi33_gc_pfp_fw.h),)
CXXFLAGS += -DRN_HAVE_NAVI33_GC_PFP_FW
endif
ifneq ($(wildcard RedNavi/navi33_gc_me_fw.h),)
CXXFLAGS += -DRN_HAVE_NAVI33_GC_ME_FW
endif
ifneq ($(wildcard RedNavi/navi33_gc_mec_fw.h),)
CXXFLAGS += -DRN_HAVE_NAVI33_GC_MEC_FW
endif
# Optional, pinned Navi33 GC11.0.2 RLC-G executable payload.
ifneq ($(wildcard RedNavi/navi33_gc_rlc_fw.h),)
CXXFLAGS += -DRN_HAVE_NAVI33_GC_RLC_FW
endif
# Navi33 IMU firmware is pinned and locally generated, never in Git.
ifneq ($(wildcard RedNavi/navi33_gc_imu_fw.h),)
CXXFLAGS += -DRN_HAVE_NAVI33_GC_IMU_FW
endif
ifneq ($(wildcard RedNavi/navi33_mes_fw.h),)
CXXFLAGS += -DRN_HAVE_NAVI33_MES_FW
endif
# PSP AUTOLOAD_RLC can START RLC hardware: never build active by default.
ifeq ($(NAVI33_RLC_AUTOLOAD_UNSAFE),1)
CXXFLAGS += -DRN_ENABLE_NAVI33_RLC_AUTOLOAD_UNSAFE
endif
ifneq ($(wildcard RedNavi/gc_fw.h),)
CXXFLAGS += -DRN_HAVE_GC_FW
endif
ifneq ($(wildcard RedNavi/dmub_fw.h),)
CXXFLAGS += -DRN_HAVE_DMUB_FW
endif
LDFLAGS := $(ARCH) -Xlinker -kext -nostdlib -lkmod -L$(SDK)/Library -lkmodc++ -lcc_kext \
	-Xlinker -object_path_lto -Xlinker /dev/null -mmacosx-version-min=12.0

all: $(KEXT) $(FBKEXT) $(ACCELKEXT) $(ACCEL2KEXT)

# Every header of the kext: gfx11.h, rn_uapi.h, the generated splash and register headers changed behind a stale object once (a new splash that never reached the build).
$(OUT)/kern_probe.o: RedNavi/kern_probe.cpp $(wildcard RedNavi/*.h compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/edid.o: RedNavi/edid.cpp RedNavi/edid.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/mode_regs.o: RedNavi/mode_regs.cpp RedNavi/mode_regs.h RedNavi/edid.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/gfx11.o: RedNavi/gfx11.cpp $(wildcard RedNavi/*.h compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/smu13.o: RedNavi/smu13.cpp RedNavi/smu13.h $(wildcard compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# LinuxKPI-style compat layer (see docs/COMPAT.md). Plain objects linked into RedNavi.kext.
COMPAT_SRC := $(wildcard compat/src/*.cpp)
COMPAT_OBJ := $(patsubst compat/src/%.cpp,$(OUT)/compat_%.o,$(COMPAT_SRC))

$(OUT)/compat_%.o: compat/src/%.cpp $(wildcard compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/kern_info.o: RedNavi/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(KEXT): $(OUT)/kern_probe.o $(OUT)/smu13.o $(OUT)/gfx11.o $(OUT)/edid.o $(OUT)/mode_regs.o $(COMPAT_OBJ) $(OUT)/kern_info.o RedNavi/Info.plist.in
	@mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/kern_probe.o $(OUT)/smu13.o $(OUT)/gfx11.o $(OUT)/edid.o $(OUT)/mode_regs.o $(COMPAT_OBJ) $(OUT)/kern_info.o -o $(KEXT)/Contents/MacOS/RedNavi
	@if nm -u $(KEXT)/Contents/MacOS/RedNavi | grep -E "RedNavi|rn_|kRN|^ *_g[A-Z]" | grep -vE "_g(IO|OS)"; then echo "unresolved RedNavi symbols in the kext"; exit 1; fi
	cp RedNavi/Info.plist.in $(KEXT)/Contents/Info.plist
	$(call SIGN,$(KEXT))
	@echo "built $(KEXT)"

# The framebuffer is its own kext: IOGraphicsFamily is not in the boot kernel collection OpenCore injects into.
$(OUT)/fb_kern_fb.o: RedNaviFB/kern_fb.cpp
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/fb_kern_info.o: RedNaviFB/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(FBKEXT): $(OUT)/fb_kern_fb.o $(OUT)/fb_kern_info.o RedNaviFB/Info.plist.in
	@mkdir -p $(FBKEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/fb_kern_fb.o $(OUT)/fb_kern_info.o -o $(FBKEXT)/Contents/MacOS/RedNaviFB
	cp RedNaviFB/Info.plist.in $(FBKEXT)/Contents/Info.plist
	$(call SIGN,$(FBKEXT))
	@echo "built $(FBKEXT)"

# The accelerator skeleton (M6): also an AuxKC kext, it depends on IOAcceleratorFamily2 (docs/M6_ACCELERATOR_STUDY.md).
$(OUT)/accel_kern_accel.o: RedNaviAccel/kern_accel.cpp
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/accel_kern_info.o: RedNaviAccel/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(ACCELKEXT): $(OUT)/accel_kern_accel.o $(OUT)/accel_kern_info.o RedNaviAccel/Info.plist.in
	@mkdir -p $(ACCELKEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/accel_kern_accel.o $(OUT)/accel_kern_info.o -o $(ACCELKEXT)/Contents/MacOS/RedNaviAccel
	cp RedNaviAccel/Info.plist.in $(ACCELKEXT)/Contents/Info.plist
	$(call SIGN,$(ACCELKEXT))
	@echo "built $(ACCELKEXT)"

# RedNaviAccel2 (M6): the same family dependency, but the family's own start() runs (docs/M6_ACCELERATOR_STUDY.md).
$(OUT)/accel2_kern_accel2.o: RedNaviAccel2/kern_accel2.cpp RedNaviAccel/IOGraphicsAccelerator2.h RedNaviAccel2/stubs.inc $(wildcard RedNaviAccel2/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/accel2_kern_info.o: RedNaviAccel2/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(ACCEL2KEXT): $(OUT)/accel2_kern_accel2.o $(OUT)/accel2_kern_info.o RedNaviAccel2/Info.plist.in
	@mkdir -p $(ACCEL2KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/accel2_kern_accel2.o $(OUT)/accel2_kern_info.o -o $(ACCEL2KEXT)/Contents/MacOS/RedNaviAccel2
	cp RedNaviAccel2/Info.plist.in $(ACCEL2KEXT)/Contents/Info.plist
	$(call SIGN,$(ACCEL2KEXT))
	@echo "built $(ACCEL2KEXT)"

# Separate diagnostic-only Navi33 metadata observer. Deliberately NOT
# part of 'all' or normal EFI Kext artifacts: never installed or loaded by CI.
DIAGKEXT := $(OUT)/RedNaviNavi33Diag.kext
$(OUT)/navi33_diag.o: RedNaviDiag/kern_diag.cpp
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/navi33_diag_info.o: RedNaviDiag/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(DIAGKEXT): $(OUT)/navi33_diag.o $(OUT)/navi33_diag_info.o RedNaviDiag/Info.plist.in
	@mkdir -p $(DIAGKEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/navi33_diag.o $(OUT)/navi33_diag_info.o -o $(DIAGKEXT)/Contents/MacOS/RedNaviNavi33Diag
	cp RedNaviDiag/Info.plist.in $(DIAGKEXT)/Contents/Info.plist
	$(call SIGN,$(DIAGKEXT))
	@echo "built $(DIAGKEXT) (compile only; DO NOT LOAD)"

diag: $(DIAGKEXT)

# EXPERIMENTAL: compile the already-integrated RedNavi P4/P5/P6 code in a
# separate bundle and non-framebuffer IOKit match category. Not in 'all',
# not released, not injected by CI. Does NOT enable PSP/MES/Metal.
# Runtime additionally requires rednavi_navi33_unsafe_p4=1 at boot.
P4KEXT := $(OUT)/RedNaviNavi33P4.kext
$(OUT)/navi33_p4_kern_probe.o: RedNavi/kern_probe.cpp $(wildcard RedNavi/*.h compat/include/*/*.h)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -DRN_ENABLE_NAVI33_UNSAFE_P4 -c $< -o $@

$(OUT)/navi33_p4_kern_info.o: RedNaviP4/kern_info.c
	@mkdir -p $(OUT)
	clang $(ARCH) $(OPT) -mkernel -nostdinc -isystem $(SDK)/Headers -DKERNEL -mmacosx-version-min=12.0 -c $< -o $@

$(P4KEXT): $(OUT)/navi33_p4_kern_probe.o $(OUT)/smu13.o $(OUT)/gfx11.o $(OUT)/edid.o $(OUT)/mode_regs.o $(COMPAT_OBJ) $(OUT)/navi33_p4_kern_info.o RedNaviP4/Info.plist.in
	@mkdir -p $(P4KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(OUT)/navi33_p4_kern_probe.o $(OUT)/smu13.o $(OUT)/gfx11.o $(OUT)/edid.o $(OUT)/mode_regs.o $(COMPAT_OBJ) $(OUT)/navi33_p4_kern_info.o -o $(P4KEXT)/Contents/MacOS/RedNaviNavi33P4
	@if nm -u $(P4KEXT)/Contents/MacOS/RedNaviNavi33P4 | grep -E "RedNavi|rn_|kRN|^ *_g[A-Z]" | grep -vE "_g(IO|OS)"; then echo "unresolved RedNavi P4 symbols"; exit 1; fi
	cp RedNaviP4/Info.plist.in $(P4KEXT)/Contents/Info.plist
	$(call SIGN,$(P4KEXT))
	@echo "built $(P4KEXT) (P4 EXPERIMENTAL; NOT SAFE TO AUTO-INJECT)"

navi33-p4: $(P4KEXT)

# The Metal plug-in stub (user space, docs/M6_ACCELERATOR_STUDY.md) and the program that makes Metal load it.
MTLBUNDLE := $(OUT)/RedNaviMTLDriver.bundle
$(MTLBUNDLE): RedNaviMTL/plugin.m RedNaviMTL/compute.m RedNaviMTL/metallib.c RedNaviMTL/rn_air.c RedNaviMTL/rn_vk.c RedNaviMTL/rn_vk_selftest.c $(wildcard RedNaviMTL/*.h) RedNaviMTL/Info.plist
	@mkdir -p $(MTLBUNDLE)/Contents/MacOS
	clang -bundle -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -I/opt/local/include -framework Foundation -framework Metal -framework IOKit RedNaviMTL/plugin.m RedNaviMTL/compute.m RedNaviMTL/metallib.c RedNaviMTL/rn_air.c RedNaviMTL/rn_vk.c RedNaviMTL/rn_vk_selftest.c -framework IOSurface -framework ImageIO -framework CoreGraphics -o $(MTLBUNDLE)/Contents/MacOS/RedNaviMTLDriver
	cp RedNaviMTL/Info.plist $(MTLBUNDLE)/Contents/Info.plist
	xattr -cr $(MTLBUNDLE)
	$(call SIGN,$(MTLBUNDLE))
	@echo "built $(MTLBUNDLE)"

$(OUT)/mtl_probe: RedNaviMTL/mtl_probe.m
	@mkdir -p $(OUT)
	clang -arch x86_64 -mmacosx-version-min=12.0 -framework Metal -framework Foundation $< -o $@

mtl: $(MTLBUNDLE) $(OUT)/mtl_probe

clean:
	rm -rf build

# Host tests for the pure contracts (macOS or Linux, never in the kernel).
# Use HOST_CXX=g++ where Clang is unavailable; native kext builds still use CXX.
test:
	@mkdir -p build
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_probe_admission_test.cpp -o build/navi33_probe_admission_test
	./build/navi33_probe_admission_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_address_spaces_test.cpp -o build/navi33_address_spaces_test
	./build/navi33_address_spaces_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_m4_asic_test.cpp -o build/navi33_m4_asic_test
	./build/navi33_m4_asic_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_mmhub302_regs_test.cpp -o build/navi33_mmhub302_regs_test
	./build/navi33_mmhub302_regs_test
	$(HOST_CXX) -std=c++17 -Wall -DRN_HOST_TEST -Icompat/include tests/host/compat_test.cpp tests/host/host_hooks.cpp \
		compat/src/rn_slab.cpp compat/src/rn_firmware.cpp compat/src/rn_reg.cpp -o build/compat_test
	./build/compat_test
	$(HOST_CXX) -std=c++17 -Wall tests/host/edid_test.cpp RedNavi/edid.cpp -o build/edid_test
	./build/edid_test
	$(HOST_CXX) -std=c++17 -Wall tests/host/mode_regs_test.cpp RedNavi/mode_regs.cpp RedNavi/edid.cpp -o build/mode_regs_test
	./build/mode_regs_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_facts_test.cpp -o build/navi33_facts_test
	./build/navi33_facts_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p1_test.cpp -o build/navi33_p1_test
	./build/navi33_p1_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi -Imesa-bridge/include tests/host/navi33_p2_test.cpp -o build/navi33_p2_test
	./build/navi33_p2_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_runtime_test.cpp -o build/navi33_runtime_test
	./build/navi33_runtime_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_bootstrap_test.cpp -o build/navi33_bootstrap_test
	./build/navi33_bootstrap_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p6_psp_test.cpp -o build/navi33_p6_psp_test
	./build/navi33_p6_psp_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p6_psp_boot_plan_test.cpp -o build/navi33_p6_psp_boot_plan_test
	./build/navi33_p6_psp_boot_plan_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p6_staging_test.cpp -o build/navi33_p6_staging_test
	./build/navi33_p6_staging_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_gart_owner_test.cpp -o build/navi33_gart_owner_test
	./build/navi33_gart_owner_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_fw_pri_dma_owner_test.cpp -o build/navi33_fw_pri_dma_owner_test
	./build/navi33_fw_pri_dma_owner_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_vram_reservation_test.cpp -o build/navi33_vram_reservation_test
	./build/navi33_vram_reservation_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_vram_ledger_test.cpp -o build/navi33_vram_ledger_test
	./build/navi33_vram_ledger_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_allocator_carve_test.cpp -o build/navi33_allocator_carve_test
	./build/navi33_allocator_carve_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p7_runtime_test.cpp -o build/navi33_p7_runtime_test
	./build/navi33_p7_runtime_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p7_postpsp_test.cpp -o build/navi33_p7_postpsp_test
	./build/navi33_p7_postpsp_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p7_tmr_plan_test.cpp -o build/navi33_p7_tmr_plan_test
	./build/navi33_p7_tmr_plan_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p74_setup_tmr_test.cpp -o build/navi33_p74_setup_tmr_test
	./build/navi33_p74_setup_tmr_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p75_sdma_test.cpp -o build/navi33_p75_sdma_test
	./build/navi33_p75_sdma_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p76_sdma_th1_test.cpp -o build/navi33_p76_sdma_th1_test
	./build/navi33_p76_sdma_th1_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p77_gc_pfp_test.cpp -o build/navi33_p77_gc_pfp_test
	./build/navi33_p77_gc_pfp_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p78_gc_pfp_p0_test.cpp -o build/navi33_p78_gc_pfp_p0_test
	./build/navi33_p78_gc_pfp_p0_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p79_gc_pfp_p1_test.cpp -o build/navi33_p79_gc_pfp_p1_test
	./build/navi33_p79_gc_pfp_p1_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p710_gc_me_test.cpp -o build/navi33_p710_gc_me_test
	./build/navi33_p710_gc_me_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p711_gc_me_p0_test.cpp -o build/navi33_p711_gc_me_p0_test
	./build/navi33_p711_gc_me_p0_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p712_gc_me_p1_test.cpp -o build/navi33_p712_gc_me_p1_test
	./build/navi33_p712_gc_me_p1_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p713_stage_expand_test.cpp -o build/navi33_p713_stage_expand_test
	./build/navi33_p713_stage_expand_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p713_gc_mec_test.cpp -o build/navi33_p713_gc_mec_test
	./build/navi33_p713_gc_mec_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p714_gc_mec_p0_test.cpp -o build/navi33_p714_gc_mec_p0_test
	./build/navi33_p714_gc_mec_p0_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p715_gc_mec_p1_test.cpp -o build/navi33_p715_gc_mec_p1_test
	./build/navi33_p715_gc_mec_p1_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p716_gc_mec_p2_test.cpp -o build/navi33_p716_gc_mec_p2_test
	./build/navi33_p716_gc_mec_p2_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p717_gc_mec_p3_test.cpp -o build/navi33_p717_gc_mec_p3_test
	./build/navi33_p717_gc_mec_p3_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p718_rlc_g_test.cpp -o build/navi33_p718_rlc_g_test
	./build/navi33_p718_rlc_g_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p719_imu_i_test.cpp -o build/navi33_p719_imu_i_test
	./build/navi33_p719_imu_i_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p720_imu_d_test.cpp -o build/navi33_p720_imu_d_test
	./build/navi33_p720_imu_d_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p721_mes_sched_code_test.cpp -o build/navi33_p721_mes_sched_code_test
	./build/navi33_p721_mes_sched_code_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p722_mes_sched_data_test.cpp -o build/navi33_p722_mes_sched_data_test
	./build/navi33_p722_mes_sched_data_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p723_mes_kiq_code_test.cpp -o build/navi33_p723_mes_kiq_code_test
	./build/navi33_p723_mes_kiq_code_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p724_mes_kiq_data_test.cpp -o build/navi33_p724_mes_kiq_data_test
	./build/navi33_p724_mes_kiq_data_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p725_rlc_autoload_test.cpp -o build/navi33_p725_rlc_autoload_test
	./build/navi33_p725_rlc_autoload_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_gc_queue_bringup_test.cpp -o build/navi33_gc_queue_bringup_test
	./build/navi33_gc_queue_bringup_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_p7_pmfw_test.cpp -o build/navi33_p7_pmfw_test
	./build/navi33_p7_pmfw_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/navi33_linux_boot_parity_test.cpp -o build/navi33_linux_boot_parity_test
	./build/navi33_linux_boot_parity_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/dma_pages_test.cpp -o build/dma_pages_test
	./build/dma_pages_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -IRedNavi tests/host/mes_workspace_test.cpp -o build/mes_workspace_test
	./build/mes_workspace_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -DRN_HOST_TEST -Itests/host/stubs -Icompat/include -IRedNavi \
		tests/host/mes_runtime_reject_test.cpp RedNavi/gfx11.cpp -o build/mes_runtime_reject_test
	./build/mes_runtime_reject_test
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -DRN_HOST_TEST -Itests/host/stubs -Icompat/include -IRedNavi \
		tests/host/mes_runtime_submit_test.cpp -o build/mes_runtime_submit_test
	./build/mes_runtime_submit_test
	python3 -m unittest discover -s tests/host -p 'navi33_pci_resources_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_diag_isolation_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_p6_firmware_audit_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_p6_firmware_generator_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_smu_firmware_generator_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_sdma_firmware_generator_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_gc_pfp_firmware_generator_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_gc_me_firmware_generator_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_gc_mec_firmware_generator_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_gc_rlc_firmware_generator_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_gc_imu_firmware_generator_test.py'
	python3 -m unittest discover -s tests/host -p 'navi33_mes_firmware_generator_test.py'

# Exercise optional firmware call sites without embedding or linking any firmware.
# The declaration-only fixture is visible exclusively to this syntax check.
check-kernel-firmware:
	$(CXX) $(CXXFLAGS) -DRN_HAVE_GC_FW -DRN_COMPILE_ONLY_FIRMWARE -Itests/compile -fsyntax-only RedNavi/kern_probe.cpp

check-kernel-psp-firmware:
	$(CXX) $(CXXFLAGS) -DRN_HAVE_PSP_FW -DRN_HAVE_SMU_FW -DRN_HAVE_NAVI33_SDMA_FW -DRN_HAVE_NAVI33_GC_PFP_FW -DRN_HAVE_NAVI33_GC_ME_FW -DRN_HAVE_NAVI33_GC_MEC_FW -DRN_HAVE_NAVI33_GC_RLC_FW -DRN_HAVE_NAVI33_GC_IMU_FW -DRN_HAVE_NAVI33_MES_FW -Itests/compile -fsyntax-only RedNavi/kern_probe.cpp

# Force compilation of the dangerous opt-in path under a fake fixture;
# no GPU hardware writes are performed by -fsyntax-only.
check-kernel-rlc-autoload:
	$(CXX) $(CXXFLAGS) -DRN_HAVE_NAVI33_MES_FW -DRN_ENABLE_NAVI33_RLC_AUTOLOAD_UNSAFE -Itests/compile -fsyntax-only RedNavi/kern_probe.cpp

.PHONY: all clean test mtl diag navi33-p4 check-kernel-firmware check-kernel-psp-firmware check-kernel-rlc-autoload
