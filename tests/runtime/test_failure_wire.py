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
            'wire-trigger-manual-not-allowed.json', 'wire-trigger-execution-busy.json', 'wire-trigger-service-not-admissible.json',
            'wire-trigger-not-found.json', 'wire-trigger-generic.json']


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
#define ESP_LOGI(...) ((void)0)
#define TS_MALLOC_PSRAM malloc
#define TS_STRDUP_PSRAM strdup
static const char *error_name(esp_err_t e) {return e==ESP_OK?"ESP_OK":"ESP_FAIL";}
#define esp_err_to_name error_name
static ts_rule_reject_t mock_why = TS_RULE_REJECT_NONE;
static esp_err_t mock_ret = ESP_OK;
esp_err_t ts_rule_trigger_ex(const char *id, ts_rule_reject_t *why) {if(why)*why=mock_why;return mock_ret;}
'''
for name in ['ts_api_result_free', 'ts_api_result_ok', 'ts_api_result_error']:
    code += extract('components/ts_api/src/ts_api.c', name)
for name in ['rule_commit_reply', 'trigger_reject_token', 'trigger_reject_code', 'api_automation_rules_trigger']:
    code += extract('components/ts_api/src/ts_api_automation.c', name)
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
int main(void) {
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
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_RUNNING,"wire-trigger-execution-busy.json",TS_API_ERR_BUSY,"execution_busy");
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_SERVICE_STATE,"wire-trigger-service-not-admissible.json",TS_API_ERR_INTERNAL,"service_not_admissible");
    trigger(ESP_ERR_NOT_FOUND,TS_RULE_REJECT_NONE,"wire-trigger-not-found.json",TS_API_ERR_NOT_FOUND,"Rule not found");     /* unchanged text */
    trigger(ESP_ERR_INVALID_STATE,TS_RULE_REJECT_NONE,"wire-trigger-generic.json",TS_API_ERR_INTERNAL,"Failed to trigger rule"); /* no reason known */
    /* every refusal the engine can report has a token, and none is reported for a call that was not refused */
    for(int why=TS_RULE_REJECT_NOT_ACCEPTING;why<=TS_RULE_REJECT_CHANGED;why++) {
        assert(trigger_reject_token((ts_rule_reject_t)why)!=NULL);
        assert(trigger_reject_code((ts_rule_reject_t)why)==TS_API_ERR_BUSY||trigger_reject_code((ts_rule_reject_t)why)==TS_API_ERR_INTERNAL);
    }
    assert(trigger_reject_token(TS_RULE_REJECT_NONE)==NULL);
    free(http_body);
    puts("PASS production rule save/toggle replies and manual-trigger refusals through the HTTP response chain: reason tokens, data, codes");
}
'''
includes = ['tests/runtime/stubs', 'tests/certificate/stubs', 'components/ts_automation/include', 'components/ts_security/include',
            'components/ts_api/include', str(idf / 'components/json/cJSON')]
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
