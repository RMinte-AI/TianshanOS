#define OP_ADAPTER_NO_MAIN
#include "test_operation_adapter.c"
static unsigned mode;
static char delivered[32][40000];static size_t lengths[32];static cJSON *finals[32];
static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void capture(httpd_ws_frame_t *f){
 cJSON *m=cJSON_ParseWithLength((char*)f->payload,f->len);assert(m);
 const char *type=cJSON_GetObjectItem(m,"type")->valuestring;
 if(!strcmp(type,"ssh_exec_output")){
  const char *b=cJSON_GetObjectItem(m,"data_b64")->valuestring;
  unsigned v=0,bits=0;size_t start=lengths[sent_fd];
  for(;*b && *b!='=';b++){const char *p=strchr(alphabet,*b);assert(p);v=(v<<6)|(unsigned)(p-alphabet);bits+=6;if(bits>=8){bits-=8;delivered[sent_fd][lengths[sent_fd]++]=(char)(v>>bits);}}
  assert(lengths[sent_fd]-start==(size_t)cJSON_GetObjectItem(m,"output_length")->valuedouble);
 }else if(!strcmp(type,"ssh_exec_done")||!strcmp(type,"ssh_exec_error")||!strcmp(type,"ssh_exec_cancelled")){
  if(finals[sent_fd])cJSON_Delete(finals[sent_fd]);finals[sent_fd]=cJSON_Duplicate(m,true);
 }
 cJSON_Delete(m);
}
static void produce(void){
 ssh_exec_task_params_t *p=exec_task.arg;
 if(mode>=4){char data[512];memset(data,'L',sizeof(data));for(int i=0;i<64;i++)ssh_exec_output_callback(data,sizeof(data),false,p);return;}
 const char *text=mode==3?"中文\n\"quoted\"\\\0尾":"xxxxxxxx";
 size_t n=mode==3?sizeof("中文\n\"quoted\"\\\0尾")-1:4096;
 for(size_t i=0;i<n;){size_t step=mode==3?1:512;if(step>n-i)step=n-i;
  char data[512];if(mode==3)memcpy(data,text+i,step);else memset(data,'x',step);
  ssh_exec_output_callback(data,step,mode==3 && i%2,p);i+=step;
  if(mode==1 || mode==3)drive_all();
 }
 assert(ts_ws_op_is_open(p->op));
}
int main(void){
 cJSON_Hooks h={tracked_malloc,tracked_free};cJSON_InitHooks(&h);
 for(mode=0;mode<6;mode++){
  setup();memset(lengths,0,sizeof(lengths));sent_hook=capture;remote_hook=produce;
  if(mode==2)send_fail_fd=2;
  ts_webui_ssh_receipt_t receipt;ts_webui_ssh_options_t options={.receipt=&receipt,.collect_output=true,.extract_pattern=mode==4?"value=(.*)":mode==5?"missing=(.*);":NULL,.max_output_size=mode>=4?1024:65536};uint32_t id;unsigned before=commands_submitted;
  assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&options,&id)==ESP_OK);
  char *status=ts_webui_ssh_exec_status(id,receipt.boot_id,receipt.result_key,NULL);assert(status&&strstr(status,"ssh_exec_running"));tracked_free(status);
  assert(!ts_webui_ssh_exec_status(id,receipt.boot_id,"00000000000000000000000000000000",NULL));
  assert(!ts_webui_ssh_exec_status(id,"00000000000000000000000000000000",receipt.result_key,NULL));
  exec_task.fn(exec_task.arg);drive_all();assert(commands_submitted==before+1);
  status=ts_webui_ssh_exec_status(id,receipt.boot_id,receipt.result_key,NULL);assert(status&&strstr(status,"ssh_exec_done")&&strstr(status,"\"execution_status\":\"success\""));
  cJSON *result=cJSON_Parse(status);tracked_free(status);assert(cJSON_GetObjectItem(result,"exit_code")->valueint==0);
  if(mode<=2){assert(lengths[1]==4105 && !memcmp(delivered[1],"xxxxxxxx",8));assert(!memcmp(delivered[1]+4096,"value=42\n",9));assert(finals[1]);assert(!cJSON_IsTrue(cJSON_GetObjectItem(result,"output_incomplete")));}
  if(mode==3){const char text[]="中文\n\"quoted\"\\\0尾value=42\n";assert(lengths[1]==sizeof(text)-1 && !memcmp(delivered[1],text,sizeof(text)-1));}
  if(mode==4){assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"conditions_complete")));assert(!strcmp(cJSON_GetObjectItem(result,"extracted")->valuestring,"42"));assert(!strcmp(cJSON_GetObjectItem(result,"status")->valuestring,"success"));}
  if(mode==5){assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"output_incomplete")));assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"collection_truncated")));assert(cJSON_IsFalse(cJSON_GetObjectItem(result,"conditions_complete")));assert(!strcmp(cJSON_GetObjectItem(result,"status")->valuestring,"unknown"));assert(lengths[1]<=EXEC_OUTPUT_BYTES);}
  cJSON_Delete(result);for(unsigned i=0;i<32;i++)if(finals[i]){cJSON_Delete(finals[i]);finals[i]=NULL;}
  remote_hook=NULL;send_fail_fd=-1;sent_hook=observe;finish_all();
 }
 puts("PASS actual EXEC pipeline: same 4105-byte burst/paced output, failed observer isolation, binary/UTF-8/stream order, bounded overflow, collection truncation, final result retained, wrong boot/capability rejected, each command submitted once");
}
