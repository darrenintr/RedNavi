"""P7.35: check that "read" wrappers cannot silently become MMIO-write-free.

This test is a source-pattern guard. It does not prove that reading MP1
registers has no device side effects and does not authorize live reads.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Navi33PassiveMmioReadPathAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reg = (ROOT / "compat/src/rn_reg.cpp").read_text()
        cls.kern = (ROOT / "RedNavi/kern_probe.cpp").read_text()
        cls.plan = (
            ROOT / "RedNavi/navi33_p735_passive_mp1_read_plan.h"
        ).read_text()

    def test_indirect_rreg32_read_has_index_mmio_write(self):
        body = self.reg.split("u32 rn_rreg32(", 1)[1].split(
            "void rn_wreg32(", 1)[0]
        self.assertIn("if (off + 4 <= adev->rmmio_size)", body)
        self.assertIn("writel((u32)off, adev->rmmio + RN_RSMU_INDEX);",
                      body)
        self.assertIn("readl(adev->rmmio + RN_RSMU_DATA)", body)

    def test_vram_read_has_index_mmio_writes(self):
        body = self.kern.split(
            "uint32_t vramRead(uint64_t pos)", 1
        )[1].split("void vramWrite(", 1)[0]
        self.assertIn("wr(kRNMmIndex,", body)
        self.assertIn("wr(kRNMmIndexHi,", body)
        self.assertIn("rd(kRNMmData)", body)
        compat = self.reg.split("u32 rn_vram_rd32(", 1)[1].split(
            "void rn_vram_wr32(", 1)[0]
        self.assertIn("RN_MM_INDEX", compat)
        self.assertIn("RN_MM_INDEX_HI", compat)
        self.assertIn("writel(", compat)

    def test_direct_rednavi_rd_does_not_write_an_index(self):
        line = next(
            line for line in self.kern.splitlines()
            if "uint32_t rd(uint32_t off)" in line
        )
        self.assertIn(
            "*reinterpret_cast<volatile uint32_t *>"
            "(regs->getVirtualAddress() + off)", line
        )
        self.assertNotIn("wr(", line)
        self.assertNotIn("writel", line)
        self.assertNotIn("rn_rreg32(", line)

    def test_plan_not_connected_to_live_gpu_or_smucmd(self):
        self.assertNotIn(
            '#include "navi33_p735_passive_mp1_read_plan.h"',
            self.kern,
        )
        self.assertIn("bool liveReadPermitted = false;", self.plan)
        self.assertIn(
            "bool registerReadSideEffectsExcluded = false;", self.plan
        )
        self.assertIn("bool mailboxExclusiveProven = false;", self.plan)
        self.assertIn("bool mayIssueSmuCommand = false;", self.plan)
        self.assertIn(
            "constexpr bool p727MailboxLeaseHeld = false;", self.kern
        )
        for forbidden in ("WREG32", "RREG32", "readl(", "writel(",
                          "pci->open(", "pci->configWrite",
                          "mapDeviceMemory(", "rn_smu13_send(", "wr(", "rd("):
            self.assertNotIn(forbidden, self.plan)

    def test_plan_uses_existing_validated_byte_offsets_only(self):
        self.assertIn("smu_mailbox_roundtrip::plan(", self.plan)
        self.assertIn("in.bar5MappedBytes", self.plan)
        self.assertIn("if (!reg.valid)", self.plan)
        self.assertIn("r.responseByteOffset = reg.responseByteOffset;", self.plan)


if __name__ == "__main__":
    unittest.main()
