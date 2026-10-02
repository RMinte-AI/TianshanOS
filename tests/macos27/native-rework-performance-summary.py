"""Derive statistics from a completed run. Do not infer a regression allowance."""
import hashlib
import json
import math
import statistics
import sys
from pathlib import Path


def stats(values):
    values = sorted(values)
    if not values:
        return None
    return {'n': len(values), 'median': statistics.median(values),
            'p95': values[math.ceil(.95 * len(values)) - 1], 'min': values[0], 'max': values[-1]}


directory = Path(sys.argv[1])
raw_path = directory / 'raw.json'
raw = json.loads(raw_path.read_text())
if not raw.get('completed') or not raw.get('configurationUnchanged'):
    raise SystemExit('Completed identity-stable raw run required')
if raw.get('errors') != []:
    raise SystemExit('Error-free raw run required')
required_interactions = {'/network', '/files', '/automation', '/', 'modal'}
for label in ['A', 'B']:
    cold = [r for r in raw['cold'] if r['label'] == label]
    interactions = [r for r in raw['interactions'] if r['label'] == label]
    resident = raw['residency'][label]
    kinds = {r.get('route', r['type']) for r in interactions}
    if not required_interactions.issubset(kinds):
        raise SystemExit(f'{label}: missing required interaction kinds')
    if len({r['iteration'] for r in cold}) < 10 or any(r['errors'] for r in cold):
        raise SystemExit(f'{label}: ten distinct error-free cold trials required')
    for kind in required_interactions:
        samples = [r for r in interactions if r.get('route', r['type']) == kind]
        if len({r['cycle'] for r in samples}) < 50:
            raise SystemExit(f'{label}: fifty distinct cycles required for {kind}')
    if len({r['cycle'] for r in resident['cycles']}) < 50:
        raise SystemExit(f'{label}: fifty residency cycles required')
    if (resident['durationMs'] < 1800000 or not resident['samples']
            or resident['samples'][-1]['at'] - resident['start'] < 1800000):
        raise SystemExit(f'{label}: thirty minutes of observed residency required')
    if resident['samples'][-1].get('observation', {}).get('errors') != []:
        raise SystemExit(f'{label}: error-free residency observer required')
    if any(not math.isfinite(r['readyMs']) or r['readyMs'] < 0 for r in cold):
        raise SystemExit(f'{label}: invalid cold timing')
    if any(not math.isfinite(r['ms']) or r['ms'] < 0 for r in interactions):
        raise SystemExit(f'{label}: invalid interaction timing')
target = directory / 'summary.json'
if target.exists():
    raise SystemExit('Preserve existing summary')
output = {'rawSha256': hashlib.sha256(raw_path.read_bytes()).hexdigest(),
          'environment': raw['environment'], 'labels': {}, 'errors': raw['errors'],
          'scope': 'Driver wall-clock navigation/interaction, headless local synthetic backend; simultaneous A/B residency. No physical-device or GPU acceptance.',
          'instrumentationLimit': 'Identical observation harness retains request/sample arrays and renders a diagnostic pre every 30 seconds. Heap includes this instrumentation; absolute growth alone is not a product leak diagnosis. Readiness is the primary LED control, not all asynchronous widgets.',
          'overallStatus': 'INCONCLUSIVE',
          'remaining': ['Separate scroll/paint/compositing measurement', 'Interpret measured differences without an invented tolerance', 'Cross-reference final transfer/capacity evidence']}
for label in ['A', 'B']:
    cold = [r for r in raw['cold'] if r['label'] == label]
    interactions = [r for r in raw['interactions'] if r['label'] == label]
    resident = raw['residency'][label]
    cycles = resident['cycles']
    steady = [s for s in resident['samples'] if s['at'] >= cycles[-1]['at'] + 30000]
    last = resident['samples'][-1]
    assert steady, 'No post-cycle residency samples'
    summary = {'coldReadyMs': stats([r['readyMs'] for r in cold]),
               'coldErrors': [e for r in cold for e in r['errors']],
               'interactionMs': {}, 'cycles': len(cycles), 'durationMs': resident['durationMs'],
               'steadySamples': len(steady), 'steadyStartMs': steady[0]['at'] - resident['start'],
               'steadyDOM': stats([s['dom'] for s in steady]),
               'steadyHeapBytes': stats([s['heap'] for s in steady if s.get('heap') is not None]),
               'steadyCounters': {key: stats([s['counters'][key] for s in steady]) for key in last['counters']},
               'longTaskDurationMs': stats([t['duration'] for t in last['longs']]),
               'longTaskCount': len(last['longs'])}
    for kind in sorted(set(r.get('route', r['type']) for r in interactions)):
        summary['interactionMs'][kind] = stats([r['ms'] for r in interactions if r.get('route', r['type']) == kind])
    observation = last.get('observation') or {}
    summary['observationKeys'] = list(observation)
    samples = observation.get('samples', [])
    if samples:
        summary['socketsFinal'] = samples[-1].get('sockets')
    requests = observation.get('requests', [])
    summary['requestCount'] = len(requests)
    summary['collectionRequirementsMet'] = (len(cold) >= 10 and len(cycles) >= 50 and resident['durationMs'] >= 1800000
        and all(s['n'] >= 50 for s in summary['interactionMs'].values()) and not summary['coldErrors'])
    output['labels'][label] = summary
output['medianDeltaBminusA'] = {'coldReadyMs': output['labels']['B']['coldReadyMs']['median'] - output['labels']['A']['coldReadyMs']['median'],
    'interactionMs': {key: output['labels']['B']['interactionMs'][key]['median'] - value['median']
                      for key, value in output['labels']['A']['interactionMs'].items()}}
target.write_text(json.dumps(output, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({k: v for k, v in output.items() if k not in ['environment', 'labels']}, ensure_ascii=False))
