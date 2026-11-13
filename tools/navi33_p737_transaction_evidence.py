#!/usr/bin/env python3
"""Analyze saved P7.36/P7.37 IORegistry output without touching hardware.

A stable C2PMSG_90 value of 1 is an existing status, NOT a response to
a command authored by RedNavi. Likewise, a changed C2PMSG_66 / _82
value does NOT identify an Apple or PSP/SMU writer. Samples are
sequential, non-atomic; missing observations remain null/unknown.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

SAMPLE = re.compile(
    r'"navi33-p7-mp1-(?P<kind>response|triplet)-sample-(?P<index>[12])"'
    r'\s*=\s*\{(?P<body>[^{}\n]*)\}'
)
ROUNDTRIP = re.compile(
    r'"navi33-p7-smu-roundtrip"\s*=\s*\{(?P<body>[^{}\n]*)\}'
)
FIELD = re.compile(
    r'"(?P<key>[A-Za-z0-9_-]+)"\s*=\s*'
    r'(?P<value>0x[0-9A-Fa-f]+|[0-9]+|Yes|No)'
)


def parse_fields(text: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for field in FIELD.finditer(text):
        raw = field.group("value")
        values[field.group("key")] = (
            1 if raw == "Yes" else 0 if raw == "No" else int(raw, 0)
        )
    return values


def observed(record: dict[str, int] | None, key: str) -> int | None:
    return record.get(key) if record is not None else None


def classify(previous: dict[str, int] | None,
             current: dict[str, int] | None,
             response: dict[str, int] | None) -> str:
    if current is None or response is None:
        return "missing_matched_samples"
    if (current.get("read_performed") != 1 or
        response.get("read_performed") != 1 or
        current.get("paired_p736_response_from_this_tick") != 1):
        return "unverified_or_unpaired_read"
    if current.get("response") != response.get("response"):
        return "cross_report_response_mismatch"
    if previous is None:
        return "first_unattributed_triplet"
    for key in ("message", "parameter", "response"):
        if key not in previous or key not in current:
            return "incomplete_triplet_evidence"
    if any(previous[key] != current[key]
           for key in ("message", "parameter", "response")):
        return "register_change_writer_unattributed"
    return "stable_triplet_not_a_new_transaction"


def summarize(text: str) -> dict[str, object]:
    pairs: dict[int, dict[str, dict[str, int]]] = {
        1: {}, 2: {}
    }
    for match in SAMPLE.finditer(text):
        pairs[int(match.group("index"))][match.group("kind")] = (
            parse_fields(match.group("body"))
        )
    roundtrips = [
        parse_fields(m.group("body")) for m in ROUNDTRIP.finditer(text)
    ]
    p727 = roundtrips[-1] if roundtrips else None
    result: list[dict[str, object]] = []
    previous: dict[str, int] | None = None
    for index in (1, 2):
        response = pairs[index].get("response")
        triplet = pairs[index].get("triplet")
        state = classify(previous, triplet, response)
        if (
            triplet is not None and response is not None and
            triplet.get("read_performed") == 1 and
            response.get("read_performed") == 1 and
            triplet.get("paired_p736_response_from_this_tick") == 1 and
            triplet.get("response") == response.get("response")
        ):
            previous = triplet
        result.append({
            "sample": index,
            "p736_response": observed(response, "response"),
            "p736_read_performed": observed(response, "read_performed"),
            "p737_read_performed": observed(triplet, "read_performed"),
            "message": observed(triplet, "message"),
            "parameter": observed(triplet, "parameter"),
            "triplet_response": observed(triplet, "response"),
            "reported_evidence_class": observed(triplet, "evidence_class"),
            "paired_from_this_timer_tick": observed(
                triplet, "paired_p736_response_from_this_tick"),
            "source_classification": state,
            "non_atomic_sequential_reads": True,
            "apple_or_other_host_writer_attributed": False,
            "new_firmware_response_to_rednavi_command_proven": False,
        })
    if p727 is None:
        p727_attempted = None
        p727_sent = None
        p727_handshake = None
    else:
        p727_attempted = p727.get("attempted")
        p727_sent = p727.get("command_sent")
        p727_handshake = p727.get("handshake_verified")
    return {
        "schema": "navi33-p737-passive-triplet-evidence-v1",
        "offline_only": True,
        "p727_attempted": p727_attempted,
        "p727_command_sent": p727_sent,
        "p727_handshake_verified": p727_handshake,
        "samples": result,
        "interpretation": {
            "stable_response_one_proves_new_handshake": False,
            "triplet_change_identifies_apple_or_psp_writer": False,
            "triplet_unchanged_excludes_between_sample_writes": False,
            "snapshot_is_atomic": False,
            "external_mailbox_writers_excluded": False,
            "mailbox_exclusive_proven": False,
            "may_issue_smu_command": False,
        },
        "limitations": [
            "The P7.36 Response value is sampled before P7.37 Message and Parameter.",
            "A register may be changed and restored between timer callbacks.",
            "P7.27 telemetry covers that one guarded RedNavi path, not all drivers.",
            "Apple/firmware writer attribution requires independently validated tracing.",
            "This tool only parses saved text; it cannot access or command a GPU.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", type=Path,
                        help="optional path for a new JSON report")
    args = parser.parse_args()
    data = args.input.expanduser().read_text(encoding="utf-8")
    report = summarize(data)
    payload = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        with args.output.expanduser().open("x", encoding="utf-8") as stream:
            stream.write(payload)
        print("Wrote P7.37 report to", args.output.expanduser())
    else:
        print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
