"""Replay frozen production translation units; only add scheduling barriers."""
from pathlib import Path
import hashlib, json, os, shutil, subprocess, tarfile, tempfile
root = Path(__file__).resolve().parents[2]
os.chdir(root)
sdk = os.environ.get('IDF_PATH')
if not sdk:
    raise SystemExit('NOT RUN: set IDF_PATH to the project SDK')
for name,want in json.loads(Path('docs/repair/ws-operation/public-headers.json').read_text()).items():
    assert hashlib.sha256(Path(name).read_bytes()).hexdigest()==want,name
with tempfile.TemporaryDirectory(prefix='ts-operation-baseline-') as tmp:
    base = Path(tmp)
    with tarfile.open('docs/repair/ws-operation/baseline.tar.gz') as archive:
        archive.extractall(base)
    for name, want in json.loads(Path('docs/repair/ws-operation/baseline.json').read_text())['sha256'].items():
        assert hashlib.sha256((base/name).read_bytes()).hexdigest() == want, name
    print('Frozen production source and original tests SHA256 verified', flush=True)
    source = base/'components/ts_webui/src/ts_webui_ws.c'
    code = source.read_text()
    for old, new in [
        ('atomic_fetch_sub(&s_ssh_output_pending,1);', 'atomic_fetch_sub(&s_ssh_output_pending,1); TS_WS_TEST_POINT("G1");'),
        ('ts_ws_message_t *m=json?ts_ws_message_text(json,strlen(json)):NULL;', 'TS_WS_TEST_POINT("G2");\n    ts_ws_message_t *m=json?ts_ws_message_text(json,strlen(json)):NULL;')]:
        assert code.count(old) == 1
        code = code.replace(old, new)
    source.write_text(code)
    fixture = base/'tests/ws_subscriptions/baseline_operation_adapter.c'
    shutil.copy(root/'tests/ws_subscriptions/baseline_operation_adapter.c', fixture)
    shutil.copy(root/'tests/ws_subscriptions/stubs/esp_console.h', base/'tests/ws_subscriptions/stubs/esp_console.h')
    shutil.copy(root/'tests/ws_subscriptions/stubs/freertos/timers.h', base/'tests/ws_subscriptions/stubs/freertos/timers.h')
    # Public business headers are unchanged from HEAD; compile against their real declarations.
    includes = [base/'tests/ws_subscriptions/stubs', base/'components/ts_webui/include', base/'components/ts_webui/src']
    includes += [root/'components'/p/'include' for p in ('ts_security','ts_automation','ts_drivers','ts_console','ts_net')]
    cjson = Path(sdk)/'components/json/cJSON'
    cmd = [os.environ.get('CC','cc'), '-std=gnu11','-g','-Wno-deprecated-declarations','-Wno-macro-redefined','-fsanitize=address,undefined']
    cmd += ['-I'+str(p) for p in includes+[cjson]]
    cmd += [str(fixture), str(cjson/'cJSON.c'), '-o', str(base/'baseline')]
    subprocess.run(cmd, check=True)
    for case, expected in [('g1','replacement_corrupted=1'),('g2','terminal_before_late_output=1')]:
        run = subprocess.run([str(base/'baseline'),case], capture_output=True, text=True)
        print(run.stdout, end='', flush=True)
        assert run.returncode == 1 and expected in run.stdout and not run.stderr, run.stderr
    print('CONFIRMED G1/G2 are business assertion failures in the complete frozen WS unit, not compile/dependency/cleanup failures')
