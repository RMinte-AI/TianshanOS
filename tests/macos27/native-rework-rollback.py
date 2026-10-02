"""Prepare current rework-only rollback and rehearse exclusively in a scratch copy."""
import datetime
import argparse
import difflib
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parents[2]
root = repo / 'output/macos27-20260929-native-rework'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', required=True, choices=['build-v4', 'build-v5', 'build-v6', 'build-v14', 'build-v15', 'build-v16', 'build-v17', 'build-v18', 'build-v19'])
args = parser.parse_args()
out = root / ('diff-audit-' + args.build.removeprefix('build-'))
if out.exists():
    parser.error('Evidence output already exists: ' + str(out))
files = ['components/ts_webui/web/index.html', 'components/ts_webui/web/css/style.css',
         'components/ts_webui/web/js/app.js', 'tools/minify_web.py']
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
baseline = json.loads((root / 'baseline-identity.json').read_text())
manifest = json.loads((root / args.build / 'manifest.json').read_text())
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip() == baseline['head']
assert subprocess.check_output(['git', 'branch', '--show-current'], cwd=repo, text=True).strip() == baseline['branch']
protected = []
for name, expected in baseline['tracked_hashes'].items():
    if name in files:
        continue
    assert sha(repo / name) == expected, name
    protected.append(name)
for name, expected in baseline['references'].items():
    assert sha(Path(name)) == expected, name
identities = {}
reverse = []
forward = []
for name in files:
    original = root / 'baseline' / name
    current = repo / name
    identities[name] = {'current': sha(current), 'baseline': sha(original)}
    if name.startswith('components/ts_webui/web/'):
        assert sha(current) == manifest['sourceIdentity'][name.removeprefix('components/ts_webui/web/')]
    a, b = original.read_text().splitlines(True), current.read_text().splitlines(True)
    forward.extend(difflib.unified_diff(a, b, fromfile='a/' + name, tofile='b/' + name))
    reverse.extend(difflib.unified_diff(b, a, fromfile='a/' + name, tofile='b/' + name))
assert identities['tools/minify_web.py']['current'] == identities['tools/minify_web.py']['baseline']
out.mkdir(exist_ok=False)
patch = out / 'rollback-rework-only.patch'
patch.write_text(''.join(reverse))
(out / 'rework-to-current.patch').write_text(''.join(forward))
(out / 'original-to-current.patch').write_bytes(subprocess.check_output(['git', 'diff', baseline['head'], '--', *files], cwd=repo))
commands = []
with tempfile.TemporaryDirectory(prefix='macos27-rework-rollback-') as temp:
    scratch = Path(temp)
    for name in files:
        target = scratch / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / name, target)
    sentinel = scratch / 'unrelated-sentinel.txt'
    sentinel.write_text('preserve unrelated work\n')
    for args in [['git', 'apply', '--check', str(patch)], ['git', 'apply', str(patch)]]:
        p = subprocess.run(args, cwd=scratch, text=True, capture_output=True)
        commands.append({'argv': args, 'exitCode': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr})
        assert p.returncode == 0, p.stderr
    restored = {name: sha(scratch / name) for name in files}
    assert all(restored[name] == identities[name]['baseline'] for name in files)
    assert sentinel.read_text() == 'preserve unrelated work\n'
assert all(sha(repo / name) == identities[name]['current'] for name in files)
result = {'at': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'status': 'PASS',
          'scope': 'Rollback rehearsal only; full review and product acceptance separate. Real checkout unchanged.',
          'buildHash': manifest['imageHash'], 'identities': identities, 'commands': commands,
          'restored': restored, 'unrelatedSentinelPreserved': True,
          'protectedTrackedCount': len(protected), 'frozenReferenceCount': len(baseline['references']),
          'patches': {p.name: sha(p) for p in out.glob('*.patch')}}
(out / 'rollback-rehearsal.json').write_text(json.dumps(result, indent=2) + '\n')
print({k: result[k] for k in ['status', 'buildHash', 'protectedTrackedCount', 'frozenReferenceCount']})
