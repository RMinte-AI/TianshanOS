"""Select required checks and release eligibility from the actual Actions event."""
import json
import os
from pathlib import Path
import re
import subprocess

UPSTREAM = 'RMinte-AI/TianshanOS'
TAG = re.compile(r'v\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?')


def changed_paths(base, head):
    raw = subprocess.check_output(['git', 'diff', '--name-status', '-z', '-M', '--merge-base', base, head, '--'])
    fields = raw.decode().split('\0')
    paths = []
    at = 0
    while at < len(fields) - 1:
        status = fields[at]
        count = 2 if status.startswith(('R', 'C')) else 1
        if at + count >= len(fields) - 1:
            raise ValueError('Incomplete git diff record')
        paths.extend(fields[at + 1:at + count + 1])
        at += count + 1
    return paths


def ordinary_document(path):
    return path in ('README.md', 'README_EN.md') or (path.startswith('docs/') and path.endswith('.md'))


def classify(repository, event_name, ref, event, paths=None):
    if not repository or not ref:
        raise ValueError('Missing repository or ref')
    if event_name not in ('pull_request', 'push', 'release', 'workflow_dispatch'):
        raise ValueError('Unsupported Actions event')
    if event_name == 'pull_request' and paths is None:
        raise ValueError('PR paths were not obtained')
    light = event_name == 'pull_request' and bool(paths) and all(ordinary_document(p) for p in paths)
    tag_push = event_name == 'push' and ref.startswith('refs/tags/') and TAG.fullmatch(ref.removeprefix('refs/tags/')) is not None
    eligible = repository == UPSTREAM and (
        (event_name == 'push' and ref == 'refs/heads/main') or tag_push or
        (event_name == 'release' and event.get('action') == 'published'))
    return {'run_full': not light, 'publish_release': eligible}


def main():
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
    name = os.environ['GITHUB_EVENT_NAME']
    paths = None
    if name == 'pull_request':
        pr = event['pull_request']
        paths = changed_paths(pr['base']['sha'], pr['head']['sha'])
    result = classify(os.environ['GITHUB_REPOSITORY'], name, os.environ['GITHUB_REF'], event, paths)
    with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
        for key, value in result.items():
            output.write(f'{key}={str(value).lower()}\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
