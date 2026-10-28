"""Real production pack create/verify/decrypt with SDK Mbed TLS 3.6.5 and disposable PKI."""
from pathlib import Path
import os, subprocess, shutil, hashlib, sys, time
root = Path(__file__).resolve().parents[2]
build = Path('/tmp/tianshan-rule-pack-tests')
build.mkdir(exist_ok=True)
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
mbed = idf/'components/mbedtls/mbedtls'
cmake = shutil.which('cmake') or '/Users/massif/.espressif/tools/cmake/3.30.2/CMake.app/Contents/bin/cmake'
native = Path('/tmp/tianshan-pack-mbedtls')
env = {**os.environ, 'DEVELOPER_DIR': '/Library/Developer/CommandLineTools'}
def run(args, **kw):
    kw.setdefault('env',env)
    return subprocess.run([str(a) for a in args], check=True, **kw)
if not (native/'library/libmbedcrypto.a').exists():
    run([cmake, '-S', mbed, '-B', native, '-DENABLE_PROGRAMS=OFF', '-DENABLE_TESTING=OFF'])
    run([cmake, '--build', native, '-j', '4'])
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
sources = [root/'tests/runtime/rule_pack/test_crypto.c', root/'components/ts_config_pack/src/ts_config_pack.c',
           root/'components/ts_config_pack/src/ts_config_pack_trust.c',root/'components/ts_security/src/ts_crypto.c',
           idf/'components/json/cJSON/cJSON.c']
if '--baseline' in sys.argv:
    old = build/'baseline_pack.c'
    old.write_bytes(subprocess.check_output(['git','show','bdc0354b:components/ts_config_pack/src/ts_config_pack.c'],cwd=root))
    sources[1] = old
    sources.remove(root/'components/ts_config_pack/src/ts_config_pack_trust.c')
cmd = ['cc','-std=c11','-D_DEFAULT_SOURCE','-g','-fsanitize=address,undefined','-Wno-deprecated-declarations',
       '-include',build/'config.h','-I'+str(root/'tests/runtime/rule_pack/stubs'),
       '-I'+str(root/'components/ts_storage/include'),'-I'+str(root/'components/ts_config_pack/include'),'-I'+str(root/'components/ts_config_pack/src'),
       '-I'+str(root/'components/ts_cert/include'),'-I'+str(root/'components/ts_security/include'),
       '-I'+str(mbed/'include'),'-I'+str(idf/'components/json/cJSON')]
if '--baseline' in sys.argv: cmd += ['-DPACK_BASELINE']
if '--unconfigured' in sys.argv: cmd += ['-DPACK_UNCONFIGURED']
integration='--store' in sys.argv or '--engine' in sys.argv
if integration:
    case=next((arg.split('=',1)[1] for arg in sys.argv if arg.startswith('--case=')),None)
    if case not in (None,'view'): raise ValueError('The delayed-rule-store cases were retired; use the configuration owner suite.')
    run(['bash',root/'tests/runtime/run_config_storage.sh'],cwd=root)
executable=build/('crypto-baseline' if '--baseline' in sys.argv else 'unconfigured' if '--unconfigured' in sys.argv else 'engine' if '--engine' in sys.argv else 'store' if '--store' in sys.argv else 'crypto')
cmd += sources+[native/'library/libmbedx509.a',native/'library/libmbedcrypto.a','-lpthread','-lm','-o',executable]
run(cmd,cwd=root)
case=None if integration else next((a.split('=',1)[1] for a in sys.argv if a.startswith('--case=')),None)
run([executable,pki,*([case] if case else [])],cwd=root,env={**env,'ASAN_OPTIONS':'detect_leaks=0'} if '--store' in sys.argv else env)
