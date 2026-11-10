"""Static safety contract: RX 7600 must not match legacy framebuffer probe.

This does not certify kernel attachment is safe. No GPU/device operations.
"""
import pathlib
import plistlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


def personality(path):
    root = plistlib.loads((ROOT / path).read_bytes())
    return next(iter(root["IOKitPersonalities"].values()))


class Navi33DiagIsolationTests(unittest.TestCase):
    def test_rx7600_removed_from_legacy_framebuffer_personality(self):
        legacy = personality("RedNavi/Info.plist.in")
        ids = legacy["IOPCIPrimaryMatch"].lower().split()
        self.assertIn("0x747e1002", ids)  # Preserve legacy Navi32 M4 path.
        self.assertNotIn("0x74801002", ids)
        self.assertEqual(legacy["IOMatchCategory"], "IOFramebuffer")

    def test_diagnostic_uses_separate_category_and_exact_id(self):
        diag = personality("RedNaviDiag/Info.plist.in")
        self.assertEqual(diag["IOPCIPrimaryMatch"].lower(), "0x74801002")
        self.assertEqual(diag["IOProviderClass"], "IOPCIDevice")
        self.assertEqual(diag["IOClass"], "RedNaviNavi33Diag")
        self.assertEqual(diag["IOMatchCategory"], "RedNaviDiagnostic")
        self.assertLess(diag["IOProbeScore"], 100)
        self.assertNotEqual(diag["IOMatchCategory"], "IOFramebuffer")

    def test_diag_cpp_has_no_register_access_or_gpu_writes(self):
        source = (ROOT / "RedNaviDiag/kern_diag.cpp").read_text()
        # Strip comments before searching for actual calls.
        source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
        forbidden = [
            r"\bconfigWrite(?:8|16|32)\s*\(",
            r"\bsetMemoryEnable\s*\(",
            r"\bmapDeviceMemoryWithRegister\s*\(",
            r"\bmapDeviceMemoryWithIndex\s*\(",
            r"\bcreateMappingInTask\s*\(",
            r"\b(?:wr|vramWrite|rn_mes_start|rn_vm_bind|rn_vm_flush)\s*\(",
            r"\bMM_INDEX\b",
        ]
        for expression in forbidden:
            with self.subTest(expression=expression):
                self.assertNotRegex(source, expression)
        self.assertIn("getDeviceMemoryWithRegister", source)
        self.assertIn("configRead16", source)

    def test_not_built_by_default_or_uploaded_by_ci(self):
        makefile = (ROOT / "Makefile").read_text()
        self.assertIn("diag: $(DIAGKEXT)", makefile)
        self.assertNotIn("$(DIAGKEXT)", re.search(r"(?m)^all:.*$", makefile).group())
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        # CI can compile this target but must not publish/inject it.
        self.assertNotIn("RedNaviNavi33Diag.kext", workflow)


if __name__ == "__main__":
    unittest.main()
