"""Production ssh.exec / ssh.test host-key results, serialized by the production HTTP response branches.

The Web UI's "test connection" button asks the user to confirm a host fingerprint when the device answers with
code 1002 (new host) or 1001 (changed fingerprint) *and* sends the fingerprint in `data`.  ssh.exec reports those
statuses together with a non-OK return value, which used to take the generic failure branch of the HTTP handler and
lose `data`.  This suite runs the production handler functions and the production response chain (extracted from the
sources, not copied) with synthetic SSH/known-hosts back ends, checks the exact bytes that go on the wire, and keeps
them in tests/fixtures/ssh-fingerprint/.  The browser suite replays those bytes into the real Web UI, so firmware and
UI cannot drift apart unnoticed.  Regenerate with UPDATE_FIXTURES=1 after an intentional wire-format change.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys

build = Path('/tmp/tianshan-ssh-host-tests')
build.mkdir(exist_ok=True)
wire_dir = build / 'wire'
wire_dir.mkdir(exist_ok=True)
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
fixtures = Path('tests/fixtures/ssh-fingerprint')
update = os.environ.get('UPDATE_FIXTURES') == '1'
CAPTURES = ['wire-new-host.json', 'wire-changed-host.json', 'wire-exec-ok.json', 'wire-generic-failure.json',
            'wire-ssh-dns-failed.json', 'wire-ssh-port-refused.json', 'wire-ssh-unreachable.json', 'wire-ssh-timeout.json',
            'wire-ssh-handshake-failed.json', 'wire-ssh-auth-failed.json', 'wire-ssh-connect-failed.json']


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
#define TS_LOGE(...) ((void)0)
static const char *error_name(esp_err_t e) {
    switch(e) {
    case ESP_OK:return "ESP_OK";
    case ESP_ERR_INVALID_STATE:return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NO_MEM:return "ESP_ERR_NO_MEM";
    default:return "ESP_FAIL";
    }
}
#define esp_err_to_name error_name
#define TS_API_ERR_HOST_NEW 1002
#define TS_API_ERR_HOST_MISMATCH 1001
struct ts_ssh_session_s {ts_ssh_config_t config;};
typedef struct {char *data;size_t len;} ssh_request_key_t;

static ts_host_verify_result_t host_status = TS_HOST_VERIFY_OK;
static char observed_fp[80] = "SHA256:SYNTHETICobservedFINGERPRINT0123456789abcdefghi";
static int sessions, host_saves, auth_attempts, exec_calls;
static ts_ssh_fail_t mock_fail = TS_SSH_FAIL_NONE;     /* where the next connection attempt fails */

esp_err_t ts_keystore_load_private_key(const char *id,char **data,size_t *len) {
    *data=strdup("SYNTHETIC PRIVATE KEY");*len=strlen(*data);return ESP_OK;
}
esp_err_t ts_ssh_session_create(const ts_ssh_config_t *config,ts_ssh_session_t *out) {
    *out=malloc(sizeof(**out));(*out)->config=*config;++sessions;return ESP_OK;
}
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t s) {free(s);--sessions;return ESP_OK;}
esp_err_t ts_ssh_disconnect(ts_ssh_session_t s) {return ESP_OK;}
const char *ts_ssh_get_error(ts_ssh_session_t s) {
    /* the client's text carries only the ESP error number: 259 authentication, 263 timeout, -1 everything else */
    return mock_fail==TS_SSH_FAIL_AUTH?"SSH connection failed (code=259)":mock_fail==TS_SSH_FAIL_TIMEOUT?"SSH connection failed (code=263)":"SSH connection failed (code=-1)";
}
ts_ssh_fail_t ts_ssh_get_fail_stage(ts_ssh_session_t s) {return mock_fail;}
esp_err_t ts_known_hosts_verify(ts_ssh_session_t s,ts_host_verify_result_t *status,ts_known_host_t *info) {
    *status=host_status;
    if(info) {
        strcpy(info->host,s->config.host);info->port=s->config.port;
        strcpy(info->fingerprint,observed_fp);
    }
    return ESP_OK;
}
esp_err_t ts_known_hosts_add(ts_ssh_session_t s) {++host_saves;host_status=TS_HOST_VERIFY_OK;return ESP_OK;}
esp_err_t ts_known_hosts_get(const char *host,uint16_t port,ts_known_host_t *info) {
    strcpy(info->fingerprint,"SHA256:SYNTHETICstoredFINGERPRINT0123456789abcdefghijkl");return ESP_OK;
}
esp_err_t ts_ssh_connect_with_verifier(ts_ssh_session_t s,ts_ssh_verify_cb_t verify,void *context) {
    /* Same order as production: the network and the handshake first, then the host key, only then the credentials. */
    if(mock_fail>=TS_SSH_FAIL_DNS && mock_fail<=TS_SSH_FAIL_HANDSHAKE)return mock_fail==TS_SSH_FAIL_TIMEOUT?ESP_ERR_TIMEOUT:ESP_FAIL;
    esp_err_t ret=verify(s,context);if(ret!=ESP_OK)return ret;
    ++auth_attempts;return mock_fail==TS_SSH_FAIL_AUTH?ESP_ERR_INVALID_STATE:ESP_OK;
}
esp_err_t ts_ssh_exec(ts_ssh_session_t s,const char *cmd,ts_ssh_exec_result_t *r) {
    ++exec_calls;memset(r,0,sizeof(*r));r->stdout_data=strdup("TianshanOS SSH Test OK\n");return ESP_OK;
}
void ts_ssh_exec_result_free(ts_ssh_exec_result_t *r) {free(r->stdout_data);free(r->stderr_data);}
'''
for name in ['ts_api_result_free', 'ts_api_result_ok', 'ts_api_result_error', 'ts_api_result_reason']:
    code += extract('components/ts_api/src/ts_api.c', name)
code += extract('components/ts_api/src/ts_api_ssh.c', 'cleanup_key_buffer')
code += extract('components/ts_api/src/ts_api_ssh.c', 'configure_ssh_from_params')
code += extract('components/ts_api/src/ts_api_ssh.c', 'verify_host_fingerprint')
source = Path('components/ts_api/src/ts_api_ssh.c').read_text()
start = source.index('typedef struct {\n    const cJSON *params;')
code += source[start:source.index('} verify_context_t;', start) + len('} verify_context_t;')] + '\n'
code += extract('components/ts_security/src/ts_ssh_client.c', 'ts_ssh_connect_reason')
for name in ['ssh_connect_error', 'verify_before_auth', 'connect_verified', 'api_ssh_exec', 'api_ssh_test']:
    code += extract('components/ts_api/src/ts_api_ssh.c', name)

# The production response chain (success / API-not-found / failure) of api_handler, taken from the source.
web_source = Path('components/ts_webui/src/ts_webui_api.c').read_text()
start = web_source.index('    if (ret == ESP_OK || result.code == TS_API_OK) {')
end = web_source.index('\n}\n\nstatic esp_err_t login_handler', start)
code += r'''
static char *http_body;
static int http_error_status;
static char http_error_text[96];
static esp_err_t ts_http_send_json(void *req,int status,const char *json) {
    assert(status==200);free(http_body);http_body=strdup(json);return ESP_OK;
}
static esp_err_t ts_http_send_error(void *req,int status,const char *msg) {
    http_error_status=status;snprintf(http_error_text,sizeof http_error_text,"%s",msg);return ESP_FAIL;
}
static esp_err_t send_result(esp_err_t ret,ts_api_result_t result) {
    void *req=NULL;
''' + web_source[start:end] + '\n}\n'

code += r'''
static void save(const char *name) {
    const char *dir=getenv("WIRE_DIR");assert(dir);
    char path[512];snprintf(path,sizeof path,"%s/%s",dir,name);
    FILE *f=fopen(path,"w");assert(f);fputs(http_body,f);fclose(f);
}
static void reset(ts_host_verify_result_t status) {
    host_status=status;host_saves=auth_attempts=exec_calls=0;mock_fail=TS_SSH_FAIL_NONE;
    strcpy(observed_fp,"SHA256:SYNTHETICobservedFINGERPRINT0123456789abcdefghi");
}
/* Exactly what the Web UI sends for "test connection". */
static cJSON *page_params(void) {
    return cJSON_Parse("{\"host\":\"192.0.2.8\",\"port\":22,\"user\":\"test\",\"keyid\":\"synthetic-key\","
                       "\"command\":\"echo \\\"TianshanOS SSH Test OK\\\"\",\"trust_new\":false}");
}
typedef esp_err_t (*handler_t)(const cJSON *,ts_api_result_t *);
/* Run a production handler, push its result through the production response chain, return the wire JSON. */
static cJSON *wire_of(handler_t handler,cJSON *params,const char *capture,esp_err_t *ret_out,int *code_out) {
    ts_api_result_t r={0};esp_err_t ret=handler(params,&r);
    if(ret_out)*ret_out=ret;
    if(code_out)*code_out=r.code;
    int code=r.code;
    assert(send_result(ret,r)==ESP_OK);
    cJSON *wire=cJSON_Parse(http_body);assert(wire);
    assert(cJSON_GetObjectItem(wire,"code")->valueint==code);
    if(capture)save(capture);
    return wire;
}
static const char *str(cJSON *o,const char *key) {
    cJSON *i=cJSON_GetObjectItem(o,key);assert(cJSON_IsString(i));return i->valuestring;
}
static void expect_no_progress(void) {assert(!host_saves && !auth_attempts && !exec_calls && sessions==0);}

static void new_host(void) {
    reset(TS_HOST_VERIFY_NOT_FOUND);
    esp_err_t ret;int code;
    cJSON *p=page_params();
    cJSON *wire=wire_of(api_ssh_exec,p,"wire-new-host.json",&ret,&code);
    assert(ret!=ESP_OK && code==TS_API_ERR_HOST_NEW);
    /* THE regression: the fingerprint must reach the client even though ssh.exec returned a failure. */
    cJSON *data=cJSON_GetObjectItem(wire,"data");assert(cJSON_IsObject(data));
    assert(!strcmp(str(wire,"error"),"ssh_host_key_unknown"));
    assert(!strcmp(str(data,"status"),"new_host") && !strcmp(str(data,"host"),"192.0.2.8"));
    assert(cJSON_GetObjectItem(data,"port")->valueint==22);
    assert(!strcmp(str(data,"fingerprint"),observed_fp));
    expect_no_progress();                                     /* nothing trusted, no credentials sent, no command run */
    char shown[80];strcpy(shown,str(data,"fingerprint"));cJSON_Delete(wire);

    /* The user confirms exactly what was shown. */
    cJSON_AddStringToObject(p,"confirmed_fingerprint",shown);
    cJSON_ReplaceItemInObject(p,"trust_new",cJSON_CreateTrue());
    cJSON_AddBoolToObject(p,"accept_changed",false);
    wire=wire_of(api_ssh_exec,p,"wire-exec-ok.json",&ret,&code);
    assert(ret==ESP_OK && code==TS_API_OK && host_saves==1 && auth_attempts==1 && exec_calls==1 && sessions==0);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(wire,"data"),"exit_code")->valueint==0);
    cJSON_Delete(wire);cJSON_Delete(p);
}
static void new_host_other_key_on_confirm(void) {
    /* The key presented changed between the dialog and the confirmation: confirming the old text must not trust it. */
    reset(TS_HOST_VERIFY_NOT_FOUND);
    esp_err_t ret;int code;
    cJSON *p=page_params();
    cJSON_AddStringToObject(p,"confirmed_fingerprint","SHA256:SYNTHETICsomethingTHEuserNEVERsaw000000000000000000");
    cJSON_ReplaceItemInObject(p,"trust_new",cJSON_CreateTrue());
    cJSON *wire=wire_of(api_ssh_exec,p,NULL,&ret,&code);
    assert(ret!=ESP_OK && code==TS_API_ERR_HOST_NEW);
    assert(!strcmp(str(cJSON_GetObjectItem(wire,"data"),"fingerprint"),observed_fp));   /* shows what is really presented */
    expect_no_progress();
    cJSON_Delete(wire);cJSON_Delete(p);
}
static void changed_host(void) {
    reset(TS_HOST_VERIFY_MISMATCH);
    esp_err_t ret;int code;
    cJSON *p=page_params();
    cJSON *wire=wire_of(api_ssh_exec,p,"wire-changed-host.json",&ret,&code);
    assert(ret!=ESP_OK && code==TS_API_ERR_HOST_MISMATCH);
    cJSON *data=cJSON_GetObjectItem(wire,"data");assert(cJSON_IsObject(data));
    assert(!strcmp(str(wire,"error"),"ssh_host_key_changed"));
    assert(!strcmp(str(data,"status"),"mismatch"));
    assert(!strcmp(str(data,"current_fingerprint"),observed_fp));
    assert(!strncmp(str(data,"stored_fingerprint"),"SHA256:SYNTHETICstored",22));
    expect_no_progress();
    char shown[80];strcpy(shown,str(data,"current_fingerprint"));cJSON_Delete(wire);

    /* accept_changed alone (no matching confirmation) is not enough. */
    cJSON_ReplaceItemInObject(p,"trust_new",cJSON_CreateFalse());
    cJSON_AddBoolToObject(p,"accept_changed",true);
    wire=wire_of(api_ssh_exec,p,NULL,&ret,&code);
    assert(ret!=ESP_OK && code==TS_API_ERR_HOST_MISMATCH);expect_no_progress();cJSON_Delete(wire);

    /* Confirmation of a different fingerprint is not enough either. */
    cJSON_AddStringToObject(p,"confirmed_fingerprint","SHA256:SYNTHETICsomethingELSE00000000000000000000000000000");
    wire=wire_of(api_ssh_exec,p,NULL,&ret,&code);
    assert(ret!=ESP_OK && code==TS_API_ERR_HOST_MISMATCH);expect_no_progress();cJSON_Delete(wire);

    /* The fingerprint the user was shown, with accept_changed, is accepted exactly once. */
    cJSON_ReplaceItemInObject(p,"confirmed_fingerprint",cJSON_CreateString(shown));
    wire=wire_of(api_ssh_exec,p,NULL,&ret,&code);
    assert(ret==ESP_OK && code==TS_API_OK && host_saves==1 && auth_attempts==1 && exec_calls==1 && sessions==0);
    cJSON_Delete(wire);cJSON_Delete(p);
}
static void known_host(void) {
    reset(TS_HOST_VERIFY_OK);
    esp_err_t ret;int code;
    cJSON *p=page_params();
    cJSON *wire=wire_of(api_ssh_exec,p,NULL,&ret,&code);
    assert(ret==ESP_OK && code==TS_API_OK && host_saves==0 && auth_attempts==1 && exec_calls==1 && sessions==0);
    cJSON_Delete(wire);cJSON_Delete(p);
}
static void ssh_test_endpoint(void) {
    /* ssh.test reports the same statuses with an OK return value; it always carried the data. */
    reset(TS_HOST_VERIFY_NOT_FOUND);
    esp_err_t ret;int code;
    cJSON *p=page_params();
    cJSON *wire=wire_of(api_ssh_test,p,NULL,&ret,&code);
    assert(ret==ESP_OK && code==TS_API_ERR_HOST_NEW);
    assert(!strcmp(str(cJSON_GetObjectItem(wire,"data"),"fingerprint"),observed_fp));
    expect_no_progress();cJSON_Delete(wire);cJSON_Delete(p);
}
static void connection_failures(void) {
    /* Each way a connection can fail reaches the client as its own reason; the client's text is kept as detail. */
    static const struct {ts_ssh_fail_t stage;const char *token,*stage_name,*detail,*capture;} cases[]={
        {TS_SSH_FAIL_DNS,"ssh_dns_failed","dns","SSH connection failed (code=-1)","wire-ssh-dns-failed.json"},
        {TS_SSH_FAIL_REFUSED,"ssh_port_refused","refused","SSH connection failed (code=-1)","wire-ssh-port-refused.json"},
        {TS_SSH_FAIL_UNREACHABLE,"ssh_host_unreachable","unreachable","SSH connection failed (code=-1)","wire-ssh-unreachable.json"},
        {TS_SSH_FAIL_TIMEOUT,"ssh_connect_timeout","timeout","SSH connection failed (code=263)","wire-ssh-timeout.json"},
        {TS_SSH_FAIL_HANDSHAKE,"ssh_handshake_failed","handshake","SSH connection failed (code=-1)","wire-ssh-handshake-failed.json"},
        {TS_SSH_FAIL_AUTH,"ssh_auth_failed","auth","SSH connection failed (code=259)","wire-ssh-auth-failed.json"},
        {TS_SSH_FAIL_CONNECT,"ssh_connect_failed","connect","SSH connection failed (code=-1)","wire-ssh-connect-failed.json"},
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        reset(TS_HOST_VERIFY_OK);mock_fail=cases[i].stage;
        esp_err_t ret;int code;cJSON *p=page_params();
        cJSON *wire=wire_of(api_ssh_exec,p,cases[i].capture,&ret,&code);
        assert(ret!=ESP_OK && code==TS_API_ERR_CONNECTION);
        assert(!strcmp(str(wire,"error"),cases[i].token));
        cJSON *data=cJSON_GetObjectItem(wire,"data");assert(cJSON_IsObject(data));
        assert(!strcmp(str(data,"stage"),cases[i].stage_name) && !strcmp(str(data,"detail"),cases[i].detail));
        assert(!exec_calls && !host_saves && sessions==0);
        assert(cases[i].stage==TS_SSH_FAIL_AUTH ? auth_attempts==1 : auth_attempts==0);
        cJSON_Delete(wire);cJSON_Delete(p);
    }
    /* ssh.test keeps its envelope (success code, data.success=false, the old `error` text) and gains reason + stage */
    reset(TS_HOST_VERIFY_OK);mock_fail=TS_SSH_FAIL_AUTH;
    esp_err_t ret;int code;cJSON *p=page_params();
    cJSON *wire=wire_of(api_ssh_test,p,NULL,&ret,&code);
    cJSON *data=cJSON_GetObjectItem(wire,"data");
    assert(ret==ESP_OK && code==TS_API_OK && cJSON_IsFalse(cJSON_GetObjectItem(data,"success")));
    assert(!strcmp(str(data,"error"),"SSH connection failed (code=259)") && !strcmp(str(data,"reason"),"ssh_auth_failed") && !strcmp(str(data,"stage"),"auth"));
    cJSON_Delete(wire);cJSON_Delete(p);
}
static void failures_without_details_keep_their_exact_shape(void) {
    esp_err_t ret;int code;
    cJSON *p=cJSON_Parse("{\"host\":\"192.0.2.8\",\"user\":\"test\",\"keyid\":\"k\"}");   /* no command */
    cJSON *wire=wire_of(api_ssh_exec,p,"wire-generic-failure.json",&ret,&code);
    assert(ret!=ESP_OK && code==TS_API_ERR_INVALID_ARG);
    assert(!strcmp(http_body,"{\"code\":1,\"error\":\"Missing 'command' parameter\"}"));
    assert(!cJSON_GetObjectItem(wire,"data"));
    cJSON_Delete(wire);cJSON_Delete(p);
    /* A missing API stays an HTTP 404, not a business failure. */
    ts_api_result_t r={0};ts_api_result_error(&r,TS_API_ERR_NOT_FOUND,"API not found");
    http_error_status=0;assert(send_result(ESP_ERR_NOT_FOUND,r)==ESP_FAIL);
    assert(http_error_status==404 && !strcmp(http_error_text,"API not found"));
}
static int alloc_calls, alloc_fail_at;
static void *counting_malloc(size_t n) {if(++alloc_calls==alloc_fail_at)return NULL;return malloc(n);}
static void failure_branch_under_allocation_failure(void) {
    /* An allocation can fail at any point while the failure response is built. The branch must then send either the
     * complete response (code, error and the full details) or HTTP 500 - never a truncated or corrupt document. */
    reset(TS_HOST_VERIFY_NOT_FOUND);
    cJSON_Hooks counting={counting_malloc,free};
    int with_details=0,without_details=0,internal_error=0;
    for(int k=1;k<=64;k++) {
        cJSON *p=page_params();ts_api_result_t r={0};
        esp_err_t ret=api_ssh_exec(p,&r);cJSON_Delete(p);                 /* the real handler, normal allocator */
        assert(ret!=ESP_OK && r.code==TS_API_ERR_HOST_NEW && r.data);
        free(http_body);http_body=NULL;http_error_status=0;
        alloc_calls=0;alloc_fail_at=k;cJSON_InitHooks(&counting);          /* only the response chain runs with injection */
        esp_err_t sent=send_result(ret,r);
        cJSON_InitHooks(NULL);
        if(http_body) {
            assert(sent==ESP_OK && !http_error_status);
            cJSON *wire=cJSON_Parse(http_body);assert(wire);
            assert(cJSON_GetObjectItem(wire,"code")->valueint==TS_API_ERR_HOST_NEW);
            assert(!strcmp(str(wire,"error"),"ssh_host_key_unknown"));
            cJSON *data=cJSON_GetObjectItem(wire,"data");
            if(data) {                                                     /* complete, never partial */
                assert(!strcmp(str(data,"status"),"new_host") && !strcmp(str(data,"host"),"192.0.2.8"));
                assert(cJSON_GetObjectItem(data,"port")->valueint==22 && !strcmp(str(data,"fingerprint"),observed_fp));
                assert(cJSON_IsString(cJSON_GetObjectItem(data,"message")));
                ++with_details;
            } else ++without_details;                                      /* details dropped, error kept */
            cJSON_Delete(wire);
        } else {
            assert(sent!=ESP_OK && http_error_status==500);++internal_error;
        }
        if(alloc_calls<k) break;                       /* the injected failure was never reached: sweep complete */
    }
    assert(with_details>=1 && without_details>=1 && internal_error>=1);
}
int main(void) {
    new_host();
    new_host_other_key_on_confirm();
    changed_host();
    known_host();
    connection_failures();
    ssh_test_endpoint();
    failures_without_details_keep_their_exact_shape();
    failure_branch_under_allocation_failure();
    free(http_body);
    puts("PASS production ssh.exec/ssh.test + HTTP response chain: fingerprint reaches the client for new/changed hosts; nothing trusted, authenticated or executed before an exact confirmation; wrong or missing confirmation rejected; plain failures keep {code,error}; allocation failure at every point gives the complete response or HTTP 500");
}
'''
includes = ['tests/certificate/stubs', 'components/ts_security/include',
            'components/ts_api/include', 'components/ch405labs_esp_libssh2/libssh2/include',
            str(idf / 'components/json/cJSON')]
env = {**os.environ, 'DEVELOPER_DIR': '/Library/Developer/CommandLineTools', 'WIRE_DIR': str(wire_dir)}
for stale in wire_dir.glob('*.json'):
    stale.unlink()
file = build / 'ssh_fingerprint_wire.c'
file.write_text(code)
subprocess.run(['cc', '-std=c11', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                '-include', 'ts_ssh_probe.h', *[f'-I{x}' for x in includes], str(file),
                'components/ts_security/src/ts_ssh_probe.c', str(idf / 'components/json/cJSON/cJSON.c'),
                '-o', str(build / 'ssh_fingerprint_wire')], check=True, env=env)
subprocess.run([str(build / 'ssh_fingerprint_wire')], check=True, env=env)

# The bytes the device sends are contract fixtures for the Web UI tests.
status = 0
fixtures.mkdir(parents=True, exist_ok=True)
for name in CAPTURES:
    produced = (wire_dir / name).read_text()
    target = fixtures / name
    if update:
        target.write_text(produced)
        print('updated', target)
    elif not target.exists() or json.loads(target.read_text()) != json.loads(produced):
        print(f'FIXTURE DRIFT: {target} no longer matches what the production code sends.')
        print('  produced:', produced)
        print('  committed:', target.read_text() if target.exists() else '(missing)')
        print('  If the wire change is intentional: UPDATE_FIXTURES=1 python3 tests/runtime/test_ssh_fingerprint_wire.py')
        status = 1
if status:
    sys.exit(status)
print('PASS wire fixtures match: ' + ', '.join(CAPTURES))
