"""Freeze completed local evidence identities without treating hashes as acceptance."""
import hashlib,json,subprocess
from pathlib import Path
root=Path('output/macos27-20260929-card-revision')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
raw=json.loads((root/'performance/raw.json').read_text())
assert raw.get('completed') and raw.get('configurationUnchanged'), 'Final performance run has not completed against unchanged artifacts'
assert not raw['errors']
for label in ['A','B']:
 assert raw['residency'][label]['durationMs']>=1800000
 assert len(raw['residency'][label]['cycles'])>=50
source=json.loads((root/'source-integrity.json').read_text())
assert all(sha(Path(p))==h for p,h in source['files'].items())
reviewPaths=sorted(p for base in ['tests/macos27','docs/macos27'] for p in Path(base).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
review={'scope':'Reviewed current local tools, docs, inventory and frozen references; identity is not a PASS claim. Historical tools remain identifiable and are not rerun as current report generators.','files':{str(p):sha(p) for p in reviewPaths}}
(root/'review-source-scope.json').write_text(json.dumps(review,ensure_ascii=False,indent=2)+'\n')
files=sorted(p for p in root.rglob('*') if p.is_file() and p.name!='final-manifest.json' and not any(part.startswith('performance-superseded') or part=='rejected' for part in p.parts))
manifest={'status':'LOCAL_EVIDENCE_FROZEN_RELEASE_NOT_ACCEPTED','head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'branch':subprocess.check_output(['git','branch','--show-current'],text=True).strip(),'source':source['files'],'builtImage':{'path':str(root/'after/www.bin'),'sha256':sha(root/'after/www.bin')},'reviewScope':sha(root/'review-source-scope.json'),'evidence':{str(p):{'bytes':p.stat().st_size,'sha256':sha(p)} for p in files},'excluded':'Rejected and superseded evidence retained separately; historical PASS is not current acceptance.'}
(root/'final-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
print('Frozen',len(files),'evidence files;',len(reviewPaths),'tools/docs/reference files; image',manifest['builtImage']['sha256'])
