#!/usr/bin/env python3
"""Actual C shell builder + /bin/sh/grep, synthetic files only."""
import ctypes, pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[2]
out=pathlib.Path('/tmp/tianshan-runtime-tests/probe.dylib')
subprocess.run(['/Library/Developer/CommandLineTools/usr/bin/cc','-isysroot','/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk','-shared','-fPIC','-I'+str(root/'components/ts_security/include'),str(root/'components/ts_security/src/ts_ssh_probe.c'),'-o',str(out)],check=True)
lib=ctypes.CDLL(str(out)); lib.ts_ssh_log_probe_command.restype=ctypes.c_bool;lib.ts_ssh_probe_token.restype=ctypes.c_bool
with tempfile.TemporaryDirectory(prefix='ts-probe-') as temp:
 p=pathlib.Path(temp)/"a ' quoted file"; marker=pathlib.Path(temp)/'injected'
 ready=f"-ready ' $(touch {marker})"; fail="fail ' --.*"
 def run(content,capacity=3072):
  p.write_text(content);buf=ctypes.create_string_buffer(capacity)
  assert lib.ts_ssh_log_probe_command(str(p).encode(),ready.encode(),fail.encode(),buf,capacity)
  result=subprocess.run(['/bin/sh','-c',buf.value.decode()],capture_output=True,text=True)
  assert not marker.exists();return result.returncode,result.stdout.strip()
 assert run(ready)==(0,'READY');assert run(ready+'\n'+fail)==(0,'FAIL');assert run('not matched')==(0,'WAITING')
 b=ctypes.create_string_buffer(32);assert not lib.ts_ssh_log_probe_command(str(p).encode(),ready.encode(),fail.encode(),b,32)
 for value in [b'NOTREADY',b'prefix READY\n',b'READY garbage',b'READY\nREADY\n']:
  assert not lib.ts_ssh_probe_token(value,b'READY')
 assert lib.ts_ssh_probe_token(b'READY\r\n',b'READY')
print('PASS actual probe builder: quotes, spaces, leading dash, literal shell metacharacters, fail priority, exact tokens, overflow')
