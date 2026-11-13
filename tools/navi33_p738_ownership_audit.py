#!/usr/bin/env python3
"""P7.38: offline Navi33 MP1 observation and mailbox ownership audit.

Consumes saved IORegistry text and optionally RedNavi source files. It never
opens a device, maps a BAR, writes registers, claims a mailbox lease, or
enables an SMU command. Textual evidence cannot prove an exclusive GPU owner.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

TOOL_DIR = Path(__file__).resolve().parent
if str(TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(TOOL_DIR))

import navi33_mp1_source_inventory as source_inventory
import navi33_p737_transaction_evidence as triplet_evidence
import navi33_xnu_access_review as host_evidence

REPO = TOOL_DIR.parent
EXPECTED_OFFSETS = {
    "message_byte_offset": 363016,
    "parameter_byte_offset": 363080,
    "response_byte_offset": 363112,
}
SAMPLE_FIELDS = {
    "response": ("read_performed", "response"),
    "triplet": ("read_performed", "message", "parameter", "response"),
}
TRIPLET_ZERO_FLAGS = (
    "mailbox_exclusive_proven",
    "may_issue_smu_command",
    "rednavi_command_submitted_proven",
    "external_host_writer_attributed",
    "firmware_response_to_new_command_proven",
)
RESPONSE_ZERO_FLAGS = (
    "mailbox_exclusive_proven",
    "may_issue_smu_command",
    "handshake_verified",
    "register_writes_in_new_path",
)
NAMED_SAMPLE = re.compile(
    r'"navi33-p7-mp1-(response|triplet)-sample-([12])"\s*=\s*'
    r'\{([^{}\n]*)\}'
)


def nanoseconds(record: dict[str, int]) -> int | None:
    low = record.get("timestamp_ns_lo")
    high = record.get("timestamp_ns_hi")
    if high is None or low is None:
        return None
    if high < 0 or high > 0xFFFFFFFF or low < 0 or low > 0xFFFFFFFF:
        return None
    return (high << 32) | low


def audit_saved_capture(
    text: str, sources: dict[str, str] | None = None
) -> dict[str, object]:
    records: dict[tuple[str, int], list[dict[str, int]]] = {
        (kind, index): []
        for kind in ("response", "triplet") for index in (1, 2)
    }
    for match in NAMED_SAMPLE.finditer(text):
        key = (match.group(1), int(match.group(2)))
        records[key].append(triplet_evidence.parse_fields(match.group(3)))

    findings: list[str] = []
    samples: list[dict[str, object]] = []
    prev_triplet: dict[str, int] | None = None
    prev_timestamp: int | None = None
    for index in (1, 2):
        response_rows = records[("response", index)]
        triplet_rows = records[("triplet", index)]
        if len(response_rows) != 1:
            findings.append(f"p736_sample_{index}_count_{len(response_rows)}")
        if len(triplet_rows) != 1:
            findings.append(f"p737_sample_{index}_count_{len(triplet_rows)}")
        response = response_rows[0] if len(response_rows) == 1 else {}
        triplet = triplet_rows[0] if len(triplet_rows) == 1 else {}
        paired = bool(response and triplet)
        for kind, row in (("response", response), ("triplet", triplet)):
            if not row:
                continue
            for field in SAMPLE_FIELDS[kind]:
                if field not in row:
                    findings.append(f"{kind}_{index}_missing_{field}")
            if row.get("read_performed") != 1:
                findings.append(f"{kind}_{index}_not_sampled")

        if response:
            if response.get("error") != 11:
                findings.append(f"p736_{index}_unexpected_status")
            if response.get("kext_read_count") != index:
                findings.append(f"p736_{index}_unexpected_image_budget")
            if response.get("response_byte_offset") != 363112:
                findings.append(f"p736_{index}_wrong_response_offset")
            for field in RESPONSE_ZERO_FLAGS:
                if response.get(field) != 0:
                    findings.append(f"p736_{index}_{field}_not_zero")

        if triplet:
            if triplet.get("status") != 13:
                findings.append(f"p737_{index}_unexpected_status")
            if triplet.get("sample") != index:
                findings.append(f"p737_{index}_wrong_sample_number")
            if triplet.get("paired_p736_response_from_this_tick") != 1:
                findings.append(f"p737_{index}_not_paired_in_kernel")
            if triplet.get("additional_reads_this_sample") != 2:
                findings.append(f"p737_{index}_wrong_read_count")
            if triplet.get("kext_additional_reads_claimed") != index * 2:
                findings.append(f"p737_{index}_unexpected_image_budget")
            if triplet.get("response_was_read_before_message_parameter") != 1:
                findings.append(f"p737_{index}_unknown_read_order")
            for field, value in EXPECTED_OFFSETS.items():
                if triplet.get(field) != value:
                    findings.append(f"p737_{index}_wrong_{field}")
            for field in TRIPLET_ZERO_FLAGS:
                if triplet.get(field) != 0:
                    findings.append(f"p737_{index}_{field}_not_zero")
            if index == 1 and triplet.get("evidence_class") != 1:
                findings.append("p737_1_not_first_snapshot")
            if index == 2 and prev_triplet is not None:
                changed = any(
                    prev_triplet.get(field) != triplet.get(field)
                    for field in ("message", "parameter", "response")
                )
                expected_class = 3 if changed else 2
                if triplet.get("evidence_class") != expected_class:
                    findings.append("p737_2_evidence_class_mismatch")
            if index == 2 and triplet.get("previous_triplet_available") != 1:
                findings.append("p737_2_missing_previous_triplet")

        if paired and response.get("response") != triplet.get("response"):
            findings.append(f"sample_{index}_response_mismatch")
        t_response = nanoseconds(response)
        t_triplet = nanoseconds(triplet)
        if paired and (t_response is None or t_triplet is None):
            findings.append(f"sample_{index}_missing_timestamps")
        elif paired:
            if t_response > t_triplet:
                findings.append(f"sample_{index}_timestamp_order_invalid")
            if prev_timestamp is not None and t_response <= prev_timestamp:
                findings.append(f"sample_{index}_timestamp_not_monotonic")
            prev_timestamp = t_triplet

        if triplet:
            prev_triplet = triplet
        samples.append({
            "sample": index,
            "present_and_unambiguous": paired,
            "message": triplet.get("message"),
            "parameter": triplet.get("parameter"),
            "response": triplet.get("response"),
            "changed_from_previous": (
                None if index == 1 or not paired or
                not response or len(records[("triplet", 1)]) != 1 else
                any(records[("triplet", 1)][0].get(field) != triplet.get(field)
                    for field in ("message", "parameter", "response"))
            ),
            "timestamp_response_ns": t_response,
            "timestamp_triplet_ns": t_triplet,
        })

    host = host_evidence.summarize(text)
    p737 = triplet_evidence.summarize(text)
    source_report = (
        source_inventory.inspect_sources(sources)
        if sources is not None else None
    )
    if source_report is None:
        findings.append("source_inventory_not_provided")
    elif not source_report["known_sites_match_reviewed_baseline"]:
        findings.append("source_writer_baseline_changed")
    if host["late_snapshot_inconsistent"]:
        findings.append("provider_open_snapshot_inconsistent")
    if host["smu_command_sent_telemetry"] == 1:
        findings.append("prior_smu_command_reported")

    return {
        "schema": "navi33-p738-ownership-audit-v1",
        "offline_only": True,
        "hardware_reads_or_writes": 0,
        "sample_integrity": (
            "consistent_observations" if not findings
            else "incomplete_or_inconsistent"
        ),
        "findings": findings,
        "samples": samples,
        "p737_classifications": [
            row["source_classification"] for row in p737["samples"]
        ],
        "host_snapshot": {
            "apple_service_attached_at_sample": (
                host["any_apple_services_attached_at_samples"]
            ),
            "latest_provider_open_reported_no": (
                host["latest_host_open_reported_no_at_sample"]
            ),
            "writer_exclusion_proven": False,
        },
        "rednavi_source_inventory": {
            "checked": source_report is not None,
            "known_sites_match_reviewed_baseline": (
                source_report["known_sites_match_reviewed_baseline"]
                if source_report is not None else None
            ),
            "named_direct_write_sites": (
                len(source_report["named_direct_mp1_writes"])
                if source_report is not None else None
            ),
            "p727_dynamic_write_sites": (
                len(source_report["p727_dynamic_writes"])
                if source_report is not None else None
            ),
            "p727_lease_hardcoded_false": (
                source_report["p727_mailbox_lease_hardcoded_false"]
                if source_report is not None else None
            ),
        },
        "unresolved_ownership_blockers": [
            "No platform-enforced exclusive MP1 mailbox lease demonstrated",
            "Other host BAR5 writers, including Apple paths, not excluded",
            "Asynchronous firmware/SMU mailbox activity not excluded",
            "No attributable new RedNavi request/response transaction",
            "Passive, non-atomic snapshots cannot establish ownership",
        ],
        "decision": {
            "may_issue_smu_command": False,
            "enable_p727_get_smu_version": False,
            "enable_rlc_mes_or_graphics_queue": False,
            "reason": "No verified mailbox arbitration or owned SMU roundtrip",
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path,
                        help="saved, unmodified ioreg -r -l -w0 -c RedNaviProbe")
    parser.add_argument("--repo", type=Path, default=REPO,
                        help="RedNavi checkout to inventory (default: script repo)")
    parser.add_argument("--output", type=Path,
                        help="new JSON output file; will not overwrite")
    args = parser.parse_args()
    text = args.input.expanduser().read_text(encoding="utf-8")
    sources = source_inventory.load_repo_sources(args.repo.expanduser())
    report = audit_saved_capture(text, sources)
    payload = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        with args.output.expanduser().open("x", encoding="utf-8") as out:
            out.write(payload)
        print("Wrote P7.38 ownership audit to", args.output.expanduser())
    else:
        print(payload, end="")
    return 0 if report["sample_integrity"] == "consistent_observations" else 2


if __name__ == "__main__":
    raise SystemExit(main())
