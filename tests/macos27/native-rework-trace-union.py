"""Summarize per-thread unions; diagnostic CPU intervals, not GPU or wall time."""
import collections
import hashlib
import json
import statistics
import sys
from pathlib import Path

root = Path(sys.argv[1])
out = root / 'union-summary.json'
if out.exists():
    raise SystemExit('Preserve previous summary')
raw_path = root / 'raw.json'
raw = json.loads(raw_path.read_text())
assert raw['completed'] and raw['configurationUnchanged'] and not raw['errors']


def interval_union(intervals):
    end = float('-inf')
    duration = 0
    for start, stop in sorted(intervals):
        assert stop >= start
        duration += max(0, stop - max(start, end))
        end = max(end, stop)
    return duration


assert interval_union([(0, 10), (2, 5), (8, 15), (20, 22)]) == 17
assert interval_union([]) == 0
samples = []
for row in raw['rows']:
    p = Path(row['trace']['path'])
    assert hashlib.sha256(p.read_bytes()).hexdigest() == row['trace']['sha256']
    events = json.loads(p.read_text())['traceEvents']
    for metric in ['Paint', 'PrePaint', 'Layerize', 'RasterTask']:
        selected = [e for e in events if e['name'] == metric and e['ph'] == 'X']
        threads = collections.defaultdict(list)
        for e in selected:
            threads[(e['pid'], e['tid'])].append((e['ts'], e['ts'] + e.get('dur', 0)))
        samples.append({'surface': row['surface'], 'label': row['label'], 'trial': row['trial'],
                        'metric': metric, 'count': len(selected),
                        'nestedSumMs': sum(e.get('dur', 0) for e in selected) / 1000,
                        'threadUnionSumMs': sum(interval_union(v) for v in threads.values()) / 1000})
summary = []
for surface, label, metric in sorted({(s['surface'], s['label'], s['metric']) for s in samples}):
    rows = [s for s in samples if (s['surface'], s['label'], s['metric']) == (surface, label, metric)]
    summary.append({'surface': surface, 'label': label, 'metric': metric, 'trials': len(rows),
                    'medianUnionMs': statistics.median(s['threadUnionSumMs'] for s in rows),
                    'medianNestedSumMs': statistics.median(s['nestedSumMs'] for s in rows)})
result = {'status': 'DIAGNOSTIC_ONLY', 'rawSha256': hashlib.sha256(raw_path.read_bytes()).hexdigest(),
          'scope': 'Union removes same-thread overlap within each metric. Threads and different metrics can still overlap. No zero-cost or hardware acceptance claim.',
          'summary': summary, 'samples': samples}
out.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(summary))
