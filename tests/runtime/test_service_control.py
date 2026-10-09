"""Production API -> admission -> executor -> SSH wrapping -> service registry.
Only storage, SSH transport and RTOS scheduling are simulated. IDF_PATH supplies cJSON.
"""
from pathlib import Path
import os,re,subprocess
sdk=Path(os.environ['IDF_PATH'])/'components/json/cJSON'
source=Path('components/ts_automation/src/ts_action_manager.c').read_text()
def fn(name,src=source):
 m=re.search(r'^(?:static )?(?:esp_err_t|void|bool|int)\s+'+name+r'\([^;]+?\)\s*\{',src,re.M);assert m,name
 return src[m.start():src.index('\n}',m.start())+2]
def typedef(name):
 end=source.index('} '+name+';')+len('} '+name+';');start=source.rfind('typedef struct {',0,end)
 return source[start:end]
base=Path('tests/runtime/test_service_protocol.c').read_text().split('int main(void)')[0]
# Keep the real registry, watcher and probe; change only the transport response.
base=base.replace('static int replace_on_log;', 'static _Atomic int hold_connect, connect_entered;static int replace_on_log;static char sent_command[8192];static int launch_count, fail_alloc, fail_send;')
base=base.replace('return calloc(n,s);','return fail_alloc && --fail_alloc==0 ? NULL : calloc(n,s);')
base=base.replace('if(strstr(cmd,"grep -qF"))', 'if(strstr(cmd,"mkdir /tmp/ts_nohup_")){snprintf(sent_command,sizeof(sent_command),"%s",cmd);launch_count++;remote_running=1;token="STARTED abcd-1234:123:456\\n";}else if(strstr(cmd,"grep -qF"))')
base=base.replace(' ++connect_count;', ' ++connect_count;\n if(hold_connect){connect_entered=1;while(hold_connect){struct timespec d={0,1000000};nanosleep(&d,NULL);}}')
base=base.replace('static _Thread_local TaskHandle_t self;', 'static _Thread_local TaskHandle_t self;static int draining;')
base=base.replace('assert(!task);pthread_exit(NULL);', 'assert(!task);if(!draining)pthread_exit(NULL);')
code=base+r'''
#include "ts_action_manager.h"
#include "ts_api.h"
#define MAX_SSH_HOSTS 8
#define SERVICE_QUEUE_TIMEOUT_MS 30000
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef void *QueueHandle_t;
#define TS_STRDUP_PSRAM strdup
'''+typedef('action_manager_ctx_t')+'\n'+typedef('action_binding_t')+r'''
static action_manager_ctx_t context,*s_ctx=&context;
static int run_immediately;
static void action_executor_task(void*);
static ts_action_queue_entry_t queue[16];static unsigned queue_count;
static int xQueueSend(void*q,void*item,unsigned wait){assert(wait==0);if(fail_send||queue_count==16)return 0;queue[queue_count++]=*(ts_action_queue_entry_t*)item;if(run_immediately){draining=1;action_executor_task(NULL);draining=0;}return 1;}
static int xQueueReceive(void*q,void*out,unsigned wait){if(!queue_count){s_ctx->running=false;return 0;}*(ts_action_queue_entry_t*)out=queue[0];memmove(queue,queue+1,--queue_count*sizeof(*queue));return 1;}
static void completion_release(void*p){assert(!p);}
static void vTaskDelay(unsigned n){assert(!"direct service control has no action delay");}
esp_err_t ts_ssh_disconnect(ts_ssh_session_t s){return ESP_OK;}
esp_err_t ts_ssh_commands_config_update_exec_time(const char*id){return ESP_OK;}
static ts_auto_value_t expansion_value;
esp_err_t ts_variable_get(const char*name,ts_auto_value_t*out){if(strcmp(name,"value"))return ESP_ERR_NOT_FOUND;*out=expansion_value;return ESP_OK;}
'''
for name in ['ts_action_snapshot_release','snapshot_command','ts_action_get_ssh_host_ex','ts_action_get_ssh_host','action_finished','direct_service_admit','direct_service_finished','entry_finished','ts_action_manager_quiesce','ts_action_manager_resume','ts_action_service_control','ts_action_expand_variables','exec_ssh_ref_bound','execute_bound_ref']:
 code+='\n'+fn(name)
code+='\nstatic esp_err_t execute_action_internal(const ts_auto_action_t*a,ts_action_result_t*r){assert(a->type==TS_AUTO_ACT_SSH_CMD_REF);return execute_bound_ref(a,r);}\n'
for name in ['execute_service_control','action_executor_task','ts_action_cancel_all']:code+='\n'+fn(name)
api=Path('components/ts_api/src/ts_api_ssh.c').read_text();core=Path('components/ts_api/src/ts_api.c').read_text();automation=Path('components/ts_api/src/ts_api_automation.c').read_text()
for name,src in [('ts_api_result_ok',core),('ts_api_result_error',core),('ts_api_service_control',api),('api_ssh_service_start',api),('service_request',automation)]:code+='\n'+fn(name,src)
code+=r'''
static uint32_t submit(ts_service_operation_t kind){
 cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,"command_id",command.id);
 if(kind==TS_SERVICE_VERIFY)cJSON_AddBoolToObject(params,"verify",true);
 ts_api_result_t result={0};int before=connect_count;
 esp_err_t ret=kind==TS_SERVICE_START?api_ssh_service_start(params,&result):service_request(params,&result,kind==TS_SERVICE_STOP);
 assert(ret==ESP_OK&&result.code==TS_API_OK&&connect_count==before);
 uint32_t id=cJSON_GetObjectItem(result.data,"operation_id")->valuedouble;
 cJSON_Delete(params);cJSON_Delete(result.data);free(result.message);assert(id);return id;
}
static ts_ssh_service_status_t cached(void){ts_ssh_service_status_t s;assert(ts_ssh_service_cached(command.id,&s)==ESP_OK);return s;}
static void drain(void){draining=1;action_executor_task(NULL);draining=0;s_ctx->running=true;assert(!s_ctx->direct_pending&&!queue_count&&!ssh_live);}
static void *drain_thread(void*unused){drain();return NULL;}
static void *no_json_memory(size_t n){return NULL;}
int main(void){
 initialize_command("model");strcpy(command.command,"printf '%s' '${value}'");
 assert(ts_ssh_service_init()==ESP_OK&&ts_ssh_log_watch_init()==ESP_OK);
 s_ctx->stats_mutex=xSemaphoreCreateMutex();s_ctx->ssh_hosts_mutex=xSemaphoreCreateMutex();s_ctx->running=true;
 expansion_value.type=TS_AUTO_VAL_STRING;memset(expansion_value.str_val,'X',63);expansion_value.str_val[63]=0;
 uint32_t id=submit(TS_SERVICE_VERIFY);assert(queue_count==1);ts_ssh_service_status_t st=cached();assert(st.operation_id==id&&!strcmp(st.operation_phase,"queued"));
 uint32_t duplicate;assert(ts_action_service_control(command.id,TS_SERVICE_STOP,&duplicate)==ESP_ERR_INVALID_STATE);
 assert(ts_action_manager_quiesce()==ESP_OK);drain();st=cached();assert(!strcmp(st.state,"stopped")&&!strcmp(st.operation_phase,"succeeded"));
 id=submit(TS_SERVICE_START);assert(!launch_count);drain();st=cached();assert(st.operation_id==id&&!strcmp(st.state,"running")&&launch_count==1);assert(strstr(sent_command,"${value}"));
 /* A rule's retained snapshot must not block a direct stop. */
 uint32_t pin;assert(ts_ssh_service_pin(&command,"192.0.2.8",22,&pin)==ESP_OK);
 ts_ssh_service_observe(command.id,st.generation,"ready",command.var_name);
 submit(TS_SERVICE_STOP);drain();assert(!remote_running);assert(ts_ssh_service_command_protected(command.id));ts_ssh_service_unpin(command.id,pin);
 /* Slow remote work does not occupy the API caller; cached API reads still return. */
 hold_connect=1;connect_entered=0;submit(TS_SERVICE_START);
 pthread_t worker;assert(!pthread_create(&worker,NULL,drain_thread,NULL));
 while(!connect_entered){struct timespec d={0,1000000};nanosleep(&d,NULL);}
 cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,"command_id",command.id);
 ts_api_result_t response={0};assert(service_request(params,&response,false)==ESP_OK&&hold_connect);
 assert(!strcmp(cJSON_GetObjectItem(response.data,"operation_phase")->valuestring,"executing"));cJSON_Delete(response.data);response.data=NULL;
 assert(ts_action_manager_quiesce()==ESP_OK);assert(ts_action_service_control(command.id,TS_SERVICE_STOP,&id)==ESP_ERR_INVALID_STATE);
 hold_connect=0;pthread_join(worker,NULL);
 st=cached();ts_ssh_service_observe(command.id,st.generation,"ready",command.var_name);
 ts_ssh_service_observe(command.id,st.generation,"unknown",command.var_name);
 submit(TS_SERVICE_VERIFY);queue[0].enqueue_time-=SERVICE_QUEUE_TIMEOUT_MS;drain();assert(task_count==0);
 submit(TS_SERVICE_STOP);drain();
 /* JSON admission allocation fails before any ownership transfer. */
 cJSON_Hooks hooks={.malloc_fn=no_json_memory,.free_fn=free};cJSON_InitHooks(&hooks);
 assert(ts_api_service_control(params,&response,TS_SERVICE_VERIFY)==ESP_ERR_NO_MEM&&!s_ctx->direct_pending);
 cJSON_InitHooks(NULL);cJSON_Delete(params);
 /* Expired queue entries never reach SSH, including starts and stops. */
 for(int kind=TS_SERVICE_START;kind<=TS_SERVICE_STOP;kind++){
  submit(kind);queue[0].enqueue_time-=SERVICE_QUEUE_TIMEOUT_MS;int before=connect_count;drain();st=cached();assert(connect_count==before&&!strcmp(st.operation_phase,"expired"));
 }
 /* A transport timeout is distinct from a queue expiry. No auto retry. */
 fail_transport=1;submit(TS_SERVICE_VERIFY);drain();st=cached();assert(!strcmp(st.operation_phase,"unconfirmed"));fail_transport=0;
 submit(TS_SERVICE_VERIFY);drain();
 /* Execute the real bound automation path: variables expand; raw direct did not. */
 ts_auto_action_t action={.type=TS_AUTO_ACT_SSH_CMD_REF};strcpy(action.ssh_ref.cmd_id,command.id);assert(snapshot_command(&action,true)==ESP_OK);
 ts_action_result_t result={0};assert(execute_bound_ref(&action,&result)==ESP_OK);assert(!strstr(sent_command,"${value}")&&strstr(sent_command,"XXXXXXXX"));ts_action_snapshot_release(&action);
 submit(TS_SERVICE_STOP);drain();
 /* A long expansion must fail before any SSH I/O or launch. */
 memset(command.command,'a',1000);strcpy(command.command+1000,"${value}");
 memset(&action,0,sizeof(action));action.type=TS_AUTO_ACT_SSH_CMD_REF;strcpy(action.ssh_ref.cmd_id,command.id);
 assert(snapshot_command(&action,true)==ESP_OK);int before=connect_count;
 assert(execute_bound_ref(&action,&result)==ESP_ERR_INVALID_SIZE&&connect_count==before);ts_action_snapshot_release(&action);
 /* Same stored direct command is literal and fits the command capacity. */
 submit(TS_SERVICE_START);drain();assert(strstr(sent_command,"${value}"));submit(TS_SERVICE_STOP);drain();
 /* Queue/allocation failures release the actual snapshot and service reservation. */
 fail_send=1;assert(ts_action_service_control(command.id,TS_SERVICE_START,&id)==ESP_ERR_NO_MEM);fail_send=0;
 assert(ts_ssh_service_start_admissible(command.id)&&!s_ctx->direct_pending);
 for(int n=1;n<=2;n++){fail_alloc=n;assert(ts_action_service_control(command.id,TS_SERVICE_VERIFY,&id)==ESP_ERR_NO_MEM);assert(!s_ctx->direct_pending);}fail_alloc=0;
 queue_count=16;assert(ts_action_service_control(command.id,TS_SERVICE_VERIFY,&id)==ESP_ERR_NO_MEM&&!s_ctx->direct_pending);queue_count=0;
 submit(TS_SERVICE_START);assert(ts_action_cancel_all()==ESP_OK);s_ctx->running=true;assert(ts_ssh_service_start_admissible(command.id)&&!s_ctx->direct_pending);
 run_immediately=1;assert(ts_action_service_control(command.id,TS_SERVICE_VERIFY,&id)==ESP_OK);run_immediately=0;s_ctx->running=true;assert(cached().operation_id==id&&!s_ctx->direct_pending&&!ssh_live);
 for(int n=0;n<100;n++){submit(TS_SERVICE_VERIFY);drain();}
 puts("PASS production service API/executor/SSH/registry: zero-I/O admission, slow SSH with responsive cached API, engine-independent controls, literal vs expanded commands, overflow before I/O, retained-rule stop, 30s expiry, timeout evidence, JSON/entry/snapshot/queue/cancel cleanup, fast worker, 100 cycles");
}
'''
build=Path('/tmp/tianshan-runtime-tests');build.mkdir(exist_ok=True);(build/'service_control.c').write_text(code)
env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
args=['cc','-std=c11','-g','-fsanitize=address,undefined','-ftrivial-auto-var-init=pattern','-Wno-deprecated-declarations']
for path in ['tests/runtime/state_stubs','tests/runtime/ssh_stubs','tests/runtime/stubs','tests/certificate/stubs','components/ts_security/include','components/ts_automation/include','components/ts_api/include',str(sdk)]:args+=['-I'+path]
args += [str(build/'service_control.c'),'components/ts_security/src/ts_ssh_service.c','components/ts_security/src/ts_ssh_log_watch.c','components/ts_security/src/ts_ssh_probe.c',str(sdk/'cJSON.c'),'-lpthread','-o',str(build/'service_control')]
subprocess.run(args,check=True,env=env);subprocess.run([str(build/'service_control')],check=True,env=env)
