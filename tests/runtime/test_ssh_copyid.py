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
static esp_err_t connect_verified(ts_ssh_session_t s,const cJSON *p,ts_api_result_t *r) {
    assert(s->config.auth_method==TS_SSH_AUTH_PASSWORD);return ESP_OK;
}
static esp_err_t verify_host_fingerprint(ts_ssh_session_t s,const cJSON *p,ts_api_result_t *r,ts_known_host_t *h) {return ESP_OK;}
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
for name in ['cleanup_key_buffer', 'api_ssh_copyid']:
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
file = build / 'ssh_copyid.c'
file.write_text(code)
includes = ['tests/certificate/stubs', 'components/ts_security/include',
            'components/ts_api/include', str(idf / 'components/json/cJSON')]
env = {**os.environ, 'DEVELOPER_DIR': '/Library/Developer/CommandLineTools'}
subprocess.run(['cc', '-std=c11', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                *[f'-I{x}' for x in includes], str(file), str(idf / 'components/json/cJSON/cJSON.c'),
                '-o', str(build / 'ssh_copyid')], check=True, env=env)
subprocess.run([str(build / 'ssh_copyid')], check=True, env=env)
