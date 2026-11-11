#!/usr/bin/env python3
"""No firmware distributed: host tests exercise synthetic SDMA2.0 files."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "navi33_sdma_generator",
    ROOT / "tools" / "gen_navi33_sdma_fw_header.py",
)
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)


def fake_image():
    b = bytearray(GEN.REVIEWED_FILE_BYTES)
    ctx_off, ctx_bytes = 256, 16384
    ctl_off, ctl_bytes = ctx_off + ctx_bytes, 8192
    struct.pack_into("<IIHHHHIIII", b, 0, len(b), 64, 2, 0, 6, 0,
                     0x10607, ctx_bytes + ctl_bytes, ctx_off, 0)
    struct.pack_into("<I", b, 36, ctx_bytes)
    struct.pack_into("<II", b, 48, ctl_off, ctl_bytes)
    b[ctx_off:ctx_off + ctx_bytes] = b"\x23" * ctx_bytes
    b[ctl_off:ctl_off + ctl_bytes] = b"\x45" * ctl_bytes
    return bytes(b)


class SdmaNavi33GeneratorTests(unittest.TestCase):
    def test_sha1_blob_format(self):
        data = b"test"
        self.assertEqual(
            GEN.git_blob_sha1(data),
            hashlib.sha1(b"blob 4\0test").hexdigest(),
        )

    def test_correct_extracts(self):
        parsed = GEN.parse_image(fake_image())
        self.assertEqual(len(parsed["ctx"]), 16384)
        self.assertEqual(len(parsed["ctl"]), 8192)
        self.assertEqual(parsed["ctx"][:1], b"\x23")
        self.assertEqual(parsed["ctl"][:1], b"\x45")
        self.assertEqual(parsed["version"], 0x10607)

    def test_fails_unreviewed_image_even_with_correct_name(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / GEN.TARGET_BASENAME
            src.write_bytes(fake_image())
            out = Path(td) / "out.h"
            with self.assertRaises(ValueError):
                GEN.generate(src, out)
            self.assertFalse(out.exists())

    def test_wrong_asic_filename_rejected_before_hash(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "sdma_6_0_3.bin"
            src.write_bytes(fake_image())
            with self.assertRaises(ValueError):
                GEN.generate(src, Path(td) / "out.h")

    def test_generated_metadata_from_reviewed_identity(self):
        data = fake_image()
        saved = GEN.REVIEWED_GIT_BLOB_SHA1
        GEN.REVIEWED_GIT_BLOB_SHA1 = GEN.git_blob_sha1(data)
        try:
            with tempfile.TemporaryDirectory() as td:
                src = Path(td) / GEN.TARGET_BASENAME
                src.write_bytes(data)
                out = Path(td) / "fw.h"
                GEN.generate(src, out)
                text = out.read_text()
                self.assertIn("RN_NAVI33_SDMA_FW_SOURCE_VERIFIED 1", text)
                self.assertIn("kRNNavi33SdmaIpRevision 2u", text)
                self.assertIn("kRNNavi33SdmaCtxBytes 16384u", text)
                self.assertIn("kRNNavi33SdmaCtlBytes 8192u", text)
                self.assertIn("static const unsigned char kRNNavi33SdmaCtxFw[]", text)
        finally:
            GEN.REVIEWED_GIT_BLOB_SHA1 = saved

    def test_invalid_header_version(self):
        b = bytearray(fake_image())
        struct.pack_into("<H", b, 8, 1)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)

    def test_invalid_ip(self):
        b = bytearray(fake_image())
        struct.pack_into("<H", b, 12, 7)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)

    def test_rejects_overlapping_thread(self):
        b = bytearray(fake_image())
        struct.pack_into("<I", b, 48, 512)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)

    def test_rejects_truncated_file(self):
        with self.assertRaises(ValueError):
            GEN.parse_image(fake_image()[:-16])

    def test_rejects_misaligned_size(self):
        b = bytearray(fake_image())
        struct.pack_into("<I", b, 36, 16385)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)

    def test_rejects_out_of_range_thread(self):
        b = bytearray(fake_image())
        struct.pack_into("<I", b, 52, 999999)
        with self.assertRaises(ValueError):
            GEN.parse_image(b)


if __name__ == "__main__":
    unittest.main()
