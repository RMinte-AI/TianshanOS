"""Real production pack create/verify/decrypt with SDK Mbed TLS 3.6.5 and disposable PKI."""
from pathlib import Path
import fcntl, os, subprocess, shutil, hashlib, sys, tempfile, time, re
root = Path(__file__).resolve().parents[2]
# Every run owns its scratch tree, so concurrent runs (another worktree, another
# agent) never share generated sources, PKI, config.h or the linked executable.
build = Path(tempfile.mkdtemp(prefix='tianshan-rule-pack-'))
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
mbed = idf/'components/mbedtls/mbedtls'
cmake = shutil.which('cmake') or '/Users/massif/.espressif/tools/cmake/3.30.2/CMake.app/Contents/bin/cmake'
env = {**os.environ, 'DEVELOPER_DIR': '/Library/Developer/CommandLineTools'}
def run(args, **kw):
    kw.setdefault('env',env)
    return subprocess.run([str(a) for a in args], check=True, **kw)

def mbedtls_version():
    header = mbed/'include/mbedtls/build_info.h'
    if not header.exists():
        raise RuntimeError(f'Mbed TLS build_info.h not found under {mbed}')
    for line in header.read_text().splitlines():
        if line.startswith('#define MBEDTLS_VERSION_STRING '):
            return line.split('"')[1]
    raise RuntimeError(f'MBEDTLS_VERSION_STRING not found in {header}')

MBEDTLS_ARGS = ['-DENABLE_PROGRAMS=OFF', '-DENABLE_TESTING=OFF']
# The cache directory is keyed by SDK path, Mbed TLS version and build arguments.
# A different identity therefore gets its own directory instead of deleting
# another run's cache, and a completed build is never removed. Concurrent first
# builds of the same identity are serialised by a lock on that directory.
identity = hashlib.sha256('|'.join([str(idf), mbedtls_version(), *MBEDTLS_ARGS]).encode()).hexdigest()[:16]
native = Path('/tmp/tianshan-pack-mbedtls-' + identity)
complete = native/'build-complete'
with Path(str(native) + '.lock').open('w') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    if not complete.exists():
        if native.exists():
            shutil.rmtree(native)
        run([cmake, '-S', mbed, '-B', native, *MBEDTLS_ARGS])
        run([cmake, '--build', native, '-j', '4'])
        complete.write_text(mbedtls_version())
openssl = shutil.which('openssl')
assert openssl
pki = build/'pki'
pki.mkdir(exist_ok=True)
def quiet(args):
    run([openssl, *args], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
ca = pki/'root.pem'
if not ca.exists():
    quiet(['req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:P-256','-nodes',
           '-keyout',pki/'root.key','-out',ca,'-days','3650','-subj','/CN=Rule pack test root',
           '-addext','basicConstraints=critical,CA:TRUE','-addext','keyUsage=critical,keyCertSign,cRLSign'])
for name, role in [('developer','Developer'),('recipient','Device'),('other','Device'),('not-developer','NotDeveloper'),('tls-developer','Developer'),('no-sign','Developer'),('p384','Developer'),('duplicate-role','Developer/OU=Developer')]:
    if not (pki/(name+'.pem')).exists():
        quiet(['req','-new','-newkey','ec','-pkeyopt','ec_paramgen_curve:'+('P-384' if name=='p384' else 'P-256'),'-nodes',
               '-keyout',pki/(name+'.key'),'-out',pki/(name+'.csr'),'-subj',f'/CN={name}/OU={role}'])
        (pki/'leaf.ext').write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,'+('keyAgreement' if name=='no-sign' else 'digitalSignature,keyAgreement')+'\n'+('extendedKeyUsage=clientAuth,serverAuth\n' if name=='tls-developer' else ''))
        quiet(['x509','-req','-in',pki/(name+'.csr'),'-CA',ca,'-CAkey',pki/'root.key',
               '-CAcreateserial','-out',pki/(name+'.pem'),'-days','3650','-extfile',pki/'leaf.ext'])
# Renewal keeps the same device key/subject but changes the certificate identity.
if not (pki/'developer-renewed.pem').exists():
    quiet(['x509','-req','-in',pki/'developer.csr','-CA',ca,'-CAkey',pki/'root.key',
           '-set_serial','20261103','-out',pki/'developer-renewed.pem','-days','3650','-extfile',pki/'leaf.ext'])
if not (pki/'alternate-root.pem').exists():
    quiet(['req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:P-256','-nodes',
           '-keyout',pki/'alternate-root.key','-out',pki/'alternate-root.pem','-days','3650','-subj','/CN=Unselected root',
           '-addext','basicConstraints=critical,CA:TRUE','-addext','keyUsage=critical,keyCertSign,cRLSign'])
    quiet(['req','-new','-newkey','ec','-pkeyopt','ec_paramgen_curve:P-256','-nodes',
           '-keyout',pki/'alternate-developer.key','-out',pki/'alternate-developer.csr','-subj','/CN=alternate/OU=Developer'])
    (pki/'alternate.ext').write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n')
    quiet(['x509','-req','-in',pki/'alternate-developer.csr','-CA',pki/'alternate-root.pem','-CAkey',pki/'alternate-root.key',
           '-CAcreateserial','-out',pki/'alternate-developer.pem','-days','3650','-extfile',pki/'alternate.ext'])
der = subprocess.check_output([openssl,'x509','-in',str(ca),'-outform','DER'])
(build/'config.h').write_text('#define TEST_TIME '+str(int(time.time())+2)+'LL\n#define CONFIG_TS_CONFIG_PACK_MAX_SIZE 65536\n#define CONFIG_TS_CONFIG_PACK_SIGNING_ROOT_SHA256 "'+('' if '--unconfigured' in sys.argv else hashlib.sha256(der).hexdigest())+'"\n')
# Keep the actual ordinary consumers; map only the /sdcard filesystem boundary.
(build/'paths.h').write_text(r'''#include <stdio.h>
#include <sys/stat.h>
FILE *fixture_fopen(const char *, const char *);
int fixture_stat(const char *, struct stat *);
int fixture_mkdir(const char *, mode_t);
#define fopen fixture_fopen
#define stat(path, out) fixture_stat(path, out)
#define mkdir fixture_mkdir
''')
pack_source=build/'current_pack.c'
pack_source.write_text('#include "paths.h"\n'+(root/'components/ts_config_pack/src/ts_config_pack.c').read_text())
api=(root/'components/ts_api/src/ts_api_config_pack.c').read_text()
m=re.search(r'^static esp_err_t api_config_pack_export_cert\([^;]*?\)\s*\{',api,re.M)
assert m
(build/'export_api.inc').write_text(api[m.start():api.index('\n}',m.end())+2]+'\n')
sources = [root/'tests/runtime/rule_pack/test_crypto.c', pack_source,
           root/'components/ts_config_pack/src/ts_config_pack_trust.c',root/'components/ts_security/src/ts_crypto.c',
           idf/'components/json/cJSON/cJSON.c']
if '--baseline' in sys.argv:
    old = build/'baseline_pack.c'
    old.write_bytes(subprocess.check_output(['git','show','bdc0354b:components/ts_config_pack/src/ts_config_pack.c'],cwd=root))
    sources[1] = old
    sources.remove(root/'components/ts_config_pack/src/ts_config_pack_trust.c')
cmd = ['cc','-std=c11','-D_DEFAULT_SOURCE','-g','-fsanitize=address,undefined','-Wno-deprecated-declarations',
       '-include',build/'config.h','-I'+str(build),'-I'+str(root/'components/ts_api/include'),'-I'+str(root/'tests/runtime/rule_pack/stubs'),
       '-I'+str(root/'components/ts_storage/include'),'-I'+str(root/'components/ts_config_pack/include'),'-I'+str(root/'components/ts_config_pack/src'),
       '-I'+str(root/'components/ts_cert/include'),'-I'+str(root/'components/ts_security/include'),
       '-I'+str(mbed/'include'),'-I'+str(idf/'components/json/cJSON')]
if '--baseline' in sys.argv: cmd += ['-DPACK_BASELINE']
if '--unconfigured' in sys.argv: cmd += ['-DPACK_UNCONFIGURED']
export_payloads=Path(sys.argv[sys.argv.index('--export-payloads')+1]).resolve() if '--export-payloads' in sys.argv else None
if export_payloads:
    assert export_payloads.is_file()
    cmd += ['-DPACK_EXPORT_PAYLOADS']
integration='--store' in sys.argv or '--engine' in sys.argv
if integration:
    case=next((arg.split('=',1)[1] for arg in sys.argv if arg.startswith('--case=')),None)
    if case not in (None,'view'): raise ValueError('The delayed-rule-store cases were retired; use the configuration owner suite.')
    run(['bash',root/'tests/runtime/run_config_storage.sh'],cwd=root)
executable=build/('export-payloads' if export_payloads else 'crypto-baseline' if '--baseline' in sys.argv else 'unconfigured' if '--unconfigured' in sys.argv else 'engine' if '--engine' in sys.argv else 'store' if '--store' in sys.argv else 'crypto')
cmd += sources+[native/'library/libmbedx509.a',native/'library/libmbedcrypto.a','-lpthread','-lm','-o',executable]
run(cmd,cwd=root)
case=None if integration else next((a.split('=',1)[1] for a in sys.argv if a.startswith('--case=')),None)
run([executable,pki,*([export_payloads] if export_payloads else [case] if case else [])],cwd=root,env={**env,'ASAN_OPTIONS':'detect_leaks=0'} if '--store' in sys.argv else env)
# Scratch tree is disposable; it is kept only when a step above raised.
shutil.rmtree(build, ignore_errors=True)
