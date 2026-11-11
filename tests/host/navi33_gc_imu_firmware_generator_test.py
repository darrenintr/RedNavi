"""Host tests for pinned Navi33 IMU I/D metadata and fail-closed generator."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "gen_navi33_gc_imu_fw_header.py"
SPEC = importlib.util.spec_from_file_location("navi33_imu_generator", SCRIPT)
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def fixture():
    b = bytearray(96)
    struct.pack_into("<IIHHHHIIII", b, 0, 96, 48, 1, 0, 11, 0,
                     186730240, 16, 64, 0)
    struct.pack_into("<IIII", b, 32, 8, 0, 8, 8)
    b[64:72] = b"IMUicode"
    b[72:80] = b"IMUddddd"
    return b


class ImuGeneratorTests(unittest.TestCase):
    def test_parser_exact_two_segments(self):
        m = M.parse_image(fixture())
        self.assertEqual(m["instruction"], b"IMUicode")
        self.assertEqual(m["data"], b"IMUddddd")
        self.assertEqual(m["instruction_bytes"], 8)
        self.assertEqual(m["data_bytes"], 8)

    def test_invalid_metadata(self):
        for label in ("truncated", "length", "version", "ip", "header",
                      "array_offset", "empty_iram", "empty_dram", "bad_segments",
                      "size_mismatch", "out_of_bounds"):
            b = fixture()
            if label == "truncated": b = b[:44]
            elif label == "length": struct.pack_into("<I", b, 0, 300)
            elif label == "version": struct.pack_into("<H", b, 8, 2)
            elif label == "ip": struct.pack_into("<H", b, 12, 12)
            elif label == "header": struct.pack_into("<I", b, 4, 44)
            elif label == "array_offset": struct.pack_into("<I", b, 24, 47)
            elif label == "empty_iram": struct.pack_into("<I", b, 32, 0)
            elif label == "empty_dram": struct.pack_into("<I", b, 40, 0)
            elif label == "bad_segments": struct.pack_into("<I", b, 44, 4)
            elif label == "size_mismatch": struct.pack_into("<I", b, 20, 12)
            elif label == "out_of_bounds": struct.pack_into("<I", b, 24, 92)
            with self.subTest(label=label), self.assertRaises(ValueError):
                M.parse_image(b)

    def test_untrusted_source_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / M.SOURCE_BASENAME
            dest = Path(td) / "out.h"
            src.write_bytes(bytes(M.PINNED_SOURCE_BYTES))
            with self.assertRaises(ValueError):
                M.generate(src, dest)
            self.assertFalse(dest.exists())
            wrong = Path(td) / "gc_11_0_3_imu.bin"
            wrong.write_bytes(bytes(M.PINNED_SOURCE_BYTES))
            with self.assertRaises(ValueError):
                M.generate(wrong, dest)
            self.assertFalse(dest.exists())


if __name__ == "__main__":
    unittest.main()
