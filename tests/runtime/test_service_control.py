"""Actual API -> queue admission -> executor entry -> SSH -> durable run registry.
Only transport, driver storage and RTOS scheduling are simulated.
"""
from pathlib import Path
import os,re,subprocess
root=Path.cwd();sdk=Path(os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf'))/'components/json/cJSON'
source=Path('components/ts_automation/src/ts_action_manager.c').read_text()
def fn(name,src=source):
 m=re.search(r'^(?:static )?(?:esp_err_t|void|bool|int|uint64_t|cJSON \*)\s*'+name+r'\([^;]+?\)\s*\{',src,re.M);assert m,name
 return src[m.start():src.index('\n}',m.start())+2]
def typedef(name):
 end=source.index('} '+name+';')+len('} '+name+';');start=source.rfind('typedef struct {',0,end)
 return source[start:end]
code=Path('tests/runtime/test_service_runs.c').read_text().split('#include "../../components/ts_security/src/ts_ssh_service.c"')[0]
code=code.replace('static char last_target[64]', 'static atomic_bool hold_connect,connect_entered;\nstatic char last_target[64]')
code=code.replace('++connections;strcpy(last_target,session->host);', '++connections;strcpy(last_target,session->host);if(hold_connect){connect_entered=true;while(hold_connect){struct timespec delay={0,1000000};nanosleep(&delay,NULL);}}')
code+=r'''
#include "ts_action_manager.h"
#include "ts_api.h"
#include "ts_automation_document.h"
#define SERVICE_QUEUE_TIMEOUT_MS 30000
#define TAG "native"
#define TS_STRDUP_PSRAM strdup
typedef void *QueueHandle_t;
'''+typedef('action_manager_ctx_t')+'\n'+typedef('action_binding_t')+r'''
static action_manager_ctx_t context,*s_ctx=&context;
static ts_action_queue_entry_t queued;static bool has_queue,fail_send;
static int xQueueSend(void *queue,const void *entry,unsigned timeout){(void)queue;assert(!timeout);if(fail_send||has_queue)return 0;queued=*(const ts_action_queue_entry_t*)entry;has_queue=true;return 1;}
static esp_err_t action_manager_execute(const ts_auto_action_t *,ts_action_result_t *,bool,bool);
static void vTaskDelay(unsigned delay){(void)delay;assert(false);}
void ts_auto_config_status(ts_auto_config_status_t *status){memset(status,0,sizeof(*status));strcpy(status->active_version,"current-version");}
esp_err_t ts_auto_config_edit_begin(const char *version,ts_auto_config_error_t *error){
    if(!version||strcmp(version,"current-version")){error->reason=version?"configuration_changed":"configuration_version_required";return ESP_ERR_INVALID_STATE;}return ESP_OK;}
void ts_auto_config_edit_end(void){}
const ts_auto_config_view_t *ts_auto_config_acquire(void){return (void *)1;}
void ts_auto_config_release(const ts_auto_config_view_t *view){assert(view==(void *)1);}
static cJSON *key_preview_config;
const cJSON *ts_auto_config_json(const ts_auto_config_view_t *view){assert(view==(void *)1);return key_preview_config;}
const ts_auto_document_t *ts_auto_config_definitions(const ts_auto_config_view_t *view){
    assert(view==(void *)1);static ts_auto_document_t document;
    document.commands=&command;document.command_count=definition_present?1:0;
    document.hosts=&current_host;document.host_count=1;return &document;
}


esp_err_t ts_ssh_disconnect(ts_ssh_session_t session){(void)session;return ESP_OK;}
esp_err_t ts_ssh_commands_config_update_exec_time(const ts_ssh_command_config_t *executed){(void)executed;return ESP_OK;}
esp_err_t ts_variable_get(const char *name,ts_auto_value_t *out){if(strcmp(name,"value"))return ESP_ERR_NOT_FOUND;out->type=TS_AUTO_VAL_STRING;strcpy(out->str_val,"expanded");return ESP_OK;}
static esp_err_t exec_cli_bound(const ts_auto_action_cli_t *cli,ts_action_result_t *result,const ts_auto_action_t *origin){(void)cli;(void)result;(void)origin;return ESP_ERR_NOT_SUPPORTED;}
static esp_err_t execute_webhook(const ts_auto_action_t *action){(void)action;return ESP_ERR_NOT_SUPPORTED;}
'''
# Hardware leaves are not invoked by this test.
for suffix,typ in [('led','led'),('gpio','gpio'),('log','log'),('set_var','set_var'),('device','device')]:
 code+=f'\nesp_err_t ts_action_exec_{suffix}(const ts_auto_action_{typ}_t *value,ts_action_result_t *result){{(void)value;(void)result;return ESP_ERR_NOT_SUPPORTED;}}\n'
code+=re.search(r'^static const char \*const cli_outputs[^;]+;',source,re.M).group(0)+'\n'
for name in ['same_host','action_variable','ts_action_snapshot_current','action_result_begin','action_result_end',
             'ts_action_snapshot_release','snapshot_command','ts_action_get_ssh_host','action_finished','direct_service_admit',
             'direct_service_finished','entry_finished','ts_action_manager_check_start','prepare_run','ts_action_service_control','ts_action_expand_variables',
             'exec_ssh_bound','automatic_start_expired','exec_ssh_ref_bound','execute_bound_ref','execute_action_internal','execute_service_control','execute_entry']:
 code+='\n'+fn(name)
api=Path('components/ts_api/src/ts_api_ssh.c').read_text();core=Path('components/ts_api/src/ts_api.c').read_text();automation=Path('components/ts_api/src/ts_api_automation.c').read_text()
for name,src in [('ts_api_result_ok',core),('ts_api_result_error',core),('ts_api_service_control',api),('api_ssh_service_start',api),
                 ('service_status_json',automation),('api_service_list',automation),('service_request',automation),
                 ('key_delete_preview',Path('components/ts_api/src/ts_api_key.c').read_text())]:code+='\n'+fn(name,src)
code+=r'''
static unsigned queued_preflights;
static esp_err_t action_manager_execute(const ts_auto_action_t *action,ts_action_result_t *result,bool delay_handled,bool check){
    (void)delay_handled;assert(check);++queued_preflights;ts_action_queue_entry_t entry={.action=*action,.service_check=true};return execute_entry(&entry,result);
}
static void drain(void){assert(has_queue);has_queue=false;ts_action_result_t result={0};esp_err_t error=execute_entry(&queued,&result);ts_action_snapshot_release(&queued.action);entry_finished(&queued,error);assert(!s_ctx->direct_pending);}
static void *drain_thread(void *unused){(void)unused;drain();return NULL;}
static uint32_t submit(ts_service_operation_t kind,const char *id,char run_id[32])
{
    cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,kind==TS_SERVICE_START?"command_id":"run_id",id);
    if(kind==TS_SERVICE_START)cJSON_AddStringToObject(params,"active_version","current-version");
    if(kind==TS_SERVICE_VERIFY)cJSON_AddBoolToObject(params,"verify",true);
    ts_api_result_t result={0};unsigned before=connections;
    assert((kind==TS_SERVICE_START?api_ssh_service_start(params,&result):service_request(params,&result,kind==TS_SERVICE_STOP))==ESP_OK);
    assert(result.code==TS_API_OK&&connections==before);
    strcpy(run_id,cJSON_GetObjectItem(result.data,"run_id")->valuestring);
    uint32_t operation=cJSON_GetObjectItem(result.data,"operation_id")->valuedouble;
    cJSON_Delete(params);cJSON_Delete(result.data);free(result.message);return operation;
}
static void rejected_control(ts_service_operation_t kind,const char *id,const char *reason)
{
    cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,kind==TS_SERVICE_START?"command_id":"run_id",id);
    if(kind==TS_SERVICE_START)cJSON_AddStringToObject(params,"active_version","current-version");
    if(kind==TS_SERVICE_VERIFY)cJSON_AddBoolToObject(params,"verify",true);
    unsigned before=connections;bool was_queued=has_queue;ts_api_result_t result={0};
    if(kind==TS_SERVICE_START)api_ssh_service_start(params,&result);else service_request(params,&result,kind==TS_SERVICE_STOP);
    assert(result.code!=TS_API_OK&&!strcmp(result.message,reason)&&connections==before&&has_queue==was_queued);
    cJSON_Delete(params);cJSON_Delete(result.data);free(result.message);
}
static ts_ssh_service_status_t cached(const char *id){ts_ssh_service_status_t state;assert(ts_ssh_service_cached(id,&state)==ESP_OK);return state;}
int main(void)
{
    ts_auto_action_ssh_t inline_ssh={0};strcpy(inline_ssh.command,"true");
    ts_action_ssh_host_t inline_host={.use_key_auth=true,.port=22};
    strcpy(inline_host.host,"192.0.2.8");strcpy(inline_host.username,"operator");strcpy(inline_host.key_path,"original-key");
    ts_action_result_t inline_result={0};unsigned before_inline=connections;
    missing_key=true;
    assert(exec_ssh_bound(&inline_ssh,&inline_host,&inline_result)==ESP_ERR_NOT_FOUND);
    assert(inline_result.status==TS_ACTION_STATUS_FAILED&&!strcmp(inline_result.reason,"ssh_key_unavailable")&&connections==before_inline);
    missing_key=false;
    strcpy(command.id,"command");strcpy(command.host_id,"host");strcpy(command.name,"job");strcpy(command.var_name,"job");
    strcpy(command.command,"printf '%s' '${value}'");command.instance=3;command.enabled=command.nohup=true;
    strcpy(current_host.id,"host");strcpy(current_host.host,"192.0.2.8");strcpy(current_host.username,"original-user");strcpy(current_host.keyid,"original-key");current_host.port=22;
    assert(ts_ssh_service_init()==ESP_OK&&ts_ssh_log_watch_init()==ESP_OK);
    s_ctx->stats_mutex=xSemaphoreCreateMutex();s_ctx->running=s_ctx->accepting=true;
    cJSON *stale=cJSON_CreateObject();cJSON_AddStringToObject(stale,"command_id",command.id);cJSON_AddStringToObject(stale,"active_version","old-version");
    ts_api_result_t rejected={0};assert(api_ssh_service_start(stale,&rejected)==ESP_OK&&rejected.code!=TS_API_OK&&!has_queue&&!connections);
    assert(!strcmp(rejected.message,"configuration_changed"));cJSON_Delete(stale);free(rejected.message);
    command.enabled=false;rejected_control(TS_SERVICE_START,command.id,"command_disabled");command.enabled=true;
    definition_present=false;rejected_control(TS_SERVICE_START,command.id,"command_unavailable");definition_present=true;
    strcpy(command.host_id,"missing");rejected_control(TS_SERVICE_START,command.id,"command_host_unavailable");strcpy(command.host_id,"host");
    command.nohup=false;rejected_control(TS_SERVICE_START,command.id,"background_command_required");command.nohup=true;
    current_host.keyid[0]=0;rejected_control(TS_SERVICE_START,command.id,"service_key_required");strcpy(current_host.keyid,"original-key");
    s_ctx->direct_closed=true;rejected_control(TS_SERVICE_START,command.id,"service_controls_unavailable");s_ctx->direct_closed=false;
    rejected_control(TS_SERVICE_VERIFY,"ffffffffffffffffffffffff","service_record_not_found");
    char run_id[32],reply_id[32];missing_key=true;
    uint32_t operation=submit(TS_SERVICE_START,command.id,run_id);drain();
    assert(!connections&&!launches&&!stored_size[0]&&!strcmp(cached(run_id).state,"stopped"));
    assert(!strcmp(cached(run_id).operation_reason,"ssh_key_unavailable"));missing_key=false;
    operation=submit(TS_SERVICE_START,command.id,run_id);
    assert(has_queue&&!launches&&!connections&&!strcmp(cached(run_id).operation_phase,"queued"));
    rejected_control(TS_SERVICE_VERIFY,run_id,"service_control_busy");
    definition_present=false;drain();assert(!launches&&!connections&&!strcmp(cached(run_id).state,"stopped"));
    definition_present=true;++command.instance;
    operation=submit(TS_SERVICE_START,command.id,run_id);drain();
    assert(launches==1&&strstr(launched_command,"${value}")&&!strcmp(cached(run_id).operation_phase,"succeeded"));
    submit(TS_SERVICE_START,command.id,reply_id);drain();assert(launches==1&&!strcmp(cached(reply_id).operation_reason,"service_already_running"));
    assert(remote_running&&!strcmp(cached(run_id).state,"running")&&!strcmp(status_value,"running"));
    ts_action_result_t checked_running={0};unsigned prior_connections=connections;
    ts_auto_action_t automatic={.type=TS_AUTO_ACT_SSH_CMD_REF};strcpy(automatic.ssh_ref.cmd_id,command.id);
    assert(snapshot_command(&automatic,true,NULL)==ESP_OK);
    for(unsigned n=0;n<100;++n)assert(ts_action_manager_check_start(&automatic,&checked_running,true)==ESP_ERR_INVALID_STATE&&!strcmp(checked_running.reason,"service_already_running"));
    assert(connections==prior_connections&&!queued_preflights);ts_action_snapshot_release(&automatic);
    ts_api_result_t live_list={0};assert(api_service_list(NULL,&live_list)==ESP_OK);
    cJSON *live_item;unsigned selected=0;
    cJSON_ArrayForEach(live_item,cJSON_GetObjectItem(live_list.data,"items"))if(cJSON_IsTrue(cJSON_GetObjectItem(live_item,"current"))){
        ++selected;assert(!strcmp(cJSON_GetObjectItem(live_item,"run_id")->valuestring,run_id));
    }
    assert(selected==1);cJSON_Delete(live_list.data);
    /* Recreating an ID on the same host cannot inherit a ready lamp. The old
     * physical process still blocks duplicate automatic launches without I/O. */
    ts_ssh_service_observe(run_id,cached(run_id).generation,"ready");
    ++command.instance;
    live_list=(ts_api_result_t){0};assert(api_service_list(NULL,&live_list)==ESP_OK);selected=0;
    cJSON_ArrayForEach(live_item,cJSON_GetObjectItem(live_list.data,"items"))
        selected+=cJSON_IsTrue(cJSON_GetObjectItem(live_item,"current"));
    assert(!selected);cJSON_Delete(live_list.data);
    automatic=(ts_auto_action_t){.type=TS_AUTO_ACT_SSH_CMD_REF};strcpy(automatic.ssh_ref.cmd_id,command.id);
    assert(snapshot_command(&automatic,true,NULL)==ESP_OK);prior_connections=connections;
    assert(ts_action_manager_check_start(&automatic,&checked_running,true)==ESP_ERR_INVALID_STATE&&!strcmp(checked_running.reason,"service_already_running"));
    assert(connections==prior_connections&&!queued_preflights);ts_action_snapshot_release(&automatic);
    --command.instance;

    /* Even explicit verification of a failed request cannot own the live task's variables. */
    submit(TS_SERVICE_VERIFY,reply_id,reply_id);drain();assert(!strcmp(status_value,"ready"));
    submit(TS_SERVICE_STOP,run_id,reply_id);drain();assert(!remote_running);
    /* Once admitted, started I/O owns its cleanup and independent run. */
    hold_connect=true;connect_entered=false;submit(TS_SERVICE_START,command.id,run_id);
    pthread_t worker;assert(!pthread_create(&worker,NULL,drain_thread,NULL));
    while(!connect_entered){struct timespec delay={0,1000000};nanosleep(&delay,NULL);}
    cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,"run_id",run_id);ts_api_result_t response={0};
    assert(service_request(params,&response,false)==ESP_OK&&!strcmp(cJSON_GetObjectItem(response.data,"operation_phase")->valuestring,"executing"));
    cJSON_Delete(response.data);cJSON_Delete(params);
    definition_present=false;strcpy(current_host.host,"192.0.2.99");unsigned before=variable_writes;
    hold_connect=false;pthread_join(worker,NULL);
    assert(remote_running&&variable_writes==before&&!strcmp(cached(run_id).state,"running"));
    response=(ts_api_result_t){0};assert(api_service_list(NULL,&response)==ESP_OK);
    cJSON *items=cJSON_GetObjectItem(response.data,"items");assert(cJSON_GetArraySize(items)>=1);
    cJSON *item=cJSON_GetArrayItem(items,0);assert(!strcmp(cJSON_GetObjectItem(item,"host")->valuestring,"192.0.2.8"));assert(!strcmp(cJSON_GetObjectItem(item,"keyid")->valuestring,"original-key"));assert(strstr(cJSON_GetObjectItem(item,"log_file")->valuestring,"/tmp/ts_nohup_r_"));cJSON_Delete(response.data);
    key_preview_config=cJSON_Parse("{\"hosts\":[{\"id\":\"uses-key\",\"auth_type\":\"key\",\"keyid\":\"original-key\"},{\"id\":\"password-host\",\"auth_type\":\"password\",\"keyid\":\"original-key\"}]}");
    unsigned preview_connections=connections;response=(ts_api_result_t){0};
    assert(key_delete_preview("original-key",&response)==ESP_OK&&connections==preview_connections);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(response.data,"complete")));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(response.data,"hosts"))==1&&cJSON_GetArraySize(cJSON_GetObjectItem(response.data,"runs"))>=1);
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(response.data,"runs"),0),"host")->valuestring,"192.0.2.8"));
    cJSON_Delete(response.data);free(response.message);cJSON_Delete(key_preview_config);key_preview_config=NULL;
    response=(ts_api_result_t){0};assert(key_delete_preview("original-key",&response)==ESP_OK&&cJSON_IsFalse(cJSON_GetObjectItem(response.data,"complete"))&&connections==preview_connections);
    cJSON_Delete(response.data);free(response.message);
    submit(TS_SERVICE_STOP,run_id,reply_id);drain();assert(!remote_running&&!strcmp(last_target,"192.0.2.8"));
    /* Expired and queue-rejected starts never reach SSH and release ownership. */
    definition_present=true;strcpy(current_host.host,"192.0.2.8");
    submit(TS_SERVICE_START,command.id,run_id);queued.enqueue_time-=SERVICE_QUEUE_TIMEOUT_MS;before=connections;drain();
    assert(connections==before&&!strcmp(cached(run_id).operation_phase,"expired"));
    const char *admission_reason=NULL;fail_send=true;assert(ts_action_service_control(command.id,TS_SERVICE_START,run_id,&operation,&admission_reason)==ESP_ERR_NO_MEM&&!s_ctx->direct_pending&&!strcmp(admission_reason,"service_queue_full"));fail_send=false;
    assert(ts_ssh_service_check_start(command.id,&current_host,NULL,0,NULL)==ESP_OK);
    /* A rebooted host needs no manual verify before the next start. A live or
     * unreachable host must never receive a duplicate launch. */
    submit(TS_SERVICE_START,command.id,run_id);drain();unsigned launches_before=launches;
    submit(TS_SERVICE_START,command.id,reply_id);drain();
    assert(launches==launches_before&&remote_running&&!strcmp(cached(reply_id).operation_reason,"service_already_running"));
    fail_transport=true;submit(TS_SERVICE_START,command.id,reply_id);drain();
    assert(launches==launches_before&&remote_running);fail_transport=false;
    remote_running=false;
    submit(TS_SERVICE_START,command.id,reply_id);drain();
    assert(launches==launches_before+1&&remote_running&&!strcmp(cached(reply_id).operation_phase,"succeeded"));
    submit(TS_SERVICE_STOP,reply_id,run_id);drain();
    /* Network failure is unknown, and never prevents local definition deletion. */
    submit(TS_SERVICE_START,command.id,run_id);drain();fail_transport=true;
    submit(TS_SERVICE_VERIFY,run_id,reply_id);drain();assert(!strcmp(cached(run_id).state,"unknown"));
    definition_present=false;fail_transport=false;submit(TS_SERVICE_STOP,run_id,reply_id);drain();assert(!remote_running);
    /* Automation expands variables through the same managed background path. */
    definition_present=true;ts_auto_action_t action={.type=TS_AUTO_ACT_SSH_CMD_REF};strcpy(action.ssh_ref.cmd_id,command.id);
    assert(snapshot_command(&action,true,NULL)==ESP_OK);ts_action_result_t result={0};
    unsigned timestamps_before=timestamp_writes,codes_before=exit_code_writes;
    assert(execute_bound_ref(&action,&result)==ESP_OK&&strstr(launched_command,"expanded")&&!strstr(launched_command,"${value}"));
    assert(timestamp_writes==timestamps_before+1&&exit_code_writes==codes_before+1&&!strcmp(status_value,"running"));
    ts_action_queue_entry_t check={.action=action,.service_check=true};ts_action_result_t checked={0};
    unsigned preflight_launches=launches;
    assert(execute_entry(&check,&checked)==ESP_ERR_INVALID_STATE&&!strcmp(checked.reason,"service_already_running")&&launches==preflight_launches);
    ts_action_snapshot_release(&action);
    ts_ssh_run_record_t record;ts_ssh_service_status_t state;assert(ts_ssh_service_get(0,&record,&state)==ESP_OK);
    submit(TS_SERVICE_STOP,record.run_id,reply_id);drain();assert(!remote_running);
    puts("PASS service API/executor: queued plan and persistence before launch, stale start cancellation, responsive cached reads, admitted-I/O cleanup, deleted-definition controls, frozen target, expiry, queue rejection and unknown remote state");
}
'''
build=Path('/tmp/tianshan-service-runs-tests');build.mkdir(exist_ok=True);(build/'service_control.c').write_text(code)
args=['cc','-std=c11','-D_DEFAULT_SOURCE','-g','-fsanitize=address,undefined','-Wno-deprecated-declarations']
for path in ['tests/runtime/state_stubs','tests/runtime/ssh_stubs','tests/runtime/stubs','tests/runtime/storage_stubs','tests/certificate/stubs','components/ts_security/include','components/ts_automation/include','components/ts_automation/src','components/ts_api/include',str(sdk)]:args+=['-I'+path]
args += [str(build/'service_control.c'),'components/ts_security/src/ts_ssh_service.c','components/ts_security/src/ts_ssh_run_store.c','components/ts_security/src/ts_ssh_log_watch.c','components/ts_security/src/ts_ssh_probe.c',str(sdk/'cJSON.c'),'-lpthread','-lm','-o',str(build/'service_control')]
subprocess.run(args,check=True);subprocess.run([str(build/'service_control')],check=True)
