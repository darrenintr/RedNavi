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

    def test_duplicate_fields_are_ambiguous_even_if_last_value_is_safe(self):
        raw = fixture().replace('"mailbox_exclusive_proven"=0',
                                '"mailbox_exclusive_proven"=1,"mailbox_exclusive_proven"=0', 1)
        r = p739.report(self.sources, raw)
        self.assertEqual(r['capture_audit']['findings'], [])
        self.assertFalse(r['evidence_complete'])
        self.assertEqual(r['duplicate_sample_fields'][0]['field'], 'mailbox_exclusive_proven')
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

    def test_candidate_delta_detects_new_dynamic_write_and_duplicate(self):
        old = p739.inventory({'a.cpp': 'wr(offset, value);\n'})
        new = p739.inventory({'a.cpp': 'wr(offset, value);\nwr(offset, value);\n'
                                        'writel(value, base + RN_RSMU_DATA);\n'})
        delta = p739.candidate_delta(old, new)
        self.assertTrue(delta['review_required'])
        self.assertEqual(sum(row['count'] for row in delta['added']), 2)
        self.assertEqual(delta['removed'], [])

    def test_candidate_delta_ignores_line_movement_but_detects_gate_change(self):
        old = p739.inventory({'a.cpp': 'constexpr bool p727MailboxLeaseHeld = false;\n'})
        moved = p739.inventory({'a.cpp': '\nconstexpr bool p727MailboxLeaseHeld = false;\n'})
        changed = p739.inventory({'a.cpp': 'constexpr bool p727MailboxLeaseHeld = true;\n'})
        self.assertFalse(p739.candidate_delta(old, moved)['review_required'])
        self.assertTrue(p739.candidate_delta(old, changed)['review_required'])

    def test_delta_compares_content_beyond_excerpt(self):
        prefix = 'wr(offset, value); ' + ('x' * 250)
        old = p739.inventory({'a.cpp': prefix + 'A\n'})
        new = p739.inventory({'a.cpp': prefix + 'B\n'})
        self.assertEqual(old['candidates'][0]['text'], new['candidates'][0]['text'])
        self.assertTrue(p739.candidate_delta(old, new)['review_required'])

    def test_bad_revision_is_rejected(self):
        import subprocess
        with self.assertRaises(subprocess.CalledProcessError):
            p739.load_revision_sources(ROOT, '--not-a-real-revision')

    def test_numeric_mailbox_and_raw_volatile_are_candidates(self):
        r = p739.inventory({'a.cpp': 'auto x = 0x58a68;\n'
                                     'volatile uint32_t *ptr;\n'})
        self.assertIn('observed_mailbox_offsets', r['candidates'][0]['kinds'])
        self.assertIn('volatile_mmio', r['candidates'][1]['kinds'])

    def test_revision_inventory_matches_tracked_file(self):
        sources = p739.load_revision_sources(ROOT, 'HEAD')
        self.assertIn('RedNavi/kern_probe.cpp', sources)
        self.assertIn('p727MailboxLeaseHeld = false', sources['RedNavi/kern_probe.cpp'])

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
