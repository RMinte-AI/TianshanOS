/* Compile the entire production WS unit, its actual declarations/public types,
 * controller, manager and TX. Only external platform/business boundaries fake. */
#define main manager_regressions
#include "test_manager.c"
#undef main
#include "../../components/ts_webui/include/ts_webui.h"
#include "../../components/ts_core/ts_log/include/ts_log.h"
#undef TS_LOGD
#undef TS_LOGI
#undef TS_LOGW
#undef TS_LOGE
#define TS_LOGD(...) ((void)0)
#define TS_LOGI(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#include "ts_console.h"
#include "ts_ssh_client.h"
#include "ts_ssh_shell.h"
#include "ts_variable.h"
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTP_PUT 2
#define HTTP_DELETE 3
#define HTTP_PATCH 4
#include "ts_http_server.h"
#include "freertos/timers.h"
#include <setjmp.h>
#define HTTP_GET 0
#define HTTPD_WS_TYPE_CLOSE 8
#define CONFIG_TS_LOG_BUFFER_SIZE 16
#define TS_EVENT_BASE_POWER "power"
#define TS_EVENT_ANY_ID -1
typedef struct {const char *uri;int method;esp_err_t (*handler)(httpd_req_t *);void *user_ctx;bool is_websocket,handle_ws_control_frames;} httpd_uri_t;
static int httpd_register_uri_handler(httpd_handle_t h,const httpd_uri_t *u){(void)h;(void)u;return 0;}
static int httpd_ws_send_frame(httpd_req_t *r,httpd_ws_frame_t *f){return httpd_ws_send_frame_async(r->handle,r->fd,f);}
static const char *incoming;
static int httpd_ws_recv_frame(httpd_req_t *r,httpd_ws_frame_t *f,size_t n){(void)r;if(!incoming)return ESP_FAIL;f->type=1;f->len=strlen(incoming);if(n)memcpy(f->payload,incoming,f->len);return 0;}
static void *host_heap_malloc(size_t n,unsigned caps){(void)caps;return tracked_malloc(n);}
static void *host_calloc(size_t n,size_t z){void *p=tracked_malloc(n*z);if(p)memset(p,0,n*z);return p;}
static void *host_heap_calloc(size_t n,size_t z,unsigned caps){(void)caps;return host_calloc(n,z);}
static char *host_strdup(const char *s){size_t n=strlen(s)+1;char *p=tracked_malloc(n);if(p)memcpy(p,s,n);return p;}
static char *host_strndup(const char *s,size_t n){char *p=tracked_malloc(n+1);if(p){memcpy(p,s,n);p[n]=0;}return p;}
static struct {void (*fn)(void *);void *arg;} shell_task,exec_task;
static bool task_create_fail,close_during_create,inline_exec;
static int adapter_create(void(*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out){
 (void)stack;(void)priority;(void)out;if(task_create_fail)return 0;
 if(!strcmp(name,"ssh_poll"))shell_task=(typeof(shell_task)){fn,arg};else exec_task=(typeof(exec_task)){fn,arg};
 if(inline_exec && !strcmp(name,"ssh_exec"))fn(arg);
 return pdPASS;
}
static int adapter_create_caps(void(*fn)(void*),const char *n,unsigned stack,void *arg,unsigned p,TaskHandle_t *out,unsigned caps);
static BaseType_t xTimerChangePeriod(TimerHandle_t t,TickType_t period,TickType_t wait){(void)t;(void)period;(void)wait;return pdPASS;}
static BaseType_t xTimerStop(TimerHandle_t t,TickType_t wait){(void)t;(void)wait;return pdPASS;}
#define malloc tracked_malloc
#define calloc host_calloc
#define free tracked_free
#define strdup host_strdup
#define strndup host_strndup
#define heap_caps_malloc host_heap_malloc
#define heap_caps_calloc host_heap_calloc
#define xTaskCreate adapter_create
#define xTaskCreateWithCaps adapter_create_caps
#include "ts_webui_ws.c"
#undef malloc
#undef calloc
#undef free
#undef strdup
#undef strndup
#undef heap_caps_malloc
#undef heap_caps_calloc
#undef xTaskCreate
#undef xTaskCreateWithCaps
struct ts_ssh_session_s {bool aborted;};
struct ts_ssh_shell_s {bool active;};
static unsigned aborts,variable_writes,session_destroys;
static int remote_failure;
static void (*remote_hook)(void);
static bool remote_timeout;
static struct host_timer {void *id;void (*fn)(TimerHandle_t);bool active;} *host_timer;
static struct {void (*fn)(void *,uint32_t);void *arg;uint32_t value;TimerHandle_t deleted;} timer_queue[16];
static unsigned timer_queued;static bool timer_queue_fail,timer_inline;
TimerHandle_t xTimerCreate(const char*n,TickType_t t,BaseType_t repeat,void*id,void(*fn)(TimerHandle_t)){
 (void)n;(void)t;(void)repeat;host_timer=calloc(1,sizeof(*host_timer));host_timer->id=id;host_timer->fn=fn;return host_timer;
}
void *pvTimerGetTimerID(TimerHandle_t t){return t->id;}
BaseType_t xTimerStart(TimerHandle_t t,TickType_t wait){(void)wait;t->active=true;return pdPASS;}
BaseType_t xTimerDelete(TimerHandle_t t,TickType_t wait){(void)wait;if(timer_queue_fail)return pdFAIL;assert(timer_queued<16);timer_queue[timer_queued++]=(typeof(timer_queue[0])){.deleted=t};return pdPASS;}
BaseType_t xTimerPendFunctionCall(void(*fn)(void*,uint32_t),void *arg,uint32_t v,TickType_t wait){
 (void)wait;if(timer_queue_fail)return pdFAIL;
 if(timer_inline && !timer_queued){fn(arg,v);return pdPASS;}
 assert(timer_queued<16);timer_queue[timer_queued++]=(typeof(timer_queue[0])){fn,arg,v,NULL};return pdPASS;
}
static void timer_pump(void){while(timer_queued){typeof(timer_queue[0]) q=timer_queue[0];memmove(timer_queue,timer_queue+1,--timer_queued*sizeof(*timer_queue));if(q.deleted){free(q.deleted);host_timer=NULL;}else q.fn(q.arg,q.value);}}
esp_err_t ts_ssh_session_create(const ts_ssh_config_t*c,ts_ssh_session_t *out){(void)c;if(remote_failure==1)return ESP_FAIL;*out=host_calloc(1,sizeof(**out));return *out?ESP_OK:ESP_ERR_NO_MEM;}
esp_err_t ts_ssh_connect(ts_ssh_session_t s){(void)s;return remote_failure==2?ESP_FAIL:ESP_OK;}
const char *ts_ssh_get_error(ts_ssh_session_t s){(void)s;return "fake network error";}
esp_err_t ts_ssh_disconnect(ts_ssh_session_t s){(void)s;return ESP_OK;}
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t s){session_destroys++;tracked_free(s);return ESP_OK;}
void ts_ssh_abort(ts_ssh_session_t s){s->aborted=true;aborts++;}
esp_err_t ts_ssh_shell_open(ts_ssh_session_t s,const ts_shell_config_t*c,ts_ssh_shell_t *out){(void)s;(void)c;if(remote_failure==3)return ESP_FAIL;*out=host_calloc(1,sizeof(**out));(*out)->active=true;return 0;}
bool ts_ssh_shell_is_active(ts_ssh_shell_t s){return s&&s->active;}
esp_err_t ts_ssh_shell_close(ts_ssh_shell_t s){tracked_free(s);return 0;}
esp_err_t ts_ssh_shell_read(ts_ssh_shell_t s,char *b,size_t n,size_t *out){(void)b;(void)n;*out=0;s->active=false;return 0;}
esp_err_t ts_ssh_shell_write(ts_ssh_shell_t s,const char *b,size_t n,size_t *out){(void)s;(void)b;(void)n;(void)out;return 0;}
esp_err_t ts_ssh_shell_resize(ts_ssh_shell_t s,uint16_t w,uint16_t h){(void)s;(void)w;(void)h;return 0;}
esp_err_t ts_ssh_shell_send_signal(ts_ssh_shell_t s,const char *signal){(void)s;(void)signal;return 0;}
esp_err_t ts_ssh_exec_stream(ts_ssh_session_t s,const char *command,ts_ssh_output_cb_t cb,void *arg,int *code){
 (void)command;if(remote_hook)remote_hook();if(remote_timeout && host_timer)host_timer->fn(host_timer);
 cb("value=42\n",9,false,arg);*code=0;return s->aborted?ESP_ERR_TIMEOUT:ESP_OK;
}
esp_err_t ts_keystore_load_private_key(const char *id,char **out,size_t *n){(void)id;(void)out;(void)n;return ESP_FAIL;}
esp_err_t ts_variable_upsert(const ts_auto_variable_t *v){(void)v;variable_writes++;return 0;}
esp_err_t ts_console_exec(const char *cmd,ts_cmd_result_t *result){(void)cmd;memset(result,0,sizeof(*result));return 0;}
esp_err_t ts_console_clear_output_cb(void){return 0;}
esp_err_t ts_console_set_output_cb(ts_console_output_cb_t cb,void *arg){(void)cb;(void)arg;return 0;}
void ts_console_request_interrupt(void){}
esp_err_t ts_log_add_callback(ts_log_callback_t cb,ts_log_level_t l,void *arg,ts_log_callback_handle_t *out){(void)cb;(void)l;(void)arg;*out=(void*)1;return 0;}
static int remove_log_result;
esp_err_t ts_log_remove_callback(ts_log_callback_handle_t h){(void)h;return remove_log_result;}
size_t ts_log_buffer_search(ts_log_entry_t *e,size_t n,ts_log_level_t min,ts_log_level_t max,const char *tag,const char *keyword){(void)e;(void)n;(void)min;(void)max;(void)tag;(void)keyword;return 0;}
httpd_handle_t ts_http_server_get_handle(void){return (void*)11;}
void ts_http_server_set_stop_hooks(esp_err_t(*stop)(httpd_handle_t),void(*stopped)(httpd_handle_t)){(void)stop;(void)stopped;}
static int adapter_create_caps(void(*fn)(void*),const char*n,unsigned stack,void*arg,unsigned p,TaskHandle_t*out,unsigned caps){(void)caps;if(close_during_create)s_ssh_shell->active=false;return adapter_create(fn,n,stack,arg,p,out);}
static unsigned replacement,terminal_count,output_count;static bool terminal_first;
static void observe_old(httpd_ws_frame_t *f){const char*p=(char*)f->payload;if(strstr(p,"ssh_exec_error")||strstr(p,"ssh_exec_done"))terminal_count++;if(strstr(p,"ssh_exec_output")){output_count++;if(terminal_count)terminal_first=true;}}
static void connect_old(void){cJSON*j=cJSON_Parse("{\"host\":\"fake\",\"user\":\"fake\"}");current=(void*)3;handle_ssh_connect(&reqs[1],j);current=(void*)1;cJSON_Delete(j);}
static void baseline_poll_exit(void){current=(void*)4;shell_task.fn(shell_task.arg);current=(void*)3;}
static void baseline_open(int fd){current=(void*)3;reqs[fd]=(httpd_req_t){.handle=(void*)11,.fd=fd};assert(add_client(&reqs[fd],WS_CLIENT_TYPE_EVENT)==ESP_OK);current=(void*)1;}
static void g1_old_window(void){delay_hook=baseline_poll_exit;close_peer(1);delay_hook=NULL;assert(!s_ssh_poll_alive);baseline_open(1);connect_old();replacement=s_ssh_generation;}
static jmp_buf baseline_yield;
static void baseline_wait(void){longjmp(baseline_yield,1);}
static void g2_old_window(void){
 current=(void*)3;typeof(queue[0]) q=queue[0];memmove(queue,queue+1,--queued*sizeof(*queue));q.fn(q.arg);current=(void*)1;
 queue_fail=true;wait_hook=baseline_wait;current=(void*)2;if(!setjmp(baseline_yield))worker_fn(NULL);current=(void*)1;wait_hook=NULL;
 queue_fail=false;test_now+=100000;pump();assert(s_exec_output_failed && terminal_count==1);
}
int main(int argc,char**argv){
 assert(argc==2);cJSON_Hooks h={tracked_malloc,tracked_free};cJSON_InitHooks(&h);start();s_server=(void*)11;transport_server.closed=peer_closed;baseline_open(1);
 if(!strcmp(argv[1],"g1")){
  connect_old();unsigned old=s_ssh_generation;ts_ws_message_t*m=ts_ws_message_text("block",5);
  assert(ts_ws_transport_submit(s_ssh_peer,m,0,0,NULL,NULL)==0);ts_ws_message_release(m);
  ssh_send_output("old",3); /* poller returns; SDK admission is deferred to worker */
  current=(void*)3;typeof(queue[0]) q=queue[0];memmove(queue,queue+1,--queued*sizeof(*queue));q.fn(q.arg);current=(void*)1;
  barrier_name="G1";barrier_action=g1_old_window;queue_fail=true;
  wait_hook=baseline_wait;current=(void*)2;if(!setjmp(baseline_yield))worker_fn(NULL);current=(void*)1;wait_hook=NULL;
  queue_fail=false;test_now+=100000;pump();bool bad=replacement>old && s_ssh_state==SSH_TERMINAL;
  printf("G1 complete production unit: replacement=%u old=%u replacement_corrupted=%d\n",replacement,old,bad);
  s_ssh_running=false;shell_task.fn(shell_task.arg);pump();stop();assert(!live_allocations);return bad?1:0;
 }
 assert(!strcmp(argv[1],"g2"));uint32_t id;
 assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==0);
 sent_hook=observe_old;
 ts_ws_peer_t peer=*(ts_ws_peer_t*)reqs[1].sess_ctx;ts_ws_message_t *m=ts_ws_message_text("block",5);
 assert(ts_ws_transport_submit(peer,m,0,0,NULL,NULL)==0);ts_ws_message_release(m);
 ssh_exec_output_callback("A",1,false,(void*)(uintptr_t)id);
 barrier_name="G2";barrier_action=g2_old_window;
 ssh_exec_output_callback("prepared",8,false,(void*)(uintptr_t)id);pump();bool bad=terminal_first;
 printf("G2 complete production unit: terminal_before_late_output=%d\n",bad);
 /* Baseline's separately known credential leak: track/free only after its real
  * task cleanup. This does not replace any lifecycle code or affect assertion. */
 const char *host=s_exec_params->config.host,*user=s_exec_params->config.username,*password=s_exec_params->config.auth.password;
 exec_task.fn(exec_task.arg);pump();tracked_free((void*)host);tracked_free((void*)user);tracked_free((void*)password);stop();assert(!live_allocations);return bad?1:0;
}
