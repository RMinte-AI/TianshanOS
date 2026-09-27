#define OP_ADAPTER_NO_MAIN
#define PROJECT_REAL_DRIVER
#include "test_operation_adapter.c"
static void emit(httpd_ws_frame_t*f){
 printf("{\"frame\":%.*s,\"fd\":%d,\"bytes\":\"",(int)f->len,f->payload,sent_fd);
 for(size_t i=0;i<f->len;i++){unsigned char c=f->payload[i];if(c=='"'||c=='\\')putchar('\\');if(c<32)printf("\\u%04x",c);else putchar(c);}puts("\"}");
}
static void close_during_error(void){ssh_shell_context_t *ctx=shell_task.arg;ssh_send_output(ctx,"last output",11);ssh_close(ctx->op,"closed","SSH session closed");}
static void fail_error_encoding(void){fail_allocation=allocations+1;}
int main(void){
 cJSON_Hooks h={tracked_malloc,tracked_free};cJSON_InitHooks(&h);sent_hook=emit;setup();
 char line[4096];while(fgets(line,sizeof(line),stdin)){
  cJSON *j=cJSON_Parse(line);assert(j);const char *cmd=cJSON_GetStringValue(cJSON_GetObjectItem(j,"cmd"));
  if(cmd && !strcmp(cmd,"finish")){ssh_cleanup();if(shell_task.fn){shell_task.fn(shell_task.arg);shell_task.fn=NULL;}drive_all();cJSON_Delete(j);finish_all();assert(!driver_channels&&!driver_sessions&&!driver_sockets);puts("{\"finished\":true}");return 0;}
  if(cmd && !strcmp(cmd,"eof")){driver_eof=true;shell_task.fn(shell_task.arg);shell_task.fn=NULL;drive_all();}
  else if(cmd && !strcmp(cmd,"power")){
   ts_power_policy_status_t status={.state=TS_POWER_POLICY_STATE_LOW_VOLTAGE,.current_voltage=10.5f,.countdown_remaining_sec=30};
   ts_event_t event={.id=TS_POWER_POLICY_EVENT_LOW_VOLTAGE,.data=&status};
   power_policy_event_handler(&event,NULL);drive_all();
  }
  else if(cmd && !strcmp(cmd,"output")){ssh_shell_context_t *ctx=shell_task.arg;ssh_send_output(ctx,"stream",6);}
  else if(cmd && !strcmp(cmd,"error_close")){barrier_name="request_error_encode";barrier_action=close_during_error;}
  else if(cmd && !strcmp(cmd,"write_again"))driver_write_again=true;
  else if(cmd && !strcmp(cmd,"close_again"))driver_close_again=true;
  else if(cmd && !strcmp(cmd,"reset_driver")){driver_write_fail=driver_write_again=driver_resize_reject=driver_close_again=driver_eof=false;driver_phase=0;}
  else if(cmd && !strcmp(cmd,"setup_fail"))driver_phase=4;
  else if(cmd && !strcmp(cmd,"queue_fail"))queue_fail=true;
  else if(cmd && !strcmp(cmd,"queue_resume")){queue_fail=false;test_now+=100000;drive_all();}
  else if(cmd && !strcmp(cmd,"send_fail"))send_fail=true;
  else if(cmd && !strcmp(cmd,"send_resume"))send_fail=false;
  else if(cmd && !strcmp(cmd,"encode_fail")){barrier_name="request_error_encode";barrier_action=fail_error_encoding;}
  else if(cmd && !strcmp(cmd,"encode_resume"))fail_allocation=0;
  else if(cmd && !strcmp(cmd,"write_fail"))driver_write_fail=true;
  else if(cmd && !strcmp(cmd,"resize_reject"))driver_resize_reject=true;
  else if(cmd && !strcmp(cmd,"drain"))drive_all();
  else {
   int fd=1;cJSON *f=cJSON_GetObjectItem(j,"fd");if(f)fd=f->valueint;
   cJSON *msg=cJSON_GetObjectItem(j,"message");char *raw=cJSON_PrintUnformatted(msg);incoming=raw;reqs[fd].method=1;current=(void*)3;int ret=ws_handler(&reqs[fd]);current=(void*)1;incoming=NULL;cJSON_free(raw);assert(ret==0);drive_all();
  }
  if(shell_task.fn && (((ssh_shell_context_t*)shell_task.arg)->disconnect_requested || !ts_ws_op_is_open(((ssh_shell_context_t*)shell_task.arg)->op))){shell_task.fn(shell_task.arg);shell_task.fn=NULL;drive_all();}
  ts_ws_op_stats_t st;ts_ws_op_stats(TS_WS_OP_SHELL,&st);
  printf("{\"identity\":%u,\"state\":%d,\"writes\":%d,\"live\":%u,\"channels\":%d,\"refs\":%u,\"preparing\":%u,\"unsettled\":%u}\n",st.identity,st.phase,driver_writes,live_allocations,driver_channels,st.refs,st.preparing,st.unsettled);fflush(stdout);cJSON_Delete(j);
 }
 return 2;
}
