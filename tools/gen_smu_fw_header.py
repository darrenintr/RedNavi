#!/usr/bin/env python3
"""Generate a local SMU/PMFW C header from an AMD firmware image.

The full source SHA-256 must be checked against a separately reviewed
firmware-to-MP1 binding. A caller-provided hash alone is not evidence that
firmware targets the installed GPU. Firmware bytes must stay local.
"""
import argparse
import hashlib
import os
import struct

TARGETS = {
    "navi33": {"basename": "smu_13_0_7.bin", "mp1": (13, 0, 7)},
    "navi32": {"basename": "smu_13_0_10.bin", "mp1": (13, 0, 10)},
}
# Source-file digests of separately inspected, pinned linux-firmware images.
# A new firmware revision requires a source review before adding its digest.
REVIEWED_SHA256_TARGET = {
    "68ec18bd605e680085c927ff72c609f8c771aff0718d0cfab58a3744dff8e5b7": "navi33",
    "9376ae64149e6b0b684898ffbc12c2230f8c50a2e9447dc7dafc95c0c16b5227": "navi32",
}
TARGET_BASENAME = TARGETS["navi33"]["basename"]


def parse_args(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("output")
    ap.add_argument("--target", required=True, choices=sorted(TARGETS))
    ap.add_argument("--expected-sha256")
    return ap.parse_args(argv)


def parse_firmware(data):
    if len(data) < 32:
        raise ValueError("firmware is smaller than the common AMD header")
    (size_bytes, hdr_size, vmaj, vmin, imaj, imin, ucode_version,
     ucode_size, array_off, crc) = struct.unpack_from("<IIHHHHIIII", data, 0)
    if size_bytes != len(data):
        raise ValueError(f"file size mismatch: file={len(data)} header={size_bytes}")
    if hdr_size < 32 or hdr_size > len(data):
        raise ValueError(f"invalid header size {hdr_size}")
    if array_off < hdr_size or array_off > len(data):
        raise ValueError(f"invalid ucode offset {array_off}")
    if ucode_size == 0 or ucode_size > len(data) - array_off:
        raise ValueError(f"ucode is out of bounds: off={array_off} size={ucode_size}")
    if imaj != 13 or imin != 0:
        raise ValueError(f"unexpected SMU firmware common-header IP {imaj}.{imin}")
    return {
        "header_size": hdr_size,
        "header_version_major": vmaj,
        "header_version_minor": vmin,
        "ip_major": imaj,
        "ip_minor": imin,
        "ucode_version": ucode_version,
        "ucode_size": ucode_size,
        "ucode_offset": array_off,
        "crc": crc,
        "payload": data[array_off:array_off + ucode_size],
    }


def main(argv=None):
    args = parse_args(argv)
    target = TARGETS[args.target]
    basename = os.path.basename(args.source)
    if basename != target["basename"]:
        raise SystemExit(f"expected {target['basename']} for {args.target}, got {basename}")

    with open(args.source, "rb") as src:
        data = src.read()
    meta = parse_firmware(data)
    digest = hashlib.sha256(data).hexdigest()
    expected = args.expected_sha256.lower() if args.expected_sha256 else None
    if expected and digest != expected:
        raise SystemExit(f"SHA-256 mismatch: expected {expected}, got {digest}")

    reviewed_target = REVIEWED_SHA256_TARGET.get(digest)
    if reviewed_target is not None and reviewed_target != args.target:
        raise SystemExit(
            f"firmware SHA-256 belongs to {reviewed_target}, not {args.target}"
        )
    digest_trusted = bool(expected and expected == digest)
    target_verified = bool(reviewed_target == args.target)
    major, minor, revision = target["mp1"]

    # Write only after all validation, to avoid replacing an existing header
    # with an image whose provenance or layout was rejected.
    with open(args.output, "w") as f:
        f.write(
            f"// Generated from {target['basename']} by "
            "tools/gen_smu_fw_header.py. Do not commit.\n"
        )
        f.write("#define RN_SMU_FW_METADATA_V2 1\n")
        f.write(f"#define RN_SMU_FW_DIGEST_TRUSTED {int(digest_trusted)}\n")
        f.write(f"#define RN_SMU_FW_TARGET_VERIFIED {int(target_verified)}\n")
        f.write(f"#define kRNSmuFwTargetMp1Major {major}u\n")
        f.write(f"#define kRNSmuFwTargetMp1Minor {minor}u\n")
        f.write(f"#define kRNSmuFwTargetMp1Revision {revision}u\n")
        f.write(f"#define kRNSmuFwSourceBytes {len(data)}u\n")
        f.write(f"#define kRNSmuFwHeaderSize {meta['header_size']}u\n")
        f.write(f"#define kRNSmuFwUcodeOffset {meta['ucode_offset']}u\n")
        f.write(f"#define kRNSmuFwVersion 0x{meta['ucode_version']:x}u\n")
        f.write(f"#define kRNSmuFwSize {meta['ucode_size']}u\n")
        f.write(f'static const char kRNSmuFwSourceSha256[] = "{digest}";\n')
        f.write("static const unsigned char kRNSmuFw[] = {\n")
        payload = meta["payload"]
        for j in range(0, len(payload), 16):
            f.write("    " + ", ".join(f"0x{x:02x}" for x in payload[j:j + 16]) + ",\n")
        f.write("};\n")
    print(
        f"header v{meta['header_version_major']}."
        f"{meta['header_version_minor']}, "
        f"IP {meta['ip_major']}.{meta['ip_minor']}, "
        f"ucode 0x{meta['ucode_version']:x}, {meta['ucode_size']} bytes, "
        f"sha256 {digest}, trusted={int(digest_trusted)}, "
        f"target={args.target}, target_verified={int(target_verified)}"
    )


if __name__ == "__main__":
    main()
