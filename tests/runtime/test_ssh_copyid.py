"""Execute production ssh.copyid with synthetic SSH and registration failures."""
from pathlib import Path
import os
import re
import subprocess

build = Path('/tmp/tianshan-ssh-host-tests')
build.mkdir(exist_ok=True)
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))

def extract(path, name):
    source = Path(path).read_text()
    match = re.search(r'^(?:static )?[^\n;]+\b' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.start()) + 2] + '\n'

code = r'''
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "ts_api.h"
#include "ts_ssh_client.h"
#include "ts_ssh_hosts_config.h"
#include "ts_known_hosts.h"
#define TS_MALLOC_PSRAM malloc
#define TS_STRDUP_PSRAM strdup
#define TS_LOGI(...) ((void)0)
#define TS_LOGW(...) ((void)0)
static const char *error_name(esp_err_t e) {
    switch(e) {
    case ESP_OK:return "ESP_OK";
    case ESP_ERR_INVALID_STATE:return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NO_MEM:return "ESP_ERR_NO_MEM";
    default:return "ESP_FAIL";
    }
}
#define esp_err_to_name error_name
static esp_err_t registration_error;
static bool fail_deploy, fail_verify;
static int registrations, deployments, sessions, verifications;
static ts_host_verify_result_t host_status = TS_HOST_VERIFY_OK;
static esp_err_t host_save_error;
static bool fail_password;
static int host_saves, password_auths;
#define TS_API_ERR_HOST_NEW 1002
#define TS_API_ERR_HOST_MISMATCH 1001
static ts_ssh_host_config_t registered_config;
struct ts_ssh_session_s {ts_ssh_config_t config;};
typedef struct {char *data;size_t len;} ssh_request_key_t;
esp_err_t ts_keystore_load_public_key(const char *id,char **data,size_t *len) {
    assert(!strcmp(id,"synthetic-key"));*data=strdup("ssh-ed25519 SYNTHETIC test");*len=strlen(*data);return ESP_OK;
}
esp_err_t ts_keystore_load_private_key(const char *id,char **data,size_t *len) {
    *data=strdup("SYNTHETIC PRIVATE KEY");*len=strlen(*data);return ESP_OK;
}
esp_err_t ts_ssh_session_create(const ts_ssh_config_t *config,ts_ssh_session_t *out) {
    *out=malloc(sizeof(**out));(*out)->config=*config;++sessions;return ESP_OK;
}
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t s) {free(s);--sessions;return ESP_OK;}
esp_err_t ts_ssh_disconnect(ts_ssh_session_t s) {return ESP_OK;}
esp_err_t ts_ssh_connect(ts_ssh_session_t s) {
    assert(s->config.auth_method==TS_SSH_AUTH_PUBLICKEY);++verifications;
    return fail_verify?ESP_FAIL:ESP_OK;
}
const char *ts_ssh_get_error(ts_ssh_session_t s) {return "synthetic transport failure";}
esp_err_t ts_known_hosts_verify(ts_ssh_session_t s,ts_host_verify_result_t *status,ts_known_host_t *info) {
    *status=host_status;
    if(info) {
        strcpy(info->host,s->config.host);info->port=s->config.port;
        strcpy(info->fingerprint,"observed-fingerprint");
    }
    return ESP_OK;
}
esp_err_t ts_known_hosts_add(ts_ssh_session_t s) {
    ++host_saves;if(host_save_error)return host_save_error;
    host_status=TS_HOST_VERIFY_OK;return ESP_OK;
}
esp_err_t ts_known_hosts_get(const char *host,uint16_t port,ts_known_host_t *info) {
    strcpy(info->fingerprint,"stored-fingerprint");return ESP_OK;
}
esp_err_t ts_ssh_connect_with_verifier(ts_ssh_session_t s,ts_ssh_verify_cb_t verify,void *context) {
    assert(s->config.auth_method==TS_SSH_AUTH_PASSWORD);
    esp_err_t ret=verify(s,context);if(ret!=ESP_OK)return ret;
    ++password_auths;return fail_password?ESP_FAIL:ESP_OK;
}
esp_err_t ts_ssh_exec(ts_ssh_session_t s,const char *cmd,ts_ssh_exec_result_t *r) {
    assert(strstr(cmd,"authorized_keys") && strstr(cmd,"SYNTHETIC"));++deployments;
    r->exit_code=fail_deploy?1:0;r->stderr_data=fail_deploy?strdup("synthetic remote failure"):NULL;return ESP_OK;
}
void ts_ssh_exec_result_free(ts_ssh_exec_result_t *r) {free(r->stderr_data);}
esp_err_t ts_ssh_hosts_config_add(const ts_ssh_host_config_t *cfg) {
    ++registrations;registered_config=*cfg;return registration_error;
}
'''
for name in ['ts_api_result_free', 'ts_api_result_ok', 'ts_api_result_error']:
    code += extract('components/ts_api/src/ts_api.c', name)
code += extract('components/ts_api/src/ts_api_ssh.c', 'verify_host_fingerprint')
source = Path('components/ts_api/src/ts_api_ssh.c').read_text()
start = source.index('typedef struct {\n    const cJSON *params;')
code += source[start:source.index('} verify_context_t;', start) + len('} verify_context_t;')] + '\n'
for name in ['verify_before_auth', 'connect_verified', 'cleanup_key_buffer', 'api_ssh_copyid']:
    code += extract('components/ts_api/src/ts_api_ssh.c', name)
# Exercise the actual HTTP success/partial-result serialization branch as well.
web_source = Path('components/ts_webui/src/ts_webui_api.c').read_text()
start = web_source.index('    if (ret == ESP_OK || result.code == TS_API_OK) {')
end = web_source.index(' else if (ret == ESP_ERR_NOT_FOUND', start)
code += r"""
static char *http_body;
static esp_err_t ts_http_send_json(void *req,int status,const char *json) {
    assert(status==200);free(http_body);http_body=strdup(json);return ESP_OK;
}
static esp_err_t ts_http_send_error(void *req,int status,const char *msg) {return ESP_FAIL;}
static esp_err_t send_result(esp_err_t ret,ts_api_result_t result) {
    void *req=NULL;
""" + web_source[start:end] + '\nreturn ESP_FAIL;\n}\n'
code += r'''
static cJSON *item(ts_api_result_t *r,const char *key) {return cJSON_GetObjectItem(r->data,key);}
static cJSON *copyid_params(void) {
    return cJSON_Parse("{\"host\":\"192.0.2.8\",\"user\":\"test\",\"password\":\"synthetic\",\"keyid\":\"synthetic-key\"}");
}
static void first_use_case(ts_host_verify_result_t status,int trust,esp_err_t save_error,bool bad_password,int expected) {
    host_status=status;host_save_error=save_error;fail_password=bad_password;
    host_saves=password_auths=deployments=registrations=verifications=0;
    registration_error=ESP_OK;fail_deploy=fail_verify=false;
    cJSON *p=copyid_params();
    if(trust>=0)cJSON_AddBoolToObject(p,"trust_new",trust);
    ts_api_result_t r={0};esp_err_t ret=api_ssh_copyid(p,&r);
    assert(r.code==expected && sessions==0);
    bool permitted=status==TS_HOST_VERIFY_OK || (status==TS_HOST_VERIFY_NOT_FOUND && trust!=0);
    assert(host_saves==(status==TS_HOST_VERIFY_NOT_FOUND && trust!=0?1:0));
    assert(password_auths==(permitted && !save_error?1:0));
    if(expected==TS_API_OK) {
        assert(ret==ESP_OK && deployments==1 && registrations==1 && verifications==1);
        assert(host_status==TS_HOST_VERIFY_OK && cJSON_IsTrue(item(&r,"deployed")));
    } else {
        assert(ret!=ESP_OK && deployments==0 && registrations==0 && verifications==0);
        if(expected==TS_API_ERR_HOST_MISMATCH) {
            assert(!strcmp(item(&r,"current_fingerprint")->valuestring,"observed-fingerprint"));
            assert(!strcmp(item(&r,"stored_fingerprint")->valuestring,"stored-fingerprint"));
        }
    }
    ts_api_result_free(&r);cJSON_Delete(p);
    host_save_error=ESP_OK;fail_password=false;
}
static void other_operation_trust(void) {
    ts_ssh_config_t config=TS_SSH_DEFAULT_CONFIG();config.host="192.0.2.8";config.port=22;
    ts_ssh_session_t s;assert(ts_ssh_session_create(&config,&s)==ESP_OK);
    cJSON *p=cJSON_Parse("{\"trust_new\":true}");ts_api_result_t r={0};
    host_status=TS_HOST_VERIFY_NOT_FOUND;host_saves=password_auths=0;
    assert(connect_verified(s,p,&r,false)!=ESP_OK && r.code==TS_API_ERR_HOST_NEW);
    assert(!password_auths && !host_saves);ts_api_result_free(&r);r.code=TS_API_OK;
    cJSON_AddStringToObject(p,"confirmed_fingerprint","observed-fingerprint");
    assert(connect_verified(s,p,&r,false)==ESP_OK && password_auths==1 && host_saves==1);
    ts_api_result_free(&r);cJSON_Delete(p);assert(ts_ssh_session_destroy(s)==ESP_OK);
}
static void changed_key_confirmation(void) {
    for(int confirmation=0;confirmation<3;confirmation++) {
        host_status=TS_HOST_VERIFY_MISMATCH;host_saves=password_auths=deployments=registrations=0;
        registration_error=ESP_OK;fail_deploy=fail_verify=false;
        cJSON *p=copyid_params();cJSON_AddBoolToObject(p,"trust_new",true);
        cJSON_AddBoolToObject(p,"accept_changed",true);
        if(confirmation)cJSON_AddStringToObject(p,"confirmed_fingerprint",confirmation==1?"different-fingerprint":"observed-fingerprint");
        ts_api_result_t r={0};esp_err_t ret=api_ssh_copyid(p,&r);
        if(confirmation==2) {
            assert(ret==ESP_OK && r.code==TS_API_OK && host_saves==1 && password_auths==1 && deployments==1 && registrations==1);
        } else {
            assert(ret!=ESP_OK && r.code==TS_API_ERR_HOST_MISMATCH && !host_saves && !password_auths && !deployments && !registrations);
        }
        assert(sessions==0);ts_api_result_free(&r);cJSON_Delete(p);
    }
}
static void run_case(esp_err_t err,ts_api_result_code_t expected,bool verify_failure,int port,bool verify) {
    registration_error=err;fail_verify=verify_failure;registrations=deployments=verifications=0;
    cJSON *params=cJSON_Parse("{\"host\":\"192.0.2.8\",\"user\":\"test\",\"password\":\"synthetic\",\"keyid\":\"synthetic-key\"}");
    cJSON_AddNumberToObject(params,"port",port);cJSON_AddBoolToObject(params,"verify",verify);
    ts_api_result_t r={0};esp_err_t ret=api_ssh_copyid(params,&r);
    assert(ret==ESP_OK && r.code==expected && registrations==1 && deployments==1 && sessions==0);
    assert(cJSON_IsTrue(item(&r,"deployed")));
    assert(cJSON_IsTrue(item(&r,"verified"))==(verify && !verify_failure));
    assert(verifications==(verify?1:0));
    assert(cJSON_IsTrue(item(&r,"registered"))==(err==ESP_OK));
    const char *id=port==22?"test@192.0.2.8":"test@192.0.2.8:2222";
    assert(!strcmp(item(&r,"host_id")->valuestring,id));
    assert(!strcmp(registered_config.id,id) && registered_config.port==port);
    assert(!strcmp(registered_config.keyid,"synthetic-key"));
    if(err) {
        assert(!strcmp(item(&r,"registration_error")->valuestring,error_name(err)));
        assert(!strcmp(r.message,"host_registration_failed"));
    } else assert(!item(&r,"registration_error"));
    assert(send_result(ret,r)==ESP_OK);
    cJSON *wire=cJSON_Parse(http_body);assert(wire);
    assert(cJSON_GetObjectItem(wire,"code")->valueint==expected);
    cJSON *data=cJSON_GetObjectItem(wire,"data");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(data,"deployed")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(data,"registered"))==(err==ESP_OK));
    cJSON_Delete(wire);cJSON_Delete(params);
}
int main(void) {
    first_use_case(TS_HOST_VERIFY_NOT_FOUND,1,ESP_OK,false,TS_API_OK); // WebUI default
    first_use_case(TS_HOST_VERIFY_NOT_FOUND,-1,ESP_OK,false,TS_API_OK); // API default
    first_use_case(TS_HOST_VERIFY_OK,1,ESP_OK,false,TS_API_OK);
    first_use_case(TS_HOST_VERIFY_OK,0,ESP_OK,false,TS_API_OK);
    first_use_case(TS_HOST_VERIFY_MISMATCH,1,ESP_OK,false,TS_API_ERR_HOST_MISMATCH);
    first_use_case(TS_HOST_VERIFY_NOT_FOUND,0,ESP_OK,false,TS_API_ERR_HOST_NEW);
    first_use_case(TS_HOST_VERIFY_NOT_FOUND,1,ESP_FAIL,false,TS_API_ERR_INTERNAL);
    first_use_case(TS_HOST_VERIFY_NOT_FOUND,1,ESP_OK,true,TS_API_ERR_CONNECTION);
    first_use_case(TS_HOST_VERIFY_ERROR,1,ESP_OK,false,TS_API_ERR_INTERNAL);
    other_operation_trust();
    changed_key_confirmation();
    puts("PASS production copyid first-use: automatic trust/save, API defaults, known host, changed-key rejection before auth, explicit opt-out, save/auth failures, other SSH operations retain exact confirmation");
    run_case(ESP_OK,TS_API_OK,false,22,true);
    run_case(ESP_OK,TS_API_OK,true,2222,true);
    run_case(ESP_OK,TS_API_OK,false,22,false);
    run_case(ESP_ERR_INVALID_STATE,TS_API_ERR_BUSY,false,22,true);
    run_case(ESP_ERR_NO_MEM,TS_API_ERR_NO_MEM,false,22,true);
    run_case(ESP_FAIL,TS_API_ERR_INTERNAL,true,2222,true);
    fail_deploy=true;registrations=deployments=0;
    cJSON *p=cJSON_Parse("{\"host\":\"192.0.2.8\",\"user\":\"test\",\"password\":\"synthetic\",\"keyid\":\"synthetic-key\"}");
    ts_api_result_t r={0};assert(api_ssh_copyid(p,&r)==ESP_FAIL && r.code!=TS_API_OK);
    assert(!r.data && registrations==0 && deployments==1 && sessions==0);
    ts_api_result_free(&r);cJSON_Delete(p);
    free(http_body);
    puts("PASS production ssh.copyid + HTTP serialization: actual deployment/verification/registration outcomes, default/custom IDs, busy/capacity/storage errors, partial data retained, no repeat or rollback");
}
'''
includes = ['tests/certificate/stubs', 'components/ts_security/include',
            'components/ts_api/include', 'components/ch405labs_esp_libssh2/libssh2/include',
            str(idf / 'components/json/cJSON')]
env = {**os.environ, 'DEVELOPER_DIR': '/Library/Developer/CommandLineTools'}
def compile_and_run(code, name):
    file = build / (name + '.c')
    file.write_text(code)
    subprocess.run(['cc', '-std=c11', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                    '-include', 'ts_ssh_probe.h', *[f'-I{x}' for x in includes], str(file),
                    'components/ts_security/src/ts_ssh_probe.c', str(idf / 'components/json/cJSON/cJSON.c'),
                    '-o', str(build / name)], check=True, env=env)
    subprocess.run([str(build / name)], check=True, env=env)

compile_and_run(code, 'ssh_copyid')

# Exercise real fingerprint extraction, save and readback, not the policy-only mocks above.
known_path = 'components/ts_security/src/ts_known_hosts.c'
known_source = Path(known_path).read_text()
stored_type = re.search(r'typedef struct \{.*?\} stored_host_t;', known_source, re.S).group()
known = '#include <libssh2.h>\n' + '\n'.join(re.findall(
    r'^#define\s+(?:MAX_HOST_LEN|MAX_FINGERPRINT_LEN)\b.*', known_source, re.M)) + '\n' + stored_type
known += r'''
static struct {bool initialized;nvs_handle_t nvs;SemaphoreHandle_t mutex;} s_state;
static bool s_loading_from_sdcard;
static struct {char key[32];stored_host_t value;} saved_hosts[8];
static int saved_count, store_writes, sd_exports;
static esp_err_t read_error, write_error, commit_error;
static stored_host_t pending_host;
static char pending_key[32];
static unsigned char observed_hash[32];
const char *ts_ssh_get_host(ts_ssh_session_t s) {return s->config.host;}
uint16_t ts_ssh_get_port(ts_ssh_session_t s) {return s->config.port;}
LIBSSH2_SESSION *ts_ssh_get_libssh2_session(ts_ssh_session_t s) {return (LIBSSH2_SESSION *)s;}
const char *libssh2_session_hostkey(LIBSSH2_SESSION *s,size_t *len,int *type) {
    *len=3;*type=LIBSSH2_HOSTKEY_TYPE_ECDSA_256;return "key";
}
const char *libssh2_hostkey_hash(LIBSSH2_SESSION *s,int type) {
    assert(type==LIBSSH2_HOSTKEY_HASH_SHA256);return (const char *)observed_hash;
}
const char *ts_host_key_type_str(ts_host_key_type_t type) {return "ECDSA-256";}
esp_err_t ts_known_hosts_init(void) {s_state.initialized=true;return ESP_OK;}
static void export_host_to_sdcard(const char *host,uint16_t port,const stored_host_t *value) {
    assert(!strcmp(value->host,host) && value->port==port);++sd_exports;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *len) {
    if(read_error)return read_error;
    for(int i=0;i<saved_count;i++)if(!strcmp(saved_hosts[i].key,key)) {
        assert(*len>=sizeof(stored_host_t));memcpy(out,&saved_hosts[i].value,sizeof(stored_host_t));
        *len=sizeof(stored_host_t);return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t len) {
    ++store_writes;if(write_error)return write_error;
    assert(len==sizeof(stored_host_t));strcpy(pending_key,key);memcpy(&pending_host,value,len);return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    if(commit_error)return commit_error;
    int slot=0;while(slot<saved_count && strcmp(saved_hosts[slot].key,pending_key))++slot;
    assert(slot<8);if(slot==saved_count)++saved_count;
    strcpy(saved_hosts[slot].key,pending_key);saved_hosts[slot].value=pending_host;return ESP_OK;
}
'''
for name in ['simple_hash', 'make_nvs_key', 'bytes_to_hex', 'get_key_type',
             'ts_known_hosts_get_fingerprint', 'ts_known_hosts_verify',
             'ts_known_hosts_add_manual', 'ts_known_hosts_add', 'ts_known_hosts_get']:
    known += extract(known_path, name)
integration = code
for name in ['ts_known_hosts_verify', 'ts_known_hosts_add', 'ts_known_hosts_get']:
    match = re.search(r'^esp_err_t ' + name + r'\([^;]+?\)\s*\{', integration, re.M)
    end = integration.index('\n}', match.start()) + 2
    integration = integration[:match.start()] + integration[end:]
integration = integration.replace('static esp_err_t verify_host_fingerprint(', known + '\nstatic esp_err_t verify_host_fingerprint(', 1)
integration = integration[:integration.index('int main(void) {')] + r'''
static void persisted_case(const char *host,int port,int expected) {
    cJSON *p=copyid_params();cJSON_ReplaceItemInObject(p,"host",cJSON_CreateString(host));
    cJSON_AddNumberToObject(p,"port",port);cJSON_AddBoolToObject(p,"trust_new",true);
    ts_api_result_t r={0};password_auths=deployments=registrations=verifications=0;
    esp_err_t ret=api_ssh_copyid(p,&r);assert(r.code==expected && sessions==0);
    if(expected==TS_API_OK) {
        assert(ret==ESP_OK && password_auths==1 && deployments==1 && registrations==1);
        assert(!strcmp(registered_config.host,host) && registered_config.port==port);
    } else assert(ret!=ESP_OK && !password_auths && !deployments && !registrations && !verifications);
    ts_api_result_free(&r);cJSON_Delete(p);
}
int main(void) {
    StaticSemaphore_t mutex;s_state.mutex=xSemaphoreCreateMutexStatic(&mutex);s_state.initialized=true;
    memset(observed_hash,0x41,sizeof(observed_hash));
    persisted_case("192.0.2.8",22,TS_API_OK);
    assert(saved_count==1 && store_writes==1 && sd_exports==1);
    assert(strlen(saved_hosts[0].value.fingerprint)==64);
    assert(!strcmp(saved_hosts[0].value.host,"192.0.2.8") && saved_hosts[0].value.port==22);
    persisted_case("192.0.2.8",22,TS_API_OK);assert(store_writes==1);
    persisted_case("192.0.2.8",2222,TS_API_OK);
    persisted_case("192.0.2.9",22,TS_API_OK);
    assert(saved_count==3 && store_writes==3 && sd_exports==3);
    stored_host_t original=saved_hosts[0].value;
    observed_hash[31]=0x42; // A change in the final SHA-256 byte must also be rejected.
    persisted_case("192.0.2.8",22,TS_API_ERR_HOST_MISMATCH);
    assert(store_writes==3 && !memcmp(&original,&saved_hosts[0].value,sizeof(original)));
    read_error=ESP_FAIL;persisted_case("192.0.2.10",22,TS_API_ERR_INTERNAL);read_error=ESP_OK;
    assert(store_writes==3);
    write_error=ESP_ERR_NO_MEM;persisted_case("192.0.2.10",22,TS_API_ERR_INTERNAL);write_error=ESP_OK;
    commit_error=ESP_FAIL;persisted_case("192.0.2.10",22,TS_API_ERR_INTERNAL);commit_error=ESP_OK;
    assert(saved_count==3 && sd_exports==3);
    memset(observed_hash,0x41,sizeof(observed_hash));
    persisted_case("192.0.2.8",22,TS_API_OK);assert(saved_count==3 && store_writes==5);
    pthread_mutex_destroy(&mutex);
    puts("PASS copyid + production known hosts: full SHA-256 save/readback, repeat deployment, host/port isolation, final-byte mismatch preserved, read/write/commit failure blocks authentication");
}
'''
compile_and_run(integration, 'ssh_copyid_known_hosts')
