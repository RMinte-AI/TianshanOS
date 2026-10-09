#!/usr/bin/env python3
"""Compare deterministic traces and separately report uncalibrated closed-loop results."""
import argparse
import collections
import csv
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('before', type=Path)
parser.add_argument('after', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()


def read(path):
    cases = collections.defaultdict(list)
    for row in csv.DictReader(path.open()):
        cases[row['scenario']].append({k: v if k == 'scenario' else float(v) for k, v in row.items()})
    return cases


before, after = read(args.before), read(args.after)
assert before.keys() == after.keys()
checks = []
for name in ['sustained_warming', 'small_surplus', 'limited_output', 'partial_stale']:
    assert before[name] == after[name], name
    checks.append(name + ': identical full trace')
# First rise has identical samples, history, gain, demand, request and successful PWM.
assert before['short_load'][:51] == after['short_load'][:51]
checks.append('short_load: first heating response identical through t=50s')
for name in ['video_like_87_to_14', 'video_like_82_to_14', 'video_like_69_to_13']:
    assert all(x['demand_pct'] == y['demand_pct'] for x, y in zip(before[name], after[name]))
    assert after[name][60]['applied_pct'] <= after[name][60]['demand_pct'] + 10
    assert before[name][60]['applied_pct'] > before[name][60]['demand_pct'] + 30
    assert all(y['applied_pct'] <= x['applied_pct'] for x, y in zip(before[name], after[name]))
    checks.append(name + ': same demand, excess decays, never undershoots demand')
for name in ['hard_guard', 'stale_and_recovery']:
    field = 'guard' if name == 'hard_guard' else 'stale'
    assert [x[field] for x in before[name]] == [x[field] for x in after[name]]
    assert all(x['applied_pct'] == y['applied_pct'] for x, y in zip(before[name], after[name]) if x[field])
    checks.append(name + ': guard/stale activation and forced output unchanged')
# Every new heating step, including recurrent bursts, retains production rise permission.
for name, rows in after.items():
    for previous, row in zip(rows, rows[1:]):
        if name == 'mode_switch' or row['stale'] or row['guard'] or row['hal_result']:
            continue
        if row['demand_pct'] > previous['applied_pct']:
            assert row['request_pct'] >= min(row['demand_pct'], previous['applied_pct'] + 12), (name, row)
checks.append('all fixtures and plants: 12 percentage points/s rise retained whenever demand rises')


def metrics(rows):
    direction = 0
    reversals = 0
    changes = 0
    for a, b in zip(rows, rows[1:]):
        delta = b['applied_pct'] - a['applied_pct']
        if delta:
            changes += 1
            sign = 1 if delta > 0 else -1
            if direction and direction != sign:
                reversals += 1
            direction = sign
    return {'max_temp_c': max(r['temp_c'] for r in rows),
            'mean_pwm_pct': round(sum(r['applied_pct'] for r in rows) / len(rows), 2),
            'output_changes': changes, 'direction_reversals': reversals,
            'guard_samples': sum(r['guard'] for r in rows)}


plants = {name: {'before': metrics(before[name]), 'after': metrics(after[name])}
          for name in before if name.startswith('plant_')}
for name in plants:
    if name.endswith('noise'):
        assert before[name] == after[name], name
checks.append('three noise plants: identical trace, no added noise-induced output changes')
summary = {'deterministic_checks': checks,
           'controller_trace_rows_per_version': sum(map(len, before.values())),
           'scenarios_per_version': len(before), 'closed_loop_cases': plants,
           'thermal_effect_acceptance': 'NOT established: plant parameters are assumed, no hardware validation',
           'settle': {name: {
               version: next(r['second'] for r in rows if r['applied_pct'] <= r['demand_pct'] + 10)
               for version, rows in [('before', before[name]), ('after', after[name])]}
               for name in ['video_like_87_to_14', 'video_like_82_to_14', 'video_like_69_to_13']}}
args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
print('PASS', len(checks), 'cross-version deterministic checks;', len(plants), 'offline plant comparisons reported separately')
print(json.dumps(summary['settle']))
