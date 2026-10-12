"""Production rule-save/toggle and manual-trigger failures, serialized by the production HTTP response chain.

Runs the production `rule_commit_reply` (every automation.rules.add / update / enable / disable result goes through it)
and the production `api_automation_rules_trigger`, with every way they can refuse, through the production response chain
of ts_webui_api.c, and keeps the exact bytes the device sends in tests/fixtures/failure-wire/. The browser suite replays
those bytes into the real Web UI, so the reason tokens the firmware sends and the texts the page shows cannot drift apart.
Regenerate with UPDATE_FIXTURES=1 after an intentional wire-format change.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys

build = Path('/tmp/tianshan-runtime-tests')
build.mkdir(exist_ok=True)
wire_dir = build / 'failure-wire'
wire_dir.mkdir(exist_ok=True)
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
fixtures = Path('tests/fixtures/failure-wire')
update = os.environ.get('UPDATE_FIXTURES') == '1'
CAPTURES = ['wire-rule-ok.json', 'wire-rule-ok-mirror.json', 'wire-rule-execution-busy.json', 'wire-rule-revision-conflict.json',
            'wire-rule-service-busy.json', 'wire-rule-commit-unknown.json', 'wire-rule-capacity.json',
            'wire-trigger-ok.json', 'wire-trigger-engine-not-accepting.json', 'wire-trigger-rule-disabled.json',
            'wire-trigger-manual-not-allowed.json', 'wire-trigger-rule-pending-delete.json', 'wire-trigger-execution-busy.json', 'wire-trigger-service-not-admissible.json',
            'wire-trigger-not-found.json', 'wire-trigger-generic.json',
            'wire-engine-start-ok.json', 'wire-engine-start-paused.json', 'wire-engine-start-unavailable.json', 'wire-engine-start-nomem.json',
            'wire-engine-stop-timeout.json', 'wire-engine-pause-not-running.json', 'wire-engine-pause-paused.json',
            'wire-engine-reload-config-invalid.json', 'wire-engine-reload-unavailable.json',
            'wire-action-save-capacity.json', 'wire-action-save-exists.json', 'wire-action-save-protected.json',
            'wire-action-save-not-found.json', 'wire-action-save-generic.json',
            'wire-action-run-failed.json', 'wire-action-run-failed-no-output.json', 'wire-action-run-timeout.json', 'wire-action-run-not-found.json',
            'wire-source-add-ok.json', 'wire-source-add-capacity.json', 'wire-source-add-unavailable.json',
            'wire-proxy-connect-failed.json', 'wire-proxy-http-error.json',
            'wire-pack-wrong-device.json', 'wire-pack-signature.json', 'wire-pack-parse.json', 'wire-pack-not-allowed.json']


def extract(path, name):
    source = Path(path).read_text()
    match = re.search(r'^(?:static )?[^\n;]+\b' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.start()) + 2] + '\n'


code = r'''
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "ts_api.h"
#include "ts_rule_engine.h"
#include "ts_automation.h"
#include "ts_action_manager.h"
#include "ts_source_manager.h"
#include "ts_config_pack.h"
#define ESP_LOGI(...) ((void)0)
#define TS_MALLOC_PSRAM malloc
#define TS_STRDUP_PSRAM strdup
static const char *error_name(esp_err_t e) {
    switch(e){case ESP_OK:return "ESP_OK";case ESP_ERR_INVALID_SIZE:return "ESP_ERR_INVALID_SIZE";case ESP_ERR_INVALID_ARG:return "ESP_ERR_INVALID_ARG";
              case ESP_ERR_TIMEOUT:return "ESP_ERR_TIMEOUT";default:return "ESP_FAIL";}
}
#define esp_err_to_name error_name
static ts_rule_reject_t mock_why = TS_RULE_REJECT_NONE;
static esp_err_t mock_ret = ESP_OK;
esp_err_t ts_rule_trigger_ex(const char *id, ts_rule_reject_t *why) {if(why)*why=mock_why;return mock_ret;}
bool ts_rule_restart_pending(void) {return false;}
void ts_rule_saved_status(const char *id, cJSON *object) {(void)id;(void)object;}
static esp_err_t mock_engine = ESP_OK;
static ts_automation_state_t mock_state = TS_AUTO_STATE_RUNNING;
esp_err_t ts_automation_start(void) {return mock_engine;}
esp_err_t ts_automation_stop(void) {return mock_engine;}
esp_err_t ts_automation_pause(void) {return mock_engine;}
esp_err_t ts_automation_reload(void) {return mock_engine;}
esp_err_t ts_automation_get_status(ts_automation_status_t *s) {memset(s,0,sizeof *s);s->state=mock_state;return ESP_OK;}
'''
for name in ['ts_api_result_free', 'ts_api_result_ok', 'ts_api_result_error', 'ts_api_result_reason']:
    code += extract('components/ts_api/src/ts_api.c', name)
code += extract('components/ts_api/src/ts_api_pack.h', 'pack_failure')
for name in ['proxy_http_error', 'automation_state', 'engine_refusal_token', 'api_automation_start', 'api_automation_stop', 'api_automation_pause',
             'api_automation_reload', 'action_template_write_reply', 'action_execute_failure', 'source_register_reply',
             'rule_commit_reply', 'trigger_reject_token', 'trigger_reject_code', 'api_automation_rules_trigger']:
    code += extract('components/ts_api/src/ts_api_automation.c', name)
pack_source = Path('components/ts_config_pack/src/ts_config_pack.c').read_text()
table = re.search(r'static const char \*s_error_messages\[\] = \{.*?\n\};\n', pack_source, re.S)
assert table
code += table.group(0)
for name in ['ts_config_pack_strerror', 'ts_config_pack_reason']:
    code += extract('components/ts_config_pack/src/ts_config_pack.c', name)
web_source = Path('components/ts_webui/src/ts_webui_api.c').read_text()
start = web_source.index('    if (ret == ESP_OK || result.code == TS_API_OK) {')
end = web_source.index('\n}\n\nstatic esp_err_t login_handler', start)
code += r'''
static char *http_body;
static esp_err_t ts_http_send_json(void *req,int status,const char *json) {
    assert(status==200);free(http_body);http_body=strdup(json);return ESP_OK;
}
static esp_err_t ts_http_send_error(void *req,int status,const char *msg) {return ESP_FAIL;}
static esp_err_t send_result(esp_err_t ret,ts_api_result_t result) {
    void *req=NULL;
''' + web_source[start:end] + '\n}\n'
code += r'''
static void save(const char *name) {
    const char *dir=getenv("WIRE_DIR");assert(dir);
    char path[512];snprintf(path,sizeof path,"%s/%s",dir,name);
    FILE *f=fopen(path,"w");assert(f);fputs(http_body,f);fclose(f);
}
static cJSON *wire(void) {cJSON *w=cJSON_Parse(http_body);assert(w);return w;}
static const char *str(cJSON *o,const char *key) {cJSON *i=cJSON_GetObjectItem(o,key);assert(cJSON_IsString(i));return i->valuestring;}

/* what rule_mutate_locked does with a commit outcome: reply, then return ESP_OK whatever happened */
static void rule_reply(esp_err_t ret,const char *error_code,int applied,int durable,int mirror,const char *capture,int expect_code) {
    ts_rule_commit_result_t commit={.applied=applied,.durable=durable,.mirror_synced=mirror,.revision=7,.error_code=error_code};
    ts_api_result_t r={0};rule_commit_reply(&r,ret,&commit);
    assert(r.code==expect_code);
    assert(send_result(ESP_OK,r)==ESP_OK);
    cJSON *w=wire();
    assert(cJSON_GetObjectItem(w,"code")->valueint==expect_code);
    assert(!strcmp(str(w,"message"),error_code));                              /* the reason token is `message` here */
    cJSON *data=cJSON_GetObjectItem(w,"data");assert(cJSON_IsObject(data));
    assert(!strcmp(str(data,"error_code"),error_code) && cJSON_GetObjectItem(data,"revision")->valueint==7);
    save(capture);cJSON_Delete(w);
}
static void trigger(esp_err_t ret,ts_rule_reject_t why,const char *capture,int expect_code,const char *expect_message) {
    mock_ret=ret;mock_why=why;
    cJSON *p=cJSON_Parse("{\"id\":\"night-off\"}");ts_api_result_t r={0};
    assert(api_automation_rules_trigger(p,&r)==ESP_OK);
    assert(r.code==expect_code);
    assert(send_result(ESP_OK,r)==ESP_OK);
    cJSON *w=wire();assert(cJSON_GetObjectItem(w,"code")->valueint==expect_code);
    assert(!strcmp(str(w,"message"),expect_message));
    save(capture);cJSON_Delete(w);cJSON_Delete(p);
}
/* what the production HTTP chain sends for a handler's result, and the checks every reply has to pass */
static cJSON *sent(ts_api_result_t r,const char *capture,int code,const char *message) {
    assert(r.code==code);
    assert(send_result(ESP_OK,r)==ESP_OK);
    cJSON *w=wire();assert(cJSON_GetObjectItem(w,"code")->valueint==code);
    assert(!strcmp(str(w,"message"),message));
    save(capture);
    return w;
}
static void sent_with_detail(ts_api_result_t r,const char *capture,int code,const char *message,const char *detail) {
    cJSON *w=sent(r,capture,code,message);
    cJSON *data=cJSON_GetObjectItem(w,"data");
    if(detail){assert(cJSON_IsObject(data));assert(!strcmp(str(data,"detail"),detail));}
    cJSON_Delete(w);
}
static void engine(esp_err_t (*handler)(const cJSON *,ts_api_result_t *),esp_err_t ret,ts_automation_state_t state,const char *capture,int code,const char *message,const char *detail) {
    mock_engine=ret;mock_state=state;
    ts_api_result_t r={0};
    assert(handler(NULL,&r)==ESP_OK);
    sent_with_detail(r,capture,code,message,detail);
}
int main(void) {
    /* the engine's own buttons: start / stop / pause / reload say exactly why they were refused */
    engine(api_automation_start,ESP_OK,TS_AUTO_STATE_INITIALIZED,"wire-engine-start-ok.json",TS_API_OK,"Automation engine started",NULL);
    engine(api_automation_start,ESP_ERR_INVALID_STATE,TS_AUTO_STATE_PAUSED,"wire-engine-start-paused.json",TS_API_ERR_BUSY,"engine_paused",NULL);
    engine(api_automation_start,ESP_ERR_INVALID_STATE,TS_AUTO_STATE_INITIALIZED,"wire-engine-start-unavailable.json",TS_API_ERR_BUSY,"engine_unavailable",NULL);
    engine(api_automation_start,ESP_ERR_NO_MEM,TS_AUTO_STATE_INITIALIZED,"wire-engine-start-nomem.json",TS_API_ERR_NO_MEM,"no_memory",NULL);
    engine(api_automation_stop,ESP_ERR_TIMEOUT,TS_AUTO_STATE_RUNNING,"wire-engine-stop-timeout.json",TS_API_ERR_BUSY,"engine_stop_timeout",NULL);
    engine(api_automation_pause,ESP_ERR_INVALID_STATE,TS_AUTO_STATE_INITIALIZED,"wire-engine-pause-not-running.json",TS_API_ERR_BUSY,"engine_not_running",NULL);
    engine(api_automation_pause,ESP_ERR_INVALID_STATE,TS_AUTO_STATE_PAUSED,"wire-engine-pause-paused.json",TS_API_ERR_BUSY,"engine_paused",NULL);
    engine(api_automation_reload,ESP_ERR_INVALID_SIZE,TS_AUTO_STATE_RUNNING,"wire-engine-reload-config-invalid.json",TS_API_ERR_INTERNAL,"config_file_invalid","ESP_ERR_INVALID_SIZE");
    engine(api_automation_reload,ESP_ERR_INVALID_STATE,TS_AUTO_STATE_UNINITIALIZED,"wire-engine-reload-unavailable.json",TS_API_ERR_BUSY,"engine_unavailable",NULL);
    /* anything else stays what it was: a class code and the old English text */
    {ts_api_result_t r={0};mock_engine=ESP_FAIL;assert(api_automation_start(NULL,&r)==ESP_OK);assert(r.code==TS_API_ERR_INTERNAL&&!strcmp(r.message,"Failed to start automation engine"));ts_api_result_free(&r);}
    {ts_api_result_t r={0};mock_engine=ESP_FAIL;assert(api_automation_stop(NULL,&r)==ESP_OK);assert(r.code==TS_API_ERR_INTERNAL&&!strcmp(r.message,"Failed to stop automation engine"));ts_api_result_free(&r);}
    {ts_api_result_t r={0};mock_engine=ESP_FAIL;assert(api_automation_pause(NULL,&r)==ESP_OK);assert(r.code==TS_API_ERR_INTERNAL&&!strcmp(r.message,"Failed to pause automation engine"));ts_api_result_free(&r);}
    {ts_api_result_t r={0};mock_engine=ESP_FAIL;assert(api_automation_reload(NULL,&r)==ESP_OK);assert(r.code==TS_API_ERR_INTERNAL&&!strcmp(r.message,"Failed to reload configuration"));ts_api_result_free(&r);}
    /* saving an action template */
    {ts_api_result_t r={0};action_template_write_reply(&r,ESP_ERR_NO_MEM,false);sent_with_detail(r,"wire-action-save-capacity.json",TS_API_ERR_INTERNAL,"template_capacity",NULL);}
    {ts_api_result_t r={0};action_template_write_reply(&r,ESP_ERR_INVALID_STATE,false);sent_with_detail(r,"wire-action-save-exists.json",TS_API_ERR_INVALID_ARG,"template_exists",NULL);}
    {ts_api_result_t r={0};action_template_write_reply(&r,ESP_ERR_INVALID_STATE,true);sent_with_detail(r,"wire-action-save-protected.json",TS_API_ERR_BUSY,"service_delete_protected",NULL);}
    {ts_api_result_t r={0};action_template_write_reply(&r,ESP_ERR_NOT_FOUND,true);sent_with_detail(r,"wire-action-save-not-found.json",TS_API_ERR_NOT_FOUND,"Template not found",NULL);}
    {ts_api_result_t r={0};action_template_write_reply(&r,ESP_FAIL,true);sent_with_detail(r,"wire-action-save-generic.json",TS_API_ERR_INTERNAL,"Failed to update template",NULL);}
    {ts_api_result_t r={0};action_template_write_reply(&r,ESP_FAIL,false);assert(!strcmp(r.message,"Failed to create template"));ts_api_result_free(&r);}
    /* running an action template: the reason for the page, what the action printed for the folded details */
    {ts_action_result_t x={0};x.status=TS_ACTION_STATUS_FAILED;snprintf(x.output,sizeof x.output,"command exited with 3");
     ts_api_result_t r={0};r.data=cJSON_CreateObject();action_execute_failure(&r,ESP_FAIL,&x);sent_with_detail(r,"wire-action-run-failed.json",TS_API_ERR_INTERNAL,"action_failed","command exited with 3");}
    {ts_action_result_t x={0};x.status=TS_ACTION_STATUS_FAILED;
     ts_api_result_t r={0};r.data=cJSON_CreateObject();action_execute_failure(&r,ESP_FAIL,&x);sent_with_detail(r,"wire-action-run-failed-no-output.json",TS_API_ERR_INTERNAL,"action_failed","Action failed: ESP_FAIL (status=3)");}
    {ts_action_result_t x={0};x.status=TS_ACTION_STATUS_TIMEOUT;snprintf(x.output,sizeof x.output,"no answer");
     ts_api_result_t r={0};r.data=cJSON_CreateObject();action_execute_failure(&r,ESP_ERR_TIMEOUT,&x);sent_with_detail(r,"wire-action-run-timeout.json",TS_API_ERR_INTERNAL,"action_timeout","no answer");}
    {ts_action_result_t x={0};x.status=TS_ACTION_STATUS_FAILED;snprintf(x.output,sizeof x.output,"Template not found: demo");
     ts_api_result_t r={0};r.data=cJSON_CreateObject();action_execute_failure(&r,ESP_ERR_NOT_FOUND,&x);sent_with_detail(r,"wire-action-run-not-found.json",TS_API_ERR_NOT_FOUND,"Template not found",NULL);}
    /* creating a data source */
    {ts_auto_source_t src={0};snprintf(src.id,sizeof src.id,"power");
     ts_api_result_t r={0};source_register_reply(&r,ESP_OK,&src);cJSON *w=sent(r,"wire-source-add-ok.json",TS_API_OK,"Source created successfully");assert(!strcmp(str(cJSON_GetObjectItem(w,"data"),"id"),"power"));cJSON_Delete(w);}
    {ts_auto_source_t src={0};ts_api_result_t r={0};source_register_reply(&r,ESP_ERR_NO_MEM,&src);sent_with_detail(r,"wire-source-add-capacity.json",TS_API_ERR_INTERNAL,"source_capacity",NULL);}
    {ts_auto_source_t src={0};ts_api_result_t r={0};source_register_reply(&r,ESP_ERR_INVALID_STATE,&src);sent_with_detail(r,"wire-source-add-unavailable.json",TS_API_ERR_BUSY,"engine_unavailable",NULL);}
    {ts_auto_source_t src={0};ts_api_result_t r={0};source_register_reply(&r,ESP_FAIL,&src);assert(!strcmp(r.message,"Failed to create source"));ts_api_result_free(&r);}
    /* testing a data source's address: the reason for the page, the status as a number, the start of the body as detail */
    {ts_api_result_t r={0};ts_api_result_reason(&r,TS_API_ERR_CONNECTION,"proxy_connect_failed","ESP_ERR_TIMEOUT");sent_with_detail(r,"wire-proxy-connect-failed.json",TS_API_ERR_CONNECTION,"proxy_connect_failed","ESP_ERR_TIMEOUT");}
    {ts_api_result_t r={0};proxy_http_error(&r,401,"Unauthorized");cJSON *w=sent(r,"wire-proxy-http-error.json",TS_API_ERR_INTERNAL,"proxy_http_error");
     cJSON *d=cJSON_GetObjectItem(w,"data");assert(cJSON_GetObjectItem(d,"status")->valueint==401&&!strcmp(str(d,"detail"),"Unauthorized"));cJSON_Delete(w);}
    {char body[400];memset(body,'x',sizeof body);body[399]=0;ts_api_result_t r={0};proxy_http_error(&r,500,body);
     cJSON *d=cJSON_GetObjectItem(r.data,"data");(void)d;assert(strlen(cJSON_GetObjectItem(r.data,"detail")->valuestring)==160&&cJSON_GetObjectItem(r.data,"status")->valueint==500);ts_api_result_free(&r);}
    {ts_api_result_t r={0};proxy_http_error(&r,404,NULL);assert(cJSON_GetObjectItem(r.data,"status")->valueint==404&&!cJSON_GetObjectItem(r.data,"detail"));ts_api_result_free(&r);}
    /* config packs: every result has a reason token of its own, the library's English text goes to the details */
    {ts_api_result_t r={0};pack_failure(&r,TS_CONFIG_PACK_ERR_RECIPIENT);sent_with_detail(r,"wire-pack-wrong-device.json",TS_API_ERR_INVALID_ARG,"pack_wrong_device","Not intended for this device");}
    {ts_api_result_t r={0};pack_failure(&r,TS_CONFIG_PACK_ERR_SIGNATURE);sent_with_detail(r,"wire-pack-signature.json",TS_API_ERR_INVALID_ARG,"pack_signature_invalid","Signature verification failed");}
    {ts_api_result_t r={0};pack_failure(&r,TS_CONFIG_PACK_ERR_PARSE);sent_with_detail(r,"wire-pack-parse.json",TS_API_ERR_INVALID_ARG,"pack_parse_failed","JSON parse error");}
    {ts_api_result_t r={0};pack_failure(&r,TS_CONFIG_PACK_ERR_PERMISSION);sent_with_detail(r,"wire-pack-not-allowed.json",TS_API_ERR_NO_PERMISSION,"pack_not_allowed","Permission denied (not a developer device)");}
    {ts_api_result_t r={0};pack_failure(&r,TS_CONFIG_PACK_ERR_NO_MEM);assert(r.code==TS_API_ERR_NO_MEM&&!strcmp(r.message,"no_memory"));ts_api_result_free(&r);}
    {ts_api_result_t r={0};pack_failure(&r,TS_CONFIG_PACK_ERR_IO);assert(r.code==TS_API_ERR_INTERNAL&&!strcmp(r.message,"pack_io_failed"));ts_api_result_free(&r);}
    for(int pr=TS_CONFIG_PACK_ERR_PARSE;pr<=TS_CONFIG_PACK_ERR_NOT_INIT;pr++) {
        const char *token=ts_config_pack_reason((ts_config_pack_result_t)pr);
        assert(token&&token[0]&&strcmp(token,"pack_verify_failed"));    /* every real failure has its own token */
        for(const char *c=token;*c;c++) assert((*c>='a'&&*c<='z')||(*c>='0'&&*c<='9')||*c=='_');
    }
    assert(!strcmp(ts_config_pack_reason((ts_config_pack_result_t)99),"pack_verify_failed"));
    /* every refusal state of the engine has a token, and the token depends only on the state and on start / pause */
    assert(!strcmp(engine_refusal_token(TS_AUTO_STATE_PAUSED,false),"engine_paused")&&!strcmp(engine_refusal_token(TS_AUTO_STATE_PAUSED,true),"engine_paused"));
    assert(!strcmp(engine_refusal_token(TS_AUTO_STATE_RUNNING,true),"engine_unavailable"));   /* running but stopping */
    assert(!strcmp(engine_refusal_token(TS_AUTO_STATE_ERROR,true),"engine_not_running")&&!strcmp(engine_refusal_token(TS_AUTO_STATE_UNINITIALIZED,false),"engine_unavailable"));
    /* success */
    rule_reply(ESP_OK,"ok",1,1,1,"wire-rule-ok.json",TS_API_OK);
    rule_reply(ESP_OK,"mirror_failed",1,1,0,"wire-rule-ok-mirror.json",TS_API_OK);
    /* the ways a save / enable / disable can be refused: all use the INVALID_ARG class, the reason is the token */
    rule_reply(ESP_ERR_INVALID_STATE,"execution_busy",0,0,0,"wire-rule-execution-busy.json",TS_API_ERR_INVALID_ARG);
    rule_reply(ESP_ERR_INVALID_STATE,"revision_conflict",0,0,0,"wire-rule-revision-conflict.json",TS_API_ERR_INVALID_ARG);
    rule_reply(ESP_ERR_INVALID_STATE,"service_busy",0,0,0,"wire-rule-service-busy.json",TS_API_ERR_INVALID_ARG);
    rule_reply(ESP_FAIL,"commit_unknown",-1,-1,-1,"wire-rule-commit-unknown.json",TS_API_ERR_INVALID_ARG);
    rule_reply(ESP_ERR_NO_MEM,"capacity",0,0,0,"wire-rule-capacity.json",TS_API_ERR_INVALID_ARG);
    {   /* a failed commit with no code at all must not look like success */
        ts_rule_commit_result_t commit={0};ts_api_result_t r={0};rule_commit_reply(&r,ESP_FAIL,&commit);
        assert(r.code!=TS_API_OK && !strcmp(r.message,"invalid_configuration"));ts_api_result_free(&r);
    }
    /* manual trigger */
    trigger(ESP_OK,TS_RULE_REJECT_NONE,"wire-trigger-ok.json",TS_API_OK,"Rule triggered");
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_NOT_ACCEPTING,"wire-trigger-engine-not-accepting.json",TS_API_ERR_BUSY,"engine_not_accepting");
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_DISABLED,"wire-trigger-rule-disabled.json",TS_API_ERR_INTERNAL,"rule_disabled");
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_MANUAL_NOT_ALLOWED,"wire-trigger-manual-not-allowed.json",TS_API_ERR_INTERNAL,"manual_not_allowed");
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_PENDING_DELETE,"wire-trigger-rule-pending-delete.json",TS_API_ERR_INTERNAL,"rule_pending_delete");
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_RUNNING,"wire-trigger-execution-busy.json",TS_API_ERR_BUSY,"execution_busy");
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_SERVICE_STATE,"wire-trigger-service-not-admissible.json",TS_API_ERR_INTERNAL,"service_not_admissible");
    trigger(ESP_ERR_NOT_FOUND,TS_RULE_REJECT_NONE,"wire-trigger-not-found.json",TS_API_ERR_NOT_FOUND,"Rule not found");     /* unchanged text */
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_NONE,"wire-trigger-generic.json",TS_API_ERR_INTERNAL,"Failed to trigger rule"); /* no reason known */
    /* every refusal the engine can report has a token, and none is reported for a call that was not refused */
    for(int why=TS_RULE_REJECT_NOT_ACCEPTING;why<=TS_RULE_REJECT_PENDING_DELETE;why++) {
        assert(trigger_reject_token((ts_rule_reject_t)why)!=NULL);
        assert(trigger_reject_code((ts_rule_reject_t)why)==TS_API_ERR_BUSY||trigger_reject_code((ts_rule_reject_t)why)==TS_API_ERR_INTERNAL);
    }
    assert(trigger_reject_token(TS_RULE_REJECT_NONE)==NULL);
    free(http_body);
    puts("PASS production rule save/toggle replies and manual-trigger refusals through the HTTP response chain: reason tokens, data, codes");
}
'''
includes = ['tests/runtime/stubs', 'tests/certificate/stubs', 'components/ts_automation/include', 'components/ts_security/include',
            'components/ts_config_pack/include', 'components/ts_api/include', str(idf / 'components/json/cJSON')]
env = {**os.environ, 'DEVELOPER_DIR': '/Library/Developer/CommandLineTools', 'WIRE_DIR': str(wire_dir)}
for stale in wire_dir.glob('*.json'):
    stale.unlink()
file = build / 'failure_wire.c'
file.write_text(code)
subprocess.run(['cc', '-std=c11', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                *[f'-I{x}' for x in includes], str(file), str(idf / 'components/json/cJSON/cJSON.c'),
                '-lm', '-o', str(build / 'failure_wire')], check=True, env=env)
subprocess.run([str(build / 'failure_wire')], check=True, env=env)

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
        print('  If the wire change is intentional: UPDATE_FIXTURES=1 python3 tests/runtime/test_failure_wire.py')
        status = 1
if status:
    sys.exit(status)
print('PASS wire fixtures match: ' + ', '.join(CAPTURES))
