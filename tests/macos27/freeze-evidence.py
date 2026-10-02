"""Freeze review scope and hashes after all reports and running tests finish."""
import hashlib,json,subprocess,datetime
from pathlib import Path
base=Path('output/macos27-20260928');sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
production=[Path('components/ts_webui/web')/p for p in ['css/style.css','index.html','js/app.js']]
tools=sorted(p for p in Path('tests/macos27').glob('*')if p.is_file());docs=sorted(p for p in Path('docs/macos27').glob('*')if p.is_file()and p.name!='GOAL-PROMPT.md')
scopes=[{'path':str(p),'sha256':sha(p),'review':'Production diff: every hunk manually reviewed, AST/negative controls and protected hashes verified'}for p in production]
scopes += [{'path':str(p),'sha256':sha(p),'review':'Entire tool source read; synthetic/local scope, result boundaries and path behavior checked'}for p in tools]
scopes += [{'path':str(p),'sha256':sha(p),'review':'Generated inventories checked row-by-row programmatically against frozen source; runtime scope explicit'if p.suffix=='.json'else'Full document reviewed for evidence, status and authorization consistency'}for p in docs]
(base/'review-scope.json').write_text(json.dumps({'reviewer':'Current agent, separate final review phase; not independent human/second-agent signoff','files':scopes,'preservedUserInputs':['docs/macos27/GOAL-PROMPT.md','docs/macos27/reference/','output/ui-exploration-20260928/','docs/runtime-repair/evidence/reviewer-fixes/diff-check.txt','tmp/'],'generatedEvidence':'Images/binaries/raw traces reviewed via actual views, invariants, summaries and SHA manifest, not fictitious textual line-by-line review.'},ensure_ascii=False,indent=2))
# Full source/document addition diff, excluding generated binary/raw browser evidence.
with (base/'support-files.patch').open('wb')as stream:
 for p in tools+docs:
  r=subprocess.run(['git','diff','--no-index','--','/dev/null',str(p)],capture_output=True);assert r.returncode in [0,1];stream.write(r.stdout)
paths=production+tools+docs+[p for p in (base/'after/web_optimized').rglob('*')if p.is_file()]+[base/'after/www.bin',base/'visual.patch',base/'rollback-visual.patch',base/'support-files.patch',base/'review-scope.json',base/'command-results.json',base/'regression-summary.json',base/'performance/final-v3/raw.json',base/'performance/summary.json',base/'screenshot-catalog.json',base/'screenshots.html']
manifest={'frozenAt':datetime.datetime.now(datetime.timezone.utc).isoformat(),'files':{str(p):sha(p)for p in paths}}
(base/'final-hashes.json').write_text(json.dumps(manifest,indent=2));print(len(paths),'final source/report/artifact hashes')
