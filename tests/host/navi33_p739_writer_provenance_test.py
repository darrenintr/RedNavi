import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests/host'))
import navi33_p739_writer_provenance as p739
from navi33_p738_ownership_audit_test import fixture


class ProvenanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = p739.load_sources(ROOT)

    def test_complete_stable_capture_is_not_ownership(self):
        r = p739.report(self.sources, fixture())
        self.assertTrue(r['evidence_complete'])
        self.assertEqual(r['provenance']['runtime_register_transition'], 'no_difference_in_sequential_samples')
        self.assertEqual(r['provenance']['exclusive_mailbox_ownership'], 'unknown')
        self.assertFalse(r['decision']['may_issue_smu_command'])

    def test_transition_does_not_attribute_writer(self):
        r = p739.report(self.sources, fixture(change=True))
        self.assertEqual(r['provenance']['runtime_register_transition'], 'observed_between_sequential_samples')
        self.assertEqual(r['provenance']['attributed_host_write'], 'unknown')
        self.assertEqual(r['provenance']['firmware_originated_activity'], 'unknown')

    def test_missing_or_contradictory_evidence_fails_closed(self):
        for raw in (None, '', fixture().replace('"evidence_class"=2', '"evidence_class"=3'),
                    fixture().replace('"mailbox_exclusive_proven"=0', '"mailbox_exclusive_proven"=1')):
            r = p739.report(self.sources, raw)
            self.assertFalse(r['evidence_complete'])
            self.assertEqual(r['provenance']['runtime_register_transition'], 'unknown')
            self.assertFalse(r['decision']['may_issue_smu_command'])

    def test_dynamic_and_indirect_candidates_are_retained(self):
        src = {'new/path.cpp': 'void f() { wr(offset, v); rn_wreg32(dev, reg, v); }\n'
                               'void g() { writel(v, base + RN_RSMU_DATA); }\n'}
        rows = p739.inventory(src)['candidates']
        self.assertIn('raw_probe_access', rows[0]['kinds'])
        self.assertIn('register_write_wrapper', rows[0]['kinds'])
        self.assertIn('indirect_aperture', rows[1]['kinds'])
        self.assertFalse(p739.inventory(src)['all_possible_writers_excluded'])

    def test_disabled_gate_change_is_detected(self):
        sources = dict(self.sources)
        name = 'RedNavi/kern_probe.cpp'
        sources[name] = sources[name].replace('constexpr bool p727MailboxLeaseHeld = false;',
                                              'constexpr bool p727MailboxLeaseHeld = true;')
        r = p739.report(sources, fixture())
        self.assertFalse(r['evidence_complete'])
        self.assertFalse(r['decision']['may_issue_smu_command'])


if __name__ == '__main__':
    unittest.main()
