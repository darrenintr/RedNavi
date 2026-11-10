#!/usr/bin/env python3
"""Decode macOS IOPCIDevice assigned-addresses without touching PCI hardware.

The byte sequence is the Open Firmware five-cell PCI assigned-addresses
property as exported by macOS ioreg on x86_64, with host-endian 32-bit cells.
These are *CPU* resource ranges, never GPU MC, GPU PTE, or GPU virtual addresses.
"""
from __future__ import annotations

import struct

_RECORD_BYTES = 20
_KNOWN_REGISTERS = {0x10: "BAR0", 0x14: "BAR1", 0x18: "BAR2",
                    0x1C: "BAR3", 0x20: "BAR4", 0x24: "BAR5",
                    0x30: "Expansion ROM"}


def _is_power_two(n: int) -> bool:
    return n > 0 and (n & (n - 1)) == 0


def decode_assigned_addresses(raw: object) -> list[dict]:
    """Decode bounded PCI resources from the OS-reported 5-cell array.

    No BAR accesses, driver calls, memory mappings, or privileged operations.
    The address-space type is reported as its OF PCI code; it does not
    independently establish a PCI BAR's 32/64-bit hardware capability.
    """
    if not isinstance(raw, (bytes, bytearray)) or not raw:
        raise ValueError("missing assigned-addresses OSData")
    if len(raw) % _RECORD_BYTES or len(raw) > _RECORD_BYTES * 32:
        raise ValueError("truncated or oversized PCI assigned-addresses")
    result: list[dict] = []
    regs: set[int] = set()
    for off in range(0, len(raw), _RECORD_BYTES):
        flags, high, low, size_high, size_low = struct.unpack_from("<IIIII", raw, off)
        register = flags & 0xFF
        space = (flags >> 24) & 0x03
        name = _KNOWN_REGISTERS.get(register)
        if not name or register in regs:
            raise ValueError(f"unsupported or repeated PCI register 0x{register:02x}")
        regs.add(register)
        if space not in (1, 2, 3):
            raise ValueError("unrecognized PCI address-space code")
        if space == 1 and register != 0x20:
            raise ValueError("unexpected PCI I/O region")
        address = (high << 32) | low
        length = (size_high << 32) | size_low
        if not _is_power_two(length) or address % length != 0:
            raise ValueError(f"{name}: resource has invalid size or alignment")
        if address + length > (1 << 64):
            raise ValueError(f"{name}: CPU physical address overflow")
        result.append({
            "name": name,
            "register": f"0x{register:02x}",
            "cpu_physical_base": f"0x{address:x}",
            "cpu_physical_end_inclusive": f"0x{address + length - 1:x}",
            "length_bytes": length,
            "address_space_code": space,
            "kind": "io" if space == 1 else "memory",
            "prefetchable_flag": bool(flags & 0x40000000),
        })
    # Detect overlapping MMIO resources, but do not treat PCI I/O as memory.
    memory = sorted((int(b["cpu_physical_base"], 16),
                     int(b["cpu_physical_end_inclusive"], 16), b["name"])
                    for b in result if b["kind"] == "memory")
    for first, second in zip(memory, memory[1:]):
        if first[1] >= second[0]:
            raise ValueError(f"overlapping PCI regions: {first[2]} and {second[2]}")
    return sorted(result, key=lambda b: int(b["register"], 16))


def _device_memory_matches(node: dict, region: dict) -> bool | None:
    """Cross-check published descriptors; do not trust their list position."""
    descriptors = node.get("IODeviceMemory")
    if not isinstance(descriptors, list) or region["kind"] != "memory":
        return None
    expected_addr = int(region["cpu_physical_base"], 16)
    expected_len = region["length_bytes"]
    observed = [item for item in descriptors
                if isinstance(item, dict) and
                isinstance(item.get("address"), int) and
                isinstance(item.get("length"), int)]
    if not observed:
        return None
    return any(item["address"] == expected_addr and
               item["length"] == expected_len for item in observed)


def describe_navi33_pci(node: dict) -> dict:
    """Produce a whitelist-only, publishable PCI report for the RX 7600."""
    try:
        bars = decode_assigned_addresses(node.get("assigned-addresses"))
    except ValueError as exc:
        return {"status": "invalid", "error": str(exc), "bars": [],
                "p4_p5_resource_gate": "fail"}
    for bar in bars:
        bar["matches_iodevice_memory"] = _device_memory_matches(node, bar)
    by_name = {bar["name"]: bar for bar in bars}
    # Resource gate is *only* a structural sanity check, never permission to
    # bind RedNavi, access MM_INDEX, reset the GPU or load firmware.
    required = {"BAR0": ("memory", 4096),
                "BAR2": ("memory", 4096),
                "BAR5": ("memory", 1 << 20)}
    errors = []
    for name, (kind, minimum) in required.items():
        item = by_name.get(name)
        if not item:
            errors.append(f"missing {name}")
        elif item["kind"] != kind or item["length_bytes"] < minimum:
            errors.append(f"{name} type/length is insufficient")
        elif item["matches_iodevice_memory"] is False:
            errors.append(f"{name} disagrees with IODeviceMemory")
    link = node.get("IOPCIExpressLinkStatus")
    link_report = None
    if isinstance(link, int):
        link_report = {
            "raw": f"0x{link & 0xffff:04x}",
            "reported_speed_code": link & 0xF,
            "reported_width": (link >> 4) & 0x3F,
            "note": "OS register report only; root-port/BIOS validation needed",
        }
    return {
        "status": "ok" if not errors else "incomplete",
        "bars": bars,
        "reported_pcie_link": link_report,
        "p4_p5_resource_gate": "pass" if not errors else "fail",
        "issues": errors,
        "warning": ("Resource gate never authorizes GPU MMIO, firmware "
                    "initialization or kext loading on the only display GPU."),
    }
