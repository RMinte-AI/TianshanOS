"""Verify local revision scope and prove a source-only inverse patch in a copy."""
import hashlib,json,subprocess,tempfile,shutil
from pathlib import Path
root=Path('output/macos27-20260929-card-revision')
visual=['components/ts_webui/web/css/style.css','components/ts_webui/web/index.html','components/ts_webui/web/js/app.js']
allowed=visual+['tools/minify_web.py']
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
frozen=json.loads(Path('output/macos27-20260928/baseline/tracked-hashes.json').read_text())
changed=[p for p,h in frozen.items() if sha(p)!=h]
assert sorted(changed)==sorted(allowed),changed
identity={'status':'PASS','head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'branch':subprocess.check_output(['git','branch','--show-current'],text=True).strip(),'changed':changed,'otherTrackedFilesUnchanged':len(frozen)-len(changed),'files':{p:sha(p) for p in allowed},'scope':'Source revision identity; does not claim final build or performance acceptance.'}
assert identity['head']=='ac109b00dbbba5654c611b746b7ef7228b05987c'
assert identity['branch']=='feat/macos27-webui'
for name,args,paths in [('visual.patch',[],visual),('rollback-visual.patch',['-R'],visual),('build-fix.patch',[],['tools/minify_web.py']),('rollback-build-fix.patch',['-R'],['tools/minify_web.py']),('rollback-all.patch',['-R'],allowed)]:
    (root/name).write_bytes(subprocess.check_output(['git','diff','--binary',*args,'--',*paths]))
subprocess.run(['git','diff','--check'],check=True)
subprocess.run(['git','apply','--check',str(root/'rollback-all.patch')],check=True)
with tempfile.TemporaryDirectory(prefix='macos27-revision-rollback-') as tmp:
    for p in allowed:
        dst=Path(tmp)/p;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,dst)
    subprocess.run(['git','apply',str((root/'rollback-all.patch').resolve())],cwd=tmp,check=True)
    restored={p:sha(Path(tmp)/p)==frozen[p] for p in allowed}
    assert all(restored.values())
identity['rollback']={'status':'PASS','actualApply':'disposable copy only','restored':restored}
manifest=json.loads(Path('docs/macos27/reference/manifest.json').read_text())
assert all(sha(p)==h for p,h in manifest.items())
identity['approvedReferenceHashes']='PASS'
identity['buildFixAuthorization']='User explicitly approved the proposed two-line minifier correction on 2026-09-29.'
(root/'source-integrity.json').write_text(json.dumps(identity,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(identity,ensure_ascii=False,indent=2))
