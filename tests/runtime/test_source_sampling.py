"""Actual REST/poll/publication paths with local HTTP HAL and a controlled clock."""
import argparse
import re
from pathlib import Path
from test_console_lifecycle import RTOS, IDF, body, run_native

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root',type=Path,default=Path('.'))
args=parser.parse_args()
MANAGER='components/ts_automation/src/ts_source_manager.c'
RUNTIME='components/ts_automation/src/ts_source_runtime.c'
def source(path):
    chosen=args.source_root/path
    return chosen if chosen.exists() else Path(path)
def function(path,name):return body(source(path),name)
rtos=re.sub(r'static int64_t esp_timer_get_time\(void\) \{[^\n]+\}',
            'static int64_t clock_ms=100000; static int64_t esp_timer_get_time(void) {return clock_ms*1000;}',RTOS)
code=rtos+r'''
#include "ts_source_runtime.h"
#include "ts_api.h"
#include "cJSON.h"
#define TAG "local-source"
#define CONFIG_TS_AUTOMATION_MAX_SOURCES 4
#define ESP_ERR_INVALID_RESPONSE 0x108
#define TS_STRDUP_PSRAM strdup
static ts_source_runtime_t ts_test_runtime;
#define ts_source_runtime ts_test_runtime
static struct {atomic_bool activation_pending;} s_sio_ctx;
static int activation_calls,variable_reads,http_calls,http_status=200;
static int64_t request_duration;static void (*during_read)(void);
static const char *http_payload="{\"temperature\":21}";
static int http_offset;static char actual_api_name[64];static cJSON *actual_params;
static bool fail_query_copy;
static esp_err_t api_hosts_info(const cJSON *,ts_api_result_t *);
static char *query_copy(const char *s) {if(fail_query_copy)return NULL;return strdup(s);}
typedef void *esp_http_client_handle_t;
enum {HTTP_METHOD_GET,HTTP_METHOD_POST};
typedef struct {const char *url;int method,timeout_ms,buffer_size;bool skip_cert_common_name_check;int (*crt_bundle_attach)(void*);} esp_http_client_config_t;
static int esp_crt_bundle_attach(void *p){(void)p;return ESP_OK;}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c){assert(c->url&&c->method==HTTP_METHOD_GET);++http_calls;http_offset=0;return (void*)1;}
static int esp_http_client_set_header(void *c,const char *n,const char *v){(void)c;(void)n;(void)v;return ESP_OK;}
static int esp_http_client_open(void *c,int n){(void)c;(void)n;return ESP_OK;}
static int esp_http_client_fetch_headers(void *c){(void)c;clock_ms+=request_duration;return 0;}
static int esp_http_client_get_status_code(void *c){(void)c;return http_status;}
static int esp_http_client_read(void *c,char *out,int n){(void)c;if(during_read){void(*hook)(void)=during_read;during_read=NULL;hook();}size_t len=strlen(http_payload);assert(n>=(int)len);if(http_offset)return 0;memcpy(out,http_payload,len);http_offset=(int)len;return (int)len;}
static int esp_http_client_close(void *c){(void)c;return ESP_OK;}
static int esp_http_client_cleanup(void *c){(void)c;return ESP_OK;}
static void ts_variable_registration_begin(void){}
static void ts_variable_registration_end(void){}
static int process_source_mappings(ts_auto_source_t *s,cJSON *j){(void)s;(void)j;assert(false);return 0;}
static int auto_discover_json_fields(ts_auto_source_t *s,cJSON *j,const char *p,int d,bool c){(void)s;(void)j;(void)p;(void)d;(void)c;assert(false);return 0;}
static cJSON *ts_jsonpath_get(cJSON *j,const char *p){return cJSON_Duplicate(cJSON_GetObjectItem(j,p),true);}
static esp_err_t read_variable_source(ts_auto_source_t *s,ts_auto_value_t *v){(void)s;++variable_reads;*v=(ts_auto_value_t){.type=TS_AUTO_VAL_INT,.int_val=7};return ESP_OK;}
void ts_source_config_activate(void){++activation_calls;}
void ts_source_transport_changed(void){}
esp_err_t ts_api_call(const char *name,const cJSON *p,ts_api_result_t *r){strcpy(actual_api_name,name);cJSON_Delete(actual_params);actual_params=cJSON_Duplicate(p,true);return api_hosts_info(p,r);}
'''
known=Path('components/ts_security/include/ts_known_hosts.h').read_text()
a=known.index('typedef enum {');b=known.index('} ts_known_host_t;')+len('} ts_known_host_t;')
code+=known[a:b]+'\n'
code+=r'''
static esp_err_t ts_known_hosts_get(const char *host,int port,ts_known_host_t *out){assert(!strcmp(host,"192.0.2.8")&&port==2222);*out=(ts_known_host_t){.port=port,.type=TS_HOST_KEY_ED25519};strcpy(out->host,host);strcpy(out->fingerprint,"synthetic-local-fingerprint");return ESP_OK;}
static const char *ts_host_key_type_str(ts_host_key_type_t type){assert(type==TS_HOST_KEY_ED25519);return "ed25519";}
'''
for name in ['ts_api_result_ok','ts_api_result_error']:
    code+=body('components/ts_api/src/ts_api.c',name)
code+=body('components/ts_api/src/ts_api_hosts.c','api_hosts_info')
for name in ['find_source_index','current_source_index','extract_json_value','cjson_to_value']:
    code+=function(MANAGER,name)
code+=body('components/ts_api/src/ts_api.c','ts_api_parse_query')
code+='#define strdup query_copy\n'+function(MANAGER,'read_rest_source')+'\n#undef strdup\n'
for name in ['poll_instance','ts_source_poll_all']:
    code+=function(MANAGER,name)
code+=re.search(r'^esp_err_t ts_source_poll\(.*$',source(MANAGER).read_text(),re.M).group(0)+'\n'
runtime=source(RUNTIME).read_text();a=runtime.index('struct ts_source_config_view {');b=runtime.index('\n};',a)+3
code+='typedef struct ts_source_config_view ts_source_config_view_t;\n'+runtime[a:b]+'\n'
code+=function('components/ts_automation/src/ts_action_codec.c','ts_source_config_same')
for name in ['ts_source_config_discard','ts_source_config_publish']:
    code+=function(RUNTIME,name)
code+=r'''
static ts_auto_source_t *sample;
static void reset(void){memset(ts_source_runtime.sources,0,4*sizeof(*sample));ts_source_runtime.count=1;ts_source_runtime.running=true;sample=ts_source_runtime.sources;*sample=(ts_auto_source_t){.type=TS_AUTO_SRC_REST,.enabled=true,.poll_interval_ms=60000,.instance=1};strcpy(sample->id,"temperature");strcpy(sample->rest.url,"http://fixture.invalid/read");strcpy(sample->rest.path,"temperature");clock_ms=100000;http_calls=variable_reads=0;http_status=200;request_duration=0;http_payload="{\"temperature\":21}";}
static void publish(bool retained){ts_source_config_view_t *v=calloc(1,sizeof(*v));v->count=1;v->keep_runtime=retained;v->sources=calloc(1,sizeof(*v->sources));*v->sources=*sample;if(!retained){v->sources[0].instance=sample->instance+1;v->sources[0].last_update_ms=0;v->sources[0].last_attempt_ms=0;v->sources[0].last_value=(ts_auto_value_t){0};strcpy(v->sources[0].rest.url,"http://fixture.invalid/new");}ts_source_config_publish(v);}
static void unchanged(int64_t attempt,int64_t success,int value){assert(sample->last_attempt_ms==attempt&&sample->last_update_ms==success&&sample->last_value.int_val==value);}
int main(void){ts_source_runtime.sources=calloc(4,sizeof(*sample));ts_source_runtime.capacity=4;ts_source_runtime.initialized=true;ts_source_runtime.mutex=xSemaphoreCreateMutex();
 reset();http_status=503;assert(ts_source_poll_all()==0&&http_calls==1);clock_ms+=100;assert(ts_source_poll_all()==0&&http_calls==1);
 reset();assert(ts_source_poll_all()==1&&http_calls==1);unchanged(100000,100000,21);
 http_status=503;for(clock_ms=100100;clock_ms<160000;clock_ms+=100)assert(ts_source_poll_all()==0&&http_calls==1);assert(ts_source_poll_all()==0&&http_calls==2);unchanged(160000,100000,21);
 publish(true);unchanged(160000,100000,21);for(clock_ms=160100;clock_ms<220000;clock_ms+=100)assert(ts_source_poll_all()==0&&http_calls==2);
 http_status=200;http_payload="invalid JSON";assert(ts_source_poll_all()==0&&http_calls==3);unchanged(220000,100000,21);
 clock_ms=279900;assert(ts_source_poll_all()==0&&http_calls==3);clock_ms=280000;http_payload="{\"temperature\":32}";assert(ts_source_poll_all()==1&&http_calls==4);unchanged(280000,280000,32);
 /* Slow successes retain the existing completion-to-next-attempt interval. */
 clock_ms=340000;request_duration=9000;assert(ts_source_poll_all()==1&&clock_ms==349000);unchanged(349000,349000,32);clock_ms=400000;assert(ts_source_poll_all()==0&&http_calls==5);clock_ms=409000;assert(ts_source_poll_all()==1&&http_calls==6);
 request_duration=0;clock_ms=418100;assert(ts_source_poll(sample->id)==ESP_OK&&http_calls==7);unchanged(418100,418100,32);
 sample->poll_interval_ms=0;assert(ts_source_poll_all()==1&&ts_source_poll_all()==1&&http_calls==9);
 reset();sample->type=TS_AUTO_SRC_VARIABLE;assert(ts_source_poll_all()==1&&variable_reads==1&&!http_calls);clock_ms+=100;assert(ts_source_poll_all()==0&&variable_reads==1);clock_ms=160000;assert(ts_source_poll_all()==1&&variable_reads==2);
 reset();assert(ts_source_poll_all()==1);clock_ms+=100;publish(false);assert(sample->instance==2&&!sample->last_attempt_ms);assert(ts_source_poll_all()==1&&http_calls==2);
 /* Late old response passes through actual REST and publication owner guards. */
 clock_ms+=60000;http_payload="{\"temperature\":90}";
 /* Explicit replacement while the HTTP HAL read is in flight. */
 extern void replace_source(void);during_read=replace_source;assert(ts_source_poll_all()==0&&sample->instance==3);unchanged(0,0,0);
 reset();strcpy(sample->rest.url,"http://127.0.0.1/api/v1/hosts/info?host=192.0.2.8&label=A+B%252E&n=12&port=2222");strcpy(sample->rest.path,"port");assert(ts_source_poll_all()==1&&!http_calls&&sample->last_value.int_val==2222);assert(!strcmp(actual_api_name,"hosts.info"));assert(!strcmp(cJSON_GetObjectItem(actual_params,"host")->valuestring,"192.0.2.8"));assert(!strcmp(cJSON_GetObjectItem(actual_params,"label")->valuestring,"A B%2E"));assert(cJSON_IsNumber(cJSON_GetObjectItem(actual_params,"n"))&&cJSON_GetObjectItem(actual_params,"n")->valueint==12);
 fail_query_copy=true;clock_ms+=60000;assert(ts_source_poll_all()==0&&sample->last_update_ms==100000&&sample->last_attempt_ms==160000);fail_query_copy=false;
 cJSON_Delete(actual_params);free(ts_source_runtime.sources);vSemaphoreDelete(ts_source_runtime.mutex);puts("PASS actual REST/poll/publication: failure intervals, success freshness, slow completion cadence, manual/zero/VARIABLE, retained/new/stale instances, actual local API query receiver and allocation failure");}
void replace_source(void){publish(false);}
'''
headers={p.name:p.read_text() for p in [Path('components/ts_automation/include/ts_automation_types.h'),Path('components/ts_automation/include/ts_source_manager.h'),Path('components/ts_automation/src/ts_source_runtime.h')]}
headers.update({'freertos/FreeRTOS.h':'#pragma once\n','freertos/semphr.h':'#pragma once\n'})
run_native(code,[IDF/'components/json/cJSON/cJSON.c'],headers)
