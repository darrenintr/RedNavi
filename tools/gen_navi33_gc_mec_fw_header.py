#!/usr/bin/env python3
"""Generate a pinned, local Navi33 GC11.0.2 RS64 MEC instruction PSP header.

Source: beagleboard/mirror-linux-firmware at fixed commit
2f411c10e4570d03426d3cb095471aa1120114ab,
amdgpu/gc_11_0_2_mec.bin (reviewed Git blob SHA-1).
No proprietary binary, payload, or generated header is committed.

Usage:
    python3 tools/gen_navi33_gc_mec_fw_header.py \
      /path/gc_11_0_2_mec.bin RedNavi/navi33_gc_mec_fw.h
"""
import argparse
import hashlib
from pathlib import Path
import struct

TARGET_BASENAME = "gc_11_0_2_mec.bin"
SOURCE_REPO = "beagleboard/mirror-linux-firmware"
SOURCE_COMMIT = "2f411c10e4570d03426d3cb095471aa1120114ab"
REVIEWED_GIT_BLOB_SHA1 = "48e4b8f68763f21196de71732f319805ec8483dd"
REVIEWED_FILE_BYTES = 406528
MAX_STAGING_BYTES = 1 << 20


def git_blob_sha1(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def parse_image(data):
    if len(data) < 60:
        raise ValueError("GC RS64 MEC file truncated below header v2.0")
    file_bytes, header_bytes, vmajor, vminor, ipmajor, ipminor, version, \
        common_ucode_bytes, common_ucode_off, _crc = struct.unpack_from(
            "<IIHHHHIIII", data, 0)
    if file_bytes != len(data) or file_bytes > REVIEWED_FILE_BYTES:
        raise ValueError("invalid firmware declared/actual length")
    if (vmajor, vminor) != (2, 0):
        raise ValueError("expected gfx_firmware_header_v2_0 (RS64)")
    # Current common firmware header identifies GC major/minor=11.0.
    # Exact Navi33 GC11.0.2 revision is pinned by the full Git blob hash.
    if (ipmajor, ipminor) != (11, 0):
        raise ValueError("wrong GC firmware IP major/minor")
    if header_bytes < 60 or common_ucode_off < header_bytes:
        raise ValueError("invalid common/GFX header geometry")
    code_bytes = u32(data, 36)
    code_off = u32(data, 40)
    data_bytes = u32(data, 44)
    data_off = u32(data, 48)
    start_lo = u32(data, 52)
    start_hi = u32(data, 56)
    if (not code_bytes or not data_bytes or
        any(x & 3 for x in (
            common_ucode_off, common_ucode_bytes, code_bytes,
            code_off, data_bytes, data_off))):
        raise ValueError("GFX RS64 size/offset is zero or unaligned")
    if (common_ucode_off > len(data) or
        common_ucode_bytes > len(data) - common_ucode_off or
        code_off < common_ucode_off or
        code_off > len(data) or
        code_bytes > len(data) - code_off or
        data_off < code_off + code_bytes or
        data_off > len(data) or
        data_bytes > len(data) - data_off or
        code_bytes + data_bytes > common_ucode_bytes):
        raise ValueError("GC MEC code/data segments overlap or exceed source")
    if max(code_bytes, data_bytes) > MAX_STAGING_BYTES:
        raise ValueError("MEC instruction/data segment exceeds absolute staging budget")
    # MEC code is 267008 B, over the legacy 256 KiB DMA mapping.
    # Runtime separately expands the stage before PSP LOAD_IP_FW.
    return {
        "version": version,
        "code": data[code_off:code_off + code_bytes],
        "data": data[data_off:data_off + data_bytes],
        "code_bytes": code_bytes,
        "data_bytes": data_bytes,
        "code_offset": code_off,
        "data_offset": data_off,
        "start_lo": start_lo,
        "start_hi": start_hi,
    }


def generate(source, target):
    source = Path(source)
    if source.name != TARGET_BASENAME:
        raise ValueError("wrong firmware basename, refusing another ASIC")
    data = source.read_bytes()
    if len(data) != REVIEWED_FILE_BYTES:
        raise ValueError("unreviewed ME source file length")
    actual = git_blob_sha1(data)
    if actual != REVIEWED_GIT_BLOB_SHA1:
        raise ValueError(
            f"unreviewed ME source Git blob: {actual}; expected "
            f"{REVIEWED_GIT_BLOB_SHA1}; cannot self-pin input")
    parsed = parse_image(data)
    digest = hashlib.sha256(data).hexdigest()
    with Path(target).open("w", encoding="utf-8") as f:
        f.write("// Generated locally from pinned Navi33 GC11.0.2 ME; DO NOT COMMIT.\n")
        f.write("#define RN_NAVI33_GC_MEC_FW_METADATA_V1 1\n")
        f.write("#define RN_NAVI33_GC_MEC_FW_SOURCE_VERIFIED 1\n")
        f.write("#define kRNNavi33GcMecIpMajor 11u\n")
        f.write("#define kRNNavi33GcMecIpMinor 0u\n")
        f.write("#define kRNNavi33GcMecIpRevision 2u\n")
        f.write(f"#define kRNNavi33GcMecSourceBytes {len(data)}u\n")
        f.write(f"#define kRNNavi33GcMecVersion {parsed['version']}u\n")
        f.write(f"#define kRNNavi33GcMecCodeBytes {parsed['code_bytes']}u\n")
        f.write(f"#define kRNNavi33GcMecDataBytes {parsed['data_bytes']}u\n")
        f.write(f"#define kRNNavi33GcMecStartLo 0x{parsed['start_lo']:x}u\n")
        f.write(f"#define kRNNavi33GcMecStartHi 0x{parsed['start_hi']:x}u\n")
        f.write(f'static const char kRNNavi33GcMecSourceSha256[] = "{digest}";\n')
        f.write(f'static const char kRNNavi33GcMecSourceGitBlobSha1[] = "{actual}";\n')
        f.write("static const unsigned char kRNNavi33GcMecCodeFw[] = {\n")
        data_to_emit = parsed["code"]
        for offset in range(0, len(data_to_emit), 16):
            f.write("    " + ", ".join(
                f"0x{byte:02x}" for byte in data_to_emit[offset:offset+16]) +
                    ",\n")
        f.write("};\n")
        # Linux uses identical data bytes for P0/P1 ME stacks,
        # but they require separate PSP type 92 / 93 commands.
        f.write("static const unsigned char kRNNavi33GcMecDataFw[] = {\n")
        for offset in range(0, len(parsed["data"]), 16):
            f.write("    " + ", ".join(
                f"0x{byte:02x}" for byte in parsed["data"][offset:offset + 16]) +
                ",\n")
        f.write("};\n")
    print(
        f"Navi33 GC11.0.2 RS64 MEC v{parsed['version']}; "
        f"code {parsed['code_bytes']} B; data {parsed['data_bytes']} B "
        f"(ME P0/P1 stage available); Git blob {actual}; SHA-256 {digest}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("output")
    args = parser.parse_args(argv)
    generate(args.source, args.output)


if __name__ == "__main__":
    main()
