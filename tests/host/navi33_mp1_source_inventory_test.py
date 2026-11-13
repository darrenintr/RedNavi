"""P7.34 source inventory test, never accesses PCI or GPU."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/navi33_mp1_source_inventory.py"
SPEC = importlib.util.spec_from_file_location("navi33_mp1_source_inventory", SCRIPT)
assert SPEC and SPEC.loader
inv = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inv)


class Navi33Mp1SourceInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = inv.load_repo_sources(ROOT)
        cls.report = inv.inspect_sources(cls.sources)

    def test_known_named_direct_and_dynamic_sites_are_both_counted(self):
        r = self.report
        self.assertTrue(r["known_sites_match_reviewed_baseline"])
        self.assertEqual(len(r["named_direct_mp1_writes"]), 4)
        self.assertEqual(len(r["p727_dynamic_writes"]), 3)
        self.assertEqual({
            x["register"] for x in r["p727_dynamic_writes"]
        }, {"responseByteOffset", "parameterByteOffset",
            "messageByteOffset"})
        self.assertEqual({
            (x["path"], x["register"])
            for x in r["named_direct_mp1_writes"]
        }, {
            ("RedNavi/smu13.cpp", "66"),
            ("RedNavi/smu13.cpp", "82"),
            ("RedNavi/smu13.cpp", "90"),
            ("RedNavi/kern_probe.cpp", "90"),
        })

    def test_static_analysis_never_certifies_external_writers(self):
        r = self.report
        self.assertTrue(r["source_review_only"])
        self.assertTrue(r["p727_mailbox_lease_hardcoded_false"])
        self.assertTrue(r["p727_uses_raw_bar5_write_callback"])
        self.assertFalse(r["indirect_or_unrecognized_host_writers_excluded"])
        self.assertFalse(r["third_party_bar5_writers_excluded"])
        self.assertFalse(r["firmware_writers_excluded"])
        self.assertFalse(r["mailbox_exclusive_proven"])
        self.assertFalse(r["may_issue_smu_command"])

    def test_unknown_new_named_write_fails_reviewed_baseline(self):
        sources = dict(self.sources)
        sources["RedNavi/smu13.cpp"] += (
            "\nvoid extra() { WREG32_SOC15(RN_MP1, 0, "
            "mmMP1_SMN_C2PMSG_66, 0); }\n"
        )
        r = inv.inspect_sources(sources)
        self.assertFalse(r["known_sites_match_reviewed_baseline"])
        self.assertFalse(r["mailbox_exclusive_proven"])

    def test_dynamic_offset_write_removed_fails_reviewed_baseline(self):
        sources = dict(self.sources)
        path = "RedNavi/navi33_smu_mailbox_roundtrip.h"
        sources[path] = sources[path].replace(
            "write(reg.messageByteOffset, admission::kGetSmuVersionMessage);",
            "unknown_dynamic_access();",
        )
        r = inv.inspect_sources(sources)
        self.assertFalse(r["known_sites_match_reviewed_baseline"])

    def test_unsafe_mailbox_lease_toggle_fails_reviewed_baseline(self):
        sources = dict(self.sources)
        path = "RedNavi/kern_probe.cpp"
        sources[path] = sources[path].replace(
            "constexpr bool p727MailboxLeaseHeld = false;",
            "constexpr bool p727MailboxLeaseHeld = true;",
        )
        r = inv.inspect_sources(sources)
        self.assertFalse(r["known_sites_match_reviewed_baseline"])
        self.assertFalse(r["may_issue_smu_command"])


if __name__ == "__main__":
    unittest.main()
