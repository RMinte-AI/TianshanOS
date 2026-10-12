"""The required status accepts only the checks selected by Check scope."""
import json
import os

REQUIRED = ('web-tests', 'runtime-tests', 'build', 'web-artifacts')


def boolean(value):
    if value not in ('true', 'false'):
        raise ValueError(f'Invalid boolean output: {value!r}')
    return value == 'true'


def check_gate(needs, cancelled):
    if cancelled:
        raise ValueError('The workflow is cancelled at the final decision')
    changes = needs.get('changes', {})
    if changes.get('result') != 'success':
        raise ValueError('Check scope did not succeed')
    outputs = changes.get('outputs', {})
    full = boolean(outputs.get('run_full'))
    publish = boolean(outputs.get('publish_release'))
    if publish and not full:
        raise ValueError('A release cannot use the lightweight path')
    expected = 'success' if full else 'skipped'
    for job in REQUIRED:
        actual = needs.get(job, {}).get('result')
        if actual != expected:
            raise ValueError(f'{job}: expected {expected}, received {actual}')
    return 'Full checks passed' if full else 'Document checks passed; heavy checks intentionally skipped'


def main():
    try:
        print(check_gate(json.loads(os.environ['NEEDS_JSON']), boolean(os.environ['RUN_CANCELLED'])))
    except (ValueError, KeyError) as error:
        raise SystemExit(f'CI Gate failed: {error}')


if __name__ == '__main__':
    main()
