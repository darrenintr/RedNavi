#include "mmhub_regs.h"

static_assert(regMMVM_L2_CNTL == 0x0680, "Navi33 MMHUB 3.0.2 L2 base");
static_assert(regMMVM_CONTEXT0_CNTL == 0x06c0, "Navi33 MMHUB 3.0.2 context0");
static_assert(regMMVM_INVALIDATE_ENG17_SEM == 0x06e2, "Navi33 MMHUB 3.0.2 ENG17 semaphore");
static_assert(regMMVM_INVALIDATE_ENG17_REQ == 0x06f4, "Navi33 MMHUB 3.0.2 ENG17 request");
static_assert(regMMVM_INVALIDATE_ENG17_ACK == 0x0706, "Navi33 MMHUB 3.0.2 ENG17 acknowledge");
static_assert(regMMVM_INVALIDATE_ENG17_ADDR_RANGE_LO32 == 0x0729, "Navi33 MMHUB 3.0.2 invalidate range");
static_assert(regMMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32 == 0x072b, "Navi33 MMHUB 3.0.2 PT base");
static_assert(regMMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32 == 0x074b, "Navi33 MMHUB 3.0.2 PT start");
static_assert(regMMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32 == 0x076b, "Navi33 MMHUB 3.0.2 PT end");
static_assert(regMMMC_VM_MX_L1_TLB_CNTL == 0x0873, "Navi33 MMHUB 3.0.2 L1 TLB");
static_assert(MMVM_INVALIDATE_ENG17_REQ__PER_VMID_INVALIDATE_REQ_MASK == 0x0000ffffu,
              "ENG17 VMID mask");
static_assert(MMVM_INVALIDATE_ENG17_ACK__SEMAPHORE_MASK == 0x00010000u,
              "ENG17 semaphore ACK bit");

int main() { return 0; }
