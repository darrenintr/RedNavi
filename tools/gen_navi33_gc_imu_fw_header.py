#!/usr/bin/env python3
"""Generate a pinned, local Navi33 GC11.0.2 IMU I/D PSP firmware header.

Source pinned to beagleboard/mirror-linux-firmware commit:
2f411c10e4570d03426d3cb095471aa1120114ab
File: amdgpu/gc_11_0_2_imu.bin (Git blob SHA-1 verified).

Linux imu_v11_0.c and amdgpu_ucode.c use:
  IMU_I: header.ucode_array_offset_bytes, imu_iram_ucode_size_bytes
  IMU_D: header.ucode_array_offset_bytes + imu_iram_ucode_size_bytes,
         imu_dram_ucode_size_bytes

No firmware binary or generated header may be committed.

Usage:
  python3 tools/gen_navi33_gc_imu_fw_header.py \
    /tmp/gc_11_0_2_imu.bin RedNavi/navi33_gc_imu_fw.h
"""
import argparse
import hashlib
from pathlib import Path
import struct

SOURCE_BASENAME = "gc_11_0_2_imu.bin"
SOURCE_REPO = "beagleboard/mirror-linux-firmware"
SOURCE_COMMIT = "2f411c10e4570d03426d3cb095471aa1120114ab"
PINNED_GIT_BLOB_SHA1 = "7d0941dadc1f8ac1584ee763b357604503fca6d9"
PINNED_SOURCE_BYTES = 132352
MAX_STAGE_BYTES = 270336


def git_blob_sha1(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def parse_image(data):
    if len(data) < 48:
        raise ValueError("truncated IMU header")
    (file_bytes, header_bytes, header_major, header_minor,
     ip_major, ip_minor, version, total_bytes, array_offset,
     _crc) = struct.unpack_from("<IIHHHHIIII", data, 0)
    iram_bytes, iram_offset, dram_bytes, dram_offset = (
        struct.unpack_from("<IIII", data, 32)
    )
    if file_bytes != len(data):
        raise ValueError("IMU declared file bytes mismatch")
    if (header_major, header_minor) != (1, 0):
        raise ValueError("IMU header version must be 1.0")
    if (ip_major, ip_minor) != (11, 0):
        raise ValueError("IMU firmware is not GC11.0.x")
    if header_bytes < 48 or header_bytes & 3:
        raise ValueError("bad IMU header size")
    if array_offset < header_bytes or array_offset & 3:
        raise ValueError("bad IMU ucode array offset")
    if not iram_bytes or not dram_bytes:
        raise ValueError("missing IMU I or D segment")
    if (iram_bytes | dram_bytes | total_bytes) & 3:
        raise ValueError("unaligned IMU payload size")
    if iram_bytes > MAX_STAGE_BYTES or dram_bytes > MAX_STAGE_BYTES:
        raise ValueError("IMU segment exceeds retained DMA stage")
    if iram_offset != 0 or dram_offset != iram_bytes:
        raise ValueError("IMU segment offsets are not contiguous")
    if total_bytes != iram_bytes + dram_bytes:
        raise ValueError("IMU declared ucode size differs from I+D")
    if array_offset > len(data) or total_bytes > len(data) - array_offset:
        raise ValueError("IMU payload exceeds firmware image")
    return {
        "version": version,
        "instruction_bytes": iram_bytes,
        "data_bytes": dram_bytes,
        "instruction": data[array_offset:array_offset + iram_bytes],
        "data": data[array_offset + iram_bytes:
                     array_offset + iram_bytes + dram_bytes],
    }


def generate(source, target):
    source = Path(source)
    target = Path(target)
    if source.name != SOURCE_BASENAME:
        raise ValueError("refuse firmware from a different ASIC")
    raw = source.read_bytes()
    if len(raw) != PINNED_SOURCE_BYTES:
        raise ValueError("unreviewed IMU source size")
    blob_hash = git_blob_sha1(raw)
    if blob_hash != PINNED_GIT_BLOB_SHA1:
        raise ValueError(
            "unreviewed IMU source Git blob SHA-1: " + blob_hash
        )
    metadata = parse_image(raw)
    sha256 = hashlib.sha256(raw).hexdigest()
    with target.open("w", encoding="utf-8") as f:
        f.write("// Locally generated from pinned Navi33 IMU: DO NOT COMMIT.\n")
        f.write("#define RN_NAVI33_GC_IMU_FW_METADATA_V1 1\n")
        f.write("#define RN_NAVI33_GC_IMU_FW_SOURCE_VERIFIED 1\n")
        f.write("#define kRNNavi33GcImuIpMajor 11u\n")
        f.write("#define kRNNavi33GcImuIpMinor 0u\n")
        f.write("#define kRNNavi33GcImuIpRevision 2u\n")
        f.write(f"#define kRNNavi33GcImuSourceBytes {len(raw)}u\n")
        f.write(f"#define kRNNavi33GcImuVersion {metadata['version']}u\n")
        f.write(f"#define kRNNavi33GcImuIBytes {metadata['instruction_bytes']}u\n")
        f.write(f"#define kRNNavi33GcImuDBytes {metadata['data_bytes']}u\n")
        f.write(f'#define kRNNavi33GcImuSourceBlob "{blob_hash}"\n')
        f.write(f'#define kRNNavi33GcImuSourceSha256 "{sha256}"\n')
        for name, payload in (("IFw", metadata["instruction"]),
                              ("DFw", metadata["data"])):
            f.write(f"static const unsigned char kRNNavi33GcImu{name}[] = {{\n")
            for i in range(0, len(payload), 16):
                f.write("    " + ", ".join(
                    f"0x{value:02x}" for value in payload[i:i + 16]
                ) + ",\n")
            f.write("};\n")
    print(
        f"Navi33 GC11.0.2 IMU v{metadata['version']}, "
        f"I={metadata['instruction_bytes']} B type68, "
        f"D={metadata['data_bytes']} B type69, "
        f"Git blob={blob_hash}, sha256={sha256}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("target")
    args = parser.parse_args()
    generate(args.source, args.target)


if __name__ == "__main__":
    main()
