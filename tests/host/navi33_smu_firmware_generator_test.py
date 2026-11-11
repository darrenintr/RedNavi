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
    array_off = 64
    data = bytearray(array_off + len(payload))
    struct.pack_into(
        "<IIHHHHIIII", data, 0,
        len(data), 32, 1, 0, 13, 0,
        ucode_version, len(payload), array_off, 0
    )
    data[array_off:] = payload
    return bytes(data)


class SmuGeneratorTests(unittest.TestCase):
    def run_generator(self, target="navi33", data=None, filename=None,
                      expected=True):
        data = make_image() if data is None else data
        digest = hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryDirectory() as td:
            src = pathlib.Path(td) / (filename or GEN.TARGETS[target]["basename"])
            out = pathlib.Path(td) / "smu_fw.h"
            src.write_bytes(data)
            args = [str(src), str(out), "--target", target]
            if expected:
                args.extend(["--expected-sha256", digest])
            GEN.main(args)
            return out.read_text()

    def test_emits_exact_ucode_and_unverified_synthetic_target(self):
        text = self.run_generator()
        self.assertIn("#define RN_SMU_FW_METADATA_V2 1", text)
        self.assertIn("#define RN_SMU_FW_DIGEST_TRUSTED 1", text)
        self.assertIn("#define RN_SMU_FW_TARGET_VERIFIED 0", text)
        self.assertIn("#define kRNSmuFwTargetMp1Revision 7u", text)
        self.assertIn("#define kRNSmuFwVersion 0x505600u", text)
        self.assertIn("#define kRNSmuFwSize 256u", text)
        self.assertIn("0x50, 0x4d, 0x46, 0x57", text)

    def test_navi32_is_explicit(self):
        text = self.run_generator("navi32")
        self.assertIn("#define kRNSmuFwTargetMp1Revision 10u", text)
        self.assertIn("#define RN_SMU_FW_TARGET_VERIFIED 0", text)

    def test_without_expected_digest_is_untrusted(self):
        text = self.run_generator(expected=False)
        self.assertIn("#define RN_SMU_FW_DIGEST_TRUSTED 0", text)

    def test_wrong_filename_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.run_generator(filename="smu_13_0_10.bin")

    def test_missing_target_is_rejected(self):
        with self.assertRaises(SystemExit):
            GEN.parse_args(["a.bin", "b.h"])

    def test_digest_mismatch_is_rejected(self):
        data = make_image()
        with tempfile.TemporaryDirectory() as td:
            src = pathlib.Path(td) / "smu_13_0_7.bin"
            src.write_bytes(data)
            with self.assertRaises(SystemExit):
                GEN.main([str(src), str(pathlib.Path(td)/"o.h"), "--target",
                          "navi33", "--expected-sha256", "00" * 32])

    def test_renamed_known_other_target_bytes_are_rejected(self):
        data = make_image()
        digest = hashlib.sha256(data).hexdigest()
        GEN.REVIEWED_SHA256_TARGET[digest] = "navi32"
        try:
            with self.assertRaises(SystemExit):
                self.run_generator("navi33", data)
        finally:
            del GEN.REVIEWED_SHA256_TARGET[digest]

    def test_reviewed_target_signature_is_separate_from_caller_hash(self):
        data = make_image()
        digest = hashlib.sha256(data).hexdigest()
        GEN.REVIEWED_SHA256_TARGET[digest] = "navi33"
        try:
            text = self.run_generator(data=data)
            self.assertIn("#define RN_SMU_FW_TARGET_VERIFIED 1", text)
            self.assertIn("#define RN_SMU_FW_DIGEST_TRUSTED 1", text)
        finally:
            del GEN.REVIEWED_SHA256_TARGET[digest]

    def test_rejects_short_header(self):
        with self.assertRaises(ValueError):
            GEN.parse_firmware(b"\0" * 31)

    def test_rejects_wrong_file_size(self):
        data = bytearray(make_image())
        struct.pack_into("<I", data, 0, len(data) + 1)
        with self.assertRaises(ValueError):
            GEN.parse_firmware(bytes(data))

    def test_rejects_out_of_bounds_ucode(self):
        data = bytearray(make_image())
        struct.pack_into("<I", data, 20, 0x100000)
        with self.assertRaises(ValueError):
            GEN.parse_firmware(bytes(data))

    def test_rejects_bad_common_ip(self):
        data = bytearray(make_image())
        struct.pack_into("<H", data, 12, 12)
        with self.assertRaises(ValueError):
            GEN.parse_firmware(bytes(data))


if __name__ == "__main__":
    unittest.main()
