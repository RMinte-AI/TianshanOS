"""Attach exact DOM-id render observations; never infer action/all-path PASS."""
import collections
import hashlib
import json
import re
from pathlib import Path

root = Path('output/macos27-20260929-native-rework')
inventory_path = root / 'entry-coverage.json'
inventory = json.loads(inventory_path.read_text())
entries = inventory['entries']
by_id = collections.defaultdict(list)
for entry in entries:
    match = re.search(r'(?:^|\s)id="([^"$]+)"', entry['baseline']['markup'])
    if match:
        by_id[match[1]].append(entry)
observations = []
ambiguous = []
for procedure in ['network', 'commands', 'files', 'nested-surfaces']:
    source = root / 'built-v3' / (procedure + '.json')
    identity = json.loads(source.with_name(procedure + '.identity.json').read_text())
    assert identity['stable'] and identity['before'] == identity['after']
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    for row_index, row in enumerate(json.loads(source.read_text())):
        for field in row.get('fields', row.get('inputs', [])):
            if not field.get('id') or field.get('visible') is False:
                continue
            candidates = by_id.get(field['id'], [])
            if len(candidates) != 1:
                ambiguous.append({'procedure': procedure, 'id': field['id'], 'candidates': len(candidates)})
                continue
            entry = candidates[0]
            observation = {'entryId': entry['id'], 'domId': field['id'], 'kind': 'LAYOUT_FIELD_OBSERVED',
                           'procedure': procedure, 'state': row['name'], 'language': row.get('lang', 'zh-CN'),
                           'width': row['innerWidth'], 'rowIndex': row_index, 'artifact': str(source),
                           'artifactSha256': digest, 'buildIdentity': identity['after'],
                           'scope': 'Field has a layout box in recorded UI state; may be outside current scroll viewport. Not validation, submit, or all-condition PASS'}
            observations.append(observation)
            existing = entry.setdefault('renderEvidence', [])
            if observation not in existing:
                existing.append(observation)
inventory_path.write_text(json.dumps(inventory, ensure_ascii=False, indent=2) + '\n')
report = {'scope': 'Exact unique DOM id to recorded field layout only. Runtime status intentionally unchanged.',
          'observedEntryCount': len({x['entryId'] for x in observations}), 'observationCount': len(observations),
          'ambiguousOrUnmapped': ambiguous, 'observations': observations}
(root / 'built-v3/field-entry-mapping.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print({k: v for k, v in report.items() if k not in ['observations', 'ambiguousOrUnmapped']})
