"""Host-only tests for tools/gen_psp_fw_header.py packed PSP parsing."""
import importlib.util
import pathlib
import struct
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "gen_psp_fw_header", ROOT / "tools/gen_psp_fw_header.py"
)
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)


def image(minor: int, descriptors, aux_index=0):
    desc_start = 36 if minor == 0 else 40
    header_size = desc_start + 16 * len(descriptors)
    uoff = (header_size + 15) & ~15
    payload = bytearray()
    packed = []
    for typ, version, data in descriptors:
        off = len(payload)
        payload += data
        packed.append((typ, version, off, len(data)))
    b = bytearray(uoff + len(payload))
    struct.pack_into("<IIHH", b, 0, len(b), header_size, 2, minor)
    struct.pack_into("<I", b, 24, uoff)
    struct.pack_into("<I", b, 32, len(descriptors))
    if minor == 1:
        struct.pack_into("<I", b, 36, aux_index)
    for i, desc in enumerate(packed):
        struct.pack_into("<IIII", b, desc_start + 16 * i, *desc)
    b[uoff:] = payload
    return bytes(b)


def parse_bytes(data: bytes):
    with tempfile.TemporaryDirectory() as td:
        p = pathlib.Path(td) / GEN.TARGET_FILE
        p.write_bytes(data)
        return GEN.parse(p)


class PspGeneratorTests(unittest.TestCase):
    def test_v20_descriptor_layout(self):
        data = image(0, [
            (2, 0x11, b"S" * 32),  # SYS_DRV
            (1, 0x22, b"O" * 48),  # SOS
        ])
        _blob, major, minor, found = parse_bytes(data)
        self.assertEqual((major, minor), (2, 0))
        self.assertEqual(found["SYS_DRV"], (0x11, b"S" * 32))
        self.assertEqual(found["SOS"], (0x22, b"O" * 48))

    def test_v21_skips_aux_half_like_linux_normal_path(self):
        data = image(1, [
            (2, 1, b"A" * 16),  # normal SYS
            (1, 2, b"B" * 16),  # normal SOS
            (2, 3, b"C" * 16),  # auxiliary copy: must not replace normal
            (1, 4, b"D" * 16),
        ], aux_index=2)
        _blob, major, minor, found = parse_bytes(data)
        self.assertEqual((major, minor), (2, 1))
        self.assertEqual(found["SYS_DRV"][0], 1)
        self.assertEqual(found["SOS"][0], 2)

    def test_requires_sys_and_sos(self):
        with self.assertRaisesRegex(ValueError, "SYS_DRV"):
            parse_bytes(image(0, [(1, 1, b"O" * 16)]))
        with self.assertRaisesRegex(ValueError, "SOS"):
            parse_bytes(image(0, [(2, 1, b"S" * 16)]))

    def test_rejects_bad_aux_index_and_large_component(self):
        with self.assertRaisesRegex(ValueError, "aux index"):
            parse_bytes(image(1, [
                (2, 1, b"S" * 16),
                (1, 1, b"O" * 16),
            ], aux_index=3))
        with self.assertRaisesRegex(ValueError, "exceeds"):
            parse_bytes(image(0, [
                (2, 1, b"S" * (GEN.PSP_1_MEG + 1)),
                (1, 1, b"O" * 16),
            ]))

    def test_bootloader_commands_match_linux_psp13(self):
        commands = dict(GEN.ORDER)
        self.assertEqual(commands["SYS_DRV"], 0x10000)
        self.assertEqual(commands["SOS"], 0x20000)
        self.assertEqual(commands["KDB"], 0x80000)
        self.assertEqual(commands["SOC_DRV"], 0xB0000)
        self.assertEqual(commands["DBG_DRV"], 0xC0000)
        self.assertEqual(commands["INTF_DRV"], 0xD0000)
        self.assertEqual(commands["RAS_DRV"], 0xE0000)
        self.assertEqual(commands["IPKEYMGR_DRV"], 0xF0000)
        self.assertEqual(commands["SPL"], 0x10000000)
        self.assertEqual(commands["SPDM_DRV"], 0x20000000)


if __name__ == "__main__":
    unittest.main()
