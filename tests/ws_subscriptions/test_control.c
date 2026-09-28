#define OP_ADAPTER_NO_MAIN
#include "test_operation_adapter.c"
#define TS_API_ERR_NOT_FOUND 2
#define TS_API_ERR_INTERNAL 7
static void ts_api_result_error(ts_api_result_t *r,int code,const char *message){(void)message;r->code=code;}
static void ts_api_result_ok(ts_api_result_t *r,cJSON *data){r->code=0;r->data=data;}
#include "api_cancel.inc"
static void dispatch(unsigned fd,const char *text){incoming=text;reqs[fd].method=1;current=(void*)3;assert(ws_handler(&reqs[fd])==ESP_OK);current=(void*)1;incoming=NULL;}
static void shell_on(unsigned fd){cJSON *j=cJSON_Parse("{\"host\":\"fake\",\"user\":\"fake\"}");current=(void*)3;handle_ssh_connect(&reqs[fd],j);current=(void*)1;cJSON_Delete(j);}
static bool ownership_case(void){
 setup();connect_shell();ssh_cleanup();shell_task.fn(shell_task.arg);drive_all();shell_on(2);
 unsigned w=control_writes,s=control_signals,r=control_resizes;
 dispatch(1,"{\"type\":\"ssh_input\",\"data\":\"bad\"}");dispatch(1,"{\"type\":\"ssh_signal\",\"signal\":\"INT\"}");dispatch(1,"{\"type\":\"ssh_resize\",\"width\":90,\"height\":30}");dispatch(1,"{\"type\":\"ssh_disconnect\"}");
 bool ok=control_writes==w && control_signals==s && control_resizes==r && !((ssh_shell_context_t*)shell_task.arg)->disconnect_requested;
 ssh_cleanup();shell_task.fn(shell_task.arg);finish_all();return ok;
}
static bool early_cancel_case(void){
 setup();uint32_t id;assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==0);
 unsigned before=commands_submitted;assert(ts_webui_ssh_exec_cancel(id)==ESP_OK);exec_task.fn(exec_task.arg);finish_all();return commands_submitted==before;
}
static uint32_t target_id;
static const char *cancel_stage;
static unsigned cancelled_frames,timeout_frames,match_frames;
static void result_observe(httpd_ws_frame_t *frame){
 observe(frame);const char *s=(char*)frame->payload;
 if(strstr(s,"ssh_exec_cancelled"))cancelled_frames++;
 if(strstr(s,"\"status\":\"timeout\""))timeout_frames++;
 if(strstr(s,"\"status\":\"match_success\""))match_frames++;
}
static void cancel_now(void){
 assert(ts_webui_ssh_exec_cancel(target_id)==ESP_OK);
 assert(ts_webui_ssh_exec_cancel(target_id)==ESP_OK);
 assert(ts_webui_ssh_exec_cancel(target_id+1)==ESP_ERR_INVALID_STATE);
}
static void cancel_at(const char *name){if(cancel_stage && !strcmp(name,cancel_stage)){cancel_stage=NULL;cancel_now();}}
static void ended_rejects(void){
 assert(!ts_webui_ssh_exec_is_running(target_id));
 assert(ts_webui_ssh_exec_cancel(target_id)==ESP_ERR_INVALID_STATE);
}
static void close_borrowed(void){
 ssh_shell_context_t *ctx=shell_task.arg;uint32_t id=ts_ws_op_id(ctx->op);
 ssh_cleanup();shell_task.fn(shell_task.arg);drive_all();shell_on(2);
 ts_ws_op_stats_t state;ts_ws_op_stats(TS_WS_OP_SHELL,&state);
 assert(state.identity==id && state.refs && state.phase!=OP_FREE);
}
static void controls(void){
 setup();connect_shell();unsigned w=control_writes,s=control_signals,r=control_resizes;
 dispatch(1,"{\"type\":\"ssh_input\",\"data\":\"ok\"}");
 dispatch(1,"{\"type\":\"ssh_signal\",\"signal\":\"INT\"}");
 dispatch(1,"{\"type\":\"ssh_resize\",\"width\":90,\"height\":30}");
 assert(control_writes==w+1 && control_signals==s+1 && control_resizes==r+1);
 dispatch(1,"{\"type\":\"ssh_disconnect\"}");assert(((ssh_shell_context_t*)shell_task.arg)->disconnect_requested);
 shell_task.fn(shell_task.arg);finish_all();
 // A borrow survives close/cleanup and prevents reuse until its last access.
 setup();connect_shell();w=control_writes;barrier_name="C01";barrier_action=close_borrowed;
 dispatch(1,"{\"type\":\"ssh_input\",\"data\":\"late\"}");assert(!barrier_action && control_writes==w);
 drive_all();shell_on(2);assert(ts_ws_op_is_open(((ssh_shell_context_t*)shell_task.arg)->op));ssh_cleanup();shell_task.fn(shell_task.arg);finish_all();
 // Reused fd, same HTTPD handle: old peer and old close cannot touch replacement.
 setup();connect_shell();ts_ws_peer_t old=*(ts_ws_peer_t*)reqs[1].sess_ctx;
 close_peer(1);shell_task.fn(shell_task.arg);drive_all();
 reqs[1]=(httpd_req_t){.handle=(void*)11,.fd=1};current=(void*)3;assert(add_client(&reqs[1],WS_CLIENT_TYPE_EVENT)==0);current=(void*)1;connect_shell();
 ts_ws_peer_t replacement=*(ts_ws_peer_t*)reqs[1].sess_ctx,found;
 current=(void*)3;peer_closed(old);current=(void*)1;
 assert(ts_ws_peer_get(replacement.server,replacement.fd,&found) && ts_ws_peer_equal(found,replacement));
 assert(ts_ws_op_is_open(((ssh_shell_context_t*)shell_task.arg)->op));
 httpd_req_t stale=reqs[1];stale.method=1;stale.sess_ctx=&old;incoming="{\"type\":\"ssh_disconnect\"}";
 current=(void*)3;assert(ws_handler(&stale)==ESP_ERR_INVALID_STATE);current=(void*)1;incoming=NULL;
 old=replacement;old.epoch++;assert(!ts_ws_op_shell_control(old));old=replacement;old.server=(void*)12;assert(!ts_ws_op_shell_control(old));
 delay_hook=stop_progress_shell;assert(ts_webui_ws_stop((void*)11)==ESP_OK);delay_hook=NULL;finish_ws();
 puts("PASS actual WS controls: all four foreign controls rejected, owner controls work; borrowed original pinned, fd/epoch/server reuse and late close isolated; owner stop drains");
}
static void cancellation_phases(void){
 const char *phases[]={"key","create","connect","auth","submit","C02","C03","C04"};
 for(unsigned i=0;i<sizeof(phases)/sizeof(*phases);i++){
  setup();unsigned before=commands_submitted,c=cancelled_frames,t=timeout_frames;
  assert(ts_webui_ssh_exec_start("fake",22,"u",i==0?"key":NULL,i==0?NULL:"p","cmd",&target_id)==0);
  if(i<5){cancel_stage=phases[i];stage_hook=cancel_at;}else{barrier_name=phases[i];barrier_action=cancel_now;}
  exec_task.fn(exec_task.arg);stage_hook=NULL;assert(!cancel_stage && !barrier_action);drive_all();
  assert(cancelled_frames==c+2 && timeout_frames==t);
  assert(commands_submitted==before+(i==7)); // C04 happens after simulated remote completion, before local freeze
  assert(ts_webui_ssh_exec_cancel(target_id)==ESP_ERR_INVALID_STATE);finish_all();
 }
 setup();unsigned c=cancelled_frames;assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&target_id)==0);
 barrier_name="C05";barrier_action=ended_rejects;exec_task.fn(exec_task.arg);finish_all();assert(cancelled_frames==c && !barrier_action);
 uint32_t old=target_id;
 setup();assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&target_id)==0);assert(target_id!=old);
 assert(ts_webui_ssh_exec_cancel(old)==ESP_ERR_INVALID_STATE);assert(ts_webui_ssh_exec_cancel(0)==ESP_ERR_INVALID_STATE);
 exec_task.fn(exec_task.arg);finish_all();
 puts("PASS accepted cancellation persists through task/key/create/connect/auth; cancel/submit claim both orders and result freeze tested; duplicate/old/zero IDs isolated");
}
static void stop_busy_cancel(void){
 delay_hook=progress;assert(ts_webui_ws_stop((void*)11)==ESP_ERR_INVALID_STATE);delay_hook=NULL;
 assert(!ts_ws_op_cancelled(((ssh_exec_task_params_t*)exec_task.arg)->op));
 cancel_now();
}
static void reason_tests(void){
 immediate=true;
 ts_webui_ssh_options_t opts={.timeout_ms=1000,.collect_output=true,.extract_pattern="value=(.*)",.stop_on_match=true};
 setup();unsigned c=cancelled_frames,t=timeout_frames,m=match_frames;
 assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&target_id)==0);
 remote_hook=cancel_now;exec_task.fn(exec_task.arg);remote_hook=NULL;finish_all();assert(cancelled_frames==c+2 && match_frames==m && timeout_frames==t);
 setup();assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&target_id)==0);
 exec_task.fn(exec_task.arg);finish_all();assert(match_frames==m+2);
 setup();opts.stop_on_match=false;opts.extract_pattern=NULL;t=timeout_frames;c=cancelled_frames;
 assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&target_id)==0);
 remote_timeout=true;exec_task.fn(exec_task.arg);remote_timeout=false;finish_all();assert(timeout_frames==t+2 && cancelled_frames==c);
 immediate=false;setup();assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&target_id)==0);
 remote_hook=stop_busy_cancel;exec_task.fn(exec_task.arg);remote_hook=NULL;
 drive_all();finish_ws();
 puts("PASS USER/TIMEOUT/MATCH keep distinct results; service stop busy does not cancel business, accepted user intent survives stop retry and all resource ledgers drain");
}
static void observe_then_cancel(void){
 fail_observation_business_continues();cancel_now();
}
static void timeout_then_cancel(void){host_timer->fn(host_timer);cancel_now();}
static void intersecting_failures(void){
 setup();unsigned c=cancelled_frames;
 assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&target_id)==0);
 remote_hook=observe_then_cancel;exec_task.fn(exec_task.arg);remote_hook=NULL;
 finish_all();assert(cancelled_frames==c); /* sealed observation failure is not rewritten */
 setup();unsigned t=timeout_frames;c=cancelled_frames;
 ts_webui_ssh_options_t opts={.timeout_ms=1000,.collect_output=true};
 assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&target_id)==0);
 remote_hook=timeout_then_cancel;exec_task.fn(exec_task.arg);remote_hook=NULL;finish_all();
 assert(timeout_frames==t+2 && cancelled_frames==c);
 puts("PASS output failure seals observation without cancelling business; later explicit cancel still reaches original executor without rewriting terminal; earlier timeout retains reason");
}
static void api_controls(void){
 s_op_sequence=0x80000000u; /* valid public uint32 IDs must not pass through signed valueint */
 setup();assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&target_id)==0);
 const char *bad[]={"{}","{\"session_id\":0}","{\"session_id\":-1}","{\"session_id\":1.5}","{\"session_id\":4294967296}","{\"session_id\":\"1\"}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){cJSON *j=cJSON_Parse(bad[i]);ts_api_result_t r={0};assert(api_ssh_cancel(j,&r)!=0 && r.code);cJSON_Delete(j);assert(!ts_ws_op_cancelled(((ssh_exec_task_params_t*)exec_task.arg)->op));}
 cJSON *j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"session_id",target_id);
 ts_api_result_t r={0};assert(api_ssh_cancel(j,&r)==0 && cJSON_IsTrue(cJSON_GetObjectItem(r.data,"cancelled")));cJSON_Delete(r.data);
 exec_task.fn(exec_task.arg);drive_all();r=(ts_api_result_t){0};assert(api_ssh_cancel(j,&r)!=0);cJSON_Delete(j);finish_all();
 puts("PASS actual ssh.cancel API exact-ID validation and accepted-intent response; ended operations reject without wildcard control");
}
int main(int argc,char **argv){
 cJSON_Hooks h={tracked_malloc,tracked_free};cJSON_InitHooks(&h);sent_hook=result_observe;
 if(argc==2){bool ok=!strcmp(argv[1],"v1")?ownership_case():early_cancel_case();printf("%s business_contract=%s\n",argv[1],ok?"PASS":"FAIL");return ok?0:1;}
 assert(ownership_case());assert(early_cancel_case());controls();cancellation_phases();reason_tests();intersecting_failures();api_controls();return 0;
}
