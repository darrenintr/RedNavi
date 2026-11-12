"""Offline MES_2 scheduler/MES1 KIQ parser tests; no vendor firmware in CI."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "gen_navi33_mes_fw_header.py"
SPEC = importlib.util.spec_from_file_location("navi33_mes_gen", SCRIPT)
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def fixture():
    b = bytearray(224)
    struct.pack_into("<IIHHHHIIII", b, 0,
                     224, 72, 1, 0, 11, 0, 0, 96, 128, 0)
    struct.pack_into("<IIIIIIIIII", b, 32,
                     7, 64, 128, 8, 32, 192,
                     0xF0000000, 0, 0xF0001000, 0)
    b[128:192] = bytes(range(64))
    b[192:224] = bytes(range(32))
    return b


class MesTests(unittest.TestCase):
    def test_distinct_code_and_data(self):
        p = M.parse_mes(fixture())
        self.assertEqual(p["code_bytes"], 64)
        self.assertEqual(p["data_bytes"], 32)
        self.assertEqual(p["code"], bytes(range(64)))
        self.assertEqual(p["data"], bytes(range(32)))
        self.assertEqual(p["code_pc"], 0xF0000000)

    def test_corrupted_headers_fail_closed(self):
        for name in ("length", "header", "ip", "version",
                     "code_empty", "data_empty", "code_odd",
                     "bad_code_offset", "overlap", "data_oob",
                     "truncated"):
            b = fixture()
            if name == "length": struct.pack_into("<I", b, 0, 222)
            elif name == "header": struct.pack_into("<I", b, 4, 44)
            elif name == "ip": struct.pack_into("<H", b, 12, 12)
            elif name == "version": struct.pack_into("<H", b, 8, 2)
            elif name == "code_empty": struct.pack_into("<I", b, 36, 0)
            elif name == "data_empty": struct.pack_into("<I", b, 48, 0)
            elif name == "code_odd": struct.pack_into("<I", b, 36, 65)
            elif name == "bad_code_offset": struct.pack_into("<I", b, 40, 32)
            elif name == "overlap": struct.pack_into("<I", b, 52, 128)
            elif name == "data_oob": struct.pack_into("<I", b, 52, 224)
            elif name == "truncated": b = b[:40]
            with self.subTest(name=name), self.assertRaises(ValueError):
                M.parse_mes(b)

    def test_source_is_exactly_pinned(self):
        with tempfile.TemporaryDirectory() as directory:
            sched = Path(directory) / M.PINNED["Sched"][0]
            kiq = Path(directory) / M.PINNED["Kiq"][0]
            output = Path(directory) / "out.h"
            sched.write_bytes(bytes(M.PINNED["Sched"][1]))
            kiq.write_bytes(bytes(M.PINNED["Kiq"][1]))
            with self.assertRaises(ValueError):
                M.generate(sched, kiq, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
