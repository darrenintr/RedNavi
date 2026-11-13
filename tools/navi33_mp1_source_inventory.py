#!/usr/bin/env python3
"""P7.34 inventory of KNOWN RedNavi MP1 C2PMSG host write sites.

Static source review only. Not a complete proof: dynamic BAR5 offsets,
other Kexts, direct physical mappings, interrupts and firmware writers may
not be captured by these patterns. This script never executes a Kext,
accesses IOKit, or interacts with any hardware.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

REPO = Path(__file__).resolve().parents[1]
DIRECT = re.compile(
    r"\bWREG32_SOC15\s*\(\s*RN_MP1\s*,\s*0\s*,\s*"
    r"mmMP1_SMN_C2PMSG_(66|82|90)\b"
)
DYNAMIC = re.compile(
    r"\bwrite\s*\(\s*reg\."
    r"(responseByteOffset|parameterByteOffset|messageByteOffset)\s*,"
)
P727_GATE = "constexpr bool p727MailboxLeaseHeld = false;"
P727_CALLBACK = "wr(offset, value);"
P727_ENTRY = "p727r::execute("
SOURCE_DIRS = ("RedNavi", "compat/src")


def matches(path: str, source: str, pattern: re.Pattern[str],
            kind: str) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for hit in pattern.finditer(source):
        line = source.count("\n", 0, hit.start()) + 1
        lines = source.splitlines()
        rows.append({
            "path": path,
            "line": line,
            "kind": kind,
            "register": hit.group(1),
            "excerpt": lines[line - 1].strip()[:180],
        })
    return rows


def inspect_sources(sources: dict[str, str]) -> dict[str, object]:
    direct: list[dict[str, object]] = []
    for name, contents in sorted(sources.items()):
        if name.endswith((".cpp", ".c", ".h")):
            direct.extend(matches(name, contents, DIRECT, "named_mp1_direct_write"))

    transport_name = "RedNavi/navi33_smu_mailbox_roundtrip.h"
    kern_name = "RedNavi/kern_probe.cpp"
    transport = sources.get(transport_name, "")
    kern = sources.get(kern_name, "")
    dynamic = matches(transport_name, transport, DYNAMIC,
                      "p727_dynamic_byte_offset_write")
    named_counts: dict[str, int] = {}
    for site in direct:
        path = str(site["path"])
        named_counts[path] = named_counts.get(path, 0) + 1
    expected_named = {
        "RedNavi/smu13.cpp": 3,
        "RedNavi/kern_probe.cpp": 1,
    }
    actual_dynamic = {str(s["register"]) for s in dynamic}
    expected_dynamic = {
        "responseByteOffset", "parameterByteOffset", "messageByteOffset"
    }
    p727_has_callsite = P727_ENTRY in kern
    p727_callback_is_raw_write = P727_CALLBACK in kern
    lease_remains_false = P727_GATE in kern
    complete_known_sites = (
        named_counts == expected_named and
        len(dynamic) == 3 and
        actual_dynamic == expected_dynamic and
        p727_has_callsite and p727_callback_is_raw_write and
        lease_remains_false
    )
    return {
        "schema": "navi33-p734-known-host-writers-v1",
        "source_review_only": True,
        "named_direct_mp1_writes": direct,
        "p727_dynamic_writes": dynamic,
        "p727_uses_raw_bar5_write_callback": p727_callback_is_raw_write,
        "p727_callsite_present": p727_has_callsite,
        "p727_mailbox_lease_hardcoded_false": lease_remains_false,
        "known_sites_match_reviewed_baseline": complete_known_sites,
        "indirect_or_unrecognized_host_writers_excluded": False,
        "third_party_bar5_writers_excluded": False,
        "firmware_writers_excluded": False,
        "mailbox_exclusive_proven": False,
        "may_issue_smu_command": False,
        "interpretation": [
            "The four named direct sites are not the only potential writes.",
            "P7.27 has three dynamic byte-offset writes via a caller-owned "
            "wr(offset,value) callback. Its physical lease is hardcoded off.",
            "Navi32 legacy timeout recovery writes C2PMSG_90=1; this "
            "synthetic response is not a firmware acknowledgment.",
            "RSMU index/data access can write an INDEX register while "
            "reading a firmware flag; no MP1 transaction is implied.",
            "No source scan can exclude external Kext, firmware, or "
            "unrecognized dynamic MMIO writers.",
        ],
    }


def load_repo_sources(root: Path) -> dict[str, str]:
    sources: dict[str, str] = {}
    for folder in SOURCE_DIRS:
        for file in (root / folder).glob("*"):
            if file.is_file() and file.suffix in {".cpp", ".c", ".h"}:
                sources[str(file.relative_to(root))] = file.read_text(
                    encoding="utf-8"
                )
    return sources


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=REPO)
    parser.add_argument("--output", type=Path,
                        help="optional new JSON output file")
    args = parser.parse_args()
    report = inspect_sources(load_repo_sources(args.repo.expanduser()))
    payload = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        with args.output.expanduser().open("x", encoding="utf-8") as target:
            target.write(payload)
        print("Wrote static source inventory to", args.output.expanduser())
    else:
        print(payload, end="")
    return 0 if report["known_sites_match_reviewed_baseline"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
