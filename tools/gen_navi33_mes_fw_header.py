#!/usr/bin/env python3
"""Generate locally pinned Navi33 MES scheduler + KIQ PSP payloads.

Matches the Linux GC11 (mes_v11_0.c / amdgpu_mes.c) selection:
  gc_11_0_2_mes_2.bin  -> MES Scheduler (types 33 and 34)
  gc_11_0_2_mes1.bin   -> MES KIQ (types 81 and 82)
Linux uses mes_firmware_header_v1_0 offsets for each code/data payload.
Never embed, ship or commit the vendor binaries or resulting header.

Usage:
  python3 tools/gen_navi33_mes_fw_header.py \
    /tmp/gc_11_0_2_mes_2.bin /tmp/gc_11_0_2_mes1.bin \
    RedNavi/navi33_mes_fw.h
"""
import argparse
import hashlib
from pathlib import Path
import struct

SOURCE_REPO = "beagleboard/mirror-linux-firmware"
SOURCE_COMMIT = "2f411c10e4570d03426d3cb095471aa1120114ab"
PINNED = {
    "Sched": ("gc_11_0_2_mes_2.bin", 256288,
              "693e5796e64f69d8c79e3ca8180be8c8928d9c26"),
    "Kiq": ("gc_11_0_2_mes1.bin", 218400,
            "e10f5a044e93acce3cef5038da4afaff6ad3e6f9"),
}
STAGE_BYTES = 270336


def git_blob_sha1(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def parse_mes(data):
    if len(data) < 72:
        raise ValueError("MES header truncated")
    file_bytes, header_bytes, header_maj, header_min, ip_maj, ip_min, \
        common_version, common_size, common_offset, crc = \
        struct.unpack_from("<IIHHHHIIII", data, 0)
    ucode_version, code_bytes, code_offset, data_version, data_bytes, \
        data_offset, code_pc_lo, code_pc_hi, data_pc_lo, data_pc_hi = \
        struct.unpack_from("<IIIIIIIIII", data, 32)
    if file_bytes != len(data) or header_bytes != 72:
        raise ValueError("invalid declared MES image or header bytes")
    if (header_maj, header_min, ip_maj, ip_min) != (1, 0, 11, 0):
        raise ValueError("MES header target/version mismatch")
    if not code_bytes or not data_bytes:
        raise ValueError("MES code or data missing")
    if code_bytes > STAGE_BYTES or data_bytes > STAGE_BYTES:
        raise ValueError("MES segment exceeds retained DMA staging buffer")
    if code_bytes & 3 or data_bytes & 3:
        raise ValueError("MES payload size not DWORD-aligned")
    if code_offset < header_bytes or code_offset & 3:
        raise ValueError("MES code offset invalid")
    if data_offset < code_offset + code_bytes or data_offset & 3:
        raise ValueError("MES data offset overlaps instruction segment")
    if data_offset > len(data) or data_bytes > len(data) - data_offset:
        raise ValueError("MES data extends beyond image")
    if code_bytes > len(data) - code_offset:
        raise ValueError("MES code extends beyond image")
    # Some published MES v1 files have common_size differing from the
    # combined code+data byte lengths. Linux trusts the explicit MES spans.
    return {
        "code": data[code_offset:code_offset + code_bytes],
        "data": data[data_offset:data_offset + data_bytes],
        "code_bytes": code_bytes,
        "data_bytes": data_bytes,
        "version": ucode_version,
        "data_version": data_version,
        "code_pc": code_pc_lo | (code_pc_hi << 32),
        "data_pc": data_pc_lo | (data_pc_hi << 32),
    }


def checked_source(path, kind):
    path = Path(path)
    filename, expected_bytes, expected_blob = PINNED[kind]
    if path.name != filename:
        raise ValueError("unexpected MES firmware filename: " + path.name)
    data = path.read_bytes()
    if len(data) != expected_bytes:
        raise ValueError("unreviewed MES whole-file length")
    if git_blob_sha1(data) != expected_blob:
        raise ValueError("unreviewed MES Git blob: " + git_blob_sha1(data))
    parsed = parse_mes(data)
    parsed["source_bytes"] = len(data)
    parsed["source_blob"] = expected_blob
    parsed["source_sha256"] = hashlib.sha256(data).hexdigest()
    return parsed


def generate(sched_path, kiq_path, output):
    # Validate both sources fully *before* writing either payload.
    parsed = {k: checked_source(p, k) for k, p in
              (("Sched", sched_path), ("Kiq", kiq_path))}
    lines = [
        "// Locally generated from two pinned Navi33 MES firmware images.",
        "// Proprietary vendor data: NEVER COMMIT OR REDISTRIBUTE.",
        "#define RN_NAVI33_MES_FW_METADATA_V1 1",
        "#define RN_NAVI33_MES_FW_SOURCE_VERIFIED 1",
        "#define kRNNavi33MesIpMajor 11u",
        "#define kRNNavi33MesIpMinor 0u",
        "#define kRNNavi33MesIpRevision 2u",
    ]
    for kind in ("Sched", "Kiq"):
        p = parsed[kind]
        prefix = "kRNNavi33Mes" + kind
        lines.extend([
            f"#define {prefix}SourceBytes {p['source_bytes']}u",
            f"#define {prefix}Version {p['version']}u",
            f"#define {prefix}CodeBytes {p['code_bytes']}u",
            f"#define {prefix}DataBytes {p['data_bytes']}u",
            f'#define {prefix}SourceBlob "{p["source_blob"]}"',
            f'#define {prefix}SourceSha256 "{p["source_sha256"]}"',
        ])
        for suffix in ("Code", "Data"):
            payload = p[suffix.lower()]
            lines.append(f"static const unsigned char {prefix}{suffix}Fw[] = {{")
            for offset in range(0, len(payload), 16):
                lines.append("    " + ", ".join(
                    f"0x{x:02x}" for x in payload[offset:offset + 16]
                ) + ",")
            lines.append("};")
    Path(output).write_text("\n".join(lines) + "\n", encoding="utf-8")
    for kind in ("Sched", "Kiq"):
        p = parsed[kind]
        print(f"{kind}: pinned blob {p['source_blob']}, "
              f"code={p['code_bytes']} B data={p['data_bytes']} B, "
              f"file={p['source_bytes']} B sha256={p['source_sha256']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("scheduler")
    ap.add_argument("kiq")
    ap.add_argument("output")
    args = ap.parse_args()
    generate(args.scheduler, args.kiq, args.output)


if __name__ == "__main__":
    main()
