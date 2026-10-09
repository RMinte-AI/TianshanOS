#!/usr/bin/env python3
"""Read-only device sampling. Credentials and task submission stay in the WebUI."""
import argparse
import json
import time
from pathlib import Path
from urllib.request import urlopen

parser = argparse.ArgumentParser()
parser.add_argument('--base', default='http://10.10.99.97')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--seconds', type=float, default=180)
parser.add_argument('--runs', type=int, default=1)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)

def read(endpoint):
    started = time.monotonic()
    try:
        with urlopen(args.base.rstrip('/') + '/api/v1/' + endpoint, timeout=3) as response:
            result = json.load(response)
        return {'rtt_ms': round((time.monotonic() - started) * 1000, 3), 'response': result}
    except Exception as error:
        return {'rtt_ms': round((time.monotonic() - started) * 1000, 3), 'error': str(error)}

baseline = read('network/lpmu_access/status?diagnostics=1')
initial_run = baseline.get('response', {}).get('data', {}).get('run_id')
initial_running = baseline.get('response', {}).get('data', {}).get('running') is True
completed = set()
finish_at = None
started = time.monotonic()
with args.output.open('w') as log:
    log.write(json.dumps({'kind': 'baseline', 'status': baseline}, ensure_ascii=False) + '\n')
    while time.monotonic() - started < args.seconds:
        status = read('network/lpmu_access/status?diagnostics=1')
        memory = read('system/memory_detail')
        envelope = memory.get('response', {})
        raw = envelope.get('data', {})
        memory['response'] = {'code': envelope.get('code'), 'error': envelope.get('error'),
                              'data': {key: raw.get(key) for key in ('dram', 'psram')}}
        data = status.get('response', {}).get('data', {})
        run = data.get('run_id')
        sample = {'elapsed_s': round(time.monotonic() - started, 3), 'status': status, 'memory': memory}
        log.write(json.dumps(sample, ensure_ascii=False) + '\n'); log.flush()
        if run is not None and (run != initial_run or initial_running) and data.get('running') is False and data.get('stage') in ('success', 'failed'):
            if run not in completed:
                completed.add(run)
                print(json.dumps({'run_id': run, 'stage': data['stage'], 'error': data.get('last_error'), 'diagnostics': data.get('diagnostics')}, ensure_ascii=False), flush=True)
            if len(completed) >= args.runs and finish_at is None:
                finish_at = time.monotonic() + 5
        if finish_at is not None and time.monotonic() >= finish_at:
            break
        time.sleep(1)
print(json.dumps({'completed_runs': sorted(completed), 'samples': str(args.output)}, ensure_ascii=False))
