#!/usr/bin/env python3
"""Capture *post-boot* Navi33 IOKit service evidence without GPU I/O.

This is an observation helper, NOT a mailbox owner attestation. It does
not access BARs, write registers, obtain a PCI provider open, or issue
commands. It cannot show whether AMDSupport/firmware writes SMU C2PMSG.
Runs unprivileged on macOS using ioreg and kmutil.
"""

from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path
import re
import subprocess
import sys
import time

TREE_ENTRY = re.compile(r"^(?P<prefix>.*?)(?:\+-o\s+)(?P<body>.+)$")
GFX0 = re.compile(r"^GFX0(?:@|\s|$)")
INTERESTING_KEXT = re.compile(
    r"\b(?:AMDSupport|AMDRadeon\w*|RedNavi\w*|AppleGraphicsControl)\b",
    re.IGNORECASE,
)
INTERESTING_PROPERTY = re.compile(
    r'"navi33-p7-(?:mailbox-provider-audit|smu-host-serializer|'
    r'smu-transaction-guard|smu-lifetime-latch|smu-roundtrip)"'
)


def gfx0_subtrees(tree: str) -> list[str]:
    """Extract GFX0 node(s) and descendants, stopping at same-depth siblings."""
    lines = tree.splitlines()
    blocks = []
    for i, line in enumerate(lines):
        match = TREE_ENTRY.match(line)
        if not match or not GFX0.match(match.group("body")):
            continue
        base_depth = len(match.group("prefix"))
        block = [line]
        for tail in lines[i + 1:]:
            child = TREE_ENTRY.match(tail)
            if child and len(child.group("prefix")) <= base_depth:
                break
            if child:
                block.append(tail)
        blocks.append("\n".join(block))
    return blocks


def select_interesting_kexts(data: str) -> str:
    return "\n".join(x for x in data.splitlines() if INTERESTING_KEXT.search(x))


def select_telemetry(data: str) -> str:
    return "\n".join(x for x in data.splitlines()
                     if INTERESTING_PROPERTY.search(x))


def invoke(command: list[str]) -> tuple[str, str, int]:
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                errors="replace", timeout=20, check=False)
        return result.stdout, result.stderr, result.returncode
    except (OSError, subprocess.TimeoutExpired) as exc:
        return "", f"{type(exc).__name__}: {exc}", 127


def capture(index: int) -> str:
    now = dt.datetime.now(dt.timezone.utc).astimezone().isoformat()
    tree, tree_err, tree_rc = invoke(["ioreg", "-p", "IOService", "-w0", "-t"])
    props, props_err, props_rc = invoke(
        ["ioreg", "-p", "IOService", "-r", "-l", "-w0", "-c", "RedNaviProbe"]
    )
    loaded, loaded_err, loaded_rc = invoke(["kmutil", "showloaded"])

    subtrees = gfx0_subtrees(tree)
    kexts = select_interesting_kexts(loaded)
    telemetry = select_telemetry(props)

    parts = [
        f"===== sample {index} at {now} =====",
        f"ioreg tree exit={tree_rc}" + (f" stderr={tree_err.strip()}" if tree_err else ""),
        "\n-- GFX0 IOService subtree (children, not proven open owners) --",
        "\n\n".join(subtrees) if subtrees else "(no GFX0 found)",
        f"\nioreg RedNaviProbe exit={props_rc}" +
        (f" stderr={props_err.strip()}" if props_err else ""),
        "\n-- RedNavi P7 owner/admission telemetry --",
        telemetry if telemetry else "(no matching P7 properties)",
        f"\nkmutil showloaded exit={loaded_rc}" +
        (f" stderr={loaded_err.strip()}" if loaded_err else ""),
        "\n-- related loaded Kexts (loading != hardware binding) --",
        kexts if kexts else "(no matching loaded Kexts)",
        "\n-- interpretation limits --",
        "A child appearing under GFX0 does not establish it opened the PCI provider.",
        "An opened PCI provider does not establish exclusive access to MP1 firmware mailbox.",
        "An unchanged IORegistry snapshot does NOT exclude BAR5 writers or firmware activity.",
        "Neither ioreg nor kmutil proves which service has written C2PMSG_66/82/90.",
    ]
    return "\n".join(parts) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=2,
                        help="post-boot snapshots (1 to 5; default 2)")
    parser.add_argument("--interval", type=float, default=3.0,
                        help="seconds between samples (0 to 30; default 3)")
    parser.add_argument("--output", type=Path, required=True,
                        help="write collected text to this new file")
    args = parser.parse_args(argv)
    if not 1 <= args.samples <= 5:
        parser.error("--samples must be in [1, 5]")
    if not 0 <= args.interval <= 30:
        parser.error("--interval must be in [0, 30]")
    if sys.platform != "darwin":
        parser.error("this command collects live IOKit evidence on macOS only")

    # Exclusive create prevents accidentally replacing a prior evidence log.
    with args.output.expanduser().open("x", encoding="utf-8") as report:
        report.write(
            "RedNavi P7.32: post-boot user-space IOService observations\n"
            "READ-ONLY: no GPU register or PCI configuration writes\n"
            "This is not an SMU mailbox ownership or command-success proof.\n\n"
        )
        for i in range(args.samples):
            if i:
                time.sleep(args.interval)
            report.write(capture(i + 1))
            report.flush()
    print(f"Wrote {args.samples} read-only snapshots to {args.output.expanduser()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
