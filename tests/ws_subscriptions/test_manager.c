#define TEST_ALLOC
#define TEST_FAIL_EVENTS
#include "platform.h"
static void *tracked_malloc(size_t n);
static void tracked_free(void *p);
#define malloc tracked_malloc
#define free tracked_free
static void capacity_done(uint64_t r,uint64_t d,esp_err_t e){(void)r;(void)d;(void)e;}
static void test_point(const char *name);
#define TS_WS_TEST_POINT(name) test_point(name)
#define shutdown test_shutdown
#define s_lock transport_lock
#define s_server transport_server
#define s_sequence transport_sequence
#include "ts_ws_transport.c"
#undef shutdown
#undef s_lock
#undef s_server
#undef s_sequence
#include "ts_ws_subscriptions.c"
#include "ts_ws_operation.c"
#undef malloc
#undef free
static unsigned allocations, live_allocations;
static int fail_allocation;
static void *tracked_malloc(size_t n) {
 allocations++; if(fail_allocation>0 && allocations==(unsigned)fail_allocation)return NULL;
 void*p=malloc(n);if(p)live_allocations++;return p;
}
static void tracked_free(void*p) {if(p){assert(live_allocations);live_allocations--;free(p);}}
int test_result_frees, test_event_fail_at, test_event_registers;
int64_t test_now=10000000;
static TaskHandle_t current=(void*)1;
static void(*worker_fn)(void*);
static void(*delay_hook)(void);
static void(*api_hook)(void);
static bool queue_fail, immediate, send_fail, task_fail, api_business_error;
static unsigned shutdowns;
static int sent_fd,send_fail_fd=-1;
int test_shutdown(int fd,int how){(void)how;assert(fd>0);shutdowns++;return 0;}
static void(*wait_hook)(void);
static const char *barrier_name;
static void(*barrier_action)(void);
static void test_point(const char *name) {if(barrier_action && !strcmp(name,barrier_name)){void(*fn)(void)=barrier_action;barrier_action=NULL;fn();}}
static unsigned reads[7], frames[32], notifications, queued;
static void (*sent_hook)(httpd_ws_frame_t*);
static char payloads[64][TS_WS_FRAME_BYTES];
static struct {void(*fn)(void*);void*arg;} queue[64];
TaskHandle_t xTaskGetCurrentTaskHandle(void) {return current;}
int xTaskCreate(void(*fn)(void*),const char*n,unsigned stack,void*a,unsigned p,TaskHandle_t*out) {(void)n;(void)stack;(void)a;(void)p;if(task_fail)return 0;worker_fn=fn;*out=(void*)2;return pdPASS;}
void xTaskNotifyGive(TaskHandle_t t) {assert(t==(void*)2);notifications++;}
unsigned ulTaskNotifyTake(int clear,TickType_t ticks) {(void)clear;(void)ticks;assert(wait_hook);wait_hook();unsigned n=notifications;notifications=0;return n;}
void vTaskDelete(TaskHandle_t t) {(void)t;}
void vTaskDelay(TickType_t t) {test_now+=(int64_t)t*1000;if(delay_hook)delay_hook();}
int httpd_ws_get_fd_info(httpd_handle_t h,int fd) {(void)h;(void)fd;return HTTPD_WS_CLIENT_WEBSOCKET;}
int httpd_ws_send_frame_async(httpd_handle_t h,int fd,httpd_ws_frame_t*f) {(void)h;assert(fd>=0&&fd<32);frames[fd]++;sent_fd=fd;if(sent_hook && !send_fail && send_fail_fd!=fd)sent_hook(f);memcpy(payloads[fd],f->payload,f->len);payloads[fd][f->len]=0;return send_fail || send_fail_fd==fd?ESP_FAIL:0;}
int httpd_queue_work(httpd_handle_t h,void(*fn)(void*),void*a) {(void)h;if(queue_fail)return ESP_FAIL;if(immediate) {TaskHandle_t saved=current;current=(void*)3;fn(a);current=saved;}else{assert(queued<64);queue[queued++]=(typeof(queue[0])){fn,a};}return 0;}
int ts_api_call(const char*method,const cJSON*p,ts_api_result_t*r) {(void)p;for(int i=0;i<7;i++)if(!strcmp(metrics[i],method))reads[i]++;if(api_hook)api_hook();r->data=cJSON_CreateObject();r->message=malloc(8);r->code=api_business_error?1:0;return 0;}
static void pump(void) {
 while(queued || ts_ws_transport_needs_flush()){
  current=(void*)1;ts_ws_transport_flush();
  if(!queued)break;
  assert(queued<=2); /* at most one TX control item plus a lifecycle barrier */
  current=(void*)3;typeof(queue[0]) q=queue[0];memmove(queue,queue+1,--queued*sizeof(queue[0]));q.fn(q.arg);
 }
 current=(void*)1;
}
static void worker_exit(void) {if(s_worker){current=(void*)2;worker_fn(NULL);current=(void*)1;}}
static void closed(ts_ws_peer_t peer) {ts_ws_client_disconnected(peer);}
static httpd_req_t reqs[32];
static ts_ws_peer_t open_peer(int fd) {current=(void*)3;reqs[fd]=(httpd_req_t){.handle=(void*)11,.fd=fd};ts_ws_peer_t p;assert(ts_ws_peer_open(&reqs[fd],&p)==0);current=(void*)1;return p;}
static void close_peer(int fd) {current=(void*)3;reqs[fd].free_ctx(reqs[fd].sess_ctx);reqs[fd].sess_ctx=NULL;current=(void*)1;}
static void start(void) {assert(ts_ws_transport_start((void*)11,closed)==0);assert(ts_ws_subscriptions_init()==0);}
static void stop(void) {delay_hook=worker_exit;assert(ts_ws_subscriptions_deinit()==0);delay_hook=NULL;pump();assert(ts_ws_transport_stop((void*)11,5)==0);for(int i=0;i<32;i++)if(reqs[i].sess_ctx)close_peer(i);ts_ws_transport_stopped((void*)11);assert(!s_outstanding);for(int i=0;i<TS_WS_MESSAGE_SLOTS;i++)assert(!s_messages[i].refs);}
static void subscribe(ts_ws_peer_t p,const char*t,int interval) {cJSON*j=cJSON_CreateObject();if(interval>=0)cJSON_AddNumberToObject(j,"interval",interval);assert(ts_ws_subscribe(p,t,j)==0);cJSON_Delete(j);}
static unsigned total_reads(void) {unsigned n=0;for(int i=0;i<7;i++)n+=reads[i];return n;}
static ts_ws_peer_t cancel_peer;
static void stop_during_api(void) {api_hook=NULL;assert(ts_ws_subscriptions_deinit()==ESP_ERR_TIMEOUT);assert(s_state==STOPPING);assert(ts_ws_subscriptions_init()==ESP_ERR_INVALID_STATE);}
static void wake_at_wait(void) {wait_hook=NULL;subscribe(cancel_peer,"system.cpu",1000);assert(notifications);s_state=STOPPING;}
static void resub_at_barrier(void) {ts_ws_unsubscribe(cancel_peer,"system.cpu");subscribe(cancel_peer,"system.cpu",10000);}
static void reuse_at_barrier(void) {close_peer(1);cancel_peer=open_peer(1);subscribe(cancel_peer,"system.cpu",10000);}
static int cross_action;
static void cross_mutation(void) {
 api_hook=NULL;
 if(cross_action==0)ts_ws_unsubscribe(cancel_peer,"system.cpu");
 if(cross_action==1)resub_at_barrier();
 if(cross_action==2)close_peer(1);
 if(cross_action==3)reuse_at_barrier();
 if(cross_action==4){TaskHandle_t saved=current;current=(void*)1;assert(ts_ws_subscriptions_deinit()==ESP_ERR_TIMEOUT);assert(ts_ws_subscriptions_init()==ESP_ERR_INVALID_STATE);current=saved;}
}
static void cancel_during_api(void) {api_hook=NULL;ts_ws_unsubscribe(cancel_peer,"system.dashboard");}
int main(void) {
 cJSON_Hooks hooks={tracked_malloc,tracked_free};cJSON_InitHooks(&hooks);
 start();run_batch();assert(total_reads()==0);assert(next_wait()==portMAX_DELAY);
 ts_ws_peer_t a=open_peer(1),b=open_peer(2);open_peer(3);
 subscribe(a,"system.dashboard",1000);subscribe(b,"system.dashboard",10000);
 run_batch();assert(total_reads()==0);test_now+=1000000;run_batch();assert(total_reads()==7);assert(queued==1);pump();assert(frames[1]==1&&frames[2]==0&&frames[3]==0);
 unsigned n=total_reads();run_batch();assert(total_reads()==n);
 subscribe(a,"system.dashboard",1000);run_batch();assert(total_reads()==n); /* idempotent */
 ts_ws_unsubscribe(a,"system.dashboard");test_now+=1000000;run_batch();assert(total_reads()==n);
 ts_ws_unsubscribe(b,"system.dashboard");test_now+=10000000;run_batch();assert(total_reads()==n);assert(next_wait()==portMAX_DELAY);
 puts("PASS T01 T02 T05 T06 T07 T08: demand, independent intervals, targeted fanout");
 subscribe(a,"system.dashboard",1000);subscribe(b,"system.cpu",1000);test_now+=1000000;unsigned cpu=reads[0];run_batch();assert(reads[0]==cpu+1);assert(queued==1 && s_outstanding==2);
 ts_ws_unsubscribe(a,"system.dashboard");subscribe(a,"system.dashboard",10000);pump();assert(frames[1]==1&&frames[2]==1);assert(!s_subs[0].pending);
 puts("PASS T09 T12 T16: shared batch, old revision cannot send or update new state");
 ts_ws_unsubscribe(a,"system.dashboard");ts_ws_unsubscribe(b,"system.cpu");
 subscribe(a,"system.info",5000);test_now+=5000000;run_batch();assert(queued==1);close_peer(1);a=open_peer(1);pump();assert(frames[1]==1);
 puts("PASS T11 T14: standalone info, queued delivery cannot cross fd reuse");
 subscribe(a,"system.dashboard",1000);cancel_peer=a;api_hook=cancel_during_api;test_now+=1000000;n=total_reads();run_batch();assert(total_reads()==n+1);assert(queued==0);
 puts("PASS T13: cancellation during API skips remaining metrics");
 subscribe(a,"system.cpu",1000);test_now+=1000000;queue_fail=true;run_batch();assert(!s_outstanding);n=total_reads();run_batch();assert(total_reads()==n);queue_fail=false;
 test_now+=1000000;immediate=true;run_batch();assert(!s_outstanding);immediate=false;assert(frames[1]==2);
 puts("PASS T17 T19 T20: queue failure backoff, completion-before-return ownership");
 ts_ws_unsubscribe(a,"system.cpu");subscribe(a,"config.pack.validated",0);
 cJSON*j=cJSON_Parse("{\"path\":\"first\"}");ts_ws_broadcast_to_topic("config.pack.validated",j);cJSON_SetValuestring(cJSON_GetObjectItem(j,"path"),"second");ts_ws_broadcast_to_topic("config.pack.validated",j);cJSON_Delete(j);
 run_batch();assert(queued==1);pump();assert(strstr(payloads[1],"first"));run_batch();pump();assert(strstr(payloads[1],"second"));
 puts("PASS T23: ordered operation events own borrowed data");
 const char*bad[]={"0","-1","1.5","\"1000\"","4294967296"};for(unsigned i=0;i<5;i++){j=cJSON_CreateObject();cJSON_AddItemToObject(j,"interval",cJSON_Parse(bad[i]));assert(ts_ws_subscribe(a,"system.cpu",j)==ESP_ERR_INVALID_ARG);cJSON_Delete(j);}
 assert(wait_ticks(1)==1);assert(wait_ticks(INT64_MAX)>0);assert(wait_ticks(INT64_MAX)<portMAX_DELAY);
 puts("PASS T10 T34: interval validation and nonzero capped monotonic waits");
 stop();assert(live_allocations==0);start();a=open_peer(1);b=open_peer(2);open_peer(3);
 subscribe(a,"system.cpu",1000);test_now+=1000000;run_batch();unsigned before=frames[1];
 assert(ts_ws_subscriptions_deinit()==ESP_ERR_TIMEOUT);assert(s_state==STOPPING && s_worker);
 assert(ts_ws_transport_stop((void*)11,2)==ESP_ERR_TIMEOUT);assert(transport_server.handle==(void*)11);
 pump();assert(frames[1]==before);delay_hook=worker_exit;assert(ts_ws_subscriptions_deinit()==0);delay_hook=NULL;
 assert(ts_ws_transport_stop((void*)11,2)==0);for(int i=1;i<=3;i++)close_peer(i);ts_ws_transport_stopped((void*)11);
 start();ts_ws_peer_t old=a;a=open_peer(1);ts_ws_peer_close(old);assert(ts_ws_peer_get(a.server,1,&b)&&ts_ws_peer_equal(a,b));
 assert(ts_ws_transport_start((void*)12,closed)==ESP_ERR_INVALID_STATE);
 puts("PASS T15 T27 T28: no second HTTP owner, reused handle epoch isolation, stop timeout retains live resources and retries");
 current=(void*)3;assert(ts_ws_subscriptions_deinit()==ESP_ERR_INVALID_STATE);current=(void*)2;assert(ts_ws_subscriptions_deinit()==ESP_ERR_INVALID_STATE);current=(void*)1;
 subscribe(a,"system.dashboard",1000);test_now+=1000000;api_hook=stop_during_api;run_batch();assert(!s_outstanding);stop();assert(live_allocations==0);
 puts("PASS T27 T30: stop during API rejects new instance; worker/HTTPD self-wait refused");
 start();a=open_peer(1);subscribe(a,"system.dashboard",1000);
 for(unsigned point=1;point<180;point++) {
   test_now+=1000000;allocations=0;fail_allocation=point;run_batch();fail_allocation=0;pump();if(live_allocations!=1)fprintf(stderr,"point=%u live=%u alloc=%u\n",point,live_allocations,allocations);assert(live_allocations==1); /* session context only */
 }
 assert(test_result_frees>100);stop();assert(live_allocations==0);
 puts("PASS T21 T22: allocation failure sweep across production JSON/TX paths; per-call results freed; allocation baseline restored");
 start();a=open_peer(1);b=open_peer(2);open_peer(3);
 subscribe(a,"device.status",2000);const char raw[]={ '{','"','x','"',':','1','}' };
 ts_event_t ev={.data=(void*)raw,.data_size=sizeof(raw)};event_handler(&ev,(void*)8);event_handler(&ev,(void*)8);
 run_batch();pump();before=frames[1];run_batch();assert(!queued);test_now+=2000000;run_batch();pump();assert(frames[1]==before+1);
 httpd_ws_frame_t global={.payload=(uint8_t*)"{\"alarm\":true}",.len=14};unsigned f1=frames[1],f2=frames[2],f3=frames[3];assert(ts_ws_transport_broadcast(&global)==0);pump();assert(frames[1]==f1+1&&frames[2]==f2+1&&frames[3]==f3+1);
 puts("PASS T24 T25: bounded non-NUL event parse, retained paced events, original global recipient set");
 ts_ws_unsubscribe(a,"device.status");subscribe(a,"config.pack.validated",0);
 j=cJSON_CreateObject();uint64_t dropped=s_stats.event_dropped;for(int i=0;i<EVENT_SLOTS+1;i++)ts_ws_broadcast_to_topic("config.pack.validated",j);assert(s_stats.event_dropped==dropped+1);cJSON_Delete(j);
 for(int i=0;i<EVENT_SLOTS;i++){run_batch();pump();}stop();assert(!live_allocations);
 puts("PASS T23: bounded event FIFO full is counted, accepted results remain ordered");
 assert(ts_ws_transport_start((void*)11,closed)==0);task_fail=true;assert(ts_ws_subscriptions_init()==ESP_ERR_NO_MEM);task_fail=false;assert(s_state==OFF);
 test_event_registers=0;test_event_fail_at=2;assert(ts_ws_subscriptions_init()==ESP_ERR_NO_MEM);test_event_fail_at=0;assert(s_state==OFF);assert(ts_ws_subscriptions_init()==0);stop();assert(!live_allocations);
 puts("PASS T26: task and partial handler registration rollback permit retry");
 start();cancel_peer=open_peer(1);notifications=0;wait_hook=wake_at_wait;current=(void*)2;worker_fn(NULL);current=(void*)1;assert(!s_worker);stop();assert(!live_allocations);
 puts("PASS T29 T33: repeated lifecycle and notification at wait boundary");
 start();a=open_peer(1);b=open_peer(2);
 subscribe(a,"system.cpu",1000);
 ts_ws_message_t *held[TS_WS_MESSAGE_SLOTS];j=cJSON_CreateObject();
 for(int i=0;i<TS_WS_MESSAGE_SLOTS-2;i++){held[i]=ts_ws_message_json("system.cpu",j,0);assert(held[i]);}
 test_now+=1000000;n=total_reads();run_batch();assert(total_reads()==n);assert(next_wait()>=1000);
 ts_ws_message_t *alarm=ts_ws_message_text("{}",2);assert(alarm);assert(ts_ws_transport_submit(b,alarm,0,0,NULL,NULL)==0);ts_ws_message_release(alarm);pump();
 for(int i=0;i<TS_WS_MESSAGE_SLOTS-2;i++)ts_ws_message_release(held[i]);cJSON_Delete(j);
 run_batch();assert(queued==1);test_now+=10000000;pump();n=total_reads();run_batch();assert(total_reads()==n);test_now+=1000000;run_batch();pump();
 char *big=malloc(TS_WS_LEGACY_BYTES);memset(big,'x',TS_WS_LEGACY_BYTES);
 ts_ws_message_t *large=ts_ws_message_text(big,TS_WS_LEGACY_BYTES-1);assert(large);assert(!ts_ws_message_text(big,TS_WS_LEGACY_BYTES));ts_ws_message_release(large);free(big);
 ts_ws_message_t *one=ts_ws_message_text("{}",2);assert(one);
 j=cJSON_CreateObject();ts_ws_message_t *topic_message=ts_ws_message_json("capacity",j,0);cJSON_Delete(j);assert(topic_message);
 for(int i=0;i<TS_WS_TOPIC_TX_SLOTS;i++)assert(ts_ws_transport_submit(a,topic_message,0,0,NULL,capacity_done)==0);
 ts_ws_message_release(topic_message);
 for(int i=0;i<TS_WS_CONNECTIONS;i++)assert(ts_ws_transport_submit(a,one,0,0,NULL,NULL)==0);
 assert(ts_ws_transport_submit(a,one,0,0,NULL,NULL)==ESP_ERR_NO_MEM);
 ts_ws_message_release(one);pump();
 ts_ws_transport_stats_t stats;ts_ws_transport_get_stats(&stats);assert(!stats.jobs&&!stats.bytes);assert(stats.jobs_high==TS_WS_TX_SLOTS);assert(stats.bytes_high<=TS_WS_TOTAL_BYTES);assert(stats.queued>stats.success);
 stop();assert(!live_allocations);
 puts("PASS T18 T19 T20: bounded slots/bytes, reserved global capacity, delayed completion cadence, separate queue/send counters");
 for(int point=0;point<2;point++)for(int action=0;action<2;action++) {
  start();cancel_peer=open_peer(1);subscribe(cancel_peer,"system.cpu",1000);test_now+=1000000;
  n=total_reads();before=frames[1];barrier_name=point?"sent":"snapshot";barrier_action=action?reuse_at_barrier:resub_at_barrier;
  run_batch();pump();assert(total_reads()==n+(point?1:0));assert(frames[1]==before+(point?1:0));
  subscription_t *current_sub=NULL;for(int i=0;i<MAX_SUBSCRIPTIONS;i++)if(s_subs[i].active)current_sub=&s_subs[i];
  assert(current_sub && current_sub->interval==10000 && !current_sub->pending && current_sub->last_success==0);
  stop();assert(!live_allocations);
 }
 puts("PASS T09 T14 T16: snapshot-before-collection and send-before-completion barriers with resubscribe/fd reuse");
 assert(after_ms(INT64_MAX-100,UINT32_MAX)==INT64_MAX);
 start();a=open_peer(1);queue_fail=true;assert(ts_ws_transport_quiesce(a.server,1)==ESP_FAIL);queue_fail=false;assert(!transport_server.outstanding);
 assert(ts_ws_transport_quiesce(a.server,1)==ESP_ERR_TIMEOUT);assert(queued==1&&transport_server.outstanding==1);assert(ts_ws_transport_quiesce(a.server,1)==ESP_ERR_TIMEOUT);assert(queued==1);pump();assert(ts_ws_transport_quiesce(a.server,1)==0);stop();
 start();a=open_peer(1);immediate=true;assert(ts_ws_transport_quiesce(a.server,1)==0);immediate=false;assert(!transport_server.outstanding);stop();assert(!live_allocations);
 puts("PASS T17 T28: HTTP handler barrier publication, timeout retention, single retry and callback-before-return");
 start();a=open_peer(1);subscribe(a,"system.dashboard",1000);api_business_error=true;test_now+=1000000;run_batch();assert(!queued&&!s_outstanding&&live_allocations==1);api_business_error=false;
 test_now+=1000000;run_batch();send_fail=true;pump();send_fail=false;assert(shutdowns==1);assert(!ts_ws_peer_get(a.server,a.fd,&b));close_peer(1);stop();assert(!live_allocations);
 puts("PASS T04 T20 T21: business-error results freed; send failure shuts down only the pinned session and invalidates demand");
 for(int point=0;point<4;point++)for(cross_action=0;cross_action<5;cross_action++) {
  start();cancel_peer=open_peer(1);subscribe(cancel_peer,"system.cpu",1000);test_now+=1000000;
  before=frames[1];n=total_reads();
  if(point==0||point==3){barrier_name=point==0?"snapshot":"sent";barrier_action=cross_mutation;}
  if(point==1)api_hook=cross_mutation;
  run_batch();if(point==2)cross_mutation();pump();
  assert(frames[1]==before+(point==3?1:0));assert(total_reads()==n+(point==0?0:1));
  stop();assert(!live_allocations);
 }
 puts("PASS cross matrix: 4 barriers x 5 mutations (cancel, resubscribe, disconnect, fd reuse, stop/restart exclusion); restart succeeds after every drain");

 j=cJSON_CreateObject();cJSON *services=cJSON_AddObjectToObject(j,"services"),*array=cJSON_AddArrayToObject(services,"services");
 char service_name[32];memset(service_name,1,31);service_name[31]=0;
 for(int i=0;i<32;i++){cJSON *item=cJSON_CreateObject();cJSON_AddStringToObject(item,"name",service_name);cJSON_AddStringToObject(item,"state","STARTING");cJSON_AddStringToObject(item,"phase","SECURITY");cJSON_AddBoolToObject(item,"healthy",true);cJSON_AddNumberToObject(item,"start_time_ms",UINT32_MAX);cJSON_AddNumberToObject(item,"start_duration_ms",UINT32_MAX);cJSON_AddItemToArray(array,item);}
 ts_ws_message_t *max_services=ts_ws_message_json("system.dashboard",j,INT64_MAX/1000000);assert(max_services);printf("PASS T18 configured 32-service escaped-name fixture: %zu bytes within %u-byte frame budget\n",max_services->len,TS_WS_FRAME_BYTES);ts_ws_message_release(max_services);cJSON_Delete(j);assert(!live_allocations);





 return 0;
}
