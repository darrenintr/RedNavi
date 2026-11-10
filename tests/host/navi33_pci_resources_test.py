"""Host-only tests using real RX 7600 macOS and Linux PCI BAR measurements.

No PCI, IOKit, GPU, kernel-extension, or firmware operations.
"""
import struct
import unittest

from tools.navi33_pci_resources import decode_assigned_addresses, describe_navi33_pci


# 2026-11-09: macOS Sequoia 15.8.2 / 1002:7480 / ASUS B450M.
# Captured through ioreg -r -l -n GFX0, 5 OF PCI cells per entry.
MACOS_CAPTURE = bytes.fromhex(
    "100009c208000000000000300000000000000010"
    "180009c208000000000000400000000000002000"
    "200009810000000000e000000000000000010000"
    "2400098200000000000090fc0000000000001000"
    "30000982000000000000a0fc0000000000000200"
)


def record(register, address, size, space=2):
    return struct.pack("<IIIII", (space << 24) | register,
                       address >> 32, address & 0xffffffff,
                       size >> 32, size & 0xffffffff)


class MacosPciResourceTests(unittest.TestCase):
    def test_observed_macos_bar_layout(self):
        bars = decode_assigned_addresses(MACOS_CAPTURE)
        self.assertEqual(len(bars), 5)
        by = {bar["name"]: bar for bar in bars}
        self.assertEqual(int(by["BAR0"]["cpu_physical_base"], 16), 0x830000000)
        self.assertEqual(by["BAR0"]["length_bytes"], 256 << 20)
        self.assertEqual(int(by["BAR2"]["cpu_physical_base"], 16), 0x840000000)
        self.assertEqual(by["BAR2"]["length_bytes"], 2 << 20)
        self.assertEqual(by["BAR4"]["kind"], "io")
        self.assertEqual(int(by["BAR4"]["cpu_physical_base"], 16), 0xe000)
        self.assertEqual(int(by["BAR5"]["cpu_physical_base"], 16), 0xfc900000)
        self.assertEqual(by["BAR5"]["length_bytes"], 1 << 20)
        self.assertEqual(int(by["Expansion ROM"]["cpu_physical_base"], 16), 0xfca00000)
        self.assertEqual(by["Expansion ROM"]["length_bytes"], 128 << 10)
        node = {
            "assigned-addresses": MACOS_CAPTURE,
            "IOPCIExpressLinkStatus": 4228,
            "IODeviceMemory": [
                {"address": 0x830000000, "length": 256 << 20},
                {"address": 0x840000000, "length": 2 << 20},
                "IOSubMemoryDescriptor is not serializable",
                {"address": 0xfc900000, "length": 1 << 20},
                {"address": 0xfca00000, "length": 128 << 10}
            ]
        }
        report = describe_navi33_pci(node)
        self.assertEqual(report["status"], "ok")
        self.assertEqual(report["p4_p5_resource_gate"], "pass")
        self.assertEqual(report["reported_pcie_link"]["raw"], "0x1084")
        self.assertEqual(report["reported_pcie_link"]["reported_width"], 8)
        self.assertTrue(all(bar["matches_iodevice_memory"] is True
                            for bar in report["bars"] if bar["kind"] == "memory"))

    def test_linux_rebar_not_treated_as_vram_address(self):
        # Linux measured BAR0 8192MiB; BAR2 256MiB; BAR5 1MiB.
        # The physical addresses differ from macOS; none are GPU MC addresses.
        linux = (record(0x10, 0x7c00000000, 8 << 30) +
                 record(0x18, 0x7e00000000, 256 << 20) +
                 record(0x24, 0xfc900000, 1 << 20))
        bars = decode_assigned_addresses(linux)
        report = describe_navi33_pci({"assigned-addresses": linux})
        self.assertEqual(report["p4_p5_resource_gate"], "pass")
        self.assertEqual(bars[0]["length_bytes"], 8 << 30)
        self.assertEqual(bars[1]["length_bytes"], 256 << 20)
        self.assertEqual(bars[2]["length_bytes"], 1 << 20)

    def test_rejects_corrupt_resources_without_pci_access(self):
        with self.assertRaises(ValueError):
            decode_assigned_addresses(MACOS_CAPTURE[:-1])
        with self.assertRaises(ValueError):
            decode_assigned_addresses(b"")
        with self.assertRaises(ValueError):
            decode_assigned_addresses(MACOS_CAPTURE + MACOS_CAPTURE[:20])
        with self.assertRaises(ValueError):
            decode_assigned_addresses(record(0x10, 0x830000001, 256 << 20))
        with self.assertRaises(ValueError):
            decode_assigned_addresses(record(0x10, 0x830000000, (256 << 20) + 4096))
        with self.assertRaises(ValueError):
            decode_assigned_addresses(record(0x10, 0x830000000, 256 << 20) +
                                      record(0x18, 0x830000000, 256 << 20))

    def test_missing_or_short_bar_fails_gate(self):
        short = (record(0x10, 0x830000000, 256 << 20) +
                 record(0x18, 0x840000000, 2 << 20) +
                 record(0x24, 0xfc900000, 64 << 10))
        report = describe_navi33_pci({"assigned-addresses": short})
        self.assertEqual(report["p4_p5_resource_gate"], "fail")
        self.assertIn("BAR5 type/length is insufficient", report["issues"])
        report = describe_navi33_pci({"assigned-addresses": record(0x10, 0x830000000, 256 << 20)})
        self.assertEqual(report["p4_p5_resource_gate"], "fail")
        self.assertIn("missing BAR2", report["issues"])
        self.assertIn("missing BAR5", report["issues"])

    def test_descriptor_disagreement_is_not_silently_ignored(self):
        node = {
            "assigned-addresses": MACOS_CAPTURE,
            "IODeviceMemory": [{"address": 0xdead0000, "length": 4096}],
        }
        report = describe_navi33_pci(node)
        self.assertEqual(report["status"], "incomplete")
        self.assertEqual(report["p4_p5_resource_gate"], "fail")
        self.assertIn("BAR0 disagrees with IODeviceMemory", report["issues"])

    def test_invalid_input_is_not_a_pass(self):
        report = describe_navi33_pci({"assigned-addresses": b"incorrect"})
        self.assertEqual(report["status"], "invalid")
        self.assertEqual(report["p4_p5_resource_gate"], "fail")


if __name__ == "__main__":
    unittest.main()
