"""Final check commands with actual return codes and captured logs."""
import subprocess,json,datetime,ast
from pathlib import Path
base=Path('output/macos27-20260928');commands=[]
def run(args,name):
 start=datetime.datetime.now(datetime.timezone.utc).isoformat();r=subprocess.run(args,text=True,capture_output=True);p=base/'checks'/f'{name}.log';p.parent.mkdir(exist_ok=True);p.write_text(r.stdout+r.stderr);commands.append({'command':args,'started':start,'exitCode':r.returncode,'log':str(p)});(base/'command-results.json').write_text(json.dumps(commands,indent=2));assert r.returncode==0,(args,r.returncode,r.stderr)
run(['node','tests/macos27/verify-contract.cjs',str(base/'baseline/web'),'components/ts_webui/web'],'contract')
for path in sorted(Path('tests/macos27').glob('*')):
 if path.suffix in ['.js','.cjs']:run(['node','--check',str(path)],'syntax-'+path.name)
 if path.suffix=='.py':ast.parse(path.read_text(),filename=str(path))
for p in ['components/ts_webui/web/js/app.js',str(base/'after/web_optimized/js/app.js')]:run(['node','--check',p],'syntax-'+('source'if p.startswith('components')else'optimized')+'-app')
for p in ['result-audit','build-evidence','final-integrity']:run(['python3','tests/macos27/'+p+'.py'],p)
run(['node','tests/macos27/map-runtime.cjs'],'entry-coverage')
print('PASS',len(commands),'commands with actual exit codes; Python tools AST parsed')
