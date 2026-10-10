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
       '-I'+str(root/'components/ts_config_pack/include'),'-I'+str(root/'components/ts_config_pack/src'),
       '-I'+str(root/'components/ts_cert/include'),'-I'+str(root/'components/ts_security/include'),
       '-I'+str(mbed/'include'),'-I'+str(idf/'components/json/cJSON')]
if '--baseline' in sys.argv: cmd += ['-DPACK_BASELINE']
if '--unconfigured' in sys.argv: cmd += ['-DPACK_UNCONFIGURED']
if '--store' in sys.argv or '--engine' in sys.argv:
    sources[0]=root/'tests/runtime/rule_pack/test_store_set.c'
    sources += [root/'components/ts_automation/src/ts_rule_codec.c',root/'components/ts_automation/src/ts_action_filter.c',root/'components/ts_automation/src/ts_rule_pack.c']
    cmd += ['-I'+str(root/'components/ts_automation/include'),'-I'+str(root/'components/ts_automation/src')]
if '--engine' in sys.argv:
    import re
    src=(root/'components/ts_automation/src/ts_rule_engine.c').read_text()
    a=src.index('typedef struct {\n    ts_auto_rule_t *rules;');b=src.index('static void payload_free',a)
    names=['find_rule_index','payload_free','payload_adopt','ts_rule_resolve_presentation',
           'ts_rule_acquire','ts_rule_release','same_config','protected_bindings','classify_rule_state','state_pending','pending_id','rule_union_count_locked','rebuild_rule_states',
           'rule_commit_impl','ts_rule_commit','ts_rule_get_by_index','ts_rule_restart_pending',
           'ts_rule_saved_status','ts_rule_pending_list','pack_reason','ts_rule_import_pack',
           'ts_rule_edit_begin','ts_rule_edit_end','ts_rule_count','ts_rule_config_status','ts_rule_load_error',
           'dependency_change_rule','ts_rule_dependency_change','load_rules','ts_rules_load',
           'ts_rules_load_from_file','ts_rule_engine_deinit','ts_rule_refresh_saved','ts_rule_deferred_load_task',
           'compare_values','ts_rule_eval_condition','ts_rule_eval_condition_group','execute_rule','ts_rule_trigger']
    parts=[src[a:b]]
    masked=re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',lambda m:' '*len(m.group()),src)
    for name in names:
        match=re.search(r'^(?:static )?(?:esp_err_t|void|bool|int|cJSON|const char|rule_payload_t|rule_state_t)[ \t]+\**'+name+r'\([^;]*?\)\s*\{',src,re.M)
        assert match,name
        opening=src.index('{',match.start());depth=0
        for end in range(opening,len(masked)):
            depth += (masked[end]=='{')-(masked[end]=='}')
            if not depth:break
        parts.append(src[match.start():end+1])
    api=(root/'components/ts_api/src/ts_api_automation.c').read_text()
    for name in ['api_automation_rules_list','rule_commit_reply','rule_mutate_locked','rule_mutate','api_automation_rules_enable']:
        match=re.search(r'^static (?:esp_err_t|void) '+name+r'\([^;]*?\)\s*\{',api,re.M);assert match,name
        parts.append(api[match.start():api.index('\n}',match.start())+2])
    (build/'pack_engine.inc').write_text('\n'.join(parts))
    manager=(root/'components/ts_automation/src/ts_action_manager.c').read_text()
    a=manager.index('typedef struct {\n    atomic_uint refs;');b=manager.index('} action_binding_t;',a)+len('} action_binding_t;')
    snapshots=[manager[a:b]]
    for name in ['ts_action_register_ssh_host','ts_action_unregister_ssh_host','ts_action_get_ssh_host_ex','ts_action_templates_load','ts_action_snapshot_owner','ts_action_snapshot_retain','ts_action_snapshot_release','snapshot_command','ts_action_snapshot']:
        m=re.search(r'^(?:static )?(?:void|esp_err_t) '+name+r'\([^;]*?\)\s*\{',manager,re.M);assert m,name
        snapshots.append(manager[m.start():manager.index('\n}',m.start())+2])
    (build/'pack_snapshot.inc').write_text('\n'.join(snapshots))
    sources[0]=root/'tests/runtime/rule_pack/test_engine_pack.c'
    cmd += ['-I'+str(build),'-DCONFIG_TS_AUTOMATION_MAX_RULES=4']
executable=build/('crypto-baseline' if '--baseline' in sys.argv else 'unconfigured' if '--unconfigured' in sys.argv else 'engine' if '--engine' in sys.argv else 'store' if '--store' in sys.argv else 'crypto')
cmd += sources+[native/'library/libmbedx509.a',native/'library/libmbedcrypto.a','-lpthread','-o',executable]
run(cmd,cwd=root)
case=next((a.split('=',1)[1] for a in sys.argv if a.startswith('--case=')),None)
run([executable,pki,*([case] if case else [])],cwd=root,env={**env,'ASAN_OPTIONS':'detect_leaks=0'} if '--store' in sys.argv else env)
