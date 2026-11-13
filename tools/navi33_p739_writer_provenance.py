#!/usr/bin/env python3
"""Offline, conservative source and saved-capture MP1 provenance report."""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile

import navi33_mp1_source_inventory as baseline
import navi33_p738_ownership_audit as audit

ROOT = Path(__file__).resolve().parent.parent
EXTENSIONS = {'.c', '.cc', '.cpp', '.h', '.hpp', '.m', '.mm', '.inc'}
# Candidates, not a parser or proof of reachability. Keep raw and indirect
# operations even when they have nothing to do with MP1.
PATTERNS = {
    'named_mp1': r'\b(?:mm|reg)?MP1_(?:SMN_)?C2PMSG_(?:66|82|90)\b',
    'dynamic_mailbox_offsets': r'\b(?:responseByteOffset|parameterByteOffset|messageByteOffset)\b',
    'register_write_wrapper': r'\b(?:WREG32(?:_SOC15)?|rn_wreg32|writel|write32|OSWriteLittleInt32)\s*\(',
    'register_read_wrapper': r'\b(?:RREG32(?:_SOC15)?|rn_rreg32|readl|read32|OSReadLittleInt32)\s*\(',
    'raw_probe_access': r'\b(?:wr|rd)\s*\(',
    'indirect_aperture': r'\b(?:RN_RSMU_(?:INDEX|DATA)|RN_MM_(?:INDEX(?:_HI)?|DATA)|kRNMm(?:Index(?:Hi)?|Data))\b',
    'pci_config_access': r'\b(?:configRead|configWrite|extendedConfigRead|extendedConfigWrite)(?:8|16|32)\s*\(',
    'bar_mapping': r'\bmapDeviceMemoryWith(?:Register|Index)\s*\(',
    'smu_sender': r'\b(?:rn_smu13_send|p727r::execute)\s*\(',
    'mailbox_gate': r'\b(?:p727MailboxLeaseHeld|smu_msg_lock_override|mmio_idx_lock)\b',
    'observed_mailbox_offsets': r'(?<![A-Za-z0-9_])(?:363016|363080|363112|0x58a08|0x58a48|0x58a68)(?![A-Za-z0-9_])',
    'volatile_mmio': r'\b(?:volatile\s+(?:u?int(?:8|16|32|64)_t|u(?:8|16|32|64))|reinterpret_cast\s*<\s*volatile)',
}
COMPILED = {key: re.compile(value) for key, value in PATTERNS.items()}


def load_sources(root: Path) -> dict[str, str]:
    # Enumerate tracked and untracked source files; ignored build/dependency files
    # are outside scope. This avoids treating an untracked new writer as invisible.
    result = subprocess.run(['git', '-C', str(root), 'ls-files', '-co',
                             '--exclude-standard', '-z'], check=True,
                            stdout=subprocess.PIPE)
    sources = {}
    for raw in result.stdout.split(b'\0'):
        if not raw:
            continue
        path = Path(raw.decode('utf-8', errors='surrogateescape'))
        if path.suffix.lower() not in EXTENSIONS:
            continue
        file = root / path
        if file.is_file() and not file.is_symlink():
            sources[path.as_posix()] = file.read_text(encoding='utf-8', errors='replace')
    return sources


def load_revision_sources(root: Path, revision: str) -> dict[str, str]:
    # git archive treats the revision as one argument, without shell expansion.
    resolved = subprocess.run(['git', '-C', str(root), 'rev-parse', '--verify',
                               '--end-of-options', revision + '^{commit}'],
                              check=True, stdout=subprocess.PIPE, text=True).stdout.strip()
    archive = subprocess.run(['git', '-C', str(root), 'archive', '--format=tar', resolved],
                             check=True, stdout=subprocess.PIPE)
    sources = {}
    with tarfile.open(fileobj=io.BytesIO(archive.stdout), mode='r:') as tar:
        for item in tar:
            path = Path(item.name)
            if item.isfile() and path.suffix.lower() in EXTENSIONS:
                stream = tar.extractfile(item)
                if stream is not None:
                    sources[path.as_posix()] = stream.read().decode('utf-8', errors='replace')
    return sources


def candidate_delta(before: dict, after: dict) -> dict:
    # Multisets avoid both line-number churn and silently dropping duplicates.
    def counts(report):
        return Counter((row['path'], row['text'], row['line_sha256'], tuple(row['kinds']))
                       for row in report['candidates'])
    old, new = counts(before), counts(after)
    def rows(delta):
        return [{'path': path, 'text': text, 'line_sha256': digest, 'kinds': list(kinds), 'count': count}
                for (path, text, digest, kinds), count in sorted(delta.items())]
    added, removed = rows(new - old), rows(old - new)
    old_files, new_files = before['file_sha256'], after['file_sha256']
    changed_files = [{'path': path, 'before_sha256': old_files.get(path),
                      'after_sha256': new_files.get(path)}
                     for path in sorted(old_files.keys() | new_files.keys())
                     if old_files.get(path) != new_files.get(path)]
    return {'added': added, 'removed': removed, 'changed_source_files': changed_files,
            'review_required': bool(changed_files),
            'limitation': 'Source changes require review; the scan cannot exclude ignored, generated, external or non-C-family code.'}


def inventory(sources: dict[str, str]) -> dict:
    candidates = []
    for path, source in sorted(sources.items()):
        for number, line in enumerate(source.splitlines(), 1):
            kinds = [name for name, pattern in COMPILED.items() if pattern.search(line)]
            if kinds:
                candidates.append({'path': path, 'line': number, 'kinds': kinds,
                                   'text': line.strip()[:240],
                                   'line_sha256': hashlib.sha256(line.strip().encode('utf-8')).hexdigest()})
    digest = hashlib.sha256()
    for path, source in sorted(sources.items()):
        digest.update(path.encode('utf-8', errors='surrogateescape') + b'\0')
        digest.update(source.encode('utf-8') + b'\0')
    reviewed = baseline.inspect_sources(sources)
    return {'scope': 'git tracked and non-ignored untracked C-family source; lexical candidates',
            'source_file_count': len(sources), 'source_sha256': digest.hexdigest(),
            'file_sha256': {path: hashlib.sha256(source.encode('utf-8')).hexdigest()
                            for path, source in sorted(sources.items())},
            'candidates': candidates, 'p734_reviewed_baseline': reviewed,
            'complete_call_graph_proven': False, 'all_possible_writers_excluded': False}


def report(sources: dict[str, str], capture: str | None) -> dict:
    source = inventory(sources)
    saved = audit.audit_saved_capture(capture, sources) if capture is not None else None
    ambiguities = []
    if capture is not None:
        for match in audit.NAMED_SAMPLE.finditer(capture):
            keys = [field.group('key') for field in audit.triplet_evidence.FIELD.finditer(match.group(3))]
            for key, count in sorted(Counter(keys).items()):
                if count > 1:
                    ambiguities.append({'kind': match.group(1), 'sample': int(match.group(2)),
                                        'field': key, 'occurrences': count})
    valid = (saved is not None and not ambiguities and
             saved['sample_integrity'] == 'consistent_observations')
    changed = saved['samples'][1]['changed_from_previous'] if valid else None
    return {
        'schema': 'navi33-p739-writer-provenance-v1', 'offline_only': True,
        'hardware_reads_or_writes': 0, 'source_inventory': source,
        'capture_audit': saved,
        'capture_sha256': hashlib.sha256(capture.encode('utf-8')).hexdigest() if capture is not None else None,
        'duplicate_sample_fields': ambiguities,
        'provenance': {
            'source_visible_potential_writers': 'candidates_only',
            'runtime_register_transition': ('observed_between_sequential_samples' if changed is True
                                            else 'no_difference_in_sequential_samples' if changed is False
                                            else 'unknown'),
            'attributed_host_write': 'unknown', 'firmware_originated_activity': 'unknown',
            'exclusive_mailbox_ownership': 'unknown', 'verified_command_response_handshake': 'unknown',
        },
        'limitations': ['Candidates include unrelated and commented code; manual review required.',
                        'Lexical scanning cannot resolve aliases, indirect calls, assembly or external code.',
                        'Samples are sequential; stable endpoints do not exclude intervening activity.',
                        'An IOService open snapshot does not revoke existing mappings or exclude firmware.'],
        'decision': {'may_issue_smu_command': False, 'reason': 'No independently verified arbitration or handshake'},
        'evidence_complete': bool(valid and source['p734_reviewed_baseline']['known_sites_match_reviewed_baseline']),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=ROOT)
    parser.add_argument('--input', type=Path, help='saved P7.36/P7.37 ioreg capture')
    parser.add_argument('--compare-ref', help='compare lexical candidates with tracked source at this Git revision')
    parser.add_argument('--output', type=Path, help='new JSON file; never overwritten')
    args = parser.parse_args()
    try:
        sources = load_sources(args.repo.expanduser())
        capture = args.input.expanduser().read_text(encoding='utf-8') if args.input else None
        result = report(sources, capture)
        if args.compare_ref:
            previous = inventory(load_revision_sources(args.repo.expanduser(), args.compare_ref))
            result['source_comparison'] = {'revision': args.compare_ref,
                'baseline_source_sha256': previous['source_sha256'],
                **candidate_delta(previous, result['source_inventory'])}
        else:
            result['source_comparison'] = None
        payload = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
        if args.output:
            with args.output.expanduser().open('x', encoding='utf-8') as out:
                out.write(payload)
        else:
            print(payload, end='')
        comparison = result['source_comparison']
        return 0 if (result['evidence_complete'] and
                     (comparison is None or not comparison['review_required'])) else 2
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f'P7.39: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
