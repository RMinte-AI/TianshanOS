from pathlib import Path
import hashlib,json,os,shutil,subprocess,tarfile,tempfile
root=Path(__file__).resolve().parents[2]; proof=root/'docs/repair/ws-control'
for f,h in json.loads((proof/'baseline-evidence.json').read_text()).items():
 assert hashlib.sha256((proof/f).read_bytes()).hexdigest()==h,f
with tempfile.TemporaryDirectory(prefix='ts-control-baseline-') as tmp:
 base=Path(tmp)
 with tarfile.open(proof/'baseline.tar.gz') as t:t.extractall(base)
 for f,h in json.loads((proof/'baseline.json').read_text())['sha256'].items():
  assert hashlib.sha256((base/f).read_bytes()).hexdigest()==h,f
 for path in (root/'components').iterdir():
  if not (base/'components'/path.name).exists():(base/'components'/path.name).symlink_to(path,target_is_directory=True)
 for path in (root/'components/ts_security/include').iterdir():
  dest=base/'components/ts_security/include'/path.name
  if not dest.exists():dest.symlink_to(path)
 for src,dst in [('baseline-adapter.c','test_operation_adapter.c'),('baseline-control.c','test_control.c')]:shutil.copyfile(proof/src,base/'tests/ws_subscriptions'/dst)
 shutil.copyfile(root/'tests/ws_subscriptions/run_control.sh',base/'tests/ws_subscriptions/run_control.sh')
 for case in ['v1','v2']:
  r=subprocess.run(['bash','tests/ws_subscriptions/run_control.sh',case],cwd=base,capture_output=True,text=True)
  print(r.stdout,end='');assert r.returncode==1 and case+' business_contract=FAIL' in r.stdout and not r.stderr,r.stderr
 print('CONFIRMED: both failures are production business assertions; complete frozen source hashes verified')
