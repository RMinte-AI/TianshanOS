#!/usr/bin/env python3
"""Local suite entry; no device access. Preserve every subprocess exit status."""
import json, os, pathlib, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[2]
out=pathlib.Path(os.environ.get('V3_RESULTS',str(root/'docs/repair/ws-project-v3/results')));out.mkdir(parents=True,exist_ok=True)
commands=[('host',['bash','tests/ws_subscriptions/run_host.sh']),('operation',['bash','tests/ws_subscriptions/run_operation.sh']),('adapter',['bash','tests/ws_subscriptions/run_operation_adapter.sh']),('control',['bash','tests/ws_subscriptions/run_control.sh']),('reviewer',['bash','tests/ws_subscriptions/run_reviewer.sh']),('f1_f2',['bash','tests/ws_subscriptions/run_f1_f2.sh']),('runtime',['bash','tests/runtime/run_all.sh']),('certificate',['bash','tests/certificate/run_host.sh']),('prompts',['npm','test','--prefix','tests/prompts']),('release',['python3','tests/release/test_release.py']),('shell_driver',['bash','tests/ws_subscriptions/run_shell_driver.sh'])]
if '--browser' in sys.argv: commands=[('browser',['npm','run','test:browser','--prefix','tests/prompts'])]
results=[]
for name,cmd in commands:
 with (out/(name+'.log')).open('w') as f:r=subprocess.run(cmd,cwd=root,stdout=f,stderr=subprocess.STDOUT)
 results.append(dict(name=name,command=cmd,exit=r.returncode));print(name,r.returncode,flush=True)
for language in ['en-US','zh-CN']:
 name=('cross-browser-' if '--browser' in sys.argv else 'cross-node-')+language
 env=dict(os.environ,CROSS_LANGUAGE=language,CROSS_OUTPUT=str(out/(name+'.json')))
 cmd=['node','tests/prompts/project-cross.cjs']+(['--browser'] if '--browser' in sys.argv else [])
 with (out/(name+'.log')).open('w') as f:r=subprocess.run(cmd,cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT)
 results.append(dict(name=name,command=cmd,exit=r.returncode));print(name,r.returncode,flush=True)
(out/('browser.json' if '--browser' in sys.argv else 'host.json')).write_text(json.dumps(results,indent=2))
sys.exit(any(r['exit'] for r in results))
