#!/usr/bin/env python3
"""Actual copyid builder, local disposable files, no SSH connections."""
import ctypes, pathlib, shlex, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='ts-copyid-') as tmp:
    directory = pathlib.Path(tmp)
    library = directory/'copyid.dylib'
    subprocess.run(['cc','-shared','-fPIC','-I'+str(root/'components/ts_security/include'),str(root/'components/ts_security/src/ts_ssh_probe.c'),'-o',str(library)],check=True)
    lib = ctypes.CDLL(str(library)); lib.ts_ssh_copyid_command.restype = ctypes.c_bool
    target = directory/'.ssh'; marker = directory/'injected'
    comments = ['ordinary', "reviewer's key", r'back\slash', 'double " quote', '-n', f"x'; touch {marker}; echo '", f'$(touch {marker})', '&quot; literal', '中文备注']
    expected = b''
    for comment in comments:
        data = ('ssh-rsa SYNTHETIC_PUBLIC_DATA '+comment+'\n').encode()
        buf = ctypes.create_string_buffer(4*len(data)+256)
        assert lib.ts_ssh_copyid_command(data,buf,len(buf))
        command = buf.value.decode()
        # Redirect only the fixed target directory, never change public-key quoting.
        command = command.replace('~/.ssh',shlex.quote(str(target)))
        subprocess.run(['/bin/sh','-c',command],check=True,capture_output=True)
        expected += data
        assert (target/'authorized_keys').read_bytes() == expected
        assert not marker.exists()
    tiny = ctypes.create_string_buffer(16)
    assert not lib.ts_ssh_copyid_command(b'key',tiny,len(tiny))
    assert not lib.ts_ssh_copyid_command(b'',tiny,len(tiny))
    assert (target/'authorized_keys').stat().st_mode & 0o777 == 0o600
for file in ['components/ts_api/src/ts_api_ssh.c','components/ts_console/commands/ts_cmd_ssh.c']:
    text=(root/file).read_text(); assert 'ts_ssh_copyid_command(pubkey_data' in text
    assert "echo '%s' >> ~/.ssh/authorized_keys" not in text
print('PASS copyid: API and CLI share actual builder; exact bytes, no command injection, bounded capacity, permissions')
