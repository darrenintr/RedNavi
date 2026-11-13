"""P7.37 offline receipt interpreter does not infer ownership."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/navi33_p737_transaction_evidence.py"
SPEC = importlib.util.spec_from_file_location("p737", SCRIPT)
assert SPEC and SPEC.loader
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


def sample(kind: str, index: int, **fields: int) -> str:
    body = ",".join(f'"{key}"={value}' for key, value in fields.items())
    return f' "navi33-p7-mp1-{kind}-sample-{index}" = {{{body}}}\n'


class Navi33P737OfflineReviewTests(unittest.TestCase):
    def test_actual_p736_rx7600_stable_response_one(self):
        text = (
            sample("response", 1, sample=1, read_performed=1,
                   response=1, changed_from_previous=0,
                   kext_read_count=1, handshake_verified=0) +
            sample("response", 2, sample=2, read_performed=1,
                   response=1, changed_from_previous=0,
                   kext_read_count=2, handshake_verified=0) +
            ' "navi33-p7-smu-roundtrip" = {"attempted"=0,'
            '"command_sent"=0,"handshake_verified"=0}\n'
        )
        r = review.summarize(text)
        self.assertEqual([s["p736_response"] for s in r["samples"]],
                         [1, 1])
        self.assertEqual([s["source_classification"] for s in r["samples"]],
                         ["missing_matched_samples"] * 2)
        self.assertEqual(r["p727_attempted"], 0)
        self.assertEqual(r["p727_command_sent"], 0)
        self.assertEqual(r["p727_handshake_verified"], 0)
        self.assertFalse(r["interpretation"]["mailbox_exclusive_proven"])
        self.assertFalse(r["interpretation"]["may_issue_smu_command"])

    def test_stable_three_field_values_not_handshake(self):
        text = ' "navi33-p7-smu-roundtrip" = {"command_sent"=0}\n'
        for i in (1, 2):
            text += sample("response", i, sample=i, read_performed=1,
                           response=1)
            text += sample("triplet", i, sample=i, read_performed=1,
                           paired_p736_response_from_this_tick=1,
                           message=12, parameter=24, response=1,
                           evidence_class=1 if i == 1 else 2)
        r = review.summarize(text)
        self.assertEqual(r["samples"][0]["source_classification"],
                         "first_unattributed_triplet")
        self.assertEqual(r["samples"][1]["source_classification"],
                         "stable_triplet_not_a_new_transaction")
        self.assertFalse(r["interpretation"]["snapshot_is_atomic"])
        self.assertFalse(
            r["interpretation"]["triplet_unchanged_excludes_between_sample_writes"])

    def test_message_change_without_response_change_is_unattributed(self):
        text = (
            sample("response", 1, read_performed=1, response=1) +
            sample("triplet", 1, read_performed=1,
                   paired_p736_response_from_this_tick=1,
                   message=10, parameter=0, response=1) +
            sample("response", 2, read_performed=1, response=1) +
            sample("triplet", 2, read_performed=1,
                   paired_p736_response_from_this_tick=1,
                   message=11, parameter=0, response=1)
        )
        r = review.summarize(text)
        self.assertEqual(r["samples"][1]["source_classification"],
                         "register_change_writer_unattributed")
        self.assertFalse(
            r["samples"][1]["apple_or_other_host_writer_attributed"])
        self.assertFalse(
            r["samples"][1]["new_firmware_response_to_rednavi_command_proven"])

    def test_response_mismatch_is_not_silently_accepted(self):
        text = (
            sample("response", 1, read_performed=1, response=1) +
            sample("triplet", 1, read_performed=1,
                   paired_p736_response_from_this_tick=1,
                   message=10, parameter=0, response=2)
        )
        r = review.summarize(text)
        self.assertEqual(r["samples"][0]["source_classification"],
                         "cross_report_response_mismatch")

    def test_explicit_unpaired_is_not_transaction(self):
        text = (
            sample("response", 1, read_performed=1, response=1) +
            sample("triplet", 1, read_performed=0,
                   paired_p736_response_from_this_tick=0)
        )
        r = review.summarize(text)
        self.assertEqual(r["samples"][0]["source_classification"],
                         "unverified_or_unpaired_read")
        self.assertIsNone(r["samples"][0]["message"])

    def test_missing_data_does_not_become_false(self):
        r = review.summarize("No RedNaviProbe attached\n")
        self.assertIsNone(r["p727_command_sent"])
        self.assertIsNone(r["samples"][0]["p736_response"])
        self.assertIsNone(r["samples"][0]["p737_read_performed"])
        self.assertEqual(r["samples"][0]["source_classification"],
                         "missing_matched_samples")
        self.assertFalse(r["interpretation"]["may_issue_smu_command"])

    def test_malicious_text_never_executes(self):
        text = (
            ' "navi33-p7-mp1-triplet-sample-1" = '
            '{"response"=$(touch /tmp/p737-not-allowed)}'
        )
        r = review.summarize(text)
        self.assertIsNone(r["samples"][0]["message"])
        self.assertFalse(r["interpretation"]["mailbox_exclusive_proven"])


if __name__ == "__main__":
    unittest.main()
