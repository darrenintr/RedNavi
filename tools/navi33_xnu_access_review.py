#!/usr/bin/env python3
"""P7.34 offline, provenance-aware Navi33 IOKit/MP1 evidence summary.

Input is a previously captured ioreg text log. No root access, Kext reload,
PCI/IOKit user client, or MMIO reads are performed. This interpreter cannot
authorize SMU commands or establish external host/firmware writer exclusion.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

P733 = re.compile(
    r'"navi33-p7-late-provider-audit-(?P<index>[12])"\s*=\s*'
    r'\{(?P<body>[^{}\n]*)\}'
)
P727 = re.compile(
    r'"navi33-p7-smu-roundtrip"\s*=\s*\{(?P<body>[^{}\n]*)\}'
)
PAIR = re.compile(r'"(?P<key>[A-Za-z0-9_-]+)"\s*=\s*'
                  r'(?P<value>0x[0-9a-fA-F]+|[0-9]+|Yes|No)')
FACTS = (
    "sample_valid", "direct_children_observed", "amd_support_present",
    "boot_framebuffer_present", "provider_open_by_any",
    "provider_open_by_rednavi", "amd_support_provider_open",
    "boot_framebuffer_provider_open", "mailbox_exclusive_proven",
    "may_issue_smu_command",
)
OPEN_KEYS = ("provider_open_by_any", "provider_open_by_rednavi",
             "amd_support_provider_open", "boot_framebuffer_provider_open")


def parse_properties(body: str) -> dict[str, int]:
    record: dict[str, int] = {}
    for match in PAIR.finditer(body):
        raw = match.group("value")
        value = 1 if raw == "Yes" else 0 if raw == "No" else int(raw, 0)
        record[match.group("key")] = value
    return record


def parse_snapshots(text: str) -> list[dict[str, object]]:
    snapshots: list[dict[str, object]] = []
    for match in P733.finditer(text):
        original = parse_properties(match.group("body"))
        s: dict[str, object] = {
            "source": "kernel IOService::isOpen() at timer callback",
            "sample_index": int(match.group("index")),
        }
        for key in FACTS:
            s[key] = original.get(key)
        s["complete_open_query"] = all(key in original for key in OPEN_KEYS)
        s["host_open_reported_no_at_that_instant"] = (
            original.get("provider_open_by_any") == 0
            if "provider_open_by_any" in original else None
        )
        s["snapshot_inconsistent"] = (
            original.get("provider_open_by_any") == 0 and
            any(original.get(key) == 1 for key in (
                "provider_open_by_rednavi", "amd_support_provider_open",
                "boot_framebuffer_provider_open"
            ))
        )
        snapshots.append(s)
    return snapshots


def summarize(text: str) -> dict[str, object]:
    snapshots = parse_snapshots(text)
    observed = [s for s in snapshots if s["sample_valid"] == 1]
    last = observed[-1] if observed else None
    attachment_samples = [
        s for s in observed if s.get("amd_support_present") is not None
        or s.get("boot_framebuffer_present") is not None
    ]
    host_open_samples = [
        s for s in observed if s.get("provider_open_by_any") is not None
    ]
    any_apple_attached = (
        any(s.get("amd_support_present") == 1 or
            s.get("boot_framebuffer_present") == 1
            for s in attachment_samples)
        if attachment_samples else None
    )
    any_host_open = (
        any(s.get("provider_open_by_any") == 1 for s in host_open_samples)
        if host_open_samples else None
    )
    observed_no_host_open = (
        last.get("host_open_reported_no_at_that_instant")
        if last else None
    )
    known_open = (
        last.get("provider_open_by_any")
        if last else None
    )
    roundtrips = [
        parse_properties(m.group("body")) for m in P727.finditer(text)
    ]
    command_sent = (roundtrips[-1].get("command_sent")
                    if roundtrips else None)
    handshake_verified = (roundtrips[-1].get("handshake_verified")
                          if roundtrips else None)
    return {
        "schema": "navi33-p734-offline-evidence-v1",
        "read_only_offline": True,
        "kernel_late_samples": snapshots,
        "latest_valid_sample_index": (
            last["sample_index"] if last is not None else None
        ),
        "any_apple_services_attached_at_samples": any_apple_attached,
        "any_host_provider_open_observed": any_host_open,
        "latest_host_open_reported_no_at_sample": observed_no_host_open,
        "latest_host_open_raw": known_open,
        "late_snapshot_inconsistent": any(
            s["snapshot_inconsistent"] for s in observed
        ),
        "smu_command_sent_telemetry": command_sent,
        "smu_handshake_verified_telemetry": handshake_verified,
        "interpretation": {
            "io_service_attachment_proves_bar5_use": False,
            "provider_is_open_proves_mailbox_ownership": False,
            "provider_is_closed_excludes_raw_mmio_writers": False,
            "stable_mmio_values_exclude_firmware_activity": False,
            "external_host_writers_excluded": False,
            "firmware_writers_excluded": False,
            "mailbox_exclusive_proven": False,
            "may_issue_smu_command": False,
        },
        "evidence_limitations": [
            "All provider-open states are host-API snapshots, not BAR5 ownership.",
            "IOService attachment does not imply a PCI provider-open client.",
            "A response register already equal to 1 is not a new SMU transaction.",
            "No sampled transitions cannot exclude between-sample writers.",
            "The report does not independently interrogate GPU MMIO or firmware.",
            "Kernel lifetime guards do not survive unloading/reloading a Kext.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True,
                        help="a saved ioreg/RedNavi P7.33 output file")
    parser.add_argument("--output", type=Path,
                        help="optional JSON report path (must not exist)")
    args = parser.parse_args()
    result = summarize(args.input.expanduser().read_text(encoding="utf-8"))
    payload = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        with args.output.expanduser().open("x", encoding="utf-8") as target:
            target.write(payload)
        print("Wrote P7.34 offline report to", args.output.expanduser())
    else:
        print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
