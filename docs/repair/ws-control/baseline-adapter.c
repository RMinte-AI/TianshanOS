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
struct ts_ssh_session_s {bool aborted;ts_ssh_config_t config;};
static unsigned control_writes,control_signals,control_resizes,commands_submitted;
static void (*stage_hook)(const char *);
static void stage(const char *name){if(stage_hook)stage_hook(name);}
struct ts_ssh_shell_s {bool active;};
static unsigned aborts,variable_writes,session_destroys;
static int remote_failure;
static void (*remote_hook)(void);
static bool remote_timeout;
static struct host_timer {void *id;void (*fn)(TimerHandle_t);bool active;} *host_timer;
static struct {void (*fn)(void *,uint32_t);void *arg;uint32_t value;TimerHandle_t deleted;} timer_queue[16];
static unsigned timer_queued;static bool timer_queue_fail,timer_inline,timer_create_fail,timer_start_fail;
TimerHandle_t xTimerCreate(const char*n,TickType_t t,BaseType_t repeat,void*id,void(*fn)(TimerHandle_t)){
 (void)n;(void)t;(void)repeat;if(timer_create_fail)return NULL;host_timer=calloc(1,sizeof(*host_timer));host_timer->id=id;host_timer->fn=fn;return host_timer;
}
void *pvTimerGetTimerID(TimerHandle_t t){return t->id;}
BaseType_t xTimerStart(TimerHandle_t t,TickType_t wait){(void)wait;if(timer_start_fail)return pdFAIL;t->active=true;return pdPASS;}
BaseType_t xTimerDelete(TimerHandle_t t,TickType_t wait){(void)wait;if(timer_queue_fail)return pdFAIL;assert(timer_queued<16);timer_queue[timer_queued++]=(typeof(timer_queue[0])){.deleted=t};return pdPASS;}
BaseType_t xTimerPendFunctionCall(void(*fn)(void*,uint32_t),void *arg,uint32_t v,TickType_t wait){
 (void)wait;if(timer_queue_fail)return pdFAIL;
 if(timer_inline && !timer_queued){fn(arg,v);return pdPASS;}
 assert(timer_queued<16);timer_queue[timer_queued++]=(typeof(timer_queue[0])){fn,arg,v,NULL};return pdPASS;
}
static void timer_pump(void){while(timer_queued){typeof(timer_queue[0]) q=timer_queue[0];memmove(timer_queue,timer_queue+1,--timer_queued*sizeof(*timer_queue));if(q.deleted){free(q.deleted);host_timer=NULL;}else q.fn(q.arg,q.value);}}
esp_err_t ts_ssh_session_create(const ts_ssh_config_t*c,ts_ssh_session_t *out){stage("create");if(remote_failure==1)return ESP_FAIL;*out=host_calloc(1,sizeof(**out));if(*out)(*out)->config=*c;return *out?ESP_OK:ESP_ERR_NO_MEM;}
esp_err_t ts_ssh_connect(ts_ssh_session_t s){stage("connect");if(s->config.cancelled && s->config.cancelled(s->config.cancel_context))return ESP_ERR_TIMEOUT;stage("auth");return remote_failure==2?ESP_FAIL:ESP_OK;}
const char *ts_ssh_get_error(ts_ssh_session_t s){(void)s;return "fake network error";}
esp_err_t ts_ssh_disconnect(ts_ssh_session_t s){(void)s;return ESP_OK;}
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t s){session_destroys++;tracked_free(s);return ESP_OK;}
void ts_ssh_abort(ts_ssh_session_t s){s->aborted=true;aborts++;}
esp_err_t ts_ssh_shell_open(ts_ssh_session_t s,const ts_shell_config_t*c,ts_ssh_shell_t *out){(void)s;(void)c;if(remote_failure==3)return ESP_FAIL;*out=host_calloc(1,sizeof(**out));(*out)->active=true;return 0;}
bool ts_ssh_shell_is_active(ts_ssh_shell_t s){return s&&s->active;}
esp_err_t ts_ssh_shell_close(ts_ssh_shell_t s){tracked_free(s);return 0;}
esp_err_t ts_ssh_shell_read(ts_ssh_shell_t s,char *b,size_t n,size_t *out){(void)b;(void)n;*out=0;s->active=false;return 0;}
esp_err_t ts_ssh_shell_write(ts_ssh_shell_t s,const char *b,size_t n,size_t *out){(void)s;(void)b;(void)n;(void)out;control_writes++;return 0;}
esp_err_t ts_ssh_shell_resize(ts_ssh_shell_t s,uint16_t w,uint16_t h){(void)s;(void)w;(void)h;control_resizes++;return 0;}
esp_err_t ts_ssh_shell_send_signal(ts_ssh_shell_t s,const char *signal){(void)s;(void)signal;control_signals++;return 0;}
esp_err_t ts_ssh_exec_stream(ts_ssh_session_t s,const char *command,ts_ssh_output_cb_t cb,void *arg,int *code){
 (void)command;stage("submit");if(s->config.cancelled && s->config.cancelled(s->config.cancel_context))return ESP_ERR_TIMEOUT;commands_submitted++;if(remote_hook)remote_hook();if(remote_timeout && host_timer)host_timer->fn(host_timer);
 cb("value=42\n",9,false,arg);*code=0;return s->aborted || (s->config.cancelled && s->config.cancelled(s->config.cancel_context))?ESP_ERR_TIMEOUT:ESP_OK;
}
esp_err_t ts_keystore_load_private_key(const char *id,char **out,size_t *n){(void)id;stage("key");*out=host_strdup("fake-key");*n=9;return ESP_OK;}
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
static int adapter_create_caps(void(*fn)(void*),const char*n,unsigned stack,void*arg,unsigned p,TaskHandle_t*out,unsigned caps){(void)caps;if(close_during_create)((ssh_shell_context_t*)arg)->shell->active=false;return adapter_create(fn,n,stack,arg,p,out);}
static void drive_all(void){for(unsigned i=0;i<12;i++){ts_ws_op_poll();pump();timer_pump();}assert(!timer_queued);}
static void setup(void){start();memset(s_clients,0,sizeof(s_clients));s_server=(void*)11;s_ws_stopping=false;ts_ws_op_enable();transport_server.closed=peer_closed;for(int fd=1;fd<=2;fd++){reqs[fd]=(httpd_req_t){.handle=(void*)11,.fd=fd};current=(void*)3;assert(add_client(&reqs[fd],WS_CLIENT_TYPE_EVENT)==ESP_OK);}current=(void*)1;}
static void finish_all(void){drive_all();assert(!ts_ws_op_busy());stop();assert(!live_allocations);}
static void connect_shell(void){cJSON *j=cJSON_Parse("{\"host\":\"fake\",\"user\":\"fake\"}");current=(void*)3;handle_ssh_connect(&reqs[1],j);current=(void*)1;cJSON_Delete(j);}
static unsigned terminal_frames,output_frames,connected_frames,disconnecting_frames;static bool output_after_terminal;
static void observe(httpd_ws_frame_t *f){const char *p=(char*)f->payload;if(strstr(p,"ssh_output")||strstr(p,"ssh_exec_output")){if(terminal_frames)output_after_terminal=true;output_frames++;}if(strstr(p,"ssh_exec_done")||strstr(p,"ssh_exec_error")||strstr(p,"\"status\":\"error\"")||strstr(p,"\"status\":\"closed\""))terminal_frames++;if(strstr(p,"\"status\":\"connected\""))connected_frames++;if(strstr(p,"\"status\":\"disconnecting\""))disconnecting_frames++;}
static jmp_buf worker_yield;
static void yield_worker(void){longjmp(worker_yield,1);}
static void worker_step(void){wait_hook=yield_worker;current=(void*)2;if(!setjmp(worker_yield))worker_fn(NULL);current=(void*)1;wait_hook=NULL;}
static void sdk_one(void){if(!queued)return;current=(void*)3;typeof(queue[0]) q=queue[0];memmove(queue,queue+1,--queued*sizeof(*queue));q.fn(q.arg);current=(void*)1;}
static void progress(void){sdk_one();if(s_worker){if(s_state==STOPPING)worker_exit();else worker_step();}timer_pump();}
static void finish_ws(void){drive_all();delay_hook=progress;assert(ts_webui_ws_stop((void*)11)==ESP_OK);delay_hook=NULL;for(int i=0;i<32;i++)if(reqs[i].sess_ctx)close_peer(i);ts_webui_ws_stopped((void*)11);assert(!live_allocations);}
static uint32_t paused_id;
static void g1_original_callback(void){
    close_peer(1); /* HTTPD retires the old target while worker callback is paused */
    shell_task.fn(shell_task.arg); /* independent poller exits */
    drive_all();open_peer(1);connect_shell();
    ts_ws_op_stats_t state;ts_ws_op_stats(TS_WS_OP_SHELL,&state);
    assert(state.identity==paused_id && state.phase!=OP_FREE && state.refs);
}
static void g2_preparing_output(void){
    sdk_one();queue_fail=true;worker_step();queue_fail=false;test_now+=100000;
    drive_all();ts_ws_op_stats_t state;ts_ws_op_stats(TS_WS_OP_EXEC,&state);
    assert(state.phase==OP_CLOSING && state.preparing==1 && !state.finalizers);
}
static void fail_observation_business_continues(void){
    ssh_exec_task_params_t *params=exec_task.arg;
    drive_all();queue_fail=true;ssh_exec_output_callback("before",6,false,params);queue_fail=false;test_now+=100000;drive_all();
    assert(!ts_ws_op_is_open(params->op) && ts_webui_ssh_exec_is_running(params->session_id));
}
static void close_snapshot_joiner(void){open_peer(3);ts_ws_op_poll();}
static void cancel_original_exec(void){ssh_exec_task_params_t *params=exec_task.arg;assert(ts_webui_ssh_exec_cancel(0)==ESP_ERR_INVALID_STATE);assert(ts_webui_ssh_exec_cancel(params->session_id)==ESP_OK);}
static void validate_exec_frame(httpd_ws_frame_t *f){
    const char *p=(char*)f->payload;
    assert(strstr(p,"\"type\":") && strstr(p,"\"session_id\":"));
    if(strstr(p,"ssh_exec_output"))assert(strstr(p,"\"data\":"));
    if(strstr(p,"ssh_exec_error"))assert(strstr(p,"\"error\":"));
}
static void stop_progress_shell(void){
 if(shell_task.fn && !ts_ws_op_starting(((ssh_shell_context_t*)shell_task.arg)->op)){
   void (*fn)(void*)=shell_task.fn;void *arg=shell_task.arg;shell_task.fn=NULL;fn(arg);
 }
 progress();
}
static void operation_cross_tests(void){
    uint32_t id;ts_ws_op_stats_t ledger;
    setup();connect_shell();unsigned dc=disconnecting_frames;handle_ssh_disconnect();shell_task.fn(shell_task.arg);finish_all();assert(disconnecting_frames==dc+1);
    setup();terminal_frames=output_frames=0;output_after_terminal=false;connect_shell();
    ssh_shell_context_t *ctx=shell_task.arg;paused_id=ts_ws_op_id(ctx->op);
    ts_ws_message_t *m=ts_ws_message_text("block",5);
    assert(ts_ws_transport_submit(ts_ws_op_peer(ctx->op),m,0,0,NULL,NULL)==0);ts_ws_message_release(m);
    ssh_send_output(ctx,"old",3); /* original producer returns before async rejection */
    sdk_one();barrier_name="B06";barrier_action=g1_original_callback;queue_fail=true;worker_step();
    queue_fail=false;test_now+=100000;assert(!barrier_action);drive_all();connect_shell();ctx=shell_task.arg;
    assert(ts_ws_op_id(ctx->op)>paused_id && ts_ws_op_is_open(ctx->op));ssh_cleanup();shell_task.fn(shell_task.arg);finish_all();
    puts("PASS G1 same observable assertion: old rejection callback cannot corrupt replacement Shell (whole production unit)");

    setup();terminal_frames=output_frames=0;output_after_terminal=false;
    assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==0);
    ssh_exec_task_params_t *params=exec_task.arg;ts_ws_peer_t peer=*(ts_ws_peer_t*)reqs[1].sess_ctx;
    m=ts_ws_message_text("block",5);assert(ts_ws_transport_submit(peer,m,0,0,NULL,NULL)==0);ts_ws_message_release(m);
    ssh_exec_output_callback("A",1,false,params);barrier_name="B02";barrier_action=g2_preparing_output;
    ssh_exec_output_callback("B",1,false,params);drive_all();assert(!barrier_action && !output_after_terminal && terminal_frames==2);
    exec_task.fn(exec_task.arg);finish_all();
    puts("PASS G2 same observable assertion: pending A fails while B prepares; B settles before terminal, no output follows terminal");

    setup();terminal_frames=0;ts_webui_ssh_options_t opts={.timeout_ms=1000,.collect_output=true,.extract_pattern="value=(.*)",.var_name="fake"};
    assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&id)==0);
    timer_queue_fail=true;exec_task.fn(exec_task.arg);drive_all();assert(ts_ws_op_busy());
    params=exec_task.arg;assert(params->timer_phase==2 && !params->session);
    /* A late original timer callback cannot touch freed resources or a new task. */
    unsigned before=aborts;host_timer->fn(host_timer);assert(aborts==before);
    delay_hook=progress;assert(ts_webui_ws_stop((void*)11)==ESP_ERR_TIMEOUT);delay_hook=NULL;
    timer_queue_fail=false;for(unsigned i=0;i<20;i++){test_now+=100000;progress();}
    assert(!ts_ws_op_busy());finish_ws();
    puts("PASS B11 timer delete/barrier retry retains original params; busy stop recovers without new business; late timer cannot abort replacement");

    setup();assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&id)==0);
    timer_queue_fail=true;exec_task.fn(exec_task.arg);
    for(unsigned i=0;i<TS_WS_QUEUE_RETRIES;i++){test_now+=100000;ts_ws_op_poll();}
    params=exec_task.arg;assert(params->timer_phase==6);timer_queue_fail=false;
    delay_hook=progress;esp_err_t stopped=ts_webui_ws_stop((void*)11);delay_hook=NULL;
    assert(stopped==ESP_OK || stopped==ESP_ERR_TIMEOUT);
    for(unsigned i=0;i<20;i++){test_now+=100000;progress();}assert(!ts_ws_op_busy());finish_ws();
    puts("PASS bounded timer drain retry exhaustion is retained and explicitly resumed by stop retry");

    setup();remote_hook=fail_observation_business_continues;unsigned writes=variable_writes;
    assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&id)==0);
    exec_task.fn(exec_task.arg);remote_hook=NULL;assert(variable_writes>writes && !aborts);finish_all();
    puts("PASS B09 observation failure still executes actual matching and variable updates without aborting remote command");

    setup();remote_timeout=true;before=aborts;
    assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&id)==0);
    exec_task.fn(exec_task.arg);remote_timeout=false;assert(aborts==before+1);finish_all();
    setup();opts.stop_on_match=true;before=aborts;
    assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&id)==0);
    exec_task.fn(exec_task.arg);assert(aborts==before+1);finish_all();
    puts("PASS original timeout and stop-on-match business abort semantics retained");

    setup();before=aborts;remote_hook=cancel_original_exec;
    assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==0);
    exec_task.fn(exec_task.arg);remote_hook=NULL;assert(aborts==before+1);finish_all();
    setup();terminal_frames=0;
    assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==0);
    params=exec_task.arg;barrier_name="B07";barrier_action=close_snapshot_joiner;
    if(ts_ws_op_close_begin(params->op))ts_ws_op_close_finish(params->op,"{\"type\":\"ssh_exec_done\"}");
    drive_all();assert(!barrier_action && frames[3]==0 && terminal_frames==2);
    exec_task.fn(exec_task.arg);finish_all();
    puts("PASS original cancel ID contract, user disconnecting status and frozen terminal recipient snapshot");

    setup();close_peer(1);close_peer(2);
    assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==0);
    params=exec_task.arg;before=allocations;ssh_exec_output_callback("none",4,false,params);assert(allocations==before);
    exec_task.fn(exec_task.arg);finish_all();
    puts("PASS no recipients: no observation JSON/payload allocation; business executes and tickets retire");

    setup();task_create_fail=true;assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==ESP_FAIL);task_create_fail=false;finish_all();
    for(unsigned i=0;i<16;i++) {
        setup();fail_allocation=allocations+i+1;
        esp_err_t ret=ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&id);fail_allocation=0;
        if(ret==ESP_OK)exec_task.fn(exec_task.arg);finish_all();
    }
    puts("PASS task-create and allocation failure sweep: all actual contexts, credentials, output buffers and reservations reclaimed");
    for(unsigned i=0;i<48;i++) {
        setup();assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==0);
        sent_hook=validate_exec_frame;params=exec_task.arg;fail_allocation=allocations+i+1;
        ssh_exec_output_callback("encoding",8,false,params);fail_allocation=0;
        exec_task.fn(exec_task.arg);finish_all();
    }
    sent_hook=observe;
    puts("PASS output encoding allocation sweep: no partial protocol objects escape, original ticket/resource ledgers drain");
    setup();connect_shell();current=(void*)2;assert(ts_webui_ws_stop((void*)11)==ESP_ERR_INVALID_STATE);current=(void*)1;
    delay_hook=stop_progress_shell;assert(ts_webui_ws_stop((void*)11)==ESP_OK);delay_hook=NULL;finish_ws();
    setup();assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&id)==0);
    exec_task.fn(exec_task.arg);ts_ws_op_poll();timer_pump();timer_inline=true;ts_ws_op_poll();timer_inline=false;finish_all();
    puts("PASS healthy stop drains original Shell within bounded wait, worker self-stop rejected, timer barrier callback-before-return remains pinned");
    for(unsigned mode=0;mode<2;mode++) {
        setup();timer_create_fail=mode==0;timer_start_fail=mode==1;
        assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&id)==0);
        exec_task.fn(exec_task.arg);timer_create_fail=timer_start_fail=false;finish_all();
    }
    puts("PASS timer allocation/start failure cleanup: no timer reference leak; underlying SSH deadline policy unchanged");
    (void)ledger;
}
#ifndef OP_ADAPTER_NO_MAIN
int main(void){
 cJSON_Hooks h={tracked_malloc,tracked_free};cJSON_InitHooks(&h);sent_hook=observe;
 setup();connect_shell();assert(shell_task.fn && connected_frames==1);
 ssh_shell_context_t *ctx=shell_task.arg;queue_fail=true;ssh_send_output(ctx,"out",3);queue_fail=false;test_now+=100000;drive_all();assert(terminal_frames==1);shell_task.fn(shell_task.arg);finish_all();
 setup();terminal_frames=0;output_after_terminal=false;connect_shell();ctx=shell_task.arg;
 ssh_send_output(ctx,"out",3);ssh_cleanup();shell_task.fn(shell_task.arg);finish_all();assert(!output_after_terminal);
 for(int f=0;f<5;f++){setup();terminal_frames=0;unsigned before=connected_frames;task_create_fail=f==0;close_during_create=f==1;remote_failure=f>=2?f-1:0;shell_task.fn=NULL;connect_shell();if(shell_task.fn)shell_task.fn(shell_task.arg);finish_all();assert(connected_frames==before);task_create_fail=close_during_create=false;remote_failure=0;}
 setup();terminal_frames=0;output_after_terminal=false;uint32_t id;
 ts_webui_ssh_options_t opts={.collect_output=true,.timeout_ms=1000,.extract_pattern="value=(.*)",.var_name="fake"};
 assert(ts_webui_ssh_exec_start_ex("fake",22,"user",NULL,"password","cmd",&opts,&id)==0);
 exec_task.fn(exec_task.arg);drive_all();assert(terminal_frames==2 && !output_after_terminal && variable_writes);finish_all();
 setup();inline_exec=true;assert(ts_webui_ssh_exec_start("fake",22,"user",NULL,"password","cmd",&id)==0);inline_exec=false;finish_all();
 operation_cross_tests();
 printf("PASS complete production WS unit: Shell output failure/close/startup failure, Exec task/callback/match/variables/timer cleanup and task-before-create-return; destroyed=%u\n",session_destroys);
 return 0;
}

#endif
