#!/usr/bin/env python3
"""Replay frozen pre-v3 product source; no checkout or mutation of working source."""
import hashlib,json,os,pathlib,shutil,subprocess,tarfile,tempfile
root=pathlib.Path(__file__).resolve().parents[2];doc=root/'docs/repair/ws-project-v3'
with tempfile.TemporaryDirectory(prefix='ts-v3-baseline-') as name:
 dst=pathlib.Path(name)
 with tarfile.open(doc/'baseline.tar.gz') as t:t.extractall(dst,filter='data')
 for file,digest in json.loads((doc/'baseline.json').read_text())['sha256'].items():
  assert hashlib.sha256((dst/file).read_bytes()).hexdigest()==digest,file
 (dst/'components/ts_console/commands').mkdir(parents=True,exist_ok=True)
 shutil.copy2(doc/'baseline-console.c',dst/'components/ts_console/commands/ts_cmd_ssh.c')
 # Console header only; do not replace the frozen callback with today's code.
 (dst/'components/ts_console/include').symlink_to(root/'components/ts_console/include',target_is_directory=True)
 # Dependencies outside the frozen affected units are read-only current headers/vendor.
 for p in (root/'components').iterdir():
  if not (dst/'components'/p.name).exists():(dst/'components'/p.name).symlink_to(p,target_is_directory=p.is_dir())
 with tarfile.open(doc/'baseline-fixture.tar.gz') as t:t.extractall(dst,filter='data')
 p=subprocess.run(['node','tests/prompts/project-cross.cjs','--baseline'],cwd=dst,env=dict(os.environ,CROSS_OUTPUT=str(doc/'baseline-h1.json')),capture_output=True,text=True)
 (doc/'baseline-h1.txt').write_text(p.stdout+p.stderr);assert p.returncode==0 and 'CONFIRMED H1' in p.stdout,p.stdout+p.stderr
 # Same current lower-driver assertions against the frozen production, separate from H1.
 for file in ['test_shell_driver.c','run_shell_driver.sh','project_driver.inc']:
  shutil.copy2(root/'tests/ws_subscriptions'/file,dst/'tests/ws_subscriptions'/file)
 for args,label,marker in [([], 'no-data','ESP_ERR_TIMEOUT'),(['--eof-only'],'eof-release','!live_allocations'),(['--cli-exit'],'cli-exit','!ts_ssh_shell_is_active(shell)')]:
  p=subprocess.run(['bash','tests/ws_subscriptions/run_shell_driver.sh',*args],cwd=dst,env=dict(os.environ,PROJECT_BASELINE='1'),capture_output=True,text=True)
  evidence=p.stdout+p.stderr;(doc/('baseline-'+label+'.txt')).write_text(evidence)
  assert p.returncode!=0 and 'Assertion failed:' in evidence and marker in evidence,evidence
  print('CONFIRMED production defect:',label)
 p=subprocess.run(['bash','tests/ws_subscriptions/run_shell_driver.sh','--metrics'],cwd=dst,env=dict(os.environ,PROJECT_BASELINE='1'),capture_output=True,text=True)
 assert p.returncode==0,p.stderr
 (doc/'baseline-allocation-metrics.json').write_text(p.stdout)
 print('CONFIRMED H1 and lower-driver business counterexamples; source hashes verified')
