#define OP_ADAPTER_NO_MAIN
#include "test_operation_adapter.c"
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
int main(int argc,char **argv){
 cJSON_Hooks h={tracked_malloc,tracked_free};cJSON_InitHooks(&h);sent_hook=observe;
 if(argc==2){bool ok=!strcmp(argv[1],"v1")?ownership_case():early_cancel_case();printf("%s business_contract=%s\n",argv[1],ok?"PASS":"FAIL");return ok?0:1;}
 assert(ownership_case());assert(early_cancel_case());puts("PASS actual WS ownership and accepted pre-task cancellation");return 0;
}
