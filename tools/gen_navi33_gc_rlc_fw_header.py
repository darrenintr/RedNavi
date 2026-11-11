#!/usr/bin/env python3
"""Generate a pinned Navi33 GC11.0.2 RLC-G PSP firmware header.

Input is the unchanged, vendor-supplied gc_11_0_2_rlc.bin fetched from
beagleboard/mirror-linux-firmware at SOURCE_COMMIT. The firmware bytes
must never be committed to the RedNavi repository.

Usage:
    python3 tools/gen_navi33_gc_rlc_fw_header.py \
        /path/to/gc_11_0_2_rlc.bin RedNavi/navi33_gc_rlc_fw.h

The RLC-G payload is the Linux common firmware header's ucode portion,
NOT the whole 178848-byte signed vendor container.
"""
import argparse
import hashlib
from pathlib import Path
import struct

SOURCE_REPO = "beagleboard/mirror-linux-firmware"
SOURCE_COMMIT = "2f411c10e4570d03426d3cb095471aa1120114ab"
SOURCE_NAME = "gc_11_0_2_rlc.bin"
SOURCE_BLOB = "062f6da3e670b256d537ef52ee9b9ad44f1e4b8c"
SOURCE_BYTES = 178848
MAX_STAGE_BYTES = 270336


def git_blob_sha1(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def parse_image(data):
    if len(data) < 104:
        raise ValueError("RLC firmware shorter than v2.0 header")
    (file_bytes, header_bytes, header_major, header_minor,
     ip_major, ip_minor, version, ucode_bytes, ucode_offset, crc) = (
        struct.unpack_from("<IIHHHHIIII", data, 0)
    )
    if file_bytes != len(data):
        raise ValueError("declared RLC file size mismatch")
    if header_major != 2 or header_minor not in (0, 1, 2, 3, 4, 5):
        raise ValueError("unsupported RLC firmware header version")
    if (ip_major, ip_minor) != (11, 0):
        raise ValueError("not GC11.0.x firmware")
    if header_bytes < 104 or header_bytes > len(data) or header_bytes % 4:
        raise ValueError("invalid RLC header size")
    if (not ucode_bytes or ucode_bytes % 4 or ucode_bytes > MAX_STAGE_BYTES
            or ucode_offset % 4 or ucode_offset < header_bytes
            or ucode_offset > len(data)
            or ucode_bytes > len(data) - ucode_offset):
        raise ValueError("invalid RLC-G executable payload size/offset")
    return {
        "version": version,
        "header_major": header_major,
        "header_minor": header_minor,
        "ucode_bytes": ucode_bytes,
        "ucode_offset": ucode_offset,
        "code": data[ucode_offset:ucode_offset + ucode_bytes],
        "crc32_field": crc,
    }


def generate(source, target):
    source = Path(source)
    target = Path(target)
    if source.name != SOURCE_NAME:
        raise ValueError("wrong firmware basename (Navi33 RLC only)")
    data = source.read_bytes()
    if len(data) != SOURCE_BYTES:
        raise ValueError("unreviewed RLC source length")
    actual = git_blob_sha1(data)
    if actual != SOURCE_BLOB:
        raise ValueError("unreviewed RLC source Git blob SHA1")
    p = parse_image(data)
    sha256 = hashlib.sha256(data).hexdigest()
    with target.open("w", encoding="utf-8") as out:
        out.write("// Locally generated pinned Navi33 RLC-G; DO NOT COMMIT.\n")
        out.write("#define RN_NAVI33_GC_RLC_FW_METADATA_V1 1\n")
        out.write("#define RN_NAVI33_GC_RLC_FW_SOURCE_VERIFIED 1\n")
        out.write("#define kRNNavi33GcRlcIpMajor 11u\n")
        out.write("#define kRNNavi33GcRlcIpMinor 0u\n")
        out.write("#define kRNNavi33GcRlcIpRevision 2u\n")
        out.write(f"#define kRNNavi33GcRlcSourceBytes {len(data)}u\n")
        out.write(f"#define kRNNavi33GcRlcVersion {p['version']}u\n")
        out.write(f"#define kRNNavi33GcRlcHeaderMajor {p['header_major']}u\n")
        out.write(f"#define kRNNavi33GcRlcHeaderMinor {p['header_minor']}u\n")
        out.write(f"#define kRNNavi33GcRlcGBytes {p['ucode_bytes']}u\n")
        out.write(f'#define kRNNavi33GcRlcSourceBlob "{actual}"\n')
        out.write(f'#define kRNNavi33GcRlcSourceSha256 "{sha256}"\n')
        out.write("static const unsigned char kRNNavi33GcRlcGFw[] = {\n")
        for off in range(0, len(p["code"]), 16):
            out.write("    " + ", ".join(
                f"0x{x:02x}" for x in p["code"][off:off+16]
            ) + ",\n")
        out.write("};\n")
    print(
        f"Navi33 RLC-G (header {p['header_major']}.{p['header_minor']}, "
        f"version {p['version']}); PSP type 8, payload {p['ucode_bytes']} B; "
        f"source blob {actual}; sha256 {sha256}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("target")
    args = parser.parse_args()
    generate(args.source, args.target)


if __name__ == "__main__":
    main()
