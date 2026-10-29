/* Actual registry, durable descriptors, process probe and watcher. NVS, RTOS
 * scheduling, configuration views and SSH transport are controlled boundaries. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "freertos/task.h"
#include "ts_ssh_service.h"
#include "ts_ssh_log_watch.h"
#include "ts_automation_config.h"
#include "ts_rule_engine.h"
#include "ts_variable.h"
#include "nvs.h"

struct test_task {pthread_t thread;pthread_mutex_t lock;pthread_cond_t cond;unsigned notifications;void(*fn)(void*);void *arg;};
static _Thread_local TaskHandle_t self;
static TaskHandle_t tasks[64];static unsigned task_count;static bool fail_task_create;
static void *task_entry(void *p){self=p;self->fn(self->arg);return NULL;}
BaseType_t xTaskCreatePinnedToCore(void(*fn)(void*),const char*name,unsigned stack,void*arg,unsigned priority,TaskHandle_t*out,unsigned core)
{(void)name;assert(stack==8192&&priority==5&&core==1);if(fail_task_create)return 0;TaskHandle_t t=calloc(1,sizeof(*t));pthread_mutex_init(&t->lock,NULL);pthread_cond_init(&t->cond,NULL);t->fn=fn;t->arg=arg;*out=t;tasks[task_count++]=t;assert(!pthread_create(&t->thread,NULL,task_entry,t));return pdPASS;}
unsigned ulTaskNotifyTake(int clear,unsigned ms)
{(void)clear;pthread_mutex_lock(&self->lock);if(!self->notifications){struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=ms/1000;until.tv_nsec+=(ms%1000)*1000000;until.tv_sec+=until.tv_nsec/1000000000;until.tv_nsec%=1000000000;while(!self->notifications&&pthread_cond_timedwait(&self->cond,&self->lock,&until)==0){}}unsigned n=self->notifications;self->notifications=0;pthread_mutex_unlock(&self->lock);return n;}
void xTaskNotifyGive(TaskHandle_t t){assert(t);pthread_mutex_lock(&t->lock);++t->notifications;pthread_cond_signal(&t->cond);pthread_mutex_unlock(&t->lock);}
void vTaskDelete(TaskHandle_t t){assert(!t);pthread_exit(NULL);}
static void join_tasks(void){for(unsigned i=0;i<task_count;++i){pthread_join(tasks[i]->thread,NULL);pthread_cond_destroy(&tasks[i]->cond);pthread_mutex_destroy(&tasks[i]->lock);free(tasks[i]);}task_count=0;}
int64_t esp_timer_get_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
void *heap_caps_malloc(size_t n,unsigned caps){(void)caps;return malloc(n);}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps){(void)caps;return calloc(n,size);}
void esp_fill_random(void *buffer,size_t size){static unsigned serial;memset(buffer,0,size);++serial;memcpy(buffer,&serial,sizeof(serial));}

static unsigned char stored[TS_SSH_RUNS_MAX][768],pending[768];
static size_t stored_size[TS_SSH_RUNS_MAX],pending_size;
static unsigned pending_slot;static bool pending_erase,fail_store;
static unsigned slot(const char *key){unsigned n;assert(sscanf(key,"r%u",&n)==1&&n<TS_SSH_RUNS_MAX);return n;}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *handle){assert(!strcmp(name,"ssh_runs"));(void)mode;*handle=1;return ESP_OK;}
void nvs_close(nvs_handle_t handle){assert(handle==1);pending_size=0;pending_erase=false;}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *out,size_t *size)
{assert(handle==1);unsigned i=slot(key);if(!stored_size[i])return ESP_ERR_NVS_NOT_FOUND;if(*size<stored_size[i])return ESP_ERR_INVALID_SIZE;memcpy(out,stored[i],stored_size[i]);*size=stored_size[i];return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t handle,const char *key,const void *data,size_t size)
{assert(handle==1&&size<=sizeof(pending));pending_slot=slot(key);pending_size=size;memcpy(pending,data,size);return ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t handle,const char *key){assert(handle==1);pending_slot=slot(key);pending_erase=true;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t handle)
{assert(handle==1);if(fail_store)return ESP_FAIL;if(pending_size){memcpy(stored[pending_slot],pending,pending_size);stored_size[pending_slot]=pending_size;}if(pending_erase)stored_size[pending_slot]=0;pending_size=0;pending_erase=false;return ESP_OK;}

static ts_ssh_command_config_t command;
static ts_ssh_host_config_t current_host;
static unsigned execution_pauses;
static bool definition_present=true;static uint64_t epoch=1,host_instance=5,variable_instance=9,template_instance=11;
static unsigned timestamp_writes,exit_code_writes;static unsigned variable_writes,connections,launches;static char status_value[16];
static pthread_mutex_t registry_gate=PTHREAD_MUTEX_INITIALIZER;
void ts_variable_registration_begin(void){pthread_mutex_lock(&registry_gate);}
void ts_variable_registration_end(void){pthread_mutex_unlock(&registry_gate);}
uint64_t ts_auto_config_execution_epoch(void){return epoch+execution_pauses;}
uint64_t ts_auto_config_binding_epoch(void){return epoch;}
uint64_t ts_auto_config_host_instance(const char *id){return !strcmp(id,current_host.id)?host_instance:0;}
bool ts_auto_config_template_current(uint64_t instance){return instance==template_instance;}
bool ts_auto_config_command_current(const char *id,uint64_t instance){return definition_present&&!strcmp(command.id,id)&&command.instance==instance;}
bool ts_rule_instance_current(uint32_t instance){return definition_present&&instance==7;}
uint64_t ts_variable_instance(const char *name){(void)name;return definition_present?variable_instance:0;}
esp_err_t ts_variable_set_string(const char *name,const char *value){assert(!strcmp(name,"job.status"));++variable_writes;strcpy(status_value,value);return ESP_OK;}
esp_err_t ts_variable_set_int(const char *name,int32_t value){(void)value;if(!strcmp(name,"job.timestamp"))++timestamp_writes;else if(!strcmp(name,"job.exit_code"))++exit_code_writes;else assert(!strcmp(name,"job.ready_time"));++variable_writes;return ESP_OK;}
esp_err_t ts_ssh_commands_config_get(const char *id,ts_ssh_command_config_t *out)
{if(!definition_present||strcmp(command.id,id))return ESP_ERR_NOT_FOUND;*out=command;return ESP_OK;}
esp_err_t ts_ssh_hosts_config_get(const char *id,ts_ssh_host_config_t *out)
{if(strcmp(current_host.id,id))return ESP_ERR_NOT_FOUND;*out=current_host;return ESP_OK;}
static bool missing_key;
esp_err_t ts_keystore_load_private_key(const char *id,char **key,size_t *length)
{assert(!strcmp(id,"original-key"));if(missing_key)return ESP_ERR_NOT_FOUND;*key=strdup("PRIVATE SYNTHETIC MATERIAL");*length=strlen(*key);return ESP_OK;}
struct ts_ssh_session_s {char host[64],username[32];};
static char last_target[64],last_command[8192],launched_command[8192];static bool remote_running,fail_transport,invalidate_on_log,ready_log=true;
static char identity[96]="abcd-1234:123:456";
esp_err_t ts_ssh_session_create(const ts_ssh_config_t *cfg,ts_ssh_session_t *out)
{*out=calloc(1,sizeof(**out));strcpy((*out)->host,cfg->host);strcpy((*out)->username,cfg->username);return ESP_OK;}
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t session){free(session);return ESP_OK;}
esp_err_t ts_ssh_connect(ts_ssh_session_t session){++connections;strcpy(last_target,session->host);return fail_transport?ESP_ERR_TIMEOUT:ESP_OK;}
const char *ts_ssh_connect_reason(ts_ssh_session_t session,const char **stage){(void)session;*stage=fail_transport?"timeout":"connect";return fail_transport?"ssh_connect_timeout":"ssh_connect_failed";}
esp_err_t ts_ssh_exec(ts_ssh_session_t session,const char *text,ts_ssh_exec_result_t *result)
{
    (void)session;strcpy(last_command,text);if(fail_transport)return ESP_ERR_TIMEOUT;
    if(getenv("TS_REAL_REMOTE")) {
        FILE *pipe=popen(text,"r");assert(pipe);char output[256];size_t n=fread(output,1,sizeof(output)-1,pipe);output[n]=0;
        int status=pclose(pipe);assert(WIFEXITED(status));result->exit_code=WEXITSTATUS(status);
        result->stdout_data=strdup(output);result->stdout_len=n;
        struct timespec pause={0,200000000};nanosleep(&pause,NULL);
        while(waitpid(-1,NULL,WNOHANG)>0){}
        return ESP_OK;
    }
    const char *token;char response[128];
    if(!strncmp(text,"mkdir /tmp/ts_nohup_",20)){
        bool saved=false;for(unsigned i=0;i<TS_SSH_RUNS_MAX;++i)if(stored_size[i])saved=true;
        assert(saved);strcpy(launched_command,text);++launches;remote_running=true;snprintf(response,sizeof(response),"STARTED %s\n",identity);token=response;
    }else if(strstr(text,"grep -qF")){if(invalidate_on_log){++epoch;invalidate_on_log=false;}token=ready_log?"READY\n":"WAITING\n";}
    else if(strstr(text,"kill -TERM")){remote_running=false;token="STOPPED\n";}
    else{snprintf(response,sizeof(response),"RUNNING %s\n",identity);token=remote_running?response:"STOPPED\n";}
    result->stdout_data=strdup(token);result->stdout_len=strlen(token);return ESP_OK;
}
void ts_ssh_exec_result_free(ts_ssh_exec_result_t *result){free(result->stdout_data);free(result->stderr_data);memset(result,0,sizeof(*result));}
#include "../../components/ts_security/src/ts_ssh_service.c"

static ts_ssh_run_origin_t origin(void)
{
    ts_ssh_run_origin_t result={.epoch=epoch,.command_instance=command.instance,.host_instance=host_instance,
        .variables={variable_instance,variable_instance}};
    memset(result.rule,'r',sizeof(result.rule)-1);strcpy(result.config_version,"frozen-version");return result;
}
static ts_ssh_service_status_t cached(const char *id){ts_ssh_service_status_t status;assert(ts_ssh_service_cached(id,&status)==ESP_OK);return status;}
static void reboot_registry(void){initialized=false;assert(ts_ssh_service_init()==ESP_OK);}
static unsigned durable_count(void){unsigned total=0;for(unsigned i=0;i<TS_SSH_RUNS_MAX;++i)total+=stored_size[i]!=0;return total;}
static esp_err_t control(const char *id,ts_service_operation_t kind)
{
    uint32_t operation;assert(ts_ssh_service_reserve_operation(id,kind,&operation,NULL)==ESP_OK);
    assert(ts_ssh_service_operation_begin(id,operation)==ESP_OK);ts_ssh_service_status_t result;
    esp_err_t ret=ts_ssh_service_execute_control(id,operation,kind,&result);
    ts_ssh_service_complete_operation(id,operation,ret);return ret;
}
static void start(char id[32],bool fail_receipt_save)
{
    ts_ssh_run_origin_t source=origin();uint32_t operation;
    assert(ts_ssh_service_prepare(&command,&current_host,&source,id,&operation,NULL)==ESP_OK);
    assert(durable_count()&&launches==0);assert(ts_ssh_service_operation_begin(id,operation)==ESP_OK);
    ts_ssh_session_t session=NULL;char *key=NULL;assert(ts_ssh_service_connect(&current_host,&session,&key,NULL,NULL)==ESP_OK);
    uint32_t generation;assert(ts_ssh_service_begin_reserved(id,session,operation,&generation,NULL)==ESP_OK);
    char text[8192];assert(ts_ssh_service_launch_command(id,"printf '%s' literal",text,sizeof(text)));
    ts_ssh_exec_result_t result={0};assert(ts_ssh_exec(session,text,&result)==ESP_OK);
    fail_store=fail_receipt_save;esp_err_t ret=ts_ssh_service_finish(id,generation,result.stdout_data,session);fail_store=false;
    assert(ret==(fail_receipt_save?ESP_FAIL:ESP_OK));ts_ssh_service_complete_operation(id,operation,ret);
    ts_ssh_exec_result_free(&result);ts_ssh_session_destroy(session);free(key);
}
static void *reap_remote(void *pid){while(waitpid(-(pid_t)(intptr_t)pid,NULL,0)>0){}return NULL;}
static int real_remote_cases(void)
{
#ifdef __linux__
    assert(!prctl(PR_SET_CHILD_SUBREAPER,1));
#endif
    for(unsigned pass=0;pass<3;++pass){
        char id[32],text[8192];uint32_t operation,generation;
        ts_ssh_run_origin_t source=origin();ts_ssh_session_t session=NULL;char *key=NULL;
        assert(ts_ssh_service_prepare(&command,&current_host,&source,id,&operation,NULL)==ESP_OK);
        assert(ts_ssh_service_connect(&current_host,&session,&key,NULL,NULL)==ESP_OK);
        assert(ts_ssh_service_begin_reserved(id,session,operation,&generation,NULL)==ESP_OK);
        assert(ts_ssh_service_launch_command(id,pass==2?"sleep 20":"true",text,sizeof(text)));
        ts_ssh_exec_result_t receipt={0};assert(ts_ssh_exec(session,text,&receipt)==ESP_OK&&receipt.exit_code==0);
        assert(instance_receipt(receipt.stdout_data,"STARTED ",NULL));
        if(pass==1){reboot_registry();assert(!strcmp(cached(id).state,"unknown"));assert(control(id,TS_SERVICE_VERIFY)==ESP_OK);}
        else {
            assert(ts_ssh_service_finish(id,generation,receipt.stdout_data,session)==ESP_OK);
            ts_ssh_service_complete_operation(id,operation,ESP_OK);
            if(pass==2){
                assert(!strcmp(cached(id).state,"running"));
                char *separator=strchr(receipt.stdout_data,':');assert(separator);pid_t pid=atoi(separator+1);
                pthread_t reaper;assert(!pthread_create(&reaper,NULL,reap_remote,(void *)(intptr_t)pid));
                assert(control(id,TS_SERVICE_STOP)==ESP_OK);pthread_join(reaper,NULL);
            }
        }
        assert(!strcmp(cached(id).state,"stopped")&&!durable_count());
        assert(ts_ssh_service_check_start(command.id,&current_host,NULL,0,NULL)==ESP_OK);
        ts_ssh_exec_result_free(&receipt);ts_ssh_session_destroy(session);free(key);
    }
    puts("PASS real Linux launcher/probe: fast exit, restart verification, relaunch and live group stop");
    return 0;
}
int main(void)
{
    strcpy(command.id,"command");strcpy(command.host_id,"host");strcpy(command.name,"job");strcpy(command.var_name,"job");
    command.instance=3;command.enabled=command.nohup=true; /* ordinary nohup is managed too */
    strcpy(current_host.id,"host");strcpy(current_host.host,"192.0.2.8");strcpy(current_host.username,"original-user");strcpy(current_host.keyid,"original-key");current_host.port=22;
    assert(ts_ssh_service_init()==ESP_OK&&ts_ssh_log_watch_init()==ESP_OK);
    if(getenv("TS_REAL_REMOTE"))return real_remote_cases();
    char id[32];uint32_t operation;ts_ssh_run_origin_t source=origin();
    const char *reason=NULL;fail_store=true;assert(ts_ssh_service_prepare(&command,&current_host,&source,id,&operation,&reason)==ESP_FAIL&&!strcmp(reason,"service_record_save_failed"));
    fail_store=false;assert(!durable_count()&&!launches&&!connections);
    start(id,false);assert(!strcmp(cached(id).state,"running"));
    ts_ssh_run_record_t saved;ts_ssh_service_status_t status;
    assert(ts_ssh_service_get(0,&saved,&status)==ESP_OK&&strlen(saved.rule)==TS_AUTO_NAME_MAX_LEN-1);
    assert(strstr(saved.locator,id)&&saved.host.port==22&&!strcmp(saved.config_version,"frozen-version"));
    assert(ts_ssh_service_check_start(command.id,&current_host,NULL,0,&reason)==ESP_ERR_INVALID_STATE&&!strcmp(reason,"service_already_running"));
    ts_ssh_service_observe(id,status.generation,"checking");
    assert(control(id,TS_SERVICE_VERIFY)==ESP_OK&&!strcmp(cached(id).state,"checking")&&cached(id).generation==status.generation);
    ts_ssh_service_observe(id,status.generation,"ready");
    assert(ts_ssh_service_check_start(command.id,&current_host,NULL,0,&reason)==ESP_ERR_INVALID_STATE);
    assert(!strcmp(cached(id).state,"ready")&&cached(id).generation==status.generation);
    char selected[TS_SSH_RUN_ID_MAX];ts_ssh_service_status_t projected;
    assert(ts_ssh_service_current(command.id,&current_host,command.instance,selected,&projected)&&!strcmp(selected,id));
    assert(!ts_ssh_service_current(command.id,&current_host,command.instance+1,selected,&projected));
    assert(ts_ssh_service_current(command.id,&current_host,0,selected,&projected)&&!strcmp(projected.state,"ready"));
    unsigned before=variable_writes;++template_instance;
    ++execution_pauses; /* Pausing new automation must not freeze a live service lamp. */
    ts_ssh_service_observe(id,status.generation,"ready");assert(variable_writes>before&&!strcmp(status_value,"ready"));before=variable_writes;
    definition_present=false;
    ts_ssh_service_observe(id,status.generation,"ready");assert(!strcmp(cached(id).state,"ready")&&variable_writes==before);
    definition_present=true;++command.instance;++variable_instance;++host_instance;strcpy(current_host.host,"192.0.2.99");
    ts_ssh_service_observe(id,status.generation,"ready");assert(variable_writes==before);
    assert(control(id,TS_SERVICE_VERIFY)==ESP_OK&&!strcmp(last_target,"192.0.2.8"));
    missing_key=true;unsigned before_connect=connections;
    assert(control(id,TS_SERVICE_VERIFY)==ESP_ERR_NOT_FOUND&&connections==before_connect);
    assert(!strcmp(cached(id).operation_reason,"ssh_key_unavailable"));missing_key=false;
    fail_transport=true;assert(control(id,TS_SERVICE_VERIFY)==ESP_ERR_TIMEOUT);
    assert(!strcmp(cached(id).operation_reason,"ssh_connect_timeout"));
    fail_transport=false;assert(control(id,TS_SERVICE_VERIFY)==ESP_OK&&!cached(id).operation_reason[0]);
    reboot_registry();status=cached(id);assert(!strcmp(status.state,"unknown")&&!strcmp(status.source,"restart")&&!status.busy&&!status.operation_id);
    assert(ts_ssh_service_current(command.id,&saved.host,command.instance,selected,&projected)&&!strcmp(projected.state,"unknown"));
    assert(control(id,TS_SERVICE_VERIFY)==ESP_OK&&variable_writes==before);
    assert(control(id,TS_SERVICE_STOP)==ESP_OK&&!remote_running&&!durable_count());

    /* Controller and host restarts leave a record, not a permanent start lock. */
    strcpy(current_host.host,"192.0.2.8");launches=0;start(id,false);remote_running=false;
    reboot_registry();assert(!strcmp(cached(id).state,"unknown"));
    assert(ts_ssh_service_check_start(command.id,&current_host,NULL,0,&reason)==ESP_OK&&!durable_count());
    launches=0;start(id,false);assert(remote_running);assert(control(id,TS_SERVICE_STOP)==ESP_OK);

    /* A failed receipt write still leaves the pre-launch locator on flash. */
    strcpy(current_host.host,"192.0.2.8");launches=0;start(id,true);assert(durable_count()==1);
    reboot_registry();assert(control(id,TS_SERVICE_VERIFY)==ESP_OK&&remote_running);
    assert(control(id,TS_SERVICE_STOP)==ESP_OK&&!durable_count());

    /* Delayed log completion updates the old run, never the new source's vars. */
    launches=0;start(id,false);status=cached(id);before=variable_writes;
    ts_ssh_log_watch_config_t watch={.run_generation=status.generation,.timeout_sec=1,.check_interval_ms=1};
    strcpy(watch.run_id,id);strcpy(watch.ready_pattern,"ready");strcpy(watch.var_name,"job");invalidate_on_log=true;
    ts_ssh_log_watch_handle_t old_handle;assert(ts_ssh_log_watch_start(&watch,&old_handle)==ESP_OK);join_tasks();
    assert(!strcmp(cached(id).state,"ready")&&variable_writes==before+1); /* initial checking only */
    assert(control(id,TS_SERVICE_STOP)==ESP_OK);

    /* Cancellation owns worker cleanup; stale handles cannot cancel a new run. */
    launches=0;start(id,false);status=cached(id);strcpy(watch.run_id,id);watch.run_generation=status.generation;
    ready_log=false;watch.check_interval_ms=1000;ts_ssh_log_watch_handle_t handle=0;
    fail_task_create=true;assert(ts_ssh_log_watch_start(&watch,&handle)==ESP_ERR_NO_MEM&&!handle);fail_task_create=false;
    assert(ts_ssh_log_watch_start(&watch,&handle)==ESP_OK&&handle!=old_handle);
    assert(ts_ssh_log_watch_stop(old_handle)==ESP_ERR_NOT_FOUND);
    assert(ts_ssh_log_watch_stop(handle)==ESP_OK);join_tasks();
    assert(!ts_ssh_log_watch_pending(handle)&&remote_running&&!strcmp(cached(id).state,"unknown"));
    ts_ssh_service_observe(id,status.generation,"ready");assert(!strcmp(cached(id).state,"unknown"));
    assert(control(id,TS_SERVICE_STOP)==ESP_OK);ready_log=true;

    /* The original plain PID locator is retained only for imported runs. */
    strcpy(command.name,"legacy name");assert(ts_ssh_service_import_legacy(&command,&current_host)==ESP_OK);
    assert(ts_ssh_service_get(0,&saved,&status)==ESP_OK&&saved.kind==TS_SSH_RUN_LEGACY_PID&&!strcmp(saved.locator,"legacyname"));
    strcpy(id,saved.run_id);remote_running=true;
    assert(control(id,TS_SERVICE_STOP)==ESP_ERR_INVALID_STATE&&remote_running);
    assert(control(id,TS_SERVICE_VERIFY)==ESP_OK);assert(control(id,TS_SERVICE_STOP)==ESP_OK&&!remote_running);

    /* Slots belong to live/unknown runs, not the number of remaining definitions. */
    for(unsigned i=0;i<TS_SSH_RUNS_MAX;++i){
        snprintf(command.id,sizeof(command.id),"c%u",i);source=origin();
        assert(ts_ssh_service_prepare(&command,&current_host,&source,id,&operation,NULL)==ESP_OK);
        services[index_of(find(id))].launch_attempted=true; /* unknown remote result */
        ts_ssh_service_complete_operation(id,operation,ESP_ERR_TIMEOUT);
    }
    assert(durable_count()==TS_SSH_RUNS_MAX);definition_present=false;
    snprintf(command.id,sizeof(command.id),"new");source=origin();
    assert(ts_ssh_service_prepare(&command,&current_host,&source,id,&operation,&reason)==ESP_ERR_INVALID_SIZE&&!strcmp(reason,"service_record_limit"));
    /* Even when full, a direct queued start reaches the executor's old-run check.
     * Unrelated records remain untouched; a stopped matching run releases its slot. */
    assert(ts_ssh_service_plan(&command,&current_host,&source,id,&operation,NULL)==ESP_OK&&durable_count()==TS_SSH_RUNS_MAX);
    ts_ssh_session_t session=NULL;char *key=NULL;uint32_t generation;
    assert(ts_ssh_service_connect(&current_host,&session,&key,NULL,NULL)==ESP_OK);
    assert(ts_ssh_service_begin_reserved(id,session,operation,&generation,NULL)==ESP_ERR_INVALID_SIZE);
    ts_ssh_service_complete_operation(id,operation,ESP_ERR_INVALID_SIZE);
    strcpy(command.id,"c0");remote_running=false;
    assert(ts_ssh_service_plan(&command,&current_host,&source,id,&operation,NULL)==ESP_OK&&durable_count()==TS_SSH_RUNS_MAX);
    assert(ts_ssh_service_begin_reserved(id,session,operation,&generation,NULL)==ESP_OK&&durable_count()==TS_SSH_RUNS_MAX);
    assert(index_of(find(id))<TS_SSH_RUNS_MAX&&find(id)->durable);
    ts_ssh_service_complete_operation(id,operation,ESP_OK);
    ts_ssh_session_destroy(session);free(key);
    assert(control(id,TS_SERVICE_VERIFY)==ESP_OK&&durable_count()==TS_SSH_RUNS_MAX-1);
    size_t wire_size=0;for(unsigned i=0;i<TS_SSH_RUNS_MAX;++i)if(stored_size[i])wire_size=stored_size[i];
    printf("64 independent service descriptors: %zu bytes (NVS entry overhead excluded)\n",wire_size*TS_SSH_RUNS_MAX);
    puts("PASS service runs: save-before-start, ordinary nohup, frozen target/key, deleted/recreated IDs, full rule IDs, reboot unknown, receipt-save failure, late logs, legacy PID adoption and independent capacity");
}
