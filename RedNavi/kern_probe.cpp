// RedNavi probe (milestone M1): read-only. Matches the RX 7800 XT (Navi 32, 1002:747E),
// maps the register BAR, and logs what the GPU tells us about itself: PCI config, BAR sizes
// and the IP discovery table (the authoritative list of IP block versions).
//
// Second stage: reads the PSP (MP0) and SMU (MP1) mailbox registers, using the base addresses
// found in that table, to show what the VBIOS left running. Third stage: reads the MMHUB
// framebuffer location registers, which give the VRAM address as the GPU itself sees it (needed
// to hand buffers to the PSP bootloader). Results are logged and published as the
// "ip-discovery", "mailbox-regs" and "mmhub-regs" properties (ioreg -rn RedNaviProbe -l -x).
//
// Fourth stage (opt-in, writes to the GPU): with the boot-arg rednavi_psp_load=1 and a build that
// has RedNavi/psp_fw.h (see tools/gen_psp_fw_header.py), copies each PSP component (KDB, system
// driver, ..., secure OS) into VRAM and hands it to the PSP bootloader, in the order and with the
// handshake of psp_hw_start / psp_v13_0_bootloader_load_component in amdgpu. Stops at the first error.
// Fifth stage (opt-in, rednavi_psp_ring=1): asks the running secure OS to create the kernel-mode
// ring (psp_v13_0_ring_create). The sOS answering is the first proof that it accepts commands.
// Sixth stage (opt-in, rednavi_psp_cmd=1): sends one harmless command through that ring, the
// boot-config query (GFX_CMD_ID_BOOT_CFG, BOOTCFG_CMD_GET), and logs the response.
// Seventh stage (opt-in, rednavi_psp_toc=1): sends GFX_CMD_ID_LOAD_TOC with the TOC component of the
// PSP firmware file; the response carries the TMR size. amdgpu does not send SETUP_TMR on this PSP
// (boot_time_tmr and autoload are both set for MP0 13.0.10, see docs/PSP_BRINGUP.md).
//
// Ninth stage (read-only, always on): reads the DMCUB control, boot-status and mailbox registers
// (DCN 3.2, register offsets from dcn_3_2_0_offset.h, all at DMU base index 2) to see whether the
// VBIOS left a display microcontroller running. Published as "dmcub-regs".
//
// Seventh-and-a-half stage (opt-in, rednavi_psp_tmr=1): GFX_CMD_ID_SETUP_TMR with the size LOAD_TOC
// returned. For MP0 13.0.10 amdgpu sets boot_time_tmr = false, so it does send this (the Linux
// reference dmesg on this card shows "reserve 0xa700000 from 0x83e0000000 for PSP TMR").
//
// Eleventh stage (read-only, always on): reads the DCN hot-plug-detect registers (HPDn_DC_HPD_INT_STATUS,
// bit 1 = DC_HPD_SENSE, bit 4 = SENSE_DELAYED, and the raw pin levels in DC_GPIO_HPD_Y) to see which
// connectors have a monitor. Published as "hpd-regs".
//
// Twelfth stage (opt-in, rednavi_edid=1, writes the DCN AUX engines): runs a native DPCD read (16 bytes at
// 0x000) on each of the five AUX engines, as dce_aux.c does, to find the one wired to the monitor; on the
// first engine that ACKs it then reads the EDID with I2C-over-AUX at address 0x50. Published as
// "aux-probe" (per-engine status), "dpcd" and "edid" (tools/decode_edid.py).
//
// Thirteenth stage (opt-in, rednavi_dp_train=1, writes DCN and the sink): enables the DisplayPort transmitter
// of path 0 through the VBIOS command table (via DMUB) and runs the DP link training (clock recovery with
// TPS1, channel equalisation with TPS3 or TPS2) against the monitor over AUX4, the way link_dp_training.c does.
// The result is the sink's own verdict: DPCD 0x202-0x204 (CR_DONE, CHANNEL_EQ_DONE, SYMBOL_LOCKED, align).
// Published as "dp-train".
//
// Fifteenth stage (opt-in, rednavi_dcn_replay=N, writes the DCN pipe): replays the register state of a lit
// pipe captured on Linux (RedNavi/dcn_replay.h, tools/gen_dcn_replay.py) in nine groups: pixel clock DTO,
// OPTC clocks, OTG timing, OPP/FMT, MPC output, DIG/DP stream setup, OTG enable, test pattern, DP stream
// enable. N limits how many groups are applied (1 to 9), so the pipe can be brought up step by step. Needs
// the DP link trained first (rednavi_dp_train=1). Every group is logged and read back.
//
// Groups 10 to 14 of the replay add a plane: the probe writes a 1920x1080 XRGB8888 test image into VRAM
// (kRNScanoutVramOff, linear, pitch 7680 as the Linux text console uses), then replays the DCHUBBUB
// watermarks, DPP, MPC tree and HUBP configuration captured on Linux for that linear surface, points the
// primary surface address at the image and finally switches the pattern generator off.
//
// Fourteenth stage (UNSAFE, rednavi_dcn_dump_unsafe=1): reading every DCN 3.2 register of the list from macOS
// HANGS THE MACHINE (found on 2026-10-28; the console freezes with stale lines on screen). Kept only as a
// record; use tools/linux_dcn_dump.py, which reads the same list safely on Linux. Reads every DCN 3.2 register of tools/dcn32_reglist.txt
// (4366 registers) after everything else has run and publishes the values, in list order, as the "dcn-dump"
// property. tools/linux_dcn_dump.py produces the same list on Linux, so the two states can be diffed.
//
// Tenth stage (opt-in, writes to the GPU, rednavi_dmub_load=1 and a build with RedNavi/dmub_fw.h):
// stops the DMCUB the VBIOS left running, builds the DMUB memory block in VRAM, backdoor-loads the
// DAL firmware and releases the reset, mirroring dmub_srv_hw_init and the dmub_dcn32_* hooks in amdgpu.
// rednavi_dmub_cmd=1 then sends DMUB_CMD__QUERY_FEATURE_CAPS through inbox1 and reads the reply from
// the command slot (the mailbox ring is a 8 KiB array of 64 byte commands in the DMUB block).
// rednavi_dmub_psp=1 first hands the signed image to the PSP (GFX_CMD_ID_LOAD_IP_FW, type DMUB) and
// skips the backdoor load, which is how amdgpu loads it when load_type is PSP.
// rednavi_dmub_mode=1 programs VRAM offsets instead of MC addresses for the data windows.
//
// Eighth stage (read-only, always on): reads the VBIOS image from the PCI expansion ROM, which the
// DMUB framebuffer window needs (amdgpu_dm_dmub.c copies adev->bios into DMUB_WINDOW_3_VBIOS).
// Published as the "vbios" property (decode with tools/check_vbios.py). Enabling the ROM decode
// bit in config space is the only write.
//
// Without that boot-arg nothing is written to the GPU except the MM_INDEX/MM_DATA window used to
// read VRAM.

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include "dcn_reglist.h"
#include "dcn_replay.h"
#include <IOKit/IODeviceTreeSupport.h>
#include "edid.h"
#include "mode_regs.h"
#include "smu13.h"
#include <IOKit/IODMACommand.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "gfx11.h"
#include "rn_dma_pages.h"
#include "rn_uapi.h"
#include "splash_1920x1080.h"
#include "linux_info_navi32.h"
#include "gc_regs.h"
#include "navi33_runtime.h"
#include "navi33_bootstrap.h"
#include "navi33_p6_psp_preflight.h"
#include "navi33_p7_runtime.h"
#include "navi33_p7_postpsp.h"
#include "navi33_p7_tmr_plan.h"
#include "navi33_p74_setup_tmr.h"
#include "navi33_p75_sdma.h"
#include "navi33_p76_sdma_th1.h"
#include "navi33_p77_gc_pfp.h"
#include "navi33_p78_gc_pfp_p0.h"
#include "navi33_p79_gc_pfp_p1.h"
#include "navi33_p710_gc_me.h"
#include "navi33_p711_gc_me_p0.h"
#include "navi33_p712_gc_me_p1.h"
#include "navi33_p713_gc_mec.h"
#include "navi33_p714_gc_mec_p0.h"
#include "navi33_p715_gc_mec_p1.h"
#include "navi33_p716_gc_mec_p2.h"
#include "navi33_p717_gc_mec_p3.h"
#include "navi33_p718_rlc_g.h"
#include "navi33_p719_imu_i.h"
#include "navi33_p720_imu_d.h"
#include "navi33_p721_mes_sched_code.h"
#include "navi33_p722_mes_sched_data.h"
#include "navi33_p723_mes_kiq_code.h"
#include "navi33_p724_mes_kiq_data.h"
#include "navi33_p725_rlc_autoload.h"
#include "navi33_rlc_readonly_preflight.h"
#include "navi33_mp1_fwflags_smn_probe.h"
#include "navi33_smu_mailbox_admission.h"
#include "navi33_smu_mailbox_roundtrip.h"
#include "navi33_gc_queue_bringup.h"
#include "navi33_p713_stage_expand.h"
#include "navi33_p7_pmfw.h"
#include "navi33_p6_psp_boot_plan.h"
#include "navi33_p6_psp_staging.h"
#include "navi33_gart_owner.h"
#include "navi33_fw_pri_dma_owner.h"
#include "navi33_vram_reservation.h"
#include "navi33_vram_ledger.h"
#include "navi33_allocator_carve.h"
#include "navi33_linux_boot_parity.h"
#include "navi33_address_spaces.h"
#include "navi33_m4_asic.h"
#include "navi33_probe_admission.h"

#ifdef RN_HAVE_PSP_FW
#include "psp_fw.h"
#endif
#ifdef RN_HAVE_SMU_FW
#include "smu_fw.h"
#endif
#ifdef RN_HAVE_NAVI33_SDMA_FW
#include "navi33_sdma_fw.h"
#endif
#ifdef RN_HAVE_NAVI33_GC_PFP_FW
#include "navi33_gc_pfp_fw.h"
#endif
#ifdef RN_HAVE_NAVI33_GC_ME_FW
#include "navi33_gc_me_fw.h"
#endif
#ifdef RN_HAVE_NAVI33_GC_MEC_FW
#include "navi33_gc_mec_fw.h"
#endif
#ifdef RN_HAVE_NAVI33_GC_RLC_FW
#include "navi33_gc_rlc_fw.h"
#endif
#ifdef RN_HAVE_NAVI33_GC_IMU_FW
#include "navi33_gc_imu_fw.h"
#endif
#ifdef RN_HAVE_NAVI33_MES_FW
#include "navi33_mes_fw.h"
#endif
#ifdef RN_HAVE_GC_FW
#include "gc_fw.h"
#endif
#ifdef RN_HAVE_DMUB_FW
#include "dmub_fw.h"
#endif

// Log a step and give the console time to print it, so a hang shows the last step reached on screen.
#define RN_STEP(...) do { IOLog(__VA_ARGS__); IOSleep(40); } while (0)

#define kRNRegBar          5          // MMIO registers (512 KB on Navi 3x)
#define kRNVramBar         0          // VRAM aperture (size depends on Resizable BAR)

// SOC15 MMIO window: byte offsets inside the register BAR (dword 0x0, 0x1, 0x6 in amdgpu).
#define kRNMmIndex         0x00
#define kRNMmData          0x04
#define kRNMmIndexHi       0x18

// Register byte offsets (dword index * 4) from amdgpu_discovery.c, "consistent across all the SOCs".
#define kRNRccConfigMemsize   (0xde3 * 4)    // VRAM size in MB (mmRCC_CONFIG_MEMSIZE)
#define kRNMp0C2pmsg33        (0x16061 * 4)  // bit 31 set once IFWI init is done (mmMP0_SMN_C2PMSG_33)

// IP discovery sits DISCOVERY_TMR_OFFSET below the end of VRAM and is DISCOVERY_TMR_SIZE long
// (amdgpu_discovery.h). BINARY_SIGNATURE is in include/discovery.h.
#define kRNDiscoveryTmrOffset (64 * 1024)
#define kRNDiscoveryTmrSize   (10 * 1024)
#define kRNBinarySignature    0x28211407u
#define kRNFallbackVramMB     (16u * 1024)  // RX 7800 XT, used if the memsize register reads 0 or all ones

// Hardware IDs in the discovery table (soc15_hw_ip.h).
#define kRNMp0C2pmsg35        0x063       // dword offsets from the MP0 base
#define kRNMp0C2pmsg36        0x064
#define kRNMp0C2pmsg58        0x07a
#define kRNPspBlLoadSos       0x20000u    // PSP_BL__LOAD_SOSDRV
#define kRNMp0C2pmsg81        0x091
#define kRNPspBlReady         0x80000000u
#define kRNVramMcBase         0x8000000000ull  // MMHUB FB_LOCATION_BASE 0x8000 << 24, read by probe stage 3
#define kRNPspBufVramOffset   0x40000000ull    // 1 GB into VRAM, 1 MB aligned, well clear of the table at the end
#define kRNPspRingVramOffset  0x40200000ull    // 4 KB ring, 2 MB into the staging area
#define kRNPspCmdVramOffset   0x40210000ull    // 4 KB command/response buffer, 4 KB aligned
#define kRNPspFenceVramOffset 0x40220000ull    // 4 KB fence buffer
#define kRNMp0C2pmsg67        0x083
#define kRNMp0C2pmsg68        0x084       // GPCOM ring read pointer (owned/advanced by PSP)
#define kRNPspCmdSetupTmr     0x05u            // GFX_CMD_ID_SETUP_TMR
#define kRNPspTmrVramOffset   0x02000000ull    // 32 MB into VRAM: inside the 256 MB CPU-visible BAR0
#define kRNPspCmdLoadToc      0x20u            // GFX_CMD_ID_LOAD_TOC
#define kRNPspCmdBootCfg      0x22u            // GFX_CMD_ID_BOOT_CFG
#define kRNPspBootCfgGet      2u               // BOOTCFG_CMD_GET
#define kRNPspRingSize        0x1000u
#define kRNPspRingTypeKm      2u               // PSP_RING_TYPE__KM
#define kRNMp0C2pmsg64        0x080
#define kRNMp0C2pmsg69        0x085
#define kRNMp0C2pmsg70        0x086
#define kRNMp0C2pmsg71        0x087
#define kRNPspMboxMask        0x8000FFFFu      // response flag plus status bits
#define kRNScanoutVramOff     0x08000000ull    // 128 MB into VRAM, inside the 256 MB macOS maps through BAR0: MC address 0x8008000000, the value in dcn_replay.h
#define kRNPspBufSize         (1u << 20)
#define kRNHwIdMp1            1
#define kRNHwIdMmhub          34
#define kRNHwIdDmu            271
#define kRNDmubBlockVramOff   0x50000000ull    // 1.25 GB into VRAM
#define kRNPspCmdLoadIpFw     0x06u            // GFX_CMD_ID_LOAD_IP_FW
#define kRNPspFwTypeDmub      51u              // GFX_FW_TYPE_DMUB
#define kRNPspFwTypeSmu       18u              // GFX_FW_TYPE_SMU
#define kRNPspCmdAutoloadRlc  0x21u            // GFX_CMD_ID_AUTOLOAD_RLC
#define kRNHwIdGc             11
#define kRNDmubGpintStopFw    0x10020000u      // status 1, command DMUB_GPINT__STOP_FW (2)
#define kRNDmubStopResponse   0xDEADDEADu
#define kRNDmubCw0Base        0x60000000u
#define kRNDmubCw1Base        0x61000000u
#define kRNDmubCw3Base        0x63000000u
#define kRNDmubCw4Base        0x64000000u
#define kRNDmubCw5Base        0x65000000u
#define kRNDmubCw6Base        0x66000000u
#define kRNDmubRegion5Base    0xA0000000u
#define kRNDmubRbSize         8192u
#define kRNDmubTraceEntryOff  16u
#define kRNHwIdMp0            255
#define kRNHwIdOsssys         40
#define kRNHwIdNbif           108

// Mailbox registers: dword offsets from the IP base address, taken from mp_13_0_0_offset.h
// (PSP, used by psp_v13_0.c) and smu_v13_0.c (SMU message, argument and response).
struct RNMailboxReg {
    const char *name;
    uint16_t hwId;
    uint32_t dword;
    uint32_t baseIdx;   // which of the block's base addresses the offset is relative to
};
static const RNMailboxReg kRNMailboxRegs[] = {
    {"MP0_C2PMSG_33", kRNHwIdMp0, 0x061},  // bit 31: IFWI init done
    {"MP0_C2PMSG_35", kRNHwIdMp0, 0x063},  // bootloader command / status, bit 31: ready
    {"MP0_C2PMSG_36", kRNHwIdMp0, 0x064},  // bootloader address
    {"MP0_C2PMSG_64", kRNHwIdMp0, 0x080},  // ring command / response
    {"MP0_C2PMSG_67", kRNHwIdMp0, 0x083},  // GPCOM ring write pointer
    {"MP0_C2PMSG_68", kRNHwIdMp0, 0x084},  // GPCOM ring read pointer
    {"MP0_C2PMSG_71", kRNHwIdMp0, 0x087},  // ring size
    {"MP0_C2PMSG_81", kRNHwIdMp0, 0x091},  // sOS alive when not zero
    {"MP1_C2PMSG_66", kRNHwIdMp1, 0x282},  // SMU message
    {"MP1_C2PMSG_82", kRNHwIdMp1, 0x292},  // SMU argument
    {"MP1_C2PMSG_90", kRNHwIdMp1, 0x29a},  // SMU response, 1 means OK
};

// MMHUB 3.0 registers (mmhub_3_0_0_offset.h). FB_BASE, FB_TOP and FB_OFFSET are 24-bit values in
// units of 16 MB (mmhub_v3_0_get_fb_location and _get_mc_fb_offset shift them left by 24).
static const RNMailboxReg kRNMmhubRegs[] = {
    {"MMHUB_FB_LOCATION_BASE", kRNHwIdMmhub, 0x8ec},
    {"MMHUB_FB_LOCATION_TOP",  kRNHwIdMmhub, 0x8ed},
    {"MMHUB_FB_OFFSET",        kRNHwIdMmhub, 0x8d7},
    {"MMHUB_AGP_BASE",         kRNHwIdMmhub, 0x8f0},
    {"MMHUB_AGP_BOT",          kRNHwIdMmhub, 0x8ef},
    {"MMHUB_AGP_TOP",          kRNHwIdMmhub, 0x8ee},
};

// DMCUB registers (dcn_3_2_0_offset.h), relative to base index 2 of the DMU block. DMCUB_SCRATCH0 is
// the firmware boot status (dmub_fw_boot_status in dmub_cmd.h); DMCUB_CNTL.DMCUB_ENABLE is bit 0 in
// the amdgpu field definitions and DMCUB_SEC_CNTL.DMCUB_SEC_RESET is the reset bit.
static const RNMailboxReg kRNDmcubRegs[] = {
    {"DMCUB_CNTL",         kRNHwIdDmu, 0x1f6, 2},
    {"DMCUB_CNTL2",        kRNHwIdDmu, 0x200, 2},
    {"DMCUB_SEC_CNTL",     kRNHwIdDmu, 0x1ce, 2},
    {"DMCUB_SCRATCH0",     kRNHwIdDmu, 0x1e3, 2},
    {"DMCUB_SCRATCH1",     kRNHwIdDmu, 0x1e4, 2},
    {"DMCUB_SCRATCH2",     kRNHwIdDmu, 0x1e5, 2},
    {"DMCUB_INBOX1_WPTR",  kRNHwIdDmu, 0x1d6, 2},
    {"DMCUB_INBOX1_RPTR",  kRNHwIdDmu, 0x1d7, 2},
    {"DMCUB_OUTBOX1_WPTR", kRNHwIdDmu, 0x1de, 2},
    {"DMCUB_GPINT_DATAOUT", kRNHwIdDmu, 0x1f9, 2},
    {"DMCUB_REGION3_CW0_OFFSET", kRNHwIdDmu, 0x1b5, 2},
};

static const RNMailboxReg kRNHpdRegs[] = {
    {"HPD0_INT_STATUS", kRNHwIdDmu, 0x1f14, 2}, {"HPD0_CONTROL", kRNHwIdDmu, 0x1f16, 2},
    {"HPD1_INT_STATUS", kRNHwIdDmu, 0x1f1c, 2}, {"HPD1_CONTROL", kRNHwIdDmu, 0x1f1e, 2},
    {"HPD2_INT_STATUS", kRNHwIdDmu, 0x1f24, 2}, {"HPD2_CONTROL", kRNHwIdDmu, 0x1f26, 2},
    {"HPD3_INT_STATUS", kRNHwIdDmu, 0x1f2c, 2}, {"HPD3_CONTROL", kRNHwIdDmu, 0x1f2e, 2},
    {"HPD4_INT_STATUS", kRNHwIdDmu, 0x1f34, 2}, {"HPD4_CONTROL", kRNHwIdDmu, 0x1f36, 2},
    {"DC_GPIO_HPD_MASK", kRNHwIdDmu, 0x28f4, 2}, {"DC_GPIO_HPD_A", kRNHwIdDmu, 0x28f5, 2},
    {"DC_GPIO_HPD_EN", kRNHwIdDmu, 0x28f6, 2}, {"DC_GPIO_HPD_Y", kRNHwIdDmu, 0x28f7, 2},
};

static void rnPublish(IOService *svc, const char *property, const char *const *names, const uint32_t *vals, unsigned n) {
    OSDictionary *out = OSDictionary::withCapacity(n);
    if (!out) return;
    for (unsigned i = 0; i < n; i++) {
        OSNumber *num = OSNumber::withNumber(static_cast<uint64_t>(vals[i]), 64);
        if (num) { out->setObject(names[i], num); num->release(); }
    }
    svc->setProperty(property, out);
    out->release();
}

static void rnPublishOne(OSDictionary *d, const char *key, uint64_t v) {
    OSNumber *n = OSNumber::withNumber(v, 64);
    if (n) { d->setObject(key, n); n->release(); }
}

class IOAccelRedNaviClient;

class RedNaviProbe : public IOService {
    OSDeclareDefaultStructors(RedNaviProbe)

    IOPCIDevice *pci {nullptr};
    IOMemoryMap *regs {nullptr};
    IOBufferMemoryDescriptor *hostBuf {nullptr};   // system memory the GPU reaches through the GART (rednavi_gart_test)
    IODMACommand *hostDma {nullptr};
    uint64_t hostBus {0};                          // its bus address as the device must use it
    uint32_t hostBusLen {0};
    // P6.4 ownership-only PSP fw_pri backing. This allocation may obtain IOVA
    // mappings from IOKit, but is NEVER installed into the GPU GART in P4.
    IOBufferMemoryDescriptor *navi33FwPriBuf {nullptr};
    IODMACommand *navi33FwPriDma {nullptr};
    uint64_t *navi33FwPriPages {nullptr};
    uint32_t navi33FwPriPageCount {0};
    uint64_t navi33FwPriBytes {0};
    bool navi33FwPriPrepared {false};
    // Set only after P7 has installed these retained pages into live GPU
    // page tables and completed the VMID0 flush. Until a verified hub/GART
    // teardown exists, freeing the DMA mapping would leave dangling GPU PTEs.
    bool navi33P7GartProgrammed {false};
    // P7.1 PSP GPCOM shared memory: one 4 KiB page each for ring,
    // command/response and fence. All three are host DMA pages mapped into
    // VMID0/GART immediately after the 1 MiB fw_pri window.
    IOBufferMemoryDescriptor *navi33PspRuntimeBuf {nullptr};
    IODMACommand *navi33PspRuntimeDma {nullptr};
    uint64_t navi33PspRuntimePages[rn_navi33::p7_postpsp::kRuntimePages] {};
    bool navi33PspRuntimePrepared {false};
    bool navi33PspRuntimeMapped {false};
    // P7.3 dedicated non-PSP firmware staging. Linux uses firmware.fw_buf,
    // separate from PSP fw_pri, for LOAD_IP_FW sources on bare metal.
    IOBufferMemoryDescriptor *navi33PmfwBuf {nullptr};
    IODMACommand *navi33PmfwDma {nullptr};
    uint64_t *navi33PmfwPages {nullptr};
    uint32_t navi33PmfwPageCount {0};
    uint64_t navi33PmfwStageBytes {0};
    bool navi33PmfwPrepared {false};
    bool navi33PmfwMapped {false};
    // Once a PTE points at this backing, retain it even after a failed flush.
    // PMFW admission and DMA exposure are separate notions.

    // Last P7 GPCOM submission telemetry.
    uint32_t p7SubmitWptrBefore {0};
    uint32_t p7SubmitWptrAfter {0};
    uint32_t p7SubmitRptrBefore {0};
    uint32_t p7SubmitRptrAfterKick {0};
    uint32_t p7SubmitRptrFinal {0};
    uint32_t p7SubmitFenceBefore {0};
    uint32_t p7SubmitFenceExpected {0};
    uint32_t p7SubmitFenceObserved {0};
    uint32_t p7SubmitPolls {0};
    bool p7SubmitWasIssued {false};
    bool p7SubmitResponseValid {false};
    bool p7SubmitTransportQuarantined {false};
    uint32_t p7SubmitTransportError {0};
    // P6.5 software-only VRAM reservation. These offsets are bookkeeping
    // claims only; P4 never writes them or exposes them to the GPU.
    bool navi33VramLedgerOwned {false};
    uint64_t navi33VramPtOffset {0};
    uint64_t navi33VramScratchOffset {0};
    uint64_t navi33VramDummyOffset {0};
    // Buffer objects: pages of host memory with an address in the GART window that is never reused (see docs/COMPAT.md on remapping).
    // gpuVa: the buffer's address in the GART window (0 for a buffer that is not mapped there); bus: the bus address of each of its 4 KB pages, from which the user address spaces get their PTEs.
    // A VRAM buffer has no md/dma/bus: vramOff is its offset in VRAM (physically contiguous, 4 KB aligned), dom is RN_BO_VRAM or RN_BO_VRAM_VISIBLE, and a visible one has vmd, the
    // sub-range of the PCI aperture it occupies, which is what the caller maps. A slot is free when size is 0.
    struct RNBo { IOBufferMemoryDescriptor *md; IODMACommand *dma; uint64_t gpuVa; uint32_t size; uint64_t *bus; bool gart; IOMemoryDescriptor *vmd; uint64_t vramOff; uint32_t dom; bool fixed; bool quarantined; };
    // VRAM that no one else in the kext uses, handed out in 4 KB pages by first fit from a sorted list of free ranges that merge when they touch. A range allocator, not a buddy
    // system: buffers come in any size up to 1 GB, a buddy system would round a 640 MB buffer up to 1 GB, and the list stays short because a buffer's neighbours are merged on free.
    struct VramPool {
        struct Seg { uint64_t first, pages; };
        static constexpr uint32_t kMax = 256;
        Seg seg[kMax];
        uint32_t n {0};
        uint64_t totalPages {0}, freePages {0};
        void init(uint64_t base, uint64_t bytes) { n = 1; seg[0] = {base / 4096, bytes / 4096}; totalPages = freePages = bytes / 4096; }
        void add(uint64_t base, uint64_t bytes) { if (n < kMax) { seg[n++] = {base / 4096, bytes / 4096}; totalPages += bytes / 4096; freePages += bytes / 4096; } }
        bool alloc(uint64_t bytes, uint64_t *off) {
            const uint64_t pages = bytes / 4096;
            for (uint32_t i = 0; i < n; i++) {
                if (seg[i].pages < pages) continue;
                *off = seg[i].first * 4096;
                seg[i].first += pages; seg[i].pages -= pages;
                if (seg[i].pages == 0) { for (uint32_t k = i + 1; k < n; k++) seg[k - 1] = seg[k]; n--; }
                freePages -= pages;
                return true;
            }
            return false;
        }
        void release(uint64_t off, uint64_t bytes) {
            const uint64_t first = off / 4096, pages = bytes / 4096;
            uint32_t i = 0;
            while (i < n && seg[i].first < first) i++;
            const bool joinPrev = i > 0 && seg[i - 1].first + seg[i - 1].pages == first;
            const bool joinNext = i < n && first + pages == seg[i].first;
            if (joinPrev && joinNext) { seg[i - 1].pages += pages + seg[i].pages; for (uint32_t k = i + 1; k < n; k++) seg[k - 1] = seg[k]; n--; }
            else if (joinPrev) seg[i - 1].pages += pages;
            else if (joinNext) { seg[i].first = first; seg[i].pages += pages; }
            else if (n < kMax) { for (uint32_t k = n; k > i; k--) seg[k] = seg[k - 1]; seg[i] = {first, pages}; n++; }
            freePages += pages;                               // (a full list loses the range's address space, never its accounting)
        }
    };
    VramPool devPool, visPool;
    IODeviceMemory *bar0Mem {nullptr};                    // the PCI aperture of VRAM (BAR0), whose first visPool.. bytes the CPU reaches
    int createVramBo(uint32_t size, bool visible);
    bool fbReady {false};                          // the display surface exists (rednavi_fb=1)
    int createScanoutBo(uint32_t surface);
    uint32_t scanoutSurfaces() const;              // 1 to 3: how many surfaces of the current mode fit
    uint64_t scanoutStride() const;
    uint64_t scanoutOff(uint32_t surface) const { return kRNScanoutVramOff + static_cast<uint64_t>(surface) * scanoutStride(); }
    uint32_t frontSurface {0};                     // the one the display was last told to scan out
    int present(uint32_t surface, bool wait);      // HUBP0 page flip at the next vertical blank; 0 ok, 1 still pending, <0 error                         // an alias of the display surface (fixed VRAM offset, never given back)
    static constexpr uint32_t kBoCap = 65536;             // buffers of the whole kext, the connections' and the kernel's
    static constexpr uint32_t kBoMaxBytes = 1u << 30;     // one buffer
    RNBo *bos {nullptr};
    uint64_t boBytes {0};                                 // host memory wired for buffers now
    uint64_t boBudget {2048ull << 20};                    // and the most it may be: wired memory is not swappable (boot-arg rednavi_bo_budget_mb)
    uint32_t boCount {0};
    uint32_t gartNextPage {16};                    // the first pages of the window are used by the boot-time tests
    struct rn_gmc gmcLive {};
    bool gfxQueueOk {false};                       // the graphics queue is mapped and passed its self test (rednavi_gfx_queue=1)
    bool gpuLive {false};                          // the memory hub is on and the GART window is set up
    // The user address spaces (rn_vm_*): VMIDs 1 to 7, 48 bits, a root directory (PDB2) and the directories and page tables below it made on demand,
    // 4 KB pages of VRAM from a pool of kVmPoolPages per VMID starting at kVmTables.
    static constexpr uint64_t kVmMin = 0x10000ull;         // virtual_address_offset of DEV_INFO on Linux 7.0 for this card
    static constexpr uint64_t kVmEnd = 1ull << 48;           // the 48-bit space: the lower half 0 to 2^47 and the upper half, which a program sees sign-extended (0xffff800000000000 and up)
    static constexpr uint64_t kVmTables = 0x60000000ull;   // VRAM, 1.5 GB in: outside the PCI aperture, so that the aperture's free space can hold CPU-visible buffers
    static constexpr uint32_t kVmPoolPages = 8192;         // 32 MB per VMID: about 8000 tables (a table maps 2 MB), 16 GB of mapped buffers; 7 VMIDs take 224 MB of VRAM from kVmTables (the 4096 of before ran out at 31 buffers of 256 MB); the device-local pool starts above them
    uint32_t vmPool[8][kVmPoolPages / 32];           // bit set: pool page in use
    bool vmPtb(int vmid, uint64_t vaPage, bool create, uint64_t *ptbOff);
    uint64_t vmPageAlloc(int vmid);                  // a zeroed pool page (VRAM offset), 0 when the pool is empty
    uint32_t vramMbytes {16384};                     // RCC_CONFIG_MEMSIZE (the INFO queries answer with it)
    uint32_t vmidUsed {0};                           // bit n: VMID n is a connection's
    bool vmReady {false};
    int vmAlloc();                                   // a VMID with an empty page table bound to it, or -1
    void vmFree(int vmid);
    bool vmMap(int vmid, int boIdx, uint32_t boPage, uint64_t vaPage, uint32_t pages);   // copies the buffer's GART PTEs into the VMID's tables; false: the pool ran out (nothing stays mapped)
    void vmUnmap(int vmid, uint64_t vaPage, uint32_t pages);
    IOLock *queueLock {nullptr};                   // sequence numbers, submits and resets of the compute queue, in that order
    uint32_t queueResets {0};
    uint32_t gfxQueueResets {0};
    uint32_t hangMs {8000};                        // a wait this long without the fence moving resets the queue (rednavi_hang_ms)
    IOAccelRedNaviClient *vmClient[8] {};                 // the connection of each VMID
    int resetQueue(uint32_t info[4]);              // resets the compute queue and settles every connection's fence; with queueLock held by the caller
    int resetGfxQueue(uint32_t info[4]);           // the same for the graphics queue
    IOLock *boLock {nullptr};                      // the buffer table and the window allocator
    int createHostBo(uint32_t size, bool writeCombine = false, bool gart = true);
    void destroyBo(int idx);                       // invalidates the PTEs, flushes, then releases the pages; the slot is reused, the GPU addresses never are
    friend class IOAccelRedNaviClient;
public:
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
private:
    IOMemoryMap *doorbellKeep {nullptr};           // the doorbell BAR mapping, kept while the compute queue is in use
    // rednavi_queue_watch=1: samples the compute queue every 250 ms for two minutes and records every change of its doorbell/control state.
    IOTimerEventSource *watchTimer {nullptr};
    uint32_t watchTick {0};
    uint32_t watchPrev[5] {};
    uint32_t watchEvents {0};
    uint32_t watchLog[8][7] {};                    // tick, doorbell control, pq control, pq status, rptr, wptr, cp_stat
    int watchBoA {-1}, watchBoB {-1};
    uint32_t watchIbDwords {0};
    uint64_t computeRingOff {0x0C020000};         // the ring of the queue that submissions go to (the mapped one after rn_kiq_map_compute)
    uint32_t watchPciCmd {0xFFFFFFFFu};
    bool keepAlive {false};                        // rednavi_keepalive=1: resubmit every tick to see whether idleness is what breaks the queue
    uint32_t kaSent {0}, kaOk {0}, kaFirstFail {0}, kaDbctl {0};              // the buffers of the kernel buffer test, reused for timed resubmissions
    uint32_t resubLog[3][7] {};
    uint32_t resubCount {0};
    void mqdSnapshot(const char *property);
    void publishMes(const struct rn_mes_result &mr);
    void watchStep();
    IOInterruptEventSource *msiSrc {nullptr};      // the message-signalled interrupt of the GPU (rednavi_msi_test)
    volatile uint32_t msiCount {0};
    volatile uint32_t msiLastWptr {0};
    static void msiHandler(OSObject *owner, IOInterruptEventSource *src, int count);
    static int waitMsi(void *ctx, uint32_t timeoutMs);
    OSData *disc {nullptr};   // copy of the IP discovery table
    OSData *vbiosCopy {nullptr};   // VBIOS image read from the PCI ROM
    bool dmubUp {false};           // our DMCUB is running and its mailbox is set up
    uint32_t dmubDmu {0};          // DMU base index 2
    uint64_t dmubMailbox {0};      // VRAM offset of the DMUB mailbox window (inbox1 ring)

    uint32_t rd(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(regs->getVirtualAddress() + off); }
    void wr(uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(regs->getVirtualAddress() + off) = v; }

    // Read one dword of VRAM through the index/data pair (works regardless of BAR0 size).
    uint32_t vramRead(uint64_t pos) {
        wr(kRNMmIndex, static_cast<uint32_t>(pos) | 0x80000000u);
        wr(kRNMmIndexHi, static_cast<uint32_t>(pos >> 31));
        uint32_t v = rd(kRNMmData);
        return v;
    }

    void vramWrite(uint64_t pos, uint32_t v) {
        wr(kRNMmIndex, static_cast<uint32_t>(pos) | 0x80000000u);
        wr(kRNMmIndexHi, static_cast<uint32_t>(pos >> 31));
        wr(kRNMmData, v);
    }

    void dumpPci();
    void dumpDiscovery();
    void dumpVbios();
    void loadDmcub();
    void loadSmuFw();
    void loadGcFw();
    void sdmaTest();
    struct rn_device gpu;          // register access in the amdgpu style, filled in start()
    void gcStatus(OSDictionary *out, const char *prefix, uint32_t timeoutMs);
    void probeAux();
    bool dmubSend(const uint8_t *cmd, uint32_t len, uint32_t reply[16]);
    void dpInit();
    void dpTrain();
    void dcnDump();
    void dcnReplay();
    void fillScanout();
    void publishFramebuffer();
    void smuProbe(const char *property, bool send);
    void publishModes(const uint8_t *edid, size_t len);
    rn_mode selMode {};            // the mode the pipe is programmed for (boot-arg rednavi_mode=WxH@R, else the EDID's preferred, else 1080p60)
    uint32_t selRate {0x06};       // DisplayPort link rate code for that mode
    uint32_t selPitch {1920};      // scanout pitch in pixels
    void selectMode();
    IOReturn callPlatformFunction(const OSSymbol *name, bool waitForFunction, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
    void applyMode(const rn_mode &m);
    void quiescePipe();
    void trace(const char *what);
    bool programLive(const rn_mode &m);
    IOReturn setModeLive(uint32_t index);
    int selIndex {-1};             // index of selMode in edidInfo.modes, -1 when it is the built-in 1080p60
    bool pipeReady {false};        // the pipe has been lit once, a live mode change is possible
    bool liveSwitch {false};       // dcnReplay is running for a mode change: do not repaint the test image
    IOLock *modeLock {nullptr};
    rn_edid_info edidInfo {};      // parsed EDID of the connected monitor, valid when edidInfo.mode_count > 0
    bool dpTx(uint8_t action, uint8_t modeOrLane, uint8_t lanes, uint32_t symclk10k);
    bool dpcdWrite(uint32_t eng, uint32_t addr, const uint8_t *d, uint32_t n);
    bool dpcdRead(uint32_t eng, uint32_t addr, uint8_t *d, uint32_t n);
    bool auxTransfer(uint32_t eng, uint32_t action, uint32_t addr, uint8_t *data, uint32_t len, uint32_t *status, uint32_t *reply);
    void dmcubSnapshot(const char *property);
    bool ipBase(uint16_t hwId, uint32_t *base, uint32_t idx = 0);
    bool loadComponent(uint32_t mp0, const char *name, uint32_t cmd, const uint8_t *data, uint32_t len, uint32_t version);
    void loadPspChain();
    bool createPspRing();
    bool pspSubmit(uint32_t cmdId, const uint32_t *args, uint32_t nargs, uint32_t *resp, uint32_t nresp);
    void sendPspBootCfgGet();
    void sendPspLoadToc();
    void sendPspSetupTmr();
    uint32_t pspTmrSize {0};
    uint32_t pspFence {0};
    void dumpRegs(const RNMailboxReg *list, size_t count, const char *property);
    bool prepareNavi33FwPriDma(bool enabled, uint32_t *mappingError);
    void releaseNavi33FwPriDma();
    bool prepareNavi33PspRuntimeDma(uint32_t *mappingError);
    void releaseNavi33PspRuntimeDma();
    bool prepareNavi33PmfwDma(uint64_t bytes, uint32_t *mappingError);
    void releaseNavi33PmfwDma();
    bool createPspRingP7(uint64_t ringGpuVa);
    bool pspSubmitP7(uint64_t ringGpuVa, uint64_t cmdGpuVa,
                     uint64_t fenceGpuVa, uint32_t cmdId,
                     const uint32_t *args, uint32_t nargs,
                     uint32_t *resp, uint32_t nresp);

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(RedNaviProbe, IOService)

void RedNaviProbe::releaseNavi33FwPriDma() {
    if (navi33FwPriDma) {
        // clearMemoryDescriptor(autoComplete=true) releases the active DMA
        // mapping created by setMemoryDescriptor(autoPrepare=true).
        navi33FwPriDma->clearMemoryDescriptor();
        navi33FwPriDma->release();
        navi33FwPriDma = nullptr;
    }
    if (navi33FwPriPages) {
        IOFree(navi33FwPriPages,
               sizeof(uint64_t) * rn_navi33::fw_pri_owner::kPages);
        navi33FwPriPages = nullptr;
    }
    if (navi33FwPriBuf) {
        navi33FwPriBuf->release();
        navi33FwPriBuf = nullptr;
    }
    navi33FwPriPageCount = 0;
    navi33FwPriBytes = 0;
    navi33FwPriPrepared = false;
}

bool RedNaviProbe::prepareNavi33FwPriDma(bool enabled,
                                        uint32_t *mappingError) {
    if (mappingError) *mappingError = 0;
    if (!enabled) return false;
    if (navi33FwPriPrepared && navi33FwPriBuf && navi33FwPriDma &&
        navi33FwPriPages &&
        navi33FwPriPageCount == rn_navi33::fw_pri_owner::kPages &&
        navi33FwPriBytes == rn_navi33::fw_pri_owner::kBytes)
        return true;

    releaseNavi33FwPriDma();

    navi33FwPriBuf = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut,
        rn_navi33::fw_pri_owner::kBytes, 4096);
    if (!navi33FwPriBuf) return false;
    void *cpu = navi33FwPriBuf->getBytesNoCopy();
    if (!cpu) {
        releaseNavi33FwPriDma();
        return false;
    }
    memset(cpu, 0, rn_navi33::fw_pri_owner::kBytes);

    navi33FwPriDma = IODMACommand::withSpecification(
        kIODMACommandOutputHost64,
        rn_navi33::fw_pri_owner::kDmaAddressBits,
        0, IODMACommand::kMapped,
        rn_navi33::fw_pri_owner::kBytes, 4096, nullptr, nullptr);
    if (!navi33FwPriDma ||
        navi33FwPriDma->setMemoryDescriptor(navi33FwPriBuf) != kIOReturnSuccess) {
        releaseNavi33FwPriDma();
        return false;
    }

    navi33FwPriPages = static_cast<uint64_t *>(
        IOMalloc(sizeof(uint64_t) * rn_navi33::fw_pri_owner::kPages));
    if (!navi33FwPriPages) {
        releaseNavi33FwPriDma();
        return false;
    }

    const auto mapping = rn_dma::collectPages<IODMACommand::Segment64>(
        navi33FwPriPages, rn_navi33::fw_pri_owner::kPages,
        rn_navi33::fw_pri_owner::kBytes,
        [this](uint64_t *offset, IODMACommand::Segment64 *segments,
               uint32_t *count) {
            UInt64 dmaOffset = *offset;
            UInt32 dmaCount = *count;
            const IOReturn rc = navi33FwPriDma->genIOVMSegments(
                &dmaOffset, segments, &dmaCount);
            *offset = dmaOffset;
            *count = dmaCount;
            return rc == kIOReturnSuccess;
        });
    if (!mapping.ok()) {
        if (mappingError) *mappingError = static_cast<uint32_t>(mapping.error);
        releaseNavi33FwPriDma();
        return false;
    }

    navi33FwPriPageCount = mapping.pageCount;
    navi33FwPriBytes = mapping.bytes;
    navi33FwPriPrepared = true;
    return true;
}

void RedNaviProbe::releaseNavi33PspRuntimeDma() {
    if (navi33PspRuntimeDma) {
        navi33PspRuntimeDma->clearMemoryDescriptor();
        navi33PspRuntimeDma->release();
        navi33PspRuntimeDma = nullptr;
    }
    if (navi33PspRuntimeBuf) {
        navi33PspRuntimeBuf->release();
        navi33PspRuntimeBuf = nullptr;
    }
    memset(navi33PspRuntimePages, 0, sizeof(navi33PspRuntimePages));
    navi33PspRuntimePrepared = false;
}

bool RedNaviProbe::prepareNavi33PspRuntimeDma(uint32_t *mappingError) {
    if (mappingError) *mappingError = 0;
    if (navi33PspRuntimePrepared && navi33PspRuntimeBuf &&
        navi33PspRuntimeDma)
        return true;

    releaseNavi33PspRuntimeDma();
    constexpr uint64_t bytes =
        rn_navi33::p7_postpsp::kRuntimePages * 4096ull;

    navi33PspRuntimeBuf = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, bytes, 4096);
    if (!navi33PspRuntimeBuf ||
        !navi33PspRuntimeBuf->getBytesNoCopy()) {
        releaseNavi33PspRuntimeDma();
        return false;
    }
    memset(navi33PspRuntimeBuf->getBytesNoCopy(), 0, bytes);

    navi33PspRuntimeDma = IODMACommand::withSpecification(
        kIODMACommandOutputHost64,
        rn_navi33::fw_pri_owner::kDmaAddressBits,
        0, IODMACommand::kMapped, bytes, 4096, nullptr, nullptr);
    if (!navi33PspRuntimeDma ||
        navi33PspRuntimeDma->setMemoryDescriptor(
            navi33PspRuntimeBuf) != kIOReturnSuccess) {
        releaseNavi33PspRuntimeDma();
        return false;
    }

    const auto mapping = rn_dma::collectPages<IODMACommand::Segment64>(
        navi33PspRuntimePages,
        rn_navi33::p7_postpsp::kRuntimePages, bytes,
        [this](uint64_t *offset, IODMACommand::Segment64 *segments,
               uint32_t *count) {
            UInt64 dmaOffset = *offset;
            UInt32 dmaCount = *count;
            const IOReturn rc =
                navi33PspRuntimeDma->genIOVMSegments(
                    &dmaOffset, segments, &dmaCount);
            *offset = dmaOffset;
            *count = dmaCount;
            return rc == kIOReturnSuccess;
        });
    if (!mapping.ok() ||
        mapping.pageCount != rn_navi33::p7_postpsp::kRuntimePages ||
        mapping.bytes != bytes) {
        if (mappingError)
            *mappingError = static_cast<uint32_t>(mapping.error);
        releaseNavi33PspRuntimeDma();
        return false;
    }

    navi33PspRuntimePrepared = true;
    return true;
}

void RedNaviProbe::releaseNavi33PmfwDma() {
    if (navi33PmfwDma) {
        navi33PmfwDma->clearMemoryDescriptor();
        navi33PmfwDma->release();
        navi33PmfwDma = nullptr;
    }
    if (navi33PmfwPages) {
        IOFree(navi33PmfwPages,
               sizeof(uint64_t) * navi33PmfwPageCount);
        navi33PmfwPages = nullptr;
    }
    if (navi33PmfwBuf) {
        navi33PmfwBuf->release();
        navi33PmfwBuf = nullptr;
    }
    navi33PmfwPageCount = 0;
    navi33PmfwStageBytes = 0;
    navi33PmfwPrepared = false;
    navi33PmfwMapped = false;
}

bool RedNaviProbe::prepareNavi33PmfwDma(
    uint64_t bytes, uint32_t *mappingError) {
    if (mappingError) *mappingError = 0;
    const uint64_t stageBytes =
        rn_navi33::p7_pmfw::alignPage(bytes);
    const uint32_t pages =
        rn_navi33::p7_pmfw::pageCount(bytes);

    if (!bytes || !stageBytes || !pages ||
        stageBytes > rn_navi33::p7_pmfw::kStageMaxBytes)
        return false;

    if (navi33PmfwPrepared && navi33PmfwBuf &&
        navi33PmfwDma && navi33PmfwPages &&
        navi33PmfwPageCount == pages &&
        navi33PmfwStageBytes == stageBytes)
        return true;

    if (navi33PmfwMapped)
        return false; // never replace backing while live PTEs exist

    releaseNavi33PmfwDma();
    navi33PmfwPageCount = pages;
    navi33PmfwStageBytes = stageBytes;

    navi33PmfwBuf = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, stageBytes, 4096);
    if (!navi33PmfwBuf ||
        !navi33PmfwBuf->getBytesNoCopy()) {
        releaseNavi33PmfwDma();
        return false;
    }
    memset(navi33PmfwBuf->getBytesNoCopy(), 0, stageBytes);

    navi33PmfwDma = IODMACommand::withSpecification(
        kIODMACommandOutputHost64,
        rn_navi33::fw_pri_owner::kDmaAddressBits,
        0, IODMACommand::kMapped,
        stageBytes, 4096, nullptr, nullptr);
    if (!navi33PmfwDma ||
        navi33PmfwDma->setMemoryDescriptor(
            navi33PmfwBuf) != kIOReturnSuccess) {
        releaseNavi33PmfwDma();
        return false;
    }

    navi33PmfwPages = static_cast<uint64_t *>(
        IOMalloc(sizeof(uint64_t) * pages));
    if (!navi33PmfwPages) {
        releaseNavi33PmfwDma();
        return false;
    }
    memset(navi33PmfwPages, 0, sizeof(uint64_t) * pages);

    const auto mapping =
        rn_dma::collectPages<IODMACommand::Segment64>(
            navi33PmfwPages, pages, stageBytes,
            [this](uint64_t *offset,
                   IODMACommand::Segment64 *segments,
                   uint32_t *count) {
                UInt64 dmaOffset = *offset;
                UInt32 dmaCount = *count;
                const IOReturn rc =
                    navi33PmfwDma->genIOVMSegments(
                        &dmaOffset, segments, &dmaCount);
                *offset = dmaOffset;
                *count = dmaCount;
                return rc == kIOReturnSuccess;
            });

    if (!mapping.ok() ||
        mapping.pageCount != pages ||
        mapping.bytes != stageBytes) {
        if (mappingError)
            *mappingError =
                static_cast<uint32_t>(mapping.error);
        releaseNavi33PmfwDma();
        return false;
    }

    navi33PmfwPrepared = true;
    return true;
}

bool RedNaviProbe::createPspRingP7(uint64_t ringGpuVa) {
    if (!navi33PspRuntimePrepared || !navi33PspRuntimeBuf ||
        p7SubmitTransportQuarantined)
        return false;
    uint32_t mp0 = 0;
    if (!ipBase(kRNHwIdMp0, &mp0)) return false;
    const uint32_t r64 = (mp0 + kRNMp0C2pmsg64) * 4;
    const uint32_t r69 = (mp0 + kRNMp0C2pmsg69) * 4;
    const uint32_t r70 = (mp0 + kRNMp0C2pmsg70) * 4;
    const uint32_t r71 = (mp0 + kRNMp0C2pmsg71) * 4;
    const uint32_t r81 = (mp0 + kRNMp0C2pmsg81) * 4;
    if (rd(r81) == 0) return false;

    uint32_t status = 0;
    for (uint32_t ms = 0; ms < 2000u; ++ms) {
        status = rd(r64);
        if ((status & kRNPspMboxMask) == kRNPspBlReady) break;
        IOSleep(1);
    }
    if ((status & kRNPspMboxMask) != kRNPspBlReady) return false;

    uint8_t *base = static_cast<uint8_t *>(
        navi33PspRuntimeBuf->getBytesNoCopy());
    memset(base, 0, kRNPspRingSize);
    __sync_synchronize();

    wr(r69, static_cast<uint32_t>(ringGpuVa));
    wr(r70, static_cast<uint32_t>(ringGpuVa >> 32));
    wr(r71, kRNPspRingSize);
    wr(r64, kRNPspRingTypeKm << 16);
    // PSP13 bootloader-to-sOS ring-create handshake needs settling time.
    IOSleep(20);

    for (uint32_t ms = 0; ms < 2000u; ++ms) {
        status = rd(r64);
        if ((status & kRNPspMboxMask) == kRNPspBlReady) break;
        IOSleep(1);
    }
    return (status & kRNPspMboxMask) == kRNPspBlReady;
}

bool RedNaviProbe::pspSubmitP7(
    uint64_t ringGpuVa, uint64_t cmdGpuVa, uint64_t fenceGpuVa,
    uint32_t cmdId, const uint32_t *args, uint32_t nargs,
    uint32_t *resp, uint32_t nresp) {
    p7SubmitWasIssued = false;
    p7SubmitResponseValid = false;
    p7SubmitTransportError = 1u; // prerequisites unavailable
    if (p7SubmitTransportQuarantined) {
        p7SubmitTransportError = 9u; // never reuse a live command after timeout
        return false;
    }
    if (!navi33PspRuntimePrepared || !navi33PspRuntimeBuf ||
        nargs > 64u || nresp > 64u ||
        (nargs && !args) || (nresp && !resp))
        return false;
    // A timed-out PSP operation has no valid response words.
    for (uint32_t n = 0; n < nresp; ++n)
        resp[n] = 0xffffffffu;

    uint32_t mp0 = 0;
    if (!ipBase(kRNHwIdMp0, &mp0)) return false;
    const uint32_t r67 = (mp0 + kRNMp0C2pmsg67) * 4;
    const uint32_t r68 = (mp0 + kRNMp0C2pmsg68) * 4;

    uint8_t *base = static_cast<uint8_t *>(
        navi33PspRuntimeBuf->getBytesNoCopy());
    uint8_t *ring = base;
    uint8_t *cmd = base + 4096;
    uint8_t *fence = base + 8192;
    memset(cmd, 0, 4096);

    // Linux clears the PSP fence buffer once before ring initialization,
    // then leaves the previous fence value in place across submissions.
    // Preserve that behavior so the next command advances 1 -> 2 rather
    // than forcing the shared fence back to zero.
    volatile uint32_t *fenceValue =
        reinterpret_cast<volatile uint32_t *>(fence);
    __sync_synchronize();
    p7SubmitFenceBefore = *fenceValue;

    *reinterpret_cast<uint32_t *>(cmd + 8) = cmdId;
    for (uint32_t n = 0; n < nargs; ++n)
        *reinterpret_cast<uint32_t *>(cmd + 28 + n * 4) = args[n];

    const uint32_t ringDw = kRNPspRingSize / 4;
    const uint32_t wptr = rd(r67);
    p7SubmitWptrBefore = wptr;
    p7SubmitWptrAfter = wptr;
    p7SubmitRptrBefore = rd(r68);
    p7SubmitRptrAfterKick = p7SubmitRptrBefore;
    p7SubmitRptrFinal = p7SubmitRptrBefore;
    p7SubmitFenceExpected = 0;
    p7SubmitFenceObserved = p7SubmitFenceBefore;
    p7SubmitPolls = 0;
    if ((wptr & 15u) || wptr >= ringDw) {
        p7SubmitTransportError = 4u;
        return false;
    }

    uint8_t *frame = ring + static_cast<uint64_t>(wptr) * 4;
    memset(frame, 0, 64);
    *reinterpret_cast<uint32_t *>(frame + 0) =
        static_cast<uint32_t>(cmdGpuVa);
    *reinterpret_cast<uint32_t *>(frame + 4) =
        static_cast<uint32_t>(cmdGpuVa >> 32);
    *reinterpret_cast<uint32_t *>(frame + 12) =
        static_cast<uint32_t>(fenceGpuVa);
    *reinterpret_cast<uint32_t *>(frame + 16) =
        static_cast<uint32_t>(fenceGpuVa >> 32);
    const uint32_t index = ++pspFence;
    p7SubmitFenceExpected = index;
    *reinterpret_cast<uint32_t *>(frame + 20) = index;
    __sync_synchronize();

    const uint32_t nextWptr = (wptr + 16u) % ringDw;
    wr(r67, nextWptr);
    p7SubmitWasIssued = true;
    p7SubmitWptrAfter = rd(r67);
    p7SubmitRptrAfterKick = rd(r68);

    uint32_t observed = p7SubmitFenceBefore;
    for (uint32_t ms = 0;
         ms < rn_navi33::p7_pmfw::kFenceTimeoutMs; ++ms) {
        __sync_synchronize();
        observed = *fenceValue;
        p7SubmitFenceObserved = observed;
        p7SubmitPolls = ms + 1u;
        if (observed == index) break;
        IOSleep(1);
    }

    // The last 1-ms sleep can complete the command after the last sample.
    if (observed != index) {
        __sync_synchronize();
        observed = *fenceValue;
        p7SubmitFenceObserved = observed;
        ++p7SubmitPolls;
    }
    p7SubmitRptrFinal = rd(r68);
    p7SubmitResponseValid = observed == index;
    if (!p7SubmitResponseValid) {
        // The PSP can still own the ring frame and command DMA page.
        // Do not overwrite either one until the GPU is reset at reboot.
        p7SubmitTransportQuarantined = true;
        p7SubmitTransportError = 8u;
        return false;
    }
    __sync_synchronize();
    for (uint32_t n = 0; n < nresp; ++n)
        resp[n] = *reinterpret_cast<volatile uint32_t *>(
            cmd + 864 + n * 4);
    p7SubmitTransportError = 0u;
    return true;
}

void RedNaviProbe::dumpPci() {
    uint32_t id = pci->configRead32(0x00);
    uint32_t sub = pci->configRead32(0x2C);
    uint8_t rev = pci->configRead8(0x08);
    IOLog("RedNavi: PCI %04x:%04x subsys %04x:%04x rev %02x\n", id & 0xFFFF, id >> 16, sub & 0xFFFF, sub >> 16, rev);

    for (uint8_t i = 0; i < 6; i++) {
        IODeviceMemory *m = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + i * 4);
        if (m) IOLog("RedNavi: BAR%u phys 0x%llx size 0x%llx\n", i, (uint64_t)m->getPhysicalAddress(), (uint64_t)m->getLength());
    }
}

void RedNaviProbe::dumpDiscovery() {
    // amdgpu waits up to 2 s for IFWI init before reading the table.
    uint32_t msg = 0;
    for (int i = 0; i < 2000; i++) {
        msg = rd(kRNMp0C2pmsg33);
        if (msg & 0x80000000u) break;
        IOSleep(1);
    }
    IOLog("RedNavi: C2PMSG_33 = 0x%08x\n", msg);

    uint32_t vramMB = rd(kRNRccConfigMemsize);
    IOLog("RedNavi: RCC_CONFIG_MEMSIZE = 0x%08x\n", vramMB);
    if (vramMB == 0 || vramMB == 0xFFFFFFFFu) {
        // Never substitute the RX 7800 XT's 16 GiB size for Navi 33.
        // The discovery location depends on physical VRAM size, so a
        // guessed address would be wrong on an RX 7600.
        if (pci->configRead16(kIOPCIConfigDeviceID) == 0x7480) {
            IOLog("RedNavi: Navi33 memsize unreadable; refusing guessed discovery address\n");
            return;
        }
        vramMB = kRNFallbackVramMB;
        IOLog("RedNavi: memsize register unusable, assuming %u MB\n", vramMB);
    }
    vramMbytes = vramMB;

    uint64_t base = ((uint64_t)vramMB << 20) - kRNDiscoveryTmrOffset;
    uint32_t sig = vramRead(base);
    IOLog("RedNavi: discovery base=0x%llx sig=0x%08x (expect 0x%08x)\n", base, sig, kRNBinarySignature);
    if (sig != kRNBinarySignature) {
        IOLog("RedNavi: IP discovery signature not found\n");
        return;
    }

    // struct binary_header: u32 signature, u16 ver_major, u16 ver_minor, u16 checksum, u16 size, then table list.
    uint32_t ver = vramRead(base + 4);
    uint32_t csumSize = vramRead(base + 8);
    uint32_t size = csumSize >> 16;
    IOLog("RedNavi: IP discovery found: version %u.%u size %u bytes\n", ver & 0xFFFF, ver >> 16, size);
    if (size == 0 || size > kRNDiscoveryTmrSize) size = kRNDiscoveryTmrSize;

    // Copy the whole table. Read it back with: ioreg -rn RedNaviProbe -l -x | grep ip-discovery
    OSData *blob = OSData::withCapacity(size);
    if (!blob) return;
    for (uint32_t off = 0; off < size; off += 4) {
        uint32_t v = vramRead(base + off);
        blob->appendBytes(&v, sizeof(v));
    }
    setProperty("ip-discovery", blob);

    // Also log it as hex lines for tools/decode_discovery.py (IOSleep keeps the log buffer from overflowing).
    const uint32_t *d = static_cast<const uint32_t *>(blob->getBytesNoCopy());
    for (uint32_t off = 0; off + 16 <= size; off += 16) {
        const uint32_t *l = d + off / 4;
        IOLog("RedNavi: DISC %04x: %08x %08x %08x %08x\n", off, l[0], l[1], l[2], l[3]);
        if ((off & 0xFF) == 0xF0) IOSleep(5);
    }
    disc = blob;
}

// Walk the discovery table (layout in include/discovery.h) and return the first base address of
// instance 0 of the given hardware ID. Every read is bounds-checked against the copied table.
bool RedNaviProbe::ipBase(uint16_t hwId, uint32_t *base, uint32_t idx) {
    if (!disc) return false;
    const uint8_t *b = static_cast<const uint8_t *>(disc->getBytesNoCopy());
    uint32_t len = disc->getLength();
    auto u16 = [&](uint32_t o) -> uint32_t { return o + 2 <= len ? static_cast<uint32_t>(b[o] | (b[o + 1] << 8)) : 0u; };
    auto u32 = [&](uint32_t o) -> uint32_t { return o + 4 <= len ? u16(o) | (u16(o + 2) << 16) : 0; };

    if (u16(4) >= 2) return false;                       // binary header v2 has another table layout
    uint32_t ipOff = u16(12);                            // table_list[0].offset (IP_DISCOVERY)
    uint32_t numDies = u16(ipOff + 12);
    bool base64 = u16(ipOff + 4) == 4 && (ipOff + 78 < len) && (b[ipOff + 78] & 1);
    if (numDies > 16) numDies = 16;

    for (uint32_t d = 0; d < numDies; d++) {
        uint32_t pos = u16(ipOff + 14 + d * 4 + 2);      // die_info[d].die_offset, absolute
        uint32_t numIps = u16(pos + 2);
        pos += 4;
        for (uint32_t i = 0; i < numIps && pos + 8 <= len; i++) {
            uint32_t nbase = b[pos + 3];
            if (u16(pos) == hwId && b[pos + 2] == 0 && idx < nbase) {
                if (base64 && u32(pos + 8 + idx * 8 + 4) != 0) return false;   // above 4 GB, not a register offset we can use
                *base = u32(pos + 8 + idx * (base64 ? 8 : 4));
                return true;
            }
            pos += 8 + nbase * (base64 ? 8 : 4);
        }
    }
    return false;
}

void RedNaviProbe::dumpRegs(const RNMailboxReg *list, size_t count, const char *property) {
    OSDictionary *out = OSDictionary::withCapacity(16);
    if (!out) return;
    uint32_t regLen = static_cast<uint32_t>(regs->getLength());

    for (size_t i = 0; i < count; i++) {
        const RNMailboxReg &r = list[i];
        uint32_t base = 0;
        if (!ipBase(r.hwId, &base, r.baseIdx)) {
            IOLog("RedNavi: %s: hardware ID %u base %u not in discovery table\n", r.name, r.hwId, r.baseIdx);
            continue;
        }
        uint32_t off = (base + r.dword) * 4;
        if (off + 4 > regLen) {
            IOLog("RedNavi: %s: byte offset 0x%x outside the %u byte register BAR\n", r.name, off, regLen);
            continue;
        }
        uint32_t v = rd(off);
        IOLog("RedNavi: %s @0x%05x = 0x%08x\n", r.name, off, v);
        OSNumber *n = OSNumber::withNumber(static_cast<uint64_t>(v), 64);
        if (n) { out->setObject(r.name, n); n->release(); }
    }
    setProperty(property, out);
    out->release();
}

// Copy one component into the VRAM staging buffer and run the bootloader handshake.
// Read the legacy x86 image of the PCI expansion ROM (amdgpu_read_bios / amdgpu_read_platform_bios).
void RedNaviProbe::dumpVbios() {
    uint32_t romReg = pci->configRead32(0x30);
    IODeviceMemory *rom = pci->getDeviceMemoryWithRegister(0x30);
    IOLog("RedNavi: ROM BAR register = 0x%08x, device memory %s\n", romReg, rom ? "present" : "absent");
    if (!rom) return;
    IOLog("RedNavi: ROM phys 0x%llx size 0x%llx\n", (uint64_t)rom->getPhysicalAddress(), (uint64_t)rom->getLength());

    pci->configWrite32(0x30, romReg | 1);                    // expansion ROM enable
    IOMemoryMap *map = pci->mapDeviceMemoryWithRegister(0x30);
    if (!map) { IOLog("RedNavi: could not map the expansion ROM\n"); pci->configWrite32(0x30, romReg); return; }

    const volatile uint8_t *p = reinterpret_cast<const volatile uint8_t *>(map->getVirtualAddress());
    uint32_t mapLen = static_cast<uint32_t>(map->getLength());
    uint32_t sig = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
    uint32_t size = static_cast<uint32_t>(p[2]) * 512;       // legacy image size in 512 byte units
    IOLog("RedNavi: ROM signature 0x%04x (expect 0xaa55), first image %u bytes, mapping %u bytes\n", sig, size, mapLen);
    if (sig == 0xAA55 && size && size <= mapLen && size <= (1u << 20)) {
        OSData *blob = OSData::withCapacity(size);
        if (blob) {
            for (uint32_t i = 0; i < size; i++) { uint8_t b = p[i]; blob->appendBytes(&b, 1); }
            setProperty("vbios", blob);
            vbiosCopy = blob;
            IOLog("RedNavi: VBIOS image of %u bytes published\n", size);
        }
    }
    map->release();
    pci->configWrite32(0x30, romReg);                        // leave the ROM decode as we found it
}

// One AUX transaction on engine `eng`, mirroring acquire_engine / submit_channel_request / read_channel_reply in dce_aux.c.
// action is the command nibble already shifted left by 4 (DP read 0x90, I2C write+MOT 0x40, I2C read 0x10, I2C read+MOT 0x50).
bool RedNaviProbe::auxTransfer(uint32_t eng, uint32_t action, uint32_t addr, uint8_t *data, uint32_t len, uint32_t *status, uint32_t *reply) {
    uint32_t dmu = 0;
    if (!ipBase(kRNHwIdDmu, &dmu, 2)) return false;
    uint32_t base = dmu + 0x1f50 + eng * 0x1c;           // DP_AUXn_AUX_CONTROL
    auto R = [&](uint32_t d) { return rd((base + d) * 4); };
    auto W = [&](uint32_t d, uint32_t v) { wr((base + d) * 4, v); };
    const uint32_t CONTROL = 0, SWCTL = 1, ARB = 2, INTC = 3, STAT = 4, DATA = 6;
    bool isWrite = (action == 0x80) || (action == 0x00) || (action == 0x40);
    *status = 0; *reply = 0xFF;

    if (!(R(CONTROL) & 1)) W(CONTROL, R(CONTROL) | 1);                    // AUX_EN
    W(ARB, R(ARB) | (1u << 16));                                           // AUX_SW_USE_AUX_REG_REQ
    if (((R(ARB) >> 2) & 3) != 1) { *status = R(ARB); W(ARB, (R(ARB) & ~(1u << 16)) | (1u << 17)); return false; }   // SW_CAN_ACCESS_AUX

    W(INTC, R(INTC) | (1u << 1));                                          // AUX_SW_DONE_ACK
    for (int i = 0; i < 100 && (R(STAT) & 1); i++) IODelay(100);

    uint32_t wr_bytes = (len ? 4 : 3) + (isWrite ? len : 0);
    W(SWCTL, (R(SWCTL) & ~((0xFu << 4) | (0xFFFFu << 16))) | (0u << 4) | (wr_bytes << 16));
    // AUX_SW_INDEX 0, AUX_SW_DATA_RW 0, autoincrement disabled for the first byte only
    uint32_t v = (1u << 31) | ((action | ((addr >> 16) & 0xF)) << 8);
    W(DATA, v);
    v &= ~(1u << 31);
    W(DATA, (v & 0xFFFF00FFu) | (((addr >> 8) & 0xFF) << 8));
    W(DATA, (v & 0xFFFF00FFu) | ((addr & 0xFF) << 8));
    if (len) W(DATA, (v & 0xFFFF00FFu) | (((len - 1) & 0xFF) << 8));
    if (isWrite) for (uint32_t i = 0; i < len; i++) W(DATA, (v & 0xFFFF00FFu) | (static_cast<uint32_t>(data[i]) << 8));
    W(SWCTL, R(SWCTL) | 1);                                                // AUX_SW_GO

    uint32_t st = 0;
    bool done = false;
    for (int i = 0; i < 2000; i++) { st = R(STAT); if (st & 1) { done = true; break; } IODelay(100); }
    *status = st;
    bool ok = false;
    uint32_t count = (st >> 24) & 0x1F;
    const uint32_t bad = (1u << 7) | (7u << 4) | (1u << 9) | (1u << 14) | (1u << 20) | (1u << 22) | (1u << 23);   // timeouts, HPD disconnect, invalid reply bits
    if (done && !(st & bad) && count) {
        W(DATA, 0);                                                        // AUX_SW_INDEX = 0
        W(DATA, 1u << 31);                                                 // AUX_SW_AUTOINCREMENT_DISABLE
        W(DATA, (1u << 31) | 1u);                                          // AUX_SW_DATA_RW = 1
        uint32_t first = (R(DATA) >> 8) & 0xFF;
        *reply = first >> 4;
        if (*reply == 0) {
            uint32_t n = count - 1;
            for (uint32_t i = 0; i < n && i < len; i++) data[i] = (R(DATA) >> 8) & 0xFF;
            ok = true;
        }
    }
    W(ARB, (R(ARB) & ~(1u << 16)) | (1u << 17));                           // release: DONE_USING_AUX_REG, clear USE request
    return ok;
}

// Queue one 64-byte command in inbox1 and wait for the firmware to consume it (dmub_srv_cmd_queue / _execute).
bool RedNaviProbe::dmubSend(const uint8_t *cmd, uint32_t len, uint32_t reply[16]) {
    if (!dmubUp || len > 64) return false;
    const uint32_t IN_WP = 0x1d6, IN_RP = 0x1d7;
    uint32_t wp = rd((dmubDmu + IN_WP) * 4), slot = wp % kRNDmubRbSize;
    uint64_t slotAddr = dmubMailbox + slot;
    for (uint32_t i = 0; i < 64; i += 4) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < 4; k++) if (i + k < len) v |= static_cast<uint32_t>(cmd[i + k]) << (8 * k);
        vramWrite(slotAddr + i, v);
    }
    uint32_t newWp = (wp + 64) % kRNDmubRbSize;
    wr((dmubDmu + IN_WP) * 4, newWp);
    uint32_t rp = 0;
    for (int i = 0; i < 1000; i++) { rp = rd((dmubDmu + IN_RP) * 4); if (rp == newWp) break; IOSleep(1); }
    for (uint32_t i = 0; i < 16; i++) reply[i] = vramRead(slotAddr + i * 4);
    return rp == newWp;
}

// VBIOS command tables executed by the DMCUB (DMUB_CMD__VBIOS), the way dc/bios/command_table2.c does when
// dc->debug.dmub_command_table is set. Here: DIG1_TRANSMITTER_CONTROL with action INIT for path 0
// (DisplayPort, UNIPHY2 enum 1 = transmitter E = phy id 4, HPD pin 5).
void RedNaviProbe::dpInit() {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_dp_init", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: DP transmitter init not requested (boot-arg rednavi_dp_init=1)\n");
        return;
    }
    if (!dmubUp) { IOLog("RedNavi: DP init: DMCUB not running\n"); return; }
    uint8_t cmd[64] = {};
    // header: type DMUB_CMD__VBIOS (128), sub_type DIG1_TRANSMITTER_CONTROL (1), payload 16 bytes (<< 24)
    cmd[0] = 128; cmd[1] = 1; cmd[3] = 16;
    // struct dig_transmitter_control_parameters_v1_6 at offset 4
    cmd[4 + 0] = 4;       // phyid: UNIPHY E
    cmd[4 + 1] = 7;       // ATOM_TRANSMITTER_ACTION_INIT
    cmd[4 + 2] = 0;       // digmode: DP
    cmd[4 + 3] = 4;       // lanenum
    cmd[4 + 8] = 5;       // hpdsel: HPD5 (pin id 5)
    cmd[4 + 9] = 0;       // digfe_sel
    cmd[4 + 10] = 19;     // connector object id: DisplayPort
    uint32_t reply[16];
    bool ok = dmubSend(cmd, 20, reply);
    IOLog("RedNavi: DP transmitter INIT via DMUB: %s, slot %08x %08x %08x %08x %08x\n", ok ? "consumed" : "NOT consumed", reply[0], reply[1], reply[2], reply[3], reply[4]);
    const char *n[3] = {"consumed", "slot0", "slot1"};
    uint32_t v[3] = {ok, reply[0], reply[1]};
    rnPublish(this, "dp-init", n, v, 3);
}

bool RedNaviProbe::dpcdWrite(uint32_t eng, uint32_t addr, const uint8_t *d, uint32_t n) {
    uint8_t buf[16];
    if (n > 16) return false;
    memcpy(buf, d, n);
    uint32_t st, rep;
    return auxTransfer(eng, 0x80, addr, buf, n, &st, &rep);
}

bool RedNaviProbe::dpcdRead(uint32_t eng, uint32_t addr, uint8_t *d, uint32_t n) {
    uint32_t st, rep;
    return auxTransfer(eng, 0x90, addr, d, n, &st, &rep);
}

// DIG1_TRANSMITTER_CONTROL for path 0 (phy id 4, HPD 5, connector id 19) through the DMCUB.
bool RedNaviProbe::dpTx(uint8_t action, uint8_t modeOrLane, uint8_t lanes, uint32_t symclk10k) {
    uint8_t cmd[64] = {};
    cmd[0] = 128; cmd[1] = 1; cmd[3] = 16;
    cmd[4 + 0] = 4; cmd[4 + 1] = action; cmd[4 + 2] = modeOrLane; cmd[4 + 3] = lanes;
    memcpy(cmd + 4 + 4, &symclk10k, 4);
    cmd[4 + 8] = 5; cmd[4 + 9] = 0; cmd[4 + 10] = 19;
    uint32_t reply[16];
    return dmubSend(cmd, 20, reply);
}

void RedNaviProbe::dpTrain() {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_dp_train", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: DP link training not requested (boot-arg rednavi_dp_train=1)\n");
        return;
    }
    if (!dmubUp) { IOLog("RedNavi: DP training: DMCUB not running\n"); return; }
    // Link rate: RBR (0x06, 162000 kHz symbol clock) like Linux uses for 1080p60 8 bpc, which is also the state the replay table
    // was captured in (DP_DTO0, VID_M/N); rednavi_dp_rate=0x14 selects HBR2 as before.
    uint32_t rateArg = selRate;                                             // from the selected mode (RBR for 1080p60)
    PE_parse_boot_argn("rednavi_dp_rate", &rateArg, sizeof(rateArg));
    const uint32_t aux = 4, lanes = 4, rateCode = rateArg, symclk10k = rateCode * 27000 / 10;
    uint32_t dmu = dmubDmu;
    // DP4 back end and DIG4 registers (dcn_3_2_0_offset.h, base index 2)
    const uint32_t DP_CONFIG = 0x250b, DP_LINK_CNTL = 0x2508, DP_DPHY_CNTL = 0x2517, DP_PATTERN_SEL = 0x2518, DP_SCRAM = 0x251e;
    const uint32_t DIG_BE_CNTL = 0x24b1;
    auto R = [&](uint32_t d) { return rd((dmu + d) * 4); };
    auto W = [&](uint32_t d, uint32_t v) { wr((dmu + d) * 4, v); };
    auto U = [&](uint32_t d, uint32_t mask, uint32_t v) { W(d, (R(d) & ~mask) | (v & mask)); };

    uint8_t caps[16] = {};
    if (!dpcdRead(aux, 0, caps, 16)) { IOLog("RedNavi: DP training: no DPCD\n"); return; }
    bool tps3 = (caps[2] & 0x40) != 0 && rateCode >= 0x14, enhanced = (caps[2] & 0x80) != 0;

    // Sink and source setup (dp_enable_link_phy, enc1_configure_encoder, dcn10_link_encoder_setup)
    uint8_t d0 = 1;
    dpcdWrite(aux, 0x600, &d0, 1);                                          // SET_POWER: D0
    U(DIG_BE_CNTL, 0xFu << 16, 0);                                          // DIG_MODE = DP
    U(DP_CONFIG, 3, lanes - 1);                                             // DP_UDI_LANES
    U(DP_SCRAM, 1u << 4, 1u << 4);                                          // DPHY_SCRAMBLER_ADVANCE
    uint8_t link[2] = {static_cast<uint8_t>(rateCode), static_cast<uint8_t>(lanes | (enhanced ? 0x80 : 0))};
    dpcdWrite(aux, 0x100, link, 2);                                         // LINK_BW_SET, LANE_COUNT_SET
    uint8_t ds = 0; dpcdWrite(aux, 0x107, &ds, 1);                          // no downspread
    uint8_t enc = 1; dpcdWrite(aux, 0x108, &enc, 1);                        // 8b/10b
    bool txOk = dpTx(1, 0, static_cast<uint8_t>(lanes), symclk10k);          // ATOM_TRANSMITTER_ACTION_ENABLE
    IOLog("RedNavi: DP training: transmitter ENABLE %s, DIG4 BE_CNTL=0x%08x, link %02x %02x\n", txOk ? "consumed" : "NOT consumed", R(DIG_BE_CNTL), link[0], link[1]);

    uint8_t vs[4] = {0, 0, 0, 0}, pe[4] = {0, 0, 0, 0};
    uint8_t st[6] = {};
    bool crOk = false, eqOk = false;
    uint32_t crTries = 0, eqTries = 0;
    auto laneSet = [&](uint32_t i) -> uint8_t { return static_cast<uint8_t>(vs[i] | (pe[i] << 3) | (vs[i] == 3 ? 4 : 0) | (pe[i] == 3 ? 0x20 : 0)); };
    auto applyDrive = [&](void) {
        for (uint32_t i = 0; i < lanes; i++) dpTx(11, laneSet(i), static_cast<uint8_t>(lanes), symclk10k);   // SETUP_VSEMPH
    };
    auto readStatus = [&]() { dpcdRead(aux, 0x202, st, 6); };
    auto adjust = [&]() {
        for (uint32_t i = 0; i < lanes; i++) {
            uint8_t req = st[4 + i / 2];
            uint8_t sh = (i & 1) ? 4 : 0;
            vs[i] = (req >> sh) & 3;
            pe[i] = (req >> (sh + 2)) & 3;
            if (vs[i] + pe[i] > 3) pe[i] = 3 - vs[i];
        }
    };

    // Clock recovery: TPS1
    U(DP_PATTERN_SEL, 3, 0);
    U(DP_LINK_CNTL, 1u << 4, 0);                                            // training not complete
    U(DP_DPHY_CNTL, 1u << 16, 0);                                           // PHY bypass off: send the training pattern
    for (crTries = 1; crTries <= 8 && !crOk; crTries++) {
        applyDrive();
        uint8_t tr[5] = {0x21, laneSet(0), laneSet(1), laneSet(2), laneSet(3)};   // TPS1, scrambling disabled, lane drive
        dpcdWrite(aux, 0x102, tr, 5);
        IOSleep(1);                                                         // TRAINING_AUX_RD_INTERVAL: 100 us minimum
        readStatus();
        crOk = (st[0] & 1) && (st[0] & 0x10) && (st[1] & 1) && (st[1] & 0x10);
        if (!crOk) adjust();
    }
    if (crOk) {
        // Channel equalisation: TPS3 when the sink has it (HBR2), else TPS2
        U(DP_PATTERN_SEL, 3, tps3 ? 2 : 1);
        uint8_t pat = tps3 ? 0x23 : 0x22;
        for (eqTries = 1; eqTries <= 6 && !eqOk; eqTries++) {
            applyDrive();
            uint8_t tr[5] = {pat, laneSet(0), laneSet(1), laneSet(2), laneSet(3)};
            dpcdWrite(aux, 0x102, tr, 5);
            IOSleep(1);                                                     // 400 us default interval
            readStatus();
            bool cr = (st[0] & 1) && (st[0] & 0x10) && (st[1] & 1) && (st[1] & 0x10);
            bool eq = (st[0] & 2) && (st[0] & 0x20) && (st[1] & 2) && (st[1] & 0x20);
            bool sl = (st[0] & 4) && (st[0] & 0x40) && (st[1] & 4) && (st[1] & 0x40);
            eqOk = cr && eq && sl && (st[2] & 1);
            if (!eqOk) adjust();
        }
    }
    uint8_t off = 0;
    dpcdWrite(aux, 0x102, &off, 1);                                         // training pattern off
    if (crOk && eqOk) U(DP_LINK_CNTL, 1u << 4, 1u << 4);                    // DP_LINK_TRAINING_COMPLETE
    readStatus();
    IOLog("RedNavi: DP training: CR %s after %u tries, EQ %s after %u tries; DPCD 202..207 = %02x %02x %02x %02x %02x %02x; drive vs %u%u%u%u pe %u%u%u%u\n",
          crOk ? "done" : "FAILED", crTries, eqOk ? "done" : "FAILED", eqTries, st[0], st[1], st[2], st[3], st[4], st[5],
          vs[0], vs[1], vs[2], vs[3], pe[0], pe[1], pe[2], pe[3]);
    const char *n[9] = {"tx_ok", "cr_ok", "eq_ok", "cr_tries", "eq_tries", "status_202_203", "status_204_205", "adjust_206_207", "tps3"};
    uint32_t v[9] = {txOk, crOk, eqOk, crTries, eqTries, static_cast<uint32_t>(st[0] | (st[1] << 8)), static_cast<uint32_t>(st[2] | (st[3] << 8)), static_cast<uint32_t>(st[4] | (st[5] << 8)), tps3};
    rnPublish(this, "dp-train", n, v, 9);
}

// What the screen shows from the moment the pipe is up until WindowServer draws the desktop: the RedNavi splash (assets/splash_1920x1080.png) in modes it
// exists for, or with boot-arg rednavi_testpattern=1 the colour test pattern below. A screen that stays on this image is the sign that the boot did not go on.
//
// The test pattern is a recognisable XRGB8888 image: red/green gradient, a checker in blue, a white frame and
// eight colour bars along the bottom, so a wrong pitch, offset or format is obvious on screen.
void RedNaviProbe::fillScanout() {
    static const uint32_t bars[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};
    const uint32_t W = selMode.hactive, H = selMode.vactive;
    uint32_t testPattern = 0;
    PE_parse_boot_argn("rednavi_testpattern", &testPattern, sizeof(testPattern));
    if (testPattern != 1 && W == kSplash1920x1080Width && H == kSplash1920x1080Height) {
        uint32_t x = 0, y = 0;
        for (uint32_t r = 0; r < kSplash1920x1080Runs; r++) {
            const uint32_t v = kSplash1920x1080Colour[r];
            for (uint32_t n = kSplash1920x1080Length[r]; n; n--) {
                vramWrite(kRNScanoutVramOff + (static_cast<uint64_t>(y) * selPitch + x) * 4, v);
                if (++x == W) { x = 0; y++; if ((y & 63) == 0) IOSleep(1); }
            }
        }
        return;
    }
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint32_t v;
            if (x < 8 || x + 8 >= W || y < 8 || y + 8 >= H) v = 0xFFFFFF;
            else if (y >= H - H / 6) v = bars[(x * 8) / W];
            else {
                uint32_t r = (x * 255) / W, g = (y * 255) / H, b = (((x >> 4) + (y >> 4)) & 1) ? 200 : 60;
                v = (r << 16) | (g << 8) | b;
            }
            vramWrite(kRNScanoutVramOff + (static_cast<uint64_t>(y) * selPitch + x) * 4, v);
        }
        if ((y & 63) == 0) IOSleep(1);
    }
}

// Opt-in (rednavi_smu=1): first talk to the SMU through the compat layer. Only read-style messages: the
// test message, the firmware version and the driver interface version. Published as "smu-info".
void RedNaviProbe::smuProbe(const char *property, bool send) {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_smu", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: SMU messages not requested (boot-arg rednavi_smu=1)\n");
        return;
    }
    uint32_t mp1 = 0;
    if (!ipBase(kRNHwIdMp1, &mp1)) { IOLog("RedNavi: SMU: no MP1 base\n"); return; }

    struct rn_device dev;
    struct rn_device *adev = &dev;       // the RREG32 macros expect a pointer called adev
    rn_device_init(adev, "RedNavi SMU", reinterpret_cast<volatile void *>(regs->getVirtualAddress()), regs->getLength());
    adev->reg_offset[RN_MP1_HWIP][0][0] = mp1;

    OSDictionary *out = OSDictionary::withCapacity(12);
    if (!out) return;
    rnPublishOne(out, "mp1_base", mp1);
    // MP1_FIRMWARE_FLAGS (SMN 0x3b10024: MP1_Public | smnMP1_FIRMWARE_FLAGS) through the NBIO RSMU index/data pair;
    // bit 0 is INTERRUPTS_ENABLED, which smu_v13_0_check_fw_status wants set before messages are sent.
    uint32_t nbif = 0;
    if (ipBase(108, &nbif, 1)) {
        wr((nbif + 0) * 4, 0x03b10024u);
        rnPublishOne(out, "fw_flags", rd((nbif + 1) * 4));
        rnPublishOne(out, "nbif_base1", nbif);
    }
    rnPublishOne(out, "c2pmsg_66_before", RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_66));
    rnPublishOne(out, "c2pmsg_82_before", RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_82));
    rnPublishOne(out, "c2pmsg_90_before", RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90));
    // The whole MP1 mailbox block and the PSP state, to compare with docs/logs/linux-reference/smu_mailbox_normal.txt.
    for (uint32_t n = 66; n <= 90; n++) {
        char key[24];
        snprintf(key, sizeof(key), "m1_%u", n);
        rnPublishOne(out, key, rd((mp1 + 0x282 + (n - 66)) * 4));
    }
    uint32_t mp0 = 0;
    if (ipBase(kRNHwIdMp0, &mp0)) {
        rnPublishOne(out, "m0_33", rd((mp0 + 0x61) * 4));
        rnPublishOne(out, "m0_35", rd((mp0 + 0x63) * 4));
        rnPublishOne(out, "m0_81", rd((mp0 + 0x91) * 4));
    }
    static const struct { const char *name; uint32_t msg; uint32_t arg; } msgs[] = {
        {"smu_version", PPSMC_MSG_GetSmuVersion, 0},
        {"driver_if_version", PPSMC_MSG_GetDriverIfVersion, 0},
        {"test", PPSMC_MSG_TestMessage, 0x12345678},
    };
    for (const auto &m : msgs) {
        if (!send) break;                    // read-only call: only the registers above
        uint32_t result = 0, resp = 0;
        int rc = rn_smu13_send(adev, m.msg, m.arg, &result, &resp, 100000);
        IOLog("RedNavi: SMU %s: rc %d, response 0x%x, argument 0x%08x\n", m.name, rc, resp, result);
        char key[40];
        snprintf(key, sizeof(key), "%s_rc", m.name);
        rnPublishOne(out, key, static_cast<uint64_t>(static_cast<uint32_t>(rc)));
        snprintf(key, sizeof(key), "%s_resp", m.name);
        rnPublishOne(out, key, resp);
        snprintf(key, sizeof(key), "%s_arg", m.name);
        rnPublishOne(out, key, result);
        if (rc == -ETIMEDOUT) WREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90, 1);   // diagnostic only: let the next message try
    }
    rnPublishOne(out, "c2pmsg_90_after", RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90));
    // Experiment (boot-arg rednavi_smu_enable=1): Linux's smu_smc_hw_setup ends with EnableAllSmuFeatures; the RLC autoload may
    // wait for it. Allowed mask first (smu_feature_set_allowed_mask), then the enable, then the features that are running.
    uint32_t en = 0;
    if (send && PE_parse_boot_argn("rednavi_smu_enable", &en, sizeof(en)) && en == 1) {
        // smu_smc_hw_setup does these before it enables the features: tell the SMU where the driver and tool tables are (in VRAM, as
        // in Linux), then run the DC BTC. The power firmware stopped answering messages a few seconds after the enable without them.
        const uint64_t drvMc = kRNVramMcBase + 0x0C300000ull, toolMc = kRNVramMcBase + 0x0C320000ull;
        for (uint32_t o = 0; o < 0x40000; o += 4) vramWrite(0x0C300000ull + o, 0);
        struct { const char *name; uint32_t msg; uint32_t arg; } steps[] = {
            {"drv_hi", PPSMC_MSG_SetDriverDramAddrHigh, static_cast<uint32_t>(drvMc >> 32)},
            {"drv_lo", PPSMC_MSG_SetDriverDramAddrLow, static_cast<uint32_t>(drvMc)},
            {"tool_hi", PPSMC_MSG_SetToolsDramAddrHigh, static_cast<uint32_t>(toolMc >> 32)},
            {"tool_lo", PPSMC_MSG_SetToolsDramAddrLow, static_cast<uint32_t>(toolMc)},
            {"run_btc", PPSMC_MSG_RunDcBtc, 0},
            {"allow_hi", PPSMC_MSG_SetAllowedFeaturesMaskHigh, 0xFFFFFFFFu},
            {"allow_lo", PPSMC_MSG_SetAllowedFeaturesMaskLow, 0xFFFFFFFFu},
            {"enable_all", PPSMC_MSG_EnableAllSmuFeatures, 0},
            {"running_lo", PPSMC_MSG_GetRunningSmuFeaturesLow, 0},
            {"running_hi", PPSMC_MSG_GetRunningSmuFeaturesHigh, 0},
            {"disallow_gfxoff", PPSMC_MSG_DisallowGfxOff, 0},
        };
        // SDMA0_VERSION reads 0x600 on a healthy card and 0 while the graphics block is power gated (GFXOFF); sample it around the
        // messages to see when it goes dark. absolute dword 0x1295 (GC base 0x1260 + 0x35, as in the Linux reference in docs/logs).
        const uint32_t sdmaVersion = 0x1295 * 4;
        rnPublishOne(out, "sdma_version_before_enable", rd(sdmaVersion));
        for (auto &st : steps) {
            uint32_t result = 0, resp = 0;
            int rc = rn_smu13_send(adev, st.msg, st.arg, &result, &resp, 3000000);   // RunDcBtc can take a while
            IOLog("RedNavi: SMU %s: rc %d, response 0x%x, argument 0x%08x\n", st.name, rc, resp, result);
            char key[40];
            snprintf(key, sizeof(key), "%s_rc", st.name); rnPublishOne(out, key, static_cast<uint64_t>(static_cast<uint32_t>(rc)));
            snprintf(key, sizeof(key), "%s_arg", st.name); rnPublishOne(out, key, result);
            snprintf(key, sizeof(key), "%s_sdma_version", st.name); rnPublishOne(out, key, rd(sdmaVersion));
            if (rc == -ETIMEDOUT) break;
        }
        // Opt-in (rednavi_smu_setup=1): the rest of what Linux's smu_smc_hw_setup/smu_hw_init/smu_late_init send after the features
        // are on and that we did not: the combo pptable transfer, the DPM tables (level count and frequency of every clock), the power
        // source. With SCPM the allowed mask is rejected by the firmware, as above, so Linux does not send it either.
        uint32_t setup = 0;
        if (PE_parse_boot_argn("rednavi_smu_setup", &setup, sizeof(setup)) && setup == 1) {
            struct { const char *name; uint32_t msg; uint32_t arg; } tail[] = {
                {"combo_pptable", PPSMC_MSG_TransferTableSmu2Dram, 1},   // TABLE_COMBO_PPTABLE, argument 0 in the upper half
            };
            for (auto &st : tail) {
                uint32_t result = 0, resp = 0;
                int rc = rn_smu13_send(adev, st.msg, st.arg, &result, &resp, 3000000);
                char key[40];
                snprintf(key, sizeof(key), "setup_%s_rc", st.name); rnPublishOne(out, key, static_cast<uint64_t>(static_cast<uint32_t>(rc)));
                snprintf(key, sizeof(key), "setup_%s_arg", st.name); rnPublishOne(out, key, result);
                if (rc == -ETIMEDOUT) break;
            }
            // PPCLK_e: GFX 0, SOC 1, UCLK 2, FCLK 3, DCLK_0 4, VCLK_0 5, DCFCLK 11. GetDpmFreqByIndex with level 0xff is the level count.
            static const struct { const char *name; uint32_t id; } clks[] = {
                {"soc", 1}, {"gfx", 0}, {"uclk", 2}, {"fclk", 3}, {"vclk", 5}, {"dclk", 4}, {"dcef", 11},
            };
            bool dead = false;
            for (const auto &c : clks) {
                if (dead) break;
                uint32_t count = 0, resp = 0;
                int rc = rn_smu13_send(adev, PPSMC_MSG_GetDpmFreqByIndex, (c.id << 16) | 0xff, &count, &resp, 1000000);
                char key[40];
                snprintf(key, sizeof(key), "setup_%s_count_rc", c.name); rnPublishOne(out, key, static_cast<uint64_t>(static_cast<uint32_t>(rc)));
                snprintf(key, sizeof(key), "setup_%s_count", c.name); rnPublishOne(out, key, count);
                if (rc == -ETIMEDOUT) { dead = true; break; }
                if (rc || (count & 0x7fffffff) > 16) continue;
                for (uint32_t lvl = 0; lvl < (count & 0x7fffffff); lvl++) {
                    uint32_t mhz = 0;
                    rc = rn_smu13_send(adev, PPSMC_MSG_GetDpmFreqByIndex, (c.id << 16) | lvl, &mhz, &resp, 1000000);
                    snprintf(key, sizeof(key), "setup_%s_%u", c.name, lvl); rnPublishOne(out, key, rc ? 0xdead0000u | (rc & 0xffff) : (mhz & 0x7fffffff));
                    if (rc == -ETIMEDOUT) { dead = true; break; }
                }
            }
            if (!dead) {
                uint32_t result = 0, resp = 0;
                int rc = rn_smu13_send(adev, PPSMC_MSG_NotifyPowerSource, 0, &result, &resp, 1000000);   // POWER_SOURCE_AC
                rnPublishOne(out, "setup_power_source_rc", static_cast<uint64_t>(static_cast<uint32_t>(rc)));
            }
            rnPublishOne(out, "setup_sdma_version", rd(sdmaVersion));
        }
        // When does the block go dark again? Only SDMA0_VERSION and the mailbox response are read, so nothing else can disturb it.
        const uint32_t waits[] = {0, 100, 400};   // the block was still alive 7 s later; the SMU stops answering some seconds after that
        for (uint32_t w : waits) {
            IOSleep(w);
            char key[40];
            snprintf(key, sizeof(key), "tick_%u_sdma_version", w); rnPublishOne(out, key, rd(sdmaVersion));
            snprintf(key, sizeof(key), "tick_%u_c2pmsg_90", w); rnPublishOne(out, key, RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90));
        }
        gcStatus(out, "after_enable_", 2000);
    }
    setProperty(property, out);
    out->release();
}

// Opt-in (rednavi_fb=1): tell the RedNaviFB kext (a separate bundle, see RedNaviFB/kern_fb.cpp) where the lit
// scanout surface is. It matches on rednavi-fb-ready, so that property has to be set last.
void RedNaviProbe::publishFramebuffer() {
    uint32_t arg = 0, replay = 0;
    if (!PE_parse_boot_argn("rednavi_fb", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: framebuffer not requested (boot-arg rednavi_fb=1)\n");
        return;
    }
    if (!PE_parse_boot_argn("rednavi_dcn_replay", &replay, sizeof(replay)) || replay < 14) {
        IOLog("RedNavi: framebuffer needs rednavi_dcn_replay=14, skipping\n");
        return;
    }
    IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + kRNVramBar * 4);
    if (!bar0) { IOLog("RedNavi: framebuffer: no BAR0\n"); return; }
    uint64_t phys = static_cast<uint64_t>(bar0->getPhysicalAddress());
    uint64_t len = static_cast<uint64_t>(bar0->getLength());
    IOLog("RedNavi: framebuffer: BAR0 phys 0x%llx, %llu MB\n", phys, len >> 20);
    uint64_t need = static_cast<uint64_t>(selPitch) * 4 * selMode.vactive;
    if (kRNScanoutVramOff + need > len) { IOLog("RedNavi: framebuffer: mode does not fit in BAR0\n"); return; }
    struct FbMode { uint32_t w, h, refresh, probeIndex; } list[RN_EDID_MAX_MODES];
    unsigned nModes = 0;
    for (unsigned i = 0; i < edidInfo.mode_count; i++) {                        // modes the link can carry and that fit the scanout area
        const rn_mode &m = edidInfo.modes[i];
        int rate = rn_min_dp_link_rate(&m, 4);
        if (rate < 0 || rate > 0x14) continue;
        if (kRNScanoutVramOff + static_cast<uint64_t>(rn_pitch_pixels(m.hactive)) * 4 * m.vactive > len) continue;
        list[nModes++] = {m.hactive, m.vactive, m.refresh_hz, i};
    }
    if (nModes) {
        OSData *d = OSData::withBytes(list, nModes * sizeof(FbMode));
        if (d) { setProperty("fb-modes", d); d->release(); }
    }
    setProperty("fb-current-mode", static_cast<uint64_t>(selIndex < 0 ? 0 : selIndex), 32);
    setProperty("fb-width", selMode.hactive, 32);
    setProperty("fb-height", selMode.vactive, 32);
    setProperty("fb-pitch", selPitch * 4, 32);
    setProperty("fb-refresh", selMode.refresh_hz, 32);
    { OSData *t = OSData::withBytes(&selMode, sizeof(selMode)); if (t) { setProperty("fb-timing", t); t->release(); } }   // struct rn_mode of the mode in use: the graphics control answers (command 0x921) read it
    setProperty("fb-scanout-phys", phys + kRNScanoutVramOff, 64);
    fbReady = true;
    setProperty("rednavi-fb-ready", kOSBooleanTrue);
}

void RedNaviProbe::dcnReplay() {
    uint32_t maxGroup = 0;
    if (!PE_parse_boot_argn("rednavi_dcn_replay", &maxGroup, sizeof(maxGroup)) || maxGroup == 0) return;
    if (maxGroup > 14) maxGroup = 14;
    uint32_t base[4] = {};
    for (uint32_t i = 0; i < 4; i++) if (!ipBase(kRNHwIdDmu, &base[i], i)) { IOLog("RedNavi: replay: no DMU base %u\n", i); return; }
    uint32_t regLen = static_cast<uint32_t>(regs->getLength());
    // State found before we touch anything (the VBIOS may have lit the pipe already), then quiesce it:
    // DC assumes the display is disabled before it programs the timing.
    {
        const uint32_t pre[16][2] = {{2, 0x1b41}, {2, 0x1b2a}, {2, 0x1b2f}, {2, 0x250c}, {2, 0x248b}, {2, 0x24b1}, {2, 0x24b2}, {2, 0x2511}, {2, 0x2512}, {1, 0x81}, {1, 0x64}, {1, 0x99}, {1, 0xb6}, {2, 0x475}, {2, 0x476}, {2, 0x46f}};
        const char *pn[16] = {"otg_control", "h_total", "v_total", "vid_stream_cntl", "fe_cntl", "be_cntl", "be_en", "vid_n", "vid_m", "dp_dto0_phase", "dentist_dispclk", "dppclk0_dto", "dppclk_dto_ctrl", "vm_fb_base", "vm_fb_top", "sdpif_cfg0"};
        uint32_t pv[16];
        for (int i = 0; i < 16; i++) pv[i] = rd((base[pre[i][0]] + pre[i][1]) * 4);
        RN_STEP("RedNavi: replay before: OTG_CONTROL=0x%08x H_TOTAL=0x%x V_TOTAL=0x%x VID_STREAM=0x%08x FE=0x%08x BE=0x%08x BE_EN=0x%08x VID_N=0x%x VID_M=0x%x DTO0=0x%08x\n", pv[0], pv[1], pv[2], pv[3], pv[4], pv[5], pv[6], pv[7], pv[8], pv[9]);
        rnPublish(this, "dcn-replay-pre", pn, pv, 16);
        uint32_t vidReg = (base[2] + 0x250c) * 4, otgReg = (base[2] + 0x1b41) * 4;
        if (maxGroup >= 3) {
            if (rd(vidReg) & 1) {                                           // stop the DP video stream first
                wr(vidReg, rd(vidReg) & ~1u);
                for (int i = 0; i < 100 && (rd(vidReg) & (1u << 16)); i++) IOSleep(1);
                RN_STEP("RedNavi: replay: DP video stream stopped, VID_STREAM=0x%08x\n", rd(vidReg));
            }
            if (rd(otgReg) & 1) {                                           // optc1_disable_crtc
                wr(otgReg, rd(otgReg) & ~1u);
                for (int i = 0; i < 200 && (rd(otgReg) & (1u << 16)); i++) IOSleep(1);
                RN_STEP("RedNavi: replay: OTG disabled, OTG_CONTROL=0x%08x\n", rd(otgReg));
            }
        }
    }
    static const char *const names[15] = {"", "clocks", "optc clocks", "otg timing", "opp", "mpc", "dig/dp stream", "otg enable", "test pattern", "dp stream enable", "hubbub", "dpp and clocks", "mpc tree", "hubp surface", "pattern off"};
    uint32_t mismatches = 0;
    OSDictionary *mm = OSDictionary::withCapacity(16);
    const rn_mode_ctx modeCtx = {&selMode, selPitch, 4};
    const rn_mode *baseMode = rn_base_mode();
    const bool baseState = selMode.pclk_khz == baseMode->pclk_khz && selMode.hactive == baseMode->hactive &&
                           selMode.vactive == baseMode->vactive && selMode.htotal == baseMode->htotal && selMode.vtotal == baseMode->vtotal;
    // The value to write for a table entry: the captured one, or the one computed for the selected mode.
    auto valueFor = [&](const RNReplayReg &r) {
        uint32_t v = r.value;
        if (!baseState) rn_mode_register(r.name, r.value, &modeCtx, &v);
        return v;
    };
    if (!baseState && maxGroup >= 1) {                                       // DISPCLK high enough for the pixel clock?
        uint32_t dentOff = (base[1] + 0x64) * 4, cur = rd(dentOff);
        int changed = 0;
        uint32_t nv = rn_dispclk_register(cur, &selMode, &changed);
        if (changed) {
            wr(dentOff, nv);
            IOSleep(5);
            RN_STEP("RedNavi: replay: DENTIST_DISPCLK_CNTL 0x%08x -> 0x%08x (read back 0x%08x) for %u kHz\n", cur, nv, rd(dentOff), selMode.pclk_khz);
        } else {
            RN_STEP("RedNavi: replay: DENTIST_DISPCLK_CNTL 0x%08x is enough for %u kHz\n", cur, selMode.pclk_khz);
        }
        setProperty("dentist-dispclk-cntl", static_cast<uint64_t>(rd(dentOff)), 32);
    }
    for (uint32_t g = 1; g <= maxGroup; g++) {
        RN_STEP("RedNavi: replay group %u: %s\n", g, names[g]);
        if (liveSwitch) { char t[40]; snprintf(t, sizeof(t), "live: replay group %u", g); trace(t); }
        if (g == 13 && !liveSwitch) { RN_STEP("RedNavi: replay: writing the scanout image to VRAM\n"); fillScanout(); RN_STEP("RedNavi: replay: scanout image written\n"); }
        for (const RNReplayReg &r : kRNReplay) {
            if (r.group != g) continue;
            uint32_t off = (base[r.baseIdx] + r.dword) * 4;
            if (off + 4 > regLen) { IOLog("RedNavi: replay: %s outside the BAR\n", r.name); continue; }
            wr(off, valueFor(r));
        }
        if (g == 7) {                                                       // wait for the OTG to report enabled
            uint32_t ctl = (base[2] + 0x1b41) * 4, ms = 0;
            for (; ms < 200; ms++) { if (rd(ctl) & (1u << 16)) break; IOSleep(1); }
            RN_STEP("RedNavi: replay: OTG_CONTROL = 0x%08x after %u ms\n", rd(ctl), ms);
        }
        for (const RNReplayReg &r : kRNReplay) {                            // read back, ignoring status bits
            if (r.group != g) continue;
            uint32_t v = rd((base[r.baseIdx] + r.dword) * 4), want = valueFor(r);
            if (v != want) {
                mismatches++;
                if (mismatches <= 24) IOLog("RedNavi: replay: %s wrote 0x%08x read 0x%08x\n", r.name, want, v);
                if (mm && mismatches <= 16) {
                    char txt[40];
                    snprintf(txt, sizeof(txt), "wrote 0x%08x read 0x%08x", want, v);
                    OSString *str = OSString::withCString(txt);
                    if (str) { mm->setObject(r.name, str); str->release(); }
                }
            }
        }
        IOSleep(40);
    }
    // Second pass: a few registers (line buffer partitions) do not take a write made early in the sequence. Write the
    // ones that read back wrong once more, now that the pipe runs.
    uint32_t fixed = 0;
    for (const RNReplayReg &r : kRNReplay) {
        if (r.group > maxGroup) continue;
        uint32_t want = valueFor(r), off = (base[r.baseIdx] + r.dword) * 4;
        if (off + 4 > regLen || rd(off) == want) continue;
        wr(off, want);
        if (rd(off) == want) { fixed++; IOLog("RedNavi: replay: %s took the second write\n", r.name); }
    }
    if (fixed) mismatches -= fixed < mismatches ? fixed : mismatches;
    // Status after the replay: is the OTG counting, is the stream flowing?
    uint32_t pos0 = rd((base[2] + 0x1b4a) * 4);
    IOSleep(2);
    uint32_t pos1 = rd((base[2] + 0x1b4a) * 4);
    uint32_t otgCtl = rd((base[2] + 0x1b41) * 4), otgStatus = rd((base[2] + 0x1b49) * 4);
    uint32_t vid = rd((base[2] + 0x250c) * 4), dpg = rd((base[2] + 0x185b) * 4), link = rd((base[2] + 0x2508) * 4);
    uint32_t beEn = rd((base[2] + 0x24b2) * 4), fe = rd((base[2] + 0x248b) * 4);
    IOLog("RedNavi: replay done: %u mismatches; OTG_CONTROL=0x%08x STATUS=0x%08x POS 0x%08x -> 0x%08x; DP_VID_STREAM_CNTL=0x%08x DPG_STATUS=0x%08x DP_LINK_CNTL=0x%08x BE_EN=0x%08x FE=0x%08x\n",
          mismatches, otgCtl, otgStatus, pos0, pos1, vid, dpg, link, beEn, fe);
    const char *n[12] = {"groups", "mismatches", "otg_control", "otg_status", "pos0", "pos1", "vid_stream_cntl", "dpg_status", "dp_link_cntl", "be_en", "fe_cntl", "otg_moving"};
    uint32_t v[12] = {maxGroup, mismatches, otgCtl, otgStatus, pos0, pos1, vid, dpg, link, beEn, fe, pos0 != pos1};
    rnPublish(this, "dcn-replay", n, v, 12);
    if (maxGroup >= 13 && pos0 != pos1) pipeReady = true;
    if (mm) { setProperty("dcn-replay-mismatch", mm); mm->release(); }
}

void RedNaviProbe::dcnDump() {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_dcn_dump_unsafe", &arg, sizeof(arg)) || arg != 1) return;
    uint32_t dmu = 0;
    if (!ipBase(kRNHwIdDmu, &dmu, 2)) return;
    uint32_t regLen = static_cast<uint32_t>(regs->getLength());
    OSData *blob = OSData::withCapacity(kRNDcnRegCount * 4);
    if (!blob) return;
    for (uint32_t i = 0; i < kRNDcnRegCount; i++) {
        uint32_t off = (dmu + kRNDcnRegs[i]) * 4, v = 0xDEADBEEF;
        if (off + 4 <= regLen) v = rd(off);
        blob->appendBytes(&v, 4);
    }
    setProperty("dcn-dump", blob);
    blob->release();
    IOLog("RedNavi: DCN dump of %u registers published\n", kRNDcnRegCount);
}

void RedNaviProbe::probeAux() {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_edid", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: AUX/EDID read not requested (boot-arg rednavi_edid=1)\n");
        return;
    }
    OSDictionary *out = OSDictionary::withCapacity(20);
    if (!out) return;
    int found = -1;
    uint8_t dpcd[16] = {};
    for (uint32_t eng = 0; eng < 5; eng++) {
        uint8_t buf[16] = {};
        uint32_t st = 0, rep = 0;
        bool ok = auxTransfer(eng, 0x90, 0x000, buf, 16, &st, &rep);
        IOLog("RedNavi: AUX%u DPCD read: %s, SW_STATUS=0x%08x, reply=0x%x, first bytes %02x %02x %02x %02x\n", eng, ok ? "ACK" : "no", st, rep, buf[0], buf[1], buf[2], buf[3]);
        char name[24];
        snprintf(name, sizeof(name), "aux%u_status", eng);
        OSNumber *n = OSNumber::withNumber(static_cast<uint64_t>(st), 64);
        if (n) { out->setObject(name, n); n->release(); }
        snprintf(name, sizeof(name), "aux%u_reply", eng);
        n = OSNumber::withNumber(static_cast<uint64_t>(rep), 64);
        if (n) { out->setObject(name, n); n->release(); }
        if (ok && found < 0) { found = static_cast<int>(eng); memcpy(dpcd, buf, 16); }
    }
    OSNumber *fn = OSNumber::withNumber(static_cast<uint64_t>(found < 0 ? 0xFF : found), 64);
    if (fn) { out->setObject("engine", fn); fn->release(); }
    setProperty("aux-probe", out);
    out->release();
    if (found < 0) { IOLog("RedNavi: no AUX engine answered\n"); return; }
    OSData *d = OSData::withBytes(dpcd, 16);
    if (d) { setProperty("dpcd", d); d->release(); }

    // EDID: set the offset to 0 (I2C write with MOT), then read 16 byte chunks (MOT until the last one).
    uint8_t edid[256] = {};
    uint32_t total = 128, got = 0;
    uint8_t zero = 0;
    uint32_t st = 0, rep = 0;
    if (!auxTransfer(static_cast<uint32_t>(found), 0x40, 0x50, &zero, 1, &st, &rep)) {
        IOLog("RedNavi: EDID: offset write failed, SW_STATUS=0x%08x reply=0x%x\n", st, rep);
        return;
    }
    while (got < total) {
        bool last = got + 16 >= total;
        if (!auxTransfer(static_cast<uint32_t>(found), last ? 0x10 : 0x50, 0x50, edid + got, 16, &st, &rep)) {
            IOLog("RedNavi: EDID: read at %u failed, SW_STATUS=0x%08x reply=0x%x\n", got, st, rep);
            break;
        }
        got += 16;
        if (got == 128 && edid[126] && total == 128) total = 256;            // one extension block
    }
    IOLog("RedNavi: EDID: %u bytes read from AUX%d\n", got, found);
    if (got) {
        OSData *e = OSData::withBytes(edid, got);
        if (e) { setProperty("edid", e); e->release(); }
        publishModes(edid, got);
    }
}

// Pick the mode to light: boot-arg rednavi_mode=WxH@R if the monitor lists it, else the preferred mode of the EDID, else the
// 1080p60 the replay table was captured at. Modes the link cannot carry (above HBR2 x4) are skipped.
void RedNaviProbe::selectMode() {
    selMode = *rn_base_mode();
    uint32_t w = 0, h = 0, r = 0;
    char arg[32] = {};
    if (PE_parse_boot_argn("rednavi_mode", arg, sizeof(arg))) {
        const char *p = arg;
        auto num = [&](uint32_t *out) { *out = 0; while (*p >= '0' && *p <= '9') *out = *out * 10 + static_cast<uint32_t>(*p++ - '0'); };
        num(&w); if (*p == 'x') { p++; num(&h); } if (*p == '@') { p++; num(&r); }
    }
    const rn_mode *pick = nullptr;
    for (unsigned i = 0; i < edidInfo.mode_count && !pick; i++) {
        const rn_mode &m = edidInfo.modes[i];
        int rate = rn_min_dp_link_rate(&m, 4);
        if (rate < 0 || rate > 0x14) continue;
        if (w) { if (m.hactive == w && m.vactive == h && (!r || m.refresh_hz == r)) pick = &m; }
        else if (m.preferred) pick = &m;
    }
    if (w && !pick) IOLog("RedNavi: mode %ux%u@%u is not usable on this monitor, falling back\n", w, h, r);
    if (!pick) {
        for (unsigned i = 0; i < edidInfo.mode_count && !pick; i++) {            // preferred mode, then the first one the link carries
            int rate = rn_min_dp_link_rate(&edidInfo.modes[i], 4);
            if (rate >= 0 && rate <= 0x14 && (edidInfo.modes[i].preferred || !w)) pick = &edidInfo.modes[i];
        }
    }
    if (pick) {
        selIndex = static_cast<int>(pick - edidInfo.modes);
        applyMode(*pick);
    } else {
        applyMode(*rn_base_mode());
    }
}

// Make m the mode the pipe will be programmed for: link rate, pitch, and the properties describing it.
void RedNaviProbe::applyMode(const rn_mode &m) {
    selMode = m;
    int rate = rn_min_dp_link_rate(&selMode, 4);
    const rn_mode *base = rn_base_mode();
    bool isBase = selMode.pclk_khz == base->pclk_khz && selMode.hactive == base->hactive && selMode.vactive == base->vactive && selMode.htotal == base->htotal;
    selRate = isBase ? 0x06 : static_cast<uint32_t>(rate < 0x06 ? 0x06 : rate);    // the base state was captured at RBR
    selPitch = rn_pitch_pixels(selMode.hactive);
    frontSurface = 0;                                                         // the replay points the plane at surface 0
    IOLog("RedNavi: mode %ux%u@%u pclk %u kHz, link rate 0x%02x, pitch %u pixels\n", selMode.hactive, selMode.vactive, selMode.refresh_hz, selMode.pclk_khz, selRate, selPitch);
    uint32_t vals[6] = {selMode.hactive, selMode.vactive, selMode.refresh_hz, selMode.pclk_khz, selRate, selPitch};
    const char *names[6] = {"width", "height", "refresh", "pclk_khz", "link_rate", "pitch_pixels"};
    rnPublish(this, "selected-mode", names, vals, 6);
}

// Breadcrumbs for hangs during a live mode change: the last step reached is kept in the NVRAM variable rednavi-trace, which
// survives a power cycle (read it with `nvram rednavi-trace` after the reboot). Only used on the live path.
void RedNaviProbe::trace(const char *what) {
    IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opt) return;
    OSString *str = OSString::withCString(what);
    if (str) { opt->setProperty("rednavi-trace", str); str->release(); }
    opt->release();
}

// Stop the DP video stream and the OTG so the link can be retrained and the timing reprogrammed (optc1_disable_crtc).
void RedNaviProbe::quiescePipe() {
    uint32_t b2 = 0;
    if (!ipBase(kRNHwIdDmu, &b2, 2)) return;
    uint32_t vidReg = (b2 + 0x250c) * 4, otgReg = (b2 + 0x1b41) * 4;
    if (rd(vidReg) & 1) {
        wr(vidReg, rd(vidReg) & ~1u);
        for (int i = 0; i < 100 && (rd(vidReg) & (1u << 16)); i++) IOSleep(1);
    }
    if (rd(otgReg) & 1) {
        wr(otgReg, rd(otgReg) & ~1u);
        for (int i = 0; i < 200 && (rd(otgReg) & (1u << 16)); i++) IOSleep(1);
    }
    wr((b2 + 0x248b) * 4, 0);                   // DIG4_DIG_FE_CNTL: the VBIOS state the replay starts from has the front end off
    IOSleep(5);
}

// Reprogram the pipe for m on the running link and report whether the DP video stream came up (VID_STREAM_STATUS).
bool RedNaviProbe::programLive(const rn_mode &m) {
    char t[64];
    snprintf(t, sizeof(t), "live %ux%u@%u: start", m.hactive, m.vactive, m.refresh_hz);
    trace(t);
    applyMode(m);
    liveSwitch = true;
    quiescePipe();
    trace("live: pipe quiesced");
    dpTrain();
    trace("live: link trained");
    dcnReplay();
    trace("live: replay done");
    liveSwitch = false;
    uint32_t b2 = 0;
    if (!ipBase(kRNHwIdDmu, &b2, 2)) return false;
    for (int i = 0; i < 100; i++) {
        if (rd((b2 + 0x250c) * 4) & (1u << 16)) return true;
        IOSleep(2);
    }
    return false;
}

// Live mode change for RedNaviFB (platform function "rednavi-set-mode", argument: index into the EDID mode list).
IOReturn RedNaviProbe::setModeLive(uint32_t index) {
    if (!pipeReady || !modeLock) return kIOReturnNotReady;
    if (index >= edidInfo.mode_count) return kIOReturnBadArgument;
    const rn_mode m = edidInfo.modes[index];
    int rate = rn_min_dp_link_rate(&m, 4);
    if (rate < 0 || rate > 0x14) return kIOReturnUnsupportedMode;
    IOLockLock(modeLock);
    IOReturn rc = kIOReturnSuccess;
    if (static_cast<int>(index) != selIndex) {
        const int prevIndex = selIndex;
        const rn_mode prevMode = selMode;
        IOLog("RedNavi: live mode change to %ux%u@%u\n", m.hactive, m.vactive, m.refresh_hz);
        selIndex = static_cast<int>(index);
        bool ok = programLive(m);
        if (!ok) {                                                      // the stream did not come up: go back to the working mode
            IOLog("RedNavi: live mode change failed, restoring %ux%u@%u\n", prevMode.hactive, prevMode.vactive, prevMode.refresh_hz);
            selIndex = prevIndex;
            programLive(prevMode);
            rc = kIOReturnIOError;
        }
        setProperty("fb-width", selMode.hactive, 32);
        setProperty("fb-height", selMode.vactive, 32);
        setProperty("fb-pitch", selPitch * 4, 32);
        setProperty("fb-refresh", selMode.refresh_hz, 32);
        { OSData *t = OSData::withBytes(&selMode, sizeof(selMode)); if (t) { setProperty("fb-timing", t); t->release(); } }
        setProperty("fb-current-mode", static_cast<uint64_t>(selIndex < 0 ? 0 : selIndex), 32);
    }
    IOLockUnlock(modeLock);
    return rc;
}

IOReturn RedNaviProbe::callPlatformFunction(const OSSymbol *name, bool waitForFunction, void *p1, void *p2, void *p3, void *p4) {
    if (name && name->isEqualTo("rednavi-set-mode")) return setModeLive(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p1)));
    return IOService::callPlatformFunction(name, waitForFunction, p1, p2, p3, p4);
}

// Interpret the EDID and publish the modes as "modes" (strings like "1920x1080@60 148500 kHz"), plus the monitor name.
void RedNaviProbe::publishModes(const uint8_t *edid, size_t len) {
    if (rn_edid_parse(edid, len, &edidInfo) != 0) { IOLog("RedNavi: EDID: header or checksum wrong, no modes\n"); return; }
    OSArray *list = OSArray::withCapacity(edidInfo.mode_count);
    if (!list) return;
    for (unsigned i = 0; i < edidInfo.mode_count; i++) {
        const rn_mode &m = edidInfo.modes[i];
        char line[64];
        snprintf(line, sizeof(line), "%ux%u@%u %u kHz%s%s", m.hactive, m.vactive, m.refresh_hz, m.pclk_khz,
                 m.preferred ? " preferred" : "", m.vic ? " (CTA)" : "");
        OSString *str = OSString::withCString(line);
        if (str) { list->setObject(str); str->release(); }
        IOLog("RedNavi: EDID mode %u: %s\n", i, line);
    }
    setProperty("modes", list);
    list->release();
    OSString *name = OSString::withCString(edidInfo.name);
    if (name) { setProperty("monitor-name", name); name->release(); }
}

bool RedNaviProbe::loadComponent(uint32_t mp0, const char *name, uint32_t cmd, const uint8_t *data, uint32_t len, uint32_t version) {
    uint32_t r35 = (mp0 + kRNMp0C2pmsg35) * 4, r36 = (mp0 + kRNMp0C2pmsg36) * 4, r58 = (mp0 + kRNMp0C2pmsg58) * 4, r81 = (mp0 + kRNMp0C2pmsg81) * 4;

    uint32_t st = 0;
    for (int i = 0; i < 2000; i++) { st = rd(r35); if (st == kRNPspBlReady) break; IOSleep(1); }
    if (st != kRNPspBlReady) { IOLog("RedNavi: PSP %s: bootloader not ready (C2PMSG_35 = 0x%08x)\n", name, st); return false; }

    // psp_copy_fw: zero the whole 1 MB buffer, then copy the image.
    if (len > kRNPspBufSize) { IOLog("RedNavi: PSP %s: image larger than the staging buffer\n", name); return false; }
    for (uint32_t off = 0; off < kRNPspBufSize; off += 4) vramWrite(kRNPspBufVramOffset + off, 0);
    uint32_t bad = 0;
    for (uint32_t off = 0; off < len; off += 4) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < 4 && off + k < len; k++) v |= static_cast<uint32_t>(data[off + k]) << (8 * k);
        vramWrite(kRNPspBufVramOffset + off, v);
        if (vramRead(kRNPspBufVramOffset + off) != v) bad++;
    }
    IOLog("RedNavi: PSP %s: %u bytes (version 0x%x) in VRAM +0x%llx, %u mismatching dwords\n", name, len, version, kRNPspBufVramOffset, bad);
    if (bad) return false;

    uint32_t before81 = rd(r81);
    wr(r36, static_cast<uint32_t>((kRNVramMcBase + kRNPspBufVramOffset) >> 20));
    wr(r35, cmd);

    if (cmd == kRNPspBlLoadSos) {
        // amdgpu waits 20 ms, then for C2PMSG_81 (sOS sign of life) to change.
        IOSleep(20);
        for (int i = 0; i < 2000; i++) { if (rd(r81) != before81) break; IOSleep(1); }
        IOLog("RedNavi: PSP %s: C2PMSG_81 = 0x%08x (was 0x%08x), C2PMSG_58 (sOS version) = 0x%08x, C2PMSG_35 = 0x%08x\n", name, rd(r81), before81, rd(r58), rd(r35));
        return rd(r81) != before81;
    }

    IOSleep(5);
    for (int i = 0; i < 2000; i++) { st = rd(r35); if (st == kRNPspBlReady) break; IOSleep(1); }
    IOLog("RedNavi: PSP %s: after command C2PMSG_35 = 0x%08x, C2PMSG_36 = 0x%08x, C2PMSG_81 = 0x%08x\n", name, st, rd(r36), rd(r81));
    return st == kRNPspBlReady;
}

void RedNaviProbe::loadPspChain() {
#ifdef RN_HAVE_PSP_FW
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_psp_load", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: PSP load not requested (boot-arg rednavi_psp_load=1)\n");
        return;
    }
    uint32_t mp0 = 0;
    if (!ipBase(kRNHwIdMp0, &mp0)) { IOLog("RedNavi: PSP: no MP0 base\n"); return; }
    if (((mp0 + kRNMp0C2pmsg81) * 4) + 4 > regs->getLength()) { IOLog("RedNavi: PSP: registers outside BAR\n"); return; }
    if (rd((mp0 + kRNMp0C2pmsg81) * 4) != 0) { IOLog("RedNavi: PSP: sOS already alive, nothing to do\n"); return; }

    uint32_t done = 0;
    for (const auto &c : kRNPspChain) {
        if (!loadComponent(mp0, c.name, c.cmd, c.data, c.size, c.version)) { IOLog("RedNavi: PSP chain stopped at %s\n", c.name); break; }
        done++;
    }
    IOLog("RedNavi: PSP chain: %u of %u components accepted\n", done, (uint32_t)(sizeof(kRNPspChain) / sizeof(kRNPspChain[0])));
    OSNumber *n = OSNumber::withNumber(static_cast<uint64_t>(done), 64);
    if (n) { setProperty("psp-chain-done", n); n->release(); }
    OSNumber *s81 = OSNumber::withNumber(static_cast<uint64_t>(rd((mp0 + kRNMp0C2pmsg81) * 4)), 64);
    if (s81) { setProperty("psp-c2pmsg81-after", s81); s81->release(); }
#else
    IOLog("RedNavi: built without psp_fw.h, PSP load not available\n");
#endif
}

bool RedNaviProbe::createPspRing() {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_psp_ring", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: PSP ring not requested (boot-arg rednavi_psp_ring=1)\n");
        return false;
    }
    uint32_t mp0 = 0;
    if (!ipBase(kRNHwIdMp0, &mp0)) { IOLog("RedNavi: PSP ring: no MP0 base\n"); return false; }
    uint32_t r64 = (mp0 + kRNMp0C2pmsg64) * 4, r69 = (mp0 + kRNMp0C2pmsg69) * 4, r70 = (mp0 + kRNMp0C2pmsg70) * 4, r71 = (mp0 + kRNMp0C2pmsg71) * 4;
    uint32_t r81 = (mp0 + kRNMp0C2pmsg81) * 4;
    if (r81 + 4 > regs->getLength()) { IOLog("RedNavi: PSP ring: registers outside BAR\n"); return false; }
    if (rd(r81) == 0) { IOLog("RedNavi: PSP ring: sOS not alive (C2PMSG_81 = 0), skipping\n"); return false; }

    // Wait for the sOS to be ready for ring creation.
    uint32_t st = 0;
    for (int i = 0; i < 2000; i++) { st = rd(r64); if ((st & kRNPspMboxMask) == 0x80000000u) break; IOSleep(1); }
    IOLog("RedNavi: PSP ring: C2PMSG_64 before = 0x%08x\n", st);
    if ((st & kRNPspMboxMask) != 0x80000000u) { IOLog("RedNavi: PSP ring: sOS not ready for ring creation\n"); return false; }

    for (uint32_t off = 0; off < kRNPspRingSize; off += 4) vramWrite(kRNPspRingVramOffset + off, 0);
    uint64_t mc = kRNVramMcBase + kRNPspRingVramOffset;
    wr(r69, static_cast<uint32_t>(mc));
    wr(r70, static_cast<uint32_t>(mc >> 32));
    wr(r71, kRNPspRingSize);
    wr(r64, kRNPspRingTypeKm << 16);
    IOSleep(20);
    for (int i = 0; i < 2000; i++) { st = rd(r64); if ((st & kRNPspMboxMask) == 0x80000000u) break; IOSleep(1); }
    IOLog("RedNavi: PSP ring: C2PMSG_64 after = 0x%08x (0x80000000 = created), ring at MC 0x%llx\n", st, mc);
    OSNumber *n = OSNumber::withNumber(static_cast<uint64_t>(st), 64);
    if (n) { setProperty("psp-ring-status", n); n->release(); }
    return (st & kRNPspMboxMask) == 0x80000000u;
}

// Submit one command through the kernel ring and wait for its fence (psp_ring_cmd_submit and
// psp_cmd_submit_buf in amdgpu_psp.c). args go to psp_gfx_cmd_resp.cmd (+28); nresp dwords of
// psp_gfx_cmd_resp.resp (+864) are returned.
bool RedNaviProbe::pspSubmit(uint32_t cmdId, const uint32_t *args, uint32_t nargs, uint32_t *resp, uint32_t nresp) {
    uint32_t mp0 = 0;
    if (!ipBase(kRNHwIdMp0, &mp0)) return false;
    uint32_t r67 = (mp0 + kRNMp0C2pmsg67) * 4;

    for (uint32_t off = 0; off < 0x1000; off += 4) {
        vramWrite(kRNPspCmdVramOffset + off, 0);
        vramWrite(kRNPspFenceVramOffset + off, 0);
    }
    vramWrite(kRNPspCmdVramOffset + 8, cmdId);               // psp_gfx_cmd_resp.cmd_id
    for (uint32_t i = 0; i < nargs; i++) vramWrite(kRNPspCmdVramOffset + 28 + i * 4, args[i]);

    uint32_t wptr = rd(r67);                                  // in dwords; a frame is 16 dwords
    uint32_t ringDw = kRNPspRingSize / 4;
    if (wptr % 16 != 0 || wptr >= ringDw) { IOLog("RedNavi: PSP cmd: odd write pointer 0x%x, not sending\n", wptr); return false; }
    uint64_t frame = kRNPspRingVramOffset + static_cast<uint64_t>(wptr) * 4;
    uint64_t cmdMc = kRNVramMcBase + kRNPspCmdVramOffset, fenceMc = kRNVramMcBase + kRNPspFenceVramOffset;
    uint32_t index = ++pspFence;
    for (uint32_t off = 0; off < 64; off += 4) vramWrite(frame + off, 0);
    vramWrite(frame + 0, static_cast<uint32_t>(cmdMc));
    vramWrite(frame + 4, static_cast<uint32_t>(cmdMc >> 32));
    vramWrite(frame + 12, static_cast<uint32_t>(fenceMc));
    vramWrite(frame + 16, static_cast<uint32_t>(fenceMc >> 32));
    vramWrite(frame + 20, index);
    wr(r67, (wptr + 16) % ringDw);

    uint32_t fence = 0;
    for (int i = 0; i < 2000; i++) { fence = vramRead(kRNPspFenceVramOffset); if (fence == index) break; IOSleep(1); }
    for (uint32_t i = 0; i < nresp; i++) resp[i] = vramRead(kRNPspCmdVramOffset + 864 + i * 4);
    if (fence != index) IOLog("RedNavi: PSP cmd 0x%x: fence %u, expected %u\n", cmdId, fence, index);
    return fence == index;
}

void RedNaviProbe::sendPspBootCfgGet() {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_psp_cmd", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: PSP command not requested (boot-arg rednavi_psp_cmd=1)\n");
        return;
    }
    // cmd.boot_cfg: timestamp, sub_cmd, boot_config, boot_config_valid
    uint32_t args[4] = {0, kRNPspBootCfgGet, 0, 0};
    uint32_t resp[17] = {};                                   // resp.status ... uresp.boot_cfg at dword 16
    bool ok = pspSubmit(kRNPspCmdBootCfg, args, 4, resp, 17);
    IOLog("RedNavi: PSP cmd BOOT_CFG_GET: fence %s, status = 0x%08x, boot_cfg = 0x%08x\n", ok ? "ok" : "missing", resp[0], resp[16]);
    const char *names[3] = {"fence_ok", "status", "boot_cfg"};
    uint32_t vals[3] = {ok, resp[0], resp[16]};
    rnPublish(this, "psp-cmd-boot-cfg", names, vals, 3);
}

void RedNaviProbe::sendPspSetupTmr() {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_psp_tmr", &arg, sizeof(arg)) || (arg != 1 && arg != 2)) {
        IOLog("RedNavi: PSP SETUP_TMR not requested (boot-arg rednavi_psp_tmr=1, or 2 for the top of VRAM as Linux does)\n");
        return;
    }
    if (!pspTmrSize) { IOLog("RedNavi: PSP SETUP_TMR: no TMR size from LOAD_TOC\n"); return; }
    IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + kRNVramBar * 4);
    if (!bar0) { IOLog("RedNavi: PSP SETUP_TMR: no BAR0\n"); return; }
    uint64_t tmrOff = arg == 2 ? 0x3e0000000ull : kRNPspTmrVramOffset;       // 2: where the Linux reference run put it
    uint64_t mc = kRNVramMcBase + tmrOff;                                      // GPU address
    uint64_t pa = static_cast<uint64_t>(bar0->getPhysicalAddress()) + tmrOff;  // CPU physical address of the same bytes (amdgpu_gmc_vram_pa)
    // cmd.cmd_setup_tmr: buf_phy_addr_lo/hi, buf_size, flags (bit 1 = virt_phy_addr), system_phy_addr_lo/hi
    uint32_t args[6] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), pspTmrSize, 2u,
                        static_cast<uint32_t>(pa), static_cast<uint32_t>(pa >> 32)};
    uint32_t resp[1] = {};
    bool ok = pspSubmit(kRNPspCmdSetupTmr, args, 6, resp, 1);
    IOLog("RedNavi: PSP cmd SETUP_TMR (0x%x bytes at MC 0x%llx, PA 0x%llx): fence %s, status = 0x%08x\n", pspTmrSize, mc, pa, ok ? "ok" : "missing", resp[0]);
    const char *names[3] = {"fence_ok", "status", "pa_lo"};
    uint32_t vals[3] = {ok, resp[0], static_cast<uint32_t>(pa)};
    rnPublish(this, "psp-cmd-setup-tmr", names, vals, 3);
}

void RedNaviProbe::sendPspLoadToc() {
#ifdef RN_HAVE_PSP_FW
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_psp_toc", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: PSP LOAD_TOC not requested (boot-arg rednavi_psp_toc=1)\n");
        return;
    }
    // psp_copy_fw: zero the staging buffer, copy the TOC, then point the command at it.
    uint32_t len = sizeof(kRNPspToc);
    for (uint32_t off = 0; off < kRNPspBufSize; off += 4) vramWrite(kRNPspBufVramOffset + off, 0);
    for (uint32_t off = 0; off < len; off += 4) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < 4 && off + k < len; k++) v |= static_cast<uint32_t>(kRNPspToc[off + k]) << (8 * k);
        vramWrite(kRNPspBufVramOffset + off, v);
    }
    uint64_t mc = kRNVramMcBase + kRNPspBufVramOffset;
    // cmd.cmd_load_toc: toc_phy_addr_lo, toc_phy_addr_hi, toc_size
    uint32_t args[3] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), len};
    uint32_t resp[5] = {};                                    // status, session_id, fw_addr_lo, fw_addr_hi, tmr_size
    bool ok = pspSubmit(kRNPspCmdLoadToc, args, 3, resp, 5);
    IOLog("RedNavi: PSP cmd LOAD_TOC (%u bytes): fence %s, status = 0x%08x, tmr_size = 0x%08x\n", len, ok ? "ok" : "missing", resp[0], resp[4]);
    const char *names[3] = {"fence_ok", "status", "tmr_size"};
    uint32_t vals[3] = {ok, resp[0], resp[4]};
    rnPublish(this, "psp-cmd-load-toc", names, vals, 3);
    if (ok && resp[0] == 0) pspTmrSize = resp[4];
#else
    IOLog("RedNavi: built without psp_fw.h, LOAD_TOC not available\n");
#endif
}

// Dump the DMCUB window, security and control registers into one property (all at DMU base index 2).
void RedNaviProbe::dmcubSnapshot(const char *property) {
    uint32_t dmu = 0;
    if (!ipBase(kRNHwIdDmu, &dmu, 2)) return;
    if ((dmu + 0x21a) * 4 + 4 > regs->getLength()) return;
    OSDictionary *out = OSDictionary::withCapacity(64);
    if (!out) return;
    auto put = [&](const char *name, uint32_t dword) {
        OSNumber *n = OSNumber::withNumber(static_cast<uint64_t>(rd((dmu + dword) * 4)), 64);
        if (n) { out->setObject(name, n); n->release(); }
    };
    static const char *const base[8] = {"CW0_BASE", "CW1_BASE", "CW2_BASE", "CW3_BASE", "CW4_BASE", "CW5_BASE", "CW6_BASE", "CW7_BASE"};
    static const char *const top[8] = {"CW0_TOP", "CW1_TOP", "CW2_TOP", "CW3_TOP", "CW4_TOP", "CW5_TOP", "CW6_TOP", "CW7_TOP"};
    static const char *const off[8] = {"CW0_OFFSET", "CW1_OFFSET", "CW2_OFFSET", "CW3_OFFSET", "CW4_OFFSET", "CW5_OFFSET", "CW6_OFFSET", "CW7_OFFSET"};
    static const char *const offHi[8] = {"CW0_OFFSET_HI", "CW1_OFFSET_HI", "CW2_OFFSET_HI", "CW3_OFFSET_HI", "CW4_OFFSET_HI", "CW5_OFFSET_HI", "CW6_OFFSET_HI", "CW7_OFFSET_HI"};
    for (uint32_t n = 0; n < 8; n++) {
        put(base[n], 0x1a5 + n);
        put(top[n], 0x1ad + n);
        put(off[n], 0x1b5 + 2 * n);
        put(offHi[n], 0x1b6 + 2 * n);
    }
    put("REGION5_OFFSET", 0x198); put("REGION5_OFFSET_HI", 0x199); put("REGION5_TOP", 0x1a2);
    put("TMR_AXI_SPACE", 0x21a); put("SEC_CNTL", 0x1ce); put("MEM_CNTL", 0x1cf);
    put("CNTL", 0x1f6); put("CNTL2", 0x200); put("SCRATCH0", 0x1e3); put("SCRATCH1", 0x1e4);
    put("FAULT_ADDR", 0x1fa); put("TIMER", 0x1fd);
    setProperty(property, out);
    out->release();
}

// psp_load_smu_fw (first thing psp_load_non_psp_fw does when autoload is supported): hand the SMU firmware to the sOS. Opt-in,
// boot-arg rednavi_smu_load=1, needs RedNavi/smu_fw.h (tools/gen_smu_fw_header.py). Linux does this after SETUP_TMR and
// before the other firmware; its SMU only answers messages afterwards.
void RedNaviProbe::loadSmuFw() {
#ifdef RN_HAVE_SMU_FW
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_smu_load", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: SMU firmware load not requested (boot-arg rednavi_smu_load=1)\n");
        return;
    }
#if defined(RN_SMU_FW_METADATA_V2)
    // The legacy Navi32 SMU path must never consume a Navi33 PMFW header.
    if (kRNSmuFwTargetMp1Major != 13u ||
        kRNSmuFwTargetMp1Minor != 0u ||
        kRNSmuFwTargetMp1Revision != 10u ||
        RN_SMU_FW_TARGET_VERIFIED != 1 ||
        RN_SMU_FW_DIGEST_TRUSTED != 1) {
        IOLog("RedNavi: legacy Navi32 SMU loader rejected firmware target\n");
        return;
    }
#endif
    if (kRNSmuFwSize > kRNPspBufSize) { IOLog("RedNavi: SMU firmware does not fit the staging buffer\n"); return; }
    for (uint32_t off = 0; off < kRNPspBufSize; off += 4) vramWrite(kRNPspBufVramOffset + off, 0);
    for (uint32_t off = 0; off < kRNSmuFwSize; off += 4) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < 4 && off + k < kRNSmuFwSize; k++) v |= static_cast<uint32_t>(kRNSmuFw[off + k]) << (8 * k);
        vramWrite(kRNPspBufVramOffset + off, v);
    }
    uint64_t mc = kRNVramMcBase + kRNPspBufVramOffset;
    uint32_t args[4] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), kRNSmuFwSize, kRNPspFwTypeSmu};
    uint32_t resp[4] = {};
    bool ok = pspSubmit(kRNPspCmdLoadIpFw, args, 4, resp, 4);
    IOLog("RedNavi: PSP LOAD_IP_FW SMU (%u bytes, version 0x%x): fence %s, status = 0x%08x, fw_addr = 0x%08x%08x\n", kRNSmuFwSize, kRNSmuFwVersion, ok ? "ok" : "missing", resp[0], resp[3], resp[2]);
    const char *pn[5] = {"fence_ok", "status", "fw_addr_lo", "fw_addr_hi", "fw_version"};
    uint32_t pv[5] = {ok, resp[0], resp[2], resp[3], kRNSmuFwVersion};
    rnPublish(this, "psp-load-smu", pn, pv, 5);
    IOSleep(100);                                                           // let the SMU firmware come up
#else
    IOLog("RedNavi: built without RedNavi/smu_fw.h, SMU firmware load unavailable\n");
#endif
}

// psp_load_non_psp_fw for the graphics block: every GC, SDMA, MES, IMU and RLC image goes to the sOS with LOAD_IP_FW in the order of
// Linux's AMDGPU_UCODE_ID enum, and once RLC_G is in, GFX_CMD_ID_AUTOLOAD_RLC starts the RLC autoload. Then gfx_v11_0_wait_for_rlc_
// autoload_complete: CP_STAT must read 0 and RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE (bit 31) must be set. Opt-in, boot-arg
// rednavi_gc_load=1, needs RedNavi/gc_fw.h (tools/gen_gc_fw_header.py).
// gfx_v11_0_wait_for_rlc_autoload_complete: CP_STAT == 0 and RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE. Publishes the values with `prefix`.
void RedNaviProbe::gcStatus(OSDictionary *out, const char *prefix, uint32_t timeoutMs) {
    uint32_t gc0 = 0, gc1 = 0;
    if (!ipBase(kRNHwIdGc, &gc0, 0) || !ipBase(kRNHwIdGc, &gc1, 1)) return;
    uint32_t cpStat = 0xFFFFFFFFu, boot = 0, ms = 0;
    for (; ms < timeoutMs; ms++) {
        cpStat = rd((gc0 + 0xf40) * 4);                             // regCP_STAT
        boot = rd((gc1 + 0x4e82) * 4);                              // regRLC_RLCS_BOOTLOAD_STATUS
        if (cpStat == 0 && (boot & 0x80000000u)) break;
        IOSleep(1);
    }
    // Each read is followed by a look at SDMA0_VERSION (0x600 while the block is up): one of these reads may be what turns it off.
    auto alive = [&](const char *name) {
        char key[48];
        snprintf(key, sizeof(key), "%safter_%s_sdma", prefix, name);
        rnPublishOne(out, key, rd(0x1295 * 4));
    };
    struct { const char *name; uint32_t reg; } regs[] = {
        {"grbm_status", (gc0 + 0xda4) * 4}, {"rlc_gpm_stat", (gc1 + 0x4e6b) * 4}, {"rlc_cntl", (gc1 + 0x4c00) * 4},
        {"imu_scratch0", (gc1 + 0x4068) * 4},
    };
    char key[40];
    snprintf(key, sizeof(key), "%swait_ms", prefix); rnPublishOne(out, key, ms);
    snprintf(key, sizeof(key), "%scp_stat", prefix); rnPublishOne(out, key, cpStat);
    snprintf(key, sizeof(key), "%sbootload_status", prefix); rnPublishOne(out, key, boot);
    alive("cp_boot");
    for (auto &r : regs) {
        snprintf(key, sizeof(key), "%s%s", prefix, r.name);
        rnPublishOne(out, key, rd(r.reg));
        alive(r.name);
    }
    IOLog("RedNavi: GC status%s: CP_STAT=0x%08x BOOTLOAD_STATUS=0x%08x after %u ms\n", prefix, cpStat, boot, ms);
}

void RedNaviProbe::loadGcFw() {
#ifdef RN_HAVE_GC_FW
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_gc_load", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: GC firmware load not requested (boot-arg rednavi_gc_load=1)\n");
        return;
    }
    uint32_t okCount = 0, failIndex = 0xFFFFFFFFu, failStatus = 0, autoload = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < kRNGcFwCount; i++) {
        const RNGcFw &f = kRNGcFw[i];
        if (f.size > kRNPspBufSize) { IOLog("RedNavi: GC firmware %s does not fit the staging buffer\n", f.name); failIndex = i; break; }
        for (uint32_t off = 0; off < f.size; off += 4) {
            uint32_t v = 0;
            for (uint32_t k = 0; k < 4 && off + k < f.size; k++) v |= static_cast<uint32_t>(kRNGcFwBlob[f.offset + off + k]) << (8 * k);
            vramWrite(kRNPspBufVramOffset + off, v);
        }
        uint64_t mc = kRNVramMcBase + kRNPspBufVramOffset;
        uint32_t args[4] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), f.size, f.type};
        uint32_t resp[4] = {};
        bool ok = pspSubmit(kRNPspCmdLoadIpFw, args, 4, resp, 4);
        IOLog("RedNavi: GC firmware %s (type %u, %u bytes): fence %s, status 0x%08x\n", f.name, f.type, f.size, ok ? "ok" : "missing", resp[0]);
        if (!ok || resp[0] != 0) { failIndex = i; failStatus = resp[0]; break; }
        okCount++;
        if (f.type == 8) {                                                  // GFX_FW_TYPE_RLC_G: all graphics firmware is in
            uint32_t r[1] = {};
            bool a = pspSubmit(kRNPspCmdAutoloadRlc, nullptr, 0, r, 1);
            autoload = a ? r[0] : 0xFFFFFFFEu;
            IOLog("RedNavi: AUTOLOAD_RLC: fence %s, status 0x%08x\n", a ? "ok" : "missing", r[0]);
            if (!a || r[0] != 0) break;
        }
    }
    OSDictionary *out = OSDictionary::withCapacity(16);
    if (!out) return;
    rnPublishOne(out, "images", kRNGcFwCount);
    rnPublishOne(out, "loaded", okCount);
    rnPublishOne(out, "fail_index", failIndex);
    rnPublishOne(out, "fail_status", failStatus);
    rnPublishOne(out, "autoload_status", autoload);
    if (autoload == 0) gcStatus(out, "", 2000);                              // only touch GC registers once the autoload was accepted
    setProperty("gc-load", out);
    out->release();
#else
    IOLog("RedNavi: built without RedNavi/gc_fw.h, GC firmware load unavailable\n");
#endif
}

// First command on the graphics engines (boot-arg rednavi_sdma_test=1, needs the GC firmware and the SMU features enabled so the RLC has
// booted): enable the GC memory hub on VRAM, start SDMA0 on a ring in VRAM and have it write a magic value that the CPU then reads.
// The GPU's interrupt: ack the IH ring (which re-arms the next one) and count it.
void RedNaviProbe::msiHandler(OSObject *owner, IOInterruptEventSource *, int) {
    RedNaviProbe *self = OSDynamicCast(RedNaviProbe, owner);
    if (!self) return;
    self->msiLastWptr = rn_ih_ack(&self->gpu, 4096);
    self->msiCount = self->msiCount + 1;
}

int RedNaviProbe::waitMsi(void *ctx, uint32_t timeoutMs) {
    RedNaviProbe *self = static_cast<RedNaviProbe *>(ctx);
    for (uint32_t ms = 0; ms < timeoutMs && self->msiCount == 0; ms++) IOSleep(1);
    return self->msiCount != 0;
}

// The door from user space (docs/COMPAT.md, "The user client", interface in rn_uapi.h): buffers in host memory that the GPU reaches
// through the GART, a way to hand the compute queue a command buffer, and a fence wait. Every buffer belongs to the connection that
// created it and goes away with it; command buffers and fences must lie inside the connection's own buffers. The caller is still trusted
// with the whole GPU (one shared address space): it needs administrator privilege unless boot-arg rednavi_uc_anyone=1 is set.
class IOAccelRedNaviClient : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelRedNaviClient)
    static constexpr uint32_t kMaxHandles = 32768;
    RedNaviProbe *gpuOwner {nullptr};
    IOLock *lock {nullptr};
    int *boIdx {nullptr};                          // handle - 1 -> index in the probe's buffer table, -1 for a free handle
    uint32_t boUsed {0};
    struct Map { uint64_t va; uint64_t size; uint32_t handle; bool used; };
    static constexpr uint32_t kMaxMaps = 65536;
    Map *maps {nullptr};                           // what is mapped in this connection's address space
    int vmid {0};
    int fenceIdx {-1};                             // the context's fence: a kernel-owned page, the GPU writes the sequence number of each finished submit to its first dword
    uint32_t seqSubmitted {0};                     // the compute queue's counter (bit 31 clear)
    uint32_t gfxSeqSubmitted {0};                  // the graphics queue's: bit 31 set once something was submitted
    uint32_t lostFrom {1}, lostTo {0};             // sequence numbers dropped by the last queue reset (none when lostFrom > lostTo)
    uint32_t gfxLostFrom {1}, gfxLostTo {0};       // the same for the graphics queue (numbers with bit 31 set)
    friend class RedNaviProbe;

    // Synchronisation objects (rn_uapi.h): the binary state and a timeline per object. Work that signals one is attached as a number of this connection's
    // fence and counts as done when the fence has reached it; nothing here is in the kernel's way, it is all evaluated when somebody looks.
    struct Syncobj {
        bool used, binSignaled;
        uint32_t binSeq;                           // work attached to the binary state (0: none)
        uint64_t tlValue;                          // the timeline, as far as it is known to be done
        struct { uint64_t point; uint32_t seq; } pend[8];   // work attached to timeline points
        uint32_t npend;
    };
    static constexpr uint32_t kMaxSync = 512;
    Syncobj *sync {nullptr};

    // The buffer behind a handle, or -1.
    int boOf(uint32_t handle) const {
        if (handle == 0 || handle > kMaxHandles) return -1;
        return boIdx[handle - 1];
    }
    // A program's address of `size` bytes as the 48 bits the page table is built over: the lower half as it is, the upper half only in canonical form (bits 63:47 all
    // set) with the high bits dropped, as Linux does in the GEM_VA ioctl (AMDGPU_GMC_HOLE_MASK). A range does not cross the hole: it stays in its half.
    static bool canonical(uint64_t va, uint64_t size, uint64_t *out) {
        if (va < (1ull << 47)) {
            if (size > (1ull << 47) - va) return false;
            *out = va;
            return true;
        }
        if (va < 0xffff800000000000ull || size > ~va + 1) return false;
        *out = va & 0x0000FFFFFFFFFFFFull;
        return true;
    }
    // Is [va, va + len) inside one mapping of this connection's address space?
    bool inMap(uint64_t va, uint64_t len) const {
        for (uint32_t i = 0; i < kMaxMaps; i++) {
            if (!maps[i].used) continue;
            if (va >= maps[i].va && len <= maps[i].size && va - maps[i].va <= maps[i].size - len) return true;
        }
        return false;
    }
    void unmapSlot(uint32_t i) {
        gpuOwner->vmUnmap(vmid, maps[i].va / 4096, static_cast<uint32_t>(maps[i].size / 4096));
        maps[i].used = false;
    }
    void freeAll() {
        for (uint32_t i = 0; i < kMaxMaps; i++) if (maps[i].used) unmapSlot(i);   // the GPU must not reach a page after it is released
        if (vmid > 0) {
            IOLockLock(gpuOwner->queueLock);
            if (gpuOwner->vmClient[vmid] == this) gpuOwner->vmClient[vmid] = nullptr;    // a reset must not look at this connection while its fence page goes away
            IOLockUnlock(gpuOwner->queueLock);
            gpuOwner->vmFree(vmid);
            vmid = 0;
        }
        for (uint32_t i = 0; i < kMaxHandles; i++)
            if (boIdx[i] >= 0) { gpuOwner->destroyBo(boIdx[i]); boIdx[i] = -1; }
        boUsed = 0;
        if (fenceIdx >= 0) { gpuOwner->destroyBo(fenceIdx); fenceIdx = -1; }
    }
    uint32_t fenceNow() const {
        return *reinterpret_cast<volatile uint32_t *>(gpuOwner->bos[fenceIdx].md->getBytesNoCopy());
    }
    // Sequence numbers are 31-bit counters (bit 31 names the queue) compared modulo 2^31 against the fence of their queue.
    uint32_t fenceWord(uint32_t ring) const {
        return *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(gpuOwner->bos[fenceIdx].md->getBytesNoCopy()) + ring * 8);
    }
    static bool seqDone(uint32_t fenceValue, uint32_t seq) { return ((fenceValue - seq) & 0x7fffffffu) < 0x40000000u; }
    bool reached(uint32_t seq) const { return seqDone(fenceWord(seq >> 31), seq); }
    Syncobj *findSync(uint32_t h) { return (h == 0 || h > kMaxSync || !sync[h - 1].used) ? nullptr : &sync[h - 1]; }
    // The timeline value: the highest point whose work is done. Done work leaves the pending list.
    uint64_t tlNow(Syncobj &s) {
        uint64_t v = s.tlValue;
        uint32_t keep = 0;
        for (uint32_t i = 0; i < s.npend; i++) {
            if (reached(s.pend[i].seq)) { if (s.pend[i].point > v) v = s.pend[i].point; }
            else s.pend[keep++] = s.pend[i];
        }
        s.npend = keep;
        s.tlValue = v;
        return v;
    }
    bool binDone(Syncobj &s) {
        if (s.binSignaled) return true;
        if (s.binSeq && reached(s.binSeq)) { s.binSignaled = true; s.binSeq = 0; return true; }
        return false;
    }
    bool isDone(Syncobj &s, uint64_t point) { return point == 0 ? binDone(s) : tlNow(s) >= point; }
    // Is there work attached for this point, or has it been done?
    bool isAttached(Syncobj &s, uint64_t point) {
        if (isDone(s, point)) return true;
        if (point == 0) return s.binSeq != 0;
        for (uint32_t i = 0; i < s.npend; i++) if (s.pend[i].point >= point) return true;
        return false;
    }
    bool canAttach(Syncobj &s, uint64_t point) {
        if (point == 0) return true;
        tlNow(s);
        return s.npend < 8;
    }
    void attachWork(Syncobj &s, uint64_t point, uint32_t seq) {
        if (point == 0) { s.binSignaled = false; s.binSeq = seq; return; }
        if (s.npend < 8) { s.pend[s.npend].point = point; s.pend[s.npend].seq = seq; s.npend++; }
    }
    void signalNow(Syncobj &s, uint64_t point) {
        if (point == 0) { s.binSignaled = true; s.binSeq = 0; }
        else if (point > s.tlValue) s.tlValue = point;
    }
    // INFO queries (DRM_AMDGPU_INFO), answered in the caller's buffer, min(size, what the query returns) bytes as Linux does. The device info, the IP info and
    // the firmware versions are what the Linux kernel answered on this card (RedNavi/linux_info_navi32.h, tools/linux_amdgpu_info_raw.py), with the one thing the
    // kext does differently: it submits to one compute queue only. What it cannot say yet is refused (kIOReturnUnsupported): the timestamp, the sensors, the video
    // and user-queue queries.
    static void putOut(void *out, uint32_t outSize, const void *v, size_t n) { memcpy(out, v, n < outSize ? n : outSize); }
    IOReturn infoQuery(const drm_amdgpu_info *r, void *out, uint32_t outSize) {
        memset(out, 0, outSize);
        const uint64_t gttTotal = gpuOwner->boBudget, gttUsed = gpuOwner->boBytes;      // the host memory that can be wired for buffers, and what is
        switch (r->query) {
        case AMDGPU_INFO_ACCEL_WORKING: { const uint32_t v = 1; putOut(out, outSize, &v, sizeof(v)); return kIOReturnSuccess; }
        case AMDGPU_INFO_DEV_INFO: putOut(out, outSize, kRNLinuxDevInfo, sizeof(kRNLinuxDevInfo)); return kIOReturnSuccess;
        case AMDGPU_INFO_HW_IP_COUNT: {                                      // only what the kext can submit to: one compute queue (Linux says 4 compute, 1 graphics, 2 SDMA)
            const uint32_t v = r->query_hw_ip.type == AMDGPU_HW_IP_COMPUTE ? 1 : (r->query_hw_ip.type == AMDGPU_HW_IP_GFX && gpuOwner->gfxQueueOk) ? 1 : 0;
            putOut(out, outSize, &v, sizeof(v));
            return kIOReturnSuccess;
        }
        case AMDGPU_INFO_HW_IP_INFO: {
            uint8_t ip[sizeof(kRNLinuxHwIpInfo[0])];
            memset(ip, 0, sizeof(ip));
            if (r->query_hw_ip.type == AMDGPU_HW_IP_COMPUTE && r->query_hw_ip.ip_instance == 0) {
                memcpy(ip, kRNLinuxHwIpInfo[AMDGPU_HW_IP_COMPUTE], sizeof(ip));
                reinterpret_cast<drm_amdgpu_info_hw_ip *>(ip)->available_rings = 1;       // Linux: 0xf, four compute rings
            }
            if (r->query_hw_ip.type == AMDGPU_HW_IP_GFX && r->query_hw_ip.ip_instance == 0 && gpuOwner->gfxQueueOk) {
                memcpy(ip, kRNLinuxHwIpInfo[AMDGPU_HW_IP_GFX], sizeof(ip));                 // one graphics ring, as Linux (available_rings 1)
            }
            putOut(out, outSize, ip, sizeof(ip));
            return kIOReturnSuccess;
        }
        case AMDGPU_INFO_FW_VERSION: {
            for (uint32_t i = 0; i < kRNLinuxFwCount; i++) {
                if (kRNLinuxFw[i].type != r->query_fw.fw_type) continue;
                drm_amdgpu_info_firmware f;
                f.ver = kRNLinuxFw[i].version;
                f.feature = kRNLinuxFw[i].feature;
                putOut(out, outSize, &f, sizeof(f));
                return kIOReturnSuccess;
            }
            return kIOReturnBadArgument;                                     // a firmware this chip does not have: -EINVAL
        }
        case AMDGPU_INFO_READ_MMR_REG: {                                     // what RADV reads for the gpu info: GB_ADDR_CONFIG, offset 0x263e as the DRM interface numbers it (Linux answered 0x444 on this card)
            if (r->read_mmr_reg.dword_offset != 0x263e || r->read_mmr_reg.count != 1) return kIOReturnUnsupported;
            const uint32_t v = 0x00000444;
            putOut(out, outSize, &v, sizeof(v));
            return kIOReturnSuccess;
        }
        case AMDGPU_INFO_GPUVM_FAULT: {                                      // struct drm_amdgpu_info_gpuvm_fault: the address (a 4 KB page number in the registers), the status, the hub
            u32 dg[11];
            rn_compute_diag(&gpuOwner->gpu, dg);
            struct { uint64_t addr; uint32_t status; uint32_t vmhub; } f = {(static_cast<uint64_t>(dg[5]) << 12) | (static_cast<uint64_t>(dg[6]) << 44), dg[4], 0};
            putOut(out, outSize, &f, sizeof(f));
            return kIOReturnSuccess;
        }
        case AMDGPU_INFO_MEMORY: {
            drm_amdgpu_memory_info m;
            memset(&m, 0, sizeof(m));
            const uint64_t devTotal = gpuOwner->devPool.totalPages * 4096, visTotal = gpuOwner->visPool.totalPages * 4096;
            const uint64_t devUsed = devTotal - gpuOwner->devPool.freePages * 4096, visUsed = visTotal - gpuOwner->visPool.freePages * 4096;
            m.vram.total_heap_size = devTotal + visTotal; m.vram.usable_heap_size = devTotal + visTotal; m.vram.heap_usage = devUsed + visUsed; m.vram.max_allocation = RedNaviProbe::kBoMaxBytes;
            m.cpu_accessible_vram.total_heap_size = visTotal; m.cpu_accessible_vram.usable_heap_size = visTotal; m.cpu_accessible_vram.heap_usage = visUsed; m.cpu_accessible_vram.max_allocation = RedNaviProbe::kBoMaxBytes;
            m.gtt.total_heap_size = gttTotal; m.gtt.usable_heap_size = gttTotal; m.gtt.heap_usage = gttUsed; m.gtt.max_allocation = RedNaviProbe::kBoMaxBytes;
            putOut(out, outSize, &m, sizeof(m));
            return kIOReturnSuccess;
        }
        case AMDGPU_INFO_VRAM_GTT: {
            drm_amdgpu_info_vram_gtt g;
            g.vram_size = gpuOwner->devPool.totalPages * 4096 + gpuOwner->visPool.totalPages * 4096; g.vram_cpu_accessible_size = gpuOwner->visPool.totalPages * 4096; g.gtt_size = gttTotal;
            putOut(out, outSize, &g, sizeof(g));
            return kIOReturnSuccess;
        }
        case AMDGPU_INFO_VRAM_USAGE: case AMDGPU_INFO_VIS_VRAM_USAGE: {
            const RedNaviProbe::VramPool &p = r->query == AMDGPU_INFO_VIS_VRAM_USAGE ? gpuOwner->visPool : gpuOwner->devPool;
            const uint64_t v = r->query == AMDGPU_INFO_VIS_VRAM_USAGE ? (p.totalPages - p.freePages) * 4096
                                                                      : (gpuOwner->devPool.totalPages - gpuOwner->devPool.freePages + gpuOwner->visPool.totalPages - gpuOwner->visPool.freePages) * 4096;
            putOut(out, outSize, &v, sizeof(v));
            return kIOReturnSuccess;
        }
        case AMDGPU_INFO_NUM_BYTES_MOVED: case AMDGPU_INFO_NUM_EVICTIONS: case AMDGPU_INFO_NUM_VRAM_CPU_PAGE_FAULTS: {
            const uint64_t v = 0;
            putOut(out, outSize, &v, sizeof(v));
            return kIOReturnSuccess;
        }
        case AMDGPU_INFO_GTT_USAGE: putOut(out, outSize, &gttUsed, sizeof(gttUsed)); return kIOReturnSuccess;
        case AMDGPU_INFO_MAX_IBS: {                                          // IBs per submit, one value per IP type: a submit of the kext takes one IB (Linux: 192, 125, 49, ...)
            uint32_t v[sizeof(kRNLinuxMaxIbs) / sizeof(kRNLinuxMaxIbs[0])];
            for (uint32_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) v[i] = (i == AMDGPU_HW_IP_COMPUTE || (i == AMDGPU_HW_IP_GFX && gpuOwner->gfxQueueOk)) ? 1 : 0;
            putOut(out, outSize, v, sizeof(v));
            return kIOReturnSuccess;
        }
        default:
            return kIOReturnUnsupported;
        }
    }

    // Waits for one (object, point): until work is attached (when `forSubmit`, else an object nothing was ever attached to is an error), then until it is done.
    IOReturn syncWaitOne(Syncobj &s, uint64_t point, uint32_t timeoutMs, bool forSubmit, bool availableOnly) {
        uint32_t ms = 0;
        for (;;) {
            if (availableOnly ? isAttached(s, point) : isDone(s, point)) return kIOReturnSuccess;
            if (!forSubmit && !isAttached(s, point)) return kIOReturnBadArgument;
            if (ms >= timeoutMs) return kIOReturnTimeout;
            IOSleep(1);
            ms++;
        }
    }

public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        gpuOwner = OSDynamicCast(RedNaviProbe, provider);
        lock = IOLockAlloc();
        boIdx = static_cast<int *>(IOMalloc(sizeof(int) * kMaxHandles));
        maps = static_cast<Map *>(IOMalloc(sizeof(Map) * kMaxMaps));
        sync = static_cast<Syncobj *>(IOMalloc(sizeof(Syncobj) * kMaxSync));
        if (!boIdx || !maps || !sync) return false;
        for (uint32_t i = 0; i < kMaxHandles; i++) boIdx[i] = -1;
        if (!gpuOwner || !lock) return false;
        memset(maps, 0, sizeof(Map) * kMaxMaps);
        memset(sync, 0, sizeof(Syncobj) * kMaxSync);
        vmid = gpuOwner->vmAlloc();
        if (vmid < 0) { vmid = 0; return false; }      // all seven address spaces are in use
        fenceIdx = gpuOwner->createHostBo(4096);
        if (fenceIdx < 0) return false;
        IOLockLock(gpuOwner->queueLock);
        gpuOwner->vmClient[vmid] = this;
        IOLockUnlock(gpuOwner->queueLock);
        return IOUserClient::start(provider);
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (lock && gpuOwner && boIdx && maps) freeAll();                // normally clientClose did it already
        if (lock) { IOLockFree(lock); lock = nullptr; }
        if (boIdx) { IOFree(boIdx, sizeof(int) * kMaxHandles); boIdx = nullptr; }
        if (maps) { IOFree(maps, sizeof(Map) * kMaxMaps); maps = nullptr; }
        if (sync) { IOFree(sync, sizeof(Syncobj) * kMaxSync); sync = nullptr; }
        IOUserClient::free();
    }
    IOReturn clientClose() APPLE_KEXT_OVERRIDE {
        if (lock && gpuOwner) {
            IOLockLock(lock);
            freeAll();                                  // the PTEs go invalid before the pages are released, so a late GPU access faults instead of corrupting memory
            IOLockUnlock(lock);
            if (gpuOwner->frontSurface != 0) gpuOwner->present(0, false);   // a program that flipped to its own surface and went away: the desktop's surface is shown again
        }
        if (!isInactive()) terminate();
        return kIOReturnSuccess;
    }

    // The buffer with handle `type` as the memory to map into the caller (IOConnectMapMemory64 with that type).
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE {
        if (!gpuOwner || !lock) return kIOReturnBadArgument;
        IOLockLock(lock);
        const int idx = boOf(type);
        IOMemoryDescriptor *md = idx < 0 ? nullptr : gpuOwner->bos[idx].dom ? gpuOwner->bos[idx].vmd : static_cast<IOMemoryDescriptor *>(gpuOwner->bos[idx].md);
        const bool vis = idx >= 0 && gpuOwner->bos[idx].dom == RN_BO_VRAM_VISIBLE;
        if (md) md->retain();
        IOLockUnlock(lock);
        if (!md) return kIOReturnBadArgument;                     // not a buffer, or a device-local one: no CPU mapping
        *options = vis ? kIOMapWriteCombineCache : 0;
        *memory = md;
        return kIOReturnSuccess;
    }

    // Exact struct sizes in and out, see rn_uapi.h.
    static bool sized(const IOExternalMethodArguments *a, size_t in, size_t out) {
        return a->structureInputSize == in && a->structureOutputSize == out && (!in || a->structureInput) && (!out || a->structureOutput) &&
               a->scalarInputCount == 0 && a->scalarOutputCount == 0;
    }

    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) APPLE_KEXT_OVERRIDE {
        if (!gpuOwner || !args || !lock) return kIOReturnBadArgument;
        IOLockLock(lock);
        IOReturn rc = kIOReturnBadArgument;
        switch (selector) {
        case RN_UC_INFO:
            if (sized(args, 0, sizeof(rn_uc_info))) {
                rn_uc_info *o = static_cast<rn_uc_info *>(args->structureOutput);
                memset(o, 0, sizeof(*o));
                o->version = RN_UAPI_VERSION;
                o->doorbell_mapped = gpuOwner->doorbellKeep != nullptr;
                o->gart_base = 0x2000000000ull;
                o->gart_bytes = gpuOwner->boBudget;                // no longer a window: the host memory the kext may wire for buffers
                o->max_bo_bytes = RedNaviProbe::kBoMaxBytes;
                o->bo_count = boUsed;
                o->bo_limit = kMaxHandles;
                o->vm_va_start = RedNaviProbe::kVmMin;
                o->vm_va_bytes = (1ull << 47) - RedNaviProbe::kVmMin;                // the lower half; the upper half is 0xffff800000000000 up to 2^64 - 4 MB
                o->vmid = static_cast<uint32_t>(vmid);
                o->va_map_limit = kMaxMaps;
                o->gfx_queue = gpuOwner->gfxQueueOk ? 1 : 0;
                if (gpuOwner->fbReady) { o->fb_width = gpuOwner->selMode.hactive; o->fb_height = gpuOwner->selMode.vactive; o->fb_pitch = gpuOwner->selPitch * 4; o->fb_surfaces = gpuOwner->scanoutSurfaces(); }
                o->vram_bytes = gpuOwner->devPool.totalPages * 4096; o->vram_free = gpuOwner->devPool.freePages * 4096;
                o->vram_vis_bytes = gpuOwner->visPool.totalPages * 4096; o->vram_vis_free = gpuOwner->visPool.freePages * 4096;
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_BO_CREATE:
            if (sized(args, sizeof(rn_uc_bo_create), sizeof(rn_uc_bo_info))) {
                const rn_uc_bo_create *in = static_cast<const rn_uc_bo_create *>(args->structureInput);
                const bool scan = in->flags == RN_BO_SCANOUT;
                if (!scan && ((in->flags & ~(RN_BO_VRAM | RN_BO_VRAM_VISIBLE)) != 0 || in->flags == (RN_BO_VRAM | RN_BO_VRAM_VISIBLE) || in->size == 0 || in->size > RedNaviProbe::kBoMaxBytes)) break;
                uint32_t h = 0;
                while (h < kMaxHandles && boIdx[h] >= 0) h++;
                if (h == kMaxHandles) { rc = kIOReturnNoResources; break; }
                const int idx = scan ? gpuOwner->createScanoutBo(in->pad)
                                : in->flags ? gpuOwner->createVramBo(static_cast<uint32_t>(in->size), in->flags == RN_BO_VRAM_VISIBLE)
                                          : gpuOwner->createHostBo(static_cast<uint32_t>(in->size), false, false);   // not in the GART window: the address spaces map it from its page list
                if (idx < 0) { rc = kIOReturnNoResources; break; }
                boIdx[h] = idx;
                boUsed++;
                rn_uc_bo_info *o = static_cast<rn_uc_bo_info *>(args->structureOutput);
                memset(o, 0, sizeof(*o));
                o->handle = h + 1;
                o->gpu_va = gpuOwner->bos[idx].gpuVa;
                o->size = gpuOwner->bos[idx].size;
                o->flags = gpuOwner->bos[idx].dom;
                if (gpuOwner->bos[idx].dom) o->gpu_va = gpuOwner->bos[idx].vramOff;           // a VRAM buffer: its offset in VRAM
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_BO_FREE:
            if (sized(args, sizeof(rn_uc_bo_free), 0)) {
                const uint32_t handle = static_cast<const rn_uc_bo_free *>(args->structureInput)->handle;
                const int idx = boOf(handle);
                if (idx < 0) break;
                for (uint32_t i = 0; i < kMaxMaps; i++) if (maps[i].used && maps[i].handle == handle) unmapSlot(i);
                gpuOwner->destroyBo(idx);
                boIdx[handle - 1] = -1;
                boUsed--;
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_SUBMIT:
            if (sized(args, sizeof(rn_uc_submit), sizeof(rn_uc_submit_result)) && fenceIdx >= 0) {
                const rn_uc_submit *in = static_cast<const rn_uc_submit *>(args->structureInput);
                uint64_t ibVa = 0;
                const bool gfx = (in->flags & RN_SUBMIT_GFX) != 0;
                if ((in->flags & ~RN_SUBMIT_GFX) != 0 || (gfx && !gpuOwner->gfxQueueOk) || in->ib_dwords == 0 || in->ib_dwords > 0xFFFFF || (in->ib_va & 3) || !canonical(in->ib_va, in->ib_dwords * 4ull, &ibVa) || !inMap(ibVa, in->ib_dwords * 4ull)) break;
                // A command buffer in the upper half of the address space hangs the compute queue (docs/COMPAT.md): the command processor does not run it and later
                // dispatches of every connection stay dead until the next boot, a reset does not bring them back. Data and shaders up there work, so only this is refused.
                if (ibVa >= (1ull << 47)) { rc = kIOReturnUnsupported; break; }
                if (in->in_count > RN_UC_SUBMIT_SYNC_MAX || in->out_count > RN_UC_SUBMIT_SYNC_MAX) break;
                bool badSync = false;
                for (uint32_t i = 0; i < in->in_count; i++) if (!findSync(in->in[i].handle)) badSync = true;
                for (uint32_t i = 0; i < in->out_count; i++) { Syncobj *o = findSync(in->out[i].handle); if (!o || !canAttach(*o, in->out[i].point)) badSync = true; }
                if (badSync) break;
                IOReturn waitRc = kIOReturnSuccess;                                 // the objects this work waits for, before anything goes to the ring
                for (uint32_t i = 0; i < in->in_count && waitRc == kIOReturnSuccess; i++) waitRc = syncWaitOne(*findSync(in->in[i].handle), in->in[i].point, 10000, true, false);
                if (waitRc != kIOReturnSuccess) { rc = waitRc; break; }
                IOLockLock(gpuOwner->queueLock);
                uint32_t seq = ((seqSubmitted + 1) & 0x7fffffffu) ? ((seqSubmitted + 1) & 0x7fffffffu) : 1;
                if (gfx) seq = ((gfxSeqSubmitted + 1) & 0x7fffffffu) | 0x80000000u;
                rn_gc_safe_mode(&gpuOwner->gpu, 1);                  // the graphics block stays awake until the wait returns
                int r = gfx ? rn_gfx_submit_ib(&gpuOwner->gpu, ibVa, in->ib_dwords, gpuOwner->bos[fenceIdx].gpuVa + 8, seq, static_cast<uint32_t>(vmid))
                            : rn_compute_submit_ib(&gpuOwner->gpu, kRNVramMcBase, gpuOwner->computeRingOff, 4096, ibVa, in->ib_dwords, gpuOwner->bos[fenceIdx].gpuVa, seq, static_cast<uint32_t>(vmid));
                if (r == -EAGAIN && !gfx) {                                  // the ring stayed full for about 2 s: the queue is not draining, it is stuck
                    uint32_t info[4];
                    const int rr = gpuOwner->resetQueue(info);
                    IOLog("RedNavi: ring full and not draining, queue reset: %d (step %u)\n", rr, info[0]);
                    if (rr == 0) r = rn_compute_submit_ib(&gpuOwner->gpu, kRNVramMcBase, gpuOwner->computeRingOff, 4096, ibVa, in->ib_dwords, gpuOwner->bos[fenceIdx].gpuVa, seq, static_cast<uint32_t>(vmid));
                }
                if (r == -EAGAIN && gfx) {
                    uint32_t info[4];
                    const int rr = gpuOwner->resetGfxQueue(info);
                    IOLog("RedNavi: graphics ring full and not draining, queue reset: %d (step %u)\n", rr, info[0]);
                    if (rr == 0) r = rn_gfx_submit_ib(&gpuOwner->gpu, ibVa, in->ib_dwords, gpuOwner->bos[fenceIdx].gpuVa + 8, seq, static_cast<uint32_t>(vmid));
                }
                if (r == 0) { if (gfx) gfxSeqSubmitted = seq; else seqSubmitted = seq; }   // taken under the queue lock, in the order the packets reach the ring
                IOLockUnlock(gpuOwner->queueLock);
                if (r == 0) {
                    for (uint32_t i = 0; i < in->out_count; i++) attachWork(*findSync(in->out[i].handle), in->out[i].point, seq);
                    rn_uc_submit_result *o = static_cast<rn_uc_submit_result *>(args->structureOutput);
                    memset(o, 0, sizeof(*o));
                    o->seq = seq;
                    rc = kIOReturnSuccess;
                } else {
                    rc = r == -EAGAIN ? kIOReturnBusy : kIOReturnNoSpace;
                }
            }
            break;
        case RN_UC_WAIT_SEQ:
            if (sized(args, sizeof(rn_uc_wait_seq), sizeof(rn_uc_wait_seq_result)) && fenceIdx >= 0) {
                const rn_uc_wait_seq *in = static_cast<const rn_uc_wait_seq *>(args->structureInput);
                const uint32_t ring = in->seq >> 31;
                const uint32_t submitted = ring ? gfxSeqSubmitted : seqSubmitted;
                if ((in->seq & 0x7fffffffu) == 0 || (ring && !gfxSeqSubmitted) || ((submitted - in->seq) & 0x7fffffffu) >= 0x40000000u) break;   // never submitted
                const uint32_t timeout = in->timeout_ms > 10000 ? 10000 : in->timeout_ms;
                const uint32_t fence0 = fenceWord(ring);
                uint32_t ms = 0;
                while (ms < timeout && !seqDone(fenceWord(ring), in->seq)) {
                    IOSleep(1);
                    ms++;
                    if (ms == gpuOwner->hangMs && fenceWord(ring) == fence0) {            // no progress for the whole hang time: the queue is stuck
                        uint32_t info[4];
                        IOLockLock(gpuOwner->queueLock);
                        const int rr = ring ? gpuOwner->resetGfxQueue(info) : gpuOwner->resetQueue(info);
                        IOLockUnlock(gpuOwner->queueLock);
                        IOLog("RedNavi: queue hung while waiting for sequence %u, reset: %d (step %u)\n", in->seq, rr, info[0]);
                    }
                }
                rn_uc_wait_seq_result *o = static_cast<rn_uc_wait_seq_result *>(args->structureOutput);
                memset(o, 0, sizeof(*o));
                IOLockLock(gpuOwner->queueLock);
                o->signaled = fenceWord(ring);
                o->reached = seqDone(o->signaled, in->seq);
                {
                    const uint32_t from = ring ? gfxLostFrom : lostFrom, to = ring ? gfxLostTo : lostTo;
                    if (o->reached && static_cast<int32_t>(in->seq - from) >= 0 && static_cast<int32_t>(to - in->seq) >= 0) o->status = RN_UC_SEQ_LOST;
                }
                IOLockUnlock(gpuOwner->queueLock);
                if (timeout) rn_gc_safe_mode(&gpuOwner->gpu, 0);
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_PRESENT:
            if (sized(args, sizeof(rn_uc_present), sizeof(rn_uc_present_result))) {
                const rn_uc_present *in = static_cast<const rn_uc_present *>(args->structureInput);
                if ((in->flags & ~RN_PRESENT_WAIT) != 0) break;
                const int pr = gpuOwner->present(in->surface, (in->flags & RN_PRESENT_WAIT) != 0);
                if (pr < 0) break;
                rn_uc_present_result *o = static_cast<rn_uc_present_result *>(args->structureOutput);
                memset(o, 0, sizeof(*o));
                o->status = static_cast<uint32_t>(pr);
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_VRAM_PEEK:
            if (sized(args, sizeof(rn_uc_vram_peek), sizeof(rn_uc_vram_peek_result))) {
                const rn_uc_vram_peek *in = static_cast<const rn_uc_vram_peek *>(args->structureInput);
                if (in->count == 0 || in->count > 64 || (in->offset & 3) || in->offset >= (static_cast<uint64_t>(gpuOwner->vramMbytes) << 20) - 256) break;
                rn_uc_vram_peek_result *o = static_cast<rn_uc_vram_peek_result *>(args->structureOutput);
                memset(o, 0, sizeof(*o));
                for (uint32_t i = 0; i < in->count; i++) o->v[i] = gpuOwner->vramRead(in->offset + i * 4ull);
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_BO_WAIT:
            if (sized(args, sizeof(rn_uc_bo_wait), sizeof(rn_uc_bo_wait_result))) {
                const rn_uc_bo_wait *in = static_cast<const rn_uc_bo_wait *>(args->structureInput);
                const int idx = boOf(in->handle);
                if (idx < 0 || gpuOwner->bos[idx].dom || in->offset > gpuOwner->bos[idx].size - 4 || (in->offset & 3)) break;   // host buffers only
                uint32_t timeout = in->timeout_ms > 10000 ? 10000 : in->timeout_ms;
                volatile uint32_t *w = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(gpuOwner->bos[idx].md->getBytesNoCopy()) + in->offset);
                uint32_t ms = 0;
                while (ms < timeout && *w != in->value) { IOSleep(1); ms++; }
                rn_uc_bo_wait_result *o = static_cast<rn_uc_bo_wait_result *>(args->structureOutput);
                memset(o, 0, sizeof(*o));
                o->reached = *w == in->value;
                rn_gc_safe_mode(&gpuOwner->gpu, 0);
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_VA_MAP:
            if (sized(args, sizeof(rn_uc_va_map), 0)) {
                const rn_uc_va_map *in = static_cast<const rn_uc_va_map *>(args->structureInput);
                const int idx = boOf(in->handle);
                uint64_t va = 0;
                if (idx < 0 || in->flags != 0 || in->size == 0 || ((in->va | in->offset | in->size) & 4095) || !canonical(in->va, in->size, &va)) break;
                if (va < RedNaviProbe::kVmMin || in->size > RedNaviProbe::kVmEnd - va) break;
                if (in->offset > gpuOwner->bos[idx].size || in->size > gpuOwner->bos[idx].size - in->offset) break;
                bool clash = false;
                uint32_t slot = kMaxMaps;
                for (uint32_t i = 0; i < kMaxMaps; i++) {
                    if (!maps[i].used) { if (slot == kMaxMaps) slot = i; continue; }
                    if (va < maps[i].va + maps[i].size && maps[i].va < va + in->size) clash = true;
                }
                if (clash) break;
                if (slot == kMaxMaps) { rc = kIOReturnNoResources; break; }
                if (!gpuOwner->vmMap(vmid, idx, static_cast<uint32_t>(in->offset / 4096), va / 4096, static_cast<uint32_t>(in->size / 4096))) { rc = kIOReturnNoResources; break; }
                maps[slot] = {va, in->size, in->handle, true};
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_VA_UNMAP:
            if (sized(args, sizeof(rn_uc_va_unmap), 0)) {
                const rn_uc_va_unmap *in = static_cast<const rn_uc_va_unmap *>(args->structureInput);
                uint64_t va = 0;
                if (!canonical(in->va, in->size, &va)) break;
                for (uint32_t i = 0; i < kMaxMaps; i++)
                    if (maps[i].used && maps[i].va == va && maps[i].size == in->size) { unmapSlot(i); rc = kIOReturnSuccess; break; }
            }
            break;
        case RN_UC_INFO_QUERY:
            if (args->structureInputSize == sizeof(drm_amdgpu_info) && args->structureInput && args->structureOutput && args->structureOutputSize > 0 && args->structureOutputSize <= 4096 &&
                args->scalarInputCount == 0 && args->scalarOutputCount == 0) {
                const drm_amdgpu_info *req = static_cast<const drm_amdgpu_info *>(args->structureInput);
                const uint32_t outSize = req->return_size < args->structureOutputSize ? req->return_size : args->structureOutputSize;
                rc = infoQuery(req, args->structureOutput, outSize);
            }
            break;
        case RN_UC_SYNCOBJ_CREATE:
            if (sized(args, sizeof(rn_uc_syncobj_create), sizeof(rn_uc_syncobj_create_result))) {
                const rn_uc_syncobj_create *in = static_cast<const rn_uc_syncobj_create *>(args->structureInput);
                if (in->flags & ~RN_UC_SYNCOBJ_CREATE_SIGNALED) break;
                uint32_t h = 0;
                while (h < kMaxSync && sync[h].used) h++;
                if (h == kMaxSync) { rc = kIOReturnNoResources; break; }
                memset(&sync[h], 0, sizeof(sync[h]));
                sync[h].used = true;
                sync[h].binSignaled = (in->flags & RN_UC_SYNCOBJ_CREATE_SIGNALED) != 0;
                rn_uc_syncobj_create_result *o = static_cast<rn_uc_syncobj_create_result *>(args->structureOutput);
                memset(o, 0, sizeof(*o));
                o->handle = h + 1;
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_SYNCOBJ_DESTROY:
            if (sized(args, sizeof(rn_uc_syncobj_destroy), 0)) {
                Syncobj *so = findSync(static_cast<const rn_uc_syncobj_destroy *>(args->structureInput)->handle);
                if (!so) break;
                so->used = false;
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_SYNCOBJ_WAIT:
            if (sized(args, sizeof(rn_uc_syncobj_wait), sizeof(rn_uc_syncobj_wait_result))) {
                const rn_uc_syncobj_wait *in = static_cast<const rn_uc_syncobj_wait *>(args->structureInput);
                if (in->count == 0 || in->count > RN_UC_SYNC_MAX || in->timeout_nsec < 0 || (in->flags & ~7u)) break;
                Syncobj *so[RN_UC_SYNC_MAX];
                bool bad = false;
                for (uint32_t i = 0; i < in->count; i++) { so[i] = findSync(in->handles[i]); if (!so[i]) bad = true; }
                if (bad) break;
                const bool all = in->flags & RN_UC_SYNCOBJ_WAIT_ALL, forSubmit = in->flags & RN_UC_SYNCOBJ_WAIT_FOR_SUBMIT, availOnly = in->flags & RN_UC_SYNCOBJ_WAIT_AVAILABLE;
                const uint64_t ns = static_cast<uint64_t>(in->timeout_nsec) > 10000000000ull ? 10000000000ull : static_cast<uint64_t>(in->timeout_nsec);
                const uint32_t timeoutMs = static_cast<uint32_t>(ns / 1000000);
                uint32_t ms = 0;
                for (;;) {
                    uint32_t nDone = 0, first = 0xffffffffu;
                    bool noWork = false;
                    for (uint32_t i = 0; i < in->count; i++) {
                        const bool ok = availOnly ? isAttached(*so[i], in->points[i]) : isDone(*so[i], in->points[i]);
                        if (ok) { nDone++; if (first == 0xffffffffu) first = i; }
                        else if (!forSubmit && !isAttached(*so[i], in->points[i])) noWork = true;
                    }
                    if (noWork) { rc = kIOReturnBadArgument; break; }       // as drm_syncobj_array_wait_timeout: an object nothing was ever attached to is an error unless the caller waits for a submit
                    if (all ? nDone == in->count : nDone > 0) {
                        rn_uc_syncobj_wait_result *o = static_cast<rn_uc_syncobj_wait_result *>(args->structureOutput);
                        memset(o, 0, sizeof(*o));
                        o->first_signaled = first == 0xffffffffu ? 0 : first;
                        rc = kIOReturnSuccess;
                        break;
                    }
                    if (ms >= timeoutMs) { rc = kIOReturnTimeout; break; }
                    IOSleep(1);
                    ms++;
                }
            }
            break;
        case RN_UC_SYNCOBJ_RESET:
        case RN_UC_SYNCOBJ_SIGNAL:
            if (sized(args, sizeof(rn_uc_syncobj_array), 0)) {
                const rn_uc_syncobj_array *in = static_cast<const rn_uc_syncobj_array *>(args->structureInput);
                if (in->count == 0 || in->count > RN_UC_SYNC_MAX) break;
                Syncobj *so[RN_UC_SYNC_MAX];
                bool bad = false;
                for (uint32_t i = 0; i < in->count; i++) { so[i] = findSync(in->handles[i]); if (!so[i]) bad = true; }
                if (bad) break;
                for (uint32_t i = 0; i < in->count; i++) {
                    if (selector == RN_UC_SYNCOBJ_RESET) { so[i]->binSignaled = false; so[i]->binSeq = 0; }
                    else signalNow(*so[i], in->points[i]);
                }
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_SYNCOBJ_QUERY:
            if (sized(args, sizeof(rn_uc_syncobj_array), sizeof(rn_uc_syncobj_array))) {
                const rn_uc_syncobj_array *in = static_cast<const rn_uc_syncobj_array *>(args->structureInput);
                if (in->count == 0 || in->count > RN_UC_SYNC_MAX) break;
                rn_uc_syncobj_array *o = static_cast<rn_uc_syncobj_array *>(args->structureOutput);
                bool bad = false;
                for (uint32_t i = 0; i < in->count; i++) if (!findSync(in->handles[i])) bad = true;
                if (bad) break;
                memcpy(o, in, sizeof(*o));
                for (uint32_t i = 0; i < in->count; i++) o->points[i] = tlNow(*findSync(in->handles[i]));
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_SYNCOBJ_TRANSFER:
            if (sized(args, sizeof(rn_uc_syncobj_transfer), 0)) {
                const rn_uc_syncobj_transfer *in = static_cast<const rn_uc_syncobj_transfer *>(args->structureInput);
                Syncobj *src = findSync(in->src_handle), *dst = findSync(in->dst_handle);
                if (!src || !dst || in->flags != 0) break;
                if (isDone(*src, in->src_point)) { signalNow(*dst, in->dst_point); rc = kIOReturnSuccess; break; }
                uint32_t seq = 0;                                                     // not done yet: the destination waits for the same work
                if (in->src_point == 0) seq = src->binSeq;
                else for (uint32_t i = 0; i < src->npend; i++) if (src->pend[i].point >= in->src_point) { seq = src->pend[i].seq; break; }
                if (seq == 0 || !canAttach(*dst, in->dst_point)) break;
                attachWork(*dst, in->dst_point, seq);
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_FB_OVERRIDE: {
            if (args->structureInputSize % 24 != 0 || args->structureInputSize > 24 * 16) break;
            IOService *fb = nullptr;
            if (OSIterator *it = gpuOwner->getClientIterator()) {
                while (OSObject *o = it->getNextObject()) {
                    IOService *c = OSDynamicCast(IOService, o);
                    if (c && !strcmp(c->getMetaClass()->getClassName(), "AMDRedNaviFB")) { fb = c; break; }
                }
                it->release();
            }
            if (!fb) { rc = kIOReturnNotFound; break; }
            OSData *d = OSData::withBytes(args->structureInput, args->structureInputSize);
            OSDictionary *dict = OSDictionary::withCapacity(1);
            if (d && dict) { dict->setObject("attr-override", d); rc = fb->setProperties(dict); }
            if (d) d->release();
            if (dict) dict->release();
            break;
        }
        case RN_UC_QUEUE_RESET:
            if ((sized(args, 0, sizeof(rn_uc_reset_result)) || sized(args, sizeof(uint32_t), sizeof(rn_uc_reset_result))) && fenceIdx >= 0) {
                const bool gfx = args->structureInputSize != 0 && *static_cast<const uint32_t *>(args->structureInput) == 1;   // the optional input names the queue: 1 graphics, otherwise compute
                if (gfx && !gpuOwner->gfxQueueOk) break;
                uint32_t info[4];
                IOLockLock(gpuOwner->queueLock);
                const int rr = gfx ? gpuOwner->resetGfxQueue(info) : gpuOwner->resetQueue(info);
                rn_uc_reset_result *o = static_cast<rn_uc_reset_result *>(args->structureOutput);
                memset(o, 0, sizeof(*o));
                o->status = rr == 0 ? 0 : (info[0] ? info[0] : 0xff);
                o->resets = gfx ? gpuOwner->gfxQueueResets : gpuOwner->queueResets;
                o->lost_from = gfx ? gfxLostFrom : lostFrom;
                o->lost_to = gfx ? gfxLostTo : lostTo;
                IOLockUnlock(gpuOwner->queueLock);
                rc = kIOReturnSuccess;
            }
            break;
        case RN_UC_DIAG:
            if (sized(args, sizeof(rn_uc_diag_req), sizeof(rn_uc_diag))) {
                const rn_uc_diag_req *req = static_cast<const rn_uc_diag_req *>(args->structureInput);
                rn_uc_diag *o = static_cast<rn_uc_diag *>(args->structureOutput);
                memset(o, 0, sizeof(*o));
                u32 dg[11], d2[8], rs[4];
                rn_compute_diag(&gpuOwner->gpu, dg);
                rn_compute_diag2(&gpuOwner->gpu, d2);
                rn_compute_ring_state(&gpuOwner->gpu, rs);
                o->hqd_rptr = dg[0]; o->hqd_wptr = dg[1]; o->hqd_active = dg[2]; o->cp_stat = dg[3];
                o->fault_status = dg[4]; o->fault_addr_lo = dg[5]; o->fault_addr_hi = dg[6]; o->ring_wptr = dg[7];
                o->sdma0_version = dg[8]; o->grbm_status = dg[9]; o->rlc_gpm_stat = dg[10];
                o->hqd_doorbell_control = d2[0]; o->cp_pq_status = d2[1]; o->doorbell_range_lower = d2[2]; o->doorbell_range_upper = d2[3];
                o->doorbell_aper_en = d2[4]; o->pq_base = d2[5]; o->pq_control = d2[6]; o->mqd_base = d2[7];
                o->ring_rptr_word = rs[0]; o->ring_wptr_word = rs[1]; o->driver_wptr = rs[2]; o->ring_full_waits = rs[3];
                o->seq_submitted = seqSubmitted; o->seq_signaled = fenceIdx >= 0 ? fenceNow() : 0;
                { u32 fs[2]; rn_vm_flush_stats(fs); o->flush_failures = fs[0]; o->flush_last_ack = fs[1]; }
                o->queue_resets = gpuOwner->queueResets; o->gfx_queue_resets = gpuOwner->gfxQueueResets;
                rn_gfx_ring_pointers(&gpuOwner->gpu, &o->gfx_rptr, &o->gfx_wptr);
                if (req->flags & RN_UC_DIAG_CLEAR_FAULT) rn_vm_clear_fault(&gpuOwner->gpu);
                rc = kIOReturnSuccess;
            }
            break;
        }
        IOLockUnlock(lock);
        return rc;
    }
};
OSDefineMetaClassAndStructors(IOAccelRedNaviClient, IOUserClient)

// The queue is shared by every connection, so a reset drops everybody's work. Each connection's numbers from the last fence value it saw to the last it
// submitted are lost; the fence then jumps to the last submitted number so that no wait is left hanging (the work that finished before keeps its result).
int RedNaviProbe::resetQueue(uint32_t info[4]) {
    uint32_t before[8];
    for (int v = 1; v <= 7; v++) before[v] = vmClient[v] ? vmClient[v]->fenceNow() : 0;
    const int rr = rn_compute_queue_reset(&gpu, info);
    queueResets++;
    if (rr != 0) return rr;
    for (int v = 1; v <= 7; v++) {
        IOAccelRedNaviClient *c = vmClient[v];
        if (!c) continue;
        c->lostFrom = before[v] + 1;
        c->lostTo = c->seqSubmitted;
        *reinterpret_cast<volatile uint32_t *>(bos[c->fenceIdx].md->getBytesNoCopy()) = c->seqSubmitted;
    }
    return 0;
}

// The graphics queue's reset: same bookkeeping, with the fence in dword 2 and sequence numbers that carry bit 31.
int RedNaviProbe::resetGfxQueue(uint32_t info[4]) {
    uint32_t before[8];
    for (int v = 1; v <= 7; v++) before[v] = vmClient[v] ? vmClient[v]->fenceWord(1) : 0;
    const int rr = rn_gfx_queue_reset(&gpu, info);
    gfxQueueResets++;
    if (rr != 0) return rr;
    for (int v = 1; v <= 7; v++) {
        IOAccelRedNaviClient *c = vmClient[v];
        if (!c) continue;
        c->gfxLostFrom = (before[v] + 1) | 0x80000000u;
        c->gfxLostTo = c->gfxSeqSubmitted;
        *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(bos[c->fenceIdx].md->getBytesNoCopy()) + 8) = c->gfxSeqSubmitted;
    }
    return 0;
}

IOReturn RedNaviProbe::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) {
    if (!gpuLive || !doorbellKeep || !handler) return kIOReturnNotReady;
    uint32_t anyone = 0;
    if (!(PE_parse_boot_argn("rednavi_uc_anyone", &anyone, sizeof(anyone)) && anyone == 1) &&
        IOUserClient::clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    IOAccelRedNaviClient *c = OSTypeAlloc(IOAccelRedNaviClient);
    if (!c) return kIOReturnNoMemory;
    if (!c->initWithTask(owningTask, securityID, type) || !c->attach(this) || !c->start(this)) {
        c->detach(this);
        c->release();
        return kIOReturnError;
    }
    *handler = c;
    return kIOReturnSuccess;
}

struct RNLockGuard {
    IOLock *l;
    explicit RNLockGuard(IOLock *lk) : l(lk) { if (l) IOLockLock(l); }
    ~RNLockGuard() { if (l) IOLockUnlock(l); }
};

void RedNaviProbe::destroyBo(int idx) {
    RNLockGuard g(boLock);
    if (!bos || idx < 0 || idx >= static_cast<int>(kBoCap) || !bos[idx].size || bos[idx].quarantined) return;
    if (bos[idx].gart) {
        const uint32_t firstPage = static_cast<uint32_t>((bos[idx].gpuVa - 0x2000000000ull) / 4096);
        for (uint32_t p = 0; p < bos[idx].size / 4096; p++) rn_gart_unmap_page(&gpu, &gmcLive, firstPage + p);
        if (rn_gart_flush(&gpu) != 0) {
            // The device may still cache a valid translation. Retain its DMA
            // mapping and backing until reboot; never recycle this BO slot.
            bos[idx].quarantined = true;
            IOLog("RedNavi: BO %d GART invalidation timed out; backing retained until reboot\n", idx);
            return;
        }
    }
    if (bos[idx].dom) {
        if (!bos[idx].fixed) (bos[idx].dom == RN_BO_VRAM_VISIBLE ? visPool : devPool).release(bos[idx].vramOff, bos[idx].size);
        if (bos[idx].vmd) bos[idx].vmd->release();
        bos[idx] = {};
        return;
    }
    if (bos[idx].bus) IOFree(bos[idx].bus, sizeof(uint64_t) * (bos[idx].size / 4096));
    boBytes -= bos[idx].size;
    if (bos[idx].dma) { bos[idx].dma->complete(); bos[idx].dma->clearMemoryDescriptor(); bos[idx].dma->release(); }
    bos[idx].md->release();
    bos[idx] = {};
}

static inline uint64_t rnRd64(struct rn_device *adev, uint64_t off) {
    return static_cast<uint64_t>(rn_vram_rd32(adev, off)) | (static_cast<uint64_t>(rn_vram_rd32(adev, off + 4)) << 32);
}

uint64_t RedNaviProbe::vmPageAlloc(int vmid) {
    uint32_t *pool = vmPool[vmid];
    for (uint32_t w = 0; w < kVmPoolPages / 32; w++) {
        if (pool[w] == 0xffffffffu) continue;
        uint32_t b = 0;
        while (pool[w] & (1u << b)) b++;
        pool[w] |= 1u << b;
        const uint64_t off = kVmTables + (static_cast<uint64_t>(vmid - 1) * kVmPoolPages + w * 32 + b) * 4096;
        for (uint32_t o = 0; o < 4096; o += 4) rn_vram_wr32(&gpu, off + o, 0);   // VRAM keeps its contents over a warm reboot
        return off;
    }
    return 0;
}

// The page table (PTB) that holds the PTE of the 4 KB page `vaPage`, walking PDB2, PDB1, PDB0 from the root; with `create` the missing directories
// and the table are made. A directory entry is the VRAM offset of the next table with the VALID bit (amdgpu_vm_pde_update, gmc_v11_0_get_vm_pde).
bool RedNaviProbe::vmPtb(int vmid, uint64_t vaPage, bool create, uint64_t *ptbOff) {
    uint64_t table = kVmTables + static_cast<uint64_t>(vmid - 1) * kVmPoolPages * 4096;   // the root is the first pool page
    for (int shift = 27; shift >= 9; shift -= 9) {
        const uint64_t slot = table + ((vaPage >> shift) & 0x1ff) * 8;
        uint64_t entry = rnRd64(&gpu, slot);
        if (!(entry & 1)) {
            if (!create) return false;
            const uint64_t next = vmPageAlloc(vmid);
            if (!next) return false;
            entry = next | 1;
            rn_vm_write_pte(&gpu, slot - ((vaPage >> shift) & 0x1ff) * 8, static_cast<uint32_t>((vaPage >> shift) & 0x1ff), entry);
        }
        table = entry & 0x0000FFFFFFFFF000ull;
    }
    *ptbOff = table;
    return true;
}

int RedNaviProbe::vmAlloc() {
    RNLockGuard g(boLock);
    if (!gpuLive) return -1;
    int vmid = -1;
    for (int v = 1; v <= 7; v++) if (!(vmidUsed & (1u << v))) { vmid = v; break; }
    if (vmid < 0) return -1;
    if (!vmReady) {
        rn_vm_enable_contexts(&gpu, 0, kVmEnd - 1, 3, 1, 7);
        vmReady = true;
    }
    memset(vmPool[vmid], 0, sizeof(vmPool[vmid]));
    const uint64_t root = vmPageAlloc(vmid);
    if (!root) return -1;
    rn_vm_bind(&gpu, vmid, root);
    vmidUsed |= 1u << vmid;
    return vmid;
}

void RedNaviProbe::vmFree(int vmid) {
    RNLockGuard g(boLock);
    if (vmid < 1 || vmid > 7 || !(vmidUsed & (1u << vmid))) return;
    rn_vm_bind(&gpu, vmid, 0);
    memset(vmPool[vmid], 0, sizeof(vmPool[vmid]));
    vmidUsed &= ~(1u << vmid);
}

bool RedNaviProbe::vmMap(int vmid, int boIdx, uint32_t boPage, uint64_t vaPage, uint32_t pages) {
    RNLockGuard g(boLock);
    const uint64_t *bus = bos[boIdx].bus;
    const bool vram = bos[boIdx].dom != 0;                 // a VRAM buffer is contiguous: page i is at kRNVramMcBase + vramOff + i * 4096
    const uint64_t vramPa = bos[boIdx].vramOff;           // the PTE of a local page holds the offset in VRAM, not the MC address (measured 2026-11-02: with 0x80_0000_0000 added the GPU's writes vanish)
    uint64_t ptb = 0;
    bool ok = true;
    uint32_t done = 0;
    for (; done < pages; done++) {
        const uint64_t vp = vaPage + done;
        if (done == 0 || (vp & 0x1ff) == 0) { if (!vmPtb(vmid, vp, true, &ptb)) { ok = false; break; } }
        rn_vm_write_pte(&gpu, ptb, static_cast<uint32_t>(vp & 0x1ff), vram ? rn_vram_pte(vramPa + static_cast<uint64_t>(boPage + done) * 4096) : rn_system_pte(bus[boPage + done]));
    }
    if (!ok) {                                       // leave nothing half mapped
        for (uint32_t p = 0; p < done; p++) {
            const uint64_t vp = vaPage + p;
            if (p == 0 || (vp & 0x1ff) == 0) { if (!vmPtb(vmid, vp, false, &ptb)) continue; }
            rn_vm_write_pte(&gpu, ptb, static_cast<uint32_t>(vp & 0x1ff), 0);
        }
    }
    rn_vm_flush(&gpu, vmid);
    return ok;
}

void RedNaviProbe::vmUnmap(int vmid, uint64_t vaPage, uint32_t pages) {
    RNLockGuard g(boLock);
    uint64_t ptb = 0;
    bool have = false;
    for (uint32_t p = 0; p < pages; p++) {
        const uint64_t vp = vaPage + p;
        if (p == 0 || (vp & 0x1ff) == 0) have = vmPtb(vmid, vp, false, &ptb);
        if (have) rn_vm_write_pte(&gpu, ptb, static_cast<uint32_t>(vp & 0x1ff), 0);
    }
    rn_vm_flush(&gpu, vmid);
}

int RedNaviProbe::createHostBo(uint32_t size, bool writeCombine, bool gart) {
    RNLockGuard g(boLock);
    if (!gpuLive || !bos || size == 0 || size > kBoMaxBytes) return -1;
    int slot = -1;
    for (uint32_t i = 0; i < boCount; i++) if (!bos[i].size) { slot = static_cast<int>(i); break; }
    if (slot < 0 && boCount < kBoCap) slot = static_cast<int>(boCount);
    if (slot < 0) return -1;
    size = (size + 4095u) & ~4095u;
    const uint32_t pages = size / 4096;
    if (gart && gartNextPage + pages > 16384) return -1;
    if (boBytes + size > boBudget) return -1;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | (writeCombine ? kIOMapWriteCombineCache : 0), size, 4096);
    if (!md) return -1;
    void *cpu = md->getBytesNoCopy();
    if (!cpu) { md->release(); return -1; }
    memset(cpu, 0, size);
    // The non-pageable IOBuffer is already wired. setMemoryDescriptor prepares
    // the DMA mapping; use the GMC11 DMA width, independently of GPU VA width.
    IODMACommand *dma = IODMACommand::withSpecification(kIODMACommandOutputHost64, rn_dma::kGmc11DmaAddressBits, 0, IODMACommand::kMapped, 0, 4096, nullptr, 0);
    if (!dma || dma->setMemoryDescriptor(md) != kIOReturnSuccess) {
        if (dma) dma->release();
        md->release();
        return -1;
    }
    uint64_t *bus = static_cast<uint64_t *>(IOMalloc(sizeof(uint64_t) * pages));
    if (!bus) {
        dma->complete(); dma->clearMemoryDescriptor(); dma->release();
        md->release();
        return -1;
    }
    const rn_dma::Result mapping = rn_dma::collectPages<IODMACommand::Segment64>(bus, pages, size,
        [dma](uint64_t *offset, IODMACommand::Segment64 *segments, uint32_t *count) {
            UInt64 dmaOffset = *offset;
            UInt32 dmaCount = *count;
            const IOReturn status = dma->genIOVMSegments(&dmaOffset, segments, &dmaCount);
            *offset = dmaOffset;
            *count = dmaCount;
            return status == kIOReturnSuccess;
        });
    if (!mapping.ok()) {
        IOLog("RedNavi: host BO DMA validation failed (%u); no GART entries installed\n", static_cast<unsigned>(mapping.error));
        IOFree(bus, sizeof(uint64_t) * pages);
        dma->complete(); dma->clearMemoryDescriptor(); dma->release();
        md->release();
        return -1;
    }
    // Install only after every DMA page has passed validation. Neither the
    // GART cursor nor any valid PTE changes on an enumeration failure.
    bool quarantined = false;
    if (gart) {
        for (uint32_t p = 0; p < pages; p++) rn_gart_map_page(&gpu, &gmcLive, gartNextPage + p, bus[p]);
        if (rn_gart_flush(&gpu) != 0) {
            for (uint32_t p = 0; p < pages; p++) rn_gart_unmap_page(&gpu, &gmcLive, gartNextPage + p);
            if (rn_gart_flush(&gpu) == 0) {
                IOFree(bus, sizeof(uint64_t) * pages);
                dma->complete(); dma->clearMemoryDescriptor(); dma->release();
                md->release();
                return -1;
            }
            // Rollback could not be acknowledged. Reserve the unexposed slot
            // and range so the device can never DMA into recycled memory.
            quarantined = true;
            IOLog("RedNavi: host BO GART rollback timed out; %u bytes retained until reboot\n", size);
        }
    }
    const int idx = slot;
    if (static_cast<uint32_t>(slot) == boCount) boCount++;
    bos[idx] = {md, dma, gart ? 0x2000000000ull + static_cast<uint64_t>(gartNextPage) * 4096 : 0, size, bus, gart};
    bos[idx].quarantined = quarantined;
    if (gart) gartNextPage += pages;
    boBytes += size;
    return quarantined ? -1 : idx;
}

// A buffer in VRAM, from the device-local pool or, for `visible`, from the part of the aperture the CPU can reach. A visible buffer is cleared through the aperture; a device-local
// one is not (nothing but the GPU can write it: rn_uapi.h).
int RedNaviProbe::createVramBo(uint32_t size, bool visible) {
    RNLockGuard g(boLock);
    int slot = -1;
    for (uint32_t i = 0; i < boCount; i++) if (!bos[i].size) { slot = static_cast<int>(i); break; }
    if (slot < 0 && boCount < kBoCap) slot = static_cast<int>(boCount);
    if (!gpuLive || !bos || slot < 0 || size == 0 || size > kBoMaxBytes) return -1;
    size = (size + 4095u) & ~4095u;
    VramPool &pool = visible ? visPool : devPool;
    uint64_t off = 0;
    if (!pool.alloc(size, &off)) return -1;
    IOMemoryDescriptor *vmd = nullptr;
    if (visible) {
        vmd = bar0Mem ? IODeviceMemory::withSubRange(bar0Mem, off, size) : nullptr;
        IOMemoryMap *km = vmd ? vmd->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
        if (!km) { if (vmd) vmd->release(); pool.release(off, size); return -1; }
        memset(reinterpret_cast<void *>(km->getVirtualAddress()), 0, size);
        km->release();
    }
    if (static_cast<uint32_t>(slot) == boCount) boCount++;
    bos[slot] = {nullptr, nullptr, 0, size, nullptr, false, vmd, off, visible ? RN_BO_VRAM_VISIBLE : RN_BO_VRAM};
    return slot;
}

// Surface 0 is the desktop's: RedNaviFB hands it to WindowServer, which keeps drawing into it. A program that presents uses the others (so that the desktop does not flash into its
// frames) and the display goes back to surface 0 when it leaves. They are packed, one MB-aligned surface after the other, in the 34 MB between the scanout and the CPU-visible pool.
uint64_t RedNaviProbe::scanoutStride() const {
    return ((static_cast<uint64_t>(selPitch) * 4 * selMode.vactive) + 0xFFFFFull) & ~0xFFFFFull;
}
uint32_t RedNaviProbe::scanoutSurfaces() const {
    const uint64_t n = 0x02200000ull / scanoutStride();
    return n > 3 ? 3 : static_cast<uint32_t>(n);
}

// A page flip, as hubp1_program_surface_flip_and_addr does it: vsync flip type (SURFACE_FLIP_TYPE 0, the value the pipe has), the high half of the address first and then the low
// half, which latches the pair for the next vertical update. Done when SURFACE_FLIP_PENDING is clear and the earliest address in use is the new one
// (hubp1_is_flip_pending). The pitch, size and format of the surface are those of the mode, which is why only surfaces of the current mode exist.
int RedNaviProbe::present(uint32_t surface, bool wait) {
    uint32_t b2 = 0;
    if (!fbReady || liveSwitch || surface >= scanoutSurfaces() || !ipBase(kRNHwIdDmu, &b2, 2)) return -EINVAL;
    const uint64_t mc = kRNVramMcBase + scanoutOff(surface);
    const uint32_t flipCtl = (b2 + 0x61b) * 4, addr = (b2 + 0x60a) * 4, addrHi = (b2 + 0x60b) * 4, earliest = (b2 + 0x624) * 4, earliestHi = (b2 + 0x625) * 4;
    wr(flipCtl, rd(flipCtl) & ~(1u << 1));
    wr(addrHi, static_cast<uint32_t>(mc >> 32));
    wr(addr, static_cast<uint32_t>(mc));
    frontSurface = surface;
    if (!wait) return 0;
    for (int i = 0; i < 5000; i++) {                                         // up to 50 ms: three frames at 60 Hz
        if (!(rd(flipCtl) & (1u << 8)) && rd(earliest) == static_cast<uint32_t>(mc) && rd(earliestHi) == static_cast<uint32_t>(mc >> 32)) return 0;
        IODelay(10);
    }
    return 1;
}

int RedNaviProbe::createScanoutBo(uint32_t surface) {
    RNLockGuard g(boLock);
    int slot = -1;
    for (uint32_t i = 0; i < boCount; i++) if (!bos[i].size) { slot = static_cast<int>(i); break; }
    if (slot < 0 && boCount < kBoCap) slot = static_cast<int>(boCount);
    const uint64_t bytes = (static_cast<uint64_t>(selPitch) * 4 * selMode.vactive + 4095) & ~4095ull;
    if (!gpuLive || !bos || slot < 0 || !bar0Mem || !fbReady || bytes == 0 || surface >= scanoutSurfaces() || scanoutOff(surface) + bytes > bar0Mem->getLength()) return -1;
    IOMemoryDescriptor *vmd = IODeviceMemory::withSubRange(bar0Mem, scanoutOff(surface), bytes);
    if (!vmd) return -1;
    if (static_cast<uint32_t>(slot) == boCount) boCount++;
    bos[slot] = {nullptr, nullptr, 0, static_cast<uint32_t>(bytes), nullptr, false, vmd, scanoutOff(surface), RN_BO_VRAM_VISIBLE, true};
    return slot;
}

// The MES bring-up result as ioreg properties (ioreg -rn RedNaviProbe -l -x). Out of line so that its name tables do not sit in sdmaTest's frame.
__attribute__((noinline)) void RedNaviProbe::publishMes(const struct rn_mes_result &mr) {
    static const char *const sn[] = {"step", "cntl", "p0_pc", "p1_pc", "kiq_active", "kiq_doorbell_ctl", "kiq_rptr", "sched_active", "ring_fence", "api_status", "hw_res_ok",
                                     "add_ok", "q_active", "q_doorbell_ctl", "fault_status", "sched_version", "kiq_version", "sched_rptr", "sched_wptr", "mepc", "mcause",
                                     "ring_fence_late", "p0_pc_end", "q_pipe", "q_queue", "workspace_error"};
    const uint32_t sv[] = {mr.step, mr.cntl, mr.p0_pc, mr.p1_pc, mr.kiq_active, mr.kiq_doorbell_ctl, mr.kiq_rptr, mr.sched_active, mr.ring_fence, mr.api_status,
                           static_cast<uint32_t>(mr.hw_res_ok), static_cast<uint32_t>(mr.add_ok), mr.q_active, mr.q_doorbell_ctl, mr.fault_status, mr.sched_version,
                           mr.kiq_version, mr.sched_rptr, mr.sched_wptr, mr.mepc, mr.mcause, mr.ring_fence_late, mr.p0_pc_end, mr.q_pipe, mr.q_queue, mr.workspace_error};
    rnPublish(this, "mes-start", sn, sv, sizeof(sv) / sizeof(sv[0]));
    static const char *const vn[] = {"sched", "kiq", "bootload", "rlc_srm", "cp_stat", "cp_me_cntl", "mec_cntl", "rlc_sched", "hqd_active"};
    const uint32_t vv[] = {mr.fw_ver0, mr.fw_ver1, mr.bootload, mr.rlc_srm, mr.cp_stat0, mr.cp_me_cntl, mr.mec_cntl, mr.rlc_sched, mr.hqd_active};
    rnPublish(this, "mes-fw-version", vn, vv, sizeof(vv) / sizeof(vv[0]));
    static const char *const isn[] = {"cntl", "rlc_sched", "pc0", "pc1", "act0", "act1", "db0", "db1", "mec_active", "bootload"};
    rnPublish(this, "mes-init", isn, mr.init_state, 10);
    static const char *const hn[] = {"pqb", "pqb_hi", "mqdb", "pqc", "dbc", "rptr", "wptr", "act", "rptr_rep", "wptr_poll"};
    rnPublish(this, "mes-sched-hqd", hn, mr.sched_hqd, 10);
    static const char *const fn[] = {"lo32", "hi32"};
    rnPublish(this, "mes-fault-addr", fn, mr.fault_addr, 2);
    static const char *const rn[] = {"d0", "d1", "d2", "d3", "rptr_w", "wptr_w", "fence", "api"};
    rnPublish(this, "mes-ring-head", rn, mr.ring_head, 8);
    static const char *const ktn[] = {"a_scratch", "a_pc", "b_scratch", "b_pc", "c_scratch", "c_pc"};
    rnPublish(this, "mes-kiq-test", ktn, mr.ktest, 6);
    static const char *const pn[] = {"cleaner", "flags"};
    static const char *const bn[] = {"gc0", "gc1", "gc2", "gc3", "mmhub0", "mmhub1", "mmhub2", "oss0", "oss1"};
    const uint32_t bv[] = {gpu.reg_offset[RN_GC_HWIP][0][0], gpu.reg_offset[RN_GC_HWIP][0][1], gpu.reg_offset[RN_GC_HWIP][0][2], gpu.reg_offset[RN_GC_HWIP][0][3],
                           gpu.reg_offset[RN_MMHUB_HWIP][0][0], gpu.reg_offset[RN_MMHUB_HWIP][0][1], gpu.reg_offset[RN_MMHUB_HWIP][0][2],
                           gpu.reg_offset[RN_OSSSYS_HWIP][0][0], gpu.reg_offset[RN_OSSSYS_HWIP][0][1]};
    rnPublish(this, "mes-bases", bn, bv, 9);   // the register bases SET_HW_RESOURCES gave the MES
}

// The memory image of the compute queue (MQD at VRAM 0x0C022000): the dwords the firmware may save and restore the queue state through.
void RedNaviProbe::mqdSnapshot(const char *property) {
    static const uint32_t dw[12] = {130, 132, 136, 137, 138, 139, 143, 145, 162, 167, 182, 183};
    static const char *nm[12] = {"active", "persist", "pq_base_lo", "pq_base_hi", "rptr", "rptr_addr_lo", "doorbell_ctl", "pq_control", "mqd_control", "eop_control", "wptr_lo", "wptr_hi"};
    uint32_t v[12];
    for (int i = 0; i < 12; i++) v[i] = rn_vram_rd32(&gpu, 0x0C022000ull + dw[i] * 4);
    rnPublish(this, property, nm, v, 12);
}

void RedNaviProbe::watchStep() {
    uint32_t d1[11], d2[8];
    rn_compute_diag(&gpu, d1);
    rn_compute_diag2(&gpu, d2);
    const uint32_t cur[5] = {d2[0], d2[6], d2[1], d1[0], d1[1]};   // doorbell control, pq control, pq status, rptr, wptr
    const bool first = watchTick == 0;
    bool changed = first;
    // the rptr/wptr move with the work; the control state is what must not change by itself
    if (cur[0] != watchPrev[0] || cur[1] != watchPrev[1] || cur[2] != watchPrev[2]) changed = true;
    {   // the PCI command register (bit 2: bus master) and status, at every change and at the start
        const uint32_t cmd = pci->configRead16(kIOPCIConfigCommand), st = pci->configRead16(kIOPCIConfigStatus);
        if (first || cmd != watchPciCmd) {
            const char *pn[3] = {"tick", "command", "status"};
            uint32_t pv[3] = {watchTick, cmd, st};
            rnPublish(this, first ? "pci-cmd-start" : "pci-cmd-changed", pn, pv, 3);
            watchPciCmd = cmd;
        }
    }
    if (first) mqdSnapshot("queue-mqd-before");
    if (changed && !first && watchEvents < 8 && watchEvents == 1) mqdSnapshot("queue-mqd-after");
    if (changed && watchEvents < 8) {
        uint32_t *e = watchLog[watchEvents++];
        e[0] = watchTick; e[1] = cur[0]; e[2] = cur[1]; e[3] = cur[2]; e[4] = cur[3]; e[5] = cur[4]; e[6] = d1[3];
        char names[56][16];
        const char *np[56];
        uint32_t vals[56];
        for (uint32_t k = 0; k < watchEvents; k++)
            for (int j = 0; j < 7; j++) {
                static const char *f[7] = {"tick", "dbctl", "pqctl", "pqst", "rptr", "wptr", "cpstat"};
                snprintf(names[k * 7 + j], 16, "e%u_%s", k, f[j]);
                np[k * 7 + j] = names[k * 7 + j];
                vals[k * 7 + j] = watchLog[k][j];
            }
        rnPublish(this, "queue-watch", np, vals, watchEvents * 7);
    }
    for (int i = 0; i < 5; i++) watchPrev[i] = cur[i];
    // Timed resubmissions of the same command buffer from the kernel (10, 30 and 80 s after the test): does an idle queue still work
    // without a user client in between? The result is read 2 s later.
    if (watchBoA >= 0 && watchBoB >= 0 && !keepAlive) {
        volatile uint32_t *B = static_cast<volatile uint32_t *>(bos[watchBoB].md->getBytesNoCopy());
        static const uint32_t at[3] = {40, 120, 320};
        for (uint32_t k = 0; k < 3; k++) {
            if (watchTick == at[k]) {
                B[0] = 0xDEAD0000u; B[1] = 0;
                resubLog[k][0] = at[k];
                uint32_t ib[48];
                (void)ib;
                rn_gc_safe_mode(&gpu, 1);
                // the command buffer in buffer A is still intact; run it again with a new fence value
                rn_compute_submit_ib(&gpu, kRNVramMcBase, computeRingOff, 4096, bos[watchBoA].gpuVa, watchIbDwords, bos[watchBoB].gpuVa + 4, 0xBEEF0000u + k, 0);
            }
            if (watchTick == at[k] + 8) {
                resubLog[k][1] = B[0]; resubLog[k][2] = B[1];
                resubLog[k][3] = d2[0]; resubLog[k][4] = d1[0]; resubLog[k][5] = d1[1]; resubLog[k][6] = d2[6];
                rn_gc_safe_mode(&gpu, 0);
                resubCount = k + 1;
                const char *f[7] = {"tick", "b0", "b1", "dbctl", "rptr", "wptr", "pqctl"};
                char names[21][16];
                const char *np[21];
                uint32_t vals[21];
                for (uint32_t kk = 0; kk < resubCount; kk++)
                    for (int j = 0; j < 7; j++) { snprintf(names[kk * 7 + j], 16, "r%u_%s", kk, f[j]); np[kk * 7 + j] = names[kk * 7 + j]; vals[kk * 7 + j] = resubLog[kk][j]; }
                rnPublish(this, "queue-resubmit", np, vals, resubCount * 7);
            }
        }
    }
    if (keepAlive && watchBoA >= 0 && watchBoB >= 0 && watchTick >= 4 && watchTick < 440) {
        volatile uint32_t *B = static_cast<volatile uint32_t *>(bos[watchBoB].md->getBytesNoCopy());
        if (kaSent && B[1] == 0xCAFE0000u + kaSent) kaOk++;                       // the previous one finished
        else if (kaSent && !kaFirstFail) { kaFirstFail = watchTick; kaDbctl = d2[0]; }
        if (!kaFirstFail) {
            kaSent++;
            B[0] = 0xDEAD0000u;
            rn_compute_submit_ib(&gpu, kRNVramMcBase, computeRingOff, 4096, bos[watchBoA].gpuVa, watchIbDwords, bos[watchBoB].gpuVa + 4, 0xCAFE0000u + kaSent, 0);
        }
        const char *kn[5] = {"sent", "ok", "first_fail_tick", "dbctl_at_fail", "tick"};
        uint32_t kv[5] = {kaSent, kaOk, kaFirstFail, kaDbctl, watchTick};
        if ((watchTick & 7) == 0 || kaFirstFail) rnPublish(this, "keepalive", kn, kv, 5);
    }
    watchTick++;
    if (watchTimer && watchTick < 480) watchTimer->setTimeoutMS(250);
}

void RedNaviProbe::sdmaTest() {
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_sdma_test", &arg, sizeof(arg)) || arg != 1) return;
    uint32_t gc1 = 0;
    if (!ipBase(kRNHwIdGc, &gc1, 1)) return;
    uint32_t boot = rd((gc1 + 0x4e82) * 4);
    if (!(boot & 0x80000000u)) { IOLog("RedNavi: SDMA test: the RLC has not booted (BOOTLOAD_STATUS 0x%08x), skipping\n", boot); return; }
    // With the SMU features on, GFXOFF powers the graphics block down when it is idle and its registers read back as zero (the SDMA
    // status did). Linux brackets register access with amdgpu_gfx_off_ctrl; here GFXOFF is simply disallowed for the test.
    struct rn_device *adev = &gpu;                                         // the RREG32 macros expect this name
    pci->setBusMasterEnable(true);                                         // the GPU reads and writes host memory (GART buffers, interrupt vectors)
    // The IP bases are only known once the discovery table is parsed, which is after rn_device_init in start(): fill them in here.
    // (They were all 0 until now, so every macro access in this test hit the wrong register.)
    for (uint32_t i = 0; i < 4; i++) ipBase(kRNHwIdGc, &gpu.reg_offset[RN_GC_HWIP][0][i], i);
    ipBase(kRNHwIdMp1, &gpu.reg_offset[RN_MP1_HWIP][0][0]);
    for (uint32_t i = 0; i < 2; i++) ipBase(kRNHwIdOsssys, &gpu.reg_offset[RN_OSSSYS_HWIP][0][i], i);
    for (uint32_t i = 0; i < 5; i++) ipBase(kRNHwIdMmhub, &gpu.reg_offset[RN_MMHUB_HWIP][0][i], i);   // SET_HW_RESOURCES hands these to the MES
    for (uint32_t i = 0; i < 6; i++) ipBase(kRNHwIdNbif, &gpu.reg_offset[RN_NBIO_HWIP][0][i], i);
    // After the features are enabled the SMU may be busy for a while with C2PMSG_90 at 0: wait for it (up to 10 s) and record how long.
    {
        const char *sn[2] = {"c2pmsg_90", "sdma_version"};
        uint32_t sv[2] = {RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90), rd(0x1295 * 4)};
        rnPublish(this, "sdma-start", sn, sv, 2);
    }
    uint32_t waitedMs = 0;
    // Not while the SDMA is up (SDMA0_VERSION nonzero): the mailbox reads 0 from here on, and the block went dark during this wait.
    while (rd(0x1295 * 4) == 0 && waitedMs < 10000 && RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90) == 0) { IOSleep(1); waitedMs++; }
    {
        const char *wn[1] = {"smu_idle_wait_ms"};
        uint32_t wv[1] = {waitedMs};
        rnPublish(this, "sdma-wait", wn, wv, 1);
    }
    uint32_t goResult = 0, goResp = 0, c66 = RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_66), c82 = RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_82), c90 = RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90);
    uint32_t trace[6] = {rd(0x1295 * 4), 0, 0, 0, 0, 0};   // SDMA0_VERSION after: start, GetSmuVersion, DisallowGfxOff, vram writes, gfxhub, test
    uint32_t verResult = 0, verResp = 0;
    int ver = rn_smu13_send(&gpu, PPSMC_MSG_GetSmuVersion, 0, &verResult, &verResp, 500000);   // is the SMU still answering at all?
    {
        const char *sn[6] = {"c2pmsg_66", "c2pmsg_82", "c2pmsg_90", "version_rc", "version_resp", "version"};
        uint32_t sv[6] = {c66, c82, c90, static_cast<uint32_t>(ver), verResp, verResult};
        rnPublish(this, "sdma-smu", sn, sv, 6);
    }
    trace[1] = rd(0x1295 * 4);
    int go = rn_smu13_send(&gpu, PPSMC_MSG_DisallowGfxOff, 0, &goResult, &goResp, 500000);
    IOLog("RedNavi: SDMA test: DisallowGfxOff rc %d response 0x%x\n", go, goResp);
    IOSleep(20);
    trace[2] = rd(0x1295 * 4);
    {
        uint32_t gc0 = 0;
        ipBase(kRNHwIdGc, &gc0, 0);
        uint32_t dn[6] = {static_cast<uint32_t>(go), goResp, rd((gc0 + 0xda4) * 4), rd((gc1 + 0x4e6b) * 4), rd((gc0 + 0xf40) * 4), rd((gc1 + 0x4c00) * 4)};
        const char *dnn[6] = {"gfxoff_rc", "gfxoff_resp", "grbm_status", "rlc_gpm_stat", "cp_stat", "rlc_cntl"};
        rnPublish(this, "sdma-pre", dnn, dn, 6);
    }
    // VRAM offsets: the first ~8 MB hold the firmware boot framebuffer that macOS draws into, the scanout is at 0x08000000, so the
    // engines work at 192 MB: page table 0x0C100000, default and fault pages 0x0C200000/0x0C201000, SDMA ring 0x0C000000, target 0x0C002000.
    // The GART window: 16384 pages (64 MB) at GPU address 0x2000000000 (outside the VRAM aperture), the page table at VRAM 0x0C100000.
    const uint64_t kGartVa = 0x2000000000ull;
    struct rn_gmc gmc = {kRNVramMcBase, kRNVramMcBase + 0x3FFFFFFFFull, kGartVa, kGartVa + 16384ull * 4096 - 1, 0x0C100000, 0x0C200000, 0x0C201000};
    
    // Does CPU access to VRAM through MM_INDEX/MM_DATA survive the memory hub being switched on? Two probe locations, read right
    // after writing and again 50 ms later, before and after rn_gfxhub_enable.
    uint32_t pt[8] = {};
    rn_vram_wr32(&gpu, 0x0C000000, 0xA1A1A1A1u); rn_vram_wr32(&gpu, 0x0C002000, 0xB2B2B2B2u);
    pt[0] = rn_vram_rd32(&gpu, 0x0C000000); pt[1] = rn_vram_rd32(&gpu, 0x0C002000);
    IOSleep(50);
    pt[2] = rn_vram_rd32(&gpu, 0x0C000000); pt[3] = rn_vram_rd32(&gpu, 0x0C002000);
    trace[3] = rd(0x1295 * 4);
    // Proof that the 11.0.3 hub offsets are right: before we write anything, FB_LOCATION_BASE/TOP should hold what the VBIOS left (0x8000/0x83ff).
    const uint32_t fbBefore = RREG32_SOC15(RN_GC, 0, regGCMC_VM_FB_LOCATION_BASE), fbTopBefore = RREG32_SOC15(RN_GC, 0, regGCMC_VM_FB_LOCATION_TOP);
    rn_gart_clear(&gpu, &gmc);
    // Opt-in (rednavi_gart_test=1): four pages of host memory, mapped into the GART before the hub is switched on.
    uint32_t gartArg = 0;
    const bool gartTest = PE_parse_boot_argn("rednavi_gart_test", &gartArg, sizeof(gartArg)) && gartArg == 1;
    if (gartTest && !hostBuf) {
        hostBuf = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 4 * 4096, 4096);
        if (hostBuf) {
            void *cpu = hostBuf->getBytesNoCopy();
            hostDma = IODMACommand::withSpecification(kIODMACommandOutputHost64, rn_dma::kGmc11DmaAddressBits, 0, IODMACommand::kMapped, 0, 4096, nullptr, 0);
            if (cpu && hostDma && hostDma->setMemoryDescriptor(hostBuf) == kIOReturnSuccess) {
                uint64_t pages[4] = {};
                const rn_dma::Result mapping = rn_dma::collectPages<IODMACommand::Segment64>(pages, 4, 4 * 4096,
                    [this](uint64_t *offset, IODMACommand::Segment64 *segments, uint32_t *count) {
                        UInt64 dmaOffset = *offset;
                        UInt32 dmaCount = *count;
                        const IOReturn status = hostDma->genIOVMSegments(&dmaOffset, segments, &dmaCount);
                        *offset = dmaOffset;
                        *count = dmaCount;
                        return status == kIOReturnSuccess;
                    });
                // The diagnostic below deliberately uses base + page offsets.
                // A contiguous CPU allocation does not establish DMA contiguity.
                if (mapping.ok() && pages[1] == pages[0] + 4096 && pages[2] == pages[0] + 8192 && pages[3] == pages[0] + 12288) {
                    memset(cpu, 0, 4 * 4096);
                    hostBus = pages[0];
                    hostBusLen = 4 * 4096;
                }
            }
            const char *hn[5] = {"buf_ok", "dma_ok", "bus_lo", "bus_hi", "seg_len"};
            uint32_t hv[5] = {1, hostBusLen == 4 * 4096, static_cast<uint32_t>(hostBus), static_cast<uint32_t>(hostBus >> 32), hostBusLen};
            rnPublish(this, "gart-info", hn, hv, 5);
            if (hostBusLen != 4 * 4096) {
                if (hostDma) { hostDma->complete(); hostDma->clearMemoryDescriptor(); hostDma->release(); hostDma = nullptr; }
                hostBuf->release(); hostBuf = nullptr;
            }
        }
        for (uint32_t pg = 0; hostBusLen == 4 * 4096 && pg < 4; pg++) rn_gart_map_page(&gpu, &gmc, pg, hostBus + pg * 4096ull);
    }
    rn_gfxhub_enable(&gpu, &gmc);
    {   // Opt-in (rednavi_mmhub=1): mmhub_v3_0_gart_enable after the gfxhub, as gmc_v11_0_gart_enable does; before/after of its main registers go to "mmhub-gart".
        uint32_t mm = 0;
        if (PE_parse_boot_argn("rednavi_mmhub", &mm, sizeof(mm)) && mm == 1) {
            uint32_t before[8], after[8], all[16];
            rn_mmhub_state(&gpu, before);
            rn_mmhub_enable(&gpu, &gmc);
            rn_mmhub_state(&gpu, after);
            for (int i = 0; i < 8; i++) { all[i] = before[i]; all[8 + i] = after[i]; }
            static const char *mn[16] = {"b_ctx0", "b_tlb", "b_l2", "b_ptb", "b_start", "b_end", "b_fault", "b_faddr", "a_ctx0", "a_tlb", "a_l2", "a_ptb", "a_start", "a_end", "a_fault", "a_faddr"};
            rnPublish(this, "mmhub-gart", mn, all, 16);
        }
    }
    gmcLive = gmc;
    {   // the VRAM the allocator hands out (docs/COMPAT.md, "VRAM buffers"). Device-local: above the user page-table pools (kVmTables + 7 x 16 MB) to 15.25 GB, below the PSP
        // TMR at 15.5 GB and the discovery table at the end. CPU-visible: the free parts of the PCI aperture, between the scanout (4K fits in 34 MB from 0x08000000) and the
        // kernel tables at 0x0C000000, and above those (the MES area and the GART table end well before 0x0D000000) up to the aperture's end.
        bar0Mem = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + kRNVramBar * 4);
        const uint64_t apLen = bar0Mem ? static_cast<uint64_t>(bar0Mem->getLength()) : 0;
        devPool.init(kVmTables + 7ull * kVmPoolPages * 4096, 0x3D0000000ull - (kVmTables + 7ull * kVmPoolPages * 4096));
        if (apLen >= 0x10000000ull) {
            visPool.init(kRNScanoutVramOff + 0x02200000ull, 0x0C000000ull - (kRNScanoutVramOff + 0x02200000ull));
            visPool.add(0x0D000000ull, 0x10000000ull - 0x0D000000ull);
        }
        IOLog("RedNavi: VRAM allocator: %llu MB device-local, %llu MB CPU-visible\n", (uint64_t)(devPool.totalPages >> 8), (uint64_t)(visPool.totalPages >> 8));
    }
    gpuLive = true;
    trace[4] = rd(0x1295 * 4);
    {   // do the GCVM writes stick? (they read 0 on a healthy Linux system too, so this may tell nothing)
        const char *gn[10] = {"l2_cntl", "ctx0_cntl", "aperture_low", "aperture_high", "l1_tlb", "pt_base_lo", "pt_end_lo", "fb_base", "fb_base_before", "fb_top_before"};
        uint32_t gv[10] = {RREG32_SOC15(RN_GC, 0, regGCVM_L2_CNTL), RREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_CNTL),
                          RREG32_SOC15(RN_GC, 0, regGCMC_VM_SYSTEM_APERTURE_LOW_ADDR), RREG32_SOC15(RN_GC, 0, regGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR),
                          RREG32_SOC15(RN_GC, 0, regGCMC_VM_MX_L1_TLB_CNTL), RREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32),
                          RREG32_SOC15(RN_GC, 0, regGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32), RREG32_SOC15(RN_GC, 0, regGCMC_VM_FB_LOCATION_BASE), fbBefore, fbTopBefore};
        rnPublish(this, "gcvm-readback", gn, gv, 10);
    }
    rn_vram_wr32(&gpu, 0x0C000000, 0xC3C3C3C3u); rn_vram_wr32(&gpu, 0x0C002000, 0xD4D4D4D4u);
    pt[4] = rn_vram_rd32(&gpu, 0x0C000000); pt[5] = rn_vram_rd32(&gpu, 0x0C002000);
    IOSleep(50);
    pt[6] = rn_vram_rd32(&gpu, 0x0C000000); pt[7] = rn_vram_rd32(&gpu, 0x0C002000);
    {
        const char *pn[8] = {"before_now_0", "before_now_2", "before_50ms_0", "before_50ms_2", "after_now_0", "after_now_2", "after_50ms_0", "after_50ms_2"};
        rnPublish(this, "vram-persist", pn, pt, 8);
    }
    IOLog("RedNavi: SDMA test: GFXHUB enabled, starting SDMA0\n");
    static struct rn_sdma_result res;   // static: sdmaTest runs once, and a 16 KB kernel stack cannot be widened
    rn_sdma0_test(&gpu, kRNVramMcBase, 0x0C000000, 4096, 0x0C002000, &res);   // MC addresses, through the system aperture
    trace[5] = rd(0x1295 * 4);
    {   // Did the write land somewhere else (fault default pages, an offset by the FB base)? Look for it in the whole work area.
        uint32_t hits[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}, n = 0;
        for (uint64_t o = 0x0C000000ull; o < 0x0C300000ull && n < 4; o += 4)
            if (rn_vram_rd32(&gpu, o) == 0xDEADBEEFu) hits[n++] = static_cast<uint32_t>(o);
        const char *hn[5] = {"hit0", "hit1", "hit2", "hit3", "target_after"};
        uint32_t hv[5] = {hits[0], hits[1], hits[2], hits[3], rn_vram_rd32(&gpu, 0x0C002000)};
        rnPublish(this, "sdma-scan", hn, hv, 5);
    }
    {
        const char *tn[10] = {"start", "after_version_msg", "after_gfxoff_msg", "after_vram", "after_gfxhub", "after_test",
                              "gc_base0", "gc_base1", "gc_base2", "macro_version"};
        uint32_t tv[10] = {trace[0], trace[1], trace[2], trace[3], trace[4], trace[5], gpu.reg_offset[RN_GC_HWIP][0][0],
                           gpu.reg_offset[RN_GC_HWIP][0][1], gpu.reg_offset[RN_GC_HWIP][0][2],
                           rn_rreg32(&gpu, gpu.reg_offset[RN_GC_HWIP][0][0] + 0x35)};
        rnPublish(this, "sdma-trace", tn, tv, 10);
    }
    IOLog("RedNavi: SDMA test: step %u, value 0x%08x, RPTR 0x%x WPTR 0x%x F32_CNTL 0x%08x RB_CNTL 0x%08x -> %s\n", res.step, res.value, res.rptr, res.wptr, res.f32_cntl, res.rb_cntl, res.ok ? "OK" : "FAILED");
    const char *n[25] = {"ok", "step", "value", "pre_value", "rptr", "wptr", "f32_cntl", "rb_cntl", "fault_status", "fault_addr_lo", "fault_addr_hi", "sdma_status", "sdma_status1", "ctx_status",
                         "version", "cntl", "power_cntl", "checksum", "freeze", "chicken", "program", "ring0", "ring3", "rptr_shadow", "wptr_shadow"};
    uint32_t v[25] = {static_cast<uint32_t>(res.ok), res.step, res.value, res.pre_value, res.rptr, res.wptr, res.f32_cntl, res.rb_cntl, res.fault_status, res.fault_addr_lo, res.fault_addr_hi, res.sdma_status, res.sdma_status1, res.ctx_status,
                      res.version, res.cntl, res.power_cntl, res.checksum, res.freeze, res.chicken, res.program, res.ring0, res.ring3, res.rptr_shadow, res.wptr_shadow};
    rnPublish(this, "sdma-test", n, v, 25);

    if (gartTest && hostBusLen == 4 * 4096) {
        // System memory through the GART: SDMA writes a dword into the host page, and copies another one from it into VRAM.
        volatile uint32_t *host = static_cast<volatile uint32_t *>(hostBuf->getBytesNoCopy());
        host[0] = 0xAAAA0000u; host[1] = 0x55667788u;
        const uint64_t vramDst = 0x0C0A0000;
        rn_vram_wr32(&gpu, vramDst, 0);
        const uint64_t dstMc = kRNVramMcBase + vramDst;
        const uint32_t pkt[12] = {
            2u, lower_32_bits(kGartVa), upper_32_bits(kGartVa), 0u, 0x11223344u,                       // WRITE_LINEAR one dword to host[0]
            1u, 3u, 0u, lower_32_bits(kGartVa + 4), upper_32_bits(kGartVa + 4), lower_32_bits(dstMc), upper_32_bits(dstMc),   // COPY_LINEAR 4 bytes host[1] to VRAM
        };
        static struct rn_sdma_run_result sr;   // static: sdmaTest runs once, and a 16 KB kernel stack cannot be widened
        rn_sdma0_run(&gpu, kRNVramMcBase, 0x0C0B0000, 4096, pkt, 12, &sr);
        const char *gn2[12] = {"run_ok", "rptr", "wptr", "host0", "host1", "vram_dst", "fault_status", "fault_addr_lo", "fault_addr_hi", "write_ok", "copy_ok", "rptr_shadow"};
        uint32_t gv2[12] = {static_cast<uint32_t>(sr.ok), sr.rptr, sr.wptr, host[0], host[1], rn_vram_rd32(&gpu, vramDst), sr.fault_status, sr.fault_addr_lo,
                            sr.fault_addr_hi, host[0] == 0x11223344u, rn_vram_rd32(&gpu, vramDst) == 0x55667788u, sr.rptr_shadow};
        rnPublish(this, "gart-test", gn2, gv2, 12);

        // The hub is on: a page is mapped at run time, used, then pointed at another host page and flushed. The second write must land
        // in the new page; a stale translation would put it in the old one. Finally the PTE is removed again.
        {
            volatile uint32_t *h = static_cast<volatile uint32_t *>(hostBuf->getBytesNoCopy());   // 1024 dwords per page
            h[3 * 1024] = 0x0; h[2 * 1024 + 1] = 0x0;
            const uint64_t va = kGartVa + 8 * 4096;
            int f1 = rn_gart_update_page(&gpu, &gmc, 8, hostBus + 3 * 4096ull);                    // page 8 -> host page 3
            const uint32_t pk1[5] = {2u, lower_32_bits(va), upper_32_bits(va), 0u, 0x99887766u};
            struct rn_sdma_run_result r1, r2;
            rn_sdma0_run(&gpu, kRNVramMcBase, 0x0C0B0000, 4096, pk1, 5, &r1);
            const uint32_t w1 = h[3 * 1024];
            int f2 = rn_gart_update_page(&gpu, &gmc, 8, hostBus + 2 * 4096ull);                    // page 8 -> host page 2
            const uint32_t pk2[5] = {2u, lower_32_bits(va + 4), upper_32_bits(va + 4), 0u, 0xAABBCCDDu};
            rn_sdma0_run(&gpu, kRNVramMcBase, 0x0C0B0000, 4096, pk2, 5, &r2);
            const uint32_t w2 = h[2 * 1024 + 1], stale = h[3 * 1024 + 1];
            int f3 = rn_gart_update_page(&gpu, &gmc, 8, 0);                                        // unmapped again
            const char *dn2[10] = {"flush1", "flush2", "flush3", "first_in_page3", "second_in_page2", "second_in_old_page", "run1_ok", "run2_ok", "fault_status", "fault_addr_lo"};
            uint32_t dv2[10] = {static_cast<uint32_t>(f1), static_cast<uint32_t>(f2), static_cast<uint32_t>(f3), w1, w2, stale,
                                static_cast<uint32_t>(r1.ok), static_cast<uint32_t>(r2.ok), r2.fault_status, r2.fault_addr_lo};
            rnPublish(this, "gart-dynamic", dn2, dv2, 10);

            // More data on why the remapped page kept its old translation: what the page table holds after an update, whether a page that
            // was never used before is mapped correctly after the hub is on, and a remap with a long pause before the flush.
            const uint64_t pteBase = gmc.pt_vram_offset;
            h[1024 + 2] = 0; h[5] = 0; h[1024 + 5] = 0;
            rn_gart_update_page(&gpu, &gmc, 9, hostBus + 1 * 4096ull);                               // fresh page 9 -> host page 1
            const uint32_t pteLo9 = rn_vram_rd32(&gpu, pteBase + 9 * 8), pteHi9 = rn_vram_rd32(&gpu, pteBase + 9 * 8 + 4);
            const uint64_t va9 = kGartVa + 9 * 4096;
            const uint32_t pk3[5] = {2u, lower_32_bits(va9 + 8), upper_32_bits(va9 + 8), 0u, 0xC0FFEE00u};
            struct rn_sdma_run_result r3, r4;
            rn_sdma0_run(&gpu, kRNVramMcBase, 0x0C0B0000, 4096, pk3, 5, &r3);
            const uint32_t fresh = h[1024 + 2];                                                      // lands in host page 1 if the new mapping works
            rn_gart_map_page(&gpu, &gmc, 9, hostBus + 0 * 4096ull);                                 // remap 9 -> host page 0, only the PTE
            const uint32_t pteLo9b = rn_vram_rd32(&gpu, pteBase + 9 * 8);
            IOSleep(50);
            rn_gart_flush(&gpu);
            IOSleep(50);
            const uint32_t pk4[5] = {2u, lower_32_bits(va9 + 20), upper_32_bits(va9 + 20), 0u, 0xC0FFEE11u};
            rn_sdma0_run(&gpu, kRNVramMcBase, 0x0C0B0000, 4096, pk4, 5, &r4);
            const char *d3n[10] = {"pte9_lo", "pte9_hi", "pte9_lo_after_remap", "fresh_in_page1", "remap_in_page0", "remap_in_old_page1", "run3_ok", "run4_ok", "fault_status", "fault_addr_lo"};
            uint32_t d3v[10] = {pteLo9, pteHi9, pteLo9b, fresh, h[5], h[1024 + 5], static_cast<uint32_t>(r3.ok), static_cast<uint32_t>(r4.ok), r4.fault_status, r4.fault_addr_lo};
            rnPublish(this, "gart-dynamic2", d3n, d3v, 10);
        }
    }

#ifdef RN_HAVE_GC_FW
    // Opt-in (rednavi_gc_init=1, after the memory hub is on): the gfx_v11_0_hw_init steps RedNavi had left out, in Linux's order (rn_gfx_constants_init).
    uint32_t gcInit = 0;
    if (PE_parse_boot_argn("rednavi_gc_init", &gcInit, sizeof(gcInit)) && gcInit == 1) rn_gfx_constants_init(&gpu);
    // Opt-in (rednavi_gfx_test=1, on top of rednavi_sdma_test=1, which turns the memory hub on): a first command on the graphics
    // ring. Written from the Linux sources and not yet run on hardware.
    uint32_t gfx = 0;
    if (PE_parse_boot_argn("rednavi_gfx_test", &gfx, sizeof(gfx)) && gfx == 1) {
        struct rn_cp_start pc = {kRNGcPfpStartLo, kRNGcPfpStartHi, kRNGcMeStartLo, kRNGcMeStartHi, kRNGcMecStartLo, kRNGcMecStartHi};
        static struct rn_gfx_result gr;   // static: sdmaTest runs once, and a 16 KB kernel stack cannot be widened
        rn_gfx_ring_test(&gpu, kRNVramMcBase, 0x0C010000, 4096, &pc, &gr);
        const char *gn[21] = {"step", "me_cntl_before", "cp_stat_before", "me_cntl", "cp_stat", "rptr", "wptr", "scratch", "rptr_shadow", "ring0", "ok",
                              "me_cntl_imm", "me_cntl_10ms", "rb0_cntl_rd", "rb0_base_rd", "rb0_base_hi_rd", "rb_active_rd", "max_ctx_rd",
                              "device_id_rd", "pfp_pc_rd", "me_pc_rd"};
        uint32_t gv[21] = {gr.step, gr.me_cntl_before, gr.cp_stat_before, gr.me_cntl, gr.cp_stat, gr.rptr, gr.wptr, gr.scratch,
                           gr.rptr_shadow, gr.ring0, static_cast<uint32_t>(gr.ok), gr.me_cntl_imm, gr.me_cntl_10ms, gr.rb0_cntl_rd,
                           gr.rb0_base_rd, gr.rb0_base_hi_rd, gr.rb_active_rd, gr.max_ctx_rd, gr.device_id_rd, gr.pfp_pc_rd, gr.me_pc_rd};
        rnPublish(this, "gfx-test", gn, gv, 21);
        const char *tn[27] = {"t0_me", "t0_stat", "t0_rptr", "t1_me", "t1_stat", "t1_rptr", "t2_me", "t2_stat", "t2_rptr", "t5_me", "t5_stat", "t5_rptr",
                              "t10_me", "t10_stat", "t10_rptr", "t20_me", "t20_stat", "t20_rptr", "t50_me", "t50_stat", "t50_rptr",
                              "t100_me", "t100_stat", "t100_rptr", "fault_status", "fault_addr_lo", "fault_addr_hi"};
        uint32_t tv[27];
        for (int i = 0; i < 8; i++) for (int j = 0; j < 3; j++) tv[i * 3 + j] = gr.trace[i][j];
        tv[24] = gr.fault_status; tv[25] = gr.fault_addr_lo; tv[26] = gr.fault_addr_hi;
        rnPublish(this, "gfx-trace", tn, tv, 27);

        // Opt-in (rednavi_compute_test=1, after the gfx test, which sets the MEC entry points): a first command on a compute queue.
        uint32_t comp = 0;
        if (PE_parse_boot_argn("rednavi_compute_test", &comp, sizeof(comp)) && comp == 1) {
            // Opt-in (rednavi_doorbell_test=1): the queue is woken by a doorbell write to BAR2 instead of by registers.
            uint32_t dbArg = 0;
            IOMemoryMap *doorbellMap = nullptr;
            if (PE_parse_boot_argn("rednavi_doorbell_test", &dbArg, sizeof(dbArg)) && dbArg == 1) {
                doorbellMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + 2 * 4);
                if (doorbellMap) {
                    uint32_t s2a = 0, before[3] = {};
                    PE_parse_boot_argn("rednavi_doorbell_s2a", &s2a, sizeof(s2a));   // 1: also write the two S2A doorbell entries
                    rn_doorbell_aperture_enable(&gpu, s2a == 1, before);
                    rn_set_doorbell(reinterpret_cast<volatile void *>(doorbellMap->getVirtualAddress()), 6);   // mec_ring0 (3) << 1
                    const char *bn[5] = {"length_kb", "doorbell_dword", "aper_en_before", "s2a0_before", "s2a3_before"};
                    uint32_t bv[5] = {static_cast<uint32_t>(doorbellMap->getLength() >> 10), 6, before[0], before[1], before[2]};
                    rnPublish(this, "doorbell-info", bn, bv, 5);
                } else {
                    IOLog("RedNavi: doorbell test: could not map BAR2\n");
                }
            }
            static struct rn_compute_result cr;   // static: sdmaTest runs once, and a 16 KB kernel stack cannot be widened
            uint32_t gcAwake = 0;     // rednavi_gc_awake=1: hold the RLC safe mode (the GC cannot enter GFXOFF) for the rest of the session; queues set up by hand
            PE_parse_boot_argn("rednavi_gc_awake", &gcAwake, sizeof(gcAwake));      // are lost when the GC powers down
            if (gcAwake == 1) rn_gc_safe_mode(&gpu, 1);
            uint32_t mesClean = 0;    // rednavi_mes_clean=1: no hand-programmed queue at all, only the MEC running and the doorbells mapped; the MES maps the queue
            PE_parse_boot_argn("rednavi_mes_clean", &mesClean, sizeof(mesClean));
            if (mesClean == 1 || mesClean == 2) {
                memset(&cr, 0, sizeof(cr));
                rn_mec_enable(&gpu);
            } else {
                rn_compute_ring_test(&gpu, kRNVramMcBase, 0x0C020000, 4096, &cr);
            }
            const char *cn[33] = {"step", "ok", "scratch", "active_rd", "pq_control_rd", "mqd_base_rd", "rptr", "wptr", "rptr_shadow", "ring0",
                                  "fault_status", "fault_addr_lo", "fault_addr_hi",
                                  "t0_cntl", "t0_active", "t0_rptr", "t1_cntl", "t1_active", "t1_rptr", "t2_cntl", "t2_active", "t2_rptr",
                                  "t5_cntl", "t5_active", "t5_rptr", "t10_cntl", "t10_active", "t10_rptr", "t20_cntl", "t20_active", "t20_rptr",
                                  "t50_cntl", "t100_cntl"};
            uint32_t cv[33] = {cr.step, static_cast<uint32_t>(cr.ok), cr.scratch, cr.active_rd, cr.pq_control_rd, cr.mqd_base_rd, cr.rptr, cr.wptr,
                               cr.rptr_shadow, cr.ring0, cr.fault_status, cr.fault_addr_lo, cr.fault_addr_hi,
                               cr.trace[0][0], cr.trace[0][1], cr.trace[0][2], cr.trace[1][0], cr.trace[1][1], cr.trace[1][2],
                               cr.trace[2][0], cr.trace[2][1], cr.trace[2][2], cr.trace[3][0], cr.trace[3][1], cr.trace[3][2],
                               cr.trace[4][0], cr.trace[4][1], cr.trace[4][2], cr.trace[5][0], cr.trace[5][1], cr.trace[5][2],
                               cr.trace[6][0], cr.trace[7][0]};
            rnPublish(this, "compute-test", cn, cv, 33);

            // Opt-in (rednavi_dispatch_test=1, after the compute test, whose queue it reuses): a first shader dispatch, an s_endpgm,
            // finished by a RELEASE_MEM fence. Written from the Linux headers, not yet run on hardware.
            uint32_t disp = 0;
            if (cr.ok && PE_parse_boot_argn("rednavi_dispatch_test", &disp, sizeof(disp)) && disp == 1) {
                static struct rn_dispatch_result dr;   // static: sdmaTest runs once, and a 16 KB kernel stack cannot be widened
                // Opt-in (rednavi_ih_test=1): an interrupt ring in VRAM, and the fence packet asks the CP for an interrupt.
                uint32_t ihTest = 0;
                const bool ih = PE_parse_boot_argn("rednavi_ih_test", &ihTest, sizeof(ihTest)) && ihTest == 1;
                // Opt-in (rednavi_msi_test=1, with the IH test): the interrupt vector is delivered to the CPU as a message-signalled interrupt.
                uint32_t msiArg = 0;
                bool msi = false;
                if (ih && PE_parse_boot_argn("rednavi_msi_test", &msiArg, sizeof(msiArg)) && msiArg == 1) {
                    int types[8], chosen = -1;
                    for (int i = 0; i < 8; i++) {
                        int type = -1;
                        types[i] = pci->getInterruptType(i, &type) == kIOReturnSuccess ? type : -1;   // 0x10000 PCI messaged (MSI), 1 level (legacy INTx), -1 none
                        if (chosen < 0 && types[i] >= 0 && (types[i] & kIOInterruptTypePCIMessaged)) chosen = i;   // MSI: type 0x10000
                    }
                    IOWorkLoop *wl = getWorkLoop();
                    if (chosen >= 0 && wl) {
                        msiSrc = IOInterruptEventSource::interruptEventSource(this, &RedNaviProbe::msiHandler, pci, chosen);
                        if (msiSrc && wl->addEventSource(msiSrc) == kIOReturnSuccess) { msiSrc->enable(); msi = true; }
                    }
                    const char *mn[10] = {"type0", "type1", "type2", "type3", "type4", "type5", "type6", "type7", "chosen", "enabled"};
                    uint32_t mv[10];
                    for (int i = 0; i < 8; i++) mv[i] = static_cast<uint32_t>(types[i]);
                    mv[8] = static_cast<uint32_t>(chosen);
                    mv[9] = msi;
                    rnPublish(this, "msi-info", mn, mv, 10);
                }
                if (ih) rn_ih_enable(&gpu, kRNVramMcBase, 0x0C040000, 4096, 0x0C041000, msi);
                uint32_t store = 0;
                PE_parse_boot_argn("rednavi_dispatch_store", &store, sizeof(store));   // 1: the shader stores a dword, 0: s_endpgm only
                uint32_t dispGart = 0;
                const bool toHost = gartTest && hostBusLen == 4 * 4096 && PE_parse_boot_argn("rednavi_dispatch_gart", &dispGart, sizeof(dispGart)) && dispGart == 1;
                volatile uint32_t *hostWord = toHost ? static_cast<volatile uint32_t *>(hostBuf->getBytesNoCopy()) + 2 : nullptr;   // dword 2 of the host page
                rn_compute_dispatch_test(&gpu, kRNVramMcBase, 0x0C020000, 0x0C030000, 0x0C031000, store == 1 ? 0x0C032000 : 0,
                                         toHost ? kGartVa + 8 : 0, hostWord, ih ? 2 : 0, msi ? &RedNaviProbe::waitMsi : nullptr, this, &dr);
                const char *dn[26] = {"step", "ok", "fence", "fence_before", "rptr", "wptr", "active", "scratch", "fault_status", "fault_addr_lo", "fault_addr_hi",
                                      "t0_rptr", "t0_fence", "t1_rptr", "t1_fence", "t5_rptr", "t5_fence", "t20_rptr", "t20_fence",
                                      "t100_rptr", "t100_fence", "t500_rptr", "t500_fence", "target", "target_before"};
                uint32_t dv[26] = {dr.step, static_cast<uint32_t>(dr.ok), dr.fence, dr.fence_before, dr.rptr, dr.wptr, dr.active, dr.scratch,
                                   dr.fault_status, dr.fault_addr_lo, dr.fault_addr_hi,
                                   dr.trace[0][0], dr.trace[0][1], dr.trace[1][0], dr.trace[1][1], dr.trace[2][0], dr.trace[2][1],
                                   dr.trace[3][0], dr.trace[3][1], dr.trace[4][0], dr.trace[4][1], dr.trace[5][0], dr.trace[5][1], dr.target, dr.target_before};
                rnPublish(this, "dispatch-test", dn, dv, 25);
                if (ih) {
                    static struct rn_ih_result ir;   // static: sdmaTest runs once, and a 16 KB kernel stack cannot be widened
                    rn_ih_read(&gpu, 0x0C040000, 0x0C041000, &ir);
                    const char *in[23] = {"rb_cntl", "wptr_reg", "wptr_shadow", "rptr_reg", "base", "cp_int", "e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7",
                                          "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "ok"};
                    uint32_t iv[23] = {ir.rb_cntl_rd, ir.wptr_reg, ir.wptr_shadow, ir.rptr_reg, ir.base_rd, ir.cp_int_rd};
                    for (int i = 0; i < 16; i++) iv[6 + i] = ir.entry[i];
                    iv[22] = ir.wptr_reg != 0 || ir.wptr_shadow != 0;
                    rnPublish(this, "ih-test", in, iv, 23);
                    if (msi) {
                        const char *cn2[3] = {"msi_count", "msi_last_wptr", "fence_ok"};
                        uint32_t cv2[3] = {msiCount, msiLastWptr, static_cast<uint32_t>(dr.ok)};
                        rnPublish(this, "msi-test", cn2, cv2, 3);
                    }
                }
            }
            // Opt-in (rednavi_mes_probe=1): read-only state of the MES (is it running?). Published as "mes-state".
            uint32_t mesProbe = 0;
            if (PE_parse_boot_argn("rednavi_mes_probe", &mesProbe, sizeof(mesProbe)) && mesProbe == 1) {
                uint32_t m[15];
                rn_mes_state(&gpu, m);
                const char *mn[15] = {"cntl", "p0_pc", "p0_pc_10ms", "p0_mepc", "p0_mcause", "p0_mbadaddr", "p0_gp0", "p0_gp3",
                                      "p1_pc", "p1_pc_10ms", "p1_mepc", "p1_mcause", "p1_mbadaddr", "p1_gp0", "p1_gp3"};
                rnPublish(this, "mes-state", mn, m, 15);
            }
            // Opt-in (rednavi_mes_step=N, 1..5): bring up the MES in stages (docs/COMPAT.md); "mes-progress" names the stage about to run.
            uint32_t mesStep = 0;
            if (PE_parse_boot_argn("rednavi_mes_step", &mesStep, sizeof(mesStep)) && mesStep >= 1) {
                static struct rn_mes_result mr;      // static: the kernel stack of sdmaTest is nearly full
                const uint32_t start[4] = {kRNMes0StartLo, kRNMes0StartHi, kRNMes1StartLo, kRNMes1StartHi};
                rn_mes::Workspace workspace;
                if (mesStep < 7) {   // Step 7's process/GPUVM path is rejected before allocating or mapping a MES workspace.
                    uint32_t mh = 1;
                    PE_parse_boot_argn("rednavi_mes_host", &mh, sizeof(mh));
                    if (mh != 0) {
                        const int hb = createHostBo(static_cast<uint32_t>(rn_mes::kWorkspaceBytes), true);
                        if (hb >= 0) {
                            workspace = rn_mes::Workspace::host(rn_mes::GpuVa(bos[hb].gpuVa), bos[hb].md->getBytesNoCopy(), bos[hb].size);
                        } else {
                            IOLog("RedNavi: MES host workspace allocation failed\n");
                        }
                    } else {
                        // Explicit VRAM mode: separate CPU aperture offset and
                        // device address, above SMU tables and the GART table.
                        workspace = rn_mes::Workspace::vram(rn_mes::GpuVa(kRNVramMcBase + 0x0C400000ull),
                                                          rn_mes::VramOffset(0x0C400000), rn_mes::kWorkspaceBytes);
                    }
                }
                rn_mes_start(&gpu, workspace, &gmc, start, mesStep,
                                 [](void *ctx, uint32_t step) {
                                     RedNaviProbe *self = static_cast<RedNaviProbe *>(ctx);
                                     const char *pn[1] = {"about_to_run"};
                                     uint32_t pv[1] = {step};
                                     rnPublish(self, "mes-progress", pn, pv, 1);
                                     IOLog("RedNavi: MES step %u\n", step);
                                     IOSleep(100);
                                 }, this, &mr);
                publishMes(mr);
                {   // Opt-in (rednavi_gfx_queue=1): the graphics queue through the MES, and a self test on it (docs/COMPAT.md, "Graphics queue")
                    uint32_t gq = 0;
                    if (PE_parse_boot_argn("rednavi_gfx_queue", &gq, sizeof(gq)) && gq == 1 && ((mr.q_active & 1u) || mr.add_ok)) {
                        static struct rn_gfxq_result gr;
                        rn_gfx_queue_map(&gpu, &gr);
                        static const char *const gn[] = {"step", "add_status", "add_ok", "hqd_active_before", "hqd_active", "hqd_cntl", "hqd_base", "mqd_base", "doorbell_ctl", "rptr", "wptr",
                                                         "cp_stat", "me_cntl", "scratch", "write_data", "fence", "rptr_word", "fault_status", "fault_lo", "fault_hi", "csb_dwords"};
                        const uint32_t gv[] = {gr.step, gr.add_status, gr.add_ok, gr.hqd_active_before, gr.hqd_active, gr.hqd_cntl, gr.hqd_base, gr.mqd_base, gr.doorbell_ctl, gr.rptr, gr.wptr,
                                               gr.cp_stat, gr.me_cntl, gr.scratch, gr.write_data, gr.fence, gr.rptr_word, gr.fault_status, gr.fault_lo, gr.fault_hi, gr.csb_dwords};
                        rnPublish(this, "gfx-queue", gn, gv, sizeof(gv) / sizeof(gv[0]));
                        gfxQueueOk = gr.step >= 7;
                    }
                }
                if ((mr.q_active & 1u) || mr.add_ok) computeRingOff = rn_mes::kComputeBlock;   // relative to the retained MES workspace
                if (mesClean == 1 && ((mr.q_active & 1u) || mr.add_ok)) cr.ok = 1;
                if (mesClean == 2) {      // the MES is up (step 4): now a hand-programmed queue, to see whether it survives with the MES running
                    rn_compute_ring_test(&gpu, kRNVramMcBase, 0x0C020000, 4096, &cr);
                }   // lets the tests that use the queue run on the MES-mapped one
            }
            // Opt-in (rednavi_kiq_test=1): the way Linux runs compute queues. The queue programmed by hand above is reset by the
            // firmware on the next doorbell after the boot-time tests; a queue mapped through the KIQ is the firmware's own.
            uint32_t kiqArg = 0;
            if (cr.ok && PE_parse_boot_argn("rednavi_kiq_test", &kiqArg, sizeof(kiqArg)) && kiqArg == 1) {
                static struct rn_kiq_result kr;   // static: sdmaTest runs once, and a 16 KB kernel stack cannot be widened
                uint32_t kiqStep = 4;
                PE_parse_boot_argn("rednavi_kiq_step", &kiqStep, sizeof(kiqStep));
                uint32_t kiqPipe = 3, kiqQueue = 0;
                PE_parse_boot_argn("rednavi_kiq_pipe", &kiqPipe, sizeof(kiqPipe));
                PE_parse_boot_argn("rednavi_kiq_queue", &kiqQueue, sizeof(kiqQueue));
                uint32_t kiqRlc = 0;
                PE_parse_boot_argn("rednavi_kiq_rlc", &kiqRlc, sizeof(kiqRlc));   // 1: also write RLC_CP_SCHEDULERS (hung the machine)
                rn_kiq_map_compute(&gpu, kRNVramMcBase, 0x0C080000, 0x0C090000, kiqStep,
                                   [](void *ctx, uint32_t step) {
                                       // the step about to run, published first: if the machine stops here, this is the last one that said so
                                       RedNaviProbe *self = static_cast<RedNaviProbe *>(ctx);
                                       const char *pn[1] = {"about_to_run"};
                                       uint32_t pv[1] = {step};
                                       rnPublish(self, "kiq-progress", pn, pv, 1);
                                       IOLog("RedNavi: KIQ step %u\n", step);
                                       IOSleep(100);
                                   }, this, kiqRlc == 1, kiqPipe, kiqQueue, &kr);
                if (kr.ok) computeRingOff = 0x0C090000;
                const char *kn[19] = {"step", "ok", "kiq_active", "kiq_rptr", "kiq_wptr", "map_active", "map_pq_control", "map_doorbell_ctl", "map_rptr", "map_wptr", "fault_status",
                                      "kiq_doorbell_ctl", "kiq_pq_control", "kiq_pq_base", "kiq_pq_status", "cp_stat", "kiq_rptr_late", "ring_dw0", "ring_dw1"};
                uint32_t kv[19] = {kr.step, static_cast<uint32_t>(kr.ok), kr.kiq_active, kr.kiq_rptr, kr.kiq_wptr, kr.map_active, kr.map_pq_control,
                                   kr.map_doorbell_ctl, kr.map_rptr, kr.map_wptr, kr.fault_status,
                                   kr.kiq_doorbell_ctl, kr.kiq_pq_control, kr.kiq_pq_base, kr.kiq_pq_status, kr.cp_stat, kr.kiq_rptr_late, kr.ring_dw0, kr.ring_dw1};
                rnPublish(this, "kiq-test", kn, kv, 19);
            }
            // Opt-in (rednavi_bo_test=1): two buffers in host memory, a shader and its command buffer in the first, the result and the fence in
            // the second; the command buffer is submitted through an INDIRECT_BUFFER packet and the fence is read from host memory.
            uint32_t boArg = 0;
            if (cr.ok && gartTest && hostBusLen == 4 * 4096 && PE_parse_boot_argn("rednavi_bo_test", &boArg, sizeof(boArg)) && boArg == 1) {
                const int a = createHostBo(4096), b = createHostBo(4096);
                uint32_t r[8] = {static_cast<uint32_t>(a), static_cast<uint32_t>(b), 0, 0, 0, 0, 0, 0};
                if (a >= 0 && b >= 0) {
                    volatile uint32_t *A = static_cast<volatile uint32_t *>(bos[a].md->getBytesNoCopy());
                    volatile uint32_t *B = static_cast<volatile uint32_t *>(bos[b].md->getBytesNoCopy());
                    const uint64_t bva = bos[b].gpuVa;
                    const uint32_t code[11] = {0xBE8000FFu, lower_32_bits(bva), 0xBE8100FFu, upper_32_bits(bva), 0x7E020280u, 0x7E0402FFu, 0x600DC0DEu,
                                               0xDC6A0000u, 0x00000201u, 0xBC7C0000u, 0xBFB00000u};
                    for (int i = 0; i < 64; i++) A[0x100 / 4 + i] = i < 11 ? code[i] : 0xBF9F0000u;
                    uint32_t ib[48];
                    const uint32_t nib = rn_build_dispatch_ib(&gpu, ib, bos[a].gpuVa + 0x100);
                    for (uint32_t i = 0; i < nib; i++) A[i] = ib[i];
                    B[0] = 0xDEAD0000u; B[1] = 0;
                    const int sub = rn_compute_submit_ib(&gpu, kRNVramMcBase, computeRingOff, 4096, bos[a].gpuVa, nib, bos[b].gpuVa + 4, 0xFEEDFACEu, 0);
                    for (int ms = 0; ms < 1500 && B[1] != 0xFEEDFACEu; ms++) IOSleep(1);
                    r[2] = static_cast<uint32_t>(sub); r[3] = B[0]; r[4] = B[1]; r[5] = lower_32_bits(bos[a].gpuVa); r[6] = lower_32_bits(bva);
                    r[7] = B[0] == 0x600DC0DEu && B[1] == 0xFEEDFACEu;
                    {
                        uint32_t h[33];
                        rn_scan_hqds(&gpu, h);
                        char hn[33][8];
                        const char *hp[33];
                        for (int i = 0; i < 32; i++) { snprintf(hn[i], 8, "%c%d", "qdrw"[i % 4], i / 4); hp[i] = hn[i]; }
                        snprintf(hn[32], 8, "count"); hp[32] = hn[32];
                        rnPublish(this, "hqd-scan", hp, h, 33);
                        uint32_t md[12];
                        rn_mes_doorbell_regs(&gpu, md);
                        const char *mdn[12] = {"p0c1", "p0c2", "p0c3", "p0c4", "p0c5", "p0c6", "p1c1", "p1c2", "p1c3", "p1c4", "p1c5", "p1c6"};
                        rnPublish(this, "mes-db-after", mdn, md, 12);
                    }
                    watchBoA = a; watchBoB = b; watchIbDwords = nib;
                }
                const char *bn[8] = {"bo_a", "bo_b", "submit_rc", "result", "fence", "a_va_lo", "b_va_lo", "ok"};
                rnPublish(this, "bo-test", bn, r, 8);
                uint32_t dg[11];
                rn_compute_diag(&gpu, dg);
                const char *dgn[11] = {"hqd_rptr", "hqd_wptr", "hqd_active", "cp_stat", "fault_status", "fault_addr_lo", "fault_addr_hi", "ring_wptr", "sdma_version", "grbm_status", "rlc_gpm_stat"};
                rnPublish(this, "bo-diag", dgn, dg, 11);
                uint32_t d2[8];
                rn_compute_diag2(&gpu, d2);
                const char *d2n[8] = {"doorbell_ctl", "pq_status", "range_lo", "range_hi", "aper_en", "pq_base", "pq_control", "mqd_base"};
                rnPublish(this, "bo-diag2", d2n, d2, 8);
            }
            uint32_t kaArg = 0;
            keepAlive = PE_parse_boot_argn("rednavi_keepalive", &kaArg, sizeof(kaArg)) && kaArg == 1;
            uint32_t watchArg = 0;
            if (cr.ok && PE_parse_boot_argn("rednavi_queue_watch", &watchArg, sizeof(watchArg)) && watchArg == 1) {
                IOWorkLoop *wl = getWorkLoop();
                if (wl) {
                    watchTimer = IOTimerEventSource::timerEventSource(this, [](OSObject *o, IOTimerEventSource *) {
                        RedNaviProbe *self = OSDynamicCast(RedNaviProbe, o);
                        if (self) self->watchStep();
                    });
                    if (watchTimer && wl->addEventSource(watchTimer) == kIOReturnSuccess) watchTimer->setTimeoutMS(250);
                }
            }
            if (doorbellMap && !doorbellKeep) { doorbellKeep = doorbellMap; doorbellMap = nullptr; }   // kept for later submissions (the user client)
            if (doorbellMap) { rn_set_doorbell(nullptr, 0); doorbellMap->release(); }
        }
    }
#endif
}

void RedNaviProbe::loadDmcub() {
#ifdef RN_HAVE_DMUB_FW
    uint32_t arg = 0;
    if (!PE_parse_boot_argn("rednavi_dmub_load", &arg, sizeof(arg)) || arg != 1) {
        IOLog("RedNavi: DMUB load not requested (boot-arg rednavi_dmub_load=1)\n");
        return;
    }
    uint32_t mode = 0;
    PE_parse_boot_argn("rednavi_dmub_mode", &mode, sizeof(mode));
    uint32_t viaPsp = 0;
    PE_parse_boot_argn("rednavi_dmub_psp", &viaPsp, sizeof(viaPsp));
    if (viaPsp) {
        // psp_load_non_psp_fw: stage the signed image in the 1 MB buffer and ask the sOS to load it.
        for (uint32_t off = 0; off < kRNPspBufSize; off += 4) vramWrite(kRNPspBufVramOffset + off, 0);
        for (uint32_t off = 0; off < kRNDmubSignedSize; off += 4) {
            uint32_t v = 0;
            for (uint32_t k = 0; k < 4 && off + k < kRNDmubSignedSize; k++) v |= static_cast<uint32_t>(kRNDmubSigned[off + k]) << (8 * k);
            vramWrite(kRNPspBufVramOffset + off, v);
        }
        uint64_t mc = kRNVramMcBase + kRNPspBufVramOffset;
        uint32_t args[4] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), kRNDmubSignedSize, kRNPspFwTypeDmub};
        uint32_t resp[4] = {};
        bool ok = pspSubmit(kRNPspCmdLoadIpFw, args, 4, resp, 4);
        IOLog("RedNavi: PSP LOAD_IP_FW DMUB (%u bytes): fence %s, status = 0x%08x, fw_addr = 0x%08x%08x\n", kRNDmubSignedSize, ok ? "ok" : "missing", resp[0], resp[3], resp[2]);
        const char *pn[4] = {"fence_ok", "status", "fw_addr_lo", "fw_addr_hi"};
        uint32_t pv[4] = {ok, resp[0], resp[2], resp[3]};
        rnPublish(this, "psp-load-dmub", pn, pv, 4);
        dmcubSnapshot("dmcub-snap-after-psp");
        if (!ok || resp[0] != 0) return;
    }
    uint32_t dmu = 0;
    if (!ipBase(kRNHwIdDmu, &dmu, 2)) { IOLog("RedNavi: DMUB: no DMU base 2\n"); return; }
    if (!vbiosCopy || vbiosCopy->getLength() != kRNDmubVbiosSize) { IOLog("RedNavi: DMUB: VBIOS size does not match the firmware header\n"); return; }
    uint32_t regLen = static_cast<uint32_t>(regs->getLength());
    if ((dmu + 0x21a) * 4 + 4 > regLen) { IOLog("RedNavi: DMUB: registers outside BAR\n"); return; }

    auto R = [&](uint32_t dword) { return rd((dmu + dword) * 4); };
    auto W = [&](uint32_t dword, uint32_t v) { wr((dmu + dword) * 4, v); };
    auto U = [&](uint32_t dword, uint32_t mask, uint32_t v) { W(dword, (R(dword) & ~mask) | (v & mask)); };
    // dword offsets from dcn_3_2_0_offset.h, all at DMU base index 2
    const uint32_t CNTL = 0x1f6, CNTL2 = 0x200, SEC = 0x1ce, S0 = 0x1e3, S7 = 0x1ea, S14 = 0x1f1, S15 = 0x1f2;
    const uint32_t GPINT1 = 0x1f8, RST = 0x342;
    const uint32_t IN_BASE = 0x1d4, IN_SIZE = 0x1d5, IN_WP = 0x1d6, IN_RP = 0x1d7;
    const uint32_t O0_BASE = 0x1d8, O0_SIZE = 0x1d9, O0_WP = 0x1da, O0_RP = 0x1db;
    const uint32_t O1_BASE = 0x1dc, O1_SIZE = 0x1dd, O1_WP = 0x1de, O1_RP = 0x1df;
    auto CWOFF = [](uint32_t n) { return 0x1b5 + 2 * n; };   // REGIONx_CWn_OFFSET (+1 is _HIGH)
    auto CWBASE = [](uint32_t n) { return 0x1a5 + n; };
    auto CWTOP = [](uint32_t n) { return 0x1ad + n; };
    const uint32_t EN = 0x80000000u, ADDR_MASK = 0x1FFFFFFFu;

    IOLog("RedNavi: DMUB: before: CNTL=0x%08x CNTL2=0x%08x SEC=0x%08x SCRATCH0=0x%08x\n", R(CNTL), R(CNTL2), R(SEC), R(S0));

    uint32_t minimal = 0;                       // rednavi_dmub_min=1: after the PSP load, only release the reset
    PE_parse_boot_argn("rednavi_dmub_min", &minimal, sizeof(minimal));
    if (viaPsp && minimal) {
        U(RST, 1u << 8, 0);
        U(CNTL, (1u << 16) | (1u << 19), (1u << 16) | (1u << 19));
        U(CNTL2, 1, 0);
        uint32_t st = 0, ms = 0;
        for (; ms < 3000; ms++) { st = R(S0); if ((st & 3) == 3) break; IOSleep(1); }
        IOLog("RedNavi: DMUB minimal release: after %u ms SCRATCH0=0x%08x\n", ms, st);
        dmcubSnapshot("dmcub-snap-end");
        return;
    }

    // dmub_dcn32_reset: ask the running firmware to stop, then hold the DMCUB in reset.
    bool enabled = (R(CNTL) >> 16) & 1, inReset = R(CNTL2) & 1;
    if (enabled && !inReset) {
        W(GPINT1, kRNDmubGpintStopFw);
        uint32_t i = 0;
        for (; i < 1000; i++) { if (R(S7) == kRNDmubStopResponse) break; IOSleep(1); }
        IOLog("RedNavi: DMUB: STOP_FW response %s after %u ms (SCRATCH7=0x%08x)\n", R(S7) == kRNDmubStopResponse ? "received" : "NOT received", i, R(S7));
        for (; i < 1000; i++) { if ((R(CNTL) >> 20) & 1) break; IOSleep(1); }
    }
    if (enabled) {
        U(CNTL2, 1, 1);
        IODelay(1);
        U(CNTL, 1u << 16, 0);
    }
    RN_STEP("RedNavi: DMUB step: clearing windows CW2-CW7\n");
    for (uint32_t n = 2; n <= 7; n++) U(CWTOP(n), EN, 0);
    RN_STEP("RedNavi: DMUB step: clearing ring pointers\n");
    W(IN_RP, 0); W(IN_WP, 0); W(O1_RP, 0); W(O1_WP, 0); W(O0_RP, 0); W(O0_WP, 0);
    W(S0, 0);
    W(GPINT1, 0);
    RN_STEP("RedNavi: DMUB step: building the block in VRAM (zeroing %u bytes)\n", kRNDmubBlockSize);

    // Build the memory block (the windows of tools/dmub_layout.py) in VRAM.
    const uint64_t blockOff = kRNDmubBlockVramOff;
    for (uint32_t off = 0; off < kRNDmubBlockSize; off += 4) vramWrite(blockOff + off, 0);
    for (uint32_t off = 0; off < kRNDmubInstSize; off += 4) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < 4 && off + k < kRNDmubInstSize; k++) v |= static_cast<uint32_t>(kRNDmubInst[off + k]) << (8 * k);
        vramWrite(blockOff + kRNDmubWin_INST_CONST_Off + off, v);
    }
    RN_STEP("RedNavi: DMUB step: zeroed, copying code (%u bytes)\n", kRNDmubInstSize);
    const uint8_t *vb = static_cast<const uint8_t *>(vbiosCopy->getBytesNoCopy());
    for (uint32_t off = 0; off < kRNDmubVbiosSize; off += 4) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < 4 && off + k < kRNDmubVbiosSize; k++) v |= static_cast<uint32_t>(vb[off + k]) << (8 * k);
        vramWrite(blockOff + kRNDmubWin_VBIOS_Off + off, v);
    }
    RN_STEP("RedNavi: DMUB step: copied VBIOS, verifying code\n");
    uint32_t bad = 0;
    for (uint32_t off = 0; off < kRNDmubInstSize; off += 4) {
        uint32_t v = 0;
        for (uint32_t k = 0; k < 4 && off + k < kRNDmubInstSize; k++) v |= static_cast<uint32_t>(kRNDmubInst[off + k]) << (8 * k);
        if (vramRead(blockOff + kRNDmubWin_INST_CONST_Off + off) != v) bad++;
    }
    IOLog("RedNavi: DMUB: block of %u bytes built at VRAM +0x%llx, %u mismatching code dwords\n", kRNDmubBlockSize, blockOff, bad);
    if (bad) return;

    auto addr = [&](uint32_t winOff) -> uint64_t { return mode ? blockOff + winOff : kRNVramMcBase + blockOff + winOff; };
    auto window = [&](uint32_t n, uint64_t offset, uint32_t base, uint32_t top) {
        W(CWOFF(n), static_cast<uint32_t>(offset));
        W(CWOFF(n) + 1, static_cast<uint32_t>(offset >> 32));
        W(CWBASE(n), base & ADDR_MASK);
        W(CWTOP(n), (top & ADDR_MASK) | EN);
    };

    // dmub_dcn32_backdoor_load: code and stack windows, addressed as VRAM offsets (fb_base removed).
    if (!viaPsp) {
    U(SEC, 1u << 16, 1u << 16);                 // DMCUB_SEC_RESET = 1
    U(CNTL, 1u << 16, 0);
    window(0, blockOff + kRNDmubWin_INST_CONST_Off, kRNDmubCw0Base, kRNDmubCw0Base + kRNDmubWin_INST_CONST_Size - 1);
    window(1, blockOff + kRNDmubWin_STACK_Off, kRNDmubCw1Base, kRNDmubCw1Base + kRNDmubWin_STACK_Size - 1);
    U(SEC, (1u << 16) | (0xFFu << 8), 0x20u << 8);   // DMCUB_SEC_RESET = 0, DMCUB_MEM_UNIT_ID = 0x20
    }

    RN_STEP("RedNavi: DMUB step: block verified, programming windows\n");
    // dmub_dcn32_enable_dmub_boot_options and setup_windows: VBIOS, mailbox, trace, firmware state.
    W(S14, 0);
    window(3, addr(kRNDmubWin_VBIOS_Off), kRNDmubCw3Base, kRNDmubCw3Base + kRNDmubWin_VBIOS_Size);
    window(4, addr(kRNDmubWin_MAILBOX_Off), kRNDmubCw4Base, kRNDmubCw4Base + kRNDmubWin_MAILBOX_Size);
    window(5, addr(kRNDmubWin_TRACEBUFF_Off), kRNDmubCw5Base, kRNDmubCw5Base + kRNDmubWin_TRACEBUFF_Size);
    uint64_t t = addr(kRNDmubWin_TRACEBUFF_Off);   // REGION5 aliases the trace buffer
    W(0x198, static_cast<uint32_t>(t));
    W(0x199, static_cast<uint32_t>(t >> 32));
    W(0x1a2, ((kRNDmubWin_TRACEBUFF_Size - 1) & ADDR_MASK) | EN);
    window(6, addr(kRNDmubWin_FW_STATE_Off), kRNDmubCw6Base, kRNDmubCw6Base + kRNDmubWin_FW_STATE_Size);
    W(O0_BASE, kRNDmubRegion5Base + kRNDmubTraceEntryOff);
    W(O0_SIZE, kRNDmubWin_TRACEBUFF_Size - kRNDmubTraceEntryOff);
    W(IN_BASE, kRNDmubCw4Base);
    W(IN_SIZE, kRNDmubRbSize);
    W(O1_BASE, kRNDmubCw4Base + kRNDmubRbSize);
    W(O1_SIZE, kRNDmubRbSize);

    RN_STEP("RedNavi: DMUB step: windows set, releasing reset\n");
    // dmub_dcn32_reset_release
    U(RST, 1u << 8, 0);                         // DMUIF_SOFT_RESET = 0
    W(S15, 0);
    U(CNTL, (1u << 16) | (1u << 19), (1u << 16) | (1u << 19));   // DMCUB_ENABLE, DMCUB_TRACEPORT_EN
    U(CNTL2, 1, 0);                             // DMCUB_SOFT_RESET = 0

    uint32_t st = 0, ms = 0;
    for (; ms < 3000; ms++) { st = R(S0); if ((st & 3) == 3) break; IOSleep(1); }
    IOLog("RedNavi: DMUB: after %u ms SCRATCH0=0x%08x (bit0 dal_fw, bit1 mailbox_rdy) CNTL=0x%08x CNTL2=0x%08x SEC=0x%08x\n", ms, st, R(CNTL), R(CNTL2), R(SEC));
    if ((st & 3) == 3) {
        dmubUp = true; dmubDmu = dmu; dmubMailbox = blockOff + kRNDmubWin_MAILBOX_Off;
        uint32_t cmdArg = 0;
        PE_parse_boot_argn("rednavi_dmub_cmd", &cmdArg, sizeof(cmdArg));
        if (cmdArg == 1) {
            // dmub_srv_fb_cmd_queue + dmub_srv_fb_cmd_execute: copy the command into the ring at the
            // write pointer, then move the write pointer; the firmware advances the read pointer.
            uint32_t wp = R(IN_WP), slot = wp % kRNDmubRbSize;
            uint64_t slotAddr = blockOff + kRNDmubWin_MAILBOX_Off + slot;
            for (uint32_t i = 0; i < 64; i += 4) vramWrite(slotAddr + i, 0);
            vramWrite(slotAddr, 6u | (14u << 24));    // header: type QUERY_FEATURE_CAPS (6), payload 14 bytes
            uint32_t newWp = (wp + 64) % kRNDmubRbSize;
            W(IN_WP, newWp);
            uint32_t rp = 0, waited = 0;
            for (; waited < 1000; waited++) { rp = R(IN_RP); if (rp == newWp) break; IOSleep(1); }
            uint32_t reply[5];
            for (uint32_t i = 0; i < 5; i++) reply[i] = vramRead(slotAddr + i * 4);
            IOLog("RedNavi: DMUB cmd QUERY_FEATURE_CAPS: wptr 0x%x -> 0x%x, rptr 0x%x after %u ms, slot %08x %08x %08x %08x %08x\n", wp, newWp, rp, waited, reply[0], reply[1], reply[2], reply[3], reply[4]);
            const char *cn[9] = {"rptr_reached", "wptr", "rptr", "slot0", "slot1", "slot2", "slot3", "slot4", "outbox1_wptr"};
            uint32_t cv[9] = {rp == newWp, newWp, rp, reply[0], reply[1], reply[2], reply[3], reply[4], R(O1_WP)};
            rnPublish(this, "dmub-cmd", cn, cv, 9);
        }
    }
    const char *names[12] = {"scratch0", "scratch1", "scratch2", "cntl", "cntl2", "inbox_wptr", "inbox_rptr", "mode", "sec_cntl", "fault_addr", "timer", "scratch14"};
    uint32_t vals[12] = {st, R(0x1e4), R(0x1e5), R(CNTL), R(CNTL2), R(IN_WP), R(IN_RP), mode, R(SEC), R(0x1fa), R(0x1fd), R(S14)};
    rnPublish(this, "dmub-load", names, vals, 12);
    dmcubSnapshot("dmcub-snap-end");
#else
    IOLog("RedNavi: built without dmub_fw.h, DMUB load not available\n");
#endif
}

bool RedNaviProbe::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;

    // The RX 7600 is currently the native macOS boot/display device.
    // Its P4 "read" sequence modifies MM_INDEX and setMemoryEnable changes
    // PCI command state. Neither is a harmless IORegistry-only diagnostic.
    // Fail closed before allocations, PCI config writes or BAR mappings.
    // This is not a runtime safety certification even in an opt-in build.
    const uint16_t vendorId = pci->configRead16(kIOPCIConfigVendorID);
    const uint16_t deviceId = pci->configRead16(kIOPCIConfigDeviceID);
    if (vendorId == 0x1002u && deviceId == 0x7480u) {
#if defined(RN_ENABLE_NAVI33_UNSAFE_P4)
        constexpr bool kExperimentalP4Built = true;
#else
        constexpr bool kExperimentalP4Built = false;
#endif
        uint32_t explicitOptIn = 0;
        PE_parse_boot_argn("rednavi_navi33_unsafe_p4", &explicitOptIn,
                           sizeof(explicitOptIn));
        const auto admission = rn_navi33::admitEarlyHardwareProbe(
            vendorId, deviceId, kExperimentalP4Built, explicitOptIn);
        if (admission != rn_navi33::ProbeAdmission::ExperimentalOnly) {
            IOLog("RedNavi: RX7600 hardware P4 refused (reason %u); "
                  "no BAR mapping, PCI memory-enable or MM_INDEX access\n",
                  static_cast<unsigned>(admission));
            return false;
        }
        IOLog("RedNavi: WARNING unsafe Navi33 P4 explicitly enabled; "
              "this may hang the sole GPU/display\n");
    }

    modeLock = IOLockAlloc();
    boLock = IOLockAlloc();
    bos = static_cast<RNBo *>(IOMalloc(sizeof(RNBo) * kBoCap));
    if (!bos) return false;
    memset(bos, 0, sizeof(RNBo) * kBoCap);
    { uint32_t mb = 0; if (PE_parse_boot_argn("rednavi_bo_budget_mb", &mb, sizeof(mb)) && mb >= 16 && mb <= 8192) boBudget = static_cast<uint64_t>(mb) << 20; }
    queueLock = IOLockAlloc();
    { uint32_t v = 0; if (PE_parse_boot_argn("rednavi_hang_ms", &v, sizeof(v)) && v >= 100 && v <= 9000) hangMs = v; }
    rn_compute_lock_init();
    pci->setMemoryEnable(true);
    dumpPci();

    regs = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0 + kRNRegBar * 4);
    if (!regs) {
        IOLog("RedNavi: could not map register BAR%u\n", kRNRegBar);
        return false;
    }
    IOLog("RedNavi: registers mapped, %llu KB\n", (uint64_t)regs->getLength() >> 10);
    rn_device_init(&gpu, "RedNavi", reinterpret_cast<volatile void *>(regs->getVirtualAddress()), regs->getLength());
    // Preserve the shipped RX 7800 XT M4 path and its Linux gfxhub_v3_0_3
    // default. The 7600 flavor is set only after verified IP Discovery.
    if (pci->configRead16(kIOPCIConfigVendorID) == 0x1002 &&
        pci->configRead16(kIOPCIConfigDeviceID) == 0x747e)
        gpu.gfxhub_variant = RN_GFXHUB_NAVI32_V3_0_3;

    // Navi 33 has distinct PSP/SMU, GC 11.0.2, SDMA 6.0.2 and DCN 3.2.1.
    // Detect and verify its actual IP Discovery image before any Navi 32 GPU
    // initialization. This is the first runtime stage of the Navi 33 compute
    // port; firmware, GPU VM and queue takeover are NOT implemented yet.
    // The discovery VRAM-index window below modifies MM_INDEX registers;
    // it must not be misrepresented as a side-effect-free read.
    if (pci->configRead16(kIOPCIConfigVendorID) == 0x1002 &&
        pci->configRead16(kIOPCIConfigDeviceID) == 0x7480) {
        dumpDiscovery();
        const rn_navi33::RuntimeReport check =
            rn_navi33::inspectRuntimeDiscovery(0x1002, 0x7480,
                disc ? static_cast<const uint8_t *>(disc->getBytesNoCopy()) : nullptr,
                disc ? disc->getLength() : 0);
        setProperty("navi33-discovery-error", static_cast<uint64_t>(check.error), 32);
        setProperty("navi33-blocks-found", static_cast<uint64_t>(check.blocksFound), 32);
        setProperty("navi33-binary-header-major", static_cast<uint64_t>(check.binaryMajor), 32);
        setProperty("navi33-ip-table-version", static_cast<uint64_t>(check.tableVersion), 32);
        if (check.eligibleForComputeBringup()) {
            // Explicitly select the gfx1102 M4 flavor without using the
            // Navi32 firmware, GART, MES or queue start path.
            const auto m4Profile = rn_navi33::m4::forNavi33(check);
            if (m4Profile.valid()) {
                gpu.gfxhub_variant = RN_GFXHUB_NAVI33_V3_0;
                setProperty("navi33-m4-selected-gfxhub", static_cast<uint64_t>(m4Profile.gfxhub), 32);
                setProperty("navi33-m4-selected-mmhub", static_cast<uint64_t>(m4Profile.mmhub), 32);
                setProperty("navi33-m4-gpu-writes-enabled", false);
            }
            IOLog("RedNavi: Navi33 GC11.0.2 / SDMA6.0.2 / PSP13.0.7 / SMU13.0.7 / DCN3.2.1 / MMHUB3.0.2 verified from device IP Discovery\n");
            setProperty("navi33-identity-verified", true);
            // Populate genuine gfx1102 segment bases before any future SOC15
            // register writes. Every source is the validated device table.
            if (rn_navi33::hasComputeRegisterBases(check)) {
                const auto &ips = check.bases;
                for (uint32_t i = 0; i < 4; ++i) gpu.reg_offset[RN_GC_HWIP][0][i] = ips.gc.dwords[i];
                for (uint32_t i = 0; i < ips.mmhub.count && i < RN_MAX_SEG; ++i)
                    gpu.reg_offset[RN_MMHUB_HWIP][0][i] = ips.mmhub.dwords[i];
                for (uint32_t i = 0; i < 2; ++i) gpu.reg_offset[RN_OSSSYS_HWIP][0][i] = ips.osssys.dwords[i];
                for (uint32_t i = 0; i < 6; ++i) gpu.reg_offset[RN_NBIO_HWIP][0][i] = ips.nbif.dwords[i];
                for (uint32_t i = 0; i < ips.sdma0.count && i < RN_MAX_SEG; ++i)
                    gpu.reg_offset[RN_SDMA0_HWIP][0][i] = ips.sdma0.dwords[i];
                gpu.reg_offset[RN_MP0_HWIP][0][0] = ips.mp0.dwords[0];
                gpu.reg_offset[RN_MP1_HWIP][0][0] = ips.mp1.dwords[0];
                for (uint32_t i = 0; i < ips.dmu.count && i < RN_MAX_SEG; ++i)
                    gpu.reg_offset[RN_DCE_HWIP][0][i] = ips.dmu.dwords[i];
                setProperty("navi33-ip-bases-ready", true);
                // Passive NBIO_HWIP identity from NBIF_HWID (108);
                // no register reads/writes or index selection here.
                setProperty("navi33-nbio-major",
                    static_cast<uint64_t>(check.nbio.major), 32);
                setProperty("navi33-nbio-minor",
                    static_cast<uint64_t>(check.nbio.minor), 32);
                setProperty("navi33-nbio-revision",
                    static_cast<uint64_t>(check.nbio.revision), 32);

                // P5 reads Navi33 MMHUB 3.0.2 FB_LOCATION_BASE/TOP/OFFSET.
                // Register offsets come from mmhub_3_0_2_offset.h, not MMHUB 3.0.0.
                // PSP/SMU mailbox status is reported separately. These are live GPU reads.
                auto readBar = [&](uint32_t base, uint32_t dword, uint32_t *out) -> bool {
                    uint32_t byteOff = 0;
                    if (!rn_navi33::checkedBarDword(base, dword, regs->getLength(), &byteOff))
                        return false;
                    *out = rd(byteOff);
                    return true;
                };
                uint32_t fbBase = 0, fbTop = 0, fbOffset = 0;
                IODeviceMemory *aperture = pci->getDeviceMemoryWithRegister(
                    kIOPCIConfigBaseAddress0 + kRNVramBar * 4);
                const uint64_t bar0Bytes = aperture ? aperture->getLength() : 0;
                const bool mmhubRead =
                    readBar(ips.mmhub.dwords[0], rn_navi33::mmhub_v3_0_2::kFbLocationBaseDword, &fbBase) &&
                    readBar(ips.mmhub.dwords[0], rn_navi33::mmhub_v3_0_2::kFbLocationTopDword, &fbTop) &&
                    readBar(ips.mmhub.dwords[0], rn_navi33::mmhub_v3_0_2::kFbOffsetDword, &fbOffset);
                bool p6MemoryVerified = false;
                uint32_t gfxFbOffsetRaw = 0;
                const bool gfxFbOffsetRead = readBar(
                    ips.gc.dwords[0], regGCMC_VM_FB_OFFSET, &gfxFbOffsetRaw);
                if (mmhubRead) {
                    const auto memory = rn_navi33::makeMemoryWindow(
                        fbBase, fbTop, fbOffset, vramMbytes, bar0Bytes);
                    const char *mn[] = {"fb_base_raw", "fb_top_raw", "fb_offset_raw",
                        "fb_start_lo", "fb_start_hi", "fb_end_lo", "fb_end_hi",
                        "vram_mib", "bar0_mib", "preflight_error"};
                    const uint32_t mv[] = {fbBase, fbTop, fbOffset,
                        static_cast<uint32_t>(memory.fbStart),
                        static_cast<uint32_t>(memory.fbStart >> 32),
                        static_cast<uint32_t>(memory.fbEnd),
                        static_cast<uint32_t>(memory.fbEnd >> 32),
                        vramMbytes, static_cast<uint32_t>(bar0Bytes >> 20),
                        static_cast<uint32_t>(memory.error)};
                    rnPublish(this, "navi33-memory-preflight", mn, mv, 10);
                    // Linux gmc_v11_0_vram_gtt_location derives the
                    // GPU-physical PTE base from FB_OFFSET, not from the
                    // FB_LOCATION_BASE MC address. Read both hubs and reject
                    // mismatches instead of silently treating them as equal.
                    const auto geometry = rn_navi33::address::makeGeometry(
                        memory, gfxFbOffsetRaw, gfxFbOffsetRead);
                    p6MemoryVerified = memory.valid() && geometry.ok();
                    setProperty("navi33-memory-window-verified", memory.valid());
                    setProperty("navi33-address-geometry-verified", geometry.ok());
                    const char *const an[] = {
                        "error", "gfx_fb_offset_raw", "mmhub_fb_offset_raw",
                        "gfx_read_ok", "mc_start_lo", "mc_start_hi",
                        "pte_phys_lo", "pte_phys_hi", "usable_mib", "cpu_visible_mib"
                    };
                    const uint64_t mc = geometry.ok() ? geometry.geometry.mcStart.value : 0ull;
                    const uint64_t physical = geometry.ok() ? geometry.geometry.pteBase.value : 0ull;
                    const uint32_t av[] = {
                        static_cast<uint32_t>(geometry.error), gfxFbOffsetRaw,
                        fbOffset, static_cast<uint32_t>(gfxFbOffsetRead),
                        static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32),
                        static_cast<uint32_t>(physical), static_cast<uint32_t>(physical >> 32),
                        static_cast<uint32_t>(memory.fbBytes >> 20),
                        static_cast<uint32_t>(memory.cpuVisibleBytes >> 20)
                    };
                    rnPublish(this, "navi33-address-space-audit", an, av, 10);
                    // Neither address represents a CPU virtual pointer,
                    // DMA IOVA, or arbitrary GPU VMID virtual address.
                    if (memory.valid()) {
                        setProperty("navi33-fb-start", memory.fbStart, 64);
                        setProperty("navi33-fb-end", memory.fbEnd, 64);
                        IOLog("RedNavi: Navi33 FB window 0x%llx..0x%llx VRAM %u MiB BAR0 %llu MiB\n",
                              memory.fbStart, memory.fbEnd, vramMbytes, bar0Bytes >> 20);
                    } else {
                        IOLog("RedNavi: Navi33 memory preflight error %u\n",
                              static_cast<unsigned>(memory.error));
                    }
                } else {
                    IOLog("RedNavi: Navi33 MMHUB FB registers not accessible through BAR5\n");
                    setProperty("navi33-memory-window-verified", false);
                }

                uint32_t pspInit = 0, pspSos = 0, smuResp = 0;
                const bool pspRead =
                    readBar(ips.mp0.dwords[0], 0x061, &pspInit) &&
                    readBar(ips.mp0.dwords[0], 0x091, &pspSos);
                const bool smuRead = readBar(ips.mp1.dwords[0], 0x29a, &smuResp);
                const char *pn[] = {"psp_ifwi_ready", "psp_sos_alive",
                                    "smu_response", "psp_read_ok", "smu_read_ok"};
                const uint32_t pv[] = {pspInit, pspSos, smuResp,
                                       static_cast<uint32_t>(pspRead),
                                       static_cast<uint32_t>(smuRead)};
                rnPublish(this, "navi33-firmware-preflight", pn, pv, 5);
                // P6.1 is a fail-closed readiness report, not a PSP loader.
                // In particular, SOS-alive is not proof of ring ownership.
                const rn_navi33::p6_psp::Evidence evidence{
                    check.eligibleForComputeBringup(), true, p6MemoryVerified,
                    pspRead, pspInit, pspSos
                };
                const auto gate = rn_navi33::p6_psp::inspect(evidence);
                const char *const p6names[] = {"status", "ifwi_ready",
                    "sos_alive", "firmware_required", "may_issue_psp_commands"};
                const uint32_t p6values[] = {
                    static_cast<uint32_t>(gate.status),
                    static_cast<uint32_t>(gate.ifwiReady),
                    static_cast<uint32_t>(gate.secureOsAlive),
                    static_cast<uint32_t>(gate.firmwareRequired),
                    static_cast<uint32_t>(gate.mayIssuePspCommands)
                };
                rnPublish(this, "navi33-p6-psp-preflight", p6names,
                          p6values, sizeof(p6values) / sizeof(p6values[0]));

                // P6.2: validate locally generated PSP13.0.7 firmware metadata,
                // then run the staging/ownership contract. This path performs
                // NO firmware copy, VRAM reservation, ring creation or PSP MMIO.
                rn_navi33::p6_boot::Evidence bootEvidence{};
#if defined(RN_HAVE_PSP_FW) && defined(RN_PSP_FW_METADATA_V1)
                bootEvidence.major = static_cast<uint8_t>(kRNPspTargetMajor);
                bootEvidence.minor = static_cast<uint8_t>(kRNPspTargetMinor);
                bootEvidence.revision = static_cast<uint8_t>(kRNPspTargetRevision);
                bootEvidence.exactTargetName = kRNPspExactTargetName != 0;
                bootEvidence.sourceDigestPresent =
                    sizeof(kRNPspSourceSha256) == 65u && kRNPspSourceSha256[0] != '\0';
                bootEvidence.sourceBytes = kRNPspSourceBytes;
                bootEvidence.componentMask = kRNPspComponentMask;
                bootEvidence.maxComponentBytes = kRNPspMaxComponentBytes;
#endif
                const auto bootFirmware = rn_navi33::p6_boot::inspect(bootEvidence);
                const char *const bfn[] = {
                    "error", "metadata_verified", "target_major", "target_minor",
                    "target_revision", "source_bytes", "component_mask",
                    "required_mask", "max_component_bytes", "stage_bytes",
                    "may_load_psp"
                };
                const uint32_t bfv[] = {
                    static_cast<uint32_t>(bootFirmware.error),
                    static_cast<uint32_t>(bootFirmware.metadataVerified),
                    bootEvidence.major, bootEvidence.minor, bootEvidence.revision,
                    bootEvidence.sourceBytes, bootEvidence.componentMask,
                    bootFirmware.requiredMask, bootEvidence.maxComponentBytes,
                    bootFirmware.stageBytes,
                    static_cast<uint32_t>(bootFirmware.mayLoadPsp)
                };
                rnPublish(this, "navi33-p6-boot-firmware", bfn, bfv,
                          sizeof(bfv) / sizeof(bfv[0]));

                const auto stageMemory = mmhubRead ?
                    rn_navi33::makeMemoryWindow(fbBase, fbTop, fbOffset,
                                                vramMbytes, bar0Bytes) :
                    rn_navi33::MemoryWindow{};
                const auto stageGeometry = rn_navi33::address::makeGeometry(
                    stageMemory, gfxFbOffsetRaw, gfxFbOffsetRead);

                // P6.4b read-only boot-framebuffer evidence. PE_current_console()
                // is XNU's current console description; on x86_64 its base is
                // the physical framebuffer address. Translate it into a BAR0
                // aperture offset, then compute (but do not claim) a VRAM
                // reservation candidate for the VMID0 GART table.
                PE_Video consoleInfo {};
                const bool consoleRead = PE_current_console(&consoleInfo) == 0;
                const uint64_t barPhys = aperture ?
                    aperture->getPhysicalSegment(0, nullptr,
                                                 kIOMemoryMapperNone) : 0ull;
                rn_navi33::vram_reservation::Input reserveInput{};
                reserveInput.barPhys = barPhys;
                reserveInput.barBytes = bar0Bytes;
                reserveInput.cpuVisibleBytes =
                    stageGeometry.ok() ? stageGeometry.geometry.cpuVisibleBytes : 0ull;
                if (consoleRead) {
                    reserveInput.consoleRawAddress =
                        static_cast<uint64_t>(consoleInfo.v_baseAddr);
                    reserveInput.consoleRowBytes =
                        static_cast<uint64_t>(consoleInfo.v_rowBytes);
                    reserveInput.consoleHeight =
                        static_cast<uint32_t>(consoleInfo.v_height);
                }
                const auto reservePlan =
                    rn_navi33::vram_reservation::inspect(reserveInput);

                const char *const fbn[] = {
                    "error", "console_read", "bar_phys_lo", "bar_phys_hi",
                    "bar_bytes", "console_raw_lo", "console_raw_hi",
                    "console_phys_lo", "console_phys_hi",
                    "physical_encoded", "row_bytes", "width", "height", "depth",
                    "console_offset_lo", "console_offset_hi",
                    "console_bytes", "inside_bar"
                };
                const uint32_t fbv[] = {
                    static_cast<uint32_t>(reservePlan.error),
                    static_cast<uint32_t>(consoleRead),
                    static_cast<uint32_t>(barPhys),
                    static_cast<uint32_t>(barPhys >> 32),
                    static_cast<uint32_t>(bar0Bytes),
                    static_cast<uint32_t>(reserveInput.consoleRawAddress),
                    static_cast<uint32_t>(reserveInput.consoleRawAddress >> 32),
                    static_cast<uint32_t>(reservePlan.consolePhys),
                    static_cast<uint32_t>(reservePlan.consolePhys >> 32),
                    static_cast<uint32_t>(reservePlan.consolePhysicalEncoded),
                    static_cast<uint32_t>(reserveInput.consoleRowBytes),
                    static_cast<uint32_t>(consoleInfo.v_width),
                    static_cast<uint32_t>(consoleInfo.v_height),
                    static_cast<uint32_t>(consoleInfo.v_depth),
                    static_cast<uint32_t>(reservePlan.console.offset),
                    static_cast<uint32_t>(reservePlan.console.offset >> 32),
                    static_cast<uint32_t>(reservePlan.consoleBytes),
                    static_cast<uint32_t>(reservePlan.consoleInsideBar)
                };
                rnPublish(this, "navi33-boot-framebuffer", fbn, fbv,
                          sizeof(fbv) / sizeof(fbv[0]));

                const char *const vrn[] = {
                    "error", "page_table_offset", "page_table_bytes",
                    "scratch_offset", "dummy_offset", "candidate_nonoverlap",
                    "reservation_owned", "may_write_vram"
                };
                const uint32_t vrv[] = {
                    static_cast<uint32_t>(reservePlan.error),
                    static_cast<uint32_t>(reservePlan.pageTable.offset),
                    static_cast<uint32_t>(reservePlan.pageTable.bytes),
                    static_cast<uint32_t>(reservePlan.scratch.offset),
                    static_cast<uint32_t>(reservePlan.dummy.offset),
                    static_cast<uint32_t>(reservePlan.candidateNonOverlapping),
                    static_cast<uint32_t>(reservePlan.reservationOwned),
                    static_cast<uint32_t>(reservePlan.mayWriteVram)
                };
                rnPublish(this, "navi33-vram-reservation-candidate", vrn, vrv,
                          sizeof(vrv) / sizeof(vrv[0]));

                // P6.5: software-only ownership ledger. This claims the
                // candidate inside RedNavi's bookkeeping, but does not touch
                // VRAM or authorize any GART/PTE operation.
                uint32_t vramOwnerArg = 0;
                const bool vramOwnerRequested =
                    stageGeometry.ok() &&
                    reservePlan.candidateNonOverlapping &&
                    PE_parse_boot_argn("rednavi_navi33_vram_owner_probe",
                                       &vramOwnerArg, sizeof(vramOwnerArg)) &&
                    vramOwnerArg == 1u;
                rn_navi33::vram_ledger::Input ledgerInput{};
                ledgerInput.requested = vramOwnerRequested;
                ledgerInput.geometryVerified = stageGeometry.ok();
                ledgerInput.consoleVerified =
                    reservePlan.error == rn_navi33::vram_reservation::Error::Ok &&
                    reservePlan.consoleInsideBar &&
                    reservePlan.consolePhysicalEncoded;
                ledgerInput.allocatorInactive = !gpuLive && !vmReady;
                ledgerInput.cpuVisibleBytes =
                    stageGeometry.ok() ? stageGeometry.geometry.cpuVisibleBytes : 0ull;
                ledgerInput.console = {
                    reservePlan.console.offset, reservePlan.console.bytes
                };
                ledgerInput.pageTable = {
                    reservePlan.pageTable.offset, reservePlan.pageTable.bytes
                };
                ledgerInput.scratch = {
                    reservePlan.scratch.offset, reservePlan.scratch.bytes
                };
                ledgerInput.dummy = {
                    reservePlan.dummy.offset, reservePlan.dummy.bytes
                };
                const auto ledger =
                    rn_navi33::vram_ledger::inspect(ledgerInput);

                if (ledger.ledgerOwned) {
                    navi33VramLedgerOwned = true;
                    navi33VramPtOffset = reservePlan.pageTable.offset;
                    navi33VramScratchOffset = reservePlan.scratch.offset;
                    navi33VramDummyOffset = reservePlan.dummy.offset;
                } else if (vramOwnerRequested) {
                    navi33VramLedgerOwned = false;
                    navi33VramPtOffset = 0;
                    navi33VramScratchOffset = 0;
                    navi33VramDummyOffset = 0;
                }

                const char *const vln[] = {
                    "error", "requested", "geometry_verified",
                    "console_verified", "allocator_inactive",
                    "page_table_offset", "page_table_bytes",
                    "scratch_offset", "dummy_offset", "reserved_bytes",
                    "ledger_owned", "allocator_carve_required",
                    "may_write_vram", "may_install_gart_ptes"
                };
                const uint32_t vlv[] = {
                    static_cast<uint32_t>(ledger.error),
                    static_cast<uint32_t>(vramOwnerRequested),
                    static_cast<uint32_t>(ledgerInput.geometryVerified),
                    static_cast<uint32_t>(ledgerInput.consoleVerified),
                    static_cast<uint32_t>(ledgerInput.allocatorInactive),
                    static_cast<uint32_t>(navi33VramPtOffset),
                    static_cast<uint32_t>(reservePlan.pageTable.bytes),
                    static_cast<uint32_t>(navi33VramScratchOffset),
                    static_cast<uint32_t>(navi33VramDummyOffset),
                    static_cast<uint32_t>(ledger.reservedBytes),
                    static_cast<uint32_t>(navi33VramLedgerOwned),
                    static_cast<uint32_t>(ledger.allocatorCarveRequired),
                    static_cast<uint32_t>(ledger.mayWriteVram),
                    static_cast<uint32_t>(ledger.mayInstallGartPtes)
                };
                rnPublish(this, "navi33-vram-reservation-ledger", vln, vlv,
                          sizeof(vlv) / sizeof(vlv[0]));

                // P6.6: software-only allocator carve + runtime quiescence
                // audit. This models removal of the owned tail range from the
                // future visible-VRAM allocator, but does NOT mutate visPool.
                uint32_t carveArg = 0;
                const bool carveRequested =
                    navi33VramLedgerOwned &&
                    PE_parse_boot_argn("rednavi_navi33_allocator_carve_probe",
                                       &carveArg, sizeof(carveArg)) &&
                    carveArg == 1u;
                rn_navi33::allocator_carve::Input carveInput{};
                carveInput.requested = carveRequested;
                carveInput.ledgerOwned = navi33VramLedgerOwned;
                carveInput.gpuLive = gpuLive;
                carveInput.vmReady = vmReady;
                carveInput.gfxQueueOk = gfxQueueOk;
                carveInput.rednaviFramebufferReady = fbReady;
                carveInput.boCount = boCount;
                carveInput.vmidUsed = vmidUsed;
                carveInput.reservation = {
                    navi33VramScratchOffset,
                    ledger.reservedBytes
                };
                const auto carve =
                    rn_navi33::allocator_carve::inspect(carveInput);

                const char *const can[] = {
                    "error", "requested", "ledger_owned", "quiescent",
                    "gpu_live", "vm_ready", "gfx_queue_ok", "fb_ready",
                    "bo_count", "vmid_used", "pool_start", "pool_bytes",
                    "reservation_start", "reservation_bytes",
                    "before_pages", "reserved_pages", "after_pages",
                    "left_start", "left_bytes", "right_start", "right_bytes",
                    "carve_verified", "may_mutate_allocator", "may_write_vram"
                };
                const uint32_t cav[] = {
                    static_cast<uint32_t>(carve.error),
                    static_cast<uint32_t>(carveRequested),
                    static_cast<uint32_t>(navi33VramLedgerOwned),
                    static_cast<uint32_t>(carve.quiescent),
                    static_cast<uint32_t>(gpuLive),
                    static_cast<uint32_t>(vmReady),
                    static_cast<uint32_t>(gfxQueueOk),
                    static_cast<uint32_t>(fbReady),
                    boCount,
                    vmidUsed,
                    static_cast<uint32_t>(carveInput.pool.offset),
                    static_cast<uint32_t>(carveInput.pool.bytes),
                    static_cast<uint32_t>(carveInput.reservation.offset),
                    static_cast<uint32_t>(carveInput.reservation.bytes),
                    carve.beforePages,
                    carve.reservedPages,
                    carve.afterPages,
                    static_cast<uint32_t>(carve.left.offset),
                    static_cast<uint32_t>(carve.left.bytes),
                    static_cast<uint32_t>(carve.right.offset),
                    static_cast<uint32_t>(carve.right.bytes),
                    static_cast<uint32_t>(carve.carveVerified),
                    static_cast<uint32_t>(carve.mayMutateAllocator),
                    static_cast<uint32_t>(carve.mayWriteVram)
                };
                rnPublish(this, "navi33-p6-allocator-carve", can, cav,
                          sizeof(cav) / sizeof(cav[0]));

                // P6.4 ownership-only probe. Opt-in allocation of Linux's 1 MiB
                // PSP fw_pri host backing. IOKit may create/retain DMA IOVAs,
                // but this path NEVER writes a GPU GART PTE or hub register.
                uint32_t dmaOwnerArg = 0;
                const bool dmaOwnerRequested =
                    gate.status == rn_navi33::p6_psp::Status::NeedsVerifiedFirmware &&
                    stageGeometry.ok() &&
                    PE_parse_boot_argn("rednavi_navi33_dma_owner_probe",
                                       &dmaOwnerArg, sizeof(dmaOwnerArg)) &&
                    dmaOwnerArg == 1u;
                uint32_t dmaMappingError = 0;
                const bool dmaOwnerPrepared =
                    prepareNavi33FwPriDma(dmaOwnerRequested, &dmaMappingError);
                const rn_navi33::fw_pri_owner::Input dmaOwnerInput{
                    dmaOwnerRequested,
                    navi33FwPriBuf != nullptr,
                    navi33FwPriDma != nullptr && navi33FwPriPrepared,
                    dmaOwnerPrepared,
                    navi33FwPriBytes,
                    navi33FwPriPageCount
                };
                const auto dmaOwner =
                    rn_navi33::fw_pri_owner::inspect(dmaOwnerInput);

                uint64_t dmaFirst = 0, dmaLast = 0, dmaMin = 0, dmaMax = 0;
                bool dmaContiguous = false;
                if (dmaOwner.ownerVerified && navi33FwPriPages &&
                    navi33FwPriPageCount) {
                    dmaFirst = navi33FwPriPages[0];
                    dmaLast = navi33FwPriPages[navi33FwPriPageCount - 1u];
                    dmaMin = dmaMax = dmaFirst;
                    dmaContiguous = true;
                    for (uint32_t p = 0; p < navi33FwPriPageCount; ++p) {
                        const uint64_t a = navi33FwPriPages[p];
                        if (a < dmaMin) dmaMin = a;
                        if (a > dmaMax) dmaMax = a;
                        if (p && a != navi33FwPriPages[p - 1u] + 4096ull)
                            dmaContiguous = false;
                    }
                }
                const char *const don[] = {
                    "error", "requested", "buffer_allocated", "dma_prepared",
                    "pages_verified", "mapping_error", "bytes", "page_count",
                    "address_bits", "first_iova_lo", "first_iova_hi",
                    "last_iova_lo", "last_iova_hi", "min_iova_lo",
                    "min_iova_hi", "max_iova_lo", "max_iova_hi",
                    "physically_contiguous_iova", "owner_verified",
                    "gpu_mapped", "may_install_gart_ptes"
                };
                const uint32_t dov[] = {
                    static_cast<uint32_t>(dmaOwner.error),
                    static_cast<uint32_t>(dmaOwnerRequested),
                    static_cast<uint32_t>(navi33FwPriBuf != nullptr),
                    static_cast<uint32_t>(navi33FwPriDma != nullptr &&
                                          navi33FwPriPrepared),
                    static_cast<uint32_t>(dmaOwnerPrepared),
                    dmaMappingError,
                    static_cast<uint32_t>(navi33FwPriBytes),
                    navi33FwPriPageCount,
                    rn_navi33::fw_pri_owner::kDmaAddressBits,
                    static_cast<uint32_t>(dmaFirst),
                    static_cast<uint32_t>(dmaFirst >> 32),
                    static_cast<uint32_t>(dmaLast),
                    static_cast<uint32_t>(dmaLast >> 32),
                    static_cast<uint32_t>(dmaMin),
                    static_cast<uint32_t>(dmaMin >> 32),
                    static_cast<uint32_t>(dmaMax),
                    static_cast<uint32_t>(dmaMax >> 32),
                    static_cast<uint32_t>(dmaContiguous),
                    static_cast<uint32_t>(dmaOwner.ownerVerified),
                    0u,
                    static_cast<uint32_t>(dmaOwner.mayInstallGartPtes)
                };
                rnPublish(this, "navi33-fw-pri-dma-owner", don, dov,
                          sizeof(dov) / sizeof(dov[0]));

                // P6.3/P6.4 GART plan. The DMA owner above is real software
                // ownership; VRAM page-table ownership remains deliberately
                // absent, so this still cannot authorize GART programming.
                rn_navi33::gart_owner::Input gartInput{};
                if (stageGeometry.ok()) {
                    gartInput.fbStart = stageGeometry.geometry.mcStart.value;
                    gartInput.fbBytes = stageGeometry.geometry.usableVramBytes;
                    gartInput.cpuVisibleBytes =
                        stageGeometry.geometry.cpuVisibleBytes;
                }
                if (reservePlan.candidateNonOverlapping) {
                    gartInput.pageTable = {
                        reservePlan.pageTable.offset, reservePlan.pageTable.bytes
                    };
                    gartInput.scratch = {
                        reservePlan.scratch.offset, reservePlan.scratch.bytes
                    };
                    gartInput.dummy = {
                        reservePlan.dummy.offset, reservePlan.dummy.bytes
                    };
                }
                // P6.5 software ownership is enough to advance the planning
                // contract, but NOT enough to authorize a hardware write.
                // gart_owner::Plan::mayProgramGart remains hard false.
                gartInput.vramOwnershipVerified = navi33VramLedgerOwned;
                gartInput.reservationMapVerified = navi33VramLedgerOwned;
                gartInput.dmaBytes = navi33FwPriBytes;
                gartInput.dmaPageCount = navi33FwPriPageCount;
                gartInput.dmaMappingRetained = dmaOwner.ownerVerified;
                gartInput.dmaPagesVerified = dmaOwner.ownerVerified;
                const auto gartPlan = rn_navi33::gart_owner::inspect(gartInput);
                const char *const gon[] = {
                    "error", "gart_start_lo", "gart_start_hi",
                    "gart_end_lo", "gart_end_hi", "gart_mib",
                    "page_table_bytes", "reservation_candidate",
                    "candidate_pt_offset", "vram_owner", "dma_owner",
                    "ownership_verified", "may_program_gart"
                };
                const uint32_t gov[] = {
                    static_cast<uint32_t>(gartPlan.error),
                    static_cast<uint32_t>(gartPlan.gartStart),
                    static_cast<uint32_t>(gartPlan.gartStart >> 32),
                    static_cast<uint32_t>(gartPlan.gartEnd),
                    static_cast<uint32_t>(gartPlan.gartEnd >> 32),
                    static_cast<uint32_t>(gartInput.gartBytes >> 20),
                    static_cast<uint32_t>(gartPlan.pageTableBytes),
                    static_cast<uint32_t>(reservePlan.candidateNonOverlapping),
                    static_cast<uint32_t>(reservePlan.pageTable.offset),
                    static_cast<uint32_t>(navi33VramLedgerOwned),
                    static_cast<uint32_t>(dmaOwner.ownerVerified),
                    static_cast<uint32_t>(gartPlan.ownershipVerified),
                    static_cast<uint32_t>(gartPlan.mayProgramGart)
                };
                rnPublish(this, "navi33-gart-owner-plan", gon, gov,
                          sizeof(gov) / sizeof(gov[0]));

                // PSP fw_pri is host DMA memory reached through VMID0/GART.
                // Firmware provenance and VRAM/GART ownership are still
                // independent gates, so no firmware is copied here.
                rn_navi33::p6_staging::Input stageInput{};
                stageInput.p6PreflightPassed =
                    gate.status == rn_navi33::p6_psp::Status::NeedsVerifiedFirmware;
                // P7 only trusts the bytes when gen_psp_fw_header.py was
                // given an independently sourced expected SHA-256 and the
                // input matched it exactly. Merely embedding its own digest
                // is not provenance proof.
                bool firmwareDigestTrusted = false;
#if defined(RN_HAVE_PSP_FW) && defined(RN_PSP_FW_DIGEST_TRUSTED)
                firmwareDigestTrusted = RN_PSP_FW_DIGEST_TRUSTED == 1;
#endif
                stageInput.firmwareBytesVerified = firmwareDigestTrusted;
                stageInput.firmwareMetadataVerified = bootFirmware.metadataVerified;
                stageInput.maxFirmwareComponentBytes =
                    bootEvidence.maxComponentBytes;
                stageInput.gartOwnershipVerified = gartPlan.ownershipVerified;
                stageInput.gartStart = gartPlan.gartStart;
                stageInput.gartEnd = gartPlan.gartEnd;
                stageInput.fwPriGpuVa = gartPlan.fwPriGpuAddress;
                stageInput.fwPriBytes = rn_navi33::p6_boot::kStageBytes;
                stageInput.dmaMappingRetained = dmaOwner.ownerVerified;
                stageInput.dmaPagesVerified = dmaOwner.ownerVerified;
                stageInput.dmaBytes = navi33FwPriBytes;
                const auto stage = rn_navi33::p6_staging::validate(stageInput);
                const char *const stn[] = {
                    "error", "p6_ready", "firmware_metadata_verified",
                    "firmware_digest_trusted", "gart_owner", "dma_owner",
                    "fw_pri_lo", "fw_pri_hi", "stage_bytes",
                    "may_copy_firmware", "may_issue_psp_commands"
                };
                const uint32_t stv[] = {
                    static_cast<uint32_t>(stage.error),
                    static_cast<uint32_t>(stageInput.p6PreflightPassed),
                    static_cast<uint32_t>(stageInput.firmwareMetadataVerified),
                    static_cast<uint32_t>(stageInput.firmwareBytesVerified),
                    static_cast<uint32_t>(stageInput.gartOwnershipVerified),
                    static_cast<uint32_t>(dmaOwner.ownerVerified),
                    static_cast<uint32_t>(stage.fwPriGpuVa),
                    static_cast<uint32_t>(stage.fwPriGpuVa >> 32),
                    static_cast<uint32_t>(stageInput.fwPriBytes),
                    static_cast<uint32_t>(stage.mayCopyFirmware),
                    static_cast<uint32_t>(stage.mayIssuePspCommands)
                };
                rnPublish(this, "navi33-p6-staging", stn, stv,
                          sizeof(stv) / sizeof(stv[0]));

                // P7: first real Navi33 takeover path. It stays behind the
                // existing unsafe-P4 build/runtime admission plus a separate
                // numbered execution step. Step 1 installs only the owned
                // VMID0/GART mapping; step 2 may additionally hand the
                // independently verified PSP13.0.7 components to the
                // bootloader. Step 3 is only a render-pipeline admission
                // signal here; MES/graphics queues remain a later explicit
                // hardware checkpoint.
                uint32_t p7StepArg = 0;
                PE_parse_boot_argn("rednavi_navi33_p7_step",
                                   &p7StepArg, sizeof(p7StepArg));
                p7StepArg =
                    rn_navi33::p7_runtime::sanitizeStep(p7StepArg);

                rn_navi33::p7_runtime::Input p7Input{};
                p7Input.step = p7StepArg;
                p7Input.identityVerified = check.eligibleForComputeBringup();
                p7Input.memoryVerified = p6MemoryVerified && stageGeometry.ok();
                p7Input.ledgerOwned = navi33VramLedgerOwned;
                p7Input.carveVerified = carve.carveVerified;
                p7Input.runtimeQuiescent = carve.quiescent;
                p7Input.dmaOwnerVerified = dmaOwner.ownerVerified;
                p7Input.gartOwnerVerified = gartPlan.ownershipVerified;
                p7Input.firmwareMetadataVerified = bootFirmware.metadataVerified;
                p7Input.firmwareDigestTrusted = firmwareDigestTrusted;
                p7Input.secureOsAlive = gate.secureOsAlive;
                auto p7Gate = rn_navi33::p7_runtime::inspect(p7Input);

                bool p7GartReady = false;
                bool p7PteVerified = false;
                int32_t p7FlushRc = -1;
                uint32_t p7BadPte = 0xffffffffu;
                struct rn_gmc p7Gmc{};

                if (p7Gate.mayProgramGart) {
                    p7Gmc.fb_start = stageGeometry.geometry.mcStart.value;
                    p7Gmc.fb_end = p7Gmc.fb_start +
                        stageGeometry.geometry.usableVramBytes - 1u;
                    p7Gmc.gart_start = gartPlan.gartStart;
                    p7Gmc.gart_end = gartPlan.gartEnd;
                    p7Gmc.pt_vram_offset = navi33VramPtOffset;
                    p7Gmc.scratch_vram_offset = navi33VramScratchOffset;
                    p7Gmc.dummy_vram_offset = navi33VramDummyOffset;

                    // Every PTE outside fw_pri starts invalid. The first 256
                    // entries map the exact retained IODMACommand page list;
                    // they are deliberately NOT derived from first_iova.
                    // The device may still see these PTEs after a later
                    // readback/invalidation failure. Keep fw_pri DMA pinned.
                    navi33P7GartProgrammed = true;
                    rn_gart_clear(&gpu, &p7Gmc);
                    for (uint32_t p = 0; p < navi33FwPriPageCount; ++p)
                        rn_gart_map_page(&gpu, &p7Gmc, p,
                                         navi33FwPriPages[p]);

                    p7PteVerified = true;
                    for (uint32_t p = 0; p < navi33FwPriPageCount; ++p) {
                        const uint64_t got = rn_gart_read_pte(&gpu, &p7Gmc, p);
                        const uint64_t expected =
                            rn_system_pte(navi33FwPriPages[p]);
                        if (got != expected) {
                            p7PteVerified = false;
                            p7BadPte = p;
                            break;
                        }
                    }

                    if (p7PteVerified) {
                        // Linux GMC11 early hw_init enables only MMHUB, then
                        // flushes VMID0 through MMHUB engine 17. GFXHUB is a
                        // graphics-block responsibility and Linux refuses a
                        // GFXHUB flush while gfx.is_poweron is false. P7 runs
                        // before PSP/SMU/GC bring-up, so touching GFXHUB here
                        // was too early and produced a permanent ACK timeout
                        // on real Navi33 hardware.
                        rn_mmhub_enable(&gpu, &p7Gmc);
                        rn_mmhub_prepare_invalidation(&gpu);
                        p7FlushRc = rn_mmhub_gart_flush(&gpu);
                        p7GartReady = p7FlushRc == 0;
                        if (p7GartReady) navi33P7GartProgrammed = true;
                    }
                }

                uint32_t p7MmhubFlushStats[3] = {};
                rn_mmhub_flush_stats(p7MmhubFlushStats);
                const char *const p7gn[] = {
                    "step", "gate_error", "may_program_gart",
                    "pte_verified", "bad_pte", "flush_rc", "gart_ready",
                    "flush_hub", "gfxhub_deferred", "mmhub_flush_failures",
                    "mmhub_last_ack", "mmhub_last_sem",
                    "gart_start_lo", "gart_start_hi", "fw_pri_lo",
                    "fw_pri_hi", "pt_offset"
                };
                const uint32_t p7gv[] = {
                    p7StepArg, static_cast<uint32_t>(p7Gate.error),
                    static_cast<uint32_t>(p7Gate.mayProgramGart),
                    static_cast<uint32_t>(p7PteVerified), p7BadPte,
                    static_cast<uint32_t>(p7FlushRc),
                    static_cast<uint32_t>(p7GartReady),
                    1u, 1u, p7MmhubFlushStats[0],
                    p7MmhubFlushStats[1], p7MmhubFlushStats[2],
                    static_cast<uint32_t>(gartPlan.gartStart),
                    static_cast<uint32_t>(gartPlan.gartStart >> 32),
                    static_cast<uint32_t>(gartPlan.fwPriGpuAddress),
                    static_cast<uint32_t>(gartPlan.fwPriGpuAddress >> 32),
                    static_cast<uint32_t>(navi33VramPtOffset)
                };
                rnPublish(this, "navi33-p7-gart-runtime", p7gn, p7gv,
                          sizeof(p7gv) / sizeof(p7gv[0]));

                uint32_t p7ChainDone = 0;
                uint32_t p7ChainTotal = 0;
                uint32_t p7ChainTarget = 0;
                uint32_t p7FailedIndex = 0xffffffffu;
                uint32_t p7FailureReason = 0;
                uint32_t p7LastIndex = 0xffffffffu;
                uint32_t p7LastCmd = 0;
                uint32_t p7LastSize = 0;
                uint32_t p7LastC35Before = 0;
                uint32_t p7LastC35After = 0;
                uint32_t p7LastC36 = 0;
                uint32_t p7LastC58 = 0;
                uint32_t p7LastC81 = pspSos;
                uint32_t p7SosAfter = pspSos;
                bool p7PspOk = false;
#if defined(RN_HAVE_PSP_FW)
                p7ChainTotal = static_cast<uint32_t>(
                    sizeof(kRNPspChain) / sizeof(kRNPspChain[0]));
                p7ChainTarget = p7ChainTotal;
                uint32_t p7LimitArg = 0;
                if (PE_parse_boot_argn(
                        "rednavi_navi33_p7_psp_limit",
                        &p7LimitArg, sizeof(p7LimitArg)) &&
                    p7LimitArg > 0 && p7LimitArg < p7ChainTarget) {
                    p7ChainTarget = p7LimitArg;
                }

                if (p7Gate.mayIssuePspBootloader && p7GartReady &&
                    !gate.secureOsAlive) {
                    uint32_t mp0Base = 0;
                    if (ipBase(kRNHwIdMp0, &mp0Base) &&
                        navi33FwPriBuf && navi33FwPriBuf->getBytesNoCopy()) {
                        const uint32_t r35 =
                            (mp0Base + kRNMp0C2pmsg35) * 4;
                        const uint32_t r36 =
                            (mp0Base + kRNMp0C2pmsg36) * 4;
                        const uint32_t r58 =
                            (mp0Base + kRNMp0C2pmsg58) * 4;
                        const uint32_t r81 =
                            (mp0Base + kRNMp0C2pmsg81) * 4;
                        uint8_t *const fwPri = static_cast<uint8_t *>(
                            navi33FwPriBuf->getBytesNoCopy());

                        bool chainOk = true;
                        for (uint32_t index = 0;
                             index < p7ChainTarget; ++index) {
                            const auto &component = kRNPspChain[index];
                            p7LastIndex = index;
                            p7LastCmd = component.cmd;
                            p7LastSize = component.size;

                            uint32_t ready = 0;
                            for (uint32_t wait = 0; wait < 2000u; ++wait) {
                                ready = rd(r35);
                                if (ready == kRNPspBlReady) break;
                                IOSleep(1);
                            }
                            p7LastC35Before = ready;
                            if (ready != kRNPspBlReady) {
                                p7FailureReason = 1; // bootloader not ready
                                p7FailedIndex = index;
                                chainOk = false;
                                break;
                            }
                            if (component.size > navi33FwPriBytes) {
                                p7FailureReason = 2; // component too large
                                p7FailedIndex = index;
                                chainOk = false;
                                break;
                            }

                            memset(fwPri, 0, navi33FwPriBytes);
                            memcpy(fwPri, component.data, component.size);
                            // Bare-metal Linux normally allocates fw_pri in
                            // GTT for PSP13. The P7 GART PTEs are
                            // SYSTEM+SNOOPED, so x86 cache-coherent host
                            // memory is intentionally used here as well.
                            __sync_synchronize();

                            const uint32_t before81 = rd(r81);
                            wr(r36, static_cast<uint32_t>(
                                gartPlan.fwPriGpuAddress >> 20));
                            __sync_synchronize();
                            p7LastC36 = rd(r36);
                            wr(r35, component.cmd);

                            if (component.cmd == kRNPspBlLoadSos) {
                                // Linux psp_v13_0_bootloader_load_sos waits
                                // 20 ms before checking for a C2PMSG_81
                                // transition.
                                IOSleep(20);
                                uint32_t after81 = before81;
                                for (uint32_t wait = 0;
                                     wait < 2000u; ++wait) {
                                    after81 = rd(r81);
                                    if (after81 != before81) break;
                                    IOSleep(1);
                                }
                                p7LastC35After = rd(r35);
                                p7LastC58 = rd(r58);
                                p7LastC81 = after81;
                                if (after81 == before81) {
                                    p7FailureReason = 4; // sOS sign-of-life did not change
                                    p7FailedIndex = index;
                                    chainOk = false;
                                    break;
                                }
                            } else {
                                uint32_t after = 0;
                                IOSleep(5);
                                for (uint32_t wait = 0;
                                     wait < 2000u; ++wait) {
                                    after = rd(r35);
                                    if (after == kRNPspBlReady) break;
                                    IOSleep(1);
                                }
                                p7LastC35After = after;
                                p7LastC58 = rd(r58);
                                p7LastC81 = rd(r81);
                                if (after != kRNPspBlReady) {
                                    p7FailureReason = 3; // command completion timeout/error
                                    p7FailedIndex = index;
                                    chainOk = false;
                                    break;
                                }
                            }
                            ++p7ChainDone;
                        }
                        p7SosAfter = rd(r81);
                        p7LastC81 = p7SosAfter;
                        // A deliberately limited chain is a successful
                        // checkpoint, not a complete PSP boot. Only advertise
                        // psp_ok after the full generated chain reaches sOS.
                        p7PspOk = chainOk &&
                            p7ChainTarget == p7ChainTotal &&
                            p7ChainDone == p7ChainTotal &&
                            p7SosAfter != 0;
                    } else {
                        p7FailureReason = 5; // MP0 base or fw_pri CPU buffer missing
                    }
                } else if (gate.secureOsAlive) {
                    p7PspOk = true;
                }
#endif

                const char *const p7pn[] = {
                    "step", "firmware_metadata_verified",
                    "firmware_digest_trusted", "gart_ready",
                    "may_issue_psp_bootloader",
                    "chain_done", "chain_total", "chain_target",
                    "chain_limited", "failed_index", "failure_reason",
                    "last_index", "last_cmd", "last_size",
                    "last_c2pmsg35_before", "last_c2pmsg35_after",
                    "last_c2pmsg36", "last_c2pmsg58",
                    "last_c2pmsg81",
                    "sos_before", "sos_after", "psp_ok"
                };
                const uint32_t p7pv[] = {
                    p7StepArg,
                    static_cast<uint32_t>(bootFirmware.metadataVerified),
                    static_cast<uint32_t>(firmwareDigestTrusted),
                    static_cast<uint32_t>(p7GartReady),
                    static_cast<uint32_t>(p7Gate.mayIssuePspBootloader),
                    p7ChainDone, p7ChainTotal, p7ChainTarget,
                    static_cast<uint32_t>(
                        p7ChainTarget != 0 &&
                        p7ChainTarget < p7ChainTotal),
                    p7FailedIndex, p7FailureReason,
                    p7LastIndex, p7LastCmd, p7LastSize,
                    p7LastC35Before, p7LastC35After,
                    p7LastC36, p7LastC58, p7LastC81,
                    pspSos, p7SosAfter,
                    static_cast<uint32_t>(p7PspOk)
                };
                rnPublish(this, "navi33-p7-psp-runtime", p7pn, p7pv,
                          sizeof(p7pv) / sizeof(p7pv[0]));

                // Re-evaluate step 3 after a same-boot PSP transition. This
                // connects P7 to the render/queue pipeline without starting
                // MES blindly: the next checkpoint must additionally prove
                // GC/MES firmware and queue workspace ownership.
                p7Input.secureOsAlive = p7SosAfter != 0;
                const auto p7AfterPsp =
                    rn_navi33::p7_runtime::inspect(p7Input);
                // P7.3 metadata and PMFW staging ownership are prepared
                // before the PSP GPCOM ring is created. Linux constructs
                // firmware.fw_buf before PSP hw_init and does not mutate the
                // active VMID0 mapping between LOAD_TOC and LOAD_IP_FW.
                bool p73PmfwPresent = false;
                bool p73PmfwMetadataVerified = false;
                bool p73PmfwDigestTrusted = false;
                bool p73PmfwTargetVerified = false;
                bool p73PmfwTargetMatch = false;
                uint32_t p73PmfwTargetRevision = 0;
                uint32_t p73HardwareMp1Revision =
                    static_cast<uint32_t>(check.observed.smu.revision);
                uint32_t p73PmfwVersion = 0;
                uint32_t p73PmfwBytes = 0;
#if defined(RN_HAVE_SMU_FW)
                p73PmfwPresent = true;
#if defined(RN_SMU_FW_METADATA_V2)
                p73PmfwMetadataVerified = true;
                p73PmfwDigestTrusted =
                    RN_SMU_FW_DIGEST_TRUSTED == 1;
                p73PmfwTargetVerified =
                    RN_SMU_FW_TARGET_VERIFIED == 1;
                p73PmfwTargetRevision =
                    kRNSmuFwTargetMp1Revision;
                p73PmfwTargetMatch =
                    check.eligibleForComputeBringup() &&
                    kRNSmuFwTargetMp1Major == check.observed.smu.major &&
                    kRNSmuFwTargetMp1Minor == check.observed.smu.minor &&
                    kRNSmuFwTargetMp1Revision == check.observed.smu.revision;
                p73PmfwVersion = kRNSmuFwVersion;
                p73PmfwBytes = kRNSmuFwSize;
#endif
#endif
                const uint64_t p73StageGpuVa =
                    gartPlan.gartStart +
                    static_cast<uint64_t>(
                        rn_navi33::p7_pmfw::kStageFirstPage) * 4096ull;
                uint32_t p73MappingError = 0;
                bool p73StagePrepared = false;
                bool p73StageMapped = false;
                bool p73StagePteVerified = false;
                uint32_t p73BadPte = 0xffffffffu;

                uint32_t p71MappingError = 0;
                bool p71RuntimeMapped = false;
                bool p71RingCreated = false;
                bool p71TocAccepted = false;
                uint32_t p71TocStatus = 0xffffffffu;
                uint32_t p71TmrBytes = 0;
                const uint64_t p71RingGpuVa =
                    gartPlan.gartStart +
                    static_cast<uint64_t>(
                        rn_navi33::p7_postpsp::kRingPage) * 4096ull;
                const uint64_t p71CmdGpuVa =
                    gartPlan.gartStart +
                    static_cast<uint64_t>(
                        rn_navi33::p7_postpsp::kCmdPage) * 4096ull;
                const uint64_t p71FenceGpuVa =
                    gartPlan.gartStart +
                    static_cast<uint64_t>(
                        rn_navi33::p7_postpsp::kFencePage) * 4096ull;

                // P7.13 stage preflight. The pinned RS64 MEC Code payload
                // is 267008 B, exceeding the 262144B PMFW DMA stage.
                // A separately consented 66-page mapping must be prepared
                // BEFORE the first PSP GPCOM command and GART flush.
                // No command is authorized by this stage expansion alone.
                uint32_t p713StageArgRaw = 0u;
                const bool p713StageOptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p713_stage_expand",
                        &p713StageArgRaw, sizeof(p713StageArgRaw)) &&
                    p713StageArgRaw == 1u;
                rn_navi33::p713_stage_expand::Input p713StageInput{};
                p713StageInput.requested = p7StepArg >= 15u;
                p713StageInput.explicitOptIn = p713StageOptIn;
                p713StageInput.identityVerified =
                    check.eligibleForComputeBringup() &&
                    check.observed.gc.major == 11u &&
                    check.observed.gc.minor == 0u &&
                    check.observed.gc.revision == 2u;
                p713StageInput.gartOwned = p7GartReady &&
                    gartPlan.ownershipVerified;
                p713StageInput.pmfwBytes = p73PmfwBytes;
                p713StageInput.gartStart = gartPlan.gartStart;
                p713StageInput.gartEnd = gartPlan.gartEnd;
#if defined(RN_HAVE_NAVI33_GC_MEC_FW) && \
    defined(RN_NAVI33_GC_MEC_FW_METADATA_V1)
                p713StageInput.firmwareVerified =
                    RN_NAVI33_GC_MEC_FW_SOURCE_VERIFIED == 1 &&
                    kRNNavi33GcMecSourceBytes == 406528u &&
                    kRNNavi33GcMecVersion == 2590u &&
                    kRNNavi33GcMecIpMajor == 11u &&
                    kRNNavi33GcMecIpMinor == 0u &&
                    kRNNavi33GcMecIpRevision == 2u;
                p713StageInput.firmwareBytes =
                    kRNNavi33GcMecCodeBytes;
#endif
                const auto p713StagePlan =
                    rn_navi33::p713_stage_expand::inspect(
                        p713StageInput);
                const uint64_t p713StageAllocBytes =
                    p713StagePlan.mayExpand ?
                    p713StagePlan.targetStageBytes :
                    static_cast<uint64_t>(p73PmfwBytes);

                // Step 3's first executable checkpoint: prove that the sOS
                // can consume a GPCOM ring backed by retained host DMA through
                // the same VMID0/GART we just validated. Stop after LOAD_TOC;
                // TMR ownership is a separate checkpoint.
                if (p7StepArg >= 3u && p7AfterPsp.mayStartQueues &&
                    p7GartReady && p7PspOk &&
                    prepareNavi33PspRuntimeDma(&p71MappingError)) {
                    bool ptesOk = true;
                    // Latch exposure before the first PTE can point to DMA.
                    // This does not claim that MMHUB invalidation succeeded.
                    navi33PspRuntimeMapped = true;
                    for (uint32_t p = 0;
                         p < rn_navi33::p7_postpsp::kRuntimePages; ++p) {
                        const uint32_t index =
                            rn_navi33::p7_postpsp::kRingPage + p;
                        rn_gart_map_page(&gpu, &p7Gmc, index,
                                         navi33PspRuntimePages[p]);
                        const uint64_t got =
                            rn_gart_read_pte(&gpu, &p7Gmc, index);
                        const uint64_t expected =
                            rn_system_pte(navi33PspRuntimePages[p]);
                        if (got != expected) {
                            ptesOk = false;
                            break;
                        }
                    }
                    // Step 5 pre-stages PMFW into the same VMID0 page
                    // table before the GPCOM ring is created. After this one
                    // invalidate, the ring lifetime does not modify GART.
#if defined(RN_HAVE_SMU_FW)
                    if (ptesOk && p7StepArg >= 5u &&
                        p73PmfwPresent &&
                        p73PmfwMetadataVerified &&
                        p73PmfwDigestTrusted &&
                        p73PmfwTargetVerified &&
                        p73PmfwTargetMatch &&
                        prepareNavi33PmfwDma(
                            p713StageAllocBytes, &p73MappingError)) {
                        p73StagePrepared = true;
                        uint8_t *const stage =
                            static_cast<uint8_t *>(
                                navi33PmfwBuf->getBytesNoCopy());
                        memset(stage, 0, navi33PmfwStageBytes);
                        memcpy(stage, kRNSmuFw, kRNSmuFwSize);
                        OSSynchronizeIO();

                        bool stagePtesOk = true;
                        // Even partial PTE installation pins this backing.
                        navi33PmfwMapped = true;
                        for (uint32_t p = 0;
                             p < navi33PmfwPageCount; ++p) {
                            const uint32_t index =
                                rn_navi33::p7_pmfw::kStageFirstPage + p;
                            rn_gart_map_page(
                                &gpu, &p7Gmc, index,
                                navi33PmfwPages[p]);
                            const uint64_t got =
                                rn_gart_read_pte(&gpu, &p7Gmc, index);
                            const uint64_t expected =
                                rn_system_pte(navi33PmfwPages[p]);
                            if (got != expected) {
                                stagePtesOk = false;
                                p73BadPte = index;
                                break;
                            }
                        }
                        p73StagePteVerified = stagePtesOk;
                    }
#endif
                    if (ptesOk) {
                        rn_mmhub_prepare_invalidation(&gpu);
                        p71RuntimeMapped =
                            rn_mmhub_gart_flush(&gpu) == 0;
                        if (p73StagePteVerified)
                            p73StageMapped = p71RuntimeMapped;
                    }
                    if (p73StageMapped)
                        navi33PmfwMapped = true;
                    if (p71RuntimeMapped) {
                        navi33PspRuntimeMapped = true;
                        p71RingCreated = createPspRingP7(p71RingGpuVa);
                    }

#if defined(RN_HAVE_PSP_FW)
                    if (p71RingCreated) {
                        uint8_t *const fwPri = static_cast<uint8_t *>(
                            navi33FwPriBuf->getBytesNoCopy());
                        memset(fwPri, 0, navi33FwPriBytes);
                        memcpy(fwPri, kRNPspToc, sizeof(kRNPspToc));
                        OSSynchronizeIO();

                        const uint32_t tocLen =
                            static_cast<uint32_t>(sizeof(kRNPspToc));
                        const uint32_t tocArgs[3] = {
                            static_cast<uint32_t>(
                                gartPlan.fwPriGpuAddress),
                            static_cast<uint32_t>(
                                gartPlan.fwPriGpuAddress >> 32),
                            tocLen
                        };
                        uint32_t tocResp[5] = {};
                        const bool fenceOk = pspSubmitP7(
                            p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                            kRNPspCmdLoadToc, tocArgs, 3,
                            tocResp, 5);
                        p71TocStatus = tocResp[0];
                        p71TmrBytes = tocResp[4];
                        p71TocAccepted =
                            fenceOk && p71TocStatus == 0 &&
                            p71TmrBytes != 0;
                    }
#endif
                }

                // P7.2 Step 4 is deliberately software-only.  Use the
                // P6.6 allocator carve plus the PSP-returned LOAD_TOC size to
                // claim a naturally aligned TMR inside RedNavi's verified
                // CPU-visible VRAM tail.  No PMFW, SETUP_TMR, or other PSP
                // command is issued at this stage.
                rn_navi33::p7_tmr::Input p72TmrInput{};
                p72TmrInput.requested = p7StepArg >= 4u;
                p72TmrInput.tocAccepted = p71TocAccepted;
                p72TmrInput.allocatorCarveVerified = carve.carveVerified;
                p72TmrInput.allocatorQuiescent = carve.quiescent;
                p72TmrInput.tmrBytes = p71TmrBytes;
                if (stageGeometry.ok()) {
                    p72TmrInput.cpuVisibleBytes =
                        stageGeometry.geometry.cpuVisibleBytes;
                    p72TmrInput.mcStart =
                        stageGeometry.geometry.mcStart.value;
                    // Linux amdgpu_gmc_vram_pa() translates VRAM MC to
                    // GPU-physical VRAM using FB_OFFSET, not PCI BAR0.
                    p72TmrInput.vramPhysBase =
                        stageGeometry.geometry.pteBase.value;
                }
                p72TmrInput.freeRange = {
                    carve.left.offset, carve.left.bytes
                };
                p72TmrInput.console = {
                    reservePlan.console.offset, reservePlan.console.bytes
                };
                p72TmrInput.priorReservation = {
                    navi33VramScratchOffset, ledger.reservedBytes
                };
                const auto p72Tmr =
                    rn_navi33::p7_tmr::inspect(p72TmrInput);

                const char *const p72n[] = {
                    "step", "error", "requested", "toc_accepted",
                    "carve_verified", "allocator_quiescent", "tmr_bytes",
                    "offset_lo", "offset_hi", "mc_lo", "mc_hi",
                    "pa_lo", "pa_hi", "natural_aligned",
                    "inside_visible_vram", "no_overlap",
                    "ownership_verified", "may_write_vram",
                    "may_issue_setup_tmr"
                };
                const uint32_t p72v[] = {
                    p7StepArg, static_cast<uint32_t>(p72Tmr.error),
                    static_cast<uint32_t>(p72TmrInput.requested),
                    static_cast<uint32_t>(p72TmrInput.tocAccepted),
                    static_cast<uint32_t>(
                        p72TmrInput.allocatorCarveVerified),
                    static_cast<uint32_t>(
                        p72TmrInput.allocatorQuiescent),
                    static_cast<uint32_t>(p72TmrInput.tmrBytes),
                    static_cast<uint32_t>(p72Tmr.candidate.offset),
                    static_cast<uint32_t>(
                        p72Tmr.candidate.offset >> 32),
                    static_cast<uint32_t>(p72Tmr.mc),
                    static_cast<uint32_t>(p72Tmr.mc >> 32),
                    static_cast<uint32_t>(p72Tmr.pa),
                    static_cast<uint32_t>(p72Tmr.pa >> 32),
                    static_cast<uint32_t>(p72Tmr.naturalAligned),
                    static_cast<uint32_t>(p72Tmr.insideVisibleVram),
                    static_cast<uint32_t>(p72Tmr.noOverlap),
                    static_cast<uint32_t>(p72Tmr.ownershipVerified),
                    static_cast<uint32_t>(p72Tmr.mayWriteVram),
                    static_cast<uint32_t>(p72Tmr.mayIssueSetupTmr)
                };
                rnPublish(this, "navi33-p7-tmr-plan",
                          p72n, p72v, sizeof(p72v) / sizeof(p72v[0]));

                // P7.3 Step 5: PMFW-only PSP checkpoint. Linux loads
                // PMFW before SETUP_TMR on MP0 13.0.7. The firmware staging
                // was already mapped before ring creation above, so no GART
                // mutation occurs between LOAD_TOC and LOAD_IP_FW.
                rn_navi33::p7_pmfw::Input p73Input{};
                p73Input.requested = p7StepArg >= 5u;
                p73Input.pspReady =
                    p7PspOk && p71RingCreated && p71RuntimeMapped;
                p73Input.tocAccepted = p71TocAccepted;
                p73Input.tmrOwnershipVerified =
                    p72Tmr.ownershipVerified;
                p73Input.firmwarePresent = p73PmfwPresent;
                p73Input.metadataVerified =
                    p73PmfwMetadataVerified;
                p73Input.digestTrusted = p73PmfwDigestTrusted;
                p73Input.targetVerified = p73PmfwTargetVerified;
                p73Input.targetMatch = p73PmfwTargetMatch;
                p73Input.firmwareBytes = p73PmfwBytes;
                p73Input.stageBytes =
                    rn_navi33::p7_pmfw::alignPage(p73PmfwBytes);
                const auto p73Gate =
                    rn_navi33::p7_pmfw::inspect(p73Input);

                bool p73FenceOk = false;
                bool p73PmfwAccepted = false;
                uint32_t p73Status = 0xffffffffu;
                uint32_t p73FwAddrLo = 0;
                uint32_t p73FwAddrHi = 0;
                uint32_t p73SmuRespBefore = smuResp;
                uint32_t p73SmuRespAfter = smuResp;

                // LOAD_TOC used the same submit helper earlier in this boot.
                // Reset trace fields so PMFW telemetry never reports the TOC
                // submission when PMFW staging itself fails.
                p7SubmitWptrBefore = 0;
                p7SubmitWptrAfter = 0;
                p7SubmitRptrBefore = 0;
                p7SubmitRptrAfterKick = 0;
                p7SubmitRptrFinal = 0;
                p7SubmitFenceBefore = 0;
                p7SubmitFenceExpected = 0;
                p7SubmitFenceObserved = 0;
                p7SubmitPolls = 0;
                p7SubmitWasIssued = false;
                p7SubmitResponseValid = false;
                p7SubmitTransportError = 0;

#if defined(RN_HAVE_SMU_FW)
                if (p73Gate.mayLoadPmfw &&
                    p73StagePrepared &&
                    p73StageMapped &&
                    p73StagePteVerified &&
                    navi33PmfwBuf &&
                    navi33PmfwBuf->getBytesNoCopy()) {
                    const uint32_t pmfwArgs[4] = {
                        static_cast<uint32_t>(p73StageGpuVa),
                        static_cast<uint32_t>(p73StageGpuVa >> 32),
                        static_cast<uint32_t>(kRNSmuFwSize),
                        kRNPspFwTypeSmu
                    };
                    uint32_t pmfwResp[4] = {};
                    p73FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa,
                        p71FenceGpuVa,
                        kRNPspCmdLoadIpFw, pmfwArgs, 4,
                        pmfwResp, 4);
                    p73Status = pmfwResp[0];
                    p73FwAddrLo = pmfwResp[2];
                    p73FwAddrHi = pmfwResp[3];
                    p73PmfwAccepted =
                        p73FenceOk && p73Status == 0;

                    // Read-only observation. Do not send an SMU mailbox
                    // message at Step 5.
                    uint32_t mp1Base = 0;
                    if (ipBase(kRNHwIdMp1, &mp1Base)) {
                        p73SmuRespAfter =
                            rd((mp1Base + 0x29au) * 4);
                    }
                }
#endif

                const char *const p73n[] = {
                    "step", "error", "requested", "psp_ready",
                    "toc_accepted", "tmr_ownership_verified",
                    "firmware_present", "metadata_verified",
                    "digest_trusted", "target_verified", "target_match",
                    "firmware_mp1_revision", "hardware_mp1_revision",
                    "firmware_version", "firmware_bytes",
                    "stage_prepared", "stage_mapped",
                    "stage_page_count", "stage_pte_verified",
                    "stage_bad_pte", "stage_mapping_error",
                    "stage_va_lo", "stage_va_hi",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final",
                    "fence_before", "fence_expected", "fence_observed",
                    "fence_polls", "fence_timeout_ms",
                    "fence_ok", "status", "fw_addr_lo", "fw_addr_hi",
                    "submitted", "response_valid", "transport_error",
                    "transport_quarantined",
                    "accepted", "smu_response_before",
                    "smu_response_after", "may_setup_tmr"
                };
                const uint32_t p73v[] = {
                    p7StepArg, static_cast<uint32_t>(p73Gate.error),
                    static_cast<uint32_t>(p73Input.requested),
                    static_cast<uint32_t>(p73Input.pspReady),
                    static_cast<uint32_t>(p73Input.tocAccepted),
                    static_cast<uint32_t>(
                        p73Input.tmrOwnershipVerified),
                    static_cast<uint32_t>(p73PmfwPresent),
                    static_cast<uint32_t>(
                        p73PmfwMetadataVerified),
                    static_cast<uint32_t>(
                        p73PmfwDigestTrusted),
                    static_cast<uint32_t>(p73PmfwTargetVerified),
                    static_cast<uint32_t>(p73PmfwTargetMatch),
                    p73PmfwTargetRevision, p73HardwareMp1Revision,
                    p73PmfwVersion, p73PmfwBytes,
                    static_cast<uint32_t>(p73StagePrepared),
                    static_cast<uint32_t>(p73StageMapped),
                    navi33PmfwPageCount,
                    static_cast<uint32_t>(p73StagePteVerified),
                    p73BadPte, p73MappingError,
                    static_cast<uint32_t>(p73StageGpuVa),
                    static_cast<uint32_t>(p73StageGpuVa >> 32),
                    p7SubmitWptrBefore, p7SubmitWptrAfter,
                    p7SubmitRptrBefore, p7SubmitRptrAfterKick,
                    p7SubmitRptrFinal,
                    p7SubmitFenceBefore,
                    p7SubmitFenceExpected, p7SubmitFenceObserved,
                    p7SubmitPolls,
                    rn_navi33::p7_pmfw::kFenceTimeoutMs,
                    static_cast<uint32_t>(p73FenceOk),
                    p73Status, p73FwAddrLo, p73FwAddrHi,
                    static_cast<uint32_t>(p7SubmitWasIssued),
                    static_cast<uint32_t>(p7SubmitResponseValid),
                    p7SubmitTransportError,
                    static_cast<uint32_t>(p7SubmitTransportQuarantined),
                    static_cast<uint32_t>(p73PmfwAccepted),
                    p73SmuRespBefore, p73SmuRespAfter,
                    0u
                };
                rnPublish(this, "navi33-p7-pmfw-runtime",
                          p73n, p73v,
                          sizeof(p73v) / sizeof(p73v[0]));

                rn_navi33::p7_postpsp::Input postInput{};
                postInput.pspAlive = p7SosAfter != 0;
                postInput.gartReady = p7GartReady;
                postInput.runtimeDmaOwner =
                    navi33PspRuntimePrepared;
                postInput.gartStart = gartPlan.gartStart;
                postInput.gartEnd = gartPlan.gartEnd;
                postInput.ringGpuVa = p71RingGpuVa;
                postInput.cmdGpuVa = p71CmdGpuVa;
                postInput.fenceGpuVa = p71FenceGpuVa;
                postInput.ringCreated = p71RingCreated;
                postInput.tocAccepted = p71TocAccepted;
                postInput.tmrBytes = p71TmrBytes;
                postInput.tmrOwnershipVerified =
                    p72Tmr.ownershipVerified;
                postInput.tmrMc = p72Tmr.mc;
                postInput.fbStart = stageGeometry.geometry.mcStart.value;
                postInput.fbBytes = stageGeometry.geometry.usableVramBytes;
                postInput.pmfwLoaded = p73PmfwAccepted;
                // P7.4: third GPCOM command, isolated from the validated
                // PMFW checkpoint. Requires both P7 step 6 and a second,
                // explicit hardware-write opt-in. Step 6 without that flag
                // publishes a read-only admission report.
                const auto p74Prereq =
                    rn_navi33::p7_postpsp::inspect(postInput);
                uint32_t p74OptInValue = 0;
                const bool p74OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p74_setup_tmr",
                        &p74OptInValue, sizeof(p74OptInValue)) &&
                    p74OptInValue == 1u;
                rn_navi33::p74_tmr::Input p74Input{};
                p74Input.requested = p7StepArg >= 6u;
                p74Input.explicitOptIn = p74OptIn;
                p74Input.identityVerified =
                    check.eligibleForComputeBringup();
                p74Input.pspReady =
                    p7PspOk && p71RingCreated && p71RuntimeMapped;
                p74Input.postPspMaySetupTmr =
                    p74Prereq.maySetupTmr;
                p74Input.pmfwAccepted = p73PmfwAccepted;
                p74Input.transportHealthy =
                    p73FenceOk && p7SubmitResponseValid &&
                    p7SubmitTransportError == 0u &&
                    !p7SubmitTransportQuarantined;
                p74Input.previousFenceExpected = p7SubmitFenceExpected;
                p74Input.previousFenceObserved = p7SubmitFenceObserved;
                p74Input.tmrOwnershipVerified =
                    p72Tmr.ownershipVerified;
                p74Input.allocatorQuiescent = carve.quiescent;
                p74Input.carveVerified = carve.carveVerified;
                p74Input.naturalAligned = p72Tmr.naturalAligned;
                p74Input.noOverlap = p72Tmr.noOverlap;
                p74Input.tocBytes = p71TmrBytes;
                p74Input.tmrBytes = p72Tmr.candidate.bytes;
                p74Input.tmrOffset = p72Tmr.candidate.offset;
                p74Input.mc = p72Tmr.mc;
                p74Input.pa = p72Tmr.pa;
                p74Input.mcStart = p72TmrInput.mcStart;
                p74Input.physBase = p72TmrInput.vramPhysBase;
                p74Input.visibleBytes = p72TmrInput.cpuVisibleBytes;
                p74Input.carveStart = carve.left.offset;
                p74Input.carveBytes = carve.left.bytes;
                const auto p74Gate =
                    rn_navi33::p74_tmr::inspect(p74Input);

                bool p74Submitted = false;
                bool p74ResponseValid = false;
                bool p74FenceOk = false;
                bool p74Accepted = false;
                uint32_t p74Status = 0xffffffffu;
                uint32_t p74TransportError = 0u;
                bool p74Quarantined = p7SubmitTransportQuarantined;
                uint32_t p74WptrBefore = 0u;
                uint32_t p74WptrAfter = 0u;
                uint32_t p74RptrBefore = 0u;
                uint32_t p74RptrFinal = 0u;
                uint32_t p74FenceBefore = 0u;
                uint32_t p74FenceExpected = 0u;
                uint32_t p74FenceObserved = 0u;
                uint32_t p74FencePolls = 0u;

                if (p74Gate.mayIssue &&
                    pspFence ==
                        rn_navi33::p74_tmr::kExpectedPriorFence &&
                    !p7SubmitTransportQuarantined) {
                    // PSP13 psp_prep_tmr_cmd_buf() from amdgpu_psp.c:
                    // GPU MC (not CPU BAR physical), size, bit 1 indicating
                    // physical address supplied, then GPU VRAM PA.
                    // The pure gate already verified each address domain
                    // and that the 19 MiB VRAM tail is owned/quiescent.
                    uint32_t p74Response[1] = {0xffffffffu};
                    p74FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p74_tmr::kCommandId,
                        p74Gate.words,
                        rn_navi33::p74_tmr::kArgCount,
                        p74Response, 1u);
                    p74Submitted = p7SubmitWasIssued;
                    p74ResponseValid = p7SubmitResponseValid;
                    p74Status = p74Response[0];
                    p74TransportError = p7SubmitTransportError;
                    p74Quarantined = p7SubmitTransportQuarantined;
                    p74WptrBefore = p7SubmitWptrBefore;
                    p74WptrAfter = p7SubmitWptrAfter;
                    p74RptrBefore = p7SubmitRptrBefore;
                    p74RptrFinal = p7SubmitRptrFinal;
                    p74FenceBefore = p7SubmitFenceBefore;
                    p74FenceExpected = p7SubmitFenceExpected;
                    p74FenceObserved = p7SubmitFenceObserved;
                    p74FencePolls = p7SubmitPolls;
                    p74Accepted =
                        p74FenceOk && p74ResponseValid &&
                        p74Status == 0u &&
                        p74FenceExpected == 3u &&
                        p74FenceObserved == 3u;
                } else if (p74Gate.mayIssue) {
                    // Fail closed: the ring was not freshly at fence 2.
                    // Do not write another PSP command buffer.
                    p74TransportError = 10u;
                }

                const char *const p74Names[] = {
                    "step", "requested", "explicit_opt_in", "gate_error",
                    "may_issue", "command_id", "tmr_size",
                    "tmr_mc_lo", "tmr_mc_hi", "tmr_pa_lo", "tmr_pa_hi",
                    "tmr_flags", "prior_fence_expected",
                    "prior_fence_observed", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_final",
                    "fence_before", "fence_expected", "fence_observed",
                    "fence_polls", "fence_ok", "response_valid",
                    "status", "transport_error", "transport_quarantined",
                    "accepted"
                };
                const uint32_t p74Values[] = {
                    p7StepArg, static_cast<uint32_t>(p74Input.requested),
                    static_cast<uint32_t>(p74OptIn),
                    static_cast<uint32_t>(p74Gate.error),
                    static_cast<uint32_t>(p74Gate.mayIssue),
                    rn_navi33::p74_tmr::kCommandId,
                    p74Gate.words[2], p74Gate.words[0],
                    p74Gate.words[1], p74Gate.words[4],
                    p74Gate.words[5], p74Gate.words[3],
                    p74Input.previousFenceExpected,
                    p74Input.previousFenceObserved,
                    static_cast<uint32_t>(p74Submitted),
                    p74WptrBefore, p74WptrAfter,
                    p74RptrBefore, p74RptrFinal,
                    p74FenceBefore, p74FenceExpected,
                    p74FenceObserved, p74FencePolls,
                    static_cast<uint32_t>(p74FenceOk),
                    static_cast<uint32_t>(p74ResponseValid),
                    p74Status, p74TransportError,
                    static_cast<uint32_t>(p74Quarantined),
                    static_cast<uint32_t>(p74Accepted)
                };
                rnPublish(this, "navi33-p7-tmr-runtime", p74Names, p74Values,
                          sizeof(p74Values) / sizeof(p74Values[0]));

                postInput.tmrAccepted = p74Accepted;
                const auto postTmrGate =
                    rn_navi33::p7_postpsp::inspect(postInput);

                // P7.5: single additional PSP LOAD_IP_FW command. The first
                // Navi33 SDMA6.0.2 Context Thread is loaded into TMR, but
                // NO SDMA/GC/MES hardware engine is started. This reuses
                // already-mapped PMFW stage only after completed fence 3.
                uint32_t p75OptInRaw = 0u;
                const bool p75OptIn =
                    PE_parse_boot_argn("rednavi_navi33_p75_sdma_th0",
                        &p75OptInRaw, sizeof(p75OptInRaw)) &&
                    p75OptInRaw == 1u;

                rn_navi33::p75_sdma::Input p75Input{};
                p75Input.requested = p7StepArg >= 7u;
                p75Input.explicitOptIn = p75OptIn;
                p75Input.identityVerified =
                    check.eligibleForComputeBringup();
                p75Input.observedSdmaMajor = check.observed.sdma.major;
                p75Input.observedSdmaMinor = check.observed.sdma.minor;
                p75Input.observedSdmaRevision = check.observed.sdma.revision;
                p75Input.pspRingReady =
                    p7PspOk && p71RingCreated && p71RuntimeMapped;
                p75Input.tmrAccepted =
                    p74Accepted && postTmrGate.mayLoadIpFirmware;
                p75Input.previousFenceExpected = p74FenceExpected;
                p75Input.previousFenceObserved = p74FenceObserved;
                p75Input.transportHealthy =
                    p74FenceOk && p74ResponseValid &&
                    p74TransportError == 0u &&
                    !p74Quarantined && !p7SubmitTransportQuarantined;
                p75Input.stageOwnerRetained =
                    navi33PmfwPrepared && navi33PmfwMapped &&
                    navi33PmfwBuf &&
                    navi33PmfwBuf->getBytesNoCopy();
                p75Input.stageMapped = p73StageMapped;
                p75Input.stagePteVerified = p73StagePteVerified;
                p75Input.stageBytes = navi33PmfwStageBytes;
                p75Input.stageGpuVa = p73StageGpuVa;
                p75Input.gartStart = gartPlan.gartStart;
                p75Input.gartEnd = gartPlan.gartEnd;

                uint32_t p75FwVersion = 0u;
#ifdef RN_HAVE_NAVI33_SDMA_FW
                p75Input.firmwarePresent = true;
#ifdef RN_NAVI33_SDMA_FW_METADATA_V1
                p75Input.firmwareMetadataVerified = true;
                p75Input.firmwareSourceVerified =
                    RN_NAVI33_SDMA_FW_SOURCE_VERIFIED == 1;
                p75Input.firmwareIpMajor = kRNNavi33SdmaIpMajor;
                p75Input.firmwareIpMinor = kRNNavi33SdmaIpMinor;
                p75Input.firmwareIpRevision = kRNNavi33SdmaIpRevision;
                p75Input.firmwareBytes = kRNNavi33SdmaCtxBytes;
                p75FwVersion = kRNNavi33SdmaVersion;
#endif
#endif
                const auto p75Gate =
                    rn_navi33::p75_sdma::inspect(p75Input);

                bool p75FenceOk = false;
                bool p75Submitted = false;
                bool p75ResponseValid = false;
                bool p75Accepted = false;
                uint32_t p75Status = 0xffffffffu;
                uint32_t p75FwAddrLo = 0u, p75FwAddrHi = 0u;
                uint32_t p75TransportError = 0u;
                bool p75Quarantined = p7SubmitTransportQuarantined;
                uint32_t p75WptrBefore = 0u, p75WptrAfter = 0u;
                uint32_t p75RptrBefore = 0u, p75RptrAfterKick = 0u;
                uint32_t p75RptrFinal = 0u;
                uint32_t p75FenceBefore = 0u, p75FenceExpected = 0u;
                uint32_t p75FenceObserved = 0u, p75FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_SDMA_FW
                if (p75Gate.mayLoad &&
                    pspFence == rn_navi33::p75_sdma::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33SdmaCtxFw) ==
                        static_cast<size_t>(kRNNavi33SdmaCtxBytes)) {
                    // No GART PTE mutation: only copy new firmware bytes
                    // into the retained CPU-visible host-DMA stage after
                    // SETUP_TMR fence 3 proved PSP has finished the last
                    // transaction. Preserve the backing until reboot.
                    uint8_t *const stage =
                        static_cast<uint8_t *>(
                            navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33SdmaCtxFw,
                           kRNNavi33SdmaCtxBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p75FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p75_sdma::kCommandId,
                        p75Gate.words,
                        rn_navi33::p75_sdma::kArgumentCount,
                        resp, 4u);
                    p75Submitted = p7SubmitWasIssued;
                    p75ResponseValid = p7SubmitResponseValid;
                    p75TransportError = p7SubmitTransportError;
                    p75Quarantined = p7SubmitTransportQuarantined;
                    p75WptrBefore = p7SubmitWptrBefore;
                    p75WptrAfter = p7SubmitWptrAfter;
                    p75RptrBefore = p7SubmitRptrBefore;
                    p75RptrAfterKick = p7SubmitRptrAfterKick;
                    p75RptrFinal = p7SubmitRptrFinal;
                    p75FenceBefore = p7SubmitFenceBefore;
                    p75FenceExpected = p7SubmitFenceExpected;
                    p75FenceObserved = p7SubmitFenceObserved;
                    p75FencePolls = p7SubmitPolls;
                    if (p75ResponseValid) {
                        p75Status = resp[0];
                        p75FwAddrLo = resp[2];
                        p75FwAddrHi = resp[3];
                    }
                    p75Accepted = p75FenceOk && p75ResponseValid &&
                        p75Status == 0u &&
                        p75FenceExpected ==
                            rn_navi33::p75_sdma::kExpectedFence &&
                        p75FenceObserved ==
                            rn_navi33::p75_sdma::kExpectedFence;
                } else if (p75Gate.mayLoad) {
                    // Fail closed on unexpected fence/shared-DMA state.
                    p75TransportError = 10u;
                }
#endif
                const char *const p75Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_ip_revision",
                    "firmware_ip_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "sdma_engine_started"
                };
                const uint32_t p75Values[] = {
                    p7StepArg, static_cast<uint32_t>(p75Gate.error),
                    static_cast<uint32_t>(p75Input.requested),
                    static_cast<uint32_t>(p75OptIn),
                    static_cast<uint32_t>(p75Gate.mayLoad),
                    static_cast<uint32_t>(p75Input.firmwarePresent),
                    static_cast<uint32_t>(p75Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p75Input.firmwareSourceVerified),
                    p75Input.observedSdmaRevision,
                    p75Input.firmwareIpRevision, p75FwVersion,
                    static_cast<uint32_t>(p75Input.firmwareBytes),
                    static_cast<uint32_t>(p75Input.stageBytes),
                    static_cast<uint32_t>(p75Input.stageMapped),
                    static_cast<uint32_t>(p75Input.stagePteVerified),
                    static_cast<uint32_t>(p75Input.stageGpuVa),
                    static_cast<uint32_t>(p75Input.stageGpuVa >> 32),
                    rn_navi33::p75_sdma::kCommandId,
                    rn_navi33::p75_sdma::kFirmwareType,
                    p75Input.previousFenceExpected,
                    p75Input.previousFenceObserved,
                    static_cast<uint32_t>(p75Submitted),
                    p75WptrBefore, p75WptrAfter,
                    p75RptrBefore, p75RptrAfterKick, p75RptrFinal,
                    p75FenceBefore, p75FenceExpected, p75FenceObserved,
                    p75FencePolls, static_cast<uint32_t>(p75FenceOk),
                    static_cast<uint32_t>(p75ResponseValid),
                    p75Status, p75FwAddrLo, p75FwAddrHi,
                    p75TransportError, static_cast<uint32_t>(p75Quarantined),
                    static_cast<uint32_t>(p75Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-sdma-runtime",
                          p75Names, p75Values,
                          sizeof(p75Values) / sizeof(p75Values[0]));
                // P7.6 SDMA6.0.2 Control Thread (TH1) PSP checkpoint.
                // Step 8 is observation-only unless a *new* independent
                // boot arg explicitly authorizes the fifth PSP command.
                // The prerequisite TH0 must have returned a valid fence 4,
                // PSP status 0, and matching Navi33 firmware source.
                uint32_t p76OptInRaw = 0u;
                const bool p76OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p76_sdma_th1",
                        &p76OptInRaw, sizeof(p76OptInRaw)) &&
                    p76OptInRaw == 1u;
                rn_navi33::p76_sdma::Input p76Input{};
                p76Input.requested = p7StepArg >= 8u;
                p76Input.explicitOptIn = p76OptIn;
                p76Input.identityVerified = p75Input.identityVerified;
                p76Input.observedSdmaMajor = p75Input.observedSdmaMajor;
                p76Input.observedSdmaMinor = p75Input.observedSdmaMinor;
                p76Input.observedSdmaRevision =
                    p75Input.observedSdmaRevision;
                p76Input.pspRingReady = p75Input.pspRingReady;
                p76Input.tmrAccepted = p74Accepted;
                p76Input.th0Accepted = p75Accepted;
                p76Input.previousFenceExpected = p75FenceExpected;
                p76Input.previousFenceObserved = p75FenceObserved;
                p76Input.transportHealthy =
                    p75FenceOk && p75ResponseValid &&
                    p75TransportError == 0u &&
                    !p75Quarantined && !p7SubmitTransportQuarantined;
                p76Input.stageOwnerRetained =
                    p75Input.stageOwnerRetained;
                p76Input.stageMapped = p75Input.stageMapped;
                p76Input.stagePteVerified =
                    p75Input.stagePteVerified;
                p76Input.stageBytes = p75Input.stageBytes;
                p76Input.stageGpuVa = p75Input.stageGpuVa;
                p76Input.gartStart = p75Input.gartStart;
                p76Input.gartEnd = p75Input.gartEnd;
                p76Input.firmwarePresent = p75Input.firmwarePresent;
                p76Input.firmwareMetadataVerified =
                    p75Input.firmwareMetadataVerified;
                p76Input.firmwareSourceVerified =
                    p75Input.firmwareSourceVerified;
                p76Input.firmwareIpMajor = p75Input.firmwareIpMajor;
                p76Input.firmwareIpMinor = p75Input.firmwareIpMinor;
                p76Input.firmwareIpRevision =
                    p75Input.firmwareIpRevision;
#if defined(RN_HAVE_NAVI33_SDMA_FW) && \
    defined(RN_NAVI33_SDMA_FW_METADATA_V1)
                p76Input.firmwareBytes = kRNNavi33SdmaCtlBytes;
#endif
                const auto p76Gate =
                    rn_navi33::p76_sdma::inspect(p76Input);

                bool p76FenceOk = false;
                bool p76Submitted = false;
                bool p76ResponseValid = false;
                bool p76Accepted = false;
                uint32_t p76Status = 0xffffffffu;
                uint32_t p76FwAddrLo = 0u, p76FwAddrHi = 0u;
                uint32_t p76TransportError = 0u;
                bool p76Quarantined = p7SubmitTransportQuarantined;
                uint32_t p76WptrBefore = 0u, p76WptrAfter = 0u;
                uint32_t p76RptrBefore = 0u, p76RptrAfterKick = 0u;
                uint32_t p76RptrFinal = 0u;
                uint32_t p76FenceBefore = 0u, p76FenceExpected = 0u;
                uint32_t p76FenceObserved = 0u, p76FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_SDMA_FW
                if (p76Gate.mayLoad &&
                    pspFence == rn_navi33::p76_sdma::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33SdmaCtlFw) ==
                        static_cast<size_t>(kRNNavi33SdmaCtlBytes)) {
                    // TH0 was already copied into PSP-owned TMR storage by
                    // the prior completed command. It is now safe to reuse
                    // this mapped staging page range for the TH1 bytes.
                    uint8_t *const stage =
                        static_cast<uint8_t *>(
                            navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33SdmaCtlFw,
                           kRNNavi33SdmaCtlBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p76FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p76_sdma::kCommandId,
                        p76Gate.words,
                        rn_navi33::p76_sdma::kArgumentCount,
                        resp, 4u);
                    p76Submitted = p7SubmitWasIssued;
                    p76ResponseValid = p7SubmitResponseValid;
                    p76TransportError = p7SubmitTransportError;
                    p76Quarantined = p7SubmitTransportQuarantined;
                    p76WptrBefore = p7SubmitWptrBefore;
                    p76WptrAfter = p7SubmitWptrAfter;
                    p76RptrBefore = p7SubmitRptrBefore;
                    p76RptrAfterKick = p7SubmitRptrAfterKick;
                    p76RptrFinal = p7SubmitRptrFinal;
                    p76FenceBefore = p7SubmitFenceBefore;
                    p76FenceExpected = p7SubmitFenceExpected;
                    p76FenceObserved = p7SubmitFenceObserved;
                    p76FencePolls = p7SubmitPolls;
                    if (p76ResponseValid) {
                        p76Status = resp[0];
                        p76FwAddrLo = resp[2];
                        p76FwAddrHi = resp[3];
                    }
                    p76Accepted = p76FenceOk && p76ResponseValid &&
                        p76Status == 0u &&
                        p76FenceExpected ==
                            rn_navi33::p76_sdma::kExpectedFence &&
                        p76FenceObserved ==
                            rn_navi33::p76_sdma::kExpectedFence;
                } else if (p76Gate.mayLoad) {
                    // Fail closed rather than touching the PSP ring with an
                    // unexpected prior fence or lost staging owner.
                    p76TransportError = 10u;
                }
#endif

                const char *const p76Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_ip_revision",
                    "firmware_ip_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "th0_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "sdma_firmware_both_loaded",
                    "sdma_engine_started"
                };
                const uint32_t p76Values[] = {
                    p7StepArg, static_cast<uint32_t>(p76Gate.error),
                    static_cast<uint32_t>(p76Input.requested),
                    static_cast<uint32_t>(p76OptIn),
                    static_cast<uint32_t>(p76Gate.mayLoad),
                    static_cast<uint32_t>(p76Input.firmwarePresent),
                    static_cast<uint32_t>(p76Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p76Input.firmwareSourceVerified),
                    p76Input.observedSdmaRevision,
                    p76Input.firmwareIpRevision, p75FwVersion,
                    static_cast<uint32_t>(p76Input.firmwareBytes),
                    static_cast<uint32_t>(p76Input.stageBytes),
                    static_cast<uint32_t>(p76Input.stageMapped),
                    static_cast<uint32_t>(p76Input.stagePteVerified),
                    static_cast<uint32_t>(p76Input.stageGpuVa),
                    static_cast<uint32_t>(p76Input.stageGpuVa >> 32),
                    rn_navi33::p76_sdma::kCommandId,
                    rn_navi33::p76_sdma::kFirmwareType,
                    p76Input.previousFenceExpected,
                    p76Input.previousFenceObserved,
                    static_cast<uint32_t>(p75Accepted),
                    static_cast<uint32_t>(p76Submitted),
                    p76WptrBefore, p76WptrAfter,
                    p76RptrBefore, p76RptrAfterKick, p76RptrFinal,
                    p76FenceBefore, p76FenceExpected, p76FenceObserved,
                    p76FencePolls, static_cast<uint32_t>(p76FenceOk),
                    static_cast<uint32_t>(p76ResponseValid),
                    p76Status, p76FwAddrLo, p76FwAddrHi,
                    p76TransportError, static_cast<uint32_t>(p76Quarantined),
                    static_cast<uint32_t>(p75Accepted && p76Accepted),
                    static_cast<uint32_t>(p75Accepted && p76Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-sdma-th1-runtime",
                          p76Names, p76Values,
                          sizeof(p76Values) / sizeof(p76Values[0]));

                // This flag means the two SDMA microcode threads were
                // accepted by PSP, NOT that SDMA rings or engines are live.
                postInput.sdmaFirmwareLoaded = p75Accepted && p76Accepted;
                // P7.7 GC11.0.2 RS64 PFP (instruction segment only).
                // This is the sixth independent PSP GPCOM checkpoint.
                // The previously verified SDMA TH0 + TH1 must both be
                // accepted at fence 5 before changing the shared DMA stage.
                // A separate boot arg is required even with Step 9.
                uint32_t p77OptInRaw = 0u;
                const bool p77OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p77_gc_pfp",
                        &p77OptInRaw, sizeof(p77OptInRaw)) &&
                    p77OptInRaw == 1u;
                rn_navi33::p77_gc_pfp::Input p77Input{};
                p77Input.requested = p7StepArg >= 9u;
                p77Input.explicitOptIn = p77OptIn;
                p77Input.identityVerified = p75Input.identityVerified;
                p77Input.observedGcMajor = check.observed.gc.major;
                p77Input.observedGcMinor = check.observed.gc.minor;
                p77Input.observedGcRevision = check.observed.gc.revision;
                p77Input.pspRingReady = p76Input.pspRingReady;
                p77Input.tmrAccepted = p74Accepted;
                p77Input.sdmaBothAccepted =
                    p75Accepted && p76Accepted;
                p77Input.previousFenceExpected = p76FenceExpected;
                p77Input.previousFenceObserved = p76FenceObserved;
                p77Input.transportHealthy =
                    p76FenceOk && p76ResponseValid &&
                    p76TransportError == 0u &&
                    !p76Quarantined && !p7SubmitTransportQuarantined;
                p77Input.stageOwnerRetained =
                    p76Input.stageOwnerRetained;
                p77Input.stageMapped = p76Input.stageMapped;
                p77Input.stagePteVerified = p76Input.stagePteVerified;
                p77Input.stageBytes = p76Input.stageBytes;
                p77Input.stageGpuVa = p76Input.stageGpuVa;
                p77Input.gartStart = p76Input.gartStart;
                p77Input.gartEnd = p76Input.gartEnd;

                uint32_t p77FwVersion = 0u;
#ifdef RN_HAVE_NAVI33_GC_PFP_FW
                p77Input.firmwarePresent = true;
#ifdef RN_NAVI33_GC_PFP_FW_METADATA_V1
                p77Input.firmwareMetadataVerified = true;
                p77Input.firmwareSourceVerified =
                    RN_NAVI33_GC_PFP_FW_SOURCE_VERIFIED == 1;
                p77Input.firmwareIpMajor = kRNNavi33GcPfpIpMajor;
                p77Input.firmwareIpMinor = kRNNavi33GcPfpIpMinor;
                p77Input.firmwareIpRevision = kRNNavi33GcPfpIpRevision;
                p77Input.firmwareBytes = kRNNavi33GcPfpCodeBytes;
                p77FwVersion = kRNNavi33GcPfpVersion;
#endif
#endif
                const auto p77Gate =
                    rn_navi33::p77_gc_pfp::inspect(p77Input);

                bool p77FenceOk = false;
                bool p77Submitted = false;
                bool p77ResponseValid = false;
                bool p77Accepted = false;
                uint32_t p77Status = 0xffffffffu;
                uint32_t p77FwAddrLo = 0u, p77FwAddrHi = 0u;
                uint32_t p77TransportError = 0u;
                bool p77Quarantined = p7SubmitTransportQuarantined;
                uint32_t p77WptrBefore = 0u, p77WptrAfter = 0u;
                uint32_t p77RptrBefore = 0u, p77RptrAfterKick = 0u;
                uint32_t p77RptrFinal = 0u;
                uint32_t p77FenceBefore = 0u, p77FenceExpected = 0u;
                uint32_t p77FenceObserved = 0u, p77FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_PFP_FW
                if (p77Gate.mayLoad &&
                    pspFence == rn_navi33::p77_gc_pfp::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcPfpCodeFw) ==
                        static_cast<size_t>(kRNNavi33GcPfpCodeBytes)) {
                    // Reuse the retained host-DMA PSP firmware stage only
                    // after SDMA TH1 has completed successfully at fence 5.
                    // No change to the GART page tables or VRAM ledger.
                    uint8_t *const stage =
                        static_cast<uint8_t *>(
                            navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcPfpCodeFw,
                           kRNNavi33GcPfpCodeBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p77FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p77_gc_pfp::kCommandId,
                        p77Gate.words,
                        rn_navi33::p77_gc_pfp::kArgumentCount,
                        resp, 4u);
                    p77Submitted = p7SubmitWasIssued;
                    p77ResponseValid = p7SubmitResponseValid;
                    p77TransportError = p7SubmitTransportError;
                    p77Quarantined = p7SubmitTransportQuarantined;
                    p77WptrBefore = p7SubmitWptrBefore;
                    p77WptrAfter = p7SubmitWptrAfter;
                    p77RptrBefore = p7SubmitRptrBefore;
                    p77RptrAfterKick = p7SubmitRptrAfterKick;
                    p77RptrFinal = p7SubmitRptrFinal;
                    p77FenceBefore = p7SubmitFenceBefore;
                    p77FenceExpected = p7SubmitFenceExpected;
                    p77FenceObserved = p7SubmitFenceObserved;
                    p77FencePolls = p7SubmitPolls;
                    if (p77ResponseValid) {
                        p77Status = resp[0];
                        p77FwAddrLo = resp[2];
                        p77FwAddrHi = resp[3];
                    }
                    p77Accepted = p77FenceOk && p77ResponseValid &&
                        p77Status == 0u &&
                        p77FenceExpected ==
                            rn_navi33::p77_gc_pfp::kExpectedFence &&
                        p77FenceObserved ==
                            rn_navi33::p77_gc_pfp::kExpectedFence;
                } else if (p77Gate.mayLoad) {
                    p77TransportError = 10u;
                }
#endif

                const char *const p77Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "sdma_both_accepted",
                    "submitted", "submit_wptr_before",
                    "submit_wptr_after", "submit_rptr_before",
                    "submit_rptr_after_kick", "submit_rptr_final",
                    "fence_before", "fence_expected", "fence_observed",
                    "fence_polls", "fence_ok", "response_valid",
                    "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_pfp_firmware_loaded",
                    "gc_engine_started"
                };
                const uint32_t p77Values[] = {
                    p7StepArg, static_cast<uint32_t>(p77Gate.error),
                    static_cast<uint32_t>(p77Input.requested),
                    static_cast<uint32_t>(p77OptIn),
                    static_cast<uint32_t>(p77Gate.mayLoad),
                    static_cast<uint32_t>(p77Input.firmwarePresent),
                    static_cast<uint32_t>(p77Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p77Input.firmwareSourceVerified),
                    p77Input.observedGcRevision,
                    p77Input.firmwareIpRevision, p77FwVersion,
                    static_cast<uint32_t>(p77Input.firmwareBytes),
                    static_cast<uint32_t>(p77Input.stageBytes),
                    static_cast<uint32_t>(p77Input.stageMapped),
                    static_cast<uint32_t>(p77Input.stagePteVerified),
                    static_cast<uint32_t>(p77Input.stageGpuVa),
                    static_cast<uint32_t>(p77Input.stageGpuVa >> 32),
                    rn_navi33::p77_gc_pfp::kCommandId,
                    rn_navi33::p77_gc_pfp::kFirmwareType,
                    p77Input.previousFenceExpected,
                    p77Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p77Submitted),
                    p77WptrBefore, p77WptrAfter,
                    p77RptrBefore, p77RptrAfterKick, p77RptrFinal,
                    p77FenceBefore, p77FenceExpected,
                    p77FenceObserved, p77FencePolls,
                    static_cast<uint32_t>(p77FenceOk),
                    static_cast<uint32_t>(p77ResponseValid),
                    p77Status, p77FwAddrLo, p77FwAddrHi,
                    p77TransportError, static_cast<uint32_t>(p77Quarantined),
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p77Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-pfp-runtime",
                          p77Names, p77Values,
                          sizeof(p77Values) / sizeof(p77Values[0]));

                // RS64 PFP instruction (fence=6) is a useful checkpoint,
                // but does not by itself complete GC firmware loading.
                // P7.8: RS64 PFP P0_STACK is the same GFX2.0 data block
                // that Linux later repeats as P1_STACK, with a different
                // PSP type. Load only P0 now, never run any graphics queue.
                // An explicit opt-in is required in addition to Step 10.
                uint32_t p78OptInRaw = 0u;
                const bool p78OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p78_pfp_p0",
                        &p78OptInRaw, sizeof(p78OptInRaw)) &&
                    p78OptInRaw == 1u;
                rn_navi33::p78_gc_pfp_p0::Input p78Input{};
                p78Input.requested = p7StepArg >= 10u;
                p78Input.explicitOptIn = p78OptIn;
                p78Input.identityVerified = p77Input.identityVerified;
                p78Input.observedGcMajor = p77Input.observedGcMajor;
                p78Input.observedGcMinor = p77Input.observedGcMinor;
                p78Input.observedGcRevision = p77Input.observedGcRevision;
                p78Input.pspRingReady = p77Input.pspRingReady;
                p78Input.tmrAccepted = p74Accepted;
                p78Input.sdmaBothAccepted = p77Input.sdmaBothAccepted;
                p78Input.pfpAccepted = p77Accepted;
                p78Input.previousFenceExpected = p77FenceExpected;
                p78Input.previousFenceObserved = p77FenceObserved;
                p78Input.transportHealthy =
                    p77FenceOk && p77ResponseValid &&
                    p77TransportError == 0u &&
                    !p77Quarantined && !p7SubmitTransportQuarantined;
                p78Input.stageOwnerRetained = p77Input.stageOwnerRetained;
                p78Input.stageMapped = p77Input.stageMapped;
                p78Input.stagePteVerified = p77Input.stagePteVerified;
                p78Input.stageBytes = p77Input.stageBytes;
                p78Input.stageGpuVa = p77Input.stageGpuVa;
                p78Input.gartStart = p77Input.gartStart;
                p78Input.gartEnd = p77Input.gartEnd;
                p78Input.firmwarePresent = p77Input.firmwarePresent;
                p78Input.firmwareMetadataVerified =
                    p77Input.firmwareMetadataVerified;
                p78Input.firmwareSourceVerified =
                    p77Input.firmwareSourceVerified;
                p78Input.firmwareIpMajor = p77Input.firmwareIpMajor;
                p78Input.firmwareIpMinor = p77Input.firmwareIpMinor;
                p78Input.firmwareIpRevision = p77Input.firmwareIpRevision;
#if defined(RN_HAVE_NAVI33_GC_PFP_FW) && \
    defined(RN_NAVI33_GC_PFP_FW_METADATA_V1)
                p78Input.firmwareBytes = kRNNavi33GcPfpDataBytes;
#endif
                const auto p78Gate =
                    rn_navi33::p78_gc_pfp_p0::inspect(p78Input);

                bool p78FenceOk = false;
                bool p78Submitted = false;
                bool p78ResponseValid = false;
                bool p78Accepted = false;
                uint32_t p78Status = 0xffffffffu;
                uint32_t p78FwAddrLo = 0u, p78FwAddrHi = 0u;
                uint32_t p78TransportError = 0u;
                bool p78Quarantined = p7SubmitTransportQuarantined;
                uint32_t p78WptrBefore = 0u, p78WptrAfter = 0u;
                uint32_t p78RptrBefore = 0u, p78RptrAfterKick = 0u;
                uint32_t p78RptrFinal = 0u;
                uint32_t p78FenceBefore = 0u, p78FenceExpected = 0u;
                uint32_t p78FenceObserved = 0u, p78FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_PFP_FW
                if (p78Gate.mayLoad &&
                    pspFence == rn_navi33::p78_gc_pfp_p0::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcPfpDataFw) ==
                        static_cast<size_t>(kRNNavi33GcPfpDataBytes)) {
                    // PFP code was accepted at fence=6; PSP has copied
                    // its data into TMR. Now reuse the retained DMA stage
                    // for the 139,264-byte RS64 PFP P0 stack.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcPfpDataFw,
                           kRNNavi33GcPfpDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p78FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p78_gc_pfp_p0::kCommandId,
                        p78Gate.words,
                        rn_navi33::p78_gc_pfp_p0::kArgumentCount,
                        resp, 4u);
                    p78Submitted = p7SubmitWasIssued;
                    p78ResponseValid = p7SubmitResponseValid;
                    p78TransportError = p7SubmitTransportError;
                    p78Quarantined = p7SubmitTransportQuarantined;
                    p78WptrBefore = p7SubmitWptrBefore;
                    p78WptrAfter = p7SubmitWptrAfter;
                    p78RptrBefore = p7SubmitRptrBefore;
                    p78RptrAfterKick = p7SubmitRptrAfterKick;
                    p78RptrFinal = p7SubmitRptrFinal;
                    p78FenceBefore = p7SubmitFenceBefore;
                    p78FenceExpected = p7SubmitFenceExpected;
                    p78FenceObserved = p7SubmitFenceObserved;
                    p78FencePolls = p7SubmitPolls;
                    if (p78ResponseValid) {
                        p78Status = resp[0];
                        p78FwAddrLo = resp[2];
                        p78FwAddrHi = resp[3];
                    }
                    p78Accepted = p78FenceOk && p78ResponseValid &&
                        p78Status == 0u &&
                        p78FenceExpected ==
                            rn_navi33::p78_gc_pfp_p0::kExpectedFence &&
                        p78FenceObserved ==
                            rn_navi33::p78_gc_pfp_p0::kExpectedFence;
                } else if (p78Gate.mayLoad) {
                    p78TransportError = 10u;
                }
#endif

                const char *const p78Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_pfp_p0_loaded",
                    "gc_engine_started"
                };
                const uint32_t p78Values[] = {
                    p7StepArg, static_cast<uint32_t>(p78Gate.error),
                    static_cast<uint32_t>(p78Input.requested),
                    static_cast<uint32_t>(p78OptIn),
                    static_cast<uint32_t>(p78Gate.mayLoad),
                    static_cast<uint32_t>(p78Input.firmwarePresent),
                    static_cast<uint32_t>(p78Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p78Input.firmwareSourceVerified),
                    p78Input.observedGcRevision,
                    p78Input.firmwareIpRevision, p77FwVersion,
                    static_cast<uint32_t>(p78Input.firmwareBytes),
                    static_cast<uint32_t>(p78Input.stageBytes),
                    static_cast<uint32_t>(p78Input.stageMapped),
                    static_cast<uint32_t>(p78Input.stagePteVerified),
                    static_cast<uint32_t>(p78Input.stageGpuVa),
                    static_cast<uint32_t>(p78Input.stageGpuVa >> 32),
                    rn_navi33::p78_gc_pfp_p0::kCommandId,
                    rn_navi33::p78_gc_pfp_p0::kFirmwareType,
                    p78Input.previousFenceExpected,
                    p78Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p78Submitted),
                    p78WptrBefore, p78WptrAfter,
                    p78RptrBefore, p78RptrAfterKick, p78RptrFinal,
                    p78FenceBefore, p78FenceExpected,
                    p78FenceObserved, p78FencePolls,
                    static_cast<uint32_t>(p78FenceOk),
                    static_cast<uint32_t>(p78ResponseValid),
                    p78Status, p78FwAddrLo, p78FwAddrHi,
                    p78TransportError, static_cast<uint32_t>(p78Quarantined),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-pfp-p0-runtime",
                          p78Names, p78Values,
                          sizeof(p78Values) / sizeof(p78Values[0]));
                // P7.9: GC11.0.2 RS64 PFP P1_STACK uses the same
                // GFX2.0 data slice as PSP-accepted P0_STACK,
                // but requires a separate type=91 PSP command at fence8.
                // No engines or graphics queues run at Step 11.
                // Independent explicit boot arg after seventh fence.
                uint32_t p79OptInRaw = 0u;
                const bool p79OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p79_pfp_p1",
                        &p79OptInRaw, sizeof(p79OptInRaw)) &&
                    p79OptInRaw == 1u;
                rn_navi33::p79_gc_pfp_p1::Input p79Input{};
                p79Input.requested = p7StepArg >= 11u;
                p79Input.explicitOptIn = p79OptIn;
                p79Input.identityVerified = p78Input.identityVerified;
                p79Input.observedGcMajor = p78Input.observedGcMajor;
                p79Input.observedGcMinor = p78Input.observedGcMinor;
                p79Input.observedGcRevision = p78Input.observedGcRevision;
                p79Input.pspRingReady = p78Input.pspRingReady;
                p79Input.tmrAccepted = p74Accepted;
                p79Input.sdmaBothAccepted = p78Input.sdmaBothAccepted;
                p79Input.pfpAccepted = p77Accepted;
                p79Input.p0Accepted = p78Accepted;
                p79Input.previousFenceExpected = p78FenceExpected;
                p79Input.previousFenceObserved = p78FenceObserved;
                p79Input.transportHealthy =
                    p78FenceOk && p78ResponseValid &&
                    p78TransportError == 0u &&
                    !p78Quarantined && !p7SubmitTransportQuarantined;
                p79Input.stageOwnerRetained = p78Input.stageOwnerRetained;
                p79Input.stageMapped = p78Input.stageMapped;
                p79Input.stagePteVerified = p78Input.stagePteVerified;
                p79Input.stageBytes = p78Input.stageBytes;
                p79Input.stageGpuVa = p78Input.stageGpuVa;
                p79Input.gartStart = p78Input.gartStart;
                p79Input.gartEnd = p78Input.gartEnd;
                p79Input.firmwarePresent = p78Input.firmwarePresent;
                p79Input.firmwareMetadataVerified =
                    p78Input.firmwareMetadataVerified;
                p79Input.firmwareSourceVerified =
                    p78Input.firmwareSourceVerified;
                p79Input.firmwareIpMajor = p78Input.firmwareIpMajor;
                p79Input.firmwareIpMinor = p78Input.firmwareIpMinor;
                p79Input.firmwareIpRevision = p78Input.firmwareIpRevision;
#if defined(RN_HAVE_NAVI33_GC_PFP_FW) && \
    defined(RN_NAVI33_GC_PFP_FW_METADATA_V1)
                p79Input.firmwareBytes = kRNNavi33GcPfpDataBytes;
#endif
                const auto p79Gate =
                    rn_navi33::p79_gc_pfp_p1::inspect(p79Input);

                bool p79FenceOk = false;
                bool p79Submitted = false;
                bool p79ResponseValid = false;
                bool p79Accepted = false;
                uint32_t p79Status = 0xffffffffu;
                uint32_t p79FwAddrLo = 0u, p79FwAddrHi = 0u;
                uint32_t p79TransportError = 0u;
                bool p79Quarantined = p7SubmitTransportQuarantined;
                uint32_t p79WptrBefore = 0u, p79WptrAfter = 0u;
                uint32_t p79RptrBefore = 0u, p79RptrAfterKick = 0u;
                uint32_t p79RptrFinal = 0u;
                uint32_t p79FenceBefore = 0u, p79FenceExpected = 0u;
                uint32_t p79FenceObserved = 0u, p79FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_PFP_FW
                if (p79Gate.mayLoad &&
                    pspFence == rn_navi33::p79_gc_pfp_p1::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcPfpDataFw) ==
                        static_cast<size_t>(kRNNavi33GcPfpDataBytes)) {
                    // PFP P0 Stack (type=90) was accepted at fence=7.
                    // PSP owns the prior firmware in TMR. Reuse retained
                    // DMA staging for the same 139264-byte P1 data
                    // with a separate PSP LOAD_IP_FW(type 91).
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcPfpDataFw,
                           kRNNavi33GcPfpDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p79FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p79_gc_pfp_p1::kCommandId,
                        p79Gate.words,
                        rn_navi33::p79_gc_pfp_p1::kArgumentCount,
                        resp, 4u);
                    p79Submitted = p7SubmitWasIssued;
                    p79ResponseValid = p7SubmitResponseValid;
                    p79TransportError = p7SubmitTransportError;
                    p79Quarantined = p7SubmitTransportQuarantined;
                    p79WptrBefore = p7SubmitWptrBefore;
                    p79WptrAfter = p7SubmitWptrAfter;
                    p79RptrBefore = p7SubmitRptrBefore;
                    p79RptrAfterKick = p7SubmitRptrAfterKick;
                    p79RptrFinal = p7SubmitRptrFinal;
                    p79FenceBefore = p7SubmitFenceBefore;
                    p79FenceExpected = p7SubmitFenceExpected;
                    p79FenceObserved = p7SubmitFenceObserved;
                    p79FencePolls = p7SubmitPolls;
                    if (p79ResponseValid) {
                        p79Status = resp[0];
                        p79FwAddrLo = resp[2];
                        p79FwAddrHi = resp[3];
                    }
                    p79Accepted = p79FenceOk && p79ResponseValid &&
                        p79Status == 0u &&
                        p79FenceExpected ==
                            rn_navi33::p79_gc_pfp_p1::kExpectedFence &&
                        p79FenceObserved ==
                            rn_navi33::p79_gc_pfp_p1::kExpectedFence;
                } else if (p79Gate.mayLoad) {
                    p79TransportError = 10u;
                }
#endif

                const char *const p79Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_pfp_p1_loaded",
                    "gc_engine_started"
                };
                const uint32_t p79Values[] = {
                    p7StepArg, static_cast<uint32_t>(p79Gate.error),
                    static_cast<uint32_t>(p79Input.requested),
                    static_cast<uint32_t>(p79OptIn),
                    static_cast<uint32_t>(p79Gate.mayLoad),
                    static_cast<uint32_t>(p79Input.firmwarePresent),
                    static_cast<uint32_t>(p79Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p79Input.firmwareSourceVerified),
                    p79Input.observedGcRevision,
                    p79Input.firmwareIpRevision, p77FwVersion,
                    static_cast<uint32_t>(p79Input.firmwareBytes),
                    static_cast<uint32_t>(p79Input.stageBytes),
                    static_cast<uint32_t>(p79Input.stageMapped),
                    static_cast<uint32_t>(p79Input.stagePteVerified),
                    static_cast<uint32_t>(p79Input.stageGpuVa),
                    static_cast<uint32_t>(p79Input.stageGpuVa >> 32),
                    rn_navi33::p79_gc_pfp_p1::kCommandId,
                    rn_navi33::p79_gc_pfp_p1::kFirmwareType,
                    p79Input.previousFenceExpected,
                    p79Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p79Submitted),
                    p79WptrBefore, p79WptrAfter,
                    p79RptrBefore, p79RptrAfterKick, p79RptrFinal,
                    p79FenceBefore, p79FenceExpected,
                    p79FenceObserved, p79FencePolls,
                    static_cast<uint32_t>(p79FenceOk),
                    static_cast<uint32_t>(p79ResponseValid),
                    p79Status, p79FwAddrLo, p79FwAddrHi,
                    p79TransportError, static_cast<uint32_t>(p79Quarantined),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-pfp-p1-runtime",
                          p79Names, p79Values,
                          sizeof(p79Values) / sizeof(p79Values[0]));
                // P7.10: GC11.0.2 RS64 ME *instruction* firmware.
                // Ninth PSP LOAD_IP_FW is independently gated at Step 12.
                // Prerequisites: PFP code, P0/P1 stacks, and fence=8.
                // Do not stage ME data/stack or start any GC/MES queue.
                uint32_t p710OptInRaw = 0u;
                const bool p710OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p710_gc_me",
                        &p710OptInRaw, sizeof(p710OptInRaw)) &&
                    p710OptInRaw == 1u;
                rn_navi33::p710_gc_me::Input p710Input{};
                p710Input.requested = p7StepArg >= 12u;
                p710Input.explicitOptIn = p710OptIn;
                p710Input.identityVerified = p79Input.identityVerified;
                p710Input.observedGcMajor = p79Input.observedGcMajor;
                p710Input.observedGcMinor = p79Input.observedGcMinor;
                p710Input.observedGcRevision = p79Input.observedGcRevision;
                p710Input.pspRingReady = p79Input.pspRingReady;
                p710Input.tmrAccepted = p74Accepted;
                p710Input.sdmaBothAccepted = p79Input.sdmaBothAccepted;
                p710Input.pfpAccepted = p77Accepted;
                p710Input.p0Accepted = p78Accepted;
                p710Input.p1Accepted = p79Accepted;
                p710Input.previousFenceExpected = p79FenceExpected;
                p710Input.previousFenceObserved = p79FenceObserved;
                p710Input.transportHealthy =
                    p79FenceOk && p79ResponseValid &&
                    p79TransportError == 0u &&
                    !p79Quarantined && !p7SubmitTransportQuarantined;
                p710Input.stageOwnerRetained = p79Input.stageOwnerRetained;
                p710Input.stageMapped = p79Input.stageMapped;
                p710Input.stagePteVerified = p79Input.stagePteVerified;
                p710Input.stageBytes = p79Input.stageBytes;
                p710Input.stageGpuVa = p79Input.stageGpuVa;
                p710Input.gartStart = p79Input.gartStart;
                p710Input.gartEnd = p79Input.gartEnd;
                // ME must be verified against its own pinned source,
                // never inherit PFP's source verification.
                uint32_t p710FwVersion = 0u;
#if defined(RN_HAVE_NAVI33_GC_ME_FW) && \
    defined(RN_NAVI33_GC_ME_FW_METADATA_V1)
                p710Input.firmwarePresent = true;
                p710Input.firmwareMetadataVerified = true;
                p710Input.firmwareSourceVerified =
                    RN_NAVI33_GC_ME_FW_SOURCE_VERIFIED == 1;
                p710Input.firmwareIpMajor = kRNNavi33GcMeIpMajor;
                p710Input.firmwareIpMinor = kRNNavi33GcMeIpMinor;
                p710Input.firmwareIpRevision = kRNNavi33GcMeIpRevision;
                p710Input.firmwareBytes = kRNNavi33GcMeCodeBytes;
                p710FwVersion = kRNNavi33GcMeVersion;
#endif
                const auto p710Gate =
                    rn_navi33::p710_gc_me::inspect(p710Input);

                bool p710FenceOk = false;
                bool p710Submitted = false;
                bool p710ResponseValid = false;
                bool p710Accepted = false;
                uint32_t p710Status = 0xffffffffu;
                uint32_t p710FwAddrLo = 0u, p710FwAddrHi = 0u;
                uint32_t p710TransportError = 0u;
                bool p710Quarantined = p7SubmitTransportQuarantined;
                uint32_t p710WptrBefore = 0u, p710WptrAfter = 0u;
                uint32_t p710RptrBefore = 0u, p710RptrAfterKick = 0u;
                uint32_t p710RptrFinal = 0u;
                uint32_t p710FenceBefore = 0u, p710FenceExpected = 0u;
                uint32_t p710FenceObserved = 0u, p710FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_ME_FW
                if (p710Gate.mayLoad &&
                    pspFence == rn_navi33::p710_gc_me::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcMeCodeFw) ==
                        static_cast<size_t>(kRNNavi33GcMeCodeBytes)) {
                    // PSP accepted PFP P1 Stack at fence=8.
                    // Copy only the verified RS64 ME instruction image
                    // into the retained host-DMA PSP staging buffer.
                    // PFP and SDMA firmware are now PSP-owned in TMR.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcMeCodeFw,
                           kRNNavi33GcMeCodeBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p710FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p710_gc_me::kCommandId,
                        p710Gate.words,
                        rn_navi33::p710_gc_me::kArgumentCount,
                        resp, 4u);
                    p710Submitted = p7SubmitWasIssued;
                    p710ResponseValid = p7SubmitResponseValid;
                    p710TransportError = p7SubmitTransportError;
                    p710Quarantined = p7SubmitTransportQuarantined;
                    p710WptrBefore = p7SubmitWptrBefore;
                    p710WptrAfter = p7SubmitWptrAfter;
                    p710RptrBefore = p7SubmitRptrBefore;
                    p710RptrAfterKick = p7SubmitRptrAfterKick;
                    p710RptrFinal = p7SubmitRptrFinal;
                    p710FenceBefore = p7SubmitFenceBefore;
                    p710FenceExpected = p7SubmitFenceExpected;
                    p710FenceObserved = p7SubmitFenceObserved;
                    p710FencePolls = p7SubmitPolls;
                    if (p710ResponseValid) {
                        p710Status = resp[0];
                        p710FwAddrLo = resp[2];
                        p710FwAddrHi = resp[3];
                    }
                    p710Accepted = p710FenceOk && p710ResponseValid &&
                        p710Status == 0u &&
                        p710FenceExpected ==
                            rn_navi33::p710_gc_me::kExpectedFence &&
                        p710FenceObserved ==
                            rn_navi33::p710_gc_me::kExpectedFence;
                } else if (p710Gate.mayLoad) {
                    p710TransportError = 10u;
                }
#endif

                const char *const p710Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted",
                    "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_me_firmware_loaded",
                    "gc_engine_started"
                };
                const uint32_t p710Values[] = {
                    p7StepArg, static_cast<uint32_t>(p710Gate.error),
                    static_cast<uint32_t>(p710Input.requested),
                    static_cast<uint32_t>(p710OptIn),
                    static_cast<uint32_t>(p710Gate.mayLoad),
                    static_cast<uint32_t>(p710Input.firmwarePresent),
                    static_cast<uint32_t>(p710Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p710Input.firmwareSourceVerified),
                    p710Input.observedGcRevision,
                    p710Input.firmwareIpRevision, p710FwVersion,
                    static_cast<uint32_t>(p710Input.firmwareBytes),
                    static_cast<uint32_t>(p710Input.stageBytes),
                    static_cast<uint32_t>(p710Input.stageMapped),
                    static_cast<uint32_t>(p710Input.stagePteVerified),
                    static_cast<uint32_t>(p710Input.stageGpuVa),
                    static_cast<uint32_t>(p710Input.stageGpuVa >> 32),
                    rn_navi33::p710_gc_me::kCommandId,
                    rn_navi33::p710_gc_me::kFirmwareType,
                    p710Input.previousFenceExpected,
                    p710Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p710Submitted),
                    p710WptrBefore, p710WptrAfter,
                    p710RptrBefore, p710RptrAfterKick, p710RptrFinal,
                    p710FenceBefore, p710FenceExpected,
                    p710FenceObserved, p710FencePolls,
                    static_cast<uint32_t>(p710FenceOk),
                    static_cast<uint32_t>(p710ResponseValid),
                    p710Status, p710FwAddrLo, p710FwAddrHi,
                    p710TransportError, static_cast<uint32_t>(p710Quarantined),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-me-runtime",
                          p710Names, p710Values,
                          sizeof(p710Values) / sizeof(p710Values[0]));
                // P7.11: GC11.0.2 RS64 ME P0_STACK data firmware.
                // Tenth PSP LOAD_IP_FW, type=92, follows confirmed
                // ME code acceptance at fence 9. P1, MEC, RLC, MES,
                // and all graphics engine/queue starts remain disabled.
                uint32_t p711OptInRaw = 0u;
                const bool p711OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p711_gc_me_p0",
                        &p711OptInRaw, sizeof(p711OptInRaw)) &&
                    p711OptInRaw == 1u;
                rn_navi33::p711_gc_me_p0::Input p711Input{};
                p711Input.requested = p7StepArg >= 13u;
                p711Input.explicitOptIn = p711OptIn;
                p711Input.identityVerified = p710Input.identityVerified;
                p711Input.observedGcMajor = p710Input.observedGcMajor;
                p711Input.observedGcMinor = p710Input.observedGcMinor;
                p711Input.observedGcRevision = p710Input.observedGcRevision;
                p711Input.pspRingReady = p710Input.pspRingReady;
                p711Input.tmrAccepted = p74Accepted;
                p711Input.sdmaBothAccepted = p710Input.sdmaBothAccepted;
                p711Input.pfpAccepted = p77Accepted;
                p711Input.p0Accepted = p78Accepted;
                p711Input.p1Accepted = p79Accepted;
                p711Input.meAccepted = p710Accepted;
                p711Input.previousFenceExpected = p710FenceExpected;
                p711Input.previousFenceObserved = p710FenceObserved;
                p711Input.transportHealthy =
                    p710FenceOk && p710ResponseValid &&
                    p710TransportError == 0u &&
                    !p710Quarantined && !p7SubmitTransportQuarantined;
                p711Input.stageOwnerRetained = p710Input.stageOwnerRetained;
                p711Input.stageMapped = p710Input.stageMapped;
                p711Input.stagePteVerified = p710Input.stagePteVerified;
                p711Input.stageBytes = p710Input.stageBytes;
                p711Input.stageGpuVa = p710Input.stageGpuVa;
                p711Input.gartStart = p710Input.gartStart;
                p711Input.gartEnd = p710Input.gartEnd;
                // This is the same reviewed, independently pinned ME
                // source as Step 12, but its GFX2.0 data region.
                p711Input.firmwarePresent = p710Input.firmwarePresent;
                p711Input.firmwareMetadataVerified =
                    p710Input.firmwareMetadataVerified;
                p711Input.firmwareSourceVerified =
                    p710Input.firmwareSourceVerified;
                p711Input.firmwareIpMajor = p710Input.firmwareIpMajor;
                p711Input.firmwareIpMinor = p710Input.firmwareIpMinor;
                p711Input.firmwareIpRevision = p710Input.firmwareIpRevision;
                uint32_t p711FwVersion = p710FwVersion;
#if defined(RN_HAVE_NAVI33_GC_ME_FW) && \
    defined(RN_NAVI33_GC_ME_FW_METADATA_V1)
                p711Input.firmwareBytes = kRNNavi33GcMeDataBytes;
#endif
                const auto p711Gate =
                    rn_navi33::p711_gc_me_p0::inspect(p711Input);

                bool p711FenceOk = false;
                bool p711Submitted = false;
                bool p711ResponseValid = false;
                bool p711Accepted = false;
                uint32_t p711Status = 0xffffffffu;
                uint32_t p711FwAddrLo = 0u, p711FwAddrHi = 0u;
                uint32_t p711TransportError = 0u;
                bool p711Quarantined = p7SubmitTransportQuarantined;
                uint32_t p711WptrBefore = 0u, p711WptrAfter = 0u;
                uint32_t p711RptrBefore = 0u, p711RptrAfterKick = 0u;
                uint32_t p711RptrFinal = 0u;
                uint32_t p711FenceBefore = 0u, p711FenceExpected = 0u;
                uint32_t p711FenceObserved = 0u, p711FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_ME_FW
                if (p711Gate.mayLoad &&
                    pspFence == rn_navi33::p711_gc_me_p0::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcMeDataFw) ==
                        static_cast<size_t>(kRNNavi33GcMeDataBytes)) {
                    // PSP accepted ME instruction (type=88) at fence9.
                    // Stage only pinned ME GFX2.0 Data region for P0
                    // in retained host-DMA buffer. No MMIO/queue start.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcMeDataFw,
                           kRNNavi33GcMeDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p711FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p711_gc_me_p0::kCommandId,
                        p711Gate.words,
                        rn_navi33::p711_gc_me_p0::kArgumentCount,
                        resp, 4u);
                    p711Submitted = p7SubmitWasIssued;
                    p711ResponseValid = p7SubmitResponseValid;
                    p711TransportError = p7SubmitTransportError;
                    p711Quarantined = p7SubmitTransportQuarantined;
                    p711WptrBefore = p7SubmitWptrBefore;
                    p711WptrAfter = p7SubmitWptrAfter;
                    p711RptrBefore = p7SubmitRptrBefore;
                    p711RptrAfterKick = p7SubmitRptrAfterKick;
                    p711RptrFinal = p7SubmitRptrFinal;
                    p711FenceBefore = p7SubmitFenceBefore;
                    p711FenceExpected = p7SubmitFenceExpected;
                    p711FenceObserved = p7SubmitFenceObserved;
                    p711FencePolls = p7SubmitPolls;
                    if (p711ResponseValid) {
                        p711Status = resp[0];
                        p711FwAddrLo = resp[2];
                        p711FwAddrHi = resp[3];
                    }
                    p711Accepted = p711FenceOk && p711ResponseValid &&
                        p711Status == 0u &&
                        p711FenceExpected ==
                            rn_navi33::p711_gc_me_p0::kExpectedFence &&
                        p711FenceObserved ==
                            rn_navi33::p711_gc_me_p0::kExpectedFence;
                } else if (p711Gate.mayLoad) {
                    p711TransportError = 10u;
                }
#endif

                const char *const p711Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_me_p0_loaded",
                    "gc_engine_started"
                };
                const uint32_t p711Values[] = {
                    p7StepArg, static_cast<uint32_t>(p711Gate.error),
                    static_cast<uint32_t>(p711Input.requested),
                    static_cast<uint32_t>(p711OptIn),
                    static_cast<uint32_t>(p711Gate.mayLoad),
                    static_cast<uint32_t>(p711Input.firmwarePresent),
                    static_cast<uint32_t>(p711Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p711Input.firmwareSourceVerified),
                    p711Input.observedGcRevision,
                    p711Input.firmwareIpRevision, p711FwVersion,
                    static_cast<uint32_t>(p711Input.firmwareBytes),
                    static_cast<uint32_t>(p711Input.stageBytes),
                    static_cast<uint32_t>(p711Input.stageMapped),
                    static_cast<uint32_t>(p711Input.stagePteVerified),
                    static_cast<uint32_t>(p711Input.stageGpuVa),
                    static_cast<uint32_t>(p711Input.stageGpuVa >> 32),
                    rn_navi33::p711_gc_me_p0::kCommandId,
                    rn_navi33::p711_gc_me_p0::kFirmwareType,
                    p711Input.previousFenceExpected,
                    p711Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p711Submitted),
                    p711WptrBefore, p711WptrAfter,
                    p711RptrBefore, p711RptrAfterKick, p711RptrFinal,
                    p711FenceBefore, p711FenceExpected,
                    p711FenceObserved, p711FencePolls,
                    static_cast<uint32_t>(p711FenceOk),
                    static_cast<uint32_t>(p711ResponseValid),
                    p711Status, p711FwAddrLo, p711FwAddrHi,
                    p711TransportError, static_cast<uint32_t>(p711Quarantined),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-me-p0-runtime",
                          p711Names, p711Values,
                          sizeof(p711Values) / sizeof(p711Values[0]));
                // P7.12: GC11.0.2 RS64 ME P1_STACK data firmware.
                // Independent eleventh PSP LOAD_IP_FW, type=93, only
                // after ME P0 accepted at PSP fence 10. Reuse precisely
                // the same pinned ME data segment, not ME code.
                // No MEC/RLC/MES or graphics engine/queue startup.
                uint32_t p712OptInRaw = 0u;
                const bool p712OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p712_gc_me_p1",
                        &p712OptInRaw, sizeof(p712OptInRaw)) &&
                    p712OptInRaw == 1u;
                rn_navi33::p712_gc_me_p1::Input p712Input{};
                p712Input.requested = p7StepArg >= 14u;
                p712Input.explicitOptIn = p712OptIn;
                p712Input.identityVerified = p711Input.identityVerified;
                p712Input.observedGcMajor = p711Input.observedGcMajor;
                p712Input.observedGcMinor = p711Input.observedGcMinor;
                p712Input.observedGcRevision = p711Input.observedGcRevision;
                p712Input.pspRingReady = p711Input.pspRingReady;
                p712Input.tmrAccepted = p74Accepted;
                p712Input.sdmaBothAccepted = p711Input.sdmaBothAccepted;
                p712Input.pfpAccepted = p77Accepted;
                p712Input.p0Accepted = p78Accepted;
                p712Input.p1Accepted = p79Accepted;
                p712Input.meAccepted = p710Accepted;
                p712Input.meP0Accepted = p711Accepted;
                p712Input.previousFenceExpected = p711FenceExpected;
                p712Input.previousFenceObserved = p711FenceObserved;
                p712Input.transportHealthy =
                    p711FenceOk && p711ResponseValid &&
                    p711TransportError == 0u &&
                    !p711Quarantined && !p7SubmitTransportQuarantined;
                p712Input.stageOwnerRetained = p711Input.stageOwnerRetained;
                p712Input.stageMapped = p711Input.stageMapped;
                p712Input.stagePteVerified = p711Input.stagePteVerified;
                p712Input.stageBytes = p711Input.stageBytes;
                p712Input.stageGpuVa = p711Input.stageGpuVa;
                p712Input.gartStart = p711Input.gartStart;
                p712Input.gartEnd = p711Input.gartEnd;
                // This is the same reviewed, independently pinned ME
                // source as Step 12, but its GFX2.0 data region.
                p712Input.firmwarePresent = p711Input.firmwarePresent;
                p712Input.firmwareMetadataVerified =
                    p711Input.firmwareMetadataVerified;
                p712Input.firmwareSourceVerified =
                    p711Input.firmwareSourceVerified;
                p712Input.firmwareIpMajor = p711Input.firmwareIpMajor;
                p712Input.firmwareIpMinor = p711Input.firmwareIpMinor;
                p712Input.firmwareIpRevision = p711Input.firmwareIpRevision;
                uint32_t p712FwVersion = p711FwVersion;
#if defined(RN_HAVE_NAVI33_GC_ME_FW) && \
    defined(RN_NAVI33_GC_ME_FW_METADATA_V1)
                p712Input.firmwareBytes = kRNNavi33GcMeDataBytes;
#endif
                const auto p712Gate =
                    rn_navi33::p712_gc_me_p1::inspect(p712Input);

                bool p712FenceOk = false;
                bool p712Submitted = false;
                bool p712ResponseValid = false;
                bool p712Accepted = false;
                uint32_t p712Status = 0xffffffffu;
                uint32_t p712FwAddrLo = 0u, p712FwAddrHi = 0u;
                uint32_t p712TransportError = 0u;
                bool p712Quarantined = p7SubmitTransportQuarantined;
                uint32_t p712WptrBefore = 0u, p712WptrAfter = 0u;
                uint32_t p712RptrBefore = 0u, p712RptrAfterKick = 0u;
                uint32_t p712RptrFinal = 0u;
                uint32_t p712FenceBefore = 0u, p712FenceExpected = 0u;
                uint32_t p712FenceObserved = 0u, p712FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_ME_FW
                if (p712Gate.mayLoad &&
                    pspFence == rn_navi33::p712_gc_me_p1::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcMeDataFw) ==
                        static_cast<size_t>(kRNNavi33GcMeDataBytes)) {
                    // PSP accepted ME P0 Stack (type=92) at fence10.
                    // PSP owns previous firmware in TMR. Reuse the same
                    // verified ME GFX2.0 data region for separate P1
                    // firmware type 93. No MMIO/queue start.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcMeDataFw,
                           kRNNavi33GcMeDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p712FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p712_gc_me_p1::kCommandId,
                        p712Gate.words,
                        rn_navi33::p712_gc_me_p1::kArgumentCount,
                        resp, 4u);
                    p712Submitted = p7SubmitWasIssued;
                    p712ResponseValid = p7SubmitResponseValid;
                    p712TransportError = p7SubmitTransportError;
                    p712Quarantined = p7SubmitTransportQuarantined;
                    p712WptrBefore = p7SubmitWptrBefore;
                    p712WptrAfter = p7SubmitWptrAfter;
                    p712RptrBefore = p7SubmitRptrBefore;
                    p712RptrAfterKick = p7SubmitRptrAfterKick;
                    p712RptrFinal = p7SubmitRptrFinal;
                    p712FenceBefore = p7SubmitFenceBefore;
                    p712FenceExpected = p7SubmitFenceExpected;
                    p712FenceObserved = p7SubmitFenceObserved;
                    p712FencePolls = p7SubmitPolls;
                    if (p712ResponseValid) {
                        p712Status = resp[0];
                        p712FwAddrLo = resp[2];
                        p712FwAddrHi = resp[3];
                    }
                    p712Accepted = p712FenceOk && p712ResponseValid &&
                        p712Status == 0u &&
                        p712FenceExpected ==
                            rn_navi33::p712_gc_me_p1::kExpectedFence &&
                        p712FenceObserved ==
                            rn_navi33::p712_gc_me_p1::kExpectedFence;
                } else if (p712Gate.mayLoad) {
                    p712TransportError = 10u;
                }
#endif

                const char *const p712Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_me_p1_loaded",
                    "gc_engine_started"
                };
                const uint32_t p712Values[] = {
                    p7StepArg, static_cast<uint32_t>(p712Gate.error),
                    static_cast<uint32_t>(p712Input.requested),
                    static_cast<uint32_t>(p712OptIn),
                    static_cast<uint32_t>(p712Gate.mayLoad),
                    static_cast<uint32_t>(p712Input.firmwarePresent),
                    static_cast<uint32_t>(p712Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p712Input.firmwareSourceVerified),
                    p712Input.observedGcRevision,
                    p712Input.firmwareIpRevision, p712FwVersion,
                    static_cast<uint32_t>(p712Input.firmwareBytes),
                    static_cast<uint32_t>(p712Input.stageBytes),
                    static_cast<uint32_t>(p712Input.stageMapped),
                    static_cast<uint32_t>(p712Input.stagePteVerified),
                    static_cast<uint32_t>(p712Input.stageGpuVa),
                    static_cast<uint32_t>(p712Input.stageGpuVa >> 32),
                    rn_navi33::p712_gc_me_p1::kCommandId,
                    rn_navi33::p712_gc_me_p1::kFirmwareType,
                    p712Input.previousFenceExpected,
                    p712Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p712Submitted),
                    p712WptrBefore, p712WptrAfter,
                    p712RptrBefore, p712RptrAfterKick, p712RptrFinal,
                    p712FenceBefore, p712FenceExpected,
                    p712FenceObserved, p712FencePolls,
                    static_cast<uint32_t>(p712FenceOk),
                    static_cast<uint32_t>(p712ResponseValid),
                    p712Status, p712FwAddrLo, p712FwAddrHi,
                    p712TransportError, static_cast<uint32_t>(p712Quarantined),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-me-p1-runtime",
                          p712Names, p712Values,
                          sizeof(p712Values) / sizeof(p712Values[0]));
                // P7.13: GC11.0.2 RS64 MEC instruction microcode.
                // Separate twelfth PSP LOAD_IP_FW, type=89, only after
                // ME P1 accepted at fence 11 AND stage expanded to
                // 66 mapped pages with explicit separate opt-in.
                // MEC data stacks, RLC, MES and queues remain off.
                uint32_t p713OptInRaw = 0u;
                const bool p713OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p713_gc_mec",
                        &p713OptInRaw, sizeof(p713OptInRaw)) &&
                    p713OptInRaw == 1u;
                rn_navi33::p713_gc_mec::Input p713Input{};
                p713Input.requested = p7StepArg >= 15u;
                p713Input.explicitOptIn = p713OptIn;
                p713Input.identityVerified = p712Input.identityVerified;
                p713Input.observedGcMajor = p712Input.observedGcMajor;
                p713Input.observedGcMinor = p712Input.observedGcMinor;
                p713Input.observedGcRevision = p712Input.observedGcRevision;
                p713Input.pspRingReady = p712Input.pspRingReady;
                p713Input.tmrAccepted = p74Accepted;
                p713Input.sdmaBothAccepted = p712Input.sdmaBothAccepted;
                p713Input.pfpAccepted = p77Accepted;
                p713Input.p0Accepted = p78Accepted;
                p713Input.p1Accepted = p79Accepted;
                p713Input.meAccepted = p710Accepted;
                p713Input.meP0Accepted = p711Accepted;
                p713Input.meP1Accepted = p712Accepted;
                p713Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p713Input.previousFenceExpected = p712FenceExpected;
                p713Input.previousFenceObserved = p712FenceObserved;
                p713Input.transportHealthy =
                    p712FenceOk && p712ResponseValid &&
                    p712TransportError == 0u &&
                    !p712Quarantined && !p7SubmitTransportQuarantined;
                p713Input.stageOwnerRetained = p712Input.stageOwnerRetained;
                p713Input.stageMapped = p712Input.stageMapped;
                p713Input.stagePteVerified = p712Input.stagePteVerified;
                p713Input.stageBytes = p712Input.stageBytes;
                p713Input.stageGpuVa = p712Input.stageGpuVa;
                p713Input.gartStart = p712Input.gartStart;
                p713Input.gartEnd = p712Input.gartEnd;
                // MEC is from its OWN pinned GC11.0.2 GFX2.0 source;
                // do not inherit ME source metadata and never stage
                // the entire full-source firmware image.
                uint32_t p713FwVersion = 0u;
#if defined(RN_HAVE_NAVI33_GC_MEC_FW) && \
    defined(RN_NAVI33_GC_MEC_FW_METADATA_V1)
                p713Input.firmwarePresent = true;
                p713Input.firmwareMetadataVerified = true;
                p713Input.firmwareSourceVerified =
                    p713StageInput.firmwareVerified;
                p713Input.firmwareIpMajor = kRNNavi33GcMecIpMajor;
                p713Input.firmwareIpMinor = kRNNavi33GcMecIpMinor;
                p713Input.firmwareIpRevision = kRNNavi33GcMecIpRevision;
                p713Input.firmwareBytes = kRNNavi33GcMecCodeBytes;
                p713FwVersion = kRNNavi33GcMecVersion;
#endif
                const auto p713Gate =
                    rn_navi33::p713_gc_mec::inspect(p713Input);

                bool p713FenceOk = false;
                bool p713Submitted = false;
                bool p713ResponseValid = false;
                bool p713Accepted = false;
                uint32_t p713Status = 0xffffffffu;
                uint32_t p713FwAddrLo = 0u, p713FwAddrHi = 0u;
                uint32_t p713TransportError = 0u;
                bool p713Quarantined = p7SubmitTransportQuarantined;
                uint32_t p713WptrBefore = 0u, p713WptrAfter = 0u;
                uint32_t p713RptrBefore = 0u, p713RptrAfterKick = 0u;
                uint32_t p713RptrFinal = 0u;
                uint32_t p713FenceBefore = 0u, p713FenceExpected = 0u;
                uint32_t p713FenceObserved = 0u, p713FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_MEC_FW
                if (p713Gate.mayLoad &&
                    pspFence == rn_navi33::p713_gc_mec::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcMecCodeFw) ==
                        static_cast<size_t>(kRNNavi33GcMecCodeBytes)) {
                    // PSP accepted ME P1 Stack at fence11. Stage only
                    // the pinned MEC Code (267008B) in an independently
                    // opted-in 66-page DMA buffer before issuing type89.
                    // PSP owns all prior firmware. No engine start.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcMecCodeFw,
                           kRNNavi33GcMecCodeBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p713FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p713_gc_mec::kCommandId,
                        p713Gate.words,
                        rn_navi33::p713_gc_mec::kArgumentCount,
                        resp, 4u);
                    p713Submitted = p7SubmitWasIssued;
                    p713ResponseValid = p7SubmitResponseValid;
                    p713TransportError = p7SubmitTransportError;
                    p713Quarantined = p7SubmitTransportQuarantined;
                    p713WptrBefore = p7SubmitWptrBefore;
                    p713WptrAfter = p7SubmitWptrAfter;
                    p713RptrBefore = p7SubmitRptrBefore;
                    p713RptrAfterKick = p7SubmitRptrAfterKick;
                    p713RptrFinal = p7SubmitRptrFinal;
                    p713FenceBefore = p7SubmitFenceBefore;
                    p713FenceExpected = p7SubmitFenceExpected;
                    p713FenceObserved = p7SubmitFenceObserved;
                    p713FencePolls = p7SubmitPolls;
                    if (p713ResponseValid) {
                        p713Status = resp[0];
                        p713FwAddrLo = resp[2];
                        p713FwAddrHi = resp[3];
                    }
                    p713Accepted = p713FenceOk && p713ResponseValid &&
                        p713Status == 0u &&
                        p713FenceExpected ==
                            rn_navi33::p713_gc_mec::kExpectedFence &&
                        p713FenceObserved ==
                            rn_navi33::p713_gc_mec::kExpectedFence;
                } else if (p713Gate.mayLoad) {
                    p713TransportError = 10u;
                }
#endif

                const char *const p713Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted",
                    "stage_expansion_verified", "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_mec_firmware_loaded",
                    "gc_engine_started"
                };
                const uint32_t p713Values[] = {
                    p7StepArg, static_cast<uint32_t>(p713Gate.error),
                    static_cast<uint32_t>(p713Input.requested),
                    static_cast<uint32_t>(p713OptIn),
                    static_cast<uint32_t>(p713Gate.mayLoad),
                    static_cast<uint32_t>(p713Input.firmwarePresent),
                    static_cast<uint32_t>(p713Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p713Input.firmwareSourceVerified),
                    p713Input.observedGcRevision,
                    p713Input.firmwareIpRevision, p713FwVersion,
                    static_cast<uint32_t>(p713Input.firmwareBytes),
                    static_cast<uint32_t>(p713Input.stageBytes),
                    static_cast<uint32_t>(p713Input.stageMapped),
                    static_cast<uint32_t>(p713Input.stagePteVerified),
                    static_cast<uint32_t>(p713Input.stageGpuVa),
                    static_cast<uint32_t>(p713Input.stageGpuVa >> 32),
                    rn_navi33::p713_gc_mec::kCommandId,
                    rn_navi33::p713_gc_mec::kFirmwareType,
                    p713Input.previousFenceExpected,
                    p713Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p713Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p713Submitted),
                    p713WptrBefore, p713WptrAfter,
                    p713RptrBefore, p713RptrAfterKick, p713RptrFinal,
                    p713FenceBefore, p713FenceExpected,
                    p713FenceObserved, p713FencePolls,
                    static_cast<uint32_t>(p713FenceOk),
                    static_cast<uint32_t>(p713ResponseValid),
                    p713Status, p713FwAddrLo, p713FwAddrHi,
                    p713TransportError, static_cast<uint32_t>(p713Quarantined),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-mec-runtime",
                          p713Names, p713Values,
                          sizeof(p713Values) / sizeof(p713Values[0]));
                // P7.14: GC11.0.2 RS64 MEC P0_STACK data microcode.
                // Linux uses the 139264-byte GFX v2.0 data segment
                // from the same pinned MEC image already accepted
                // as Code at fence12. This is the thirteenth PSP
                // LOAD_IP_FW(type=94); no MEC engine/queue startup.
                uint32_t p714OptInRaw = 0u;
                const bool p714OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p714_mec_p0",
                        &p714OptInRaw, sizeof(p714OptInRaw)) &&
                    p714OptInRaw == 1u;
                rn_navi33::p714_gc_mec_p0::Input p714Input{};
                p714Input.requested = p7StepArg >= 16u;
                p714Input.explicitOptIn = p714OptIn;
                p714Input.identityVerified = p713Input.identityVerified;
                p714Input.observedGcMajor = p713Input.observedGcMajor;
                p714Input.observedGcMinor = p713Input.observedGcMinor;
                p714Input.observedGcRevision = p713Input.observedGcRevision;
                p714Input.pspRingReady = p713Input.pspRingReady;
                p714Input.tmrAccepted = p74Accepted;
                p714Input.sdmaBothAccepted = p713Input.sdmaBothAccepted;
                p714Input.pfpAccepted = p77Accepted;
                p714Input.p0Accepted = p78Accepted;
                p714Input.p1Accepted = p79Accepted;
                p714Input.meAccepted = p710Accepted;
                p714Input.meP0Accepted = p711Accepted;
                p714Input.meP1Accepted = p712Accepted;
                p714Input.mecAccepted = p713Accepted;
                p714Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p714Input.previousFenceExpected = p713FenceExpected;
                p714Input.previousFenceObserved = p713FenceObserved;
                p714Input.transportHealthy =
                    p713FenceOk && p713ResponseValid &&
                    p713TransportError == 0u &&
                    !p713Quarantined && !p7SubmitTransportQuarantined;
                p714Input.stageOwnerRetained = p713Input.stageOwnerRetained;
                p714Input.stageMapped = p713Input.stageMapped;
                p714Input.stagePteVerified = p713Input.stagePteVerified;
                p714Input.stageBytes = p713Input.stageBytes;
                p714Input.stageGpuVa = p713Input.stageGpuVa;
                p714Input.gartStart = p713Input.gartStart;
                p714Input.gartEnd = p713Input.gartEnd;
                // Same hash-pinned, version 2590 MEC source already
                // independently reviewed by P7.13 and Linux GFX2.0,
                // but data is a separate PSP firmware type 94.
                p714Input.firmwarePresent = p713Input.firmwarePresent;
                p714Input.firmwareMetadataVerified =
                    p713Input.firmwareMetadataVerified;
                p714Input.firmwareSourceVerified =
                    p713Input.firmwareSourceVerified;
                p714Input.firmwareIpMajor = p713Input.firmwareIpMajor;
                p714Input.firmwareIpMinor = p713Input.firmwareIpMinor;
                p714Input.firmwareIpRevision = p713Input.firmwareIpRevision;
                uint32_t p714FwVersion = p713FwVersion;
#if defined(RN_HAVE_NAVI33_GC_MEC_FW) && \
    defined(RN_NAVI33_GC_MEC_FW_METADATA_V1)
                p714Input.firmwareBytes = kRNNavi33GcMecDataBytes;
#endif
                const auto p714Gate =
                    rn_navi33::p714_gc_mec_p0::inspect(p714Input);

                bool p714FenceOk = false;
                bool p714Submitted = false;
                bool p714ResponseValid = false;
                bool p714Accepted = false;
                uint32_t p714Status = 0xffffffffu;
                uint32_t p714FwAddrLo = 0u, p714FwAddrHi = 0u;
                uint32_t p714TransportError = 0u;
                bool p714Quarantined = p7SubmitTransportQuarantined;
                uint32_t p714WptrBefore = 0u, p714WptrAfter = 0u;
                uint32_t p714RptrBefore = 0u, p714RptrAfterKick = 0u;
                uint32_t p714RptrFinal = 0u;
                uint32_t p714FenceBefore = 0u, p714FenceExpected = 0u;
                uint32_t p714FenceObserved = 0u, p714FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_MEC_FW
                if (p714Gate.mayLoad &&
                    pspFence == rn_navi33::p714_gc_mec_p0::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcMecDataFw) ==
                        static_cast<size_t>(kRNNavi33GcMecDataBytes)) {
                    // PSP accepted MEC Code(type89) at fence12.
                    // Reuse the retained 66-page DMA stage and copy
                    // only the pinned MEC GFX2.0 Data segment
                    // (139264B) for P0_STACK(type94). No engines.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcMecDataFw,
                           kRNNavi33GcMecDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p714FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p714_gc_mec_p0::kCommandId,
                        p714Gate.words,
                        rn_navi33::p714_gc_mec_p0::kArgumentCount,
                        resp, 4u);
                    p714Submitted = p7SubmitWasIssued;
                    p714ResponseValid = p7SubmitResponseValid;
                    p714TransportError = p7SubmitTransportError;
                    p714Quarantined = p7SubmitTransportQuarantined;
                    p714WptrBefore = p7SubmitWptrBefore;
                    p714WptrAfter = p7SubmitWptrAfter;
                    p714RptrBefore = p7SubmitRptrBefore;
                    p714RptrAfterKick = p7SubmitRptrAfterKick;
                    p714RptrFinal = p7SubmitRptrFinal;
                    p714FenceBefore = p7SubmitFenceBefore;
                    p714FenceExpected = p7SubmitFenceExpected;
                    p714FenceObserved = p7SubmitFenceObserved;
                    p714FencePolls = p7SubmitPolls;
                    if (p714ResponseValid) {
                        p714Status = resp[0];
                        p714FwAddrLo = resp[2];
                        p714FwAddrHi = resp[3];
                    }
                    p714Accepted = p714FenceOk && p714ResponseValid &&
                        p714Status == 0u &&
                        p714FenceExpected ==
                            rn_navi33::p714_gc_mec_p0::kExpectedFence &&
                        p714FenceObserved ==
                            rn_navi33::p714_gc_mec_p0::kExpectedFence;
                } else if (p714Gate.mayLoad) {
                    p714TransportError = 10u;
                }
#endif

                const char *const p714Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "stage_expansion_verified", "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_mec_p0_loaded",
                    "gc_engine_started"
                };
                const uint32_t p714Values[] = {
                    p7StepArg, static_cast<uint32_t>(p714Gate.error),
                    static_cast<uint32_t>(p714Input.requested),
                    static_cast<uint32_t>(p714OptIn),
                    static_cast<uint32_t>(p714Gate.mayLoad),
                    static_cast<uint32_t>(p714Input.firmwarePresent),
                    static_cast<uint32_t>(p714Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p714Input.firmwareSourceVerified),
                    p714Input.observedGcRevision,
                    p714Input.firmwareIpRevision, p714FwVersion,
                    static_cast<uint32_t>(p714Input.firmwareBytes),
                    static_cast<uint32_t>(p714Input.stageBytes),
                    static_cast<uint32_t>(p714Input.stageMapped),
                    static_cast<uint32_t>(p714Input.stagePteVerified),
                    static_cast<uint32_t>(p714Input.stageGpuVa),
                    static_cast<uint32_t>(p714Input.stageGpuVa >> 32),
                    rn_navi33::p714_gc_mec_p0::kCommandId,
                    rn_navi33::p714_gc_mec_p0::kFirmwareType,
                    p714Input.previousFenceExpected,
                    p714Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p714Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p714Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p714Submitted),
                    p714WptrBefore, p714WptrAfter,
                    p714RptrBefore, p714RptrAfterKick, p714RptrFinal,
                    p714FenceBefore, p714FenceExpected,
                    p714FenceObserved, p714FencePolls,
                    static_cast<uint32_t>(p714FenceOk),
                    static_cast<uint32_t>(p714ResponseValid),
                    p714Status, p714FwAddrLo, p714FwAddrHi,
                    p714TransportError, static_cast<uint32_t>(p714Quarantined),
                    static_cast<uint32_t>(p714Accepted),
                    static_cast<uint32_t>(p714Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-mec-p0-runtime",
                          p714Names, p714Values,
                          sizeof(p714Values) / sizeof(p714Values[0]));
                // P7.15: GC11.0.2 RS64 MEC P1_STACK data microcode.
                // Linux uses the SAME 139264B pinned MEC GFX v2.0 data
                // segment for P0 and P1. Different PSP types (94 vs 95).
                // This fourteenth PSP LOAD_IP_FW(type95) requires prior
                // P0 accepted at fence13. No engine/queue startup.
                uint32_t p715OptInRaw = 0u;
                const bool p715OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p715_mec_p1",
                        &p715OptInRaw, sizeof(p715OptInRaw)) &&
                    p715OptInRaw == 1u;
                rn_navi33::p715_gc_mec_p1::Input p715Input{};
                p715Input.requested = p7StepArg >= 17u;
                p715Input.explicitOptIn = p715OptIn;
                p715Input.identityVerified = p714Input.identityVerified;
                p715Input.observedGcMajor = p714Input.observedGcMajor;
                p715Input.observedGcMinor = p714Input.observedGcMinor;
                p715Input.observedGcRevision = p714Input.observedGcRevision;
                p715Input.pspRingReady = p714Input.pspRingReady;
                p715Input.tmrAccepted = p74Accepted;
                p715Input.sdmaBothAccepted = p714Input.sdmaBothAccepted;
                p715Input.pfpAccepted = p77Accepted;
                p715Input.p0Accepted = p78Accepted;
                p715Input.p1Accepted = p79Accepted;
                p715Input.meAccepted = p710Accepted;
                p715Input.meP0Accepted = p711Accepted;
                p715Input.meP1Accepted = p712Accepted;
                p715Input.mecAccepted = p713Accepted;
                p715Input.mecP0Accepted = p714Accepted;
                p715Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p715Input.previousFenceExpected = p714FenceExpected;
                p715Input.previousFenceObserved = p714FenceObserved;
                p715Input.transportHealthy =
                    p714FenceOk && p714ResponseValid &&
                    p714TransportError == 0u &&
                    !p714Quarantined && !p7SubmitTransportQuarantined;
                p715Input.stageOwnerRetained = p714Input.stageOwnerRetained;
                p715Input.stageMapped = p714Input.stageMapped;
                p715Input.stagePteVerified = p714Input.stagePteVerified;
                p715Input.stageBytes = p714Input.stageBytes;
                p715Input.stageGpuVa = p714Input.stageGpuVa;
                p715Input.gartStart = p714Input.gartStart;
                p715Input.gartEnd = p714Input.gartEnd;
                // Same independently reviewed and pinned version 2590
                // MEC Data source as P7.14; P1 requires a distinct PSP
                // firmware type 95 and cannot inherit P0 acceptance.
                p715Input.firmwarePresent = p714Input.firmwarePresent;
                p715Input.firmwareMetadataVerified =
                    p714Input.firmwareMetadataVerified;
                p715Input.firmwareSourceVerified =
                    p714Input.firmwareSourceVerified;
                p715Input.firmwareIpMajor = p714Input.firmwareIpMajor;
                p715Input.firmwareIpMinor = p714Input.firmwareIpMinor;
                p715Input.firmwareIpRevision = p714Input.firmwareIpRevision;
                uint32_t p715FwVersion = p714FwVersion;
#if defined(RN_HAVE_NAVI33_GC_MEC_FW) && \
    defined(RN_NAVI33_GC_MEC_FW_METADATA_V1)
                p715Input.firmwareBytes = kRNNavi33GcMecDataBytes;
#endif
                const auto p715Gate =
                    rn_navi33::p715_gc_mec_p1::inspect(p715Input);

                bool p715FenceOk = false;
                bool p715Submitted = false;
                bool p715ResponseValid = false;
                bool p715Accepted = false;
                uint32_t p715Status = 0xffffffffu;
                uint32_t p715FwAddrLo = 0u, p715FwAddrHi = 0u;
                uint32_t p715TransportError = 0u;
                bool p715Quarantined = p7SubmitTransportQuarantined;
                uint32_t p715WptrBefore = 0u, p715WptrAfter = 0u;
                uint32_t p715RptrBefore = 0u, p715RptrAfterKick = 0u;
                uint32_t p715RptrFinal = 0u;
                uint32_t p715FenceBefore = 0u, p715FenceExpected = 0u;
                uint32_t p715FenceObserved = 0u, p715FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_MEC_FW
                if (p715Gate.mayLoad &&
                    pspFence == rn_navi33::p715_gc_mec_p1::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcMecDataFw) ==
                        static_cast<size_t>(kRNNavi33GcMecDataBytes)) {
                    // PSP accepted MEC P0 Stack(type94) at fence13.
                    // Reuse the retained 66-page DMA stage and copy
                    // only the same verified MEC GFX2.0 Data segment
                    // (139264B) for P1_STACK(type95). No engines.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcMecDataFw,
                           kRNNavi33GcMecDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p715FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p715_gc_mec_p1::kCommandId,
                        p715Gate.words,
                        rn_navi33::p715_gc_mec_p1::kArgumentCount,
                        resp, 4u);
                    p715Submitted = p7SubmitWasIssued;
                    p715ResponseValid = p7SubmitResponseValid;
                    p715TransportError = p7SubmitTransportError;
                    p715Quarantined = p7SubmitTransportQuarantined;
                    p715WptrBefore = p7SubmitWptrBefore;
                    p715WptrAfter = p7SubmitWptrAfter;
                    p715RptrBefore = p7SubmitRptrBefore;
                    p715RptrAfterKick = p7SubmitRptrAfterKick;
                    p715RptrFinal = p7SubmitRptrFinal;
                    p715FenceBefore = p7SubmitFenceBefore;
                    p715FenceExpected = p7SubmitFenceExpected;
                    p715FenceObserved = p7SubmitFenceObserved;
                    p715FencePolls = p7SubmitPolls;
                    if (p715ResponseValid) {
                        p715Status = resp[0];
                        p715FwAddrLo = resp[2];
                        p715FwAddrHi = resp[3];
                    }
                    p715Accepted = p715FenceOk && p715ResponseValid &&
                        p715Status == 0u &&
                        p715FenceExpected ==
                            rn_navi33::p715_gc_mec_p1::kExpectedFence &&
                        p715FenceObserved ==
                            rn_navi33::p715_gc_mec_p1::kExpectedFence;
                } else if (p715Gate.mayLoad) {
                    p715TransportError = 10u;
                }
#endif

                const char *const p715Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "mec_p0_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_mec_p1_loaded",
                    "gc_engine_started"
                };
                const uint32_t p715Values[] = {
                    p7StepArg, static_cast<uint32_t>(p715Gate.error),
                    static_cast<uint32_t>(p715Input.requested),
                    static_cast<uint32_t>(p715OptIn),
                    static_cast<uint32_t>(p715Gate.mayLoad),
                    static_cast<uint32_t>(p715Input.firmwarePresent),
                    static_cast<uint32_t>(p715Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p715Input.firmwareSourceVerified),
                    p715Input.observedGcRevision,
                    p715Input.firmwareIpRevision, p715FwVersion,
                    static_cast<uint32_t>(p715Input.firmwareBytes),
                    static_cast<uint32_t>(p715Input.stageBytes),
                    static_cast<uint32_t>(p715Input.stageMapped),
                    static_cast<uint32_t>(p715Input.stagePteVerified),
                    static_cast<uint32_t>(p715Input.stageGpuVa),
                    static_cast<uint32_t>(p715Input.stageGpuVa >> 32),
                    rn_navi33::p715_gc_mec_p1::kCommandId,
                    rn_navi33::p715_gc_mec_p1::kFirmwareType,
                    p715Input.previousFenceExpected,
                    p715Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p714Accepted),
                    static_cast<uint32_t>(p715Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p715Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p715Submitted),
                    p715WptrBefore, p715WptrAfter,
                    p715RptrBefore, p715RptrAfterKick, p715RptrFinal,
                    p715FenceBefore, p715FenceExpected,
                    p715FenceObserved, p715FencePolls,
                    static_cast<uint32_t>(p715FenceOk),
                    static_cast<uint32_t>(p715ResponseValid),
                    p715Status, p715FwAddrLo, p715FwAddrHi,
                    p715TransportError, static_cast<uint32_t>(p715Quarantined),
                    static_cast<uint32_t>(p715Accepted),
                    static_cast<uint32_t>(p715Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-mec-p1-runtime",
                          p715Names, p715Values,
                          sizeof(p715Values) / sizeof(p715Values[0]));
                // P7.16: GC11.0.2 RS64 MEC P2_STACK data microcode.
                // Linux uses the SAME 139264B pinned MEC GFX v2.0 data
                // segment for P1 and P2. Distinct PSP types (95 vs 96).
                // This fifteenth PSP LOAD_IP_FW(type96) requires prior
                // P1 accepted at fence14. No engine/queue startup.
                uint32_t p716OptInRaw = 0u;
                const bool p716OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p716_mec_p2",
                        &p716OptInRaw, sizeof(p716OptInRaw)) &&
                    p716OptInRaw == 1u;
                rn_navi33::p716_gc_mec_p2::Input p716Input{};
                p716Input.requested = p7StepArg >= 18u;
                p716Input.explicitOptIn = p716OptIn;
                p716Input.identityVerified = p715Input.identityVerified;
                p716Input.observedGcMajor = p715Input.observedGcMajor;
                p716Input.observedGcMinor = p715Input.observedGcMinor;
                p716Input.observedGcRevision = p715Input.observedGcRevision;
                p716Input.pspRingReady = p715Input.pspRingReady;
                p716Input.tmrAccepted = p74Accepted;
                p716Input.sdmaBothAccepted = p715Input.sdmaBothAccepted;
                p716Input.pfpAccepted = p77Accepted;
                p716Input.p0Accepted = p78Accepted;
                p716Input.p1Accepted = p79Accepted;
                p716Input.meAccepted = p710Accepted;
                p716Input.meP0Accepted = p711Accepted;
                p716Input.meP1Accepted = p712Accepted;
                p716Input.mecAccepted = p713Accepted;
                p716Input.mecP1Accepted = p715Accepted;
                p716Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p716Input.previousFenceExpected = p715FenceExpected;
                p716Input.previousFenceObserved = p715FenceObserved;
                p716Input.transportHealthy =
                    p715FenceOk && p715ResponseValid &&
                    p715TransportError == 0u &&
                    !p715Quarantined && !p7SubmitTransportQuarantined;
                p716Input.stageOwnerRetained = p715Input.stageOwnerRetained;
                p716Input.stageMapped = p715Input.stageMapped;
                p716Input.stagePteVerified = p715Input.stagePteVerified;
                p716Input.stageBytes = p715Input.stageBytes;
                p716Input.stageGpuVa = p715Input.stageGpuVa;
                p716Input.gartStart = p715Input.gartStart;
                p716Input.gartEnd = p715Input.gartEnd;
                // Same independently reviewed and pinned version 2590
                // MEC Data source as P7.14; P1 requires a distinct PSP
                // firmware type 96 and cannot inherit P0 acceptance.
                p716Input.firmwarePresent = p715Input.firmwarePresent;
                p716Input.firmwareMetadataVerified =
                    p715Input.firmwareMetadataVerified;
                p716Input.firmwareSourceVerified =
                    p715Input.firmwareSourceVerified;
                p716Input.firmwareIpMajor = p715Input.firmwareIpMajor;
                p716Input.firmwareIpMinor = p715Input.firmwareIpMinor;
                p716Input.firmwareIpRevision = p715Input.firmwareIpRevision;
                uint32_t p716FwVersion = p715FwVersion;
#if defined(RN_HAVE_NAVI33_GC_MEC_FW) && \
    defined(RN_NAVI33_GC_MEC_FW_METADATA_V1)
                p716Input.firmwareBytes = kRNNavi33GcMecDataBytes;
#endif
                const auto p716Gate =
                    rn_navi33::p716_gc_mec_p2::inspect(p716Input);

                bool p716FenceOk = false;
                bool p716Submitted = false;
                bool p716ResponseValid = false;
                bool p716Accepted = false;
                uint32_t p716Status = 0xffffffffu;
                uint32_t p716FwAddrLo = 0u, p716FwAddrHi = 0u;
                uint32_t p716TransportError = 0u;
                bool p716Quarantined = p7SubmitTransportQuarantined;
                uint32_t p716WptrBefore = 0u, p716WptrAfter = 0u;
                uint32_t p716RptrBefore = 0u, p716RptrAfterKick = 0u;
                uint32_t p716RptrFinal = 0u;
                uint32_t p716FenceBefore = 0u, p716FenceExpected = 0u;
                uint32_t p716FenceObserved = 0u, p716FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_MEC_FW
                if (p716Gate.mayLoad &&
                    pspFence == rn_navi33::p716_gc_mec_p2::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcMecDataFw) ==
                        static_cast<size_t>(kRNNavi33GcMecDataBytes)) {
                    // PSP accepted MEC P1 Stack(type95) at fence14.
                    // Reuse the retained 66-page DMA stage and copy
                    // only the same verified MEC GFX2.0 Data segment
                    // (139264B) for P2_STACK(type96). No engines.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcMecDataFw,
                           kRNNavi33GcMecDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p716FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p716_gc_mec_p2::kCommandId,
                        p716Gate.words,
                        rn_navi33::p716_gc_mec_p2::kArgumentCount,
                        resp, 4u);
                    p716Submitted = p7SubmitWasIssued;
                    p716ResponseValid = p7SubmitResponseValid;
                    p716TransportError = p7SubmitTransportError;
                    p716Quarantined = p7SubmitTransportQuarantined;
                    p716WptrBefore = p7SubmitWptrBefore;
                    p716WptrAfter = p7SubmitWptrAfter;
                    p716RptrBefore = p7SubmitRptrBefore;
                    p716RptrAfterKick = p7SubmitRptrAfterKick;
                    p716RptrFinal = p7SubmitRptrFinal;
                    p716FenceBefore = p7SubmitFenceBefore;
                    p716FenceExpected = p7SubmitFenceExpected;
                    p716FenceObserved = p7SubmitFenceObserved;
                    p716FencePolls = p7SubmitPolls;
                    if (p716ResponseValid) {
                        p716Status = resp[0];
                        p716FwAddrLo = resp[2];
                        p716FwAddrHi = resp[3];
                    }
                    p716Accepted = p716FenceOk && p716ResponseValid &&
                        p716Status == 0u &&
                        p716FenceExpected ==
                            rn_navi33::p716_gc_mec_p2::kExpectedFence &&
                        p716FenceObserved ==
                            rn_navi33::p716_gc_mec_p2::kExpectedFence;
                } else if (p716Gate.mayLoad) {
                    p716TransportError = 10u;
                }
#endif

                const char *const p716Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "mec_p1_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_mec_p2_loaded",
                    "gc_engine_started"
                };
                const uint32_t p716Values[] = {
                    p7StepArg, static_cast<uint32_t>(p716Gate.error),
                    static_cast<uint32_t>(p716Input.requested),
                    static_cast<uint32_t>(p716OptIn),
                    static_cast<uint32_t>(p716Gate.mayLoad),
                    static_cast<uint32_t>(p716Input.firmwarePresent),
                    static_cast<uint32_t>(p716Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p716Input.firmwareSourceVerified),
                    p716Input.observedGcRevision,
                    p716Input.firmwareIpRevision, p716FwVersion,
                    static_cast<uint32_t>(p716Input.firmwareBytes),
                    static_cast<uint32_t>(p716Input.stageBytes),
                    static_cast<uint32_t>(p716Input.stageMapped),
                    static_cast<uint32_t>(p716Input.stagePteVerified),
                    static_cast<uint32_t>(p716Input.stageGpuVa),
                    static_cast<uint32_t>(p716Input.stageGpuVa >> 32),
                    rn_navi33::p716_gc_mec_p2::kCommandId,
                    rn_navi33::p716_gc_mec_p2::kFirmwareType,
                    p716Input.previousFenceExpected,
                    p716Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p715Accepted),
                    static_cast<uint32_t>(p716Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p716Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p716Submitted),
                    p716WptrBefore, p716WptrAfter,
                    p716RptrBefore, p716RptrAfterKick, p716RptrFinal,
                    p716FenceBefore, p716FenceExpected,
                    p716FenceObserved, p716FencePolls,
                    static_cast<uint32_t>(p716FenceOk),
                    static_cast<uint32_t>(p716ResponseValid),
                    p716Status, p716FwAddrLo, p716FwAddrHi,
                    p716TransportError, static_cast<uint32_t>(p716Quarantined),
                    static_cast<uint32_t>(p716Accepted),
                    static_cast<uint32_t>(p716Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-mec-p2-runtime",
                          p716Names, p716Values,
                          sizeof(p716Values) / sizeof(p716Values[0]));
                // P7.17: GC11.0.2 RS64 MEC P3_STACK data microcode.
                // Linux uses the SAME 139264B pinned MEC GFX v2.0 data
                // segment for P2 and P3. Distinct PSP types (96 vs 97).
                // This sixteenth PSP LOAD_IP_FW(type97) requires prior
                // P2 accepted at fence15. No engine/queue startup.
                uint32_t p717OptInRaw = 0u;
                const bool p717OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p717_mec_p3",
                        &p717OptInRaw, sizeof(p717OptInRaw)) &&
                    p717OptInRaw == 1u;
                rn_navi33::p717_gc_mec_p3::Input p717Input{};
                p717Input.requested = p7StepArg >= 19u;
                p717Input.explicitOptIn = p717OptIn;
                p717Input.identityVerified = p716Input.identityVerified;
                p717Input.observedGcMajor = p716Input.observedGcMajor;
                p717Input.observedGcMinor = p716Input.observedGcMinor;
                p717Input.observedGcRevision = p716Input.observedGcRevision;
                p717Input.pspRingReady = p716Input.pspRingReady;
                p717Input.tmrAccepted = p74Accepted;
                p717Input.sdmaBothAccepted = p716Input.sdmaBothAccepted;
                p717Input.pfpAccepted = p77Accepted;
                p717Input.p0Accepted = p78Accepted;
                p717Input.p1Accepted = p79Accepted;
                p717Input.meAccepted = p710Accepted;
                p717Input.meP0Accepted = p711Accepted;
                p717Input.meP1Accepted = p712Accepted;
                p717Input.mecAccepted = p713Accepted;
                p717Input.mecP2Accepted = p716Accepted;
                p717Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p717Input.previousFenceExpected = p716FenceExpected;
                p717Input.previousFenceObserved = p716FenceObserved;
                p717Input.transportHealthy =
                    p716FenceOk && p716ResponseValid &&
                    p716TransportError == 0u &&
                    !p716Quarantined && !p7SubmitTransportQuarantined;
                p717Input.stageOwnerRetained = p716Input.stageOwnerRetained;
                p717Input.stageMapped = p716Input.stageMapped;
                p717Input.stagePteVerified = p716Input.stagePteVerified;
                p717Input.stageBytes = p716Input.stageBytes;
                p717Input.stageGpuVa = p716Input.stageGpuVa;
                p717Input.gartStart = p716Input.gartStart;
                p717Input.gartEnd = p716Input.gartEnd;
                // Same independently reviewed and pinned version 2590
                // MEC Data source as P7.14; P1 requires a distinct PSP
                // firmware type 97 and cannot inherit P0 acceptance.
                p717Input.firmwarePresent = p716Input.firmwarePresent;
                p717Input.firmwareMetadataVerified =
                    p716Input.firmwareMetadataVerified;
                p717Input.firmwareSourceVerified =
                    p716Input.firmwareSourceVerified;
                p717Input.firmwareIpMajor = p716Input.firmwareIpMajor;
                p717Input.firmwareIpMinor = p716Input.firmwareIpMinor;
                p717Input.firmwareIpRevision = p716Input.firmwareIpRevision;
                uint32_t p717FwVersion = p716FwVersion;
#if defined(RN_HAVE_NAVI33_GC_MEC_FW) && \
    defined(RN_NAVI33_GC_MEC_FW_METADATA_V1)
                p717Input.firmwareBytes = kRNNavi33GcMecDataBytes;
#endif
                const auto p717Gate =
                    rn_navi33::p717_gc_mec_p3::inspect(p717Input);

                bool p717FenceOk = false;
                bool p717Submitted = false;
                bool p717ResponseValid = false;
                bool p717Accepted = false;
                uint32_t p717Status = 0xffffffffu;
                uint32_t p717FwAddrLo = 0u, p717FwAddrHi = 0u;
                uint32_t p717TransportError = 0u;
                bool p717Quarantined = p7SubmitTransportQuarantined;
                uint32_t p717WptrBefore = 0u, p717WptrAfter = 0u;
                uint32_t p717RptrBefore = 0u, p717RptrAfterKick = 0u;
                uint32_t p717RptrFinal = 0u;
                uint32_t p717FenceBefore = 0u, p717FenceExpected = 0u;
                uint32_t p717FenceObserved = 0u, p717FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_MEC_FW
                if (p717Gate.mayLoad &&
                    pspFence == rn_navi33::p717_gc_mec_p3::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcMecDataFw) ==
                        static_cast<size_t>(kRNNavi33GcMecDataBytes)) {
                    // PSP accepted MEC P2 Stack(type96) at fence15.
                    // Reuse the retained 66-page DMA stage and copy
                    // only the same verified MEC GFX2.0 Data segment
                    // (139264B) for P3_STACK(type97). No engines.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcMecDataFw,
                           kRNNavi33GcMecDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p717FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p717_gc_mec_p3::kCommandId,
                        p717Gate.words,
                        rn_navi33::p717_gc_mec_p3::kArgumentCount,
                        resp, 4u);
                    p717Submitted = p7SubmitWasIssued;
                    p717ResponseValid = p7SubmitResponseValid;
                    p717TransportError = p7SubmitTransportError;
                    p717Quarantined = p7SubmitTransportQuarantined;
                    p717WptrBefore = p7SubmitWptrBefore;
                    p717WptrAfter = p7SubmitWptrAfter;
                    p717RptrBefore = p7SubmitRptrBefore;
                    p717RptrAfterKick = p7SubmitRptrAfterKick;
                    p717RptrFinal = p7SubmitRptrFinal;
                    p717FenceBefore = p7SubmitFenceBefore;
                    p717FenceExpected = p7SubmitFenceExpected;
                    p717FenceObserved = p7SubmitFenceObserved;
                    p717FencePolls = p7SubmitPolls;
                    if (p717ResponseValid) {
                        p717Status = resp[0];
                        p717FwAddrLo = resp[2];
                        p717FwAddrHi = resp[3];
                    }
                    p717Accepted = p717FenceOk && p717ResponseValid &&
                        p717Status == 0u &&
                        p717FenceExpected ==
                            rn_navi33::p717_gc_mec_p3::kExpectedFence &&
                        p717FenceObserved ==
                            rn_navi33::p717_gc_mec_p3::kExpectedFence;
                } else if (p717Gate.mayLoad) {
                    p717TransportError = 10u;
                }
#endif

                const char *const p717Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "mec_p2_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_mec_p3_loaded",
                    "gc_engine_started"
                };
                const uint32_t p717Values[] = {
                    p7StepArg, static_cast<uint32_t>(p717Gate.error),
                    static_cast<uint32_t>(p717Input.requested),
                    static_cast<uint32_t>(p717OptIn),
                    static_cast<uint32_t>(p717Gate.mayLoad),
                    static_cast<uint32_t>(p717Input.firmwarePresent),
                    static_cast<uint32_t>(p717Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p717Input.firmwareSourceVerified),
                    p717Input.observedGcRevision,
                    p717Input.firmwareIpRevision, p717FwVersion,
                    static_cast<uint32_t>(p717Input.firmwareBytes),
                    static_cast<uint32_t>(p717Input.stageBytes),
                    static_cast<uint32_t>(p717Input.stageMapped),
                    static_cast<uint32_t>(p717Input.stagePteVerified),
                    static_cast<uint32_t>(p717Input.stageGpuVa),
                    static_cast<uint32_t>(p717Input.stageGpuVa >> 32),
                    rn_navi33::p717_gc_mec_p3::kCommandId,
                    rn_navi33::p717_gc_mec_p3::kFirmwareType,
                    p717Input.previousFenceExpected,
                    p717Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p716Accepted),
                    static_cast<uint32_t>(p717Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p717Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p717Submitted),
                    p717WptrBefore, p717WptrAfter,
                    p717RptrBefore, p717RptrAfterKick, p717RptrFinal,
                    p717FenceBefore, p717FenceExpected,
                    p717FenceObserved, p717FencePolls,
                    static_cast<uint32_t>(p717FenceOk),
                    static_cast<uint32_t>(p717ResponseValid),
                    p717Status, p717FwAddrLo, p717FwAddrHi,
                    p717TransportError, static_cast<uint32_t>(p717Quarantined),
                    static_cast<uint32_t>(p717Accepted),
                    static_cast<uint32_t>(p717Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-mec-p3-runtime",
                          p717Names, p717Values,
                          sizeof(p717Values) / sizeof(p717Values[0]));
                // P7.18: RLC-G PSP LOAD_IP_FW type8 after MEC P3 fence16.
                // This loads RLC code but does NOT start RLC, IMU or GC.
                uint32_t p718OptInRaw = 0u;
                const bool p718OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p718_rlc_g",
                        &p718OptInRaw, sizeof(p718OptInRaw)) &&
                    p718OptInRaw == 1u;
                rn_navi33::p718_rlc_g::Input p718Input{};
                p718Input.requested = p7StepArg >= 20u;
                p718Input.explicitOptIn = p718OptIn;
                p718Input.identityVerified = p717Input.identityVerified;
                p718Input.observedGcMajor = p717Input.observedGcMajor;
                p718Input.observedGcMinor = p717Input.observedGcMinor;
                p718Input.observedGcRevision = p717Input.observedGcRevision;
                p718Input.pspRingReady = p717Input.pspRingReady;
                p718Input.tmrAccepted = p74Accepted;
                p718Input.sdmaBothAccepted = p717Input.sdmaBothAccepted;
                p718Input.pfpAccepted = p77Accepted;
                p718Input.p0Accepted = p78Accepted;
                p718Input.p1Accepted = p79Accepted;
                p718Input.meAccepted = p710Accepted;
                p718Input.meP0Accepted = p711Accepted;
                p718Input.meP1Accepted = p712Accepted;
                p718Input.mecAccepted = p713Accepted;
                p718Input.mecP3Accepted = p717Accepted;
                p718Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p718Input.previousFenceExpected = p717FenceExpected;
                p718Input.previousFenceObserved = p717FenceObserved;
                p718Input.transportHealthy =
                    p717FenceOk && p717ResponseValid &&
                    p717TransportError == 0u &&
                    !p717Quarantined && !p7SubmitTransportQuarantined;
                p718Input.stageOwnerRetained = p717Input.stageOwnerRetained;
                p718Input.stageMapped = p717Input.stageMapped;
                p718Input.stagePteVerified = p717Input.stagePteVerified;
                p718Input.stageBytes = p717Input.stageBytes;
                p718Input.stageGpuVa = p717Input.stageGpuVa;
                p718Input.gartStart = p717Input.gartStart;
                p718Input.gartEnd = p717Input.gartEnd;
                // RLC-G must come from its own pinned source, never
                // inherit trusted MEC metadata or size by accident.
                uint32_t p718FwVersion = 0u;
#if defined(RN_HAVE_NAVI33_GC_RLC_FW) && \
    defined(RN_NAVI33_GC_RLC_FW_METADATA_V1) && \
    defined(RN_NAVI33_GC_RLC_FW_SOURCE_VERIFIED)
                p718Input.firmwarePresent = kRNNavi33GcRlcGBytes > 0u;
                p718Input.firmwareMetadataVerified =
                    kRNNavi33GcRlcSourceBytes == 178848u &&
                    kRNNavi33GcRlcHeaderMajor == 2u;
                p718Input.firmwareSourceVerified =
                    RN_NAVI33_GC_RLC_FW_SOURCE_VERIFIED == 1;
                p718Input.firmwareIpMajor = kRNNavi33GcRlcIpMajor;
                p718Input.firmwareIpMinor = kRNNavi33GcRlcIpMinor;
                p718Input.firmwareIpRevision = kRNNavi33GcRlcIpRevision;
                p718Input.firmwareBytes = kRNNavi33GcRlcGBytes;
                p718FwVersion = kRNNavi33GcRlcVersion;
#endif
                const auto p718Gate =
                    rn_navi33::p718_rlc_g::inspect(p718Input);

                bool p718FenceOk = false;
                bool p718Submitted = false;
                bool p718ResponseValid = false;
                bool p718Accepted = false;
                uint32_t p718Status = 0xffffffffu;
                uint32_t p718FwAddrLo = 0u, p718FwAddrHi = 0u;
                uint32_t p718TransportError = 0u;
                bool p718Quarantined = p7SubmitTransportQuarantined;
                uint32_t p718WptrBefore = 0u, p718WptrAfter = 0u;
                uint32_t p718RptrBefore = 0u, p718RptrAfterKick = 0u;
                uint32_t p718RptrFinal = 0u;
                uint32_t p718FenceBefore = 0u, p718FenceExpected = 0u;
                uint32_t p718FenceObserved = 0u, p718FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_RLC_FW
                if (p718Gate.mayLoad &&
                    pspFence == rn_navi33::p718_rlc_g::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcRlcGFw) ==
                        static_cast<size_t>(kRNNavi33GcRlcGBytes)) {
                    // The vetted RLC-G executable payload is staged
                    // from a locally generated header. No RLC startup.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcRlcGFw,
                           kRNNavi33GcRlcGBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p718FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p718_rlc_g::kCommandId,
                        p718Gate.words,
                        rn_navi33::p718_rlc_g::kArgumentCount,
                        resp, 4u);
                    p718Submitted = p7SubmitWasIssued;
                    p718ResponseValid = p7SubmitResponseValid;
                    p718TransportError = p7SubmitTransportError;
                    p718Quarantined = p7SubmitTransportQuarantined;
                    p718WptrBefore = p7SubmitWptrBefore;
                    p718WptrAfter = p7SubmitWptrAfter;
                    p718RptrBefore = p7SubmitRptrBefore;
                    p718RptrAfterKick = p7SubmitRptrAfterKick;
                    p718RptrFinal = p7SubmitRptrFinal;
                    p718FenceBefore = p7SubmitFenceBefore;
                    p718FenceExpected = p7SubmitFenceExpected;
                    p718FenceObserved = p7SubmitFenceObserved;
                    p718FencePolls = p7SubmitPolls;
                    if (p718ResponseValid) {
                        p718Status = resp[0];
                        p718FwAddrLo = resp[2];
                        p718FwAddrHi = resp[3];
                    }
                    p718Accepted = p718FenceOk && p718ResponseValid &&
                        p718Status == 0u &&
                        p718FenceExpected ==
                            rn_navi33::p718_rlc_g::kExpectedFence &&
                        p718FenceObserved ==
                            rn_navi33::p718_rlc_g::kExpectedFence;
                } else if (p718Gate.mayLoad) {
                    p718TransportError = 10u;
                }
#endif

                const char *const p718Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "mec_p3_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_rlc_g_loaded",
                    "gc_engine_started"
                };
                const uint32_t p718Values[] = {
                    p7StepArg, static_cast<uint32_t>(p718Gate.error),
                    static_cast<uint32_t>(p718Input.requested),
                    static_cast<uint32_t>(p718OptIn),
                    static_cast<uint32_t>(p718Gate.mayLoad),
                    static_cast<uint32_t>(p718Input.firmwarePresent),
                    static_cast<uint32_t>(p718Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p718Input.firmwareSourceVerified),
                    p718Input.observedGcRevision,
                    p718Input.firmwareIpRevision, p718FwVersion,
                    static_cast<uint32_t>(p718Input.firmwareBytes),
                    static_cast<uint32_t>(p718Input.stageBytes),
                    static_cast<uint32_t>(p718Input.stageMapped),
                    static_cast<uint32_t>(p718Input.stagePteVerified),
                    static_cast<uint32_t>(p718Input.stageGpuVa),
                    static_cast<uint32_t>(p718Input.stageGpuVa >> 32),
                    rn_navi33::p718_rlc_g::kCommandId,
                    rn_navi33::p718_rlc_g::kFirmwareType,
                    p718Input.previousFenceExpected,
                    p718Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p717Accepted),
                    static_cast<uint32_t>(p718Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p718Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p718Submitted),
                    p718WptrBefore, p718WptrAfter,
                    p718RptrBefore, p718RptrAfterKick, p718RptrFinal,
                    p718FenceBefore, p718FenceExpected,
                    p718FenceObserved, p718FencePolls,
                    static_cast<uint32_t>(p718FenceOk),
                    static_cast<uint32_t>(p718ResponseValid),
                    p718Status, p718FwAddrLo, p718FwAddrHi,
                    p718TransportError, static_cast<uint32_t>(p718Quarantined),
                    static_cast<uint32_t>(p718Accepted),
                    static_cast<uint32_t>(p718Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-rlc-g-runtime",
                          p718Names, p718Values,
                          sizeof(p718Values) / sizeof(p718Values[0]));
                // P7.19: IMU_I PSP LOAD_IP_FW type 68
                // after RLC_G fence 17. No IMU execution.
                uint32_t p719OptInRaw = 0u;
                const bool p719OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p719_imu_i",
                        &p719OptInRaw, sizeof(p719OptInRaw)) &&
                    p719OptInRaw == 1u;
                rn_navi33::p719_imu_i::Input p719Input{};
                p719Input.requested = p7StepArg >= 21u;
                p719Input.explicitOptIn = p719OptIn;
                p719Input.identityVerified = p718Input.identityVerified;
                p719Input.observedGcMajor = p718Input.observedGcMajor;
                p719Input.observedGcMinor = p718Input.observedGcMinor;
                p719Input.observedGcRevision = p718Input.observedGcRevision;
                p719Input.pspRingReady = p718Input.pspRingReady;
                p719Input.tmrAccepted = p74Accepted;
                p719Input.sdmaBothAccepted = p718Input.sdmaBothAccepted;
                p719Input.pfpAccepted = p77Accepted;
                p719Input.p0Accepted = p78Accepted;
                p719Input.p1Accepted = p79Accepted;
                p719Input.meAccepted = p710Accepted;
                p719Input.meP0Accepted = p711Accepted;
                p719Input.meP1Accepted = p712Accepted;
                p719Input.mecAccepted = p713Accepted;
                p719Input.rlcGAccepted = p718Accepted;
                p719Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p719Input.previousFenceExpected = p718FenceExpected;
                p719Input.previousFenceObserved = p718FenceObserved;
                p719Input.transportHealthy =
                    p718FenceOk && p718ResponseValid &&
                    p718TransportError == 0u &&
                    !p718Quarantined && !p7SubmitTransportQuarantined;
                p719Input.stageOwnerRetained = p718Input.stageOwnerRetained;
                p719Input.stageMapped = p718Input.stageMapped;
                p719Input.stagePteVerified = p718Input.stagePteVerified;
                p719Input.stageBytes = p718Input.stageBytes;
                p719Input.stageGpuVa = p718Input.stageGpuVa;
                p719Input.gartStart = p718Input.gartStart;
                p719Input.gartEnd = p718Input.gartEnd;
                // IMU I/D source independently pinned; do not borrow
                // metadata or acceptance from the preceding firmware.
                uint32_t p719FwVersion = 0u;
#if defined(RN_HAVE_NAVI33_GC_IMU_FW) && \
    defined(RN_NAVI33_GC_IMU_FW_METADATA_V1) && \
    defined(RN_NAVI33_GC_IMU_FW_SOURCE_VERIFIED)
                p719Input.firmwarePresent = kRNNavi33GcImuIBytes > 0u;
                p719Input.firmwareMetadataVerified =
                    kRNNavi33GcImuSourceBytes == 132352u &&
                    kRNNavi33GcImuIBytes == 66048u &&
                    kRNNavi33GcImuDBytes == 66048u;
                p719Input.firmwareSourceVerified =
                    RN_NAVI33_GC_IMU_FW_SOURCE_VERIFIED == 1;
                p719Input.firmwareIpMajor = kRNNavi33GcImuIpMajor;
                p719Input.firmwareIpMinor = kRNNavi33GcImuIpMinor;
                p719Input.firmwareIpRevision = kRNNavi33GcImuIpRevision;
                p719Input.firmwareBytes = kRNNavi33GcImuIBytes;
                p719FwVersion = kRNNavi33GcImuVersion;
#endif
                const auto p719Gate =
                    rn_navi33::p719_imu_i::inspect(p719Input);

                bool p719FenceOk = false;
                bool p719Submitted = false;
                bool p719ResponseValid = false;
                bool p719Accepted = false;
                uint32_t p719Status = 0xffffffffu;
                uint32_t p719FwAddrLo = 0u, p719FwAddrHi = 0u;
                uint32_t p719TransportError = 0u;
                bool p719Quarantined = p7SubmitTransportQuarantined;
                uint32_t p719WptrBefore = 0u, p719WptrAfter = 0u;
                uint32_t p719RptrBefore = 0u, p719RptrAfterKick = 0u;
                uint32_t p719RptrFinal = 0u;
                uint32_t p719FenceBefore = 0u, p719FenceExpected = 0u;
                uint32_t p719FenceObserved = 0u, p719FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_IMU_FW
                if (p719Gate.mayLoad &&
                    pspFence == rn_navi33::p719_imu_i::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcImuIFw) ==
                        static_cast<size_t>(kRNNavi33GcImuIBytes)) {
                    // Staged from independently pinned IMU I payload;
                    // this does NOT start IMU or RLC.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcImuIFw,
                           kRNNavi33GcImuIBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p719FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p719_imu_i::kCommandId,
                        p719Gate.words,
                        rn_navi33::p719_imu_i::kArgumentCount,
                        resp, 4u);
                    p719Submitted = p7SubmitWasIssued;
                    p719ResponseValid = p7SubmitResponseValid;
                    p719TransportError = p7SubmitTransportError;
                    p719Quarantined = p7SubmitTransportQuarantined;
                    p719WptrBefore = p7SubmitWptrBefore;
                    p719WptrAfter = p7SubmitWptrAfter;
                    p719RptrBefore = p7SubmitRptrBefore;
                    p719RptrAfterKick = p7SubmitRptrAfterKick;
                    p719RptrFinal = p7SubmitRptrFinal;
                    p719FenceBefore = p7SubmitFenceBefore;
                    p719FenceExpected = p7SubmitFenceExpected;
                    p719FenceObserved = p7SubmitFenceObserved;
                    p719FencePolls = p7SubmitPolls;
                    if (p719ResponseValid) {
                        p719Status = resp[0];
                        p719FwAddrLo = resp[2];
                        p719FwAddrHi = resp[3];
                    }
                    p719Accepted = p719FenceOk && p719ResponseValid &&
                        p719Status == 0u &&
                        p719FenceExpected ==
                            rn_navi33::p719_imu_i::kExpectedFence &&
                        p719FenceObserved ==
                            rn_navi33::p719_imu_i::kExpectedFence;
                } else if (p719Gate.mayLoad) {
                    p719TransportError = 10u;
                }
#endif

                const char *const p719Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "rlc_g_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_imu_i_loaded",
                    "gc_engine_started"
                };
                const uint32_t p719Values[] = {
                    p7StepArg, static_cast<uint32_t>(p719Gate.error),
                    static_cast<uint32_t>(p719Input.requested),
                    static_cast<uint32_t>(p719OptIn),
                    static_cast<uint32_t>(p719Gate.mayLoad),
                    static_cast<uint32_t>(p719Input.firmwarePresent),
                    static_cast<uint32_t>(p719Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p719Input.firmwareSourceVerified),
                    p719Input.observedGcRevision,
                    p719Input.firmwareIpRevision, p719FwVersion,
                    static_cast<uint32_t>(p719Input.firmwareBytes),
                    static_cast<uint32_t>(p719Input.stageBytes),
                    static_cast<uint32_t>(p719Input.stageMapped),
                    static_cast<uint32_t>(p719Input.stagePteVerified),
                    static_cast<uint32_t>(p719Input.stageGpuVa),
                    static_cast<uint32_t>(p719Input.stageGpuVa >> 32),
                    rn_navi33::p719_imu_i::kCommandId,
                    rn_navi33::p719_imu_i::kFirmwareType,
                    p719Input.previousFenceExpected,
                    p719Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p718Accepted),
                    static_cast<uint32_t>(p719Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p719Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p719Submitted),
                    p719WptrBefore, p719WptrAfter,
                    p719RptrBefore, p719RptrAfterKick, p719RptrFinal,
                    p719FenceBefore, p719FenceExpected,
                    p719FenceObserved, p719FencePolls,
                    static_cast<uint32_t>(p719FenceOk),
                    static_cast<uint32_t>(p719ResponseValid),
                    p719Status, p719FwAddrLo, p719FwAddrHi,
                    p719TransportError, static_cast<uint32_t>(p719Quarantined),
                    static_cast<uint32_t>(p719Accepted),
                    static_cast<uint32_t>(p719Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-imu-i-runtime",
                          p719Names, p719Values,
                          sizeof(p719Values) / sizeof(p719Values[0]));
                // P7.20: IMU_D PSP LOAD_IP_FW type 69
                // after IMU_I fence 18. No IMU execution.
                uint32_t p720OptInRaw = 0u;
                const bool p720OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p720_imu_d",
                        &p720OptInRaw, sizeof(p720OptInRaw)) &&
                    p720OptInRaw == 1u;
                rn_navi33::p720_imu_d::Input p720Input{};
                p720Input.requested = p7StepArg >= 22u;
                p720Input.explicitOptIn = p720OptIn;
                p720Input.identityVerified = p719Input.identityVerified;
                p720Input.observedGcMajor = p719Input.observedGcMajor;
                p720Input.observedGcMinor = p719Input.observedGcMinor;
                p720Input.observedGcRevision = p719Input.observedGcRevision;
                p720Input.pspRingReady = p719Input.pspRingReady;
                p720Input.tmrAccepted = p74Accepted;
                p720Input.sdmaBothAccepted = p719Input.sdmaBothAccepted;
                p720Input.pfpAccepted = p77Accepted;
                p720Input.p0Accepted = p78Accepted;
                p720Input.p1Accepted = p79Accepted;
                p720Input.meAccepted = p710Accepted;
                p720Input.meP0Accepted = p711Accepted;
                p720Input.meP1Accepted = p712Accepted;
                p720Input.mecAccepted = p713Accepted;
                p720Input.imuIAccepted = p719Accepted;
                p720Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p720Input.previousFenceExpected = p719FenceExpected;
                p720Input.previousFenceObserved = p719FenceObserved;
                p720Input.transportHealthy =
                    p719FenceOk && p719ResponseValid &&
                    p719TransportError == 0u &&
                    !p719Quarantined && !p7SubmitTransportQuarantined;
                p720Input.stageOwnerRetained = p719Input.stageOwnerRetained;
                p720Input.stageMapped = p719Input.stageMapped;
                p720Input.stagePteVerified = p719Input.stagePteVerified;
                p720Input.stageBytes = p719Input.stageBytes;
                p720Input.stageGpuVa = p719Input.stageGpuVa;
                p720Input.gartStart = p719Input.gartStart;
                p720Input.gartEnd = p719Input.gartEnd;
                // IMU I/D source independently pinned; do not borrow
                // metadata or acceptance from the preceding firmware.
                uint32_t p720FwVersion = 0u;
#if defined(RN_HAVE_NAVI33_GC_IMU_FW) && \
    defined(RN_NAVI33_GC_IMU_FW_METADATA_V1) && \
    defined(RN_NAVI33_GC_IMU_FW_SOURCE_VERIFIED)
                p720Input.firmwarePresent = kRNNavi33GcImuDBytes > 0u;
                p720Input.firmwareMetadataVerified =
                    kRNNavi33GcImuSourceBytes == 132352u &&
                    kRNNavi33GcImuIBytes == 66048u &&
                    kRNNavi33GcImuDBytes == 66048u;
                p720Input.firmwareSourceVerified =
                    RN_NAVI33_GC_IMU_FW_SOURCE_VERIFIED == 1;
                p720Input.firmwareIpMajor = kRNNavi33GcImuIpMajor;
                p720Input.firmwareIpMinor = kRNNavi33GcImuIpMinor;
                p720Input.firmwareIpRevision = kRNNavi33GcImuIpRevision;
                p720Input.firmwareBytes = kRNNavi33GcImuDBytes;
                p720FwVersion = kRNNavi33GcImuVersion;
#endif
                const auto p720Gate =
                    rn_navi33::p720_imu_d::inspect(p720Input);

                bool p720FenceOk = false;
                bool p720Submitted = false;
                bool p720ResponseValid = false;
                bool p720Accepted = false;
                uint32_t p720Status = 0xffffffffu;
                uint32_t p720FwAddrLo = 0u, p720FwAddrHi = 0u;
                uint32_t p720TransportError = 0u;
                bool p720Quarantined = p7SubmitTransportQuarantined;
                uint32_t p720WptrBefore = 0u, p720WptrAfter = 0u;
                uint32_t p720RptrBefore = 0u, p720RptrAfterKick = 0u;
                uint32_t p720RptrFinal = 0u;
                uint32_t p720FenceBefore = 0u, p720FenceExpected = 0u;
                uint32_t p720FenceObserved = 0u, p720FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_GC_IMU_FW
                if (p720Gate.mayLoad &&
                    pspFence == rn_navi33::p720_imu_d::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33GcImuDFw) ==
                        static_cast<size_t>(kRNNavi33GcImuDBytes)) {
                    // Staged from independently pinned IMU D payload;
                    // this does NOT start IMU or RLC.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33GcImuDFw,
                           kRNNavi33GcImuDBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p720FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p720_imu_d::kCommandId,
                        p720Gate.words,
                        rn_navi33::p720_imu_d::kArgumentCount,
                        resp, 4u);
                    p720Submitted = p7SubmitWasIssued;
                    p720ResponseValid = p7SubmitResponseValid;
                    p720TransportError = p7SubmitTransportError;
                    p720Quarantined = p7SubmitTransportQuarantined;
                    p720WptrBefore = p7SubmitWptrBefore;
                    p720WptrAfter = p7SubmitWptrAfter;
                    p720RptrBefore = p7SubmitRptrBefore;
                    p720RptrAfterKick = p7SubmitRptrAfterKick;
                    p720RptrFinal = p7SubmitRptrFinal;
                    p720FenceBefore = p7SubmitFenceBefore;
                    p720FenceExpected = p7SubmitFenceExpected;
                    p720FenceObserved = p7SubmitFenceObserved;
                    p720FencePolls = p7SubmitPolls;
                    if (p720ResponseValid) {
                        p720Status = resp[0];
                        p720FwAddrLo = resp[2];
                        p720FwAddrHi = resp[3];
                    }
                    p720Accepted = p720FenceOk && p720ResponseValid &&
                        p720Status == 0u &&
                        p720FenceExpected ==
                            rn_navi33::p720_imu_d::kExpectedFence &&
                        p720FenceObserved ==
                            rn_navi33::p720_imu_d::kExpectedFence;
                } else if (p720Gate.mayLoad) {
                    p720TransportError = 10u;
                }
#endif

                const char *const p720Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "imu_i_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "gc_imu_d_loaded",
                    "gc_engine_started"
                };
                const uint32_t p720Values[] = {
                    p7StepArg, static_cast<uint32_t>(p720Gate.error),
                    static_cast<uint32_t>(p720Input.requested),
                    static_cast<uint32_t>(p720OptIn),
                    static_cast<uint32_t>(p720Gate.mayLoad),
                    static_cast<uint32_t>(p720Input.firmwarePresent),
                    static_cast<uint32_t>(p720Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p720Input.firmwareSourceVerified),
                    p720Input.observedGcRevision,
                    p720Input.firmwareIpRevision, p720FwVersion,
                    static_cast<uint32_t>(p720Input.firmwareBytes),
                    static_cast<uint32_t>(p720Input.stageBytes),
                    static_cast<uint32_t>(p720Input.stageMapped),
                    static_cast<uint32_t>(p720Input.stagePteVerified),
                    static_cast<uint32_t>(p720Input.stageGpuVa),
                    static_cast<uint32_t>(p720Input.stageGpuVa >> 32),
                    rn_navi33::p720_imu_d::kCommandId,
                    rn_navi33::p720_imu_d::kFirmwareType,
                    p720Input.previousFenceExpected,
                    p720Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p719Accepted),
                    static_cast<uint32_t>(p720Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p720Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p720Submitted),
                    p720WptrBefore, p720WptrAfter,
                    p720RptrBefore, p720RptrAfterKick, p720RptrFinal,
                    p720FenceBefore, p720FenceExpected,
                    p720FenceObserved, p720FencePolls,
                    static_cast<uint32_t>(p720FenceOk),
                    static_cast<uint32_t>(p720ResponseValid),
                    p720Status, p720FwAddrLo, p720FwAddrHi,
                    p720TransportError, static_cast<uint32_t>(p720Quarantined),
                    static_cast<uint32_t>(p720Accepted),
                    static_cast<uint32_t>(p720Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-gc-imu-d-runtime",
                          p720Names, p720Values,
                          sizeof(p720Values) / sizeof(p720Values[0]));
                // P7.21: Linux GC11 MES mes_sched_code PSP type 33.
                // Stages MES firmware only; does not start GC or queues.
                uint32_t p721OptInRaw = 0u;
                const bool p721OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p721_mes_sched_code",
                        &p721OptInRaw, sizeof(p721OptInRaw)) &&
                    p721OptInRaw == 1u;
                rn_navi33::p721_mes_sched_code::Input p721Input{};
                p721Input.requested = p7StepArg >= 23u;
                p721Input.explicitOptIn = p721OptIn;
                p721Input.identityVerified = p720Input.identityVerified;
                p721Input.observedGcMajor = p720Input.observedGcMajor;
                p721Input.observedGcMinor = p720Input.observedGcMinor;
                p721Input.observedGcRevision = p720Input.observedGcRevision;
                p721Input.pspRingReady = p720Input.pspRingReady;
                p721Input.tmrAccepted = p74Accepted;
                p721Input.sdmaBothAccepted = p720Input.sdmaBothAccepted;
                p721Input.pfpAccepted = p77Accepted;
                p721Input.p0Accepted = p78Accepted;
                p721Input.p1Accepted = p79Accepted;
                p721Input.meAccepted = p710Accepted;
                p721Input.meP0Accepted = p711Accepted;
                p721Input.meP1Accepted = p712Accepted;
                p721Input.mecAccepted = p713Accepted;
                p721Input.priorStageAccepted = p720Accepted;
                p721Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p721Input.previousFenceExpected = p720FenceExpected;
                p721Input.previousFenceObserved = p720FenceObserved;
                p721Input.transportHealthy =
                    p720FenceOk && p720ResponseValid &&
                    p720TransportError == 0u &&
                    !p720Quarantined && !p7SubmitTransportQuarantined;
                p721Input.stageOwnerRetained = p720Input.stageOwnerRetained;
                p721Input.stageMapped = p720Input.stageMapped;
                p721Input.stagePteVerified = p720Input.stagePteVerified;
                p721Input.stageBytes = p720Input.stageBytes;
                p721Input.stageGpuVa = p720Input.stageGpuVa;
                p721Input.gartStart = p720Input.gartStart;
                p721Input.gartEnd = p720Input.gartEnd;
                // Independently pinned Linux MES_Sched source; this
                // uses the MES-specific code or data bytes, never the
                // entire vendor firmware container or previous stage.
                uint32_t p721FwVersion = 0u;
#if defined(RN_HAVE_NAVI33_MES_FW) && \
    defined(RN_NAVI33_MES_FW_METADATA_V1) && \
    defined(RN_NAVI33_MES_FW_SOURCE_VERIFIED)
                p721Input.firmwarePresent =
                    kRNNavi33MesSchedCodeBytes > 0u;
                p721Input.firmwareMetadataVerified =
                    kRNNavi33MesSchedSourceBytes == 256288u &&
                    kRNNavi33MesSchedCodeBytes == 124960u &&
                    kRNNavi33MesSchedDataBytes == 131072u;
                p721Input.firmwareSourceVerified =
                    RN_NAVI33_MES_FW_SOURCE_VERIFIED == 1;
                p721Input.firmwareIpMajor = kRNNavi33MesIpMajor;
                p721Input.firmwareIpMinor = kRNNavi33MesIpMinor;
                p721Input.firmwareIpRevision = kRNNavi33MesIpRevision;
                p721Input.firmwareBytes = kRNNavi33MesSchedCodeBytes;
                p721FwVersion = kRNNavi33MesSchedVersion;
#endif
                const auto p721Gate =
                    rn_navi33::p721_mes_sched_code::inspect(p721Input);

                bool p721FenceOk = false;
                bool p721Submitted = false;
                bool p721ResponseValid = false;
                bool p721Accepted = false;
                uint32_t p721Status = 0xffffffffu;
                uint32_t p721FwAddrLo = 0u, p721FwAddrHi = 0u;
                uint32_t p721TransportError = 0u;
                bool p721Quarantined = p7SubmitTransportQuarantined;
                uint32_t p721WptrBefore = 0u, p721WptrAfter = 0u;
                uint32_t p721RptrBefore = 0u, p721RptrAfterKick = 0u;
                uint32_t p721RptrFinal = 0u;
                uint32_t p721FenceBefore = 0u, p721FenceExpected = 0u;
                uint32_t p721FenceObserved = 0u, p721FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_MES_FW
                if (p721Gate.mayLoad &&
                    pspFence == rn_navi33::p721_mes_sched_code::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33MesSchedCodeFw) ==
                        static_cast<size_t>(kRNNavi33MesSchedCodeBytes)) {
                    // MES mes_sched_code image only; no RLC/GC/MES start.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33MesSchedCodeFw,
                           kRNNavi33MesSchedCodeBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p721FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p721_mes_sched_code::kCommandId,
                        p721Gate.words,
                        rn_navi33::p721_mes_sched_code::kArgumentCount,
                        resp, 4u);
                    p721Submitted = p7SubmitWasIssued;
                    p721ResponseValid = p7SubmitResponseValid;
                    p721TransportError = p7SubmitTransportError;
                    p721Quarantined = p7SubmitTransportQuarantined;
                    p721WptrBefore = p7SubmitWptrBefore;
                    p721WptrAfter = p7SubmitWptrAfter;
                    p721RptrBefore = p7SubmitRptrBefore;
                    p721RptrAfterKick = p7SubmitRptrAfterKick;
                    p721RptrFinal = p7SubmitRptrFinal;
                    p721FenceBefore = p7SubmitFenceBefore;
                    p721FenceExpected = p7SubmitFenceExpected;
                    p721FenceObserved = p7SubmitFenceObserved;
                    p721FencePolls = p7SubmitPolls;
                    if (p721ResponseValid) {
                        p721Status = resp[0];
                        p721FwAddrLo = resp[2];
                        p721FwAddrHi = resp[3];
                    }
                    p721Accepted = p721FenceOk && p721ResponseValid &&
                        p721Status == 0u &&
                        p721FenceExpected ==
                            rn_navi33::p721_mes_sched_code::kExpectedFence &&
                        p721FenceObserved ==
                            rn_navi33::p721_mes_sched_code::kExpectedFence;
                } else if (p721Gate.mayLoad) {
                    p721TransportError = 10u;
                }
#endif

                const char *const p721Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "prior_stage_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "mes_sched_code_loaded",
                    "gc_engine_started"
                };
                const uint32_t p721Values[] = {
                    p7StepArg, static_cast<uint32_t>(p721Gate.error),
                    static_cast<uint32_t>(p721Input.requested),
                    static_cast<uint32_t>(p721OptIn),
                    static_cast<uint32_t>(p721Gate.mayLoad),
                    static_cast<uint32_t>(p721Input.firmwarePresent),
                    static_cast<uint32_t>(p721Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p721Input.firmwareSourceVerified),
                    p721Input.observedGcRevision,
                    p721Input.firmwareIpRevision, p721FwVersion,
                    static_cast<uint32_t>(p721Input.firmwareBytes),
                    static_cast<uint32_t>(p721Input.stageBytes),
                    static_cast<uint32_t>(p721Input.stageMapped),
                    static_cast<uint32_t>(p721Input.stagePteVerified),
                    static_cast<uint32_t>(p721Input.stageGpuVa),
                    static_cast<uint32_t>(p721Input.stageGpuVa >> 32),
                    rn_navi33::p721_mes_sched_code::kCommandId,
                    rn_navi33::p721_mes_sched_code::kFirmwareType,
                    p721Input.previousFenceExpected,
                    p721Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p720Accepted),
                    static_cast<uint32_t>(p721Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p721Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p721Submitted),
                    p721WptrBefore, p721WptrAfter,
                    p721RptrBefore, p721RptrAfterKick, p721RptrFinal,
                    p721FenceBefore, p721FenceExpected,
                    p721FenceObserved, p721FencePolls,
                    static_cast<uint32_t>(p721FenceOk),
                    static_cast<uint32_t>(p721ResponseValid),
                    p721Status, p721FwAddrLo, p721FwAddrHi,
                    p721TransportError, static_cast<uint32_t>(p721Quarantined),
                    static_cast<uint32_t>(p721Accepted),
                    static_cast<uint32_t>(p721Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-mes-sched-code-runtime",
                          p721Names, p721Values,
                          sizeof(p721Values) / sizeof(p721Values[0]));

                // P7.22: Linux GC11 MES mes_sched_data PSP type 34.
                // Stages MES firmware only; does not start GC or queues.
                uint32_t p722OptInRaw = 0u;
                const bool p722OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p722_mes_sched_data",
                        &p722OptInRaw, sizeof(p722OptInRaw)) &&
                    p722OptInRaw == 1u;
                rn_navi33::p722_mes_sched_data::Input p722Input{};
                p722Input.requested = p7StepArg >= 24u;
                p722Input.explicitOptIn = p722OptIn;
                p722Input.identityVerified = p721Input.identityVerified;
                p722Input.observedGcMajor = p721Input.observedGcMajor;
                p722Input.observedGcMinor = p721Input.observedGcMinor;
                p722Input.observedGcRevision = p721Input.observedGcRevision;
                p722Input.pspRingReady = p721Input.pspRingReady;
                p722Input.tmrAccepted = p74Accepted;
                p722Input.sdmaBothAccepted = p721Input.sdmaBothAccepted;
                p722Input.pfpAccepted = p77Accepted;
                p722Input.p0Accepted = p78Accepted;
                p722Input.p1Accepted = p79Accepted;
                p722Input.meAccepted = p710Accepted;
                p722Input.meP0Accepted = p711Accepted;
                p722Input.meP1Accepted = p712Accepted;
                p722Input.mecAccepted = p713Accepted;
                p722Input.priorStageAccepted = p721Accepted;
                p722Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p722Input.previousFenceExpected = p721FenceExpected;
                p722Input.previousFenceObserved = p721FenceObserved;
                p722Input.transportHealthy =
                    p721FenceOk && p721ResponseValid &&
                    p721TransportError == 0u &&
                    !p721Quarantined && !p7SubmitTransportQuarantined;
                p722Input.stageOwnerRetained = p721Input.stageOwnerRetained;
                p722Input.stageMapped = p721Input.stageMapped;
                p722Input.stagePteVerified = p721Input.stagePteVerified;
                p722Input.stageBytes = p721Input.stageBytes;
                p722Input.stageGpuVa = p721Input.stageGpuVa;
                p722Input.gartStart = p721Input.gartStart;
                p722Input.gartEnd = p721Input.gartEnd;
                // Independently pinned Linux MES_Sched source; this
                // uses the MES-specific code or data bytes, never the
                // entire vendor firmware container or previous stage.
                uint32_t p722FwVersion = 0u;
#if defined(RN_HAVE_NAVI33_MES_FW) && \
    defined(RN_NAVI33_MES_FW_METADATA_V1) && \
    defined(RN_NAVI33_MES_FW_SOURCE_VERIFIED)
                p722Input.firmwarePresent =
                    kRNNavi33MesSchedDataBytes > 0u;
                p722Input.firmwareMetadataVerified =
                    kRNNavi33MesSchedSourceBytes == 256288u &&
                    kRNNavi33MesSchedCodeBytes == 124960u &&
                    kRNNavi33MesSchedDataBytes == 131072u;
                p722Input.firmwareSourceVerified =
                    RN_NAVI33_MES_FW_SOURCE_VERIFIED == 1;
                p722Input.firmwareIpMajor = kRNNavi33MesIpMajor;
                p722Input.firmwareIpMinor = kRNNavi33MesIpMinor;
                p722Input.firmwareIpRevision = kRNNavi33MesIpRevision;
                p722Input.firmwareBytes = kRNNavi33MesSchedDataBytes;
                p722FwVersion = kRNNavi33MesSchedVersion;
#endif
                const auto p722Gate =
                    rn_navi33::p722_mes_sched_data::inspect(p722Input);

                bool p722FenceOk = false;
                bool p722Submitted = false;
                bool p722ResponseValid = false;
                bool p722Accepted = false;
                uint32_t p722Status = 0xffffffffu;
                uint32_t p722FwAddrLo = 0u, p722FwAddrHi = 0u;
                uint32_t p722TransportError = 0u;
                bool p722Quarantined = p7SubmitTransportQuarantined;
                uint32_t p722WptrBefore = 0u, p722WptrAfter = 0u;
                uint32_t p722RptrBefore = 0u, p722RptrAfterKick = 0u;
                uint32_t p722RptrFinal = 0u;
                uint32_t p722FenceBefore = 0u, p722FenceExpected = 0u;
                uint32_t p722FenceObserved = 0u, p722FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_MES_FW
                if (p722Gate.mayLoad &&
                    pspFence == rn_navi33::p722_mes_sched_data::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33MesSchedDataFw) ==
                        static_cast<size_t>(kRNNavi33MesSchedDataBytes)) {
                    // MES mes_sched_data image only; no RLC/GC/MES start.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33MesSchedDataFw,
                           kRNNavi33MesSchedDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p722FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p722_mes_sched_data::kCommandId,
                        p722Gate.words,
                        rn_navi33::p722_mes_sched_data::kArgumentCount,
                        resp, 4u);
                    p722Submitted = p7SubmitWasIssued;
                    p722ResponseValid = p7SubmitResponseValid;
                    p722TransportError = p7SubmitTransportError;
                    p722Quarantined = p7SubmitTransportQuarantined;
                    p722WptrBefore = p7SubmitWptrBefore;
                    p722WptrAfter = p7SubmitWptrAfter;
                    p722RptrBefore = p7SubmitRptrBefore;
                    p722RptrAfterKick = p7SubmitRptrAfterKick;
                    p722RptrFinal = p7SubmitRptrFinal;
                    p722FenceBefore = p7SubmitFenceBefore;
                    p722FenceExpected = p7SubmitFenceExpected;
                    p722FenceObserved = p7SubmitFenceObserved;
                    p722FencePolls = p7SubmitPolls;
                    if (p722ResponseValid) {
                        p722Status = resp[0];
                        p722FwAddrLo = resp[2];
                        p722FwAddrHi = resp[3];
                    }
                    p722Accepted = p722FenceOk && p722ResponseValid &&
                        p722Status == 0u &&
                        p722FenceExpected ==
                            rn_navi33::p722_mes_sched_data::kExpectedFence &&
                        p722FenceObserved ==
                            rn_navi33::p722_mes_sched_data::kExpectedFence;
                } else if (p722Gate.mayLoad) {
                    p722TransportError = 10u;
                }
#endif

                const char *const p722Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "prior_stage_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "mes_sched_data_loaded",
                    "gc_engine_started"
                };
                const uint32_t p722Values[] = {
                    p7StepArg, static_cast<uint32_t>(p722Gate.error),
                    static_cast<uint32_t>(p722Input.requested),
                    static_cast<uint32_t>(p722OptIn),
                    static_cast<uint32_t>(p722Gate.mayLoad),
                    static_cast<uint32_t>(p722Input.firmwarePresent),
                    static_cast<uint32_t>(p722Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p722Input.firmwareSourceVerified),
                    p722Input.observedGcRevision,
                    p722Input.firmwareIpRevision, p722FwVersion,
                    static_cast<uint32_t>(p722Input.firmwareBytes),
                    static_cast<uint32_t>(p722Input.stageBytes),
                    static_cast<uint32_t>(p722Input.stageMapped),
                    static_cast<uint32_t>(p722Input.stagePteVerified),
                    static_cast<uint32_t>(p722Input.stageGpuVa),
                    static_cast<uint32_t>(p722Input.stageGpuVa >> 32),
                    rn_navi33::p722_mes_sched_data::kCommandId,
                    rn_navi33::p722_mes_sched_data::kFirmwareType,
                    p722Input.previousFenceExpected,
                    p722Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p721Accepted),
                    static_cast<uint32_t>(p722Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p722Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p722Submitted),
                    p722WptrBefore, p722WptrAfter,
                    p722RptrBefore, p722RptrAfterKick, p722RptrFinal,
                    p722FenceBefore, p722FenceExpected,
                    p722FenceObserved, p722FencePolls,
                    static_cast<uint32_t>(p722FenceOk),
                    static_cast<uint32_t>(p722ResponseValid),
                    p722Status, p722FwAddrLo, p722FwAddrHi,
                    p722TransportError, static_cast<uint32_t>(p722Quarantined),
                    static_cast<uint32_t>(p722Accepted),
                    static_cast<uint32_t>(p722Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-mes-sched-data-runtime",
                          p722Names, p722Values,
                          sizeof(p722Values) / sizeof(p722Values[0]));

                // P7.23: Linux GC11 MES mes_kiq_code PSP type 81.
                // Stages MES firmware only; does not start GC or queues.
                uint32_t p723OptInRaw = 0u;
                const bool p723OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p723_mes_kiq_code",
                        &p723OptInRaw, sizeof(p723OptInRaw)) &&
                    p723OptInRaw == 1u;
                rn_navi33::p723_mes_kiq_code::Input p723Input{};
                p723Input.requested = p7StepArg >= 25u;
                p723Input.explicitOptIn = p723OptIn;
                p723Input.identityVerified = p722Input.identityVerified;
                p723Input.observedGcMajor = p722Input.observedGcMajor;
                p723Input.observedGcMinor = p722Input.observedGcMinor;
                p723Input.observedGcRevision = p722Input.observedGcRevision;
                p723Input.pspRingReady = p722Input.pspRingReady;
                p723Input.tmrAccepted = p74Accepted;
                p723Input.sdmaBothAccepted = p722Input.sdmaBothAccepted;
                p723Input.pfpAccepted = p77Accepted;
                p723Input.p0Accepted = p78Accepted;
                p723Input.p1Accepted = p79Accepted;
                p723Input.meAccepted = p710Accepted;
                p723Input.meP0Accepted = p711Accepted;
                p723Input.meP1Accepted = p712Accepted;
                p723Input.mecAccepted = p713Accepted;
                p723Input.priorStageAccepted = p722Accepted;
                p723Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p723Input.previousFenceExpected = p722FenceExpected;
                p723Input.previousFenceObserved = p722FenceObserved;
                p723Input.transportHealthy =
                    p722FenceOk && p722ResponseValid &&
                    p722TransportError == 0u &&
                    !p722Quarantined && !p7SubmitTransportQuarantined;
                p723Input.stageOwnerRetained = p722Input.stageOwnerRetained;
                p723Input.stageMapped = p722Input.stageMapped;
                p723Input.stagePteVerified = p722Input.stagePteVerified;
                p723Input.stageBytes = p722Input.stageBytes;
                p723Input.stageGpuVa = p722Input.stageGpuVa;
                p723Input.gartStart = p722Input.gartStart;
                p723Input.gartEnd = p722Input.gartEnd;
                // Independently pinned Linux MES_Kiq source; this
                // uses the MES-specific code or data bytes, never the
                // entire vendor firmware container or previous stage.
                uint32_t p723FwVersion = 0u;
#if defined(RN_HAVE_NAVI33_MES_FW) && \
    defined(RN_NAVI33_MES_FW_METADATA_V1) && \
    defined(RN_NAVI33_MES_FW_SOURCE_VERIFIED)
                p723Input.firmwarePresent =
                    kRNNavi33MesKiqCodeBytes > 0u;
                p723Input.firmwareMetadataVerified =
                    kRNNavi33MesKiqSourceBytes == 218400u &&
                    kRNNavi33MesKiqCodeBytes == 87072u &&
                    kRNNavi33MesKiqDataBytes == 131072u;
                p723Input.firmwareSourceVerified =
                    RN_NAVI33_MES_FW_SOURCE_VERIFIED == 1;
                p723Input.firmwareIpMajor = kRNNavi33MesIpMajor;
                p723Input.firmwareIpMinor = kRNNavi33MesIpMinor;
                p723Input.firmwareIpRevision = kRNNavi33MesIpRevision;
                p723Input.firmwareBytes = kRNNavi33MesKiqCodeBytes;
                p723FwVersion = kRNNavi33MesKiqVersion;
#endif
                const auto p723Gate =
                    rn_navi33::p723_mes_kiq_code::inspect(p723Input);

                bool p723FenceOk = false;
                bool p723Submitted = false;
                bool p723ResponseValid = false;
                bool p723Accepted = false;
                uint32_t p723Status = 0xffffffffu;
                uint32_t p723FwAddrLo = 0u, p723FwAddrHi = 0u;
                uint32_t p723TransportError = 0u;
                bool p723Quarantined = p7SubmitTransportQuarantined;
                uint32_t p723WptrBefore = 0u, p723WptrAfter = 0u;
                uint32_t p723RptrBefore = 0u, p723RptrAfterKick = 0u;
                uint32_t p723RptrFinal = 0u;
                uint32_t p723FenceBefore = 0u, p723FenceExpected = 0u;
                uint32_t p723FenceObserved = 0u, p723FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_MES_FW
                if (p723Gate.mayLoad &&
                    pspFence == rn_navi33::p723_mes_kiq_code::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33MesKiqCodeFw) ==
                        static_cast<size_t>(kRNNavi33MesKiqCodeBytes)) {
                    // MES mes_kiq_code image only; no RLC/GC/MES start.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33MesKiqCodeFw,
                           kRNNavi33MesKiqCodeBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p723FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p723_mes_kiq_code::kCommandId,
                        p723Gate.words,
                        rn_navi33::p723_mes_kiq_code::kArgumentCount,
                        resp, 4u);
                    p723Submitted = p7SubmitWasIssued;
                    p723ResponseValid = p7SubmitResponseValid;
                    p723TransportError = p7SubmitTransportError;
                    p723Quarantined = p7SubmitTransportQuarantined;
                    p723WptrBefore = p7SubmitWptrBefore;
                    p723WptrAfter = p7SubmitWptrAfter;
                    p723RptrBefore = p7SubmitRptrBefore;
                    p723RptrAfterKick = p7SubmitRptrAfterKick;
                    p723RptrFinal = p7SubmitRptrFinal;
                    p723FenceBefore = p7SubmitFenceBefore;
                    p723FenceExpected = p7SubmitFenceExpected;
                    p723FenceObserved = p7SubmitFenceObserved;
                    p723FencePolls = p7SubmitPolls;
                    if (p723ResponseValid) {
                        p723Status = resp[0];
                        p723FwAddrLo = resp[2];
                        p723FwAddrHi = resp[3];
                    }
                    p723Accepted = p723FenceOk && p723ResponseValid &&
                        p723Status == 0u &&
                        p723FenceExpected ==
                            rn_navi33::p723_mes_kiq_code::kExpectedFence &&
                        p723FenceObserved ==
                            rn_navi33::p723_mes_kiq_code::kExpectedFence;
                } else if (p723Gate.mayLoad) {
                    p723TransportError = 10u;
                }
#endif

                const char *const p723Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "prior_stage_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "mes_kiq_code_loaded",
                    "gc_engine_started"
                };
                const uint32_t p723Values[] = {
                    p7StepArg, static_cast<uint32_t>(p723Gate.error),
                    static_cast<uint32_t>(p723Input.requested),
                    static_cast<uint32_t>(p723OptIn),
                    static_cast<uint32_t>(p723Gate.mayLoad),
                    static_cast<uint32_t>(p723Input.firmwarePresent),
                    static_cast<uint32_t>(p723Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p723Input.firmwareSourceVerified),
                    p723Input.observedGcRevision,
                    p723Input.firmwareIpRevision, p723FwVersion,
                    static_cast<uint32_t>(p723Input.firmwareBytes),
                    static_cast<uint32_t>(p723Input.stageBytes),
                    static_cast<uint32_t>(p723Input.stageMapped),
                    static_cast<uint32_t>(p723Input.stagePteVerified),
                    static_cast<uint32_t>(p723Input.stageGpuVa),
                    static_cast<uint32_t>(p723Input.stageGpuVa >> 32),
                    rn_navi33::p723_mes_kiq_code::kCommandId,
                    rn_navi33::p723_mes_kiq_code::kFirmwareType,
                    p723Input.previousFenceExpected,
                    p723Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p722Accepted),
                    static_cast<uint32_t>(p723Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p723Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p723Submitted),
                    p723WptrBefore, p723WptrAfter,
                    p723RptrBefore, p723RptrAfterKick, p723RptrFinal,
                    p723FenceBefore, p723FenceExpected,
                    p723FenceObserved, p723FencePolls,
                    static_cast<uint32_t>(p723FenceOk),
                    static_cast<uint32_t>(p723ResponseValid),
                    p723Status, p723FwAddrLo, p723FwAddrHi,
                    p723TransportError, static_cast<uint32_t>(p723Quarantined),
                    static_cast<uint32_t>(p723Accepted),
                    static_cast<uint32_t>(p723Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-mes-kiq-code-runtime",
                          p723Names, p723Values,
                          sizeof(p723Values) / sizeof(p723Values[0]));

                // P7.24: Linux GC11 MES mes_kiq_data PSP type 82.
                // Stages MES firmware only; does not start GC or queues.
                uint32_t p724OptInRaw = 0u;
                const bool p724OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p724_mes_kiq_data",
                        &p724OptInRaw, sizeof(p724OptInRaw)) &&
                    p724OptInRaw == 1u;
                rn_navi33::p724_mes_kiq_data::Input p724Input{};
                p724Input.requested = p7StepArg >= 26u;
                p724Input.explicitOptIn = p724OptIn;
                p724Input.identityVerified = p723Input.identityVerified;
                p724Input.observedGcMajor = p723Input.observedGcMajor;
                p724Input.observedGcMinor = p723Input.observedGcMinor;
                p724Input.observedGcRevision = p723Input.observedGcRevision;
                p724Input.pspRingReady = p723Input.pspRingReady;
                p724Input.tmrAccepted = p74Accepted;
                p724Input.sdmaBothAccepted = p723Input.sdmaBothAccepted;
                p724Input.pfpAccepted = p77Accepted;
                p724Input.p0Accepted = p78Accepted;
                p724Input.p1Accepted = p79Accepted;
                p724Input.meAccepted = p710Accepted;
                p724Input.meP0Accepted = p711Accepted;
                p724Input.meP1Accepted = p712Accepted;
                p724Input.mecAccepted = p713Accepted;
                p724Input.priorStageAccepted = p723Accepted;
                p724Input.stageExpansionVerified =
                    p713StagePlan.mayExpand && p73StagePrepared &&
                    p73StageMapped && p73StagePteVerified &&
                    navi33PmfwStageBytes == p713StagePlan.targetStageBytes;
                p724Input.previousFenceExpected = p723FenceExpected;
                p724Input.previousFenceObserved = p723FenceObserved;
                p724Input.transportHealthy =
                    p723FenceOk && p723ResponseValid &&
                    p723TransportError == 0u &&
                    !p723Quarantined && !p7SubmitTransportQuarantined;
                p724Input.stageOwnerRetained = p723Input.stageOwnerRetained;
                p724Input.stageMapped = p723Input.stageMapped;
                p724Input.stagePteVerified = p723Input.stagePteVerified;
                p724Input.stageBytes = p723Input.stageBytes;
                p724Input.stageGpuVa = p723Input.stageGpuVa;
                p724Input.gartStart = p723Input.gartStart;
                p724Input.gartEnd = p723Input.gartEnd;
                // Independently pinned Linux MES_Kiq source; this
                // uses the MES-specific code or data bytes, never the
                // entire vendor firmware container or previous stage.
                uint32_t p724FwVersion = 0u;
#if defined(RN_HAVE_NAVI33_MES_FW) && \
    defined(RN_NAVI33_MES_FW_METADATA_V1) && \
    defined(RN_NAVI33_MES_FW_SOURCE_VERIFIED)
                p724Input.firmwarePresent =
                    kRNNavi33MesKiqDataBytes > 0u;
                p724Input.firmwareMetadataVerified =
                    kRNNavi33MesKiqSourceBytes == 218400u &&
                    kRNNavi33MesKiqCodeBytes == 87072u &&
                    kRNNavi33MesKiqDataBytes == 131072u;
                p724Input.firmwareSourceVerified =
                    RN_NAVI33_MES_FW_SOURCE_VERIFIED == 1;
                p724Input.firmwareIpMajor = kRNNavi33MesIpMajor;
                p724Input.firmwareIpMinor = kRNNavi33MesIpMinor;
                p724Input.firmwareIpRevision = kRNNavi33MesIpRevision;
                p724Input.firmwareBytes = kRNNavi33MesKiqDataBytes;
                p724FwVersion = kRNNavi33MesKiqVersion;
#endif
                const auto p724Gate =
                    rn_navi33::p724_mes_kiq_data::inspect(p724Input);

                bool p724FenceOk = false;
                bool p724Submitted = false;
                bool p724ResponseValid = false;
                bool p724Accepted = false;
                uint32_t p724Status = 0xffffffffu;
                uint32_t p724FwAddrLo = 0u, p724FwAddrHi = 0u;
                uint32_t p724TransportError = 0u;
                bool p724Quarantined = p7SubmitTransportQuarantined;
                uint32_t p724WptrBefore = 0u, p724WptrAfter = 0u;
                uint32_t p724RptrBefore = 0u, p724RptrAfterKick = 0u;
                uint32_t p724RptrFinal = 0u;
                uint32_t p724FenceBefore = 0u, p724FenceExpected = 0u;
                uint32_t p724FenceObserved = 0u, p724FencePolls = 0u;

#ifdef RN_HAVE_NAVI33_MES_FW
                if (p724Gate.mayLoad &&
                    pspFence == rn_navi33::p724_mes_kiq_data::kPriorFence &&
                    !p7SubmitTransportQuarantined &&
                    navi33PmfwBuf && navi33PmfwBuf->getBytesNoCopy() &&
                    sizeof(kRNNavi33MesKiqDataFw) ==
                        static_cast<size_t>(kRNNavi33MesKiqDataBytes)) {
                    // MES mes_kiq_data image only; no RLC/GC/MES start.
                    uint8_t *const stage = static_cast<uint8_t *>(
                        navi33PmfwBuf->getBytesNoCopy());
                    memset(stage, 0, navi33PmfwStageBytes);
                    memcpy(stage, kRNNavi33MesKiqDataFw,
                           kRNNavi33MesKiqDataBytes);
                    __sync_synchronize();

                    uint32_t resp[4] = {
                        0xffffffffu, 0xffffffffu,
                        0xffffffffu, 0xffffffffu
                    };
                    p724FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p724_mes_kiq_data::kCommandId,
                        p724Gate.words,
                        rn_navi33::p724_mes_kiq_data::kArgumentCount,
                        resp, 4u);
                    p724Submitted = p7SubmitWasIssued;
                    p724ResponseValid = p7SubmitResponseValid;
                    p724TransportError = p7SubmitTransportError;
                    p724Quarantined = p7SubmitTransportQuarantined;
                    p724WptrBefore = p7SubmitWptrBefore;
                    p724WptrAfter = p7SubmitWptrAfter;
                    p724RptrBefore = p7SubmitRptrBefore;
                    p724RptrAfterKick = p7SubmitRptrAfterKick;
                    p724RptrFinal = p7SubmitRptrFinal;
                    p724FenceBefore = p7SubmitFenceBefore;
                    p724FenceExpected = p7SubmitFenceExpected;
                    p724FenceObserved = p7SubmitFenceObserved;
                    p724FencePolls = p7SubmitPolls;
                    if (p724ResponseValid) {
                        p724Status = resp[0];
                        p724FwAddrLo = resp[2];
                        p724FwAddrHi = resp[3];
                    }
                    p724Accepted = p724FenceOk && p724ResponseValid &&
                        p724Status == 0u &&
                        p724FenceExpected ==
                            rn_navi33::p724_mes_kiq_data::kExpectedFence &&
                        p724FenceObserved ==
                            rn_navi33::p724_mes_kiq_data::kExpectedFence;
                } else if (p724Gate.mayLoad) {
                    p724TransportError = 10u;
                }
#endif

                const char *const p724Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "may_load", "firmware_present", "metadata_verified",
                    "source_verified", "observed_gc_revision",
                    "firmware_gc_revision", "firmware_version",
                    "firmware_bytes", "stage_capacity", "stage_mapped",
                    "stage_pte_verified", "stage_va_lo", "stage_va_hi",
                    "command_id", "fw_type", "prior_fence_expected",
                    "prior_fence_observed", "pfp_accepted",
                    "p0_accepted", "p1_accepted", "me_accepted",
                    "me_p0_accepted", "me_p1_accepted", "mec_accepted",
                    "prior_stage_accepted", "stage_expansion_verified",
                    "stage_expand_opt_in",
                    "stage_plan_error", "sdma_both_accepted", "submitted",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "fence_ok",
                    "response_valid", "status", "fw_addr_lo", "fw_addr_hi",
                    "transport_error", "transport_quarantined",
                    "accepted", "mes_kiq_data_loaded",
                    "gc_engine_started"
                };
                const uint32_t p724Values[] = {
                    p7StepArg, static_cast<uint32_t>(p724Gate.error),
                    static_cast<uint32_t>(p724Input.requested),
                    static_cast<uint32_t>(p724OptIn),
                    static_cast<uint32_t>(p724Gate.mayLoad),
                    static_cast<uint32_t>(p724Input.firmwarePresent),
                    static_cast<uint32_t>(p724Input.firmwareMetadataVerified),
                    static_cast<uint32_t>(p724Input.firmwareSourceVerified),
                    p724Input.observedGcRevision,
                    p724Input.firmwareIpRevision, p724FwVersion,
                    static_cast<uint32_t>(p724Input.firmwareBytes),
                    static_cast<uint32_t>(p724Input.stageBytes),
                    static_cast<uint32_t>(p724Input.stageMapped),
                    static_cast<uint32_t>(p724Input.stagePteVerified),
                    static_cast<uint32_t>(p724Input.stageGpuVa),
                    static_cast<uint32_t>(p724Input.stageGpuVa >> 32),
                    rn_navi33::p724_mes_kiq_data::kCommandId,
                    rn_navi33::p724_mes_kiq_data::kFirmwareType,
                    p724Input.previousFenceExpected,
                    p724Input.previousFenceObserved,
                    static_cast<uint32_t>(p77Accepted),
                    static_cast<uint32_t>(p78Accepted),
                    static_cast<uint32_t>(p79Accepted),
                    static_cast<uint32_t>(p710Accepted),
                    static_cast<uint32_t>(p711Accepted),
                    static_cast<uint32_t>(p712Accepted),
                    static_cast<uint32_t>(p713Accepted),
                    static_cast<uint32_t>(p723Accepted),
                    static_cast<uint32_t>(p724Input.stageExpansionVerified),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p724Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p724Submitted),
                    p724WptrBefore, p724WptrAfter,
                    p724RptrBefore, p724RptrAfterKick, p724RptrFinal,
                    p724FenceBefore, p724FenceExpected,
                    p724FenceObserved, p724FencePolls,
                    static_cast<uint32_t>(p724FenceOk),
                    static_cast<uint32_t>(p724ResponseValid),
                    p724Status, p724FwAddrLo, p724FwAddrHi,
                    p724TransportError, static_cast<uint32_t>(p724Quarantined),
                    static_cast<uint32_t>(p724Accepted),
                    static_cast<uint32_t>(p724Accepted),
                    0u
                };
                rnPublish(this, "navi33-p7-mes-kiq-data-runtime",
                          p724Names, p724Values,
                          sizeof(p724Values) / sizeof(p724Values[0]));
                // P7.25: the first command capable of starting the
                // RLC/IMU engines, NOT a firmware-fence-only checkpoint.
                // Explicit firmware and ring prerequisites; actual PSP
                // hardware command additionally requires a compile-time
                // unsafe gate and a separately consented boot argument.
                uint32_t p725OptInRaw = 0u;
                const bool p725OptIn =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p725_rlc_autoload",
                        &p725OptInRaw, sizeof(p725OptInRaw)) &&
                    p725OptInRaw == 1u;
                rn_navi33::p725_rlc_autoload::Input p725Input{};
                p725Input.requested = p7StepArg >= 27u;
                p725Input.explicitOptIn = p725OptIn;
                p725Input.asicVerified = p724Input.identityVerified;
                p725Input.gcMajor = p724Input.observedGcMajor;
                p725Input.gcMinor = p724Input.observedGcMinor;
                p725Input.gcRevision = p724Input.observedGcRevision;
                p725Input.pspRingReady = p724Input.pspRingReady;
                p725Input.tmrAccepted = p74Accepted;
                p725Input.pmfwAccepted = p73PmfwAccepted;
                p725Input.sdmaBothAccepted = p75Accepted && p76Accepted;
                p725Input.pfpMeMecAccepted =
                    p77Accepted && p78Accepted && p79Accepted &&
                    p710Accepted && p711Accepted && p712Accepted &&
                    p713Accepted && p714Accepted && p715Accepted &&
                    p716Accepted && p717Accepted;
                p725Input.rlcGAccepted = p718Accepted;
                p725Input.imuIAccepted = p719Accepted;
                p725Input.imuDAccepted = p720Accepted;
                p725Input.mesSchedulerCodeAccepted = p721Accepted;
                p725Input.mesSchedulerDataAccepted = p722Accepted;
                p725Input.mesKiqCodeAccepted = p723Accepted;
                p725Input.mesKiqDataAccepted = p724Accepted;
                p725Input.previousFenceExpected = p724FenceExpected;
                p725Input.previousFenceObserved = p724FenceObserved;
                p725Input.transportHealthy =
                    p724FenceOk && p724ResponseValid &&
                    p724TransportError == 0u && !p724Quarantined &&
                    !p7SubmitTransportQuarantined &&
                    // An already-acknowledged PSP fence can precede the
                    // RPTR register update. For actual RLC execution
                    // demand an idle ring at the prior snapshot.
                    p724RptrFinal == p724WptrAfter;
                p725Input.runtimeOwnerRetained =
                    navi33PspRuntimePrepared && navi33PspRuntimeBuf &&
                    navi33PspRuntimeBuf->getBytesNoCopy();
                // Read-only RLC preflight. Current PSP firmware loading
                // order is not Linux-equivalent (RLC_G precedes IMU/MES)
                // and no owned SMU command/response has been validated.
                // Neither a stale SMU response=1 nor PSP firmware acceptance
                // can authorize the first engine-starting PSP command.
                uint32_t p725Gc0Base = 0u, p725Gc1Base = 0u;
                uint32_t p725Mp1Base = 0u;
                const bool p725Gc0Found =
                    ipBase(kRNHwIdGc, &p725Gc0Base, 0);
                const bool p725Gc1Found =
                    ipBase(kRNHwIdGc, &p725Gc1Base, 1);
                const bool p725Mp1Found =
                    ipBase(kRNHwIdMp1, &p725Mp1Base, 0);
                rn_navi33::rlc_readonly_preflight::Input p725PreInput{};
                p725PreInput.requested = p7StepArg >= 27u;
                p725PreInput.basesDiscovered =
                    p725Gc0Found && p725Gc1Found && p725Mp1Found;
                p725PreInput.gc0Base = p725Gc0Base;
                p725PreInput.gc1Base = p725Gc1Base;
                p725PreInput.mp1Base = p725Mp1Base;
                p725PreInput.bar5Bytes = regs->getLength();
                p725PreInput.pmfwAccepted = p73PmfwAccepted;
                // Current MMIO read-only snapshot is NOT a live handshake.
                p725PreInput.smuHandshakeVerified = false;
                p725PreInput.rlcGFence = p718FenceExpected;
                p725PreInput.imuIFence = p719FenceExpected;
                p725PreInput.imuDFence = p720FenceExpected;
                p725PreInput.mesSchedCodeFence = p721FenceExpected;
                p725PreInput.mesSchedDataFence = p722FenceExpected;
                p725PreInput.mesKiqCodeFence = p723FenceExpected;
                p725PreInput.mesKiqDataFence = p724FenceExpected;

                // Derive all byte offsets from discovered IP bases and
                // bounded BAR5 length BEFORE accessing the registers.
                const auto p725AddressPlan =
                    rn_navi33::rlc_readonly_preflight::inspect(p725PreInput);
                if (p725PreInput.requested &&
                    p725AddressPlan.gcAddressesValid && p724Accepted) {
                    p725PreInput.gcSamplesRead =
                        readBar(p725Gc0Base,
                            rn_navi33::rlc_readonly_preflight::kCpStatDword,
                            &p725PreInput.cpStat) &&
                        readBar(p725Gc1Base,
                            rn_navi33::rlc_readonly_preflight::kRlcBootloadDword,
                            &p725PreInput.rlcBootloadStatus) &&
                        readBar(p725Gc1Base,
                            rn_navi33::rlc_readonly_preflight::kImuScratchDword,
                            &p725PreInput.imuScratch0);
                    p725PreInput.smuMailboxRead = readBar(p725Mp1Base,
                        rn_navi33::rlc_readonly_preflight::kSmuResponseDword,
                        &p725PreInput.smuResponse);
                }
                const auto p725Preflight =
                    rn_navi33::rlc_readonly_preflight::inspect(p725PreInput);
                p725Input.readOnlyPreflightPassed = p725Preflight.mayAutoload;
                const char *const p725PreNames[] = {
                    "step", "blocker", "requested",
                    "gc0_base_dword", "gc1_base_dword", "mp1_base_dword",
                    "bar5_bytes_lo", "bar5_bytes_hi",
                    "gc_addresses_verified", "gc_snapshot_readable",
                    "cp_stat_byte_offset", "rlc_bootload_byte_offset",
                    "imu_scratch_byte_offset", "smu_response_byte_offset",
                    "cp_stat_readback", "rlc_bootload_readback",
                    "imu_scratch0_readback", "smu_response_readback",
                    "pmfw_firmware_accepted", "smu_mailbox_readable",
                    "smu_handshake_verified", "firmware_order_matches_linux",
                    "rlc_g_fence", "imu_i_fence", "imu_d_fence",
                    "mes_sched_code_fence", "mes_sched_data_fence",
                    "mes_kiq_code_fence", "mes_kiq_data_fence",
                    "may_autoload"
                };
                const uint32_t p725PreValues[] = {
                    p7StepArg, static_cast<uint32_t>(p725Preflight.error),
                    static_cast<uint32_t>(p725PreInput.requested),
                    p725Gc0Base, p725Gc1Base, p725Mp1Base,
                    static_cast<uint32_t>(p725PreInput.bar5Bytes),
                    static_cast<uint32_t>(p725PreInput.bar5Bytes >> 32),
                    static_cast<uint32_t>(p725Preflight.gcAddressesValid),
                    static_cast<uint32_t>(p725Preflight.gcSnapshotReadable),
                    p725Preflight.cpStatByteOffset,
                    p725Preflight.rlcBootloadByteOffset,
                    p725Preflight.imuScratchByteOffset,
                    p725Preflight.smuResponseByteOffset,
                    p725PreInput.cpStat, p725PreInput.rlcBootloadStatus,
                    p725PreInput.imuScratch0, p725PreInput.smuResponse,
                    static_cast<uint32_t>(p725Preflight.pmfwAccepted),
                    static_cast<uint32_t>(p725Preflight.smuMailboxReadable),
                    static_cast<uint32_t>(p725Preflight.smuHandshakeVerified),
                    static_cast<uint32_t>(p725Preflight.linuxFirmwareOrder),
                    p718FenceExpected, p719FenceExpected, p720FenceExpected,
                    p721FenceExpected, p722FenceExpected,
                    p723FenceExpected, p724FenceExpected,
                    static_cast<uint32_t>(p725Preflight.mayAutoload)
                };
                rnPublish(this, "navi33-p7-rlc-preflight",
                    p725PreNames, p725PreValues,
                    sizeof(p725PreValues) / sizeof(p725PreValues[0]));

                // Opt-in MP1 firmware-flags SMN read. Unlike the
                // BAR5 C2PMSG mailbox, this uses NBIF RSMU index/data.
                // Index selection is an MMIO WRITE, so it is OFF by
                // default, even though the target firmware flag is
                // read-only. This is NOT an SMU GetSmuVersion message,
                // does not enable autoload, and does not claim a live
                // SMU command/response handshake.
                uint32_t p726OptInArg = 0u;
                const bool p726OptInLongPresent =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p726_mp1_fwflags",
                        &p726OptInArg, sizeof(p726OptInArg));
                uint32_t p726OptInShortArg = 0u;
                const bool p726OptInShortPresent =
                    PE_parse_boot_argn(
                        rn_navi33::mp1_fwflags_smn_probe::
                            kShortFwFlagsBootArg,
                        &p726OptInShortArg, sizeof(p726OptInShortArg));
                const bool p726OptIn =
                    rn_navi33::mp1_fwflags_smn_probe::optInFromEither(
                        p726OptInLongPresent, p726OptInArg,
                        p726OptInShortPresent, p726OptInShortArg);

                uint32_t p726Pf0OptInArg = 0u;
                const bool p726Pf0LongPresent =
                    PE_parse_boot_argn(
                        "rednavi_navi33_p726_nbio43_pf0",
                        &p726Pf0OptInArg, sizeof(p726Pf0OptInArg));
                uint32_t p726Pf0ShortArg = 0u;
                const bool p726Pf0ShortPresent =
                    PE_parse_boot_argn(
                        rn_navi33::mp1_fwflags_smn_probe::
                            kShortPf0BootArg,
                        &p726Pf0ShortArg, sizeof(p726Pf0ShortArg));
                const bool p726Pf0OptIn =
                    rn_navi33::mp1_fwflags_smn_probe::optInFromEither(
                        p726Pf0LongPresent, p726Pf0OptInArg,
                        p726Pf0ShortPresent, p726Pf0ShortArg);
                uint32_t p726NbifBase = 0u;
                const bool p726NbifPresent =
                    ipBase(108, &p726NbifBase, 1);
                rn_navi33::mp1_fwflags_smn_probe::Input p726Input{};
                p726Input.requested = p7StepArg >= 27u;
                p726Input.explicitOptIn = p726OptIn;
                p726Input.pf0ExplicitOptIn = p726Pf0OptIn;
                p726Input.step = p7StepArg;
                p726Input.exactNavi33 = p724Input.identityVerified &&
                    p724Input.observedGcMajor == 11u &&
                    p724Input.observedGcMinor == 0u &&
                    p724Input.observedGcRevision == 2u;
                p726Input.mp1Revision = p73HardwareMp1Revision;
                // Live 0.0.42 RX7600 IP Discovery reports NBIO 4.3.1,
                // not 7.11. Linux nbio_v4_3_funcs maps PF0 RSMU
                // index/data to NBIF segment 1 DWORD 0/1. Fail
                // closed on another layout despite valid BAR5 bounds.
                p726Input.nbioMajor = check.nbio.major;
                p726Input.nbioMinor = check.nbio.minor;
                p726Input.nbioRevision = check.nbio.revision;
                p726Input.pmfwAccepted = p73PmfwAccepted;
                p726Input.priorFenceExpected = p724FenceExpected;
                p726Input.priorFenceObserved = p724FenceObserved;
                p726Input.transportHealthy =
                    p724Accepted && !p7SubmitTransportQuarantined;
                p726Input.nbifBaseFound = p726NbifPresent;
                p726Input.nbifBaseDword = p726NbifBase;
                p726Input.bar5Bytes = regs->getLength();
                const auto p726Gate =
                    rn_navi33::mp1_fwflags_smn_probe::inspect(p726Input);
                // Share gpu.mmio_idx_lock with compat rn_rreg32/rn_wreg32.
                // Do not sleep or call indirect helpers inside this lock.
                // Read-only by default: the helper refuses all MMIO unless
                // BOTH explicit P7.26 boot-args pass the admission gate.
                const auto p726Trace =
                    rn_navi33::mp1_fwflags_smn_probe::sampleSerialized(
                        p726Gate,
                        [&]() { spin_lock(&gpu.mmio_idx_lock); },
                        [&]() { spin_unlock(&gpu.mmio_idx_lock); },
                        [&](uint32_t offset) { return rd(offset); },
                        [&](uint32_t offset, uint32_t value) {
                            wr(offset, value);
                        });
                const bool p726IndexRestored =
                    p726Trace.originalIndex != 0xffffffffu &&
                    p726Trace.restoreWritten &&
                    p726Trace.restoredIndex == p726Trace.originalIndex;
                const bool p726ValueValid =
                    rn_navi33::mp1_fwflags_smn_probe::
                        validTranscript(p726Trace);
                const char *const p726Names[] = {
                    "step", "requested", "explicit_opt_in",
                    "pf0_explicit_opt_in", "gate_error", "may_probe",
                    "nbif_found", "nbif_base_dword",
                    "nbio_major", "nbio_minor", "nbio_revision",
                    "nbio_pf0_layout_supported",
                    "nbio_pf1_layout_supported",
                    "bar5_bytes", "smn_address",
                    "index_byte_offset", "data_byte_offset",
                    "index_before", "index_written",
                    "index_selected_readback",
                    "index_selection_verified",
                    "data_read", "restore_written", "index_after",
                    "index_restored", "fw_flags",
                    "snapshot_valid", "interrupts_enabled",
                    "smu_handshake_verified", "smu_command_sent",
                    "rlc_autoload_submitted"
                };
                const uint32_t p726Values[] = {
                    p7StepArg, static_cast<uint32_t>(
                        p726Input.requested),
                    static_cast<uint32_t>(p726OptIn),
                    static_cast<uint32_t>(p726Pf0OptIn),
                    static_cast<uint32_t>(p726Gate.error),
                    static_cast<uint32_t>(p726Gate.mayProbe),
                    static_cast<uint32_t>(p726NbifPresent),
                    p726NbifBase,
                    p726Input.nbioMajor, p726Input.nbioMinor,
                    p726Input.nbioRevision,
                    static_cast<uint32_t>(
                        rn_navi33::mp1_fwflags_smn_probe::
                            supportedPf0RsmuVersion(
                                p726Input.nbioMajor, p726Input.nbioMinor,
                                p726Input.nbioRevision)),
                    static_cast<uint32_t>(
                        rn_navi33::mp1_fwflags_smn_probe::
                            supportedPf1RsmuVersion(
                                p726Input.nbioMajor, p726Input.nbioMinor,
                                p726Input.nbioRevision)),
                    static_cast<uint32_t>(p726Input.bar5Bytes),
                    rn_navi33::mp1_fwflags_smn_probe::
                        kSmnMp1FirmwareFlags,
                    p726Gate.indexByteOffset, p726Gate.dataByteOffset,
                    p726Trace.originalIndex, static_cast<uint32_t>(
                        p726Trace.indexWritten),
                    p726Trace.selectedReadback,
                    static_cast<uint32_t>(
                        p726Trace.selectedReadback ==
                        rn_navi33::mp1_fwflags_smn_probe::
                            kSmnMp1FirmwareFlags &&
                        p726Trace.indexWritten),
                    static_cast<uint32_t>(p726Trace.dataRead),
                    static_cast<uint32_t>(p726Trace.restoreWritten),
                    p726Trace.restoredIndex,
                    static_cast<uint32_t>(p726IndexRestored),
                    p726Trace.firmwareFlags,
                    static_cast<uint32_t>(p726ValueValid),
                    static_cast<uint32_t>(
                        rn_navi33::mp1_fwflags_smn_probe::
                            interruptsEnabled(p726Trace.firmwareFlags,
                                              p726ValueValid)),
                    0u, 0u, 0u
                };
                rnPublish(this, "navi33-p7-mp1-fwflags",
                    p726Names, p726Values,
                    sizeof(p726Values) / sizeof(p726Values[0]));

                // P7.27: prepare a bounded one-shot SMU GetSmuVersion
                // transcript. No hardware mailbox lease has yet been
                // proven. Merely owning the IOPCIDevice, mapping BAR5,
                // or reading response=1 is NOT such a lease. Fail
                // closed on real hardware even if unsafe build and
                // explicit opt-in are enabled. P7.25 RLC gates remain
                // independent and must not consume this synthetic plan.
                namespace p727a = rn_navi33::smu_mailbox_admission;
                namespace p727r = rn_navi33::smu_mailbox_roundtrip;
                uint32_t p727OptInRaw = 0u;
                const bool p727OptIn =
                    PE_parse_boot_argn("rn727smu", &p727OptInRaw,
                                       sizeof(p727OptInRaw)) &&
                    p727OptInRaw == 1u;
#ifdef RN_ENABLE_NAVI33_SMU_MAILBOX_UNSAFE
                constexpr bool p727UnsafeBuild = true;
#else
                constexpr bool p727UnsafeBuild = false;
#endif
                const auto p727Registers =
                    p727r::plan(p725Mp1Found ? p725Mp1Base : 0u,
                                regs->getLength());
                // The exact host + firmware MP1 ownership contract is
                // currently unimplemented. It must be demonstrated by
                // independent device arbitration before changing this.
                constexpr bool p727MailboxLeaseHeld = false;
                p727a::Input p727Input{};
                p727Input.requested = p7StepArg >= 27u;
                p727Input.explicitOptIn = p727OptIn;
                p727Input.unsafeBuildEnabled = p727UnsafeBuild;
                p727Input.exactNavi33 = p726Input.exactNavi33;
                p727Input.mp1Major = check.observed.smu.major;
                p727Input.mp1Minor = check.observed.smu.minor;
                p727Input.mp1Revision = check.observed.smu.revision;
                p727Input.mailboxLeaseHeld = p727MailboxLeaseHeld;
                p727Input.pspReady = p724Input.pspRingReady;
                p727Input.pmfwAccepted = p73PmfwAccepted;
                p727Input.priorFenceExpected = p724FenceExpected;
                p727Input.priorFenceObserved = p724FenceObserved;
                p727Input.pspTransportHealthy = p725Input.transportHealthy;
                p727Input.addressesVerified =
                    p725Mp1Found && p727Registers.valid;
                p727Input.responseBefore =
                    p725PreInput.smuMailboxRead
                    ? p725PreInput.smuResponse : p727a::kInvalid;
                const auto p727Admission = p727a::inspect(p727Input);
                bool p727OneShotConsumed = false;
                const auto p727Roundtrip = p727r::execute(
                    p727Admission, p727Registers, p727MailboxLeaseHeld,
                    p727OneShotConsumed,
                    [&](uint32_t offset) { return rd(offset); },
                    [&](uint32_t offset, uint32_t value) {
                        wr(offset, value);
                    },
                    [&]() { IODelay(p727r::kPauseMicroseconds); });
                const char *const p727Names[] = {
                    "step", "requested", "explicit_opt_in",
                    "unsafe_build_enabled", "mailbox_lease_held",
                    "admission_error", "may_submit",
                    "mp1_base_dword", "bar5_bytes",
                    "registers_verified", "message_byte_offset",
                    "parameter_byte_offset", "response_byte_offset",
                    "response_before", "attempted",
                    "response_clear_written", "parameter_clear_written",
                    "response_zero_observed", "command_sent",
                    "response_transition_observed", "response_after",
                    "version_read", "version", "polls", "timed_out",
                    "transaction_error", "handshake_verified",
                    "lease_quarantined"
                };
                const uint32_t p727Values[] = {
                    p7StepArg, static_cast<uint32_t>(p727Input.requested),
                    static_cast<uint32_t>(p727OptIn),
                    static_cast<uint32_t>(p727UnsafeBuild),
                    static_cast<uint32_t>(p727MailboxLeaseHeld),
                    static_cast<uint32_t>(p727Admission.error),
                    static_cast<uint32_t>(p727Admission.maySubmit),
                    p725Mp1Base,
                    static_cast<uint32_t>(regs->getLength()),
                    static_cast<uint32_t>(p727Registers.valid),
                    p727Registers.messageByteOffset,
                    p727Registers.parameterByteOffset,
                    p727Registers.responseByteOffset,
                    p727Input.responseBefore,
                    static_cast<uint32_t>(p727Roundtrip.attempted),
                    static_cast<uint32_t>(p727Roundtrip.responseClearWritten),
                    static_cast<uint32_t>(p727Roundtrip.parameterClearWritten),
                    static_cast<uint32_t>(
                        p727Roundtrip.transcript.responseClearedToZeroObserved),
                    static_cast<uint32_t>(
                        p727Roundtrip.transcript.commandSubmitted),
                    static_cast<uint32_t>(
                        p727Roundtrip.transcript.responseTransitionObserved),
                    p727Roundtrip.transcript.responseAfter,
                    static_cast<uint32_t>(
                        p727Roundtrip.transcript.argumentRead),
                    p727Roundtrip.transcript.versionResult,
                    p727Roundtrip.transcript.polls,
                    static_cast<uint32_t>(p727Roundtrip.transcript.timedOut),
                    static_cast<uint32_t>(p727Roundtrip.error),
                    static_cast<uint32_t>(p727Roundtrip.verified),
                    static_cast<uint32_t>(p727Roundtrip.quarantineLease)
                };
                rnPublish(this, "navi33-p7-smu-roundtrip",
                    p727Names, p727Values,
                    sizeof(p727Values) / sizeof(p727Values[0]));

                const auto p725Gate =
                    rn_navi33::p725_rlc_autoload::inspect(p725Input);

                uint32_t p725Compiled = 0u;
#ifdef RN_ENABLE_NAVI33_RLC_AUTOLOAD_UNSAFE
                p725Compiled = 1u;
#endif
                bool p725FenceOk = false;
                bool p725Submitted = false;
                bool p725ResponseValid = false;
                bool p725Accepted = false;
                bool p725BootComplete = false;
                uint32_t p725Status = 0xffffffffu;
                uint32_t p725TransportError = 0u;
                bool p725Quarantined = p7SubmitTransportQuarantined;
                uint32_t p725WptrBefore = 0u, p725WptrAfter = 0u;
                uint32_t p725RptrBefore = 0u, p725RptrAfterKick = 0u;
                uint32_t p725RptrFinal = 0u;
                uint32_t p725FenceBefore = 0u, p725FenceExpected = 0u;
                uint32_t p725FenceObserved = 0u, p725FencePolls = 0u;
                uint32_t p725CpStat = 0xffffffffu;
                uint32_t p725BootloadStatus = 0xffffffffu;
                uint32_t p725ImuScratch0 = 0xffffffffu;
                uint32_t p725BootPolls = 0u;
                bool p725GcRegisterAddressesVerified =
                    p725Preflight.gcAddressesValid;
#ifdef RN_ENABLE_NAVI33_RLC_AUTOLOAD_UNSAFE
                if (p725Gate.mayAutoload &&
                    pspFence ==
                        rn_navi33::p725_rlc_autoload::kPriorFence &&
                    !p7SubmitTransportQuarantined) {
                    // Linux psp_rlc_autoload_start(): empty input payload,
                    // GFX_CMD_ID_AUTOLOAD_RLC = 0x21. PSP fence 24 is a
                    // command acknowledgement; NEVER call it "RLC ready".
                    uint32_t resp[1] = {0xffffffffu};
                    p725FenceOk = pspSubmitP7(
                        p71RingGpuVa, p71CmdGpuVa, p71FenceGpuVa,
                        rn_navi33::p725_rlc_autoload::kCommandId,
                        nullptr, 0u, resp, 1u);
                    p725Submitted = p7SubmitWasIssued;
                    p725ResponseValid = p7SubmitResponseValid;
                    p725TransportError = p7SubmitTransportError;
                    p725Quarantined = p7SubmitTransportQuarantined;
                    p725WptrBefore = p7SubmitWptrBefore;
                    p725WptrAfter = p7SubmitWptrAfter;
                    p725RptrBefore = p7SubmitRptrBefore;
                    p725RptrAfterKick = p7SubmitRptrAfterKick;
                    p725RptrFinal = p7SubmitRptrFinal;
                    p725FenceBefore = p7SubmitFenceBefore;
                    p725FenceExpected = p7SubmitFenceExpected;
                    p725FenceObserved = p7SubmitFenceObserved;
                    p725FencePolls = p7SubmitPolls;
                    if (p725ResponseValid)
                        p725Status = resp[0];
                    p725Accepted = p725FenceOk && p725ResponseValid &&
                        p725Status == 0u &&
                        p725FenceExpected ==
                            rn_navi33::p725_rlc_autoload::kExpectedFence &&
                        p725FenceObserved ==
                            rn_navi33::p725_rlc_autoload::kExpectedFence;
                    // Read-only Linux GC11 boot-complete verification;
                    // no golden register, GFXHUB, doorbell, CP or queue
                    // writes. Bound polling to 250 ms on an already
                    // PSP-acknowledged command; no busy indefinite loop.
                    if (p725Accepted && p725GcRegisterAddressesVerified) {
                        // Reuse *prevalidated* BAR5 byte offsets, never
                        // unchecked base + register arithmetic here.
                        for (uint32_t sample = 0; sample < 250u;
                             ++sample) {
                            p725CpStat =
                                rd(p725Preflight.cpStatByteOffset);
                            p725BootloadStatus =
                                rd(p725Preflight.rlcBootloadByteOffset);
                            p725BootPolls = sample + 1u;
                            if (rn_navi33::p725_rlc_autoload::
                                hardwareBootComplete(
                                    p725CpStat, p725BootloadStatus)) {
                                p725BootComplete = true;
                                break;
                            }
                            IOSleep(1);
                        }
                        p725ImuScratch0 =
                            rd(p725Preflight.imuScratchByteOffset);
                    }
                } else if (p725Gate.mayAutoload) {
                    p725TransportError = 10u;
                }
#endif

                const char *const p725Names[] = {
                    "step", "gate_error", "requested", "explicit_opt_in",
                    "unsafe_build_enabled", "may_autoload",
                    "psp_ready", "tmr_accepted", "pmfw_accepted",
                    "sdma_both_accepted", "pfp_me_mec_accepted",
                    "rlc_g_accepted", "imu_i_accepted", "imu_d_accepted",
                    "mes_scheduler_code_accepted",
                    "mes_scheduler_data_accepted",
                    "mes_kiq_code_accepted", "mes_kiq_data_accepted",
                    "command_id", "prior_fence_expected",
                    "prior_fence_observed", "submitted", "response_valid",
                    "fence_ok", "fence_before", "fence_expected",
                    "fence_observed", "fence_polls", "status",
                    "submit_wptr_before", "submit_wptr_after",
                    "submit_rptr_before", "submit_rptr_after_kick",
                    "submit_rptr_final", "transport_error",
                    "transport_quarantined", "autoload_psp_accepted",
                    "gc_register_addresses_verified", "cp_stat",
                    "bootload_status", "imu_scratch0", "boot_polls",
                    "rlc_bootload_complete", "gc_engine_started",
                    "graphics_queue_started"
                };
                const uint32_t p725Values[] = {
                    p7StepArg, static_cast<uint32_t>(p725Gate.error),
                    static_cast<uint32_t>(p725Input.requested),
                    static_cast<uint32_t>(p725Input.explicitOptIn),
                    p725Compiled,
                    static_cast<uint32_t>(p725Gate.mayAutoload),
                    static_cast<uint32_t>(p725Input.pspRingReady),
                    static_cast<uint32_t>(p725Input.tmrAccepted),
                    static_cast<uint32_t>(p725Input.pmfwAccepted),
                    static_cast<uint32_t>(p725Input.sdmaBothAccepted),
                    static_cast<uint32_t>(p725Input.pfpMeMecAccepted),
                    static_cast<uint32_t>(p725Input.rlcGAccepted),
                    static_cast<uint32_t>(p725Input.imuIAccepted),
                    static_cast<uint32_t>(p725Input.imuDAccepted),
                    static_cast<uint32_t>(
                        p725Input.mesSchedulerCodeAccepted),
                    static_cast<uint32_t>(
                        p725Input.mesSchedulerDataAccepted),
                    static_cast<uint32_t>(p725Input.mesKiqCodeAccepted),
                    static_cast<uint32_t>(p725Input.mesKiqDataAccepted),
                    rn_navi33::p725_rlc_autoload::kCommandId,
                    p725Input.previousFenceExpected,
                    p725Input.previousFenceObserved,
                    static_cast<uint32_t>(p725Submitted),
                    static_cast<uint32_t>(p725ResponseValid),
                    static_cast<uint32_t>(p725FenceOk),
                    p725FenceBefore, p725FenceExpected,
                    p725FenceObserved, p725FencePolls, p725Status,
                    p725WptrBefore, p725WptrAfter,
                    p725RptrBefore, p725RptrAfterKick, p725RptrFinal,
                    p725TransportError, static_cast<uint32_t>(
                        p725Quarantined),
                    static_cast<uint32_t>(p725Accepted),
                    static_cast<uint32_t>(
                        p725GcRegisterAddressesVerified),
                    p725CpStat, p725BootloadStatus, p725ImuScratch0,
                    p725BootPolls,
                    static_cast<uint32_t>(p725BootComplete),
                    0u, 0u
                };
                rnPublish(this, "navi33-p7-rlc-autoload-runtime",
                          p725Names, p725Values,
                          sizeof(p725Values) / sizeof(p725Values[0]));

                // Do not confuse PSP firmware acceptance with started GC
                // and running MES. These facts ONLY affect diagnostics.
                postInput.gcFirmwareLoaded =
                    p725Input.pfpMeMecAccepted &&
                    p718Accepted && p719Accepted && p720Accepted &&
                    p721Accepted && p722Accepted &&
                    p723Accepted && p724Accepted;
                postInput.mesSchedulerLoaded =
                    p721Accepted && p722Accepted;
                postInput.mesKiqLoaded = p723Accepted && p724Accepted;
                postInput.smuFirmwareLoaded = p73PmfwAccepted;
                // SMU firmware in PSP TMR is NOT a verified SMU
                // hardware-ready response. Never infer one from the other.
                postInput.smuReady =
                    p725Preflight.smuHandshakeVerified;

                // A real GC/MES/queue plan must prove every prerequisite,
                // including executable workload and writeback before
                // user queue API becomes available. No writes here.
                rn_navi33::gc_queue_bringup::Input gcQueueInput{};
                gcQueueInput.rlcBootComplete = p725BootComplete;
                // The remaining hardware conditions are not yet proven
                // on real Navi33. Fail CLOSED even if AUTOLOAD_RLC
                // PSP accepted and RLC bootload bit became set.
                const auto gcQueueGate =
                    rn_navi33::gc_queue_bringup::inspect(gcQueueInput);
                const char *const gcQueueNames[] = {
                    "step", "blocker", "rlc_bootload_complete",
                    "gfxhub_initialized", "rs64_configured",
                    "gc_registers_initialized",
                    "mes_scheduler_running", "mes_kiq_running",
                    "queue_memory_owned", "doorbell_mapped",
                    "interrupts_owned", "scratch_gpu_fence_completed",
                    "may_start_diagnostic_queue", "may_expose_graphics_queue"
                };
                const uint32_t gcQueueValues[] = {
                    p7StepArg,
                    static_cast<uint32_t>(gcQueueGate.blocker),
                    static_cast<uint32_t>(gcQueueInput.rlcBootComplete),
                    static_cast<uint32_t>(
                        gcQueueInput.gfxHubInitialized),
                    static_cast<uint32_t>(gcQueueInput.rs64Configured),
                    static_cast<uint32_t>(
                        gcQueueInput.gcRegistersInitialized),
                    static_cast<uint32_t>(
                        gcQueueInput.mesSchedulerRunning),
                    static_cast<uint32_t>(gcQueueInput.mesKiqRunning),
                    static_cast<uint32_t>(
                        gcQueueInput.queueMemoryOwnerVerified),
                    static_cast<uint32_t>(gcQueueInput.doorbellMapped),
                    static_cast<uint32_t>(
                        gcQueueInput.interruptOwnerVerified),
                    static_cast<uint32_t>(
                        gcQueueInput.scratchGpuFenceCompleted),
                    static_cast<uint32_t>(
                        gcQueueGate.mayStartDiagnosticQueue),
                    static_cast<uint32_t>(
                        gcQueueGate.mayExposeGraphicsQueue)
                };
                rnPublish(this, "navi33-p7-gc-queue-preflight",
                          gcQueueNames, gcQueueValues,
                          sizeof(gcQueueValues) / sizeof(gcQueueValues[0]));

                // Dedicated preflight telemetry makes it possible to
                // prove mapping expansion separately from PSP command.
                const char *const p713StageNames[] = {
                    "step", "requested", "explicit_opt_in",
                    "plan_error", "may_expand", "default_bytes",
                    "target_bytes", "actual_bytes", "actual_pages",
                    "stage_prepared", "stage_mapped",
                    "stage_pte_verified", "mec_code_bytes"
                };
                const uint32_t p713StageValues[] = {
                    p7StepArg, static_cast<uint32_t>(p713StageInput.requested),
                    static_cast<uint32_t>(p713StageOptIn),
                    static_cast<uint32_t>(p713StagePlan.error),
                    static_cast<uint32_t>(p713StagePlan.mayExpand),
                    static_cast<uint32_t>(p713StagePlan.defaultStageBytes),
                    static_cast<uint32_t>(p713StagePlan.targetStageBytes),
                    static_cast<uint32_t>(navi33PmfwStageBytes),
                    navi33PmfwPageCount,
                    static_cast<uint32_t>(p73StagePrepared),
                    static_cast<uint32_t>(p73StageMapped),
                    static_cast<uint32_t>(p73StagePteVerified),
                    p713StageInput.firmwareBytes
                };
                rnPublish(this, "navi33-p7-gc-mec-stage-runtime",
                          p713StageNames, p713StageValues,
                          sizeof(p713StageValues) /
                          sizeof(p713StageValues[0]));
                // PSP acceptance of GC11 MEC instructions is NOT MEC
                // execution; stacks, RLC, MES, graphics queues and
                // WindowServer acceleration remain disabled.
                const auto postGate =
                    rn_navi33::p7_postpsp::inspect(postInput);

                const char *const p71n[] = {
                    "step", "mapping_error", "runtime_dma_prepared",
                    "runtime_mapped", "ring_created", "toc_accepted",
                    "toc_status", "tmr_bytes", "post_gate_error",
                    "may_load_pmfw", "may_setup_tmr", "may_load_ip_fw",
                    "sdma_firmware_both_loaded", "may_start_mes"
                };
                const uint32_t p71v[] = {
                    p7StepArg, p71MappingError,
                    static_cast<uint32_t>(navi33PspRuntimePrepared),
                    static_cast<uint32_t>(p71RuntimeMapped),
                    static_cast<uint32_t>(p71RingCreated),
                    static_cast<uint32_t>(p71TocAccepted),
                    p71TocStatus, p71TmrBytes,
                    static_cast<uint32_t>(postGate.error),
                    static_cast<uint32_t>(postGate.mayLoadPmfw),
                    static_cast<uint32_t>(postGate.maySetupTmr),
                    static_cast<uint32_t>(postGate.mayLoadIpFirmware),
                    static_cast<uint32_t>(postInput.sdmaFirmwareLoaded),
                    static_cast<uint32_t>(postGate.mayStartMes)
                };
                rnPublish(this, "navi33-p7-postpsp-runtime",
                          p71n, p71v, sizeof(p71v) / sizeof(p71v[0]));

                const char *const p7rn[] = {
                    "step", "gate_error", "gart_ready", "psp_ready",
                    "sos_alive", "post_psp_error",
                    "may_start_queues", "mes_started",
                    "graphics_queue_started"
                };
                const uint32_t p7rv[] = {
                    p7StepArg, static_cast<uint32_t>(p7AfterPsp.error),
                    static_cast<uint32_t>(p7GartReady),
                    static_cast<uint32_t>(p7PspOk),
                    static_cast<uint32_t>(p7SosAfter != 0),
                    static_cast<uint32_t>(postGate.error),
                    static_cast<uint32_t>(
                        postGate.mayCreateGraphicsQueue),
                    0u, 0u
                };
                rnPublish(this, "navi33-p7-render-pipeline", p7rn, p7rv,
                          sizeof(p7rv) / sizeof(p7rv[0]));

                // Linux GC11.0.2 boot-parity preflight. This is a read-only
                // status report; it does not substitute for a real PSP,
                // GMC11 or MES KIQ/Scheduler hardware initialization.
                const rn_navi33::linux_boot::Snapshot linuxSnapshot{
                    check.eligibleForComputeBringup(), true, p6MemoryVerified,
                    pspRead, gate.ifwiReady, gate.secureOsAlive
                };
                const auto linuxPreflight = rn_navi33::linux_boot::inspect(linuxSnapshot);
                const char *const linuxNames[] = {
                    "blocker", "observation_mask", "hardware_init_allowed"
                };
                const uint32_t linuxValues[] = {
                    static_cast<uint32_t>(linuxPreflight.blocker),
                    linuxPreflight.observationMask,
                    static_cast<uint32_t>(linuxPreflight.hardwareInitAllowed)
                };
                rnPublish(this, "navi33-linux-boot-preflight", linuxNames,
                          linuxValues, sizeof(linuxValues) / sizeof(linuxValues[0]));
                // A valid IP Discovery / sOS mailbox does not establish a
                // retained MES ring, usable GPUVM or exclusive GPU owner.
                setProperty("navi33-linux-boot-hw-writes", false);
                IOLog("RedNavi: Navi33 P6.1 PSP status %u (NO PSP GPU commands)\n",
                      static_cast<unsigned>(gate.status));
                IOLog("RedNavi: Navi33 PSP status: IFWI=0x%08x sOS=0x%08x SMU=0x%08x (no commands sent)\n",
                      pspInit, pspSos, smuResp);
            } else {
                IOLog("RedNavi: Navi33 discovery valid but required GC/MMHUB/NBIO/OSSSYS IP bases missing\n");
                setProperty("navi33-ip-bases-ready", false);
            }
        } else {
            IOLog("RedNavi: Navi33 IP Discovery not validated (error %u, %u blocks); blocking compute initialization\n",
                  static_cast<unsigned>(check.error), static_cast<unsigned>(check.blocksFound));
            setProperty("navi33-identity-verified", false);
        }
        // A failed identity can be inspected in IORegistry. No Navi32 PSP,
        // GC, SDMA, DCN, framebuffer or Metal acceleration runs on Navi33.
        registerService();
        return true;
    }
    // Other non-Navi32 devices retain the legacy diagnostic behaviour.
    // Navi33 has already taken its own validation path above.
    if (pci->configRead16(kIOPCIConfigDeviceID) != 0x747E) {
        IOLog("RedNavi: device 0x%04x is not a Navi 32: discovery diagnostic only\n",
              pci->configRead16(kIOPCIConfigDeviceID));
        dumpDiscovery();
        registerService();
        return true;
    }

    // System Information reads the GPU name and VRAM size from these properties of the PCI node.
    {
        static const char kName[] = "AMD Radeon RX 7800 XT";
        uint32_t vramMb = 16384;
        OSData *m = OSData::withBytes(kName, sizeof(kName));
        OSData *v = OSData::withBytes(&vramMb, sizeof(vramMb));
        if (m) { pci->setProperty("model", m); m->release(); }
        if (v) { pci->setProperty("VRAM,totalMB", v); v->release(); }
    }

    dumpDiscovery();
    // IP bases cannot be resolved until the copied discovery table exists.
    for (uint32_t i = 0; i < 4; i++)
        ipBase(kRNHwIdGc, &gpu.reg_offset[RN_GC_HWIP][0][i], i);
    ipBase(kRNHwIdMp1, &gpu.reg_offset[RN_MP1_HWIP][0][0]);
    dumpVbios();
    dumpRegs(kRNMailboxRegs, sizeof(kRNMailboxRegs) / sizeof(kRNMailboxRegs[0]), "mailbox-regs");
    dumpRegs(kRNMmhubRegs, sizeof(kRNMmhubRegs) / sizeof(kRNMmhubRegs[0]), "mmhub-regs");
    dumpRegs(kRNDmcubRegs, sizeof(kRNDmcubRegs) / sizeof(kRNDmcubRegs[0]), "dmcub-regs");
    dumpRegs(kRNHpdRegs, sizeof(kRNHpdRegs) / sizeof(kRNHpdRegs[0]), "hpd-regs");
    smuProbe("smu-info-early", false);   // registers only, before any stage that writes to the card
    dmcubSnapshot("dmcub-snap-boot");
    loadPspChain();
    if (createPspRing()) {
        sendPspBootCfgGet();
        sendPspLoadToc();
        sendPspSetupTmr();
        loadSmuFw();
        loadGcFw();
    }
    loadDmcub();
    dpInit();
    probeAux();
    selectMode();
    dpTrain();
    dcnReplay();
    dcnDump();
    smuProbe("smu-info", true);          // messages, once the PSP and the display are up
    {
        struct rn_device *adev = &gpu;
        const char *hn[3] = {"c2pmsg_90", "sdma_version", "ms_uptime"};
        uint32_t hv[3] = {RREG32_SOC15(RN_MP1, 0, mmMP1_SMN_C2PMSG_90), rd(0x1295 * 4), 0};
        rnPublish(this, "after-smu-probe", hn, hv, 3);
    }
    sdmaTest();
    publishFramebuffer();
    registerService();
    return true;
}

void RedNaviProbe::stop(IOService *provider) {
    // Until engine shutdown/drain is implemented, the GPU may still own any
    // published host allocation. Dropping software state cannot revoke DMA.
    const bool retainDma = gpuLive;
    gpuLive = false;
    if (watchTimer) { watchTimer->cancelTimeout(); if (getWorkLoop()) getWorkLoop()->removeEventSource(watchTimer); watchTimer->release(); watchTimer = nullptr; }
    if (msiSrc) { msiSrc->disable(); if (getWorkLoop()) getWorkLoop()->removeEventSource(msiSrc); msiSrc->release(); msiSrc = nullptr; }
    if (retainDma) rn_mes_forget_workspace();   // this provider's native path; software references only, not GPU quiescence
    if (modeLock) { IOLockFree(modeLock); modeLock = nullptr; }
    rn_compute_lock_free();
    if (disc) { disc->release(); disc = nullptr; }
    if (vbiosCopy) { vbiosCopy->release(); vbiosCopy = nullptr; }
    if (retainDma) IOLog("RedNavi: GPU shutdown not verified; host DMA backing retained until reboot\n");
    for (uint32_t i = 0; bos && i < boCount; i++) {
        if (bos[i].quarantined || (retainDma && bos[i].md)) continue;
        if (bos[i].dma) { bos[i].dma->complete(); bos[i].dma->clearMemoryDescriptor(); bos[i].dma->release(); }
        if (bos[i].md) bos[i].md->release();
        if (bos[i].vmd) bos[i].vmd->release();
        if (bos[i].bus) IOFree(bos[i].bus, sizeof(uint64_t) * (bos[i].size / 4096));
    }
    boCount = 0;
    if (bos) { IOFree(bos, sizeof(RNBo) * kBoCap); bos = nullptr; }
    if (boLock) { IOLockFree(boLock); boLock = nullptr; }
    if (queueLock) { IOLockFree(queueLock); queueLock = nullptr; }
    if (doorbellKeep) { rn_set_doorbell(nullptr, 0); doorbellKeep->release(); doorbellKeep = nullptr; }
    if (!retainDma && hostDma) { hostDma->complete(); hostDma->clearMemoryDescriptor(); hostDma->release(); hostDma = nullptr; }
    if (!retainDma && hostBuf) { hostBuf->release(); hostBuf = nullptr; }
    if (!navi33PspRuntimeMapped) {
        releaseNavi33PspRuntimeDma();
    } else {
        IOLog("RedNavi: P7.1 PSP runtime GART pages are live; retaining DMA backing until reboot\n");
    }
    if (!navi33PmfwMapped) {
        releaseNavi33PmfwDma();
    } else {
        IOLog("RedNavi: P7.3 PMFW staging GART pages are live; retaining DMA backing until reboot\n");
    }
    // P6.4 ownership-only probes can be released normally. P7, however,
    // installs these exact pages into live VMID0/GART PTEs. There is not yet
    // a native-validated GFXHUB/MMHUB teardown path, so retain the backing
    // until reboot rather than leave the GPU with dangling system PTEs.
    if (!navi33P7GartProgrammed) {
        releaseNavi33FwPriDma();
    } else {
        IOLog("RedNavi: P7 GART is live; retaining fw_pri DMA backing until reboot\n");
    }
    // P6.5 is bookkeeping only; dropping the claim performs no hardware I/O.
    navi33VramLedgerOwned = false;
    navi33VramPtOffset = 0;
    navi33VramScratchOffset = 0;
    navi33VramDummyOffset = 0;
    if (regs) { regs->release(); regs = nullptr; }
    IOService::stop(provider);
}
