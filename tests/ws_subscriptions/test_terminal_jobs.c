#define OP_ADAPTER_NO_MAIN
#include "test_operation_adapter.c"
static int scenario;
static void dispatch_terminal(int fd,const char *text){incoming=text;reqs[fd].method=1;current=(void*)3;assert(ws_handler(&reqs[fd])==0);current=(void*)1;incoming=NULL;}
static void during_command(const ts_console_exec_context_t *ctx){
 assert(ctx->remote);
 if(scenario==0){
  assert(!ctx->cancelled(ctx->user_data));ctx->output("ready\n",6,ctx->user_data);
  dispatch_terminal(2,"{\"type\":\"ping\"}");assert(strstr(payloads[2],"pong"));
  dispatch_terminal(1,"{\"type\":\"terminal_input\",\"data\":\"second\"}");assert(strstr(payloads[1],"already running"));
  dispatch_terminal(1,"{\"type\":\"terminal_interrupt\"}");assert(ctx->cancelled(ctx->user_data));
 }else{
  close_peer(1);assert(ctx->cancelled(ctx->user_data));
  reqs[1]=(httpd_req_t){.handle=(void*)11,.fd=1};current=(void*)3;assert(add_client(&reqs[1],WS_CLIENT_TYPE_EVENT)==0);
  assert(start_terminal_session(&reqs[1],NULL)==ESP_FAIL);current=(void*)1;
  ctx->output("must-not-reach-reused-fd",24,ctx->user_data);
 }
}
int main(void){
 cJSON_Hooks h={tracked_malloc,tracked_free};cJSON_InitHooks(&h);
 for(scenario=0;scenario<2;scenario++){
  setup();s_terminal_client_fd=-1;s_output_mutex=xSemaphoreCreateMutex();s_terminal_output_buf=tracked_malloc(TERMINAL_OUTPUT_BUF_SIZE);s_terminal_output_len=0;
  current=(void*)3;assert(start_terminal_session(&reqs[1],NULL)==0);current=(void*)1;
  dispatch_terminal(1,"{\"type\":\"terminal_input\",\"data\":\"forward-fixture\"}");
  assert(exec_task.fn==terminal_job_task && !((terminal_job_t*)exec_task.arg)->finished);console_exec_hook=during_command;
  exec_task.fn(exec_task.arg);console_exec_hook=NULL;drive_all();
  if(!scenario)assert(strstr(payloads[1],"done")&&!ts_ws_op_busy());
  else{assert(!strstr(payloads[1],"must-not-reach"));current=(void*)3;assert(start_terminal_session(&reqs[1],NULL)==0);current=(void*)1;}
  tracked_free(s_terminal_output_buf);s_terminal_output_buf=NULL;vSemaphoreDelete(s_output_mutex);s_output_mutex=NULL;finish_ws();
 }
 puts("PASS real terminal worker: request returns before execution; HTTP ping/stop remain available; duplicate command rejected; disconnected/reused fd isolated; new terminal admitted after retirement");
}
