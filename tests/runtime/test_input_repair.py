#!/usr/bin/env python3
"""Exercise production template/API/filter code with local storage and LED stubs."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
os.chdir(root)
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
manager = Path('components/ts_automation/src/ts_action_manager.c')
api = Path('components/ts_api/src/ts_api_automation.c')
led_api = Path('components/ts_api/src/ts_api_led.c')

def extract(path, name):
    source = path.read_text()
    match = re.search(r'^(?:static )?(?:\w+[ \t]+)+[ \t*]*' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.start()) + 2] + '\n'

code = r'''
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "ts_api.h"
#include "ts_action_manager.h"
#include "ts_action_filter.h"
#include "ts_action_store.h"
#include "ts_rule_codec.h"
#include "ts_led.h"
#include <strings.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#define TAG "test"
#define TS_MALLOC_PSRAM heap_alloc
#define NVS_NAMESPACE "actions"
#define NVS_KEY_PREFIX "tpl_"
static bool fail_heap, mounted;
static int nv_failure, sd_failure, nv_stage, sd_writes, adds, updates, led_calls, cli_calls;
static char *nv_pending, *nv_saved;
static ts_led_effect_config_t applied;
static void *heap_alloc(size_t n) { return fail_heap ? NULL : malloc(n); }
void *heap_caps_malloc(size_t n, unsigned c) { return heap_alloc(n); }
void *heap_caps_calloc(size_t n, size_t s, unsigned c) { return calloc(n,s); }
static bool ts_storage_sd_mounted(void) { return mounted; }
esp_err_t nvs_open(const char *s, int m, nvs_handle_t *h) { nv_stage=1;*h=1;return nv_failure==1?ESP_FAIL:ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *s) { assert(!strcmp(k,"tpl_0"));nv_stage=2;if(nv_failure==2)return ESP_FAIL;nv_pending=strdup(s);return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { nv_stage=3;if(nv_failure==3)return ESP_FAIL;free(nv_saved);nv_saved=nv_pending;nv_pending=NULL;return ESP_OK; }
void nvs_close(nvs_handle_t h) { free(nv_pending);nv_pending=NULL; }
static esp_err_t ensure_actions_dir(void) { return sd_failure?ESP_FAIL:ESP_OK; }
static struct { ts_action_template_t templates[2]; int template_count; SemaphoreHandle_t templates_mutex; } storage;
static typeof(storage) *s_ctx = &storage;
void ts_api_result_error(ts_api_result_t *r,ts_api_result_code_t c,const char *s) { r->code=c;r->message=strdup(s); }
void ts_api_result_ok(ts_api_result_t *r,cJSON *j) { r->code=TS_API_OK;r->data=j; }
void ts_api_result_free(ts_api_result_t *r) { free(r->message);cJSON_Delete(r->data);memset(r,0,sizeof(*r)); }
esp_err_t ts_action_template_get(const char *id,ts_action_template_t *out) { if(strcmp(id,s_ctx->templates[0].id))return ESP_ERR_NOT_FOUND;*out=s_ctx->templates[0];return ESP_OK; }
esp_err_t ts_action_template_add(const ts_action_template_t *t) { ++adds;s_ctx->templates[0]=*t;return ESP_OK; }
static const char *resolve_device_name(const char *n) { return n; }
ts_led_device_t ts_led_device_get(const char *n) { return (ts_led_device_t)1; }
ts_led_layer_t ts_led_layer_get(ts_led_device_t d,uint8_t i) { return (ts_led_layer_t)1; }
esp_err_t ts_led_layer_set_effect(ts_led_layer_t l,const ts_led_effect_config_t *c) { applied=*c;++led_calls;return ESP_OK; }
void ts_led_preset_set_current_filter(const char *d,const char *f,int s) {}
void ts_led_preset_set_current_filter_config(const char *d,const ts_led_effect_config_t *c) {}
static int ts_console_exec(const char *cmd,void *out) { assert(strlen(cmd)<128);++cli_calls;return ESP_OK; }
'''
# The public API signatures remain real; only hardware/persistence interfaces are stubbed.
code += extract(manager, 'ts_action_parse_color')
for name in ['action_type_to_str', 'str_to_action_type', 'template_to_json',
             'json_to_template', 'export_template_to_file', 'template_update_impl']:
    code += extract(manager, name)
code += r'''
esp_err_t ts_action_template_update(const char *id,const ts_action_template_t *t) { ++updates;return template_update_impl(id,t); }
'''
for name in ['action_type_from_string', 'action_type_to_string',
             'api_automation_actions_write', 'api_automation_actions_get',
             'action_template_to_export_json']:
    code += extract(api, name)
code += extract(led_api, 'filter_name_to_type')
code += extract(led_api, 'api_led_filter_start')
code += r'''
esp_err_t ts_api_call(const char *endpoint,const cJSON *params,ts_api_result_t *r) {
 assert(!strcmp(endpoint,"led.filter.start"));return api_led_filter_start(params,r);
}
static esp_err_t run_filter(const ts_auto_action_led_t *led_final,ts_action_result_t *result) {
 const char *device_name=led_final->device;esp_err_t ret=ESP_OK;
 switch(led_final->ctrl_type) {
'''
source = manager.read_text()
start = source.index('        case TS_LED_CTRL_FILTER:')
end = source.index('        case TS_LED_CTRL_FILTER_STOP:', start)
code += source[start:end]
code += r'''
 default: assert(0);
 } return ret;
}
static void same_filter(const ts_action_template_t *a,const ts_action_template_t *b) {
 assert(a->action.led.ctrl_type==b->action.led.ctrl_type);
 assert(!strcmp(a->action.led.filter,b->action.led.filter));
 assert(!memcmp(&a->action.led.filter_params,&b->action.led.filter_params,sizeof(a->action.led.filter_params)));
 assert(a->async==b->async && a->enabled==b->enabled);
}
static void check_api(cJSON *input) {
 ts_api_result_t r={0};assert(api_automation_actions_write(input,&r,false)==ESP_OK&&r.code==TS_API_OK);ts_api_result_free(&r);
 ts_action_template_t original=s_ctx->templates[0],decoded;
 char *json=template_to_json(&original);assert(json&&json_to_template(json,&decoded)==ESP_OK);free(json);same_filter(&original,&decoded);
 assert(api_automation_actions_get(input,&r)==ESP_OK&&r.code==TS_API_OK);
 assert(cJSON_HasObjectItem(cJSON_GetObjectItem(r.data,"led"),"filter_params"));
 ts_api_result_t write_result={0};assert(api_automation_actions_write(r.data,&write_result,false)==ESP_OK);ts_api_result_free(&write_result);
 same_filter(&original,&s_ctx->templates[0]);
 ts_api_result_free(&r);
 cJSON *exported=action_template_to_export_json(&original);assert(exported);
 json=cJSON_PrintUnformatted(exported);assert(json_to_template(json,&decoded)==ESP_OK);same_filter(&original,&decoded);
 free(json);cJSON_Delete(exported);
 ts_action_result_t result={0};assert(run_filter(&decoded.action.led,&result)==ESP_OK);
 ts_led_effect_config_t automatic=applied;
 cJSON *manual=cJSON_CreateObject();cJSON_AddStringToObject(manual,"device","led_matrix");
 cJSON_AddStringToObject(manual,"filter",decoded.action.led.filter);
 assert(ts_action_filter_api_params(&decoded.action.led.filter_params,manual));
 assert(api_led_filter_start(manual,&r)==ESP_OK);assert(!memcmp(&automatic,&applied,sizeof(applied)));
 ts_api_result_free(&r);cJSON_Delete(manual);
}
int main(void) {
 pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;s_ctx->templates_mutex=&lock;s_ctx->template_count=2;
 cJSON *input=cJSON_Parse("{\"id\":\"fixture\",\"name\":\"original\",\"type\":\"led\",\"async\":true,\"enabled\":false,\"led\":{\"device\":\"led_matrix\",\"ctrl_type\":\"filter\",\"filter\":\"wave\",\"filter_params\":{\"speed\":40,\"angle\":0,\"amplitude\":0,\"wavelength\":8}}}");
 check_api(input);assert(applied.params.wave.angle==0&&applied.params.wave.amplitude==0&&applied.params.wave.speed==40);
 const char *cases[]={"{\"intensity\":0,\"frequency\":0}","{\"intensity\":255,\"frequency\":100}","{\"saturation\":0,\"speed\":50}","{\"density\":0,\"decay\":0,\"speed\":5}","{\"amount\":0}","{\"speed\":30}"};
 const char *names[]={"glitch","glitch","rainbow","sparkle","contrast","fade-in"};
 cJSON *led=cJSON_GetObjectItem(input,"led");
 for(unsigned i=0;i<6;i++) { cJSON_ReplaceItemInObject(led,"filter",cJSON_CreateString(names[i]));cJSON_ReplaceItemInObject(led,"filter_params",cJSON_Parse(cases[i]));check_api(input);
  if(i==0)assert(applied.params.glitch.frequency==0&&applied.params.glitch.intensity==0);
  if(i==1)assert(applied.params.glitch.frequency==255);
  if(i==2)assert(applied.params.rainbow.saturation==0);
  if(i==3)assert(applied.params.sparkle.density==0&&applied.params.sparkle.decay==0);
  if(i==4)assert(applied.params.contrast.amount==-50);
  if(i==5)assert(applied.params.fade.duration_ms==(uint16_t)(1000.0f/(0.2f+29*4.8f/99)));
 }
 ts_auto_filter_params_t params={0},unchanged;
 const char *invalid[]={"[]","null","{\"angle\":361}","{\"speed\":0}","{\"amplitude\":-1}","{\"speed\":1.5}","{\"angle\":\"0\"}","{\"unknown\":1}","{\"speed\":1,\"speed\":2}"};
 for(unsigned i=0;i<9;i++) { cJSON *bad=cJSON_Parse(invalid[i]);params.present=1;params.values[0]=50;unchanged=params;assert(ts_action_filter_decode(bad,&params)==ESP_ERR_INVALID_ARG);assert(!memcmp(&params,&unchanged,sizeof(params)));cJSON_Delete(bad); }
 assert(ts_action_filter_decode(NULL,&params)==ESP_OK&&!params.present);
 ts_auto_action_led_t legacy={.ctrl_type=TS_LED_CTRL_FILTER};strcpy(legacy.device,"led_matrix");strcpy(legacy.filter,"wave");ts_action_result_t result={0};assert(run_filter(&legacy,&result)==ESP_OK&&cli_calls==1);
 strcpy(legacy.filter,"stop");assert(run_filter(&legacy,&result)==ESP_OK&&cli_calls==2);
 /* Preserve old template and unrelated slot on allocation/open/write/commit failures. */
 ts_action_template_t original=s_ctx->templates[0],next=original;strcpy(next.name,"updated");
 original.created_at=123;original.use_count=7;original.last_used_at=456;s_ctx->templates[0]=original;
 strcpy(s_ctx->templates[1].id,"unrelated");ts_action_template_t unrelated=s_ctx->templates[1];
 nv_saved=template_to_json(&original);
 for(int fault=0;fault<=3;fault++) { fail_heap=fault==0;nv_failure=fault;assert(template_update_impl("fixture",&next)!=ESP_OK);assert(!memcmp(&s_ctx->templates[0],&original,sizeof(original)));assert(!memcmp(&s_ctx->templates[1],&unrelated,sizeof(unrelated)));ts_action_template_t disk;assert(json_to_template(nv_saved,&disk)==ESP_OK&&!strcmp(disk.name,"original")); }
 fail_heap=false;nv_failure=0;mounted=true;sd_failure=1;assert(template_update_impl("fixture",&next)!=ESP_OK);assert(!memcmp(&s_ctx->templates[0],&original,sizeof(original)));
 ts_action_template_t old_backup;assert(json_to_template(nv_saved,&old_backup)==ESP_OK&&!strcmp(old_backup.name,"original"));
 sd_failure=0;char encrypted[256];snprintf(encrypted,sizeof(encrypted),"%s/fixture.tscfg",ACTIONS_SDCARD_DIR);FILE *f=fopen(encrypted,"w");assert(f);fputs("synthetic imported version",f);fclose(f);
 assert(template_update_impl("fixture",&next)==ESP_OK);assert(access(encrypted,F_OK)!=0&&errno==ENOENT);
 assert(s_ctx->templates[0].created_at==123&&s_ctx->templates[0].use_count==7&&s_ctx->templates[0].last_used_at==456);
 char path[256],text[4096];snprintf(path,sizeof(path),"%s/fixture.json",ACTIONS_SDCARD_DIR);f=fopen(path,"r");assert(f);size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
 ts_action_template_t disk;assert(json_to_template(text,&disk)==ESP_OK);same_filter(&disk,&next);assert(!strcmp(disk.name,"updated"));
 /* SD remains authoritative even if its NVS backup fails. */
 nv_failure=3;strcpy(next.name,"SD durable");assert(template_update_impl("fixture",&next)==ESP_OK);assert(!strcmp(s_ctx->templates[0].name,"SD durable"));
 f=fopen(path,"r");assert(f);n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);assert(json_to_template(text,&disk)==ESP_OK&&!strcmp(disk.name,"SD durable"));nv_failure=0;
 cJSON_ReplaceItemInObject(led,"filter_params",cJSON_Parse("{\"speed\":0}"));ts_api_result_t r={0};int before=updates;assert(api_automation_actions_write(input,&r,true)==ESP_ERR_INVALID_ARG&&r.code==TS_API_ERR_INVALID_ARG&&updates==before);ts_api_result_free(&r);
 /* Inline LED actions also survive the real rule codec. */
 ts_auto_action_t action={.type=TS_AUTO_ACT_LED};action.led=next.action.led;
 ts_auto_rule_t rule={.actions=&action,.action_count=1};strcpy(rule.id,"rule");strcpy(rule.name,"Rule");cJSON *encoded=ts_rule_encode(&rule);assert(encoded);ts_auto_rule_t decoded;assert(ts_rule_decode(encoded,&decoded)==ESP_OK);assert(!memcmp(&decoded.actions[0].led.filter_params,&action.led.filter_params,sizeof(params)));ts_rule_dispose(&decoded);cJSON_Delete(encoded);
 cJSON_Delete(input);free(nv_saved);
 printf("PASS input repair: real API/readback/export/import, NVS and SD reload, failed edit preservation, seven filter execution configurations, zero/units/legacy defaults, inline rule codec; filter params=%zu LED action=%zu action=%zu template=%zu\n",sizeof(params),sizeof(ts_auto_action_led_t),sizeof(ts_auto_action_t),sizeof(ts_action_template_t));
}
'''
with tempfile.TemporaryDirectory(prefix='tianshan-input-repair-') as tmp:
    directory = Path(tmp); (directory/'actions').mkdir()
    source = directory/'test.c'; binary = directory/'test'
    code = '#define ACTIONS_SDCARD_DIR "' + str(directory/'actions') + '"\n' + code
    source.write_text(code)
    includes = ['tests/runtime/stubs','tests/certificate/stubs','components/ts_api/include',
                'components/ts_automation/include','components/ts_security/include',
                'components/ts_led/include','components/ts_automation/src',str(idf/'components/json/cJSON')]
    env = {**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
    subprocess.run(['cc','-std=gnu11','-g','-Wno-deprecated-declarations','-fsanitize=address,undefined',
                    *['-I'+p for p in includes],str(source),
                    '-DTS_ACTIONS_DIR="'+str(directory/'actions')+'"',
                    'components/ts_automation/src/ts_action_store.c','components/ts_automation/src/ts_action_filter.c',
                    'components/ts_automation/src/ts_rule_codec.c',str(idf/'components/json/cJSON/cJSON.c'),
                    '-lpthread','-lm','-o',str(binary)],check=True,env=env)
    subprocess.run([str(binary)],check=True,env=env)
