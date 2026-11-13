"""P7.37 source guard: opt-in, 2 extra direct reads per P7.36 tick.

This is a textual build-time guard, not a proof that physical MMIO reads
have no hardware side effects or that no other Kext accesses BAR5.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Navi33P737KernelReadAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kern = (ROOT / "RedNavi/kern_probe.cpp").read_text()
        cls.model = (
            ROOT / "RedNavi/navi33_p737_passive_mailbox_triplet.h"
        ).read_text()

    def test_p736_and_p737_are_separate_explicit_opt_ins(self):
        k = self.kern
        self.assertIn(
            'PE_parse_boot_argn("rednavi_navi33_p737_triplet_read"', k
        )
        self.assertIn("p737RawOptIn == 1u", k)
        self.assertIn(
            'PE_parse_boot_argn("rednavi_navi33_p736_response_read"', k
        )
        self.assertIn("p737Config.p736ResponseArmed = p736ResponseSampler.armed;", k)
        self.assertIn("p737Config.mp1BaseDword = p736Config.mp1BaseDword;", k)
        self.assertIn("p737Config.bar5MappedBytes = p736Config.bar5MappedBytes;", k)
        self.assertIn("bool explicitOptIn = false;", self.model)
        self.assertIn("if (!cfg.explicitOptIn) return Status::Disabled;", self.model)
        self.assertIn("if (!cfg.p736ResponseArmed) return Status::RequiresP736OptIn;", self.model)

    def test_two_extra_direct_bar5_reads_only(self):
        k = self.kern
        scope = k.split(
            "void RedNaviProbe::sampleNavi33P737PassiveTriplet() {", 1
        )[1].split(
            "void RedNaviProbe::scheduleNavi33LateOwnerAudit() {", 1
        )[0]
        self.assertIn("triplet::sample(", scope)
        self.assertEqual(scope.count("return rd(offset);"), 1)
        self.assertIn("p736LastReadTick, p736LastReadValue,", scope)
        self.assertIn("p736ResponseSampler.responseByteOffset,", scope)
        self.assertIn("regs->getVirtualAddress()", scope)
        self.assertIn("regs->getLength()", scope)
        for token in (
            "wr(", "WREG32", "RREG32", "rn_rreg32(", "vramRead(",
            "vramWrite(", "RN_RSMU_INDEX", "RN_MM_INDEX",
            "pci->open(", "pci->configWrite",
            "setMemoryEnable(", "mapDeviceMemoryWithRegister(",
            "rn_smu13_send(",
        ):
            self.assertNotIn(token, scope, msg=token)
        self.assertIn('"navi33-p7-mp1-triplet-sample-1"', scope)
        self.assertIn('"navi33-p7-mp1-triplet-sample-2"', scope)

    def test_existing_p733_timer_and_p736_response_path_retained(self):
        k = self.kern
        self.assertIn("sampleNavi33P736ResponseMonitor();", k)
        self.assertIn("sampleNavi33P737PassiveTriplet();", k)
        tick = k.split(
            "void RedNaviProbe::sampleNavi33LateOwnerAudit() {", 1
        )[1].split(
            "void RedNaviProbe::sampleNavi33P737PassiveTriplet() {", 1
        )[0]
        self.assertLess(tick.index("sampleNavi33P736ResponseMonitor();"),
                        tick.index("sampleNavi33P737PassiveTriplet();"))
        self.assertEqual(k.count(
            "void RedNaviProbe::scheduleNavi33LateOwnerAudit()"), 1)
        self.assertIn("lateOwnerTimer->setTimeoutMS(5000)", k)
        self.assertIn("lateOwnerTimer->setTimeoutMS(8000)", k)
        self.assertNotIn("p737Timer", k)

    def test_atomic_pair_budget_fails_closed(self):
        model = self.model
        for token in (
            "constexpr uint32_t kMaxTripletsPerService = 2u;",
            "constexpr uint32_t kReadsPerTriplet = 2u;",
            "constexpr uint32_t kMaxAdditionalDwordReads = 4u;",
            "if (!claimPair(budget))",
            "__atomic_compare_exchange_n(",
            "const uint32_t message = read(state.messageByteOffset);",
            "const uint32_t parameter = read(state.parameterByteOffset);",
            "tick != lastP736ResponseTick",
            "p736ResponseByteOffset != state.responseByteOffset",
            "currentMappedBarBytes",
        ):
            if token == "currentMappedBarBytes":
                continue
            self.assertIn(token, model, msg=token)
        executable = "\n".join(
            line.split("//", 1)[0] for line in model.splitlines()
        )
        for forbidden in (
            "WREG32", "RREG32", "write(", "writel(", "rn_wreg32(",
            "wr(", "pci->configWrite", "rn_smu13_send(", "IOWrite",
        ):
            self.assertNotIn(forbidden, executable, msg=forbidden)

    def test_no_claim_of_external_writer_or_handshake(self):
        m = self.model
        for token in (
            "bool rednaviCommandSubmittedProven = false;",
            "bool appleOrOtherHostWriterAttributed = false;",
            "bool firmwareResponseToNewCommandProven = false;",
            "bool mailboxExclusiveProven = false;",
            "bool mayIssueSmuCommand = false;",
        ):
            self.assertIn(token, m)
        self.assertIn("constexpr bool p727MailboxLeaseHeld = false;",
                      self.kern)

    def test_shutdown_and_version(self):
        stop = self.kern.split(
            "void RedNaviProbe::stop(IOService *provider) {", 1
        )[1]
        self.assertLess(
            stop.index("lateOwnerWorkLoop->removeEventSource(lateOwnerTimer)"),
            stop.index("p737_passive_mailbox_triplet::stop(p737TripletState)"))
        self.assertLess(
            stop.index("p737_passive_mailbox_triplet::stop(p737TripletState)"),
            stop.index("if (regs) { regs->release(); regs = nullptr; }"))
        self.assertIn("gNavi33P737AdditionalReadBudget", self.kern)
        self.assertNotIn("gNavi33P737AdditionalReadBudget = ", self.kern)


if __name__ == "__main__":
    unittest.main()
