#define OP_ADAPTER_NO_MAIN
#include "test_operation_adapter.c"
#define TS_API_ERR_INVALID_ARG 1
#define TS_API_ERR_NOT_FOUND 2
#define TS_API_ERR_BUSY 4
#define TS_API_ERR_INTERNAL 7
static void ts_api_result_error(ts_api_result_t *r,int code,const char *message){(void)message;r->code=code;}
static void ts_api_result_reason(ts_api_result_t *r,int code,const char *reason,const char *detail){(void)detail;ts_api_result_error(r,code,reason);}
static void ts_api_result_ok(ts_api_result_t *r,cJSON *data){r->code=0;r->data=data;}
#define free tracked_free
#include "exec_api.inc"
#undef free
int main(void){
 cJSON_Hooks h={tracked_malloc,tracked_free};cJSON_InitHooks(&h);
 for(unsigned i=0;i<45;i++){
  setup();cJSON *request=cJSON_Parse("{\"host\":\"fake\",\"user\":\"u\",\"password\":\"p\",\"command\":\"cmd\"}");ts_api_result_t reply={0};
  fail_allocation=allocations+i+1;esp_err_t ret=api_ssh_exec_stream(request,&reply);fail_allocation=0;cJSON_Delete(request);
  if(ret==ESP_OK){
   assert(reply.data);cJSON *id=cJSON_GetObjectItem(reply.data,"session_id");assert(id&&id->valuedouble>0);
   assert(strlen(cJSON_GetObjectItem(reply.data,"boot_id")->valuestring)==32 && strlen(cJSON_GetObjectItem(reply.data,"result_key")->valuestring)==32);
   ts_api_result_t status={0};assert(api_ssh_exec_status(reply.data,&status)==ESP_OK);assert(strstr(cJSON_GetObjectItem(status.data,"type")->valuestring,"running"));cJSON_Delete(status.data);
   status=(ts_api_result_t){0};fail_allocation=allocations+1;assert(api_ssh_exec_status(reply.data,&status)==ESP_ERR_NO_MEM && status.code==TS_API_ERR_INTERNAL);fail_allocation=0;
   exec_task.fn(exec_task.arg);drive_all();status=(ts_api_result_t){0};assert(api_ssh_exec_status(reply.data,&status)==ESP_OK);assert(!strcmp(cJSON_GetObjectItem(status.data,"execution_status")->valuestring,"success"));cJSON_Delete(status.data);
  }else{ts_ws_operation_t *op=ts_ws_op_acquire(TS_WS_OP_EXEC,0);if(op){assert(!ts_ws_op_business_running(op));ts_ws_op_release(op);}}
  cJSON_Delete(reply.data);finish_all();
 }
 setup();ts_webui_ssh_receipt_t first,second;ts_webui_ssh_options_t opts={.receipt=&first,.collect_output=true};uint32_t a,b;
 assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&a)==ESP_OK);exec_task.fn(exec_task.arg);drive_all();
 opts.receipt=&second;assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&opts,&b)==ESP_OK);
 char *result=ts_webui_ssh_exec_status(a,first.boot_id,first.result_key,NULL);assert(result&&strstr(result,"ssh_exec_done"));tracked_free(result);
 exec_task.fn(exec_task.arg);drive_all();assert(!ts_webui_ssh_exec_status(a,first.boot_id,first.result_key,NULL));finish_all();
 puts("PASS real initiation/status API: allocation sweep prepares receipt before submit, exact owned lookup, current plus latest only, completion evicts older result");
}
