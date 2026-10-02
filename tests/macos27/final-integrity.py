"""Read-only scope verification and rollback proof in a disposable copy."""
import hashlib,json,subprocess,tempfile,shutil
from pathlib import Path
base=Path('output/macos27-20260928'); allowed=['components/ts_webui/web/css/style.css','components/ts_webui/web/index.html','components/ts_webui/web/js/app.js']
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
frozen=json.loads((base/'baseline/tracked-hashes.json').read_text())
changed=[p for p,h in frozen.items() if sha(p)!=h]
assert sorted(changed)==sorted(allowed),changed
identity={'status':'PASS','trackedFiles':len(frozen),'changed':changed,'otherTrackedFilesUnchanged':len(frozen)-len(changed),'head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'branch':subprocess.check_output(['git','branch','--show-current'],text=True).strip()}
assert identity['head']=='ac109b00dbbba5654c611b746b7ef7228b05987c' and identity['branch']=='feat/macos27-webui'
(base/'protected-final.json').write_text(json.dumps(identity,indent=2))
for name,args in [('visual.patch',[]),('rollback-visual.patch',['-R'])]:
 (base/name).write_bytes(subprocess.check_output(['git','diff','--binary',*args,'--',*allowed]))
subprocess.run(['git','diff','--check'],check=True)
subprocess.run(['git','apply','--check',str(base/'rollback-visual.patch')],check=True)
with tempfile.TemporaryDirectory(prefix='macos27-rollback-') as temp:
 for p in allowed:
  dst=Path(temp)/p;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dst)
 subprocess.run(['git','apply',str((base/'rollback-visual.patch').resolve())],cwd=temp,check=True)
 restored={p:sha(Path(temp)/p)==frozen[p] for p in allowed};assert all(restored.values())
 (base/'rollback-proof.json').write_text(json.dumps({'status':'PASS','method':'git apply --check in actual checkout; actual reverse apply ONLY in disposable copy, then compare baseline SHA-256','restored':restored},indent=2))
manifest=json.loads(Path('docs/macos27/reference/manifest.json').read_text());assert all(sha(p)==h for p,h in manifest.items())
config=json.loads((base/'final-configuration.json').read_text());assert all(sha(p)==h for p,h in config['files'].items())
print(json.dumps({**identity,'referenceHashes':'PASS','finalConfiguration':'PASS','rollback':'PASS'}))
