#!/usr/bin/env python3
"""Read-only, privacy-filtered baseline for Navi33 on native macOS.

No sudo, kernel extensions, BAR mappings, firmware commands, or device writes.
Records only whitelisted fields. Raw IORegistry/EDID/VBIOS are never saved.
"""
from __future__ import annotations

import datetime
import json
import pathlib
import plistlib
import re
import subprocess
import sys

from navi33_pci_resources import describe_navi33_pci


def run(*args: str, timeout: int = 45) -> tuple[int, str]:
    try:
        result = subprocess.run(args, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True,
                                encoding="utf-8", errors="replace",
                                timeout=timeout, check=False)
        return result.returncode, result.stdout
    except (OSError, subprocess.TimeoutExpired):
        return -1, ""


def value(*args: str) -> str | None:
    status, result = run(*args)
    return result.strip() if status == 0 and result.strip() else None


def integer_id(raw: object) -> int | None:
    if isinstance(raw, bytes):
        return int.from_bytes(raw[:4], "little") if len(raw) >= 2 else None
    if isinstance(raw, int):
        return raw
    if isinstance(raw, str):
        try:
            return int(raw, 0)
        except ValueError:
            return None
    return None


def registry_nodes(node: object):
    if isinstance(node, dict):
        yield node
        for child in node.values():
            if isinstance(child, (list, dict)):
                yield from registry_nodes(child)
    elif isinstance(node, list):
        for child in node:
            yield from registry_nodes(child)


def rx7600_pci() -> dict:
    try:
        result = subprocess.run(
            ["ioreg", "-a", "-r", "-c", "IOPCIDevice"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=60, check=False,
        )
        if result.returncode:
            return {"status": "ioreg_failed", "matching_devices": []}
        root = plistlib.loads(result.stdout)
    except (OSError, subprocess.TimeoutExpired, ValueError, TypeError, plistlib.InvalidFileException):
        return {"status": "ioreg_unavailable", "matching_devices": []}
    matches = []
    for node in registry_nodes(root):
        if integer_id(node.get("vendor-id")) != 0x1002:
            continue
        if integer_id(node.get("device-id")) != 0x7480:
            continue
        # Whitelist only identifiers; no serials, BAR bytes, VBIOS, EDID or registry dumps.
        matches.append({
            "vendor_id": "1002",
            "device_id": "7480",
            "class_code": f"{integer_id(node.get('class-code')):06x}"
            if integer_id(node.get("class-code")) is not None else None,
            "io_name": str(node.get("IOName", ""))[:90],
            "registry_name": str(node.get("IORegistryEntryName", ""))[:90],
            "pci_resources": describe_navi33_pci(node),
        })
    return {"status": "found" if matches else "not_found", "matching_devices": matches}


def display_summary() -> list[str]:
    status, output = run("system_profiler", "SPDisplaysDataType", timeout=90)
    if status:
        return []
    allowed_keys = (
        "Chipset Model", "Type", "Bus", "VRAM (Total)", "VRAM (Dynamic, Max)",
        "VRAM", "Vendor", "Device ID", "Metal Support", "Metal",
        "Resolution", "UI Looks like", "Refresh Rate", "Framebuffer",
    )
    result = []
    for line in output.splitlines():
        if ":" not in line:
            continue
        key, text = line.strip().split(":", 1)
        if key in allowed_keys:
            result.append(f"{key}: {text.strip()[:140]}")
    return result[:70]


def storage_summary() -> dict:
    status, output = run("diskutil", "info", "/", timeout=20)
    if status:
        return {}
    allowed_keys = {
        "Protocol", "Solid State", "Device / Media Name",
        "File System Personality", "APFS Volume Role",
    }
    info = {}
    for line in output.splitlines():
        if ":" in line:
            key, data = line.strip().split(":", 1)
            if key in allowed_keys:
                info[key] = data.strip()[:120]
    return info


def loaded_kexts() -> list[str]:
    status, output = run("kmutil", "showloaded", timeout=40)
    if status:
        return []
    lines = []
    for line in output.splitlines():
        if re.search(r"RedNavi|WhateverGreen|VirtualSMC|Lilu|AppleALC", line, re.I):
            lines.append(line.strip()[:170])
    return lines[:30]


def main():
    if sys.platform != "darwin":
        raise SystemExit("Run this script on the native macOS machine.")
    target = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path("navi33-macos-baseline.json")
    _, args = run("nvram", "boot-args", timeout=8)
    boot = args if "boot-args" in args else ""
    report = {
        "schema": "rednavi.macos.baseline.v2",
        "collected_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "note": "Userland-only observation. PCI visibility does not prove GPU compute or Metal.",
        "macos": {
            "version": value("sw_vers", "-productVersion"),
            "build": value("sw_vers", "-buildVersion"),
            "kernel_release": value("sysctl", "-n", "kern.osrelease"),
            "architecture": value("uname", "-m"),
            "physical_cpu_cores": value("sysctl", "-n", "hw.physicalcpu"),
        },
        "security": {
            "sip_status": value("csrutil", "status"),
            "radvesa_present": "-radvesa" in boot,
            "rednavi_boot_args_present": "rednavi_" in boot,
        },
        "build_tools": {
            "xcode_select": value("xcode-select", "-p"),
            "clang_path": value("xcrun", "-find", "clang"),
            "python3_version": value("python3", "--version"),
        },
        "rx7600": rx7600_pci(),
        "display": display_summary(),
        "root_volume": storage_summary(),
        "relevant_loaded_kexts": loaded_kexts(),
    }
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "saved_report": str(target),
        "rx7600": report["rx7600"]["status"],
        "macos": report["macos"]["version"],
        "pci_resource_gates": [d["pci_resources"]["p4_p5_resource_gate"]
                               for d in report["rx7600"]["matching_devices"]],
        "metal_display_lines": [line for line in report["display"] if "Metal" in line],
        "warning": "Do not interpret GOP/firmware framebuffer as native GPU acceleration.",
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
