#!/usr/bin/env python3
"""Generate a local PSP boot-firmware header; firmware bytes are never committed.

For Navi33 P6.2 use the exact upstream Linux firmware file:

    python3 tools/gen_psp_fw_header.py \
        /path/to/psp_13_0_7_sos.bin RedNavi/psp_fw.h \
        --expected-sha256 <digest-from-an-independent-linux-firmware-source>

The generated header contains the bootloader-loadable components plus source
metadata (target IP, source size/SHA-256, component mask, largest component).
The firmware is marked trusted only when an independently supplied expected
SHA-256 exactly matches the input bytes. Omitting the expected digest keeps
P7 hardware firmware loading fail-closed.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct
import sys

NAMES = [
    "UNKNOWN", "SOS", "SYS_DRV", "KDB", "TOC", "SPL", "RL", "SOC_DRV",
    "INTF_DRV", "DBG_DRV", "RAS_DRV", "IPKEYMGR_DRV", "SPDM_DRV",
]
ORDER = [
    ("KDB", 0x80000), ("SPL", 0x10000000), ("SYS_DRV", 0x10000),
    ("SOC_DRV", 0xB0000), ("INTF_DRV", 0xD0000), ("DBG_DRV", 0xC0000),
    ("RAS_DRV", 0xE0000), ("IPKEYMGR_DRV", 0xF0000),
    ("SPDM_DRV", 0x20000000), ("SOS", 0x20000),
]
BITS = {
    "SOS": 1 << 0,
    "SYS_DRV": 1 << 1,
    "KDB": 1 << 2,
    "TOC": 1 << 3,
    "SPL": 1 << 4,
    "RL": 1 << 5,
    "SOC_DRV": 1 << 6,
    "INTF_DRV": 1 << 7,
    "DBG_DRV": 1 << 8,
    "RAS_DRV": 1 << 9,
    "IPKEYMGR_DRV": 1 << 10,
    "SPDM_DRV": 1 << 11,
}
TARGET = (13, 0, 7)
TARGET_FILE = "psp_13_0_7_sos.bin"
PSP_1_MEG = 1 << 20


def parse(path: Path):
    b = path.read_bytes()
    if len(b) < 36:
        raise ValueError("PSP firmware is too small for the common/v2 header")
    size_bytes, header_size = struct.unpack_from("<II", b, 0)
    major, minor = struct.unpack_from("<HH", b, 8)
    uoff = struct.unpack_from("<I", b, 24)[0]
    if size_bytes != len(b):
        raise ValueError(f"file size {len(b)} != header size {size_bytes}")
    if major != 2:
        raise ValueError(f"PSP SOS header v{major}.{minor} is not handled; expected packed v2.x")
    count = struct.unpack_from("<I", b, 32)[0]
    if count == 0 or count >= 64:
        raise ValueError(f"invalid PSP packed component count {count}")

    if minor == 0:
        desc_start = 36
        parse_start = 0
        parse_end = count
    elif minor == 1:
        if len(b) < 40:
            raise ValueError("PSP v2.1 header is truncated")
        aux_index = struct.unpack_from("<I", b, 36)[0]
        if aux_index > count:
            raise ValueError(f"invalid PSP v2.1 aux index {aux_index}")
        desc_start = 40
        # Match Linux's normal discrete-GPU path: fw_bin_count -= aux_index.
        parse_start = 0
        parse_end = count - aux_index
        if parse_end == 0:
            raise ValueError("PSP v2.1 contains no normal boot components")
    else:
        raise ValueError(f"unsupported packed PSP header v2.{minor}")

    desc_end = desc_start + 16 * count
    if header_size < desc_end or desc_end > len(b) or uoff > len(b):
        raise ValueError("PSP descriptor table or ucode array is out of bounds")

    found = {}
    for i in range(parse_start, parse_end):
        typ, ver, off, size = struct.unpack_from("<IIII", b, desc_start + 16 * i)
        if typ >= len(NAMES):
            continue
        name = NAMES[typ]
        if name == "UNKNOWN":
            continue
        if off > len(b) - uoff or size > len(b) - uoff - off:
            raise ValueError(f"{name} component is outside the firmware image")
        if size > PSP_1_MEG:
            raise ValueError(f"{name} component exceeds Linux PSP_1_MEG staging buffer")
        if name in found:
            raise ValueError(f"duplicate PSP component {name}")
        found[name] = (ver, b[uoff + off:uoff + off + size])

    if "SYS_DRV" not in found or not found["SYS_DRV"][1]:
        raise ValueError("PSP SYS_DRV component missing")
    if "SOS" not in found or not found["SOS"][1]:
        raise ValueError("PSP SOS component missing")
    return b, major, minor, found


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument(
        "--expected-sha256",
        help="independently sourced SHA-256 required to mark firmware trusted",
    )
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    path, out = args.path, args.out
    b, header_major, header_minor, found = parse(path)

    exact = path.name == TARGET_FILE
    target = TARGET if exact else (0, 0, 0)
    digest = hashlib.sha256(b).hexdigest()
    expected = (args.expected_sha256 or "").strip().lower()
    if expected and (len(expected) != 64 or any(ch not in "0123456789abcdef" for ch in expected)):
        raise SystemExit("--expected-sha256 must be exactly 64 lowercase/uppercase hex characters")
    if expected and digest != expected:
        raise SystemExit(f"firmware SHA-256 mismatch: actual {digest}, expected {expected}")
    digest_trusted = bool(expected) and digest == expected
    mask = 0
    max_component = 0
    for name, (_ver, data) in found.items():
        mask |= BITS.get(name, 0)
        max_component = max(max_component, len(data))

    with out.open("w", encoding="utf-8") as f:
        f.write(f"// Generated by tools/gen_psp_fw_header.py from {path.name}. Do not commit.\n")
        f.write("#define RN_PSP_FW_METADATA_V1 1\\n")
        f.write(f"#define kRNPspTargetMajor {target[0]}u\n")
        f.write(f"#define kRNPspTargetMinor {target[1]}u\n")
        f.write(f"#define kRNPspTargetRevision {target[2]}u\n")
        f.write(f"#define kRNPspExactTargetName {1 if exact else 0}u\n")
        f.write(f"#define kRNPspSourceBytes {len(b)}u\n")
        f.write(f"#define kRNPspComponentMask 0x{mask:08x}u\n")
        f.write(f"#define kRNPspMaxComponentBytes {max_component}u\n")
        f.write(f'static const char kRNPspSourceSha256[] = "{digest}";\n')
        f.write(f"#define RN_PSP_FW_DIGEST_TRUSTED {1 if digest_trusted else 0}\n")
        f.write(f"#define kRNPspHeaderMajor {header_major}u\n")
        f.write(f"#define kRNPspHeaderMinor {header_minor}u\n")

        entries = []
        for name, cmd in ORDER:
            if name not in found:
                continue
            ver, data = found[name]
            f.write(f"static const unsigned char kRnFw_{name}[] = {{\n")
            for j in range(0, len(data), 16):
                f.write("    " + ", ".join(f"0x{x:02x}" for x in data[j:j + 16]) + ",\n")
            f.write("};\n")
            entries.append((name, cmd, ver, len(data)))
            print(f"{name}: {len(data)} bytes, version 0x{ver:x}, command 0x{cmd:x}")

        f.write("static const struct { const char *name; unsigned int cmd; unsigned int version; "
                "const unsigned char *data; unsigned int size; } kRNPspChain[] = {\n")
        for name, cmd, ver, n in entries:
            f.write(f'    {{"{name}", 0x{cmd:x}, 0x{ver:x}, kRnFw_{name}, {n}}},\n')
        f.write("};\n")

        if "TOC" in found:
            ver, data = found["TOC"]
            f.write("static const unsigned char kRNPspToc[] = {\n")
            for j in range(0, len(data), 16):
                f.write("    " + ", ".join(f"0x{x:02x}" for x in data[j:j + 16]) + ",\n")
            f.write("};\n")
            print(f"TOC: {len(data)} bytes, version 0x{ver:x}")

    print(f"source: {len(b)} bytes sha256={digest}")
    print(f"component mask=0x{mask:08x}, max={max_component} bytes")
    print("digest trust: " + ("VERIFIED" if digest_trusted else "UNVERIFIED (P7 load stays disabled)"))
    if not exact:
        print(f"WARNING: expected {TARGET_FILE}; generated metadata will NOT pass Navi33 P6.2 target gate")


if __name__ == "__main__":
    main()
