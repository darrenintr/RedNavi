"""P7.36 kernel source guard: bounded direct-read path with no new MMIO writes.

These checks are necessarily syntactic; passing does not prove that a GPU
register read has no hardware side effect or that stop races are impossible.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Navi33P736ReadOnlyKernelAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kern = (ROOT / "RedNavi/kern_probe.cpp").read_text()
        cls.monitor = (
            ROOT / "RedNavi/navi33_p736_mp1_response_sampler.h"
        ).read_text()

    def test_opt_in_is_required_and_default_is_disabled(self):
        kern = self.kern
        self.assertIn(
            'PE_parse_boot_argn("rednavi_navi33_p736_response_read"',
            kern)
        self.assertIn("p736RawOptIn == 1u", kern)
        self.assertIn("check.eligibleForComputeBringup()", kern)
        self.assertIn("check.observed.smu.revision == 7u", kern)
        self.assertIn("regs->getLength()", kern)
        self.assertIn("p736ConfigError =", kern)
        self.assertIn("p736Config.directBar5MappingReady =", kern)
        self.assertIn("if (p736ResponseSampler.armed)", kern)
        self.assertIn("bool explicitOptIn = false;", self.monitor)
        self.assertIn("if (!in.explicitOptIn) return Error::Disabled;",
                      self.monitor)

    def test_p736_has_only_one_direct_mmio_read_and_no_mmio_writes(self):
        kern = self.kern
        scope = kern.split(
            "void RedNaviProbe::sampleNavi33P736ResponseMonitor() {", 1
        )[1].split(
            "void RedNaviProbe::sampleNavi33LateOwnerAudit() {", 1
        )[0]
        self.assertIn("monitor::sample(", scope)
        self.assertEqual(scope.count("return rd(offset);"), 1)
        self.assertIn("regs->getVirtualAddress()", scope)
        self.assertIn("regs->getLength()", scope)
        self.assertIn("pci && !pci->isInactive() && !isInactive()", scope)
        self.assertNotIn("rn_smu13_send(", scope)
        for forbidden in (
            "wr(", "WREG32", "RREG32", "rn_rreg32(",
            "vramRead(", "vramWrite(", "MM_INDEX", "RSMU_INDEX",
            "pci->open(", "pci->configWrite", "setMemoryEnable",
            "mapDeviceMemoryWithRegister(", "IORead", "IOWrite",
        ):
            self.assertNotIn(forbidden, scope, msg=forbidden)
        self.assertIn("snapshot.mailboxExclusiveProven", scope)
        self.assertIn("snapshot.mayIssueSmuCommand", scope)

    def test_reuses_existing_late_timer_only(self):
        kern = self.kern
        self.assertEqual(kern.count("void RedNaviProbe::scheduleNavi33LateOwnerAudit()"), 1)
        self.assertEqual(kern.count("sampleNavi33P736ResponseMonitor();"), 2)
        self.assertIn("lateOwnerTimer->setTimeoutMS(5000)", kern)
        self.assertIn("lateOwnerTimer->setTimeoutMS(8000)", kern)
        self.assertNotIn("p736Timer", kern)
        self.assertIn('"navi33-p7-mp1-response-sample-1"', kern)
        self.assertIn('"navi33-p7-mp1-response-sample-2"', kern)

    def test_callback_teardown_precedes_bar_release(self):
        stop = self.kern.split(
            "void RedNaviProbe::stop(IOService *provider) {", 1
        )[1]
        self.assertLess(stop.index("lateOwnerTimer->cancelTimeout();"),
                        stop.index("lateOwnerTimer->disable();"))
        self.assertLess(stop.index("lateOwnerTimer->disable();"),
                        stop.index("lateOwnerWorkLoop->removeEventSource(lateOwnerTimer)"))
        self.assertLess(stop.index("lateOwnerWorkLoop->removeEventSource(lateOwnerTimer)"),
                        stop.index("p736_response_sampler::stop(p736ResponseSampler)"))
        self.assertLess(stop.index("p736_response_sampler::stop(p736ResponseSampler)"),
                        stop.index("if (regs) { regs->release(); regs = nullptr; }"))

    def test_kext_global_budget_not_reset_or_shared_with_smu_latch(self):
        kern = self.kern
        self.assertIn("gNavi33P736Mp1ResponseReadBudget", kern)
        self.assertIn("gNavi33SmuKextImageAttemptClaim", kern)
        self.assertIn("static rn_navi33::p736_response_sampler::KextBudget", kern)
        self.assertNotIn("gNavi33P736Mp1ResponseReadBudget = ", kern)
        self.assertIn("constexpr bool p727MailboxLeaseHeld = false;", kern)
        self.assertIn("constexpr uint32_t kMaxKextImageReads = 2u;",
                      self.monitor)
        self.assertIn("if (!claimOne(budget))", self.monitor)
        self.assertIn("const uint32_t value = read(s.responseByteOffset);",
                      self.monitor)

    def test_no_elevated_rights_from_a_response_transition(self):
        mon = self.monitor
        self.assertIn("bool mailboxExclusiveProven = false;", mon)
        self.assertIn("bool handshakeVerified = false;", mon)
        self.assertIn("bool mayIssueSmuCommand = false;", mon)
        self.assertIn("return r;", mon)
        for forbidden in ("WREG32", "RREG32", "readl(", "writel(",
                          "pci->open(", "configWrite",
                          "vramRead(", "rn_smu13_send(", "wr(", "rd("):
            self.assertNotIn(forbidden, mon, msg=forbidden)


if __name__ == "__main__":
    unittest.main()
