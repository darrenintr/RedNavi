#!/usr/bin/env python3
"""Generate local Navi33 SDMA 6.0.2 PSP staging bytes (never commit blobs).

The target is independently pinned to an upstream-derived linux-firmware Git
blob, not to a digest invented by the caller. The pin is a specific reviewed
snapshot of beagleboard/mirror-linux-firmware, commit 2f411c10e4570d0...
Do not silently switch versions: reviewing an updated blob is mandatory.

Usage:
    python3 tools/gen_navi33_sdma_fw_header.py \
      /path/to/sdma_6_0_2.bin RedNavi/navi33_sdma_fw.h
"""
import argparse
import hashlib
from pathlib import Path
import struct

TARGET_BASENAME = "sdma_6_0_2.bin"
TARGET_IP = (6, 0, 2)
SOURCE_REPO = "beagleboard/mirror-linux-firmware"
SOURCE_COMMIT = "2f411c10e4570d03426d3cb095471aa1120114ab"
REVIEWED_GIT_BLOB_SHA1 = "28dd03efcb3a134edcf066d0820d0c2051879525"
REVIEWED_FILE_BYTES = 34560
MAX_STAGING_BYTES = 1 << 20


def git_blob_sha1(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def u32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def parse_image(data):
    if len(data) < 64:
        raise ValueError("SDMA2.0 image is shorter than its 64-byte header")
    (file_bytes, header_bytes, hvmajor, hvminor, ipmajor, ipminor,
     version, ucode_bytes, array_off, _crc) = struct.unpack_from(
        "<IIHHHHIIII", data, 0)
    if len(data) != file_bytes:
        raise ValueError("common header file-size mismatch")
    if (hvmajor, hvminor) != (2, 0):
        raise ValueError("expected SDMA firmware header v2.0")
    # SDMA 6.0.x firmware common-header IP is the SDMA block's 6.0;
    # its exact revision is pinned by the reviewed full-image Git blob.
    if (ipmajor, ipminor) != (6, 0):
        raise ValueError("wrong SDMA firmware IP major/minor")
    if header_bytes < 64 or array_off < header_bytes:
        raise ValueError("invalid SDMA header or ucode offset")
    ctx_bytes = u32(data, 36)
    ctl_off = u32(data, 48)
    ctl_bytes = u32(data, 52)
    if not ctx_bytes or not ctl_bytes:
        raise ValueError("both context/control thread firmware are required")
    if any(v & 3 for v in (ctx_bytes, ctl_bytes, array_off, ctl_off)):
        raise ValueError("SDMA thread payloads must be 4-byte aligned")
    if (array_off > len(data) or
        ctx_bytes > len(data) - array_off or
        ctl_off < array_off + ctx_bytes or
        ctl_off > len(data) or
        ctl_bytes > len(data) - ctl_off):
        raise ValueError("SDMA thread offsets overlap or exceed the image")
    if ucode_bytes > len(data) - array_off or ucode_bytes < ctx_bytes:
        raise ValueError("invalid common-header ucode size")
    if max(ctx_bytes, ctl_bytes) > MAX_STAGING_BYTES:
        raise ValueError("SDMA thread does not fit the isolated 1 MiB staging cap")
    return {
        "version": version,
        "ctx": data[array_off:array_off + ctx_bytes],
        "ctl": data[ctl_off:ctl_off + ctl_bytes],
        "ctx_off": array_off,
        "ctl_off": ctl_off,
    }


def generate(source, target):
    src = Path(source)
    if src.name != TARGET_BASENAME:
        raise ValueError("wrong firmware basename; refuse wrong-ASIC rename")
    data = src.read_bytes()
    if len(data) != REVIEWED_FILE_BYTES:
        raise ValueError(
            "unreviewed SDMA source size; use the documented pinned snapshot")
    actual_blob = git_blob_sha1(data)
    if actual_blob != REVIEWED_GIT_BLOB_SHA1:
        raise ValueError(
            f"unreviewed source Git blob: {actual_blob}; expected "
            f"{REVIEWED_GIT_BLOB_SHA1}; DO NOT self-pin the observed hash")
    parsed = parse_image(data)
    digest = hashlib.sha256(data).hexdigest()
    out = Path(target)
    with out.open("w", encoding="utf-8") as f:
        f.write("// Locally generated from pinned Navi33 SDMA firmware; DO NOT COMMIT.\n")
        f.write("#define RN_NAVI33_SDMA_FW_METADATA_V1 1\n")
        f.write("#define RN_NAVI33_SDMA_FW_SOURCE_VERIFIED 1\n")
        f.write("#define kRNNavi33SdmaIpMajor 6u\n")
        f.write("#define kRNNavi33SdmaIpMinor 0u\n")
        f.write("#define kRNNavi33SdmaIpRevision 2u\n")
        f.write(f"#define kRNNavi33SdmaSourceBytes {len(data)}u\n")
        f.write(f"#define kRNNavi33SdmaVersion 0x{parsed['version']:x}u\n")
        f.write(f"#define kRNNavi33SdmaCtxBytes {len(parsed['ctx'])}u\n")
        f.write(f"#define kRNNavi33SdmaCtlBytes {len(parsed['ctl'])}u\n")
        f.write(f'static const char kRNNavi33SdmaSourceSha256[] = "{digest}";\n')
        f.write(f'static const char kRNNavi33SdmaSourceGitBlobSha1[] = "{actual_blob}";\n')
        # Keep the data objects separate; Step 7 only uses Context Thread.
        for name, chunk in (
            ("kRNNavi33SdmaCtxFw", parsed["ctx"]),
            ("kRNNavi33SdmaCtlFw", parsed["ctl"]),
        ):
            f.write(f"static const unsigned char {name}[] = {{\n")
            for i in range(0, len(chunk), 16):
                f.write("    " + ", ".join(f"0x{x:02x}" for x in chunk[i:i+16]) + ",\n")
            f.write("};\n")
    print(f"Navi33 SDMA 6.0.2 v0x{parsed['version']:x}, "
          f"context {len(parsed['ctx'])} B, control {len(parsed['ctl'])} B, "
          f"Git blob {actual_blob}, SHA-256 {digest}, target_verified=1")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source")
    p.add_argument("output")
    args = p.parse_args(argv)
    generate(args.source, args.output)


if __name__ == "__main__":
    main()
