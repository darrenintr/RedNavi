#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Read-only structural comparison of LOCAL Cobalt64 ioaccel-layout model.json.

Numbers describe existing RedNavi macOS 15 assumptions, NOT confirmed Tahoe
values. A structural match does NOT prove compiled vtable ABI equivalence.
No Apple-derived KC, dump, generated header or symbol list is included.
"""
import argparse
import json
from pathlib import Path
import sys

REFERENCE = {
    "IOGraphicsAccelerator2": {"nslots": 350, "size": 3544},
    "IOAccelDevice2": {"nslots": 309},
    "IOAccelDisplayPipe": {"nslots": 306},
    "IOAccelSurface": {"nslots": 327},
}

def audit_model(model, reference=REFERENCE):
    problems = []
    if not isinstance(model, dict) or not isinstance(model.get("classes"), dict):
        return ["invalid model: expected classes object"]
    classes = model["classes"]
    for name, expected in reference.items():
        entry = classes.get(name)
        if not isinstance(entry, dict):
            problems.append(name + ": missing class")
            continue
        nslots = entry.get("nslots")
        if type(nslots) is not int or nslots <= 0 or nslots > 4096:
            problems.append(name + ": invalid nslots")
            continue
        if nslots != expected["nslots"]:
            problems.append(name + ": slot count mismatches RedNavi baseline")
        if "size" in expected:
            size = entry.get("size")
            if type(size) is not int or size != expected["size"]:
                problems.append(name + ": instance size mismatches RedNavi baseline")
        rows = entry.get("rows")
        if not isinstance(rows, list) or len(rows) != nslots:
            problems.append(name + ": vtable row count mismatches nslots")
            continue
        for i, row in enumerate(rows):
            if not isinstance(row, dict) or type(row.get("i")) is not int or row["i"] != i:
                problems.append(name + ": duplicate, missing or reordered slot index")
                break
    return problems

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True,
                        help="LOCAL Cobalt64 layout-out/model.json (do not commit it)")
    args = parser.parse_args(argv)
    try:
        model = json.loads(args.model.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        print("ABI audit input error:", exc, file=sys.stderr)
        return 2
    problems = audit_model(model)
    if problems:
        print("ABI MISMATCH: assumptions from RedNavi's macOS 15 model must not be reused blindly.")
        for p in problems:
            print(" -", p)
        print("Run Cobalt64 tools/ioaccel-layout/check_layout.py on the target macOS.")
        return 1
    print("STRUCTURAL BASELINE MATCH: slot counts, indices and known instance size.")
    print("NOT proof of compiled-vtable correctness; run Cobalt64 check_layout.py.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
