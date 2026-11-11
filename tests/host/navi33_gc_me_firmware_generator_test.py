#!/usr/bin/env python3
"""Host-only tests for pinned GC11.0.2 RS64 ME generator."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "navi33_gc_me_firmware_generator",
    ROOT / "tools" / "gen_navi33_gc_me_fw_header.py",
)
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)


def synthetic_source():
    b = bytearray(GEN.REVIEWED_FILE_BYTES)
    code_off, code_size = 256, 174992
    data_off, data_size = code_off + code_size, 139264
    struct.pack_into("<IIHHHHIIII", b, 0, len(b), 60, 2, 0, 11, 0,
                     2380, code_size + data_size, code_off, 0)
    struct.pack_into("<IIIIIII", b, 32,
                     29, code_size, code_off, data_size, data_off, 12288, 458752)
    b[code_off:code_off + code_size] = b"\x23" * code_size
    b[data_off:data_off + data_size] = b"\x45" * data_size
    return bytes(b)


class Navi33GcMeTests(unittest.TestCase):
    def test_git_blob(self):
        self.assertEqual(GEN.git_blob_sha1(b"test"),
                         hashlib.sha1(b"blob 4\0test").hexdigest())

    def test_extract_only_instructions(self):
        x = GEN.parse_image(synthetic_source())
        self.assertEqual(x["version"], 2380)
        self.assertEqual(x["code_bytes"], 174992)
        self.assertEqual(x["data_bytes"], 139264)
        self.assertEqual(x["code"][0:1], b"\x23")
        self.assertEqual(x["code"][-1:], b"\x23")
        self.assertEqual(len(x["data"]), 139264)
        self.assertEqual(x["data"][:1], b"\x45")
        self.assertEqual(x["data"][-1:], b"\x45")
        self.assertEqual(x["start_lo"], 12288)

    def test_reject_unpinned_source_even_matching_filename(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / GEN.TARGET_BASENAME
            dst = Path(td) / "out.h"
            src.write_bytes(synthetic_source())
            with self.assertRaises(ValueError):
                GEN.generate(src, dst)
            self.assertFalse(dst.exists())

    def test_wrong_filename(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "gc_11_0_3_me.bin"
            src.write_bytes(synthetic_source())
            with self.assertRaises(ValueError):
                GEN.generate(src, Path(td) / "out.h")

    def test_codegen_with_synthetic_opt_in_test_override(self):
        data = synthetic_source()
        saved = GEN.REVIEWED_GIT_BLOB_SHA1
        GEN.REVIEWED_GIT_BLOB_SHA1 = GEN.git_blob_sha1(data)
        try:
            with tempfile.TemporaryDirectory() as td:
                src = Path(td) / GEN.TARGET_BASENAME
                dst = Path(td) / "out.h"
                src.write_bytes(data)
                GEN.generate(src, dst)
                output = dst.read_text()
                self.assertIn("RN_NAVI33_GC_ME_FW_SOURCE_VERIFIED 1", output)
                self.assertIn("kRNNavi33GcMeIpRevision 2u", output)
                self.assertIn("kRNNavi33GcMeCodeBytes 174992u", output)
                self.assertIn("kRNNavi33GcMeDataBytes 139264u", output)
                self.assertIn("static const unsigned char kRNNavi33GcMeCodeFw[]", output)
        finally:
            GEN.REVIEWED_GIT_BLOB_SHA1 = saved

    def test_header_v1_rejected(self):
        b = bytearray(synthetic_source())
        struct.pack_into("<H", b, 8, 1)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)

    def test_wrong_ip_rejected(self):
        b = bytearray(synthetic_source())
        struct.pack_into("<H", b, 12, 10)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)

    def test_bad_code_offset_rejected(self):
        b = bytearray(synthetic_source())
        struct.pack_into("<I", b, 40, 0)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)

    def test_overlapping_code_data_rejected(self):
        b = bytearray(synthetic_source())
        struct.pack_into("<I", b, 48, 4096)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)

    def test_truncated_rejected(self):
        with self.assertRaises(ValueError):
            GEN.parse_image(synthetic_source()[:-1])

    def test_excessive_code_rejected(self):
        b = bytearray(synthetic_source())
        struct.pack_into("<I", b, 36, GEN.MAX_STAGING_BYTES+4096)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)


if __name__ == "__main__":
    unittest.main()
