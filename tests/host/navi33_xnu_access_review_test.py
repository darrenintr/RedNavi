"""P7.34 offline IORegistry output parser; never touches hardware."""
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "tools/navi33_xnu_access_review.py"
SPEC = importlib.util.spec_from_file_location("navi33_xnu_access_review", SCRIPT)
assert SPEC and SPEC.loader
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


def line(i: int, **values: int) -> str:
    content = ",".join(f'"{k}"={v}' for k, v in values.items())
    return f'  "navi33-p7-late-provider-audit-{i}" = {{{content}}}\n'


class Navi33XnuAccessReviewTests(unittest.TestCase):
    def test_physical_p733_two_stage_mac_boot(self):
        raw = (
            line(1, sample_valid=1, direct_children_observed=1,
                 amd_support_present=0, boot_framebuffer_present=0,
                 provider_open_by_any=0, provider_open_by_rednavi=0,
                 amd_support_provider_open=0,
                 boot_framebuffer_provider_open=0) +
            line(2, sample_valid=1, direct_children_observed=3,
                 amd_support_present=1, boot_framebuffer_present=1,
                 provider_open_by_any=0, provider_open_by_rednavi=0,
                 amd_support_provider_open=0,
                 boot_framebuffer_provider_open=0) +
            ' "navi33-p7-smu-roundtrip" = {"command_sent"=0,'
            '"handshake_verified"=0,"response_before"=1}\n'
        )
        r = review.summarize(raw)
        self.assertEqual(len(r["kernel_late_samples"]), 2)
        self.assertEqual(r["latest_valid_sample_index"], 2)
        self.assertEqual(
            [x["direct_children_observed"] for x in r["kernel_late_samples"]],
            [1, 3],
        )
        self.assertTrue(r["any_apple_services_attached_at_samples"])
        self.assertFalse(r["any_host_provider_open_observed"])
        self.assertTrue(r["latest_host_open_reported_no_at_sample"])
        self.assertFalse(r["late_snapshot_inconsistent"])
        self.assertEqual(r["smu_command_sent_telemetry"], 0)
        self.assertEqual(r["smu_handshake_verified_telemetry"], 0)
        self.assertFalse(r["interpretation"]["mailbox_exclusive_proven"])
        self.assertFalse(r["interpretation"]["may_issue_smu_command"])

    def test_missing_data_is_unknown_not_false(self):
        r = review.summarize('no probe available\n')
        self.assertEqual(r["kernel_late_samples"], [])
        self.assertIsNone(r["latest_valid_sample_index"])
        self.assertIsNone(r["latest_host_open_reported_no_at_sample"])
        self.assertIsNone(r["smu_command_sent_telemetry"])
        self.assertIsNone(r["smu_handshake_verified_telemetry"])
        self.assertFalse(r["interpretation"]["may_issue_smu_command"])

    def test_missing_open_field_is_not_closed(self):
        r = review.summarize(line(2, sample_valid=1,
                                  amd_support_present=1,
                                  boot_framebuffer_present=1))
        s = r["kernel_late_samples"][0]
        self.assertFalse(s["complete_open_query"])
        self.assertIsNone(s["host_open_reported_no_at_that_instant"])
        self.assertIsNone(r["latest_host_open_raw"])
        self.assertFalse(r["interpretation"]["mailbox_exclusive_proven"])

    def test_apple_open_detected_no_auto_permission(self):
        r = review.summarize(line(
            2, sample_valid=1, provider_open_by_any=1,
            amd_support_present=1, amd_support_provider_open=1,
            boot_framebuffer_present=1, boot_framebuffer_provider_open=0,
            provider_open_by_rednavi=0))
        self.assertTrue(r["any_host_provider_open_observed"])
        self.assertFalse(r["latest_host_open_reported_no_at_sample"])
        self.assertFalse(r["late_snapshot_inconsistent"])
        self.assertFalse(r["interpretation"]["may_issue_smu_command"])

    def test_inconsistent_host_snapshot_detected(self):
        r = review.summarize(line(2, sample_valid=1,
                                  provider_open_by_any=0,
                                  amd_support_provider_open=1))
        self.assertTrue(r["late_snapshot_inconsistent"])
        self.assertFalse(r["interpretation"]["mailbox_exclusive_proven"])

    def test_not_observing_a_writer_does_not_mean_excluded(self):
        r = review.summarize(line(2, sample_valid=1,
                                  provider_open_by_any=0))
        self.assertTrue(r["latest_host_open_reported_no_at_sample"])
        self.assertFalse(r["interpretation"]["external_host_writers_excluded"])
        self.assertFalse(r["interpretation"]["firmware_writers_excluded"])

    def test_no_shell_or_code_execution_on_untrusted_input(self):
        text = '''
"navi33-p7-late-provider-audit-2" = {"sample_valid"=1,
"provider_open_by_any"=$(touch /tmp/bad)}
'''
        r = review.summarize(text)
        self.assertEqual(r["kernel_late_samples"], [])
        self.assertFalse(r["interpretation"]["may_issue_smu_command"])


if __name__ == "__main__":
    unittest.main()
