/* Actual owner, file replacement, runtime views, variable table and rule
 * admission. Network transports, PKI and the NVS driver are boundary stubs. */
#define main file_fixture_main
#include "ts_log.h"
#include "test_automation_file.c"
#undef main
#include "../../components/ts_automation/src/ts_automation_config.c"
#include "ts_variable.h"
#include "ts_action_manager.h"
#include "ts_source_runtime.h"
#include "ts_rule_engine.h"
#include "ts_rule_pack.h"
#include "ts_automation.h"
#include "ts_crypto.h"
#include "nvs.h"
#include <math.h>
#include "ts_action_codec.h"
#include "esp_timer.h"
#include "ts_jsonpath.h"
#include "ts_api.h"
#ifndef ESP_ERR_INVALID_RESPONSE
#define ESP_ERR_INVALID_RESPONSE 0x108
#endif
static uint8_t durable_control[4],pending_control[4];
static bool has_control,fail_control,control_dirty,auto_v_dirty;
static bool has_auto_v=true,sd_present=true,sd_mounted=true,internal_mounted=true;
static uint8_t auto_v=1,pending_auto_v;
static unsigned sd_probes,legacy_reads;
static const cJSON *legacy_document;
static StaticSemaphore_t sd_gate_storage;
static SemaphoreHandle_t sd_gate;
static uint64_t sd_generation=1;
void ts_storage_sd_lock(void){if(!sd_gate)sd_gate=xSemaphoreCreateRecursiveMutexStatic(&sd_gate_storage);xSemaphoreTakeRecursive(sd_gate,portMAX_DELAY);}
void ts_storage_sd_unlock(void){xSemaphoreGiveRecursive(sd_gate);}
uint64_t ts_storage_sd_generation(void){return sd_generation;}
bool ts_storage_sd_mounted(void){return sd_mounted;}
bool ts_storage_auto_mounted(void){return internal_mounted;}
esp_err_t ts_storage_sd_refresh(void)
{ ++sd_probes;if(sd_mounted!=sd_present){sd_mounted=sd_present;++sd_generation;}return sd_present?ESP_OK:ESP_ERR_NOT_FOUND; }
esp_err_t ts_config_pack_init(void){return ESP_OK;}
esp_err_t ts_auto_legacy_read(bool sd,cJSON **json,ts_auto_config_error_t *error)
{
    (void)sd;(void)error;++legacy_reads;
    ts_auto_document_t empty={.enabled=true,.migration_version=1};
    *json=legacy_document?cJSON_Duplicate(legacy_document,true):ts_auto_document_encode(&empty);
    return *json?ESP_OK:ESP_ERR_NO_MEM;
}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *handle)
{ assert(!strcmp(name,"ts_cfg_state"));if(!has_control&&!has_auto_v&&mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;*handle=1;return ESP_OK; }
void nvs_close(nvs_handle_t handle){assert(handle==1);control_dirty=auto_v_dirty=false;}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char *name,void *out,size_t *size)
{ assert(handle==1&&!strcmp(name,"auto_control"));if(!has_control)return ESP_ERR_NVS_NOT_FOUND;assert(*size>=4);memcpy(out,durable_control,4);*size=4;return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t handle,const char *name,const void *in,size_t size)
{ assert(handle==1&&!strcmp(name,"auto_control")&&size==4);memcpy(pending_control,in,4);control_dirty=true;return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle==1);if(fail_control)return ESP_FAIL;
    if(control_dirty){memcpy(durable_control,pending_control,4);has_control=true;control_dirty=false;}
    if(auto_v_dirty){auto_v=pending_auto_v;has_auto_v=true;auto_v_dirty=false;}
    return ESP_OK;
}
esp_err_t nvs_open_from_partition(const char *partition,const char *name,int mode,nvs_handle_t *handle)
{ (void)name;(void)handle;assert(!strcmp(partition,"keys_nvs")&&mode==NVS_READONLY&&has_auto_v&&auto_v==1);return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_u8(nvs_handle_t handle,const char *name,uint8_t *value)
{ assert(handle==1&&!strcmp(name,"auto_v"));if(!has_auto_v)return ESP_ERR_NVS_NOT_FOUND;*value=auto_v;return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t handle,const char *name,uint8_t value)
{ assert(handle==1&&!strcmp(name,"auto_v"));pending_auto_v=value;auto_v_dirty=true;return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t handle){(void)handle;assert(false);return ESP_FAIL;}
typedef void *esp_websocket_client_handle_t;
typedef void *esp_http_client_handle_t;
enum {HTTP_METHOD_GET,HTTP_METHOD_POST};
typedef struct {const char *url;int method,timeout_ms,buffer_size;bool skip_cert_common_name_check;esp_err_t (*crt_bundle_attach)(void *);} esp_http_client_config_t;
static int http_method,http_calls,local_api_calls,http_offset;
static bool http_bundle,http_skip_name;
static char http_url[256];
static const char *http_payload="{\"temperature\":21}";
static char http_authorization[128];
static esp_err_t esp_crt_bundle_attach(void *p){(void)p;return ESP_OK;}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{http_method=cfg->method;++http_calls;http_offset=0;http_authorization[0]=0;
 http_bundle=cfg->crt_bundle_attach==esp_crt_bundle_attach;http_skip_name=cfg->skip_cert_common_name_check;
 snprintf(http_url,sizeof(http_url),"%s",cfg->url);return (void*)1;}
static esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *name,const char *value)
{(void)c;assert(!strcmp(name,"Authorization"));snprintf(http_authorization,sizeof(http_authorization),"%s",value);return ESP_OK;}
static esp_err_t esp_http_client_open(esp_http_client_handle_t c,int n){(void)c;(void)n;return ESP_OK;}
static int esp_http_client_fetch_headers(esp_http_client_handle_t c){(void)c;return 0;}
static int esp_http_client_get_status_code(esp_http_client_handle_t c){(void)c;return 200;}
static int esp_http_client_read(esp_http_client_handle_t c,char *out,int size)
{(void)c;const char *data=http_payload;int length=(int)strlen(data);if(http_offset)return 0;assert(size>=length);memcpy(out,data,length);http_offset=length;return length;}
static esp_err_t esp_http_client_close(esp_http_client_handle_t c){(void)c;return ESP_OK;}
static esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c){(void)c;return ESP_OK;}
esp_err_t ts_api_call(const char *name,const cJSON *params,ts_api_result_t *result)
{(void)params;assert(!strcmp(name,"example.status"));++local_api_calls;result->code=0;result->data=cJSON_Parse("{\"temperature\":21}");return ESP_OK;}
typedef const char *esp_event_base_t;
typedef struct {char *data_ptr;int data_len,payload_len,payload_offset;bool fin;uint8_t op_code;} esp_websocket_event_data_t;
typedef struct {const char *uri;int buffer_size,reconnect_timeout_ms,network_timeout_ms,ping_interval_sec;bool enable_close_reconnect;esp_err_t (*crt_bundle_attach)(void *);} esp_websocket_client_config_t;
enum {WEBSOCKET_EVENT_CONNECTED=1,WEBSOCKET_EVENT_DISCONNECTED,WEBSOCKET_EVENT_DATA,WEBSOCKET_EVENT_ERROR,WEBSOCKET_EVENT_CLOSED};
#define SIO_MSG_BUF_SIZE 8192
#define SIO_PROBE_MSG "2probe"
#define SIO_UPGRADE_MSG "5"
#define SIO_PING_MSG "2"
#define SIO_PONG_MSG "3"
static int esp_websocket_client_send_text(esp_websocket_client_handle_t c,const char *data,int len,int wait)
{(void)c;(void)data;(void)len;(void)wait;return len;}
#define SIO_SID_MAX_LEN 64
#ifndef CONFIG_TS_AUTOMATION_MAX_SOURCES
#define CONFIG_TS_AUTOMATION_MAX_SOURCES 16
#endif
#define SIO_MAX_CONNECTIONS CONFIG_TS_AUTOMATION_MAX_SOURCES
#define SIO_EVENT_PREFIX "42"
static unsigned stop_notifications;
static int xTaskNotifyGive(TaskHandle_t task) { assert(task);++stop_notifications;return 1; }
typedef int BaseType_t;
#define pdPASS 1
static unsigned stream_tasks_created;
static void sio_connection_task(void *arg) {(void)arg;assert(false);}
static BaseType_t xTaskCreateWithCaps(void (*task)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out,unsigned caps)
{assert(task==sio_connection_task&&name&&stack==6144&&arg&&priority==5&&caps);*out=(void *)1;++stream_tasks_created;return pdPASS;}
#include "source_transport.inc"
void ts_source_config_activate(void) { /* Task creation and real networking are outside this fixture. */ }

static int64_t clock_us=100000;
int64_t esp_timer_get_time(void) { return ++clock_us; }
bool ts_action_manager_is_initialized(void) { return true; }
static unsigned template_executions;
static esp_err_t template_execution_ret=ESP_OK;
static ts_action_result_t template_outcome={.status=TS_ACTION_STATUS_SUCCESS};
esp_err_t ts_action_manager_execute(const ts_auto_action_t *action,ts_action_result_t *out)
{ (void)action;++template_executions;if(out)*out=template_outcome;return template_execution_ret; }
esp_err_t ts_action_queue(const ts_auto_action_t *action,ts_action_callback_t callback,void *context,uint8_t priority)
{ (void)action;(void)callback;(void)context;(void)priority;return ESP_OK; }

static atomic_bool refuse_actions, refuse_service;
static const char *preflight_reason="ssh_connect_timeout";static unsigned preflight_checks;
static pthread_mutex_t execution_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t execution_changed=PTHREAD_COND_INITIALIZER;
static bool block_action,action_reached,action_released;
static unsigned rule_actions;
bool ts_action_manager_accepting(void) { return !refuse_actions; }
esp_err_t ts_action_manager_check_start(const ts_auto_action_t *a,ts_action_result_t *result,bool automatic) {(void)automatic;if(a->type==TS_AUTO_ACT_SSH_CMD_REF){++preflight_checks;if(refuse_service){result->reason=preflight_reason;return ESP_ERR_INVALID_STATE;}}return ESP_OK;}
esp_err_t ts_ssh_service_check_start(const char *id,const ts_ssh_host_config_t *host,const char *exclude,int64_t deadline,const char **reason)
{(void)id;(void)host;(void)exclude;(void)deadline;if(reason)*reason=NULL;return refuse_service?ESP_ERR_INVALID_STATE:ESP_OK;}
esp_err_t ts_action_submit_background(const ts_auto_action_t *action,ts_action_result_t *result)
{return ts_action_manager_execute(action,result);}
static bool fail_service_import;static unsigned service_imports;
esp_err_t ts_ssh_service_import_legacy(const ts_ssh_command_config_t *command,const ts_ssh_host_config_t *host)
{assert(command&&host&&command->nohup);++service_imports;return fail_service_import?ESP_FAIL:ESP_OK;}
typedef void *QueueHandle_t;
#include "action_identity.inc"
static esp_err_t execute_auto_sequence(const ts_auto_action_t *,int,int *,int *,unsigned *,ts_action_result_t *);
static esp_err_t execute_actions_with_stats(const ts_auto_action_t *actions,int count,int *ok,int *failed,unsigned *resume,ts_action_result_t *result)
{
    if(resume)return execute_auto_sequence(actions,count,ok,failed,resume,result);
    (void)actions;(void)result;pthread_mutex_lock(&execution_gate);
    if(block_action){action_reached=true;pthread_cond_broadcast(&execution_changed);while(!action_released)pthread_cond_wait(&execution_changed,&execution_gate);}
    pthread_mutex_unlock(&execution_gate);rule_actions+=count;if(resume)*resume=count;*ok=count;*failed=0;return ESP_OK;
}

static bool fail_prepare;
static unsigned prepares, publishes, pauses;
static StaticSemaphore_t admission_storage;
static SemaphoreHandle_t admission;
void esp_fill_random(void *buffer,size_t size) {
    static unsigned sequence;
    unsigned char *bytes=buffer; ++sequence;
    for(size_t i=0;i<size;++i)bytes[i]=(unsigned char)(sequence*37+i);
}
void ts_ssh_binding_lock(void) { if(!admission)admission=xSemaphoreCreateRecursiveMutexStatic(&admission_storage);xSemaphoreTakeRecursive(admission,portMAX_DELAY); }
void ts_ssh_binding_unlock(void) { xSemaphoreGiveRecursive(admission); }
#define ts_rule_config_prepare engine_prepare
#define ts_rule_config_discard engine_discard
#define ts_rule_config_publish engine_publish
#define ts_rule_config_pause engine_pause
#include "rule_runtime.inc"
#undef ts_rule_config_prepare
#undef ts_rule_config_discard
#undef ts_rule_config_publish
#undef ts_rule_config_pause
esp_err_t ts_rule_config_prepare(const ts_auto_rule_t *rules,int count,bool keep_runtime,ts_rule_config_view_t **out)
{ ++prepares;*out=NULL;return fail_prepare?ESP_ERR_NO_MEM:engine_prepare(rules,count,keep_runtime,out); }
void ts_rule_config_discard(ts_rule_config_view_t *view) { engine_discard(view); }
void ts_rule_config_publish(ts_rule_config_view_t *view,bool enabled,const ts_auto_document_t *before,const ts_auto_document_t *after) { ++publishes;engine_publish(view,enabled,before,after); }
void ts_rule_config_pause(void) { ++pauses;engine_pause(); }


static bool reject_package;
cJSON *ts_config_pack_parse_json(const char *text,size_t length)
{ return cJSON_ParseWithLengthOpts(text,length+1,NULL,true); }
ts_config_pack_result_t ts_config_pack_load_verified_mem(const char *text,size_t length,
    const ts_config_pack_acceptance_t *accepted,ts_config_pack_t **out,ts_config_pack_acceptance_t *result,uint32_t *generation)
{
    assert(!accepted);
    if(reject_package)return TS_CONFIG_PACK_ERR_SIGNATURE;
    *out=calloc(1,sizeof(**out));assert(*out);(*out)->content=strndup(text,length);(*out)->content_len=length;
    if(result){
        memset(result,0,sizeof(*result));result->policy=1;result->accepted_at=1;
        unsigned char hash[32];assert(ts_crypto_hash(TS_HASH_SHA256,text,length,hash,32)==ESP_OK);
        assert(ts_crypto_hex_encode(hash,32,result->package_sha256,65)==ESP_OK);
        memset(result->signer_sha256,'1',64);memset(result->root_sha256,'2',64);memset(result->recipient,'3',64);
    }
    if(generation)*generation=1;
    return TS_CONFIG_PACK_OK;
}
ts_config_pack_result_t ts_config_pack_load_mem(const char *text,size_t length,ts_config_pack_t **out)
{
    return ts_config_pack_load_verified_mem(text,length,NULL,out,NULL,NULL);
}
void ts_config_pack_free(ts_config_pack_t *pack){if(pack){free(pack->content);free(pack);}}
bool ts_cert_material_begin(uint32_t generation){return generation==1;}
void ts_cert_material_end(void){}

#define CONFIG_TS_AUTOMATION_TASK_STACK_SIZE 8192
#define CONFIG_TS_AUTOMATION_TASK_PRIORITY 5
#define pdMS_TO_TICKS(n) (n)
#define pdPASS 1
typedef int BaseType_t;
static struct {ts_automation_state_t state;atomic_bool stopping,deinitializing;TaskHandle_t task_handle;SemaphoreHandle_t mutex;unsigned rule_triggers;} top_ctx;
static atomic_bool async_pending;
static unsigned source_stops;
esp_err_t ts_action_manager_quiesce(void){refuse_actions=true;s_ctx->accepting=false;return async_pending?ESP_ERR_TIMEOUT:ESP_OK;}
esp_err_t ts_action_manager_resume(void){if(async_pending)return ESP_ERR_INVALID_STATE;refuse_actions=false;s_ctx->accepting=true;return ESP_OK;}
esp_err_t ts_source_start_all(void){return ESP_OK;}
esp_err_t ts_source_stop_all(void){++source_stops;return ESP_OK;}
int ts_source_poll_all(void){return 0;}
static void (*delay_hook)(void);
static void vTaskDelay(unsigned ms){if(delay_hook){void(*hook)(void)=delay_hook;delay_hook=NULL;hook();}(void)ms;struct timespec delay={0,1000000};nanosleep(&delay,NULL);}
static void vTaskDelete(void *task){assert(!task);pthread_exit(NULL);}
static int xTaskCreatePinnedToCore(void(*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,void **handle,int core)
{(void)fn;(void)name;(void)stack;(void)arg;(void)priority;(void)core;*handle=(void*)1;return pdPASS;}
static void automation_task(void *arg);
#include "automation_task.inc"

static const char *esp_err_to_name(esp_err_t error){(void)error;return "mock platform error";}
static unsigned execution_io;
static void (*execution_hook)(void);
typedef struct {int code;char *message;void *data;size_t data_size;} ts_cmd_result_t;
static esp_err_t ts_console_exec(const char *command,ts_cmd_result_t *result)
{
    (void)command;++execution_io;if(execution_hook)execution_hook();
    result->code=0;result->message=strdup("completed");return ESP_OK;
}
static esp_err_t executed(ts_action_result_t *result)
{++execution_io;result->status=TS_ACTION_STATUS_SUCCESS;return ESP_OK;}
static esp_err_t exec_ssh_bound(const ts_auto_action_ssh_t *ssh,const ts_action_ssh_host_t *host,ts_action_result_t *result)
{(void)ssh;assert(host);return executed(result);}
static esp_err_t execute_bound_ref(const ts_auto_action_t *action,ts_action_result_t *result)
{assert(action->runtime_binding);return executed(result);}
esp_err_t ts_action_exec_led(const ts_auto_action_led_t *action,ts_action_result_t *result){(void)action;return executed(result);}
esp_err_t ts_action_exec_gpio(const ts_auto_action_gpio_t *action,ts_action_result_t *result){(void)action;return executed(result);}
esp_err_t ts_action_exec_log(const ts_auto_action_log_t *action,ts_action_result_t *result){(void)action;return executed(result);}
esp_err_t ts_action_exec_device(const ts_auto_action_device_t *action,ts_action_result_t *result){(void)action;return executed(result);}
static bool fail_webhook;
static esp_err_t execute_webhook(const ts_auto_action_t *action){(void)action;if(fail_webhook){++execution_io;return ESP_FAIL;}ts_action_result_t result;return executed(&result);}
static esp_err_t execute_service_control(const ts_action_queue_entry_t *entry,ts_action_result_t *result){(void)entry;return executed(result);}
#include "action_execution.inc"
esp_err_t ts_action_submit_prepared(const ts_auto_action_t *action)
{ts_action_queue_entry_t entry={.action=*action};ts_action_result_t result={0};return execute_entry(&entry,&result);}
#include "rule_repeat.inc"
#include "rule_sequence.inc"
static void *run_automation(void *arg){automation_task(arg);return NULL;}

static ts_auto_config_status_t status(void)
{ ts_auto_config_status_t value;ts_auto_config_status(&value);return value; }
static esp_err_t write_object(ts_auto_config_kind_t kind,const char *raw,const char *version,
                              ts_auto_config_error_t *error)
{
    cJSON *value=cJSON_Parse(raw);assert(value);
    esp_err_t ret=ts_auto_config_set(kind,value,version,error);cJSON_Delete(value);return ret;
}

static void stop_cases(void)
{
    assert(write_object(TS_AUTO_CONFIG_RULE,"{\"id\":\"stop\",\"name\":\"Stop test\",\"enabled\":true,\"cooldown_ms\":0,\"conditions\":[{\"variable\":\"test.ready\",\"value\":true}],\"actions\":[{\"type\":\"log\",\"message\":\"run\"}]}",NULL,NULL)==ESP_OK);
    top_ctx.mutex=xSemaphoreCreateMutex();top_ctx.state=TS_AUTO_STATE_RUNNING;top_ctx.task_handle=(void*)1;
    block_action=true;action_reached=action_released=false;pthread_t task;
    assert(!pthread_create(&task,NULL,run_automation,NULL));
    pthread_mutex_lock(&execution_gate);while(!action_reached)pthread_cond_wait(&execution_changed,&execution_gate);pthread_mutex_unlock(&execution_gate);
    assert(ts_automation_stop()==ESP_ERR_TIMEOUT && ts_automation_start()==ESP_ERR_INVALID_STATE);
    int index=find_rule_index("stop");assert(index>=0 && s_rule_ctx.meta[index].executing);
    async_pending=true;
    pthread_mutex_lock(&execution_gate);action_released=true;pthread_cond_broadcast(&execution_changed);pthread_mutex_unlock(&execution_gate);
    assert(!pthread_join(task,NULL));block_action=false;
    assert(!s_rule_ctx.meta[index].executing && !((rule_payload_t*)s_rule_ctx.rules[index].lease)->refs);
    assert(ts_automation_stop()==ESP_ERR_TIMEOUT && ts_automation_start()==ESP_ERR_INVALID_STATE && !source_stops);
    async_pending=false;assert(ts_automation_stop()==ESP_OK && source_stops==1);
    assert(ts_automation_start()==ESP_OK && ts_action_manager_accepting());
    top_ctx.deinitializing=true;assert(ts_automation_start()==ESP_ERR_INVALID_STATE);top_ctx.deinitializing=false;
    top_ctx.task_handle=NULL;vSemaphoreDelete(top_ctx.mutex);top_ctx.mutex=NULL;
    assert(ts_rule_unregister("stop")==ESP_OK);
}

static void rule_publication_cases(void)
{
    ts_auto_rule_t *rules=calloc(64,sizeof(*rules));assert(rules);
    for(int i=0;i<64;++i){snprintf(rules[i].id,sizeof(rules[i].id),"r%d",i);strcpy(rules[i].name,"Original");}
    ts_rule_config_view_t *prepared=NULL;
    assert(engine_prepare(rules,65,true,&prepared)==ESP_ERR_INVALID_SIZE && !prepared);
    assert(engine_prepare(rules,64,true,&prepared)==ESP_OK);engine_publish(prepared,true,NULL,NULL);
    assert(ts_rule_count()==64);
    s_rule_ctx.rules[0].last_trigger_ms=77;s_rule_ctx.rules[0].trigger_count=2;
    ts_auto_rule_t held;assert(ts_rule_acquire("r0",&held)==ESP_OK);uint32_t identity=held.instance;
    strcpy(rules[1].name,"Changed");
    assert(engine_prepare(rules,64,true,&prepared)==ESP_OK);engine_publish(prepared,true,NULL,NULL);
    assert(s_rule_ctx.rules[0].instance==identity && s_rule_ctx.rules[0].last_trigger_ms==77 && s_rule_ctx.rules[0].trigger_count==2);
    assert(!strcmp(held.name,"Original") && s_rule_ctx.retired==1);ts_rule_release(&held);assert(!s_rule_ctx.retired);
    engine_pause();assert(!s_rule_ctx.config_enabled);free(rules);
}

static void rule_config_cases(void)
{
    ts_auto_rule_t rule={0};strcpy(rule.id,"aux");strcpy(rule.name,"First");
    ts_rule_commit_result_t result;
    assert(ts_rule_commit(&rule,rule.id,0,&result)==ESP_OK && result.applied==1 && result.durable==1);
    ts_auto_rule_t held;assert(ts_rule_acquire(rule.id,&held)==ESP_OK && held.revision==1);
    strcpy(rule.name,"Second");assert(ts_rule_commit(&rule,rule.id,1,&result)==ESP_OK && result.revision==2);
    ts_auto_rule_t current;assert(ts_rule_acquire(rule.id,&current)==ESP_OK && !strcmp(current.name,"Second"));ts_rule_release(&current);
    assert(!strcmp(held.name,"First"));assert(ts_rule_unregister(rule.id)==ESP_OK);
    assert(ts_rule_acquire(rule.id,&current)==ESP_ERR_NOT_FOUND);ts_rule_release(&held);
    ts_auto_variable_t ready={0};strcpy(ready.name,"test.ready");strcpy(ready.source_id,"test_sdk");
    ready.value.type=TS_AUTO_VAL_BOOL;ready.value.bool_val=true;assert(ts_variable_register(&ready)==ESP_OK);
    const struct {bool enabled,manual,allow;} triggers[]={
        {true,false,false}, /* Automatic rule with manual control disabled. */
        {true,true,true},   /* Manual rule never runs on evaluation. */
        {false,true,true},  /* Disabled rules cannot run. */
        {true,false,true}   /* Allowed for the following admission checks. */
    };
    for(unsigned i=0;i<sizeof(triggers)/sizeof(triggers[0]);++i){
        bool enabled=triggers[i].enabled,manual=triggers[i].manual,allow=triggers[i].allow;
        char raw[512];snprintf(raw,sizeof(raw),"{\"id\":\"eval\",\"name\":\"Evaluate\",\"enabled\":%s,\"manual_trigger\":%s,\"show_on_dashboard\":false,\"allow_manual_trigger\":%s,\"cooldown_ms\":0,\"conditions\":[{\"variable\":\"test.ready\",\"value\":true}],\"actions\":[{\"type\":\"log\",\"message\":\"run\"}]}",enabled?"true":"false",manual?"true":"false",allow?"true":"false");
        assert(write_object(TS_AUTO_CONFIG_RULE,raw,NULL,NULL)==ESP_OK);
        unsigned before=rule_actions;bool triggered=true;
        assert(ts_rule_evaluate("eval",&triggered)==ESP_OK && (rule_actions>before)==(enabled&&!manual));
        ts_rule_reject_t why=TS_RULE_REJECT_NONE;before=rule_actions;
        esp_err_t ret=ts_rule_trigger_ex("eval",&why);
        assert((ret==ESP_OK)==(enabled&&allow) && (rule_actions>before)==(enabled&&allow));
    }
    refuse_actions=true;ts_rule_reject_t why;assert(ts_rule_trigger_ex("eval",&why)==ESP_ERR_INVALID_STATE && why==TS_RULE_REJECT_NOT_ACCEPTING);refuse_actions=false;
    assert(ts_auto_config_enable(false,NULL,NULL)==ESP_OK);
    const ts_auto_config_view_t *paused_view=ts_auto_config_acquire();
    assert(ts_auto_config_definitions(paused_view)!=NULL);ts_auto_config_release(paused_view);
    assert(ts_rule_trigger_ex("eval",&why)==ESP_ERR_INVALID_STATE && why==TS_RULE_REJECT_PAUSED);
    assert(ts_auto_config_enable(true,NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_COMMAND,"{\"id\":\"svc\",\"host_id\":\"host\",\"name\":\"Service\",\"command\":\"true\"}",NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_RULE,"{\"id\":\"eval\",\"name\":\"Service\",\"enabled\":true,\"allow_manual_trigger\":true,\"manual_trigger\":true,\"actions\":[{\"type\":\"ssh_cmd_ref\",\"cmd_id\":\"svc\"}]}",NULL,NULL)==ESP_OK);
    refuse_service=true;assert(ts_rule_trigger_ex("eval",&why)==ESP_ERR_INVALID_STATE && why==TS_RULE_REJECT_SERVICE_STATE);refuse_service=false;
    assert(write_object(TS_AUTO_CONFIG_ACTION,"{\"id\":\"svc_template\",\"name\":\"Service template\",\"enabled\":true,\"type\":\"ssh_cmd_ref\",\"ssh_ref\":{\"cmd_id\":\"svc\"}}",NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_RULE,"{\"id\":\"eval\",\"name\":\"Service\",\"enabled\":true,\"allow_manual_trigger\":true,\"cooldown_ms\":5000,\"actions\":[{\"type\":\"log\",\"message\":\"must not run\"},{\"template_id\":\"svc_template\"}]}",NULL,NULL)==ESP_OK);
    unsigned actions_before=rule_actions;ts_auto_rule_t before_rule,after_rule;
    assert(ts_rule_acquire("eval",&before_rule)==ESP_OK);refuse_service=true;
    assert(ts_rule_trigger_ex("eval",&why)==ESP_ERR_INVALID_STATE&&why==TS_RULE_REJECT_SERVICE_STATE);
    bool fired=true;assert(ts_rule_evaluate("eval",&fired)==ESP_OK&&!fired);
    assert(ts_rule_acquire("eval",&after_rule)==ESP_OK&&rule_actions==actions_before);
    assert(before_rule.trigger_count==after_rule.trigger_count&&before_rule.last_trigger_ms==after_rule.last_trigger_ms);
    ts_rule_release(&before_rule);ts_rule_release(&after_rule);
    refuse_service=false;assert(ts_rule_trigger_ex("eval",&why)==ESP_OK&&rule_actions==actions_before+2);
    assert(ts_rule_unregister("eval")==ESP_OK);
    assert(ts_action_template_remove("svc_template")==ESP_OK);
    assert(ts_ssh_commands_config_remove("svc")==ESP_OK);
    const char *package="{\"type\":\"automation_rule\",\"rule\":{\"id\":\"imported\",\"name\":\"Imported\",\"enabled\":true,\"actions\":[{\"type\":\"log\",\"message\":\"imported\"}]}}";
    cJSON *preview=NULL,*applied=NULL;const char *reason=NULL;
    assert(ts_rule_import_pack(package,strlen(package),true,false,0,0,0,NULL,NULL,&preview,&reason)==ESP_OK);
    uint32_t rev=cJSON_GetObjectItem(preview,"expected_revision")->valuedouble;
    uint32_t generation=cJSON_GetObjectItem(preview,"expected_generation")->valuedouble;
    const char *digest=cJSON_GetObjectItem(preview,"package_digest")->valuestring;
    const char *version=cJSON_GetObjectItem(preview,"active_version")->valuestring;
    assert(ts_rule_import_pack(package,strlen(package),false,false,rev,generation,1,digest,version,&applied,&reason)==ESP_OK);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(applied,"runtime_applied")) && cJSON_IsFalse(cJSON_GetObjectItem(applied,"restart_required")));
    assert(ts_rule_acquire("imported",&current)==ESP_OK && current.enabled);ts_rule_release(&current);
    cJSON_Delete(applied);applied=NULL;
    assert(ts_rule_import_pack(package,strlen(package),false,true,rev,generation,1,digest,version,&applied,&reason)!=ESP_OK && !strcmp(reason,"configuration_changed"));
    cJSON *again=NULL;
    assert(ts_rule_import_pack(package,strlen(package),true,false,0,0,0,NULL,NULL,&again,&reason)==ESP_OK);
    ts_auto_config_status_t imported=status();
    assert(cJSON_IsTrue(cJSON_GetObjectItem(again,"unchanged")));
    assert(ts_rule_import_pack(package,strlen(package),false,false,
        cJSON_GetObjectItem(again,"expected_revision")->valuedouble,cJSON_GetObjectItem(again,"expected_generation")->valuedouble,1,
        cJSON_GetObjectItem(again,"package_digest")->valuestring,cJSON_GetObjectItem(again,"active_version")->valuestring,&applied,&reason)==ESP_OK);
    assert(!strcmp(reason,"no_change") && !strcmp(status().active_version,imported.active_version));
    cJSON_Delete(again);cJSON_Delete(applied);applied=NULL;
    const ts_auto_config_view_t *view=ts_auto_config_acquire();const cJSON *rows=cJSON_GetObjectItem(ts_auto_config_json(view),"rules");
    assert(cJSON_IsObject(cJSON_GetObjectItem(cJSON_GetArrayItem(rows,0),"_package")));ts_auto_config_release(view);
    assert(ts_rule_disable("imported")==ESP_OK);
    view=ts_auto_config_acquire();rows=cJSON_GetObjectItem(ts_auto_config_json(view),"rules");
    assert(!cJSON_GetObjectItem(cJSON_GetArrayItem(rows,0),"_package"));ts_auto_config_release(view);
    ts_auto_config_status_t before=status();reject_package=true;
    assert(ts_rule_import_pack(package,strlen(package),true,false,0,0,0,NULL,NULL,&applied,&reason)!=ESP_OK && !strcmp(reason,"signature_invalid"));reject_package=false;
    assert(!strcmp(status().active_version,before.active_version));
    assert(ts_rule_unregister("imported")==ESP_OK);cJSON_Delete(preview);
}

static void source_cases(void)
{
    char transport_url[256];
    ts_source_sio_websocket_url("https://example.invalid:443", "sid", transport_url, sizeof(transport_url));
    assert(!strcmp(transport_url,"wss://example.invalid:443/socket.io/?EIO=4&transport=websocket&sid=sid"));
    ts_source_sio_websocket_url("http://example.invalid:80", "sid", transport_url, sizeof(transport_url));
    assert(!strcmp(transport_url,"ws://example.invalid:80/socket.io/?EIO=4&transport=websocket&sid=sid"));
    char sid[64]={0};http_payload="0{\"sid\":\"fixture\"}";
    assert(sio_http_handshake("https://example.invalid:443",sid,sizeof(sid))==ESP_OK&&!strcmp(sid,"fixture"));
    assert(!strncmp(http_url,"https://",8)&&http_bundle&&!http_skip_name);
    http_payload="{\"temperature\":21}";
    ts_auto_source_t source={.enabled=true,.type=TS_AUTO_SRC_SOCKETIO};
    strcpy(source.id,"stream");strcpy(source.label,"Stream");
    strcpy(source.socketio.url,"http://192.0.2.10:9000");strcpy(source.socketio.event,"telemetry");
    assert(ts_source_register(&source)==ESP_OK);
    ts_auto_source_t first;assert(ts_source_get_copy("stream",&first)==ESP_OK && first.instance);
    strcpy(source.id,"other");assert(ts_source_register(&source)==ESP_OK);
    ts_auto_source_t other;assert(ts_source_get_copy("other",&other)==ESP_OK);
    sio_connection_t *old=&s_sio_ctx.connections[0],*stable=&s_sio_ctx.connections[1];
    strcpy(old->source_id,"stream");old->source_instance=first.instance;old->allocated=true;old->task_handle=(void*)1;
    old->pending_mutex=xSemaphoreCreateMutex();assert(old->pending_mutex);
    strcpy(stable->source_id,"other");stable->source_instance=other.instance;stable->allocated=true;stable->task_handle=(void*)2;
    stable->pending_mutex=xSemaphoreCreateMutex();assert(stable->pending_mutex);
    sio_handle_event_message(old,"42[\"telemetry\",{\"temperature\":20}]");
    assert(ts_source_get_copy("stream",&source)==ESP_OK && source.connected);
    ts_auto_variable_t observed={0};strcpy(observed.name,"stream.temperature");strcpy(observed.source_id,"stream");
    observed.value.type=TS_AUTO_VAL_INT;observed.value.int_val=20;assert(ts_variable_register(&observed)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_HOST,"{\"id\":\"host\",\"host\":\"192.0.2.9\",\"username\":\"operator\",\"port\":22}",NULL,NULL)==ESP_OK);
    assert(ts_source_get_copy("stream",&source)==ESP_OK && source.connected && source.instance==first.instance);
    assert(ts_variable_exists("stream.temperature"));
    strcpy(source.socketio.url,"http://192.0.2.11:9000");assert(ts_source_register(&source)==ESP_OK);
    assert(ts_source_get_copy("stream",&source)==ESP_OK && source.instance!=first.instance && !source.connected);
    assert(old->should_stop && !stable->should_stop && stop_notifications);
    assert(!ts_variable_exists("stream.temperature"));
    sio_handle_event_message(old,"42[\"telemetry\",{\"temperature\":99}]");
    ts_auto_source_t current;assert(ts_source_get_copy("stream",&current)==ESP_OK && !current.connected);
    vSemaphoreDelete(old->pending_mutex);old->pending_mutex=NULL;
    sio_release_connection(old,true);
    assert(!old->allocated && stable->allocated && !strcmp(stable->source_id,"other") && stable->source_instance==other.instance);
    old->source_instance=current.instance;old->should_stop=false;old->allocated=true;
    old->pending_mutex=xSemaphoreCreateMutex();assert(old->pending_mutex);
    sio_handle_event_message(old,"42[\"telemetry\",{\"temperature\":30}]");
    assert(ts_source_get_copy("stream",&current)==ESP_OK && current.connected);
    ts_source_runtime.closing=true;
    sio_handle_event_message(old,"42[\"telemetry\",{\"temperature\":31}]");
    assert(ts_source_get_copy("stream",&source)==ESP_OK && source.last_update_ms==current.last_update_ms);
    ts_source_runtime.closing=false;
    assert(ts_source_unregister("stream")==ESP_OK && ts_source_get_copy("stream",&current)==ESP_ERR_NOT_FOUND);
    sio_handle_event_message(old,"42[\"telemetry\",{\"temperature\":40}]");
    assert(ts_source_get_copy("other",&current)==ESP_OK && current.instance==other.instance && !current.connected);
    assert(ts_source_unregister("other")==ESP_OK);
    vSemaphoreDelete(old->pending_mutex);old->pending_mutex=NULL;sio_release_connection(old,false);
    vSemaphoreDelete(stable->pending_mutex);stable->pending_mutex=NULL;sio_release_connection(stable,false);

    ts_auto_source_t ws={.enabled=true,.type=TS_AUTO_SRC_WEBSOCKET,.mapping_count=1};
    strcpy(ws.id,"plain_ws");strcpy(ws.websocket.uri,"ws://fixture.invalid/socket");strcpy(ws.websocket.path,"data.temperature");
    strcpy(ws.mappings[0].json_path,"data.temperature");strcpy(ws.mappings[0].var_name,"plain_ws.temperature");
    assert(ts_source_register(&ws)==ESP_OK&&ts_source_get_copy(ws.id,&ws)==ESP_OK);
    unsigned before_stream=stream_tasks_created;
    assert(stream_connect(ws.id,TS_AUTO_SRC_WEBSOCKET)==ESP_OK&&stream_tasks_created==before_stream+1);
    sio_connection_t *stream=sio_find_connection(ws.id);
    assert(stream&&stream->source_instance==ws.instance&&stream->type==TS_AUTO_SRC_WEBSOCKET&&stream->pending_mutex);
    assert(stream_connect(ws.id,TS_AUTO_SRC_WEBSOCKET)==ESP_OK&&stream_tasks_created==before_stream+1);
    esp_websocket_client_config_t stream_config=test_stream_client_config(stream,3210,ws.websocket.uri);
    assert(stream_config.enable_close_reconnect&&stream_config.reconnect_timeout_ms==3210&&stream_config.crt_bundle_attach==esp_crt_bundle_attach);
    stream->type=TS_AUTO_SRC_SOCKETIO;stream_config=test_stream_client_config(stream,3210,ws.websocket.uri);
    assert(!stream_config.enable_close_reconnect&&stream_config.reconnect_timeout_ms==10000);stream->type=TS_AUTO_SRC_WEBSOCKET;
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_CONNECTED,NULL);
    assert(ts_source_get_copy(ws.id,&ws)==ESP_OK&&ws.connected);
    char message[]="{\"data\":{\"temperature\":23}}";int length=(int)strlen(message);
    esp_websocket_event_data_t part={.data_ptr=message,.data_len=8,.payload_len=length,.payload_offset=0,.fin=true,.op_code=1};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);
    part.data_ptr=message+8;part.data_len=length-8;part.payload_offset=8;
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);
    assert(stream->pending_json&&!strcmp(stream->pending_json,message));test_process_pending(stream);
    ts_auto_value_t mapped;assert(ts_variable_get("plain_ws.temperature",&mapped)==ESP_OK&&mapped.int_val==23);
    assert(ts_source_get_copy(ws.id,&ws)==ESP_OK&&ws.last_value.int_val==23);
    part=(esp_websocket_event_data_t){.data_ptr=message,.data_len=8,.payload_len=length,.payload_offset=0,.fin=true,.op_code=1};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);assert(stream->msg_buf_len==8);
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DISCONNECTED,NULL);
    assert(ts_source_get_copy(ws.id,&ws)==ESP_OK&&!ws.connected&&!stream->msg_buf_len);
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_CONNECTED,NULL);
    assert(ts_source_get_copy(ws.id,&ws)==ESP_OK&&ws.connected);
    char after_reconnect[]="{\"data\":{\"temperature\":24}}";
    part=(esp_websocket_event_data_t){.data_ptr=after_reconnect,.data_len=(int)strlen(after_reconnect),.payload_len=(int)strlen(after_reconnect),.fin=true,.op_code=1};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);test_process_pending(stream);
    assert(ts_variable_get("plain_ws.temperature",&mapped)==ESP_OK&&mapped.int_val==24);
    char framed[]="{\"data\":{\"temperature\":25}}";int framed_length=(int)strlen(framed);
    ws_test_ctx_t test={0};
    part=(esp_websocket_event_data_t){.data_ptr=framed,.data_len=8,.payload_len=8,.fin=false,.op_code=1};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);ws_test_handler(&test,NULL,WEBSOCKET_EVENT_DATA,&part);
    assert(!stream->pending_json&&!test.received);
    char ping[]="ping";esp_websocket_event_data_t control={.data_ptr=ping,.data_len=4,.payload_len=4,.fin=true,.op_code=9};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&control);ws_test_handler(&test,NULL,WEBSOCKET_EVENT_DATA,&control);
    part=(esp_websocket_event_data_t){.data_ptr=framed+8,.data_len=framed_length-8,.payload_len=framed_length-8,.fin=true,.op_code=0};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);ws_test_handler(&test,NULL,WEBSOCKET_EVENT_DATA,&part);
    assert(test.received&&!strcmp(test.message,framed));free(test.message);test_process_pending(stream);
    assert(ts_variable_get("plain_ws.temperature",&mapped)==ESP_OK&&mapped.int_val==25);
    /* A separate empty FIN completes the buffered message in both paths. */
    char empty_fin_message[]="{\"data\":{\"temperature\":26}}";
    int empty_fin_length=(int)strlen(empty_fin_message);
    test=(ws_test_ctx_t){0};
    part=(esp_websocket_event_data_t){.data_ptr=empty_fin_message,.data_len=empty_fin_length,.payload_len=empty_fin_length,.fin=false,.op_code=1};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);ws_test_handler(&test,NULL,WEBSOCKET_EVENT_DATA,&part);
    assert(!stream->pending_json&&!test.received);
    control=(esp_websocket_event_data_t){.fin=true,.op_code=9};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&control);ws_test_handler(&test,NULL,WEBSOCKET_EVENT_DATA,&control);
    assert(!stream->pending_json&&!test.received);
    part=(esp_websocket_event_data_t){.fin=true,.op_code=0};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);
    assert(stream->pending_json&&!strcmp(stream->pending_json,empty_fin_message));
    ws_test_handler(&test,NULL,WEBSOCKET_EVENT_DATA,&part);
    assert(test.received&&!strcmp(test.message,empty_fin_message));free(test.message);test_process_pending(stream);
    assert(ts_variable_get("plain_ws.temperature",&mapped)==ESP_OK&&mapped.int_val==26);
    char next_message[]="{\"data\":{\"temperature\":27}}";
    part=(esp_websocket_event_data_t){.data_ptr=next_message,.data_len=(int)strlen(next_message),.payload_len=(int)strlen(next_message),.fin=true,.op_code=1};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);test_process_pending(stream);
    assert(ts_variable_get("plain_ws.temperature",&mapped)==ESP_OK&&mapped.int_val==27);
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_CLOSED,NULL);
    assert(ts_source_get_copy(ws.id,&ws)==ESP_OK&&!ws.connected);
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_CONNECTED,NULL);
    assert(ts_source_get_copy(ws.id,&ws)==ESP_OK&&ws.connected);
    char stale[]="{\"data\":{\"temperature\":88}}";
    part=(esp_websocket_event_data_t){.data_ptr=stale,.data_len=(int)strlen(stale),.payload_len=(int)strlen(stale),.fin=true,.op_code=1};
    sio_websocket_event_handler(stream,NULL,WEBSOCKET_EVENT_DATA,&part);assert(stream->pending_json);
    strcpy(ws.websocket.uri,"ws://fixture.invalid/replacement");assert(ts_source_register(&ws)==ESP_OK&&stream->should_stop);
    ts_auto_variable_t fresh={0};strcpy(fresh.name,"plain_ws.temperature");strcpy(fresh.source_id,"plain_ws");
    fresh.value.type=TS_AUTO_VAL_INT;fresh.value.int_val=99;assert(ts_variable_upsert(&fresh)==ESP_OK);
    test_process_pending(stream);assert(ts_variable_get("plain_ws.temperature",&mapped)==ESP_OK&&mapped.int_val==99);
    assert(ts_source_unregister("plain_ws")==ESP_OK);
    free(stream->msg_buf);stream->msg_buf=NULL;vSemaphoreDelete(stream->pending_mutex);stream->pending_mutex=NULL;sio_release_connection(stream,false);
    for(unsigned i=0;i<5;++i){
        ts_auto_source_t concurrent={.enabled=true,.type=TS_AUTO_SRC_WEBSOCKET};
        snprintf(concurrent.id,sizeof(concurrent.id),"plain_%u",i);strcpy(concurrent.websocket.uri,"ws://fixture.invalid/socket");
        assert(ts_source_register(&concurrent)==ESP_OK&&stream_connect(concurrent.id,TS_AUTO_SRC_WEBSOCKET)==ESP_OK);
    }
    for(unsigned i=0;i<5;++i){
        char id[32];snprintf(id,sizeof(id),"plain_%u",i);sio_connection_t *conn=sio_find_connection(id);
        assert(conn&&conn->allocated&&ts_source_unregister(id)==ESP_OK&&conn->should_stop);
        vSemaphoreDelete(conn->pending_mutex);conn->pending_mutex=NULL;sio_release_connection(conn,false);
    }

    ts_auto_source_t rest={.enabled=true,.type=TS_AUTO_SRC_REST,.mapping_count=1};
    strcpy(rest.id,"rest_method");strcpy(rest.rest.url,"http://example.invalid/measure");strcpy(rest.rest.auth_header,"Bearer synthetic");
    strcpy(rest.mappings[0].json_path,"temperature");strcpy(rest.mappings[0].var_name,"rest_method.temperature");
    assert(ts_source_register(&rest)==ESP_OK&&ts_source_get_copy(rest.id,&rest)==ESP_OK);
    assert(read_rest_source(&rest,NULL)==ESP_OK&&http_method==HTTP_METHOD_GET&&!strcmp(http_authorization,"Bearer synthetic"));
    assert(ts_variable_get("rest_method.temperature",&mapped)==ESP_OK&&mapped.int_val==21);
    strcpy(rest.rest.method,"POST");assert(ts_source_register(&rest)==ESP_OK&&ts_source_get_copy(rest.id,&rest)==ESP_OK);
    assert(read_rest_source(&rest,NULL)==ESP_OK&&http_method==HTTP_METHOD_POST&&!strcmp(http_authorization,"Bearer synthetic"));
    assert(ts_variable_get("rest_method.temperature",&mapped)==ESP_OK&&mapped.int_val==21);
    int prior_http_calls=http_calls;strcpy(rest.rest.url,"http://127.0.0.1/api/v1/example/status");
    assert(ts_source_register(&rest)==ESP_OK&&ts_source_get_copy(rest.id,&rest)==ESP_OK);
    assert(read_rest_source(&rest,NULL)==ESP_OK&&http_calls==prior_http_calls&&local_api_calls==1);
    assert(ts_source_unregister("rest_method")==ESP_OK);

    cJSON *many=cJSON_CreateArray();assert(many);
    for(int i=0;i<TS_SOURCE_MAX_SIO_CONNECTIONS+1;++i) {
        snprintf(source.id,sizeof(source.id),"stream_%d",i);
        char *raw=ts_source_config_encode(&source);assert(raw);cJSON *row=cJSON_Parse(raw);free(raw);
        assert(row && cJSON_AddItemToArray(many,row));
    }
    ts_auto_config_error_t error={0};ts_auto_config_status_t before=status();
    assert(ts_auto_config_replace_collection(TS_AUTO_CONFIG_SOURCE,many,NULL,&error)==ESP_ERR_INVALID_SIZE);
    assert(!strcmp(error.reason,"source_connection_limit") && !strcmp(status().active_version,before.active_version));
    cJSON_Delete(many);
}
#include "ts_api.h"
#include "../../components/ts_api/src/ts_api_auto_config.h"
#define TS_STRDUP_PSRAM strdup
#include "api_result.inc"
#undef TS_STRDUP_PSRAM
#include "config_api.inc"
#include "test_automation_modes.inc"
#include "test_action_lifecycle.inc"
#include "test_config_api.inc"

static void automatic_start_cases(void)
{
    assert(write_object(TS_AUTO_CONFIG_COMMAND,"{\"id\":\"retry_cmd\",\"host_id\":\"host\",\"name\":\"Retry\",\"command\":\"true\",\"nohup\":true}",NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_RULE,"{\"id\":\"retry_rule\",\"name\":\"Retry\",\"enabled\":true,\"allow_manual_trigger\":true,\"cooldown_ms\":60000,\"conditions\":[{\"variable\":\"test.ready\",\"value\":true}],\"actions\":[{\"type\":\"log\",\"message\":\"prefix\"},{\"type\":\"ssh_cmd_ref\",\"cmd_id\":\"retry_cmd\"},{\"type\":\"log\",\"message\":\"suffix\"}]}",NULL,NULL)==ESP_OK);
    bool fired=false;ts_rule_start_status_t start;unsigned checked=preflight_checks;
    refuse_service=true;preflight_reason="ssh_connect_timeout";
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&!fired&&preflight_checks==checked+1);
    assert(ts_rule_start_status("retry_rule",&start)&&!strcmp(start.state,"waiting")&&start.retry_after_ms>29000);
    ts_auto_config_status_t before_unrelated=status();
    ts_auto_rule_t identity;assert(ts_rule_acquire("retry_rule",&identity)==ESP_OK);uint32_t original_instance=identity.instance;ts_rule_release(&identity);
    int64_t original_start=s_rule_ctx.meta[find_rule_index("retry_rule")].start.first_ms;
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"retry_unrelated\",\"source_id\":\"user\",\"value_type\":2,\"value\":1}",NULL,NULL)==ESP_OK);
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&!fired&&preflight_checks==checked+1);
    assert(s_rule_ctx.meta[find_rule_index("retry_rule")].start.first_ms==original_start);
    ts_rule_reject_t stale;const char *stale_reason=NULL;
    assert(ts_rule_trigger_versioned("retry_rule",before_unrelated.active_version,&stale,&stale_reason)==ESP_ERR_INVALID_STATE&&stale==TS_RULE_REJECT_CONFIG_VERSION);
    assert(s_rule_ctx.meta[find_rule_index("retry_rule")].start.first_ms==original_start);
    assert(ts_rule_acquire("retry_rule",&identity)==ESP_OK&&identity.instance==original_instance);ts_rule_release(&identity);
    for(int i=0;i<100;++i)assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&!fired);
    assert(preflight_checks==checked+1);clock_us+=30000000;
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&!fired&&preflight_checks==checked+2);
    clock_us+=330000000;
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&!fired&&preflight_checks==checked+2);
    assert(ts_rule_start_status("retry_rule",&start)&&!strcmp(start.state,"failed")&&!strcmp(start.reason,"automatic_start_timeout"));
    /* A false condition rearms. Permanent authentication failures do not retry. */
    assert(ts_variable_set_bool("test.ready",false)==ESP_OK);assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK);
    assert(ts_variable_set_bool("test.ready",true)==ESP_OK);preflight_reason="ssh_auth_failed";
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&!fired);
    assert(ts_rule_start_status("retry_rule",&start)&&!strcmp(start.state,"failed")&&!strcmp(start.reason,"ssh_auth_failed"));
    checked=preflight_checks;clock_us+=30000000;assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&preflight_checks==checked);
    /* A manual failure is one attempt and cancels the pending automatic episode. */
    ts_rule_reject_t why;assert(ts_rule_trigger_ex("retry_rule",&why)==ESP_ERR_INVALID_STATE);
    checked=preflight_checks;clock_us+=30000000;assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&preflight_checks==checked);
    /* A failure after preflight resumes at the launch, without repeating the prefix. */
    assert(ts_variable_set_bool("test.ready",false)==ESP_OK);assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK);
    assert(ts_variable_set_bool("test.ready",true)==ESP_OK);refuse_service=false;
    template_execution_ret=ESP_ERR_TIMEOUT;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_FAILED,.reason="ssh_connect_timeout"};
    unsigned io=execution_io,launches=template_executions;
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_ERR_TIMEOUT&&fired&&execution_io==io+1&&template_executions==launches+1);
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"retry_unrelated\",\"source_id\":\"user\",\"value_type\":2,\"value\":3}",NULL,NULL)==ESP_OK);
    template_execution_ret=ESP_OK;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_SUCCESS};clock_us+=30000000;
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&fired&&execution_io==io+2&&template_executions==launches+2);
    /* A changed command discards a failed episode's cooldown, but not a
     * successful episode's normal cooldown. */
    clock_us+=60000000;
    template_execution_ret=ESP_ERR_TIMEOUT;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_FAILED,.reason="ssh_connect_timeout"};
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_ERR_TIMEOUT&&fired);
    assert(write_object(TS_AUTO_CONFIG_COMMAND,"{\"id\":\"retry_cmd\",\"host_id\":\"host\",\"name\":\"Retry\",\"command\":\"false\",\"nohup\":true}",NULL,NULL)==ESP_OK);
    template_execution_ret=ESP_OK;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_SUCCESS};
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&fired);
    assert(write_object(TS_AUTO_CONFIG_COMMAND,"{\"id\":\"retry_cmd\",\"host_id\":\"host\",\"name\":\"Retry\",\"command\":\"true\",\"nohup\":true}",NULL,NULL)==ESP_OK);
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&!fired);
    /* A successful manual takeover also retains its cooldown after an edit. */
    assert(ts_rule_trigger_ex("retry_rule",&why)==ESP_OK);
    assert(ts_rule_start_status("retry_rule",&start)&&!strcmp(start.state,"manual"));
    unsigned manual_launches=template_executions;
    assert(write_object(TS_AUTO_CONFIG_COMMAND,"{\"id\":\"retry_cmd\",\"host_id\":\"host\",\"name\":\"Renamed retry\",\"command\":\"true\",\"nohup\":true}",NULL,NULL)==ESP_OK);
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&!fired&&template_executions==manual_launches);
    clock_us+=30000000;
    refuse_service=true;preflight_reason="service_already_running";
    checked=preflight_checks;clock_us+=30000000;assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&!fired&&preflight_checks==checked+1);
    refuse_service=false;preflight_reason="ssh_connect_timeout";
    /* An unconfirmed launch that later proves live completes once, then runs the suffix. */
    assert(ts_variable_set_bool("test.ready",false)==ESP_OK);assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK);
    assert(ts_variable_set_bool("test.ready",true)==ESP_OK);
    template_execution_ret=ESP_ERR_TIMEOUT;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_FAILED,.reason="service_outcome_unknown"};
    io=execution_io;launches=template_executions;
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_ERR_TIMEOUT&&execution_io==io+1);
    ts_auto_rule_t prior;assert(ts_rule_acquire("retry_rule",&prior)==ESP_OK);unsigned triggers=prior.trigger_count;ts_rule_release(&prior);
    clock_us+=30000000;refuse_service=true;preflight_reason="service_already_running";
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&fired&&execution_io==io+2&&template_executions==launches+1);
    assert(ts_rule_acquire("retry_rule",&prior)==ESP_OK&&prior.trigger_count==triggers);ts_rule_release(&prior);
    refuse_service=false;preflight_reason="ssh_connect_timeout";template_execution_ret=ESP_OK;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_SUCCESS};
    /* A skipped SSH step cannot latch the whole rule. Later input can enable it. */
    assert(write_object(TS_AUTO_CONFIG_RULE,"{\"id\":\"retry_rule\",\"name\":\"Conditional retry\",\"enabled\":true,\"allow_manual_trigger\":true,\"cooldown_ms\":0,\"conditions\":[{\"variable\":\"test.ready\",\"value\":true}],\"actions\":[{\"type\":\"log\",\"message\":\"prefix\"},{\"type\":\"ssh_cmd_ref\",\"cmd_id\":\"retry_cmd\",\"condition\":{\"variable\":\"retry_unrelated\",\"operator\":\"eq\",\"value\":2}}]}",NULL,NULL)==ESP_OK);
    io=execution_io;launches=template_executions;checked=preflight_checks;
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&fired&&execution_io==io+1&&template_executions==launches&&preflight_checks==checked);
    assert(ts_variable_set_int("retry_unrelated",2)==ESP_OK);
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_OK&&fired&&template_executions==launches+1);
    assert(!ts_rule_start_status("retry_rule",&start));
    /* Ordinary failures before/after SSH retain sequence semantics and do not
     * become permanent automatic-start failures. Only the SSH step is retried. */
    assert(write_object(TS_AUTO_CONFIG_RULE,"{\"id\":\"retry_rule\",\"name\":\"Mixed failures\",\"enabled\":true,\"cooldown_ms\":0,\"conditions\":[{\"variable\":\"test.ready\",\"value\":true}],\"actions\":[{\"type\":\"webhook\",\"url\":\"http://192.0.2.250/notify\",\"method\":\"POST\"},{\"type\":\"ssh_cmd_ref\",\"cmd_id\":\"retry_cmd\"},{\"type\":\"webhook\",\"url\":\"http://192.0.2.250/notify\",\"method\":\"POST\"},{\"type\":\"log\",\"message\":\"suffix\"}]}",NULL,NULL)==ESP_OK);
    fail_webhook=true;io=execution_io;launches=template_executions;
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_FAIL&&fired&&execution_io==io+3&&template_executions==launches+1);
    assert(!ts_rule_start_status("retry_rule",&start));
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_FAIL&&fired&&execution_io==io+6&&template_executions==launches+2);
    template_execution_ret=ESP_ERR_TIMEOUT;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_FAILED,.reason="ssh_connect_timeout"};
    io=execution_io;launches=template_executions;
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_ERR_TIMEOUT&&execution_io==io+1&&template_executions==launches+1);
    assert(ts_rule_start_status("retry_rule",&start)&&!strcmp(start.state,"waiting"));
    template_execution_ret=ESP_OK;template_outcome=(ts_action_result_t){.status=TS_ACTION_STATUS_SUCCESS};clock_us+=30000000;
    assert(ts_rule_evaluate("retry_rule",&fired)==ESP_FAIL&&fired&&execution_io==io+3&&template_executions==launches+2);
    assert(!ts_rule_start_status("retry_rule",&start));fail_webhook=false;
    assert(ts_rule_unregister("retry_rule")==ESP_OK);assert(ts_ssh_commands_config_remove("retry_cmd")==ESP_OK);
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_VARIABLE,"retry_unrelated",TS_AUTO_DELETE_NORMAL,NULL,false,NULL,NULL,NULL)==ESP_OK);
    puts("PASS automatic start: 30-second cadence, 360-second deadline, permanent errors, manual single attempt, resumed actions, unrelated saves and conditional-step reevaluation");
}

int main(void)
{
    s_ctx->accepting=true;
    assert(mkdtemp(directory));assert(test_mkdir("/autocfg",0775)==0);
    assert(test_mkdir("/sdcard",0775)==0);
    ts_auto_config_error_t error={0};
    assert(ts_variable_init()==ESP_OK && ts_rule_engine_init()==ESP_OK);
    rule_publication_cases();
    assert(ts_source_runtime_init()==ESP_OK);
    ts_source_runtime.running=true;s_sio_ctx.mutex=xSemaphoreCreateMutex();assert(s_sio_ctx.mutex);
    ts_auto_variable_t sensor={0};strcpy(sensor.name,"system.temperature");strcpy(sensor.source_id,"system");
    sensor.value.type=TS_AUTO_VAL_INT;sensor.value.int_val=10;sensor.default_value=sensor.value;
    assert(ts_variable_register(&sensor)==ESP_OK);
    assert(ts_auto_config_open(TS_AUTO_CONFIG_SD,&error)==ESP_OK);
    assert(ts_ssh_commands_config_init()==ESP_OK && ts_ssh_hosts_config_init()==ESP_OK);
    assert(ts_ssh_commands_config_load_state()==ESP_OK && ts_ssh_hosts_config_load_state()==ESP_OK);
    ts_auto_config_status_t first=status();
    assert(first.loaded && first.enabled && strlen(first.active_version)==32);
    assert(write_object(TS_AUTO_CONFIG_HOST,"{\"id\":\"host\",\"host\":\"192.0.2.9\",\"username\":\"operator\",\"port\":22,\"keyid\":\"key-ref\"}",
                        first.active_version,&error)==ESP_OK);
    source_cases();
    rule_config_cases();
    automatic_start_cases();
    stop_cases();
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"threshold\",\"source_id\":\"operator\",\"value_type\":2,\"value\":3}",NULL,&error)==ESP_OK);
    ts_auto_variable_t sample={0};strcpy(sample.name,"threshold");strcpy(sample.source_id,"operator");
    sample.value.type=TS_AUTO_VAL_INT;sample.value.int_val=77;sample.default_value=sample.value;sample.flags=TS_AUTO_VAR_READONLY;
    assert(ts_variable_upsert(&sample)==ESP_OK);
    ts_variable_info_t info;assert(ts_variable_get_info("threshold",&info)==ESP_OK && info.value.int_val==77 && !info.flags);
    strcpy(sample.source_id,"other_source");assert(ts_variable_upsert(&sample)==ESP_ERR_INVALID_STATE);
    assert(ts_variable_set_int("threshold",11)==ESP_OK);
    const char *identity_json="{\"id\":\"identity_case\",\"host_id\":\"host\",\"name\":\"Identity\",\"command\":\"true\",\"var_name\":\"identity_output\"}";
    ts_ssh_command_config_t old_identity,new_identity;
    assert(write_object(TS_AUTO_CONFIG_COMMAND,identity_json,NULL,&error)==ESP_OK);
    assert(ts_ssh_commands_config_get("identity_case",&old_identity)==ESP_OK&&strlen(old_identity.object_id)==24);
    uint64_t output_instance=ts_variable_instance("identity_output.status");
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"identity_unrelated\",\"value_type\":2,\"value\":1}",NULL,&error)==ESP_OK);
    assert(ts_ssh_commands_config_get("identity_case",&new_identity)==ESP_OK&&!strcmp(old_identity.object_id,new_identity.object_id));
    assert(old_identity.instance==new_identity.instance&&output_instance==ts_variable_instance("identity_output.status"));
    assert(write_object(TS_AUTO_CONFIG_COMMAND,identity_json,NULL,&error)==ESP_OK);
    assert(ts_ssh_commands_config_get("identity_case",&new_identity)==ESP_OK&&!strcmp(old_identity.object_id,new_identity.object_id));
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_COMMAND,"identity_case",TS_AUTO_DELETE_NORMAL,NULL,false,NULL,NULL,&error)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_COMMAND,identity_json,NULL,&error)==ESP_OK);
    assert(ts_ssh_commands_config_get("identity_case",&new_identity)==ESP_OK&&strcmp(old_identity.object_id,new_identity.object_id));
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_COMMAND,"identity_case",TS_AUTO_DELETE_NORMAL,NULL,false,NULL,NULL,&error)==ESP_OK);
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_VARIABLE,"identity_unrelated",TS_AUTO_DELETE_NORMAL,NULL,false,NULL,NULL,&error)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_COMMAND,"{\"id\":\"command\",\"host_id\":\"host\",\"name\":\"Original\",\"command\":\"run\",\"var_name\":\"worker\"}",NULL,&error)==ESP_OK);
    int32_t observed;
    assert(ts_variable_get_int("threshold",&observed)==ESP_OK && observed==11);
    assert(ts_variable_get_int("system.temperature",&observed)==ESP_OK && observed==10);
    assert(ts_variable_exists("worker.status"));
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"worker.output\",\"source_id\":\"user\",\"value_type\":4,\"value\":\"command output\"}",NULL,&error)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_SOURCE,"{\"id\":\"watcher\",\"label\":\"Watcher\",\"type\":4,\"enabled\":true,\"var_prefix\":\"worker.\",\"var_watch_all\":true}",NULL,&error)==ESP_OK);
    assert(ts_variable_set_string("worker.status","ready")==ESP_OK);
    ts_auto_source_t watcher;ts_auto_value_t watched;
    assert(ts_source_get_copy("watcher",&watcher)==ESP_OK&&read_variable_source(&watcher,&watched)==ESP_OK);
    assert(ts_variable_get("watcher.status",&watched)==ESP_OK&&!strcmp(watched.str_val,"ready"));
    assert(ts_variable_get("watcher.output",&watched)==ESP_OK&&!strcmp(watched.str_val,"command output"));
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"unrelated_to_watcher\",\"value_type\":2,\"value\":1}",NULL,&error)==ESP_OK);
    assert(ts_variable_get("watcher.status",&watched)==ESP_OK&&!strcmp(watched.str_val,"ready"));
    assert(ts_variable_get("watcher.output",&watched)==ESP_OK&&!strcmp(watched.str_val,"command output"));
    assert(ts_source_unregister("watcher")==ESP_OK&&!ts_variable_exists("watcher.status")&&ts_variable_exists("worker.status"));
    assert(!ts_variable_exists("watcher.output")&&ts_variable_exists("worker.output"));
    /* An explicitly declared output remains user-owned, including after the
     * source is removed. Mirroring must not make an existing config invalid. */
    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"watcher.output\",\"source_id\":\"user\",\"value_type\":4,\"value\":\"initial\"}",NULL,&error)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_SOURCE,"{\"id\":\"watcher\",\"type\":4,\"var_prefix\":\"worker.\"}",NULL,&error)==ESP_OK);
    assert(ts_source_get_copy("watcher",&watcher)==ESP_OK&&read_variable_source(&watcher,&watched)==ESP_OK);
    assert(ts_variable_get("watcher.output",&watched)==ESP_OK&&!strcmp(watched.str_val,"command output"));
    assert(ts_source_unregister("watcher")==ESP_OK&&ts_variable_exists("watcher.output"));
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_VARIABLE,"watcher.output",TS_AUTO_DELETE_NORMAL,NULL,false,NULL,NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_SOURCE,"{\"id\":\"worker\",\"type\":4,\"var_prefix\":\"worker.\"}",NULL,&error)==ESP_OK);
    assert(ts_variable_get("worker.output",&watched)==ESP_OK&&!strcmp(watched.str_val,"command output"));
    assert(ts_source_unregister("worker")==ESP_OK&&ts_variable_exists("worker.output"));
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_VARIABLE,"worker.output",TS_AUTO_DELETE_NORMAL,NULL,false,NULL,NULL,NULL)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_SOURCE,"{\"id\":\"standalone\",\"label\":\"Standalone\",\"type\":4,\"var_prefix\":\"standalone.\"}",NULL,&error)==ESP_OK);
    assert(ts_variable_exists("standalone.status"));
    assert(ts_source_unregister("standalone")==ESP_OK&&!ts_variable_exists("standalone.status"));


    assert(write_object(TS_AUTO_CONFIG_VARIABLE,"{\"name\":\"worker.status\",\"value_type\":4,\"value\":\"manual\"}",NULL,&error)==ESP_ERR_INVALID_ARG);
    assert(!strcmp(error.reason,"variable_name_reserved"));
    assert(write_object(TS_AUTO_CONFIG_ACTION,"{\"id\":\"template\",\"name\":\"Start\",\"enabled\":true,\"type\":\"ssh_cmd_ref\",\"ssh_ref\":{\"cmd_id\":\"command\"}}",NULL,&error)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_RULE,"{\"id\":\"rule\",\"name\":\"Run\",\"enabled\":true,\"actions\":[{\"template_id\":\"template\"}]}",NULL,&error)==ESP_OK);
    ts_action_template_t template;
    assert(ts_action_template_get("template",&template)==ESP_OK);
    const char *execute_reason=NULL;
    assert(ts_action_template_execute_versioned("template","old-version",NULL,&execute_reason)==ESP_ERR_INVALID_STATE);
    assert(!strcmp(execute_reason,"configuration_changed"));
    assert(ts_action_template_execute("template",NULL)==ESP_OK);
    assert(ts_action_template_get("template",&template)==ESP_OK && template.use_count==1);
    ts_ssh_command_config_t executed;
    assert(ts_ssh_commands_config_get("command",&executed)==ESP_OK && executed.instance);
    assert(ts_ssh_commands_config_update_exec_time(&executed)==ESP_OK);
    ts_ssh_command_config_t latest;
    assert(ts_ssh_commands_config_get("command",&latest)==ESP_OK && latest.last_exec_time>0);
    assert(ts_ssh_commands_config_remove("command")!=ESP_OK); /* Definition references, without remote-state guards. */
    const ts_auto_config_view_t *held=ts_auto_config_acquire();
    ts_variable_config_view_t *prepared=NULL;
    assert(ts_variable_config_prepare(ts_auto_config_definitions(held),NULL,ts_auto_config_definitions(held),&prepared)==ESP_OK);
    assert(ts_variable_set_int("threshold",12)==ESP_OK && ts_variable_set_int("system.temperature",20)==ESP_OK);
    ts_variable_config_publish(prepared);
    assert(ts_variable_get_int("threshold",&observed)==ESP_OK && observed==12);
    assert(ts_variable_get_int("system.temperature",&observed)==ESP_OK && observed==20);
    const cJSON *initial=cJSON_GetArrayItem(cJSON_GetObjectItem(ts_auto_config_json(held),"variables"),0);
    assert(cJSON_GetObjectItem(initial,"value")->valueint==3);
    assert(!strcmp(ts_auto_config_definitions(held)->commands[0].name,"Original"));
    ts_auto_config_status_t before=status();
    cJSON *many=cJSON_CreateArray();assert(many);
    for(int i=0;i<ts_auto_config_limit(TS_AUTO_CONFIG_VARIABLE);++i) {
        cJSON *v=cJSON_CreateObject();char name[32];snprintf(name,sizeof(name),"user_%d",i);
        cJSON_AddStringToObject(v,"name",name);cJSON_AddNumberToObject(v,"value_type",2);cJSON_AddNumberToObject(v,"value",i);
        assert(cJSON_AddItemToArray(many,v));
    }
    assert(ts_auto_config_replace_collection(TS_AUTO_CONFIG_VARIABLE,many,before.active_version,&error)==ESP_ERR_INVALID_SIZE);
    assert(!strcmp(error.reason,"variable_capacity_reached") && !strcmp(status().active_version,before.active_version));
    cJSON_Delete(many);
    fail_prepare=true;
    unsigned published=publishes;
    assert(write_object(TS_AUTO_CONFIG_COMMAND,"{\"id\":\"command\",\"host_id\":\"host\",\"name\":\"Changed\",\"command\":\"run\"}",before.active_version,&error)==ESP_ERR_NO_MEM);
    assert(publishes==published && !strcmp(status().active_version,before.active_version));
    fail_prepare=false;
    fail_install=true;
    assert(write_object(TS_AUTO_CONFIG_COMMAND,"{\"id\":\"command\",\"host_id\":\"host\",\"name\":\"Changed\",\"command\":\"run\"}",before.active_version,&error)==ESP_FAIL);
    fail_install=false;
    assert(!strcmp(status().active_version,before.active_version));
    cJSON *impact=NULL;
    fail_prepare=true;
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_COMMAND,"command",TS_AUTO_DELETE_FORCE,NULL,
                                 false,before.active_version,&impact,&error)==ESP_ERR_INVALID_STATE);
    assert(!strcmp(error.reason,"pause_required"));
    cJSON_Delete(impact);
    fail_install=true;
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_COMMAND,"command",TS_AUTO_DELETE_FORCE,NULL,
                                 true,before.active_version,&impact,&error)==ESP_FAIL && !impact);
    assert(status().paused && !strcmp(error.reason,"save_failed_automation_paused"));
    const ts_auto_config_view_t *unchanged=ts_auto_config_acquire();
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(ts_auto_config_json(unchanged),"commands"))==1);
    ts_auto_config_release(unchanged);
    fail_install=false;
    before=status();
    assert(ts_auto_config_delete(TS_AUTO_CONFIG_COMMAND,"command",TS_AUTO_DELETE_FORCE,NULL,
                                 true,before.active_version,&impact,&error)==ESP_OK && !impact);
    assert(status().paused && pauses && !strcmp(ts_auto_config_definitions(held)->commands[0].name,"Original"));
    fail_prepare=false;
    assert(ts_ssh_commands_config_add(&executed,NULL,0)==ESP_OK);
    assert(ts_ssh_commands_config_get("command",&latest)==ESP_OK && latest.instance!=executed.instance && !latest.last_exec_time);
    assert(ts_ssh_commands_config_update_exec_time(&executed)==ESP_OK);
    assert(ts_ssh_commands_config_get("command",&latest)==ESP_OK && !latest.last_exec_time); /* An old completion cannot mark a recreated command. */
    assert(ts_action_template_get("template",&template)==ESP_ERR_NOT_FOUND);
    assert(write_object(TS_AUTO_CONFIG_ACTION,"{\"id\":\"template\",\"name\":\"Recreated\",\"type\":\"log\",\"log\":{\"message\":\"new\"}}",NULL,&error)==ESP_OK);
    ts_auto_config_template_used(held,0,123456);
    assert(ts_action_template_get("template",&template)==ESP_OK && !template.use_count);
    ts_auto_config_release(held);
    fail_prepare=false;
    assert(ts_auto_config_open(TS_AUTO_CONFIG_SD,&error)==ESP_OK && status().paused);
    before=status();fail_prepare=true;
    assert(write_object(TS_AUTO_CONFIG_HOST,"{\"id\":\"host\",\"host\":\"192.0.2.7\",\"username\":\"operator\"}",NULL,&error)==ESP_ERR_NO_MEM);
    assert(!strcmp(status().active_version,before.active_version));fail_prepare=false;
    before=status();
    fail_install=true;
    assert(ts_auto_config_enable(true,NULL,&error)==ESP_FAIL && status().paused);
    assert(!strcmp(status().active_version,before.active_version));
    fail_install=false;
    assert(ts_auto_config_enable(true,NULL,&error)==ESP_OK && status().enabled);
    uint64_t live_binding=ts_auto_config_binding_epoch(), queued_epoch=ts_auto_config_execution_epoch();
    assert(ts_auto_config_enable(false,NULL,&error)==ESP_OK);
    assert(ts_auto_config_binding_epoch()==live_binding&&ts_auto_config_execution_epoch()!=queued_epoch);
    assert(ts_auto_config_enable(true,NULL,&error)==ESP_OK&&ts_auto_config_binding_epoch()==live_binding);
    assert(ts_ssh_hosts_config_clear()!=ESP_OK && ts_ssh_hosts_config_count()==1);
    before=status();
    assert(ts_auto_config_open(TS_AUTO_CONFIG_INTERNAL,&error)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_HOST,"{\"id\":\"host\",\"host\":\"192.0.2.8\",\"username\":\"operator\"}",
                        before.active_version,&error)==ESP_ERR_INVALID_STATE);
    assert(!strcmp(error.reason,"configuration_changed"));
    ts_auto_config_status_t current=status();
    assert(ts_auto_config_edit_begin(current.active_version,&error)==ESP_OK);
    assert(write_object(TS_AUTO_CONFIG_HOST,"{\"id\":\"host\",\"host\":\"192.0.2.8\",\"username\":\"operator\"}",
                        NULL,&error)==ESP_OK);
    ts_auto_config_edit_end();
    const ts_auto_config_view_t *input=ts_auto_config_acquire();
    cJSON *migration=cJSON_Duplicate(ts_auto_config_json(input),true);assert(migration);
    ts_auto_config_release(input);
    before=status();fail_prepare=true;
    assert(ts_auto_config_install(TS_AUTO_CONFIG_SD,migration,&error)==ESP_ERR_NO_MEM);
    assert(!strcmp(status().active_version,before.active_version) && status().source==TS_AUTO_CONFIG_INTERNAL);
    fail_prepare=false;fail_install=true;
    assert(ts_auto_config_install(TS_AUTO_CONFIG_SD,migration,&error)==ESP_FAIL);
    assert(!strcmp(status().active_version,before.active_version) && status().source==TS_AUTO_CONFIG_INTERNAL);
    fail_install=false;
    assert(ts_auto_config_install(TS_AUTO_CONFIG_SD,migration,&error)==ESP_OK && status().source==TS_AUTO_CONFIG_SD);
    cJSON_Delete(migration);
    assert(ts_auto_config_enable(false,NULL,&error)==ESP_OK);
    assert(ts_auto_config_open(TS_AUTO_CONFIG_INTERNAL,&error)==ESP_OK && status().paused);
    ts_auto_config_close();s_config.control_loaded=false; /* New boot reads the persisted decision. */
    assert(ts_auto_config_open(TS_AUTO_CONFIG_INTERNAL,&error)==ESP_OK && status().paused);
    fail_control=true;error=(ts_auto_config_error_t){0};
    assert(ts_auto_config_enable(true,NULL,&error)==ESP_FAIL && error.saved && !strcmp(error.reason,"saved_automation_paused"));
    assert(status().paused);
    ts_auto_config_close();s_config.control_loaded=false;
    assert(ts_auto_config_open(TS_AUTO_CONFIG_INTERNAL,&error)==ESP_OK && status().paused);
    fail_control=false;
    assert(ts_auto_config_enable(true,NULL,&error)==ESP_OK && status().enabled);
    assert(ts_auto_config_open(TS_AUTO_CONFIG_SD,&error)==ESP_OK);
    before=status();++sd_generation;
    assert(write_object(TS_AUTO_CONFIG_HOST,"{\"id\":\"host\",\"host\":\"192.0.2.10\",\"username\":\"operator\"}",
                        before.active_version,&error)==ESP_ERR_INVALID_STATE && !strcmp(error.reason,"sd_card_changed"));
    assert(status().source==TS_AUTO_CONFIG_INTERNAL);
    assert(ts_auto_config_open(TS_AUTO_CONFIG_SD,&error)==ESP_OK);
    held=ts_auto_config_acquire();
    ts_auto_config_close();
    assert(ts_auto_config_definitions(held)->host_count==1);
    ts_auto_config_release(held);
    action_lifecycle_cases();
    config_api_cases();
    lifecycle_crud_api_cases();
    reference_origin_api_cases();
    config_pack_api_cases();
    mode_cases();
    ts_ssh_commands_config_deinit();ts_ssh_hosts_config_deinit();
    for(int location=TS_AUTO_FILE_SD;location<=TS_AUTO_FILE_INTERNAL;++location)
        assert(test_unlink(s_paths[location].current)==0);
    char path[512];
    const char *dirs[]={"/autocfg","/sdcard/config/automation","/sdcard/config","/sdcard"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);++i)
    { mapped(dirs[i],path,sizeof(path));assert(rmdir(path)==0); }
    assert(rmdir(directory)==0);
    assert(ts_rule_engine_deinit()==ESP_OK && ts_variable_deinit()==ESP_OK);
    ts_source_runtime_deinit();vSemaphoreDelete(s_sio_ctx.mutex);
    puts("PASS configuration owner/runtime: commit order, variable values/capacity, retained source connections, stable slots, stale callbacks and force/pause");
    return 0;
}
