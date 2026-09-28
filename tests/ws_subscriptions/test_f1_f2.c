#define OP_ADAPTER_NO_MAIN
#include "test_operation_adapter.c"
static unsigned powers,errors,outputs[32],terminals[32],logs,protection_order[64],protection_n,observed_topics[8];
static void observe_all(httpd_ws_frame_t*f){const char*p=(char*)f->payload;
 for(unsigned i=0;i<8;i++){char match[80];snprintf(match,sizeof(match),"\"topic\":\"%s\"",topics[i].name);if(strstr(p,match))observed_topics[i]++;}
 if(strstr(p,"power_event")){powers++;if(protection_n<64){cJSON*j=cJSON_Parse(p);protection_order[protection_n++]=cJSON_GetObjectItem(j,"protection_count")->valueint;cJSON_Delete(j);}}
 if(strstr(p,"ssh_exec_output")||strstr(p,"ssh_output"))outputs[sent_fd]++;
 if(strstr(p,"ssh_exec_error")||strstr(p,"\"status\":\"error\""))errors++;
 if(strstr(p,"ssh_exec_done")||strstr(p,"ssh_exec_error"))terminals[sent_fd]++;
 if(!strcmp(p,"log"))logs++;
}
static void power_change(void){ts_power_policy_status_t s={.state=TS_POWER_POLICY_STATE_PROTECTED,.current_voltage=10.0f,.protection_count=1};ts_event_t e={.id=TS_POWER_POLICY_EVENT_PROTECTED,.data=&s,.data_size=sizeof(s)};power_policy_event_handler(&e,NULL);}
static void shell_end(void){ssh_cleanup();shell_task.fn(shell_task.arg);drive_all();}
int main(int argc,char**argv){
 assert(argc==2);cJSON_Hooks hooks={tracked_malloc,tracked_free};cJSON_InitHooks(&hooks);setup();close_peer(2);sent_hook=observe_all;
 if(!strcmp(argv[1],"f1") || !strcmp(argv[1],"late") || !strcmp(argv[1],"early")){
  connect_shell();ssh_shell_context_t *ctx=shell_task.arg;
  bool late=!strcmp(argv[1],"late"),early=!strcmp(argv[1],"early");
  if(late){ts_ws_message_t*m=ts_ws_message_text("block",5);assert(ts_ws_transport_submit(ts_ws_op_peer(ctx->op),m,0,0,NULL,NULL)==0);ts_ws_message_release(m);}
  immediate=early;send_fail=early;queue_fail=!late&&!early;ssh_send_output(ctx,"chunk",5);
  if(late){sdk_one();queue_fail=true;worker_step();}
  queue_fail=send_fail=false;immediate=false;test_now+=100000;drive_all();
  assert(!ts_ws_op_is_open(ctx->op));if(!early)assert(errors==1);
  shell_end();finish_ws();puts("PASS F1: actual Shell output immediate/deferred failure settles original operation; early callbacks remain safe");return 0;
 }
 if(!strcmp(argv[1],"f2")){
  ts_ws_peer_t p;assert(ts_ws_peer_get((void*)11,1,&p));ts_ws_peer_log_level(p,5);
  httpd_ws_frame_t f={.type=1,.payload=(uint8_t*)"log",.len=3};assert(ts_ws_transport_log(&f,1)==0);assert(ts_ws_transport_log(&f,1)==0);
  power_change();pump();assert(powers==1);finish_ws();puts("PASS F2: protection transition survives ordinary log saturation");return 0;
 }
 if(!strcmp(argv[1],"identity")){
  connect_shell();ssh_shell_context_t *ctx=shell_task.arg;unsigned id=ts_ws_op_id(ctx->op);
  ssh_send_output(ctx,"old",3);close_peer(1);open_peer(1);assert(!ts_ws_op_create(TS_WS_OP_SHELL,*((ts_ws_peer_t*)reqs[1].sess_ctx),NULL));
  pump();assert(outputs[1]==0);shell_end();connect_shell();ctx=shell_task.arg;assert(ts_ws_op_id(ctx->op)>id);
  settle_target(id,1,ESP_FAIL);assert(ts_ws_op_is_open(ctx->op));shell_end();finish_ws();puts("PASS original peer and operation identity isolation across reconnect");return 0;
 }
 if(!strcmp(argv[1],"exec") || !strcmp(argv[1],"continuous")){
  bool partial=!strcmp(argv[1],"exec");if(partial)open_peer(2);uint32_t id;
  assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==0);ssh_exec_task_params_t *params=exec_task.arg;
  if(!partial)queue_fail=true;
  ssh_exec_output_callback("part",4,false,params);
  if(partial){if(ts_ws_op_close_begin(params->op))ts_ws_op_close_finish(params->op,"{\"type\":\"ssh_exec_done\"}");sdk_one();queue_fail=true;worker_step();}
  queue_fail=false;test_now+=100000;drive_all();assert(terminals[1]==1);if(partial)assert(outputs[1]==1 && outputs[2]==0 && terminals[2]==1);
  unsigned before=frames[1];size_t collected=params->output_len;
  ssh_exec_output_callback("later",5,false,params);drive_all();assert(frames[1]==before && params->output_len==collected+5 && !aborts);
  assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",NULL)==ESP_ERR_INVALID_STATE);
  exec_task.fn(exec_task.arg);finish_ws();assert(frames[1]==before);puts("PASS actual Exec callback: partial failure settles each target; observation closes while business collection continues without remote abort or replay");return 0;
 }
 if(!strcmp(argv[1],"class") || !strcmp(argv[1],"stop") || !strcmp(argv[1],"timeout")){
  connect_shell();ssh_shell_context_t *ctx=shell_task.arg;ssh_send_output(ctx,"last",4);
  unsigned ordinary=0;for(unsigned i=TS_WS_TOPIC_TX_SLOTS;i<TS_WS_TX_SLOTS;i++)ordinary+=s_deliveries[i].refs>0;assert(ordinary==1);
  ts_ws_transport_cancel_topics();ts_ws_op_stats_t stats;ts_ws_op_stats(TS_WS_OP_SHELL,&stats);assert(stats.unsettled==1);
  if(strcmp(argv[1],"class")){
   power_change();bool timeout=!strcmp(argv[1],"timeout");if(!timeout)delay_hook=progress;
   assert(ts_webui_ws_stop((void*)11)==ESP_ERR_TIMEOUT);delay_hook=NULL;assert(s_worker && s_state==DRAINING);
   for(unsigned i=0;i<12;i++)progress();assert(outputs[1]==1 && powers==1);
  }
  shell_end();finish_ws();puts("PASS ordinary class retained with completion; busy/timeout stop preserves output and protection progress");return 0;
 }
 if(!strcmp(argv[1],"power_retry")){
  open_peer(2);queue_fail=true;power_change();assert(s_tx_stats.power_accepted==1 && !powers);
  worker_step();queue_fail=false;test_now+=100000;worker_step();sdk_one();worker_step();sdk_one();assert(powers==2);
  send_fail_fd=2;power_change();pump();send_fail_fd=-1;assert(powers==3 && s_tx_stats.power_failed==1);finish_ws();
  puts("PASS protection queue recovery without new messages; partial socket failure never replays successful recipient");return 0;
 }
 if(!strcmp(argv[1],"capacity")){
  ts_ws_peer_t p;assert(ts_ws_peer_get((void*)11,1,&p));
  ts_ws_message_t*m=ts_ws_message_text("log",3);for(unsigned i=0;i<TS_WS_CONNECTIONS;i++)assert(ts_ws_transport_submit(p,m,0,0,NULL,NULL)==0);
  ts_ws_message_t*m2=ts_ws_message_text("two",3);assert(m2);
  unsigned before=allocations;fail_allocation=allocations+1;power_change();assert(allocations==before);fail_allocation=0;
  ts_ws_message_release(m);ts_ws_message_release(m2);pump();assert(powers==1);
  char*big=malloc(TS_WS_LEGACY_BYTES);memset(big,'x',TS_WS_LEGACY_BYTES);m=ts_ws_message_text(big,TS_WS_LEGACY_BYTES-1);m2=ts_ws_message_text(big,TS_WS_LEGACY_BYTES-TS_WS_POWER_BUDGET-1);free(big);assert(m&&m2);
  power_change();pump();assert(powers==2 && s_tx_stats.bytes<=TS_WS_TOTAL_BYTES);ts_ws_message_release(m);ts_ws_message_release(m2);
  ts_ws_reservation_t r[TS_WS_POWER_TRANSITIONS];for(unsigned i=0;i<TS_WS_POWER_TRANSITIONS;i++)assert(ts_ws_power_reserve(false,&r[i])==0);
  before=allocations;uint64_t rejected=s_tx_stats.power_rejected;power_change();assert(s_tx_stats.power_rejected==rejected+1 && allocations==before);
  ts_ws_reservation_t tick;assert(ts_ws_power_reserve(true,&tick)==0);ts_ws_reservation_release(&tick);
  for(unsigned i=0;i<TS_WS_POWER_TRANSITIONS;i++)ts_ws_reservation_release(&r[i]);
  ts_ws_peer_t peers[TS_WS_TOPIC_TX_SLOTS];for(unsigned i=0;i<TS_WS_TOPIC_TX_SLOTS;i++)peers[i]=p;
  assert(reserve_kind(peers,TS_WS_TOPIC_TX_SLOTS,MSG_POWER,TS_WS_POWER_BYTES,&r[0])==0);rejected=s_tx_stats.power_rejected;power_change();assert(s_tx_stats.power_rejected==rejected+1);ts_ws_reservation_release(&r[0]);
  power_change();pump();assert(powers==3);finish_ws();puts("PASS separate ordinary slot/descriptor/byte exhaustion cannot consume power quota; power overload counted without construction");return 0;
 }
 if(!strcmp(argv[1],"retry_limit")){
  queue_fail=true;power_change();for(unsigned i=1;i<TS_WS_QUEUE_RETRIES;i++){test_now+=100000;worker_step();}
  assert(!ts_ws_transport_needs_flush() && s_tx_stats.power_failed==1 && s_tx_stats.power_settled==1 && s_tx_stats.rejected==TS_WS_QUEUE_RETRIES && !s_tx_stats.jobs && !s_tx_stats.bytes);queue_fail=false;finish_ws();puts("PASS persistent SDK rejection has bounded retry and observable terminal settlement");return 0;
 }


 if(!strcmp(argv[1],"power_order")){
  queue_fail=true;
  ts_power_policy_status_t status={.state=TS_POWER_POLICY_STATE_LOW_VOLTAGE,.current_voltage=11.5f};ts_event_t e={.id=TS_POWER_POLICY_EVENT_COUNTDOWN_TICK,.data=&status,.data_size=sizeof(status)};
  status.protection_count=99;power_policy_event_handler(&e,NULL);power_policy_event_handler(&e,NULL);assert(s_tx_stats.power_rejected==1);
  for(unsigned i=1;i<=4;i++){status.protection_count=i;e.id=TS_POWER_POLICY_EVENT_STATE_CHANGED;power_policy_event_handler(&e,NULL);}
  assert(s_tx_stats.power_accepted==5);status.protection_count=5;power_policy_event_handler(&e,NULL);assert(s_tx_stats.power_rejected==2);
  queue_fail=false;test_now+=100000;worker_step();pump();assert(protection_n==5 && protection_order[0]==99);for(unsigned i=1;i<=4;i++)assert(protection_order[i]==i);
  finish_ws();puts("PASS ticks cannot evict transitions; four accepted transitions retain order, fifth overload is explicit");return 0;
 }

 if(!strcmp(argv[1],"fair")){
  ts_ws_peer_t p;assert(ts_ws_peer_get((void*)11,1,&p));ts_ws_peer_log_level(p,5);for(unsigned i=0;i<8;i++)subscribe(p,topics[i].name,1000);
  httpd_ws_frame_t f={.type=1,.payload=(uint8_t*)"log",.len=3};
  for(unsigned i=0;i<300;i++){
   test_now+=1000000;ts_ws_transport_log(&f,1);if(i%3==0)power_change();worker_step();sdk_one();assert(s_tx_stats.bytes<=TS_WS_TOTAL_BYTES && s_tx_stats.jobs<=TS_WS_ALL_TX_SLOTS);
  }
  pump();for(unsigned i=0;i<8;i++){printf("fair %s=%u\n",topics[i].name,observed_topics[i]);assert(observed_topics[i]>0);}assert(powers>0&&logs>0);finish_ws();puts("PASS bounded sustained log/protection/topic competition: all eligible classes progress");return 0;
 }
 return 2;
}
