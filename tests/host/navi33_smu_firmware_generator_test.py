#!/usr/bin/env python3
import hashlib
import importlib.util
import pathlib
import struct
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "gen_smu_fw_header", ROOT / "tools" / "gen_smu_fw_header.py"
)
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)


def make_image(payload=b"PMFW" * 64, ucode_version=0x00505600):
    header_size = 32
    array_off = 64
    size = array_off + len(payload)
    data = bytearray(size)
    struct.pack_into(
        "<IIHHHHIIII",
        data,
        0,
        size,
        header_size,
        1,
        0,
        13,
        0,
        ucode_version,
        len(payload),
        array_off,
        0,
    )
    data[array_off:] = payload
    return bytes(data)


class SmuGeneratorTests(unittest.TestCase):
    def test_emits_linux_ucode_payload_and_trusted_metadata(self):
        data = make_image()
        digest = hashlib.sha256(data).hexdigest()

        with tempfile.TemporaryDirectory() as td:
            src = pathlib.Path(td) / GEN.TARGET_BASENAME
            out = pathlib.Path(td) / "smu_fw.h"
            src.write_bytes(data)
            GEN.main([
                str(src), str(out), "--expected-sha256", digest
            ])
            text = out.read_text()

        self.assertIn("#define RN_SMU_FW_METADATA_V1 1", text)
        self.assertIn("#define RN_SMU_FW_DIGEST_TRUSTED 1", text)
        self.assertIn("#define kRNSmuFwVersion 0x505600u", text)
        self.assertIn("#define kRNSmuFwSize 256u", text)
        self.assertIn(digest, text)
        self.assertIn("0x50, 0x4d, 0x46, 0x57", text)

    def test_rejects_digest_mismatch(self):
        data = make_image()
        with tempfile.TemporaryDirectory() as td:
            src = pathlib.Path(td) / GEN.TARGET_BASENAME
            out = pathlib.Path(td) / "smu_fw.h"
            src.write_bytes(data)
            with self.assertRaises(SystemExit):
                GEN.main([
                    str(src), str(out), "--expected-sha256", "00" * 32
                ])

    def test_rejects_wrong_filename(self):
        data = make_image()
        digest = hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryDirectory() as td:
            src = pathlib.Path(td) / "wrong.bin"
            out = pathlib.Path(td) / "smu_fw.h"
            src.write_bytes(data)
            with self.assertRaises(SystemExit):
                GEN.main([
                    str(src), str(out), "--expected-sha256", digest
                ])

    def test_rejects_out_of_bounds_ucode(self):
        data = bytearray(make_image())
        struct.pack_into("<I", data, 20, 0x100000)
        with self.assertRaises(ValueError):
            GEN.parse_firmware(bytes(data))


if __name__ == "__main__":
    unittest.main()
