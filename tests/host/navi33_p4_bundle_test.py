"""Static validation: opt-in RedNavi Navi33 hardware P4 bundle.

These tests perform no kernel extension loading or GPU I/O.
"""
import pathlib
import plistlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def info(path):
    return plistlib.loads((ROOT / path).read_bytes())


class Navi33P4BundleTests(unittest.TestCase):
    def test_experimental_bundle_is_not_the_framebuffer_driver(self):
        p = info("RedNaviP4/Info.plist.in")
        self.assertEqual(p["CFBundleIdentifier"], "org.rednavi.Navi33P4")
        self.assertEqual(p["CFBundleExecutable"], "RedNaviNavi33P4")
        match = p["IOKitPersonalities"]["RedNaviNavi33P4"]
        self.assertEqual(match["IOPCIPrimaryMatch"], "0x74801002")
        self.assertEqual(match["IOProviderClass"], "IOPCIDevice")
        self.assertEqual(match["IOClass"], "RedNaviProbe")
        self.assertEqual(match["IOMatchCategory"], "RedNaviP4Experimental")

    def test_production_personality_still_excludes_rx7600(self):
        p = info("RedNavi/Info.plist.in")
        match = next(iter(p["IOKitPersonalities"].values()))
        self.assertNotIn("0x74801002", match["IOPCIPrimaryMatch"].lower())
        self.assertEqual(match["IOMatchCategory"], "IOFramebuffer")

    def test_p4_is_compiled_separately_and_requires_boot_opt_in(self):
        mk = (ROOT / "Makefile").read_text()
        regular = mk.split("$(OUT)/kern_probe.o:", 1)[1].split("\n\n", 1)[0]
        experimental = mk.split("$(OUT)/navi33_p4_kern_probe.o:", 1)[1].split("\n\n", 1)[0]
        self.assertNotIn("RN_ENABLE_NAVI33_UNSAFE_P4", regular)
        self.assertIn("-DRN_ENABLE_NAVI33_UNSAFE_P4", experimental)
        self.assertIn("navi33-p4: $(P4KEXT)", mk)
        self.assertNotIn("$(P4KEXT)", mk.split("\nall:", 1)[1].split("\n", 1)[0])
        src = (ROOT / "RedNavi/kern_probe.cpp").read_text()
        start = src[src.index("bool RedNaviProbe::start(IOService *provider) {"):]
        self.assertIn('PE_parse_boot_argn("rednavi_navi33_unsafe_p4"', start)
        self.assertLess(start.index("ProbeAdmission::ExperimentalOnly"),
                        start.index("pci->setMemoryEnable(true)"))

    def test_ci_compiles_only_and_does_not_upload_p4(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        self.assertIn("make navi33-p4", workflow)
        self.assertNotIn("RedNaviNavi33P4.kext", workflow)


if __name__ == "__main__":
    unittest.main()
