"""Host source audit of P7.33's delayed *read-only IOKit* sample path."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
KERN = ROOT / "RedNavi/kern_probe.cpp"


class LateProviderAuditSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = KERN.read_text(encoding="utf-8")

    def test_observer_is_scheduled_only_after_navi33_registration(self):
        source = self.source
        pos = source.index(
            "// A failed identity can be inspected in IORegistry.")
        snippet = source[pos:pos+650]
        self.assertIn("registerService();", snippet)
        self.assertIn("scheduleNavi33LateOwnerAudit();", snippet)
        self.assertLess(snippet.index("registerService();"),
                        snippet.index("scheduleNavi33LateOwnerAudit();"))
        self.assertIn('setTimeoutMS(5000)', source)
        self.assertIn('setTimeoutMS(8000)', source)

    def test_late_observer_calls_only_host_service_inspection(self):
        # Only inspect the IOService observer itself. P7.36/P7.37
        # optional callbacks are audited by their own independent tests.
        text = self.source.split(
            "void RedNaviProbe::sampleNavi33LateOwnerAudit() {", 1
        )[1].split("\n}\n", 1)[0]
        self.assertIn("pci->isOpen()", text)
        self.assertIn("pci->isOpen(this)", text)
        self.assertIn("pci->isOpen(child)", text)
        self.assertIn("getChildIterator(gIOServicePlane)", text)
        for forbidden in (
            "WREG32", "RREG32", "vramRead(", "vramWrite(",
            "wr(", "rd(", "mapDeviceMemory", "pci->open(",
            "pci->setMemoryEnable(", "pci->configWrite",
            "pci->setBusMasterEnable(", "pspSubmit(", "rn_smu13_send(",
        ):
            self.assertNotIn(forbidden, text, msg=forbidden)

    def test_stops_and_releases_timer_before_pci_teardown(self):
        stop = self.source.split(
            "void RedNaviProbe::stop(IOService *provider) {", 1
        )[1]
        timer = stop.index("lateOwnerTimer->cancelTimeout();")
        remove = stop.index("lateOwnerWorkLoop->removeEventSource(lateOwnerTimer)")
        release = stop.index("lateOwnerTimer->release();")
        regs_release = stop.index("if (regs)")
        self.assertLess(timer, remove)
        self.assertLess(remove, release)
        self.assertLess(release, regs_release)

    def test_mailbox_lease_stays_hardcoded_off(self):
        self.assertIn("constexpr bool p727MailboxLeaseHeld = false;", self.source)
        self.assertIn('"mailbox_exclusive_proven"', self.source)
        self.assertIn('"may_issue_smu_command"', self.source)
        self.assertIn('"navi33-p7-late-provider-audit-1"', self.source)
        self.assertIn('"navi33-p7-late-provider-audit-2"', self.source)


if __name__ == "__main__":
    unittest.main()
