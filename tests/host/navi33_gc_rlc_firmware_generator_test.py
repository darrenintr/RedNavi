"""Offline admission tests for gc_11_0_2_rlc.bin payload extraction.

Synthetic fixtures must never satisfy generate()'s pinned source hash.
"""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "gen_navi33_gc_rlc_fw_header.py"
SPEC = importlib.util.spec_from_file_location("navi33_rlc_generator", SCRIPT)
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def fixture():
    src = bytearray(256)
    struct.pack_into("<IIHHHHIIII", src, 0,
                     len(src), 104, 2, 2, 11, 0,
                     12345, 64, 128, 0)
    src[128:192] = bytes(range(64))
    return src


class RlcTests(unittest.TestCase):
    def test_parser_extracts_only_payload(self):
        parsed = M.parse_image(fixture())
        self.assertEqual(parsed["ucode_bytes"], 64)
        self.assertEqual(parsed["ucode_offset"], 128)
        self.assertEqual(parsed["code"], bytes(range(64)))

    def test_rejects_malformed_source(self):
        for field in ("truncated", "version", "ip", "length",
                      "overlap", "unaligned", "zero"):
            src = fixture()
            if field == "truncated":
                src = src[:80]
            elif field == "version":
                struct.pack_into("<H", src, 8, 1)
            elif field == "ip":
                struct.pack_into("<H", src, 12, 12)
            elif field == "length":
                struct.pack_into("<I", src, 0, 111)
            elif field == "overlap":
                struct.pack_into("<I", src, 24, 48)
            elif field == "unaligned":
                struct.pack_into("<I", src, 28, 129)
            elif field == "zero":
                struct.pack_into("<I", src, 24, 0)
            with self.subTest(field=field), self.assertRaises(ValueError):
                M.parse_image(src)

    def test_refuse_unreviewed_payload_or_filename(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / M.SOURCE_NAME
            dest = Path(td) / "out.h"
            src.write_bytes(bytes(M.SOURCE_BYTES))
            with self.assertRaises(ValueError):
                M.generate(src, dest)
            self.assertFalse(dest.exists())
            wrong = Path(td) / "gc_11_0_3_rlc.bin"
            wrong.write_bytes(bytes(M.SOURCE_BYTES))
            with self.assertRaises(ValueError):
                M.generate(wrong, dest)
            self.assertFalse(dest.exists())


if __name__ == "__main__":
    unittest.main()
