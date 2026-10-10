"""Read-only preflight for an already eligible release job; never writes GitHub."""
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_release import validate


def query(path):
    request = urllib.request.Request('https://api.github.com/' + path, headers={
        'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
        'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise RuntimeError(f'GitHub query failed: HTTP {error.code} for {path}') from error
    except urllib.error.URLError as error:
        raise RuntimeError(f'GitHub query failed for {path}: {error.reason}') from error


def prepare_release(repository, mode, tag, build_commit, get=query):
    release = get(f'repos/{repository}/releases/tags/{urllib.parse.quote(tag, safe="")}')
    if mode == 'main' and release is not None:
        return True, '该版本已存在，本次未发布新资产'
    if mode == 'published' and release is None:
        raise ValueError('The published Release no longer exists')
    ref = get(f'repos/{repository}/git/ref/tags/{urllib.parse.quote(tag, safe="")}')
    if ref is None:
        if mode != 'main':
            raise ValueError('The release event tag does not exist')
    else:
        obj = ref['object']
        while obj['type'] == 'tag':
            annotated = get(f'repos/{repository}/git/tags/{obj["sha"]}')
            if annotated is None:
                raise ValueError('Cannot resolve annotated release tag')
            obj = annotated['object']
        if obj['type'] != 'commit' or obj['sha'] != build_commit:
            raise ValueError('Release tag differs from the actual build commit; will not move the tag')
    return False, 'Release preflight passed for this build'


def main():
    event_name = os.environ['GITHUB_EVENT_NAME']
    ref = os.environ['GITHUB_REF']
    version = os.environ['BUILD_VERSION']
    if event_name == 'release':
        mode = 'published'
        tag = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())['release']['tag_name']
    elif event_name == 'push' and ref.startswith('refs/tags/'):
        mode, tag = 'tag', ref.removeprefix('refs/tags/')
    elif event_name == 'push' and ref == 'refs/heads/main':
        mode, tag = 'main', 'v' + Path('version.txt').read_text().strip()
    else:
        raise ValueError('Preflight invoked without a release route')
    validate(tag, version)
    commit = os.environ['BUILD_COMMIT']
    if subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() != commit:
        raise ValueError('Release checkout is not the build commit')
    skip, message = prepare_release(os.environ['GITHUB_REPOSITORY'], mode, tag, commit)
    with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
        output.write(f'tag={tag}\nskip_release={str(skip).lower()}\n')
    print(message)


if __name__ == '__main__':
    main()
