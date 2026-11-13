"""P7.30 review guard for the known MP1 C2PMSG host write sites.

This is a conservative source-level regression check, not a proof that
every dynamic BAR5 index/write or third-party/firmware writer is excluded.
"""
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
C2PMSG_DIRECT_WRITE = re.compile(
    r"\bWREG32_SOC15\s*\(\s*RN_MP1\s*,\s*0\s*,\s*"
    r"mmMP1_SMN_C2PMSG_(?:66|82|90)\b"
)


class SmuMailboxWriterAudit(unittest.TestCase):
    def read(self, name):
        return (ROOT / name).read_text(encoding="utf-8")

    def test_all_known_direct_mp1_host_writes_have_one_reviewed_site(self):
        found = {}
        # Include all RedNavi and compat C++ translation units so new
        # direct C2PMSG_66/82/90 writers cannot silently be introduced.
        for directory in (ROOT / "RedNavi", ROOT / "compat/src"):
            for path in directory.glob("*.cpp"):
                count = len(C2PMSG_DIRECT_WRITE.findall(
                    path.read_text(encoding="utf-8")))
                if count:
                    found[str(path.relative_to(ROOT))] = count
        self.assertEqual(found, {
            "RedNavi/smu13.cpp": 3,
            # Legacy timeout recovery (Navi32 only), explicitly guarded.
            "RedNavi/kern_probe.cpp": 1,
        })

    def test_legacy_smu_device_alias_uses_same_lock_as_gpu(self):
        kern = self.read("RedNavi/kern_probe.cpp")
        helper = self.read("compat/include/amd/rn_device.h")
        sender = self.read("RedNavi/smu13.cpp")
        self.assertIn(
            "adev->smu_msg_lock_override = &gpu.smu_msg_lock;", kern)
        self.assertIn(
            "ScopedSmuMessageLock guard(rn_smu_message_mutex(adev));",
            sender)
        self.assertIn("mutex_lock(rn_smu_message_mutex(adev));", kern)
        self.assertIn("mutex_unlock(rn_smu_message_mutex(adev));", kern)
        self.assertIn("struct mutex *smu_msg_lock_override;", helper)

    def test_one_shot_is_not_stack_local_and_live_submit_stays_blocked(self):
        kern = self.read("RedNavi/kern_probe.cpp")
        self.assertIn("SessionState p730SmuSession {};", kern)
        self.assertIn("p730SmuSession,", kern)
        self.assertNotIn("bool p727OneShotConsumed = false;", kern)
        self.assertIn("constexpr bool p727MailboxLeaseHeld = false;", kern)
        self.assertIn("[]() { return rn_now_ns(); }", kern)
        self.assertIn('"navi33-p7-smu-transaction-guard"', kern)

    def test_deadline_and_quarantine_are_distinct_from_poll_count(self):
        rt = self.read("RedNavi/navi33_smu_mailbox_roundtrip.h")
        self.assertIn("constexpr uint64_t kDeadlineNanoseconds", rt)
        self.assertIn("currentNs - startedNs", rt)
        self.assertIn("state.quarantined = true", rt)
        self.assertIn("state.consumed = true", rt)
        self.assertIn("Error::ClockRegressed", rt)
        self.assertIn("Error::DeadlineExpired", rt)
        self.assertIn("admission::kMaxResponsePolls", rt)


if __name__ == "__main__":
    unittest.main()
