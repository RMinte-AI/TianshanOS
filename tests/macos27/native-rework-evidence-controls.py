"""Exercise report gates using isolated copies and deliberate invalid evidence."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from PIL import Image

repo = Path(__file__).resolve().parents[2]
root = repo / 'output/macos27-20260929-native-rework'
out = Path(sys.argv[1]).resolve()
assert out.is_relative_to(root) and not out.exists()
out.mkdir(parents=True)
performance = json.loads((root / 'performance-v2/raw.json').read_text())
results = []


def run_case(name, script, raw, output_name, expect_success, expected_text=None, images=False):
    with tempfile.TemporaryDirectory(prefix='macos27-evidence-control-') as tmp:
        directory = Path(tmp)
        if images:
            for filename in ['sample.png', 'sample-background.png']:
                Image.new('RGB', (12, 12), 'white').save(directory / filename)
            for row in raw['rows']:
                row['screenshots'] = {
                    key: hashlib.sha256((directory / filename).read_bytes()).hexdigest()
                    for key, filename in [('normal', 'sample.png'), ('background', 'sample-background.png')]
                }
        (directory / 'raw.json').write_text(json.dumps(raw))
        process = subprocess.run([sys.executable, str(repo / 'tests/macos27' / script), str(directory)],
                                 text=True, capture_output=True)
        target = directory / output_name
        if expect_success:
            assert process.returncode == 0 and target.exists(), (name, process.stderr)
            value = json.loads(target.read_text())
            if script == 'native-rework-performance-summary.py':
                assert value['overallStatus'] == 'INCONCLUSIVE'
                assert all(v['collectionRequirementsMet'] for v in value['labels'].values())
            else:
                assert value['summary'] == {'PASS': 1, 'FAIL': 1, 'INCONCLUSIVE': 0}
            (out / (name + '.json')).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
        else:
            assert process.returncode != 0 and not target.exists(), name
            assert expected_text in process.stderr, (name, process.stderr)
        results.append({'name': name, 'expectedSuccess': expect_success,
                        'exitCode': process.returncode, 'stdout': process.stdout, 'stderr': process.stderr,
                        'outputCreated': target.exists(), 'expectationMet': True})


perf_script = 'native-rework-performance-summary.py'
run_case('historical-v2-valid', perf_script, performance, 'summary.json', True)
for name in ['missing-kind', 'duplicate-cycles', 'duplicate-cold', 'raw-errors',
             'observer-errors', 'short-observation', 'invalid-time']:
    raw = copy.deepcopy(performance)
    if name == 'missing-kind':
        raw['interactions'] = [r for r in raw['interactions'] if r.get('route') != '/network']
        message = 'missing required interaction kinds'
    elif name == 'duplicate-cycles':
        for row in raw['interactions']:
            row['cycle'] = 0
        message = 'fifty distinct cycles required'
    elif name == 'duplicate-cold':
        for row in raw['cold']:
            row['iteration'] = 0
        message = 'ten distinct error-free cold trials required'
    elif name == 'raw-errors':
        raw['errors'] = ['deliberate test error']
        message = 'Error-free raw run required'
    elif name == 'observer-errors':
        raw['residency']['A']['samples'][-1]['observation']['errors'] = ['deliberate observer error']
        message = 'error-free residency observer required'
    elif name == 'short-observation':
        raw['residency']['A']['samples'][-1]['at'] = raw['residency']['A']['start'] + 1000
        message = 'thirty minutes of observed residency required'
    else:
        raw['interactions'][0]['ms'] = -1
        message = 'invalid interaction timing'
    run_case(name, perf_script, raw, 'summary.json', False, message)

text_sample = {'stable': True, 'disabled': False, 'opacity': 1, 'unsupported': [],
               'color': 'rgb(0, 0, 0)', 'fontSize': 14, 'fontWeight': '400',
               'rects': [{'x': 1, 'y': 1, 'width': 10, 'height': 10}]}
low_contrast = {**text_sample, 'color': 'rgb(230, 230, 230)'}
contrast = {'completed': True, 'configurationUnchanged': True, 'errors': [],
            'rows': [{'id': 'sample', 'stable': True, 'viewport': {'dpr': 1, 'width': 12, 'height': 12},
                      'text': [text_sample, low_contrast]}]}
contrast_script = 'native-rework-composed-text.py'
run_case('contrast-known-ratios', contrast_script, copy.deepcopy(contrast), 'analysis.json', True, images=True)
for name in ['contrast-errors', 'contrast-empty']:
    raw = copy.deepcopy(contrast)
    if name == 'contrast-errors':
        raw['errors'] = ['deliberate rendering error']
    else:
        raw['rows'] = []
    run_case(name, contrast_script, raw, 'analysis.json', False,
             'Error-free nonempty collection required', images=True)

report = {'scope': 'Report gate controls only; synthetic images and historical build-v2 performance input, not new product acceptance.',
          'historicalRawSha256': hashlib.sha256((root / 'performance-v2/raw.json').read_bytes()).hexdigest(),
          'scripts': {name: hashlib.sha256((repo / 'tests/macos27' / name).read_bytes()).hexdigest()
                      for name in [perf_script, contrast_script, Path(__file__).name]},
          'cases': results, 'allExpectationsMet': all(r['expectationMet'] for r in results)}
(out / 'controls.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({'cases': len(results), 'allExpectationsMet': report['allExpectationsMet']}))
