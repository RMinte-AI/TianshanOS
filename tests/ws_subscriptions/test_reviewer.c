#define OP_ADAPTER_NO_MAIN
#include "test_operation_adapter.c"
static unsigned observed_topics[8];
static void observe_topic(httpd_ws_frame_t*f){for(unsigned i=0;i<8;i++){char match[80];snprintf(match,sizeof(match),"\"topic\":\"%s\"",topics[i].name);if(strstr((char*)f->payload,match))observed_topics[i]++;}}
int main(void){
 cJSON_Hooks hooks={tracked_malloc,tracked_free};cJSON_InitHooks(&hooks);unsigned a;
 setup();uint32_t id;assert(ts_webui_ssh_exec_start("fake",22,"u",NULL,"p","cmd",&id)==0);
 delay_hook=progress;assert(ts_webui_ws_stop((void*)11)==ESP_ERR_INVALID_STATE);delay_hook=NULL;assert(s_worker && s_state==DRAINING);
 reqs[1].method=1;incoming="{\"type\":\"ping\"}";current=(void*)3;assert(ws_handler(&reqs[1])==0 && strstr(payloads[1],"pong"));
 incoming="{\"type\":\"terminal_input\",\"data\":\"new-command\"}";assert(ws_handler(&reqs[1])==0 && strstr(payloads[1],"stopping"));current=(void*)1;
 exec_task.fn(exec_task.arg);for(unsigned i=0;i<20;i++)progress();assert(!ts_ws_op_busy());finish_ws();
 puts("PASS R1/R4 real busy stop: existing task and full fanout continue with no new business message; retry succeeds");
 setup();assert(ts_webui_ws_stop((void*)11)==ESP_ERR_TIMEOUT && s_worker);for(unsigned i=0;i<20;i++)progress();finish_ws();
 puts("PASS HTTPD barrier timeout retains worker and retry state");
 setup();ts_ws_peer_t peer;assert(ts_ws_peer_get((void*)11,1,&peer));
 s_clients[0].active=true;s_clients[0].fd=1;s_clients[0].hd=peer.server;
 set_client_role(peer,WS_CLIENT_TYPE_LOG,5);
 httpd_ws_frame_t log={.payload=(uint8_t*)"old-log",.len=7};
 a=frames[1];assert(ts_ws_transport_log(&log,3)==0);
 current=(void*)3;start_terminal_session(&reqs[1]);current=(void*)1;
 assert(s_clients[0].type==WS_CLIENT_TYPE_TERMINAL);pump();assert(frames[1]==a+1); /* welcome only */
 set_client_role(peer,WS_CLIENT_TYPE_LOG,5);
 assert(ts_ws_transport_log(&log,3)==0);set_client_role(peer,WS_CLIENT_TYPE_EVENT,0);set_client_role(peer,WS_CLIENT_TYPE_LOG,5);
 a=frames[1];pump();assert(frames[1]==a); /* old generation cannot revive */
 assert(ts_ws_transport_log(&log,3)==0);pump();assert(frames[1]==a+1);
 finish_ws();s_terminal_client_fd=-1;
 assert(ts_webui_log_stream_enable(true)==0);remove_log_result=ESP_ERR_INVALID_STATE;
 assert(ts_webui_log_stream_enable(false)==ESP_ERR_INVALID_STATE && s_log_callback_handle && !s_log_streaming_enabled);
 assert(ts_webui_log_stream_enable(true)==0 && s_log_streaming_enabled);remove_log_result=0;assert(ts_webui_log_stream_enable(false)==0);
 puts("PASS R3: real role change drops old queued logs, including unsubscribe/resubscribe ABA");

 setup();assert(ts_ws_peer_get((void*)11,1,&peer));uint64_t serial[8];for(unsigned i=0;i<8;i++){serial[i]=s_stats.serialized[i];subscribe(peer,topics[i].name,10000);}
 sent_hook=observe_topic;
 for(unsigned cycle=0;cycle<100;cycle++) {test_now+=10000000;run_batch();pump();}
 for(unsigned i=0;i<8;i++) {
  assert(observed_topics[i]>50 && observed_topics[i]==s_stats.serialized[i]-serial[i]);
  printf("R5 observed %s: %u delivered frames\n",topics[i].name,observed_topics[i]);
 }
 /* Freeing capacity services still-due targets without waiting their next interval. */
 for(unsigned i=0;i<8;i++)serial[i]=s_stats.serialized[i];
 unsigned improved=0;run_batch();pump();for(unsigned i=0;i<8;i++)improved+=s_stats.serialized[i]>serial[i];assert(improved>=2);
 ts_ws_message_t*held[6];cJSON*j=cJSON_CreateObject();for(unsigned i=0;i<6;i++){held[i]=ts_ws_message_json("held",j,0);assert(held[i]);}
 test_now+=10000000;unsigned before=total_reads();run_batch();assert(total_reads()==before);
 for(unsigned i=0;i<6;i++)ts_ws_message_release(held[i]);cJSON_Delete(j);
 run_batch();pump();assert(total_reads()>before);
 ts_ws_peer_t pinned[TS_WS_TOPIC_TX_SLOTS];for(unsigned i=0;i<TS_WS_TOPIC_TX_SLOTS;i++)pinned[i]=peer;
 ts_ws_reservation_t reserved;assert(ts_ws_reserve(pinned,TS_WS_TOPIC_TX_SLOTS,false,TS_WS_FRAME_BYTES,&reserved)==0);
 test_now+=10000000;before=total_reads();run_batch();assert(total_reads()==before);
 ts_ws_reservation_release(&reserved);run_batch();pump();assert(total_reads()>before);
 char*large=malloc(TS_WS_LEGACY_BYTES);memset(large,'x',TS_WS_LEGACY_BYTES);
 ts_ws_message_t *m1=ts_ws_message_text(large,TS_WS_LEGACY_BYTES-1);ts_ws_message_t *m2=ts_ws_message_text(large,TS_WS_LEGACY_BYTES-TS_WS_POWER_BUDGET-1);assert(m1&&m2);free(large);
 test_now+=10000000;before=total_reads();run_batch();assert(total_reads()==before);
 ts_ws_message_release(m1);ts_ws_message_release(m2);run_batch();pump();assert(total_reads()>before);finish_ws();
 puts("PASS R5: all eight topics progress under six-slot pressure; no API collection without message/descriptor/byte capacity");
}
