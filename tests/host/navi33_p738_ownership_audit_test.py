"""P7.38 offline ownership audit: consistency never confers SMU permission."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/navi33_p738_ownership_audit.py"
SPEC = importlib.util.spec_from_file_location("p738", SCRIPT)
assert SPEC and SPEC.loader
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


def record(kind, sample_index, **data):
    return (
        f'"navi33-p7-mp1-{kind}-sample-{sample_index}" = ' +
        "{" + ",".join(f'"{k}"={v}' for k, v in data.items()) + "}\n"
    )


def fixture(change=False):
    text = ""
    for i in (1, 2):
        lo = 1000 * i
        text += record(
            "response", i, error=11, read_performed=1,
            response_byte_offset=363112, kext_read_count=i,
            response=1, timestamp_ns_hi=i,
            timestamp_ns_lo=lo,
            mailbox_exclusive_proven=0, may_issue_smu_command=0,
            handshake_verified=0, register_writes_in_new_path=0,
        )
        text += record(
            "triplet", i, status=13, sample=i, read_performed=1,
            message=7 if i == 2 and change else 0,
            parameter=0, response=1, message_byte_offset=363016,
            parameter_byte_offset=363080, response_byte_offset=363112,
            additional_reads_this_sample=2, kext_additional_reads_claimed=2*i,
            response_was_read_before_message_parameter=1,
            paired_p736_response_from_this_tick=1,
            previous_triplet_available=int(i == 2),
            evidence_class=(3 if change else 2) if i == 2 else 1,
            timestamp_ns_hi=i, timestamp_ns_lo=lo+20,
            mailbox_exclusive_proven=0, may_issue_smu_command=0,
            rednavi_command_submitted_proven=0,
            external_host_writer_attributed=0,
            firmware_response_to_new_command_proven=0,
        )
    return text


class P738OfflineAuditTests(unittest.TestCase):
    def test_stable_complete_triplet_does_not_grant_lease(self):
        r = audit.audit_saved_capture(fixture())
        self.assertEqual(r["sample_integrity"], "incomplete_or_inconsistent")
        self.assertEqual(r["findings"], ["source_inventory_not_provided"])
        self.assertEqual([x["response"] for x in r["samples"]], [1, 1])
        self.assertEqual([x["message"] for x in r["samples"]], [0, 0])
        self.assertFalse(r["decision"]["may_issue_smu_command"])

    def test_changed_triplet_not_attributed(self):
        r = audit.audit_saved_capture(fixture(change=True))
        self.assertEqual(r["p737_classifications"][1],
                         "register_change_writer_unattributed")
        self.assertFalse(r["decision"]["enable_p727_get_smu_version"])

    def test_inconsistent_class_is_detected(self):
        raw = fixture().replace('"evidence_class"=2',
                                '"evidence_class"=3')
        r = audit.audit_saved_capture(raw)
        self.assertIn("p737_2_evidence_class_mismatch", r["findings"])

    def test_duplicate_sample_is_not_silently_accepted(self):
        raw = fixture() + record("response", 1, response=1)
        r = audit.audit_saved_capture(raw)
        self.assertIn("p736_sample_1_count_2", r["findings"])

    def test_mismatched_offset_is_detected(self):
        raw = fixture().replace('"message_byte_offset"=363016',
                                '"message_byte_offset"=363020')
        r = audit.audit_saved_capture(raw)
        self.assertIn("p737_1_wrong_message_byte_offset", r["findings"])

    def test_timestamp_reversal_is_detected(self):
        raw = fixture().replace('"timestamp_ns_lo"=1020',
                                '"timestamp_ns_lo"=990')
        r = audit.audit_saved_capture(raw)
        self.assertIn("sample_1_timestamp_order_invalid", r["findings"])

    def test_claimed_mailbox_ownership_does_not_grant_permission(self):
        raw = fixture().replace('"mailbox_exclusive_proven"=0',
                                '"mailbox_exclusive_proven"=1')
        r = audit.audit_saved_capture(raw)
        self.assertTrue(any("mailbox_exclusive_proven_not_zero" in item
                            for item in r["findings"]))
        self.assertFalse(r["decision"]["may_issue_smu_command"])

    def test_source_baseline_failure_is_reported(self):
        r = audit.audit_saved_capture(fixture(), {})
        self.assertIn("source_writer_baseline_changed", r["findings"])
        self.assertFalse(r["rednavi_source_inventory"]
                         ["known_sites_match_reviewed_baseline"])

    def test_missing_data_does_not_turn_into_success(self):
        r = audit.audit_saved_capture("no attached RedNaviProbe\n")
        self.assertFalse(r["samples"][0]["present_and_unambiguous"])
        self.assertFalse(r["decision"]["may_issue_smu_command"])

    def test_untrusted_saved_capture_not_executed(self):
        r = audit.audit_saved_capture(
            '"navi33-p7-mp1-triplet-sample-1" = '
            '{"message"=$(touch /tmp/should-not-run)}'
        )
        self.assertFalse(r["decision"]["may_issue_smu_command"])


if __name__ == "__main__":
    unittest.main()
