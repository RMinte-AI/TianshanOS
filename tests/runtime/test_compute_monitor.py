"""Actual legacy monitor lifecycle; HTTP/WS/NVS/temperature outputs stay local."""
from pathlib import Path
import re
from test_console_lifecycle import RTOS, body, run_native, IDF

path = 'components/ts_drivers/src/ts_compute_monitor.c'
code = RTOS + r'''
#include "ts_compute_monitor.h"
#include "cJSON.h"
#ifndef ESP_ERR_NOT_ALLOWED
#define ESP_ERR_NOT_ALLOWED 0x10d
#endif
typedef int nvs_handle_t;
typedef int esp_event_base_t;
typedef int ts_event_id_t;
enum {NVS_READONLY,NVS_READWRITE,
 TS_TEMP_SOURCE_AGX_AUTO=1,TS_EVENT_BASE_DEVICE_MON=2,
 TS_EVT_AGX_CONNECTED=100,TS_EVT_AGX_DISCONNECTED,TS_EVT_AGX_ERROR,TS_EVT_AGX_DATA_UPDATED,
 WEBSOCKET_EVENT_ANY=-1,WEBSOCKET_EVENT_CONNECTED=1,WEBSOCKET_EVENT_DISCONNECTED,
 WEBSOCKET_EVENT_DATA,WEBSOCKET_EVENT_ERROR};
typedef struct {const char *url;int timeout_ms;} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
typedef struct {const char *uri;int buffer_size,reconnect_timeout_ms,network_timeout_ms,ping_interval_sec;} esp_websocket_client_config_t;
typedef struct {const char *data_ptr;int data_len;} esp_websocket_event_data_t;
typedef void (*ws_callback_t)(void *,esp_event_base_t,int32_t,void *);
typedef struct fixture_ws {ws_callback_t callback;void *arg;pthread_t thread;bool started;atomic_bool stop;} *esp_websocket_client_handle_t;

static SemaphoreHandle_t init_entered,init_release,entry_entered,entry_release,create_entered,create_release;
static SemaphoreHandle_t notify_entered,notify_release,cleanup_entered;
static atomic_bool hold_notify,worker_deleted;
static SemaphoreHandle_t http_entered,http_release,retry_entered,callback_entered,callback_release,tail_entered,tail_release;
static atomic_bool hold_init,hold_entry,hold_create,hold_http,hold_callback,hold_tail,control_done;
static atomic_int mode,http_calls,http_cleanup,ws_creates,ws_destroys,ws_alive,provider_live,provider_removals;
static atomic_int other_cleanup,data_posts,callback_data,retry_count,retry_skip;
static int control_result,init_result,self_stop_result,self_deinit_result,self_driver_result;
static bool self_check,fail_binary;
static unsigned retry_ms[16];
static char last_ws_url[256];
static SemaphoreHandle_t notifications[128];
static pthread_mutex_t notification_mutex=PTHREAD_MUTEX_INITIALIZER;
static SemaphoreHandle_t notification(TaskHandle_t task) {
 pthread_mutex_lock(&notification_mutex);int i;
 for(i=0;i<task_count&&tasks[i]!=task;++i){}
 assert(i<task_count);
 if(!notifications[i])notifications[i]=semaphore(true);
 SemaphoreHandle_t value=notifications[i];pthread_mutex_unlock(&notification_mutex);return value;
}
static int xTaskNotifyGive(TaskHandle_t task){
 if(hold_notify){xSemaphoreGive(notify_entered);assert(xSemaphoreTake(notify_release,portMAX_DELAY));}
 assert(!worker_deleted);return xSemaphoreGive(notification(task));
}
static unsigned ulTaskNotifyTake(int clear,unsigned timeout){
 (void)clear;int i=atomic_fetch_add(&retry_count,1);assert(i<16);retry_ms[i]=timeout;
 if(retry_skip>0){--retry_skip;return 0;}
 xSemaphoreGive(retry_entered);
 return xSemaphoreTake(notification(xTaskGetCurrentTaskHandle()),timeout);
}
static void (*worker_fn)(void *);static void *worker_arg;
static void held_entry(void *arg){
 (void)arg;if(hold_entry){xSemaphoreGive(entry_entered);assert(xSemaphoreTake(entry_release,portMAX_DELAY));}
 worker_fn(worker_arg);
}
static int controlled_create(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out){
 worker_deleted=false;worker_fn=fn;worker_arg=arg;
 int ret=xTaskCreate(held_entry,name,stack,NULL,priority,out);
 if(ret==pdPASS&&hold_create){xSemaphoreGive(create_entered);assert(xSemaphoreTake(create_release,portMAX_DELAY));}
 return ret;
}
static void worker_delete(TaskHandle_t task){worker_deleted=true;vTaskDelete(task);}
static void *context_alloc(size_t n,size_t size){
 if(hold_init){xSemaphoreGive(init_entered);assert(xSemaphoreTake(init_release,portMAX_DELAY));}
 return calloc(n,size);
}
static SemaphoreHandle_t controlled_binary(void){if(fail_binary){fail_binary=false;return NULL;}return xSemaphoreCreateBinary();}
static void local_delay(unsigned ticks){(void)ticks;vTaskDelay(1);}
static int nvs_writes;
static bool saved_config;
static int nvs_open(const char *ns,int access,nvs_handle_t *h){(void)ns;(void)access;*h=1;return ESP_OK;}
static int nvs_get_str(nvs_handle_t h,const char *key,char *out,size_t *n){(void)h;(void)key;if(saved_config){assert(*n>=strlen("saved.fixture.invalid")+1);strcpy(out,"saved.fixture.invalid");return ESP_OK;}return ESP_ERR_NVS_NOT_FOUND;}
static int nvs_get_u16(nvs_handle_t h,const char *key,uint16_t *out){(void)h;(void)key;if(saved_config){*out=54321;return ESP_OK;}return ESP_ERR_NVS_NOT_FOUND;}
static int nvs_set_str(nvs_handle_t h,const char *key,const char *value){(void)h;(void)key;(void)value;++nvs_writes;return ESP_OK;}
static int nvs_set_u16(nvs_handle_t h,const char *key,uint16_t value){(void)h;(void)key;(void)value;++nvs_writes;return ESP_OK;}
static int nvs_commit(nvs_handle_t h){(void)h;return ESP_OK;}
static void nvs_close(nvs_handle_t h){(void)h;}
static int ts_temp_provider_register(int id,const char *name){(void)id;(void)name;++provider_live;return ESP_OK;}
static int ts_temp_provider_unregister(int id){(void)id;provider_live=0;++provider_removals;return ESP_OK;}
static int ts_temp_provider_update(int id,int16_t value){(void)id;assert(value==425);return ESP_OK;}
static int local_post(int base,int event,const void *data,size_t len,unsigned timeout){
 (void)base;(void)data;(void)len;(void)timeout;
 if(event==TS_EVT_AGX_DATA_UPDATED){
  ++data_posts;if(hold_callback){xSemaphoreGive(callback_entered);assert(xSemaphoreTake(callback_release,portMAX_DELAY));}
 }
 return ESP_OK;
}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg){
 assert(cfg->timeout_ms==5000);return (void *)1;
}
static int esp_http_client_open(void *client,int n){
 (void)client;(void)n;++http_calls;
 if(hold_http){xSemaphoreGive(http_entered);assert(xSemaphoreTake(http_release,portMAX_DELAY));}
 return mode==1?ESP_FAIL:ESP_OK;
}
static int esp_http_client_fetch_headers(void *client){(void)client;return 24;}
static int esp_http_client_read(void *client,char *out,int n){
 (void)client;const char *reply="0{\"sid\":\"fixture-session\"}";assert(n>(int)strlen(reply));strcpy(out,reply);return strlen(reply);
}
static int esp_http_client_close(void *client){(void)client;return ESP_OK;}
static int esp_http_client_cleanup(void *client){(void)client;++http_cleanup;return ESP_OK;}
static int esp_websocket_client_send_text(esp_websocket_client_handle_t client,const char *text,int n,unsigned timeout){
 (void)client;(void)timeout;assert(!strcmp(text,"2probe")||!strcmp(text,"5")||!strcmp(text,"3"));return n;
}
static void emit(esp_websocket_client_handle_t client,int event,const char *text){
 esp_websocket_event_data_t data={text,text?(int)strlen(text):0};client->callback(client->arg,0,event,&data);
}
static void *ws_entry(void *arg){
 struct task local={0};current_task=&local;esp_websocket_client_handle_t client=arg;
 if(mode!=4){
  emit(client,WEBSOCKET_EVENT_CONNECTED,NULL);emit(client,WEBSOCKET_EVENT_DATA,"3probe");
  emit(client,WEBSOCKET_EVENT_DATA,"42[\"tegrastats_update\",{\"temperature\":{\"cpu\":42.5},\"ram\":{\"used\":20,\"total\":100},\"gpu\":{\"gr3d_freq\":17}}]");
 }
 while(!client->stop)vTaskDelay(1);return NULL;
}
static esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *cfg){
 ++ws_creates;snprintf(last_ws_url,sizeof(last_ws_url),"%s",cfg->uri);assert(cfg->network_timeout_ms==10000);
 if(mode==2)return NULL;
 esp_websocket_client_handle_t client=calloc(1,sizeof(*client));assert(client);++ws_alive;return client;
}
static int esp_websocket_register_events(esp_websocket_client_handle_t client,int event,ws_callback_t callback,void *arg){
 (void)event;client->callback=callback;client->arg=arg;return ESP_OK;
}
static int esp_websocket_client_start(esp_websocket_client_handle_t client){
 if(mode==3)return ESP_FAIL;
 client->started=true;assert(!pthread_create(&client->thread,NULL,ws_entry,client));return ESP_OK;
}
static int esp_websocket_client_stop(esp_websocket_client_handle_t client){
 if(client->started){client->stop=true;assert(!pthread_join(client->thread,NULL));client->started=false;}return ESP_OK;
}
static int esp_websocket_client_destroy(esp_websocket_client_handle_t client){
 esp_websocket_client_stop(client);--ws_alive;++ws_destroys;free(client);xSemaphoreGive(cleanup_entered);return ESP_OK;
}
/* The old data callback retains its cJSON_Print buffer. Track it explicitly:
 * lifecycle tests release fixture-owned observations afterwards, not production.
 * This test does not claim to repair or prove whole-monitor heap reclamation. */
static char *observed_prints[64];static atomic_int print_count;
static char *observed_print(const cJSON *json){char *s=cJSON_Print(json);int i=atomic_fetch_add(&print_count,1);assert(i<64);observed_prints[i]=s;return s;}
#define cJSON_Print observed_print
#define ts_event_post local_post
#define TS_CALLOC_PSRAM context_alloc
#define xTaskCreate controlled_create
#define xSemaphoreCreateBinary controlled_binary
#define vTaskDelay local_delay
#define vTaskDelete worker_delete
'''
code += re.sub(r'^#include[^\n]*\n', '', Path(path).read_text(), flags=re.M)
code += r'''
#undef cJSON_Print
#undef xTaskCreate
#undef xSemaphoreCreateBinary
#undef vTaskDelay
#undef vTaskDelete
#define CONFIG_TS_DRIVERS_POWER_ENABLE 1
static int ts_power_deinit(void){++other_cleanup;return ESP_OK;}
static int ts_temp_source_deinit(void){++other_cleanup;return ESP_OK;}
'''
code += body('components/ts_drivers/src/ts_drivers.c', 'ts_drivers_deinit')
code += r'''
typedef struct {int code;cJSON *data;} ts_api_result_t;
#define TS_API_OK 0
static void ts_api_result_init(ts_api_result_t *r){memset(r,0,sizeof(*r));}
static void ts_api_result_free(ts_api_result_t *r){cJSON_Delete(r->data);}
#define TS_API_ERR_NO_MEM 1
static void ts_api_result_error(ts_api_result_t *r,int code,const char *message){(void)message;r->code=code;}
static void ts_api_result_ok(ts_api_result_t *r,cJSON *data){r->code=TS_API_OK;r->data=data;}
static esp_err_t api_monitor_config(const cJSON *,ts_api_result_t *);
static int ts_api_call(const char *name,const cJSON *params,ts_api_result_t *r){assert(!strcmp(name,"monitor.config"));return api_monitor_config(params,r);}
static char cli_output[4096];
static void ts_console_printf(const char *fmt,...){va_list args;va_start(args,fmt);size_t n=strlen(cli_output);vsnprintf(cli_output+n,sizeof(cli_output)-n,fmt,args);va_end(args);}
static void ts_console_error(const char *fmt,...){(void)fmt;}
'''
code += r'''
static bool cli_query_running(void);
static bool cli_query_initialized(void);
#define ts_compute_monitor_is_running cli_query_running
#define ts_compute_monitor_is_initialized cli_query_initialized
'''
code += body('components/ts_api/src/ts_api_monitor.c', 'api_monitor_config')
code += body('components/ts_console/commands/ts_cmd_monitor.c', 'do_monitor_config')
code += '\n#undef ts_compute_monitor_is_running\n#undef ts_compute_monitor_is_initialized\n'

code += r'''
static void collect(ts_compute_status_t status,const ts_compute_data_t *data,void *arg){
 (void)arg;if(status==TS_COMPUTE_STATUS_RECONNECTING&&self_check){
  self_stop_result=ts_compute_monitor_stop();self_deinit_result=ts_compute_monitor_deinit();self_driver_result=ts_drivers_deinit();self_check=false;
 }
 if(data->is_valid)++callback_data;
}
static void tail_gate(SemaphoreHandle_t signal){
 if(hold_tail&&s_ctx&&signal==s_ctx->exit_done){
  xSemaphoreGive(tail_entered);assert(xSemaphoreTake(tail_release,portMAX_DELAY));
 }
}
static void *control(void *arg){
 int op=(int)(intptr_t)arg;
 control_result=op==0?ts_compute_monitor_stop():op==1?ts_compute_monitor_deinit():ts_drivers_deinit();
 control_done=true;return NULL;
}
static bool stale_stop,stale_deinit;
static void *query_interloper(void *arg){
 int ret=(intptr_t)arg?ts_compute_monitor_deinit():ts_compute_monitor_stop();assert(ret==ESP_OK);return NULL;
}
static bool cli_query_running(void){
 bool was=ts_compute_monitor_is_running();
 if(was&&stale_stop){stale_stop=false;pthread_t t;assert(!pthread_create(&t,NULL,query_interloper,NULL));assert(!pthread_join(t,NULL));}
 return was;
}
static bool cli_query_initialized(void){
 bool was=ts_compute_monitor_is_initialized();
 if(was&&stale_deinit){stale_deinit=false;pthread_t t;assert(!pthread_create(&t,NULL,query_interloper,(void *)1));assert(!pthread_join(t,NULL));}
 return was;
}
static void *initialise(void *arg){(void)arg;init_result=ts_compute_monitor_init(NULL);return NULL;}
static void *start(void *arg){(void)arg;control_result=ts_compute_monitor_start();control_done=true;return NULL;}
static void join_workers(void){
 join_tasks();
 for(int i=0;i<128;i++)if(notifications[i]){vSemaphoreDelete(notifications[i]);notifications[i]=NULL;}
 for(int i=0;i<print_count;i++){free(observed_prints[i]);observed_prints[i]=NULL;}print_count=0;
}
static ts_compute_config_t config(unsigned retry){
 ts_compute_config_t cfg;assert(ts_compute_monitor_get_default_config(&cfg)==ESP_OK);cfg.reconnect_interval_ms=retry;return cfg;
}
static void reset(void){
 assert(!s_ctx&&!ws_alive&&!provider_live);xSemaphoreTake(retry_entered,0);xSemaphoreTake(cleanup_entered,0);hold_notify=false;mode=0;retry_count=retry_skip=0;self_check=false;
 hold_init=hold_entry=hold_create=hold_http=hold_callback=hold_tail=false;control_done=false;
 callback_data=0;data_posts=0;self_stop_result=self_deinit_result=self_driver_result=0;
}
static pthread_t begin_control(int op){pthread_t t;control_done=false;assert(!pthread_create(&t,NULL,control,(void *)(intptr_t)op));return t;}
int main(void){
 SemaphoreHandle_t *signals[]={&init_entered,&init_release,&entry_entered,&entry_release,&create_entered,&create_release,
 &notify_entered,&notify_release,&cleanup_entered,&http_entered,&http_release,&retry_entered,&callback_entered,&callback_release,&tail_entered,&tail_release};
 for(unsigned i=0;i<sizeof(signals)/sizeof(signals[0]);i++)*signals[i]=semaphore(true);
 after_binary_give=tail_gate;

 reset();ts_compute_config_t current=config(12345);
 strcpy(current.server_ip,"custom.fixture.invalid");current.server_port=23456;
 current.startup_delay_ms=0;current.heartbeat_timeout_ms=777;current.auto_start=false;current.update_temp_source=false;
 assert(ts_compute_monitor_init(&current)==ESP_OK);assert(ts_compute_monitor_start()==ESP_OK);WAIT_UNTIL(data_posts>0);
 int writes=nvs_writes;ts_compute_config_t observed;
 cli_output[0]=0;assert(do_monitor_config(NULL,0,false)==0);assert(strstr(cli_output,"custom.fixture.invalid")&&strstr(cli_output,"23456"));
 cli_output[0]=0;assert(do_monitor_config(NULL,0,true)==0);cJSON *query=cJSON_Parse(cli_output);assert(query);
 assert(!strcmp(cJSON_GetObjectItem(query,"server")->valuestring,current.server_ip));assert(cJSON_GetObjectItem(query,"port")->valueint==23456);cJSON_Delete(query);
 assert(s_ctx->running&&nvs_writes==writes);assert(ts_compute_monitor_stop()==ESP_OK);join_workers();
 assert(do_monitor_config(NULL,34567,false)==0);current.server_port=34567;
 assert(ts_compute_monitor_get_config(&observed)==ESP_OK&&!memcmp(&current,&observed,sizeof(current)));assert(!s_ctx->running&&nvs_writes==writes);
 assert(ts_compute_monitor_start()==ESP_OK);WAIT_UNTIL(strstr(last_ws_url,"custom.fixture.invalid:34567"));assert(ts_compute_monitor_stop()==ESP_OK);join_workers();
 assert(do_monitor_config("next.fixture.invalid",0,false)==0);memset(current.server_ip,0,sizeof(current.server_ip));strcpy(current.server_ip,"next.fixture.invalid");
 assert(ts_compute_monitor_get_config(&observed)==ESP_OK&&!memcmp(&current,&observed,sizeof(current)));
 assert(do_monitor_config("both.fixture.invalid",45678,false)==0);memset(current.server_ip,0,sizeof(current.server_ip));strcpy(current.server_ip,"both.fixture.invalid");current.server_port=45678;
 assert(ts_compute_monitor_get_config(&observed)==ESP_OK&&!memcmp(&current,&observed,sizeof(current)));assert(!s_ctx->running&&nvs_writes==writes);
 assert(ts_compute_monitor_deinit()==ESP_OK);join_workers();
 saved_config=true;cli_output[0]=0;assert(do_monitor_config(NULL,0,false)==0);assert(strstr(cli_output,"saved.fixture.invalid")&&strstr(cli_output,"54321"));
 assert(do_monitor_config(NULL,34567,false)==0);assert(!strcmp(s_ctx->config.server_ip,"saved.fixture.invalid")&&s_ctx->config.server_port==34567);
 assert(ts_compute_monitor_deinit()==ESP_OK);saved_config=false;
 cli_output[0]=0;assert(do_monitor_config(NULL,0,false)==0);assert(strstr(cli_output,"10.10.99.98")&&strstr(cli_output,"58090"));
 assert(ts_compute_monitor_init(NULL)==ESP_OK);assert(ts_compute_monitor_start()==ESP_OK);WAIT_UNTIL(strstr(last_ws_url,"10.10.99.98:58090"));
 assert(ts_compute_monitor_deinit()==ESP_OK);join_workers();assert(nvs_writes==writes);
 puts("PASS F23 actual CLI text/JSON query, port-only/address-only/both edits, explicit start, saved/default startup; no CLI NVS writes or automatic restart");


 provider_removals=0;
 reset();assert(ts_drivers_deinit()==ESP_OK);int cleanup_before=other_cleanup;
 hold_init=true;pthread_t t;assert(!pthread_create(&t,NULL,initialise,NULL));assert(xSemaphoreTake(init_entered,portMAX_DELAY));
 assert(!ts_compute_monitor_is_initialized());assert(ts_drivers_deinit()==ESP_ERR_NOT_FINISHED);
 assert(other_cleanup==cleanup_before&&!provider_removals);xSemaphoreGive(init_release);assert(!pthread_join(t,NULL));assert(init_result==ESP_OK);
 hold_init=false;assert(ts_drivers_deinit()==ESP_OK);assert(other_cleanup==cleanup_before+2);
 puts("PASS init owns lifecycle before initialized publication; driver stop propagates BUSY, absent monitor remains ordinary stop");

 reset();ts_compute_config_t cfg=config(12000);assert(ts_compute_monitor_init(&cfg)==ESP_OK);
 assert(ts_compute_monitor_register_callback(collect,NULL)==ESP_OK);
 for(int round=0;round<2;round++){
  assert(ts_compute_monitor_start()==ESP_OK);WAIT_UNTIL(data_posts>round);
  ts_compute_data_t data;assert(ts_compute_monitor_get_data(&data)==ESP_OK&&data.is_valid&&data.temperature.cpu==42.5f&&data.memory.ram.used_mb==20);
  assert(strstr(last_ws_url,"fixture-session"));assert(ts_compute_monitor_stop()==ESP_OK);
  assert(!s_ctx->task_handle&&!ts_compute_monitor_is_running()&&!ws_alive);join_workers();
 }
 assert(ts_compute_monitor_set_server("new.fixture.invalid",1234)==ESP_OK);
 assert(ts_compute_monitor_start()==ESP_OK);WAIT_UNTIL(data_posts>2);
 assert(strstr(last_ws_url,"new.fixture.invalid:1234"));assert(ts_compute_monitor_set_server("other.fixture.invalid",4321)==ESP_OK);
 WAIT_UNTIL(strstr(last_ws_url,"other.fixture.invalid:4321"));assert(ts_compute_monitor_deinit()==ESP_OK);join_workers();
 puts("PASS normal actual sid/probe/upgrade/data, public data read, stop/restart and active server change retain connection flow");

 for(int failure=1;failure<=4;failure++){
  reset();mode=failure;cfg=config(12000);assert(ts_compute_monitor_init(&cfg)==ESP_OK);retry_skip=failure==1?2:0;
  assert(ts_compute_monitor_start()==ESP_OK);assert(xSemaphoreTake(retry_entered,portMAX_DELAY));
  if(failure==1)assert(retry_count==3&&retry_ms[0]==12000&&retry_ms[1]==24000&&retry_ms[2]==30000);
  assert(ts_compute_monitor_deinit()==ESP_OK);join_workers();assert(!ws_alive);
 }
 puts("PASS HTTP/WS init/start/connect failures retain retry strategy; 12/24/30 second waits wake by task notification");

 reset();mode=1;hold_entry=true;assert(ts_compute_monitor_init(NULL)==ESP_OK);assert(ts_compute_monitor_start()==ESP_OK);
 assert(xSemaphoreTake(entry_entered,portMAX_DELAY));t=begin_control(1);WAIT_UNTIL(s_ctx->should_stop);
 assert(!control_done&&s_ctx);xSemaphoreGive(entry_release);assert(!pthread_join(t,NULL));assert(control_result==ESP_OK);join_workers();
 puts("PASS immediate deinit waits for the task even before its first production entry");

 reset();mode=1;hold_create=true;assert(ts_compute_monitor_init(NULL)==ESP_OK);
 assert(!pthread_create(&t,NULL,start,NULL));assert(xSemaphoreTake(create_entered,portMAX_DELAY));
 assert(ts_compute_monitor_stop()==ESP_ERR_NOT_FINISHED);assert(ts_compute_monitor_deinit()==ESP_ERR_NOT_FINISHED);
 xSemaphoreGive(create_release);assert(!pthread_join(t,NULL));assert(control_result==ESP_OK);
 assert(ts_compute_monitor_deinit()==ESP_OK);join_workers();
 puts("PASS child scheduled before creator returns cannot bypass publication or allow a second controller");

 reset();hold_http=true;assert(ts_compute_monitor_init(NULL)==ESP_OK);assert(ts_compute_monitor_start()==ESP_OK);
 assert(xSemaphoreTake(http_entered,portMAX_DELAY));int creates=ws_creates;t=begin_control(1);WAIT_UNTIL(s_ctx->should_stop);
 assert(!control_done);xSemaphoreGive(http_release);assert(!pthread_join(t,NULL));assert(control_result==ESP_OK&&ws_creates==creates);join_workers();
 puts("PASS in-flight HTTP must finish before context release and cannot start a new WS after stop");

 reset();hold_callback=true;assert(ts_compute_monitor_init(NULL)==ESP_OK);assert(ts_compute_monitor_start()==ESP_OK);
 assert(xSemaphoreTake(callback_entered,portMAX_DELAY));int removals=provider_removals;t=begin_control(1);WAIT_UNTIL(s_ctx->should_stop);
 assert(!control_done&&ws_alive&&provider_removals==removals);assert(ts_drivers_deinit()==ESP_ERR_NOT_FINISHED);
 xSemaphoreGive(callback_release);assert(!pthread_join(t,NULL));assert(control_result==ESP_OK&&!ws_alive);join_workers();
 puts("PASS SDK-shaped WS stop joins an in-flight actual callback before monitor/provider/context teardown");

 reset();hold_notify=true;assert(ts_compute_monitor_init(NULL)==ESP_OK);assert(ts_compute_monitor_start()==ESP_OK);
 WAIT_UNTIL(data_posts>0);t=begin_control(1);assert(xSemaphoreTake(notify_entered,portMAX_DELAY));
 assert(xSemaphoreTake(cleanup_entered,portMAX_DELAY));assert(s_ctx->data_mutex->owner&&!worker_deleted&&!control_done);
 xSemaphoreGive(notify_release);assert(!pthread_join(t,NULL));assert(control_result==ESP_OK);join_workers();
 puts("PASS stop publishes cancellation and notifies under the final-exit mutex; the task handle remains alive through notification");

 reset();mode=1;hold_tail=true;assert(ts_compute_monitor_init(NULL)==ESP_OK);assert(ts_compute_monitor_start()==ESP_OK);
 assert(xSemaphoreTake(retry_entered,portMAX_DELAY));t=begin_control(1);assert(xSemaphoreTake(tail_entered,portMAX_DELAY));
 assert(!control_done&&s_ctx);assert(ts_compute_monitor_start()==ESP_ERR_NOT_FINISHED);
 assert(ts_compute_monitor_deinit()==ESP_ERR_NOT_FINISHED);assert(do_monitor_config("busy.invalid",42,false)==1);
 xSemaphoreGive(tail_release);assert(!pthread_join(t,NULL));assert(control_result==ESP_OK);join_workers();
 puts("PASS exit notification before final unlock cannot permit free, new start, or CLI reinitialization");

 reset();mode=1;self_check=true;hold_http=true;assert(ts_compute_monitor_init(NULL)==ESP_OK);assert(ts_compute_monitor_register_callback(collect,NULL)==ESP_OK);
 assert(ts_compute_monitor_start()==ESP_OK);assert(xSemaphoreTake(http_entered,portMAX_DELAY));xSemaphoreGive(http_release);assert(xSemaphoreTake(retry_entered,portMAX_DELAY));
 assert(self_stop_result==ESP_ERR_NOT_ALLOWED&&self_deinit_result==ESP_ERR_NOT_ALLOWED&&self_driver_result==ESP_ERR_NOT_ALLOWED&&s_ctx);
 assert(ts_compute_monitor_deinit()==ESP_OK);join_workers();
 fail_binary=true;assert(ts_compute_monitor_init(NULL)==ESP_ERR_NO_MEM&&!s_ctx);
 assert(ts_compute_monitor_init(NULL)==ESP_OK);fail_next_create=true;
 assert(ts_compute_monitor_start()==ESP_FAIL&&!s_ctx->running&&!s_ctx->task_handle);
 assert(ts_compute_monitor_deinit()==ESP_OK);assert(!s_ctx&&!provider_live);
 puts("PASS self wait is distinct from no context; signal/task allocation failures release lifecycle ownership and clean resources");

 reset();mode=1;assert(ts_compute_monitor_init(NULL)==ESP_OK);assert(ts_compute_monitor_start()==ESP_OK);
 assert(xSemaphoreTake(retry_entered,portMAX_DELAY));stale_stop=true;
 int stopped_config=do_monitor_config("stopped.fixture.invalid",123,false);
 printf("OBS CLI after another controller already stopped: %d\n",stopped_config);fflush(stdout);
 if(stopped_config==0)assert(s_ctx&&!s_ctx->running&&!strcmp(s_ctx->config.server_ip,"stopped.fixture.invalid"));
 assert(ts_compute_monitor_deinit()==ESP_OK);join_workers();
 reset();assert(ts_compute_monitor_init(NULL)==ESP_OK);stale_deinit=true;
 int absent_config=do_monitor_config("absent.fixture.invalid",456,false);
 printf("OBS CLI after another controller already deinitialized: %d\n",absent_config);fflush(stdout);
 if(s_ctx){assert(!s_ctx->running&&!strcmp(s_ctx->config.server_ip,"absent.fixture.invalid"));assert(ts_compute_monitor_deinit()==ESP_OK);}
 assert(stopped_config==0&&absent_config==0);
 puts("PASS actual CLI continues configuration when another real controller already completed stop/deinit; BUSY and self errors stay distinct");

 for(unsigned i=0;i<sizeof(signals)/sizeof(signals[0]);i++)vSemaphoreDelete(*signals[i]);
}
'''
run_native(code, [IDF / 'components/json/cJSON/cJSON.c'])
