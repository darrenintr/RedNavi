"""Static checks for the isolated, explicit RX7600 RedNavi P4 experiment.
No hardware access or driver load.
"""
import pathlib
import plistlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def info(path):
    return plistlib.loads((ROOT / path).read_bytes())


class Navi33P4BundleTests(unittest.TestCase):
    def test_target_has_separate_identity_and_no_framebuffer_match(self):
        p = info("RedNaviP4/Info.plist.in")
        self.assertEqual(p["CFBundleIdentifier"], "org.rednavi.Navi33P4")
        self.assertEqual(p["CFBundleExecutable"], "RedNaviNavi33P4")
        self.assertEqual(p["CFBundlePackageType"], "KEXT")
        personality = p["IOKitPersonalities"]["RedNaviNavi33P4"]
        self.assertEqual(personality["IOPCIPrimaryMatch"], "0x74801002")
        self.assertEqual(personality["IOProviderClass"], "IOPCIDevice")
        self.assertEqual(personality["IOClass"], "RedNaviProbe")
        self.assertEqual(personality["IOMatchCategory"], "RedNaviP4Experimental")
        self.assertNotEqual(personality["IOMatchCategory"], "IOFramebuffer")

    def test_production_still_does_not_bind_navi33(self):
        p = info("RedNavi/Info.plist.in")
        m = next(iter(p["IOKitPersonalities"].values()))
        self.assertNotIn("0x74801002", m["IOPCIPrimaryMatch"].lower())
        self.assertEqual(m["IOMatchCategory"], "IOFramebuffer")

    def test_compile_time_and_boot_time_both_required(self):
        mk = (ROOT / "Makefile").read_text()
        self.assertIn("-DRN_ENABLE_NAVI33_UNSAFE_P4", mk)
        normal = re.search(r"(?m)^\\$\\(OUT\\)/kern_probe\\.o:.*?(?=\\n\\n)", mk, re.S)
        self.assertIsNotNone(normal)
        self.assertNotIn("RN_ENABLE_NAVI33_UNSAFE_P4", normal.group())
        special = re.search(r"(?m)^\\$\\(OUT\\)/navi33_p4_kern_probe\\.o:.*?(?=\\n\\n)", mk, re.S)
        self.assertIsNotNone(special)
        self.assertIn("-DRN_ENABLE_NAVI33_UNSAFE_P4", special.group())
        self.assertIn("navi33-p4: $(P4KEXT)", mk)
        self.assertNotIn("$(P4KEXT)", re.search(r"(?m)^all:.*$", mk).group())
        source = (ROOT / "RedNavi/kern_probe.cpp").read_text()
        self.assertIn('PE_parse_boot_argn("rednavi_navi33_unsafe_p4"', source)
        self.assertIn("ProbeAdmission::ExperimentalOnly", source)
        self.assertLess(source.index("ProbeAdmission::ExperimentalOnly"), source.index("pci->setMemoryEnable(true);", source.index("bool RedNaviProbe::start")))

    def test_no_default_ci_upload(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        self.assertNotIn("RedNaviNavi33P4.kext", workflow)
        self.assertNotIn("navi33-p4", re.search(r"(?m)^\\s+- name: Build\\n.*?(?=\\n\\s+- name:|\\Z)", workflow, re.S).group())


if __name__ == "__main__":
    unittest.main()
