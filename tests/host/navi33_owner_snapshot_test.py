"""Host-only parsing tests; no ioreg, kmutil, or hardware operations."""
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "tools/navi33_owner_snapshot.py"
SPEC = importlib.util.spec_from_file_location("navi33_owner_snapshot", SCRIPT)
assert SPEC and SPEC.loader
observer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(observer)


class Navi33OwnerSnapshotTests(unittest.TestCase):
    def test_gfx0_subtree_only_until_matching_next_pci_sibling(self):
        tree = """+-o IOService
  +-o pci-bridge@0
    +-o GFX0@0 <class IOPCIDevice>
      +-o RedNaviProbe <class RedNaviProbe>
      +-o AMDSupport <class AMDSupport>
      +-o .Display_boot <class IONDRVFramebuffer>
        +-o display0 <class IODisplayConnect>
    +-o HDAU@0,1 <class IOPCIDevice>
      +-o AppleHDA <class IOService>
  +-o other@0
"""
        result = observer.gfx0_subtrees(tree)
        self.assertEqual(len(result), 1)
        self.assertIn("RedNaviProbe", result[0])
        self.assertIn("AMDSupport", result[0])
        self.assertIn(".Display_boot", result[0])
        self.assertIn("display0", result[0])
        self.assertNotIn("HDAU", result[0])
        self.assertNotIn("AppleHDA", result[0])

    def test_gfx0_missing_and_not_false_positive(self):
        tree = "+-o GFX01@0\n+-o GFX0BAR@2\n"
        self.assertEqual(observer.gfx0_subtrees(tree), [])

    def test_kext_filter_does_not_claim_attachment(self):
        lines = """  30 0 com.apple.kext.AMDSupport (7.0)
  40 0 org.rednavi.Navi33P4 (0.0.50)
  42 0 com.apple.driver.SomeUSB (1.0)
"""
        text = observer.select_interesting_kexts(lines)
        self.assertIn("AMDSupport", text)
        self.assertIn("Navi33P4", text)
        self.assertNotIn("SomeUSB", text)

    def test_p7_properties_selected_without_inventing_data(self):
        props = '''"navi33-p7-smu-lifetime-latch" = {"attempt_slot_claimed"=0}
"navi33-p7-smu-roundtrip" = {"command_sent"=0}
"other-property" = {"value"=1}'''
        text = observer.select_telemetry(props)
        self.assertIn("attempt_slot_claimed", text)
        self.assertIn("command_sent", text)
        self.assertNotIn("other-property", text)

    def test_capture_never_calls_pci_mmio_or_shell(self):
        with mock.patch.object(observer, "invoke") as run:
            run.side_effect = [
                ("  +-o GFX0@0\n    +-o RedNaviProbe\n", "", 0),
                ('"navi33-p7-smu-roundtrip" = {"command_sent"=0}\n', "", 0),
                ("  10 0 org.rednavi.Navi33P4 (0.0.50)\n", "", 0),
            ]
            report = observer.capture(1)
        self.assertEqual(run.call_count, 3)
        self.assertEqual(run.call_args_list[0].args[0],
                         ["ioreg", "-p", "IOService", "-w0", "-t"])
        self.assertEqual(run.call_args_list[1].args[0],
                         ["ioreg", "-p", "IOService", "-r", "-l", "-w0",
                          "-c", "RedNaviProbe"])
        self.assertEqual(run.call_args_list[2].args[0],
                         ["kmutil", "showloaded"])
        self.assertIn("command_sent", report)
        self.assertIn("not proven open owners", report)
        self.assertIn("does NOT exclude", report)


if __name__ == "__main__":
    unittest.main()
