#!/usr/bin/env python3
"""Production filter codecs and LED execution; unified object API persistence is checked by run_config_storage.sh."""
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
#include "ts_action_codec.h"
#include "ts_rule_codec.h"
#include "ts_led.h"
#include "ts_storage.h"
#include <strings.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#define TAG "test"
void *heap_caps_malloc(size_t n,unsigned caps) { (void)caps;return malloc(n); }
void *heap_caps_calloc(size_t n,size_t size,unsigned caps) { (void)caps;return calloc(n,size); }
static int led_calls, cli_calls;
static ts_led_effect_config_t applied;
void ts_api_result_error(ts_api_result_t *r,ts_api_result_code_t c,const char *s) { r->code=c;r->message=strdup(s); }
void ts_api_result_reason(ts_api_result_t *r,ts_api_result_code_t c,const char *s,const char *d) { ts_api_result_error(r,c,s);if(d&&d[0]){r->data=cJSON_CreateObject();cJSON_AddStringToObject(r->data,"detail",d);} }
void ts_api_result_ok(ts_api_result_t *r,cJSON *j) { r->code=TS_API_OK;r->data=j; }
void ts_api_result_free(ts_api_result_t *r) { free(r->message);cJSON_Delete(r->data);memset(r,0,sizeof(*r)); }
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
code += r'''
static FILE *resource_open(const char *path,const char *mode) {const char *root=getenv("RESOURCE_DIR");assert(root&&!strncmp(path,"/sdcard/",8));char full[512];snprintf(full,sizeof(full),"%s%s",root,path+7);return fopen(full,mode);}
'''
code += '#define fopen resource_open\n' + extract(Path('components/ts_storage/src/ts_storage.c'),'ts_storage_read_file') + '\n#undef fopen\n'
code += extract(manager,'require_action_resource')
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
static esp_err_t run_resource_action(const ts_auto_action_led_t *led_final,ts_action_result_t *result) {
 const char *device_name=led_final->device;esp_err_t ret=ESP_OK;result->reason=NULL;
 switch(led_final->ctrl_type) {
'''
resource_start=source.index('        case TS_LED_CTRL_TEXT:')
resource_end=source.index('        case TS_LED_CTRL_QRCODE:',resource_start)
code += source[resource_start:resource_end]
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
static void check_codec(cJSON *input) {
 ts_action_template_t original,decoded;
 char *json=cJSON_PrintUnformatted(input);assert(json&&ts_action_template_decode(json,&original)==ESP_OK);free(json);
 json=ts_action_template_encode(&original);assert(json&&ts_action_template_decode(json,&decoded)==ESP_OK);free(json);same_filter(&original,&decoded);
 ts_action_result_t result={0};assert(run_filter(&decoded.action.led,&result)==ESP_OK);
 ts_led_effect_config_t automatic=applied;ts_api_result_t r={0};
 cJSON *manual=cJSON_CreateObject();cJSON_AddStringToObject(manual,"device","led_matrix");
 cJSON_AddStringToObject(manual,"filter",decoded.action.led.filter);
 assert(ts_action_filter_api_params(&decoded.action.led.filter_params,manual));
 assert(api_led_filter_start(manual,&r)==ESP_OK);assert(!memcmp(&automatic,&applied,sizeof(applied)));
 ts_api_result_free(&r);cJSON_Delete(manual);
}

int main(void) {
 cJSON *input=cJSON_Parse("{\"id\":\"fixture\",\"name\":\"original\",\"type\":\"led\",\"async\":true,\"enabled\":false,\"led\":{\"device\":\"led_matrix\",\"ctrl_type\":\"filter\",\"filter\":\"wave\",\"filter_params\":{\"speed\":40,\"angle\":0,\"amplitude\":0,\"wavelength\":8}}}");
 check_codec(input);assert(applied.params.wave.angle==0&&applied.params.wave.amplitude==0&&applied.params.wave.speed==40);
 const char *cases[]={"{\"intensity\":0,\"frequency\":0}","{\"intensity\":255,\"frequency\":100}","{\"saturation\":0,\"speed\":50}","{\"density\":0,\"decay\":0,\"speed\":5}","{\"amount\":0}","{\"speed\":30}"};
 const char *names[]={"glitch","glitch","rainbow","sparkle","contrast","fade-in"};
 cJSON *led=cJSON_GetObjectItem(input,"led");
 for(unsigned i=0;i<6;i++) { cJSON_ReplaceItemInObject(led,"filter",cJSON_CreateString(names[i]));cJSON_ReplaceItemInObject(led,"filter_params",cJSON_Parse(cases[i]));check_codec(input);
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
 /* Resource failures keep their own reason/path and do not execute a substitute. */
 ts_auto_action_led_t media={.ctrl_type=TS_LED_CTRL_IMAGE};strcpy(media.device,"led_matrix");
 strcpy(media.image_path,"/sdcard/images/missing.png");int calls=cli_calls;
 assert(run_resource_action(&media,&result)==ESP_ERR_NOT_FOUND&&cli_calls==calls);
 assert(!strcmp(result.reason,"action_image_unavailable")&&!strcmp(result.output,media.image_path));
 strcpy(media.image_path,"/sdcard/images/available.png");
 assert(run_resource_action(&media,&result)==ESP_OK&&!result.reason&&cli_calls==calls+1);
 media.ctrl_type=TS_LED_CTRL_TEXT;strcpy(media.text,"text");strcpy(media.font,"missing");calls=cli_calls;
 assert(run_resource_action(&media,&result)==ESP_ERR_NOT_FOUND&&cli_calls==calls);
 assert(!strcmp(result.reason,"action_font_unavailable")&&!strcmp(result.output,"/sdcard/fonts/missing.fnt"));
 media.font[0]=0;
 assert(run_resource_action(&media,&result)==ESP_OK&&!result.reason&&cli_calls==calls+1);
 /* Inline LED actions also survive the real rule codec. */
 ts_action_template_t next;char *last=cJSON_PrintUnformatted(input);assert(last&&ts_action_template_decode(last,&next)==ESP_OK);free(last);
 ts_auto_action_t action={.type=TS_AUTO_ACT_LED};action.led=next.action.led;
 ts_auto_rule_t rule={.actions=&action,.action_count=1};strcpy(rule.id,"rule");strcpy(rule.name,"Rule");cJSON *encoded=ts_rule_encode(&rule);assert(encoded);ts_auto_rule_t decoded;assert(ts_rule_decode(encoded,&decoded)==ESP_OK);assert(!memcmp(&decoded.actions[0].led.filter_params,&action.led.filter_params,sizeof(params)));ts_rule_dispose(&decoded);cJSON_Delete(encoded);
 cJSON_Delete(input);
 printf("PASS input repair: production codecs and LED filter API, missing image/font reasons before execution, seven filter configurations, zero/units/legacy defaults, inline rule codec; filter params=%zu LED action=%zu action=%zu template=%zu\n",sizeof(params),sizeof(ts_auto_action_led_t),sizeof(ts_auto_action_t),sizeof(ts_action_template_t));
}
'''
with tempfile.TemporaryDirectory(prefix='tianshan-input-repair-') as tmp:
    directory = Path(tmp)
    (directory/'images').mkdir();(directory/'fonts').mkdir()
    (directory/'images/available.png').write_bytes(b'fixture')
    (directory/'fonts/pixel9x9.fnt').write_bytes(b'fixture')
    source = directory/'test.c'; binary = directory/'test'
    source.write_text(code)
    includes = ['tests/runtime/stubs','tests/certificate/stubs','components/ts_api/include',
                'components/ts_automation/include','components/ts_security/include',
                'components/ts_led/include','components/ts_storage/include','components/ts_automation/src',str(idf/'components/json/cJSON')]
    env = {**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools','RESOURCE_DIR':str(directory)}
    subprocess.run(['cc','-std=gnu11','-g','-Wno-deprecated-declarations','-fsanitize=address,undefined',
                    *['-I'+p for p in includes],str(source),
                    'components/ts_automation/src/ts_action_codec.c','components/ts_automation/src/ts_action_filter.c',
                    'components/ts_automation/src/ts_rule_codec.c',str(idf/'components/json/cJSON/cJSON.c'),
                    '-lpthread','-lm','-o',str(binary)],check=True,env=env)
    subprocess.run([str(binary)],check=True,env=env)
