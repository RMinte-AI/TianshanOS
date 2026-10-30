#include "ts_ssh_service.h"
#include "ts_ssh_run_store.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ts_keystore.h"
#include "ts_ssh_log_watch.h"
#include "ts_ssh_probe.h"
#include "ts_automation_config.h"
#include "ts_variable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    ts_ssh_run_record_t record;
    ts_ssh_run_origin_t origin;
    ts_ssh_service_status_t status;
    bool durable, launch_attempted, owns_variables;
    /* Readiness belongs to the observed process, independently of a control
     * operation's temporary state. Never restored from a historical record. */
    char readiness[16];
    uint64_t observation_revision;
    uint32_t pending_control, operation;
} service_t;
static service_t *services;
static SemaphoreHandle_t mutex, binding_gate;
static bool initialized;
static uint32_t serial;
static uint32_t ticket(void) { return serial == UINT32_MAX ? 0 : ++serial; }
void ts_ssh_binding_lock(void) { if(binding_gate)xSemaphoreTakeRecursive(binding_gate,portMAX_DELAY); }
bool ts_ssh_binding_try_lock(void) { return !binding_gate||xSemaphoreTakeRecursive(binding_gate,0)==pdTRUE; }
void ts_ssh_binding_unlock(void) { if(binding_gate)xSemaphoreGiveRecursive(binding_gate); }
static service_t *find(const char *id)
{
    for(unsigned i=0;i<TS_SSH_RUN_SLOTS;++i)
        if(services[i].record.run_id[0]&&!strcmp(services[i].record.run_id,id))return &services[i];
    return NULL;
}
static unsigned index_of(const service_t *s) { return (unsigned)(s-services); }
static service_t *free_slot(void)
{
    for(unsigned i=0;i<TS_SSH_RUNS_MAX;++i)
        if(!services[i].durable&&!services[i].pending_control&&!services[i].status.busy)return &services[i];
    return NULL;
}
static bool same_target(const ts_ssh_host_config_t *a,const ts_ssh_host_config_t *b)
{
    return !strcmp(a->host,b->host)&&a->port==b->port&&!strcmp(a->username,b->username);
}
static bool duplicate_busy(const char *command,const ts_ssh_host_config_t *host)
{
    for(unsigned i=0;i<TS_SSH_RUN_SLOTS;++i)
        if(services[i].record.run_id[0]&&!strcmp(services[i].record.command_id,command)&&same_target(&services[i].record.host,host)&&
           (services[i].pending_control||services[i].status.busy))return true;
    return false;
}
static void state_set(service_t *s,const char *state,const char *source)
{
    const char *previous=s->status.state;
    if(strcmp(previous,state)&&(!strcmp(previous,"running")||!strcmp(previous,"ready")||!strcmp(previous,"stopped")||
        !strcmp(previous,"failed")||!strcmp(previous,"timeout")))strcpy(s->status.last_known,previous);
    snprintf(s->status.state,sizeof(s->status.state),"%s",state);
    snprintf(s->status.source,sizeof(s->status.source),"%s",source);
    if(strcmp(state,"unknown")&&strcmp(source,"request"))s->status.confirmed_ms=esp_timer_get_time()/1000;
}
/* Registry -> binding -> service. State mutation never calls into variables
 * while holding the service lock; deletion does not visit this registry. */
static void publish(const char *id,uint32_t generation)
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *entry=find(id);
    uint64_t epoch=entry&&entry->owns_variables&&entry->status.generation==generation?entry->origin.epoch:0;
    xSemaphoreGive(mutex);
    /* Restored records have no permission to access the variable registry,
     * which may not yet be initialized when security becomes available. */
    if(!epoch||epoch!=ts_auto_config_binding_epoch())return;
    ts_variable_registration_begin();ts_ssh_binding_lock();
    ts_ssh_run_origin_t origin={0};char command_id[TS_SSH_CMD_ID_MAX]={0},host_id[TS_SSH_HOST_ID_MAX]={0};
    char variable[TS_SSH_CMD_VARNAME_MAX]={0},state[16]={0};
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    if(s&&s->owns_variables&&s->status.generation==generation){
        origin=s->origin;strcpy(command_id,s->record.command_id);strcpy(host_id,s->record.host.id);
        strcpy(variable,s->record.variable);strcpy(state,s->status.state);
    }
    xSemaphoreGive(mutex);
    bool current=origin.epoch&&origin.epoch==ts_auto_config_binding_epoch()&&
        ts_auto_config_host_instance(host_id)==origin.host_instance&&
        ts_auto_config_command_current(command_id,origin.command_instance);
    if(current&&variable[0]){
        char key[64];snprintf(key,sizeof(key),"%s.status",variable);
        if(origin.variables[0]&&ts_variable_instance(key)==origin.variables[0])ts_variable_set_string(key,state);
        snprintf(key,sizeof(key),"%s.ready_time",variable);
        if(!strcmp(state,"ready")&&origin.variables[1]&&ts_variable_instance(key)==origin.variables[1])ts_variable_set_int(key,(int32_t)time(NULL));
    }
    ts_ssh_binding_unlock();ts_variable_registration_end();
}

esp_err_t ts_ssh_service_init(void)
{
    if(initialized)return ESP_OK;
    if(!binding_gate)binding_gate=xSemaphoreCreateRecursiveMutex();
    if(!mutex)mutex=xSemaphoreCreateMutex();
    if(!services)services=heap_caps_calloc(TS_SSH_RUN_SLOTS,sizeof(*services),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!binding_gate||!mutex||!services)return ESP_ERR_NO_MEM;
    for(unsigned i=0;i<TS_SSH_RUNS_MAX;++i){
        service_t *s=&services[i];memset(s,0,sizeof(*s));
        esp_err_t ret=ts_ssh_run_load(i,&s->record);
        if(ret==ESP_ERR_NVS_NOT_FOUND)continue;
        if(ret!=ESP_OK)return ret;
        s->durable=true;s->status.generation=ticket();
        strcpy(s->status.state,"unknown");strcpy(s->status.source,"restart");
    }
    memset(&services[TS_SSH_RUNS_MAX],0,sizeof(*services));
    initialized=true;return ESP_OK;
}
static void new_identity(char out[32])
{
    uint8_t random[12];esp_fill_random(random,sizeof(random));
    for(unsigned i=0;i<sizeof(random);++i)snprintf(out+i*2,3,"%02x",random[i]);
}
static esp_err_t prepare(const ts_ssh_command_config_t *command,const ts_ssh_host_config_t *host,
    const ts_ssh_run_origin_t *origin,char run_id[TS_SSH_RUN_ID_MAX],uint32_t *operation_id,const char **reason,bool queued)
{
    if(reason)*reason="invalid_request";
    if(!initialized){if(reason)*reason="service_controls_unavailable";return ESP_ERR_INVALID_STATE;}
    if(!command||!host||!command->nohup||!run_id||!operation_id)return ESP_ERR_INVALID_ARG;
    if(host->auth_type!=TS_SSH_HOST_AUTH_KEY||!host->keyid[0]){if(reason)*reason="service_key_required";return ESP_ERR_NOT_SUPPORTED;}
    if(reason)*reason=NULL;
    run_id[0]=0;*operation_id=0;
    xSemaphoreTake(mutex,portMAX_DELAY);
    ts_ssh_host_config_t target=*host;target.port=target.port?target.port:22;
    bool existing=duplicate_busy(command->id,&target);
    service_t *s=existing?NULL:queued?&services[TS_SSH_RUNS_MAX]:free_slot();
    if(s&&queued&&(s->pending_control||s->status.busy)){s=NULL;existing=true;}
    esp_err_t ret=s?ESP_OK:existing?ESP_ERR_INVALID_STATE:ESP_ERR_INVALID_SIZE;uint32_t token=s?ticket():0;
    if(!s&&reason)*reason=existing?"service_control_busy":"service_record_limit";
    if(s&&!token){ret=ESP_ERR_INVALID_STATE;if(reason)*reason="service_operation_limit";}
    if(ret==ESP_OK){
        memset(s,0,sizeof(*s));new_identity(s->record.run_id);
        for(unsigned i=0;i<TS_SSH_RUNS_MAX;++i)
            if(&services[i]!=s&&!strcmp(services[i].record.run_id,s->record.run_id)){ret=ESP_ERR_INVALID_STATE;if(reason)*reason="service_record_create_failed";}
        strcpy(s->record.command_id,command->id);strcpy(s->record.name,command->name);
        strcpy(s->record.variable,command->var_name);s->record.host=target;
        strcpy(s->record.command_object_id,command->object_id);
        snprintf(s->record.locator,sizeof(s->record.locator),"r_%.24s",s->record.run_id);
        if(origin){s->origin=*origin;strcpy(s->record.rule,origin->rule);strcpy(s->record.config_version,origin->config_version);}
        if(ret==ESP_OK&&!queued){ret=ts_ssh_run_save(index_of(s),&s->record);if(ret!=ESP_OK&&reason)*reason=ret==ESP_ERR_NO_MEM?"out_of_memory":"service_record_save_failed";}
        if(ret==ESP_OK){
            s->durable=!queued;s->pending_control=token;s->status.generation=token;s->status.operation_id=token;
            strcpy(s->status.state,"unknown");strcpy(s->status.source,"planned");
            strcpy(s->status.operation_phase,"queued");strcpy(s->status.operation_kind,"start");
            strcpy(run_id,s->record.run_id);*operation_id=token;
        }else memset(s,0,sizeof(*s));
    }
    xSemaphoreGive(mutex);return ret;
}
esp_err_t ts_ssh_service_prepare(const ts_ssh_command_config_t *command,const ts_ssh_host_config_t *host,
    const ts_ssh_run_origin_t *origin,char run_id[TS_SSH_RUN_ID_MAX],uint32_t *operation_id,const char **reason)
{
    return prepare(command,host,origin,run_id,operation_id,reason,false);
}
esp_err_t ts_ssh_service_plan(const ts_ssh_command_config_t *command,const ts_ssh_host_config_t *host,
    const ts_ssh_run_origin_t *origin,char run_id[TS_SSH_RUN_ID_MAX],uint32_t *operation_id,const char **reason)
{
    return prepare(command,host,origin,run_id,operation_id,reason,true);
}
static void legacy_name(const ts_ssh_command_config_t *cmd, char out[32]) {
    unsigned n = 0;
    const char *src = cmd->name;
    for (unsigned pass = 0; pass < 2 && !n; pass++, src = cmd->id)
        for (unsigned i = 0; src[i] && n < 20; i++)
            if ((src[i] >= 'a' && src[i] <= 'z') || (src[i] >= 'A' && src[i] <= 'Z') ||
                (src[i] >= '0' && src[i] <= '9'))
                out[n++] = src[i];
    if (!n) {
        strcpy(out, "cmd");
        return;
    }
    out[n] = 0;
}
static bool instance_receipt(const char *text, const char *prefix, char out[96]) {
    if (!text || strncmp(text, prefix, strlen(prefix))) return false;
    const char *v = text + strlen(prefix);
    size_t len = strcspn(v, "\r\n");
    if (!len || len >= 96 || strspn(v, "0123456789abcdefABCDEF-:") != len ||
        (v[len] && strcmp(v + len, "\n") && strcmp(v + len, "\r\n"))) return false;
    unsigned separators = 0;
    for (size_t i = 0; i < len; ++i) separators += v[i] == ':';
    if (separators != 2) return false;
    if (out) { memcpy(out, v, len); out[len] = 0; }
    return true;
}

esp_err_t ts_ssh_service_import_legacy(const ts_ssh_command_config_t *command,const ts_ssh_host_config_t *host)
{
    if(!initialized||!command||!host)return ESP_ERR_INVALID_ARG;
    if(!command->nohup)return ESP_OK;
    char locator[32];legacy_name(command,locator);
    xSemaphoreTake(mutex,portMAX_DELAY);
    for(unsigned i=0;i<TS_SSH_RUNS_MAX;++i)
        if(services[i].durable&&same_target(&services[i].record.host,host)&&!strcmp(services[i].record.locator,locator)){
            xSemaphoreGive(mutex);return ESP_OK;
        }
    service_t *s=free_slot();esp_err_t ret=s?ESP_OK:ESP_ERR_INVALID_SIZE;
    if(s){
        memset(s,0,sizeof(*s));new_identity(s->record.run_id);
        strcpy(s->record.command_id,command->id);strcpy(s->record.name,command->name);
        strcpy(s->record.variable,command->var_name);strcpy(s->record.locator,locator);
        s->record.host=*host;s->record.host.port=host->port?host->port:22;
        s->record.kind=command->service_mode?TS_SSH_RUN_LEGACY_GROUP:TS_SSH_RUN_LEGACY_PID;
        ret=ts_ssh_run_save(index_of(s),&s->record);
        if(ret==ESP_OK){s->durable=true;s->status.generation=ticket();strcpy(s->status.state,"unknown");strcpy(s->status.source,"migration");}
        else memset(s,0,sizeof(*s));
    }
    xSemaphoreGive(mutex);return ret;
}

/* A locator identifies one launch. Legacy PID-only files are adopted by an
 * explicit verification before their first stop; new launches always use groups. */
static esp_err_t probe(ts_ssh_session_t session,const ts_ssh_run_record_t *record,bool stop,bool *running,char instance[96])
{
    if(stop&&record->kind==TS_SSH_RUN_LEGACY_PID&&!record->instance[0])return ESP_ERR_INVALID_STATE;
    char *command=heap_caps_malloc(3072,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!command)return ESP_ERR_NO_MEM;
    const char *path=record->kind==TS_SSH_RUN_LEGACY_PID?"f=/tmp/ts_nohup_%s.pid; d=; ":
        record->kind==TS_SSH_RUN_LEGACY_GROUP?
        "d=/tmp/ts_nohup_%s.lock; f=$d/pid; if [ ! -d \"$d\" ]; then f=${d%%.lock}.pid; fi; ":
        "d=/tmp/ts_nohup_%s.lock; f=$d/pid; if [ -d \"$d\" ] && [ ! -f \"$f\" ]; then exit 2; fi; ";
    int n=snprintf(command,3072,path,record->locator);
    const char *body=
        "if [ ! -f \"$f\" ]; then echo STOPPED; exit 0; fi; "
        "read p stamp < \"$f\" || exit 2; case $p in ''|*[!0-9]*) exit 2;; esac; [ \"$p\" -gt 1 ] || exit 2; "
        "if [ ! -r /proc/$p/stat ]; then if kill -0 -$p 2>/dev/null; then exit 2; "
        "else rm -f -- \"$f\"; [ -z \"$d\" ] || [ ! -d \"$d\" ] || rmdir -- \"$d\" || exit 2; echo STOPPED; exit 0; fi; fi; "
        "info=$(sed 's/^.*) //' /proc/$p/stat) || exit 2; set -- $info; pg=$3; shift 19; actual=$1; "
        "[ -z \"$stamp\" ] || [ \"$stamp\" = \"$actual\" ] || exit 2; "
        "boot=$(cat /proc/sys/kernel/random/boot_id) || exit 2; identity=$boot:$p:$actual; ";
    if(n<0||(size_t)n>=3072||strlen(body)>=3072-(size_t)n){free(command);return ESP_ERR_INVALID_SIZE;}
    strcpy(command+n,body);n+=strlen(body);
    if(stop){
        if(record->instance[0])n+=snprintf(command+n,3072-n,"[ \"$identity\" = '%s' ] || exit 2; ",record->instance);
        if(record->kind!=TS_SSH_RUN_LEGACY_PID)n+=snprintf(command+n,3072-n,"[ -n \"$stamp\" ] && [ \"$pg\" = \"$p\" ] || exit 2; ");
        n+=snprintf(command+n,3072-n,
            "target=%s; kill -TERM \"$target\" || exit 2; i=0; while [ $i -lt 30 ]; do "
            "if ! kill -0 \"$target\" 2>/dev/null; then rm -f -- \"$f\"; "
            "[ -z \"$d\" ] || [ ! -d \"$d\" ] || rmdir -- \"$d\" || exit 2; echo STOPPED; exit 0; fi; "
            "sleep 0.1; i=$((i+1)); done; exit 3",record->kind==TS_SSH_RUN_LEGACY_PID?"$p":"-$p");
    }else n+=snprintf(command+n,3072-n,"printf 'RUNNING %%s\\n' \"$identity\"");
    if(n<0||n>=3072){free(command);return ESP_ERR_INVALID_SIZE;}
    ts_ssh_exec_result_t result={0};esp_err_t ret=ts_ssh_exec(session,command,&result);free(command);
    if(ret==ESP_OK){
        if(result.exit_code==0&&ts_ssh_probe_token(result.stdout_data,"STOPPED"))*running=false;
        else if(!stop&&result.exit_code==0&&instance_receipt(result.stdout_data,"RUNNING ",instance))*running=true;
        else ret=ESP_ERR_INVALID_RESPONSE;
    }
    ts_ssh_exec_result_free(&result);return ret;
}

/* One owner applies process observations for verification, stop and start checks.
 * Caller holds the registry mutex and has reserved the slot against reuse. */
static esp_err_t apply_probe(service_t *s,esp_err_t ret,bool running,const char *identity,
    const char **reason,bool *cancel_watch)
{
    *cancel_watch=false;
    ++s->observation_revision;
    if(ret!=ESP_OK){state_set(s,"unknown","pid");return ret;}
    /* A check of the same process preserves its readiness observer. Only a
     * confirmed end/replacement invalidates observations from the old process. */
    if(!running||strcmp(identity,s->record.instance)){
        *cancel_watch=true;
        if(strcmp(s->status.state,"stopping"))s->status.generation=ticket();
        s->readiness[0]=0;
    }
    if(!running){
        ret=ts_ssh_run_remove(index_of(s));
        if(ret==ESP_OK){s->durable=false;s->record.instance[0]=0;state_set(s,"stopped","pid");}
        else {if(reason)*reason="service_record_save_failed";state_set(s,"unknown","pid");}
    }else{
        bool same=!strcmp(identity,s->record.instance);
        if(!same){strcpy(s->record.instance,identity);ret=ts_ssh_run_save(index_of(s),&s->record);}
        bool observed=s->readiness[0]&&strcmp(s->readiness,"unknown");
        state_set(s,observed?s->readiness:"running",observed?"pid+log":"pid");
        if(ret!=ESP_OK&&reason)*reason="service_record_save_failed";
    }
    return ret;
}

static bool start_deadline_passed(void *context)
{
    int64_t deadline=*(const int64_t *)context;
    return deadline && esp_timer_get_time()/1000>=deadline;
}
esp_err_t ts_ssh_service_check_start(const char *command_id,const ts_ssh_host_config_t *host,
    const char *exclude_run,int64_t deadline,const char **reason)
{
    if(reason)*reason=NULL;
    if(!initialized||!command_id||!host)return ESP_ERR_INVALID_STATE;
    for(unsigned i=0;i<TS_SSH_RUN_SLOTS;++i){
        if(start_deadline_passed(&deadline)){if(reason)*reason="automatic_start_timeout";return ESP_ERR_TIMEOUT;}
        ts_ssh_run_record_t record;uint32_t generation=0;
        xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=&services[i];
        bool matches=s->record.run_id[0]&&(s->durable||s->pending_control)&&!strcmp(s->record.command_id,command_id)&&same_target(&s->record.host,host)&&
            (!exclude_run||strcmp(s->record.run_id,exclude_run));
        if(matches&&(s->pending_control||s->status.busy)){
            xSemaphoreGive(mutex);if(reason)*reason="service_control_busy";return ESP_ERR_INVALID_STATE;
        }
        if(matches){s->status.busy=true;record=s->record;}
        xSemaphoreGive(mutex);if(!matches)continue;
        ts_ssh_session_t session=NULL;char *key=NULL;const char *failed=NULL,*stage=NULL;
        bool running=true;char identity[96]={0};
        esp_err_t ret=ts_ssh_service_connect(&record.host,&session,&key,start_deadline_passed,&deadline);
        if(ret!=ESP_OK)failed=ret==TS_SSH_ERR_HOST_UNKNOWN?"ssh_host_key_unknown":
            ret==TS_SSH_ERR_HOST_CHANGED?"ssh_host_key_changed":ret==ESP_ERR_NO_MEM?"out_of_memory":!key?"ssh_key_unavailable":
            session?ts_ssh_connect_reason(session,&stage):"ssh_connect_failed";
        if(ret==ESP_OK){ret=probe(session,&record,false,&running,identity);if(ret!=ESP_OK)failed="service_process_unconfirmed";}
        if(session)ts_ssh_session_destroy(session);
        free(key);
        xSemaphoreTake(mutex,portMAX_DELAY);s=&services[i];
        bool cancel_watch=false;
        ret=apply_probe(s,ret,running,identity,&failed,&cancel_watch);
        generation=s->status.generation;
        if(ret==ESP_OK&&running){failed="service_already_running";ret=ESP_ERR_INVALID_STATE;}
        s->status.busy=false;xSemaphoreGive(mutex);
        if(cancel_watch)ts_ssh_log_watch_cancel_run(record.run_id);
        publish(record.run_id,generation);
        if(ret!=ESP_OK){if(reason)*reason=failed;return ret;}
    }
    if(!exclude_run){
        xSemaphoreTake(mutex,portMAX_DELAY);bool room=free_slot()!=NULL;xSemaphoreGive(mutex);
        if(!room){if(reason)*reason="service_record_limit";return ESP_ERR_INVALID_SIZE;}
    }
    return ESP_OK;
}

esp_err_t ts_ssh_service_reserve_operation(const char *id,ts_service_operation_t kind,uint32_t *operation_id,const char **reason)
{
    if(reason)*reason="invalid_request";
    if(!initialized){if(reason)*reason="service_controls_unavailable";return ESP_ERR_INVALID_STATE;}
    if(!id||!operation_id||(kind!=TS_SERVICE_VERIFY&&kind!=TS_SERVICE_STOP))return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);uint32_t token=0;
    if(!s){if(reason)*reason="service_record_not_found";}
    else if(s->status.busy||s->pending_control){if(reason)*reason="service_control_busy";}
    else{token=ticket();if(reason)*reason=token?NULL:"service_operation_limit";}
    if(token){s->pending_control=token;s->status.operation_id=token;strcpy(s->status.operation_phase,"queued");
        strcpy(s->status.operation_kind,kind==TS_SERVICE_STOP?"stop":"verify");s->status.operation_error=ESP_OK;s->status.operation_reason[0]=0;*operation_id=token;}
    xSemaphoreGive(mutex);return token?ESP_OK:s?ESP_ERR_INVALID_STATE:ESP_ERR_NOT_FOUND;
}
esp_err_t ts_ssh_service_operation_begin(const char *id,uint32_t operation_id)
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool current=s&&operation_id&&s->pending_control==operation_id;
    if(current)strcpy(s->status.operation_phase,"executing");
    xSemaphoreGive(mutex);return current?ESP_OK:ESP_ERR_INVALID_STATE;
}
void ts_ssh_service_set_operation_reason(const char *id,uint32_t operation_id,const char *reason)
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    if(s&&operation_id&&s->status.operation_id==operation_id)
        snprintf(s->status.operation_reason,sizeof(s->status.operation_reason),"%s",reason?reason:"");
    xSemaphoreGive(mutex);
}
bool ts_ssh_service_complete_operation(const char *id,uint32_t operation_id,esp_err_t error)
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);bool executed=false;uint32_t generation=0;
    if(s&&operation_id&&s->status.operation_id==operation_id){
        executed=!strcmp(s->status.operation_phase,"executing");
        bool queued=!strcmp(s->status.operation_phase,"queued");
        s->pending_control=0;s->operation=0;s->status.busy=false;generation=s->status.generation;
        if(!strcmp(s->status.operation_kind,"start")&&!s->launch_attempted){
            esp_err_t cleanup=s->durable?ts_ssh_run_remove(index_of(s)):ESP_OK;
            if(cleanup==ESP_OK){s->durable=false;state_set(s,"stopped","not_launched");}
            else if(error==ESP_OK)error=cleanup;
        }
        if(error==ESP_OK)s->status.operation_reason[0]=0;
        else if(!s->status.operation_reason[0])snprintf(s->status.operation_reason,sizeof(s->status.operation_reason),"%s",
            error==ESP_ERR_NO_MEM?"out_of_memory":!strcmp(s->status.operation_kind,"start")&&!s->launch_attempted?"service_not_started":"service_outcome_unknown");
        const char *phase=error==ESP_OK?"succeeded":error==ESP_ERR_TIMEOUT?(queued?"expired":"unconfirmed"):"failed";
        strcpy(s->status.operation_phase,phase);s->status.operation_error=error;
    }
    xSemaphoreGive(mutex);if(generation)publish(id,generation);return executed;
}
esp_err_t ts_ssh_service_begin_reserved(const char *id,ts_ssh_session_t session,uint32_t operation_id,uint32_t *generation,const char **failure)
{
    if(failure)*failure=NULL;
    if(!session||!generation)return ESP_ERR_INVALID_ARG;
    ts_ssh_run_record_t record;int64_t deadline=0;
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool current=s&&operation_id&&s->pending_control==operation_id&&!s->status.busy&&(s->durable||index_of(s)==TS_SSH_RUNS_MAX);
    if(current){s->operation=operation_id;s->status.busy=true;record=s->record;deadline=s->origin.deadline_ms;}
    xSemaphoreGive(mutex);if(!current)return ESP_ERR_INVALID_STATE;
    const char *reason=NULL;
    esp_err_t ret=ts_ssh_service_check_start(record.command_id,&record.host,id,deadline,&reason);
    if(ret!=ESP_OK){
        if(failure)*failure=reason;
        ts_ssh_service_set_operation_reason(id,operation_id,reason);
        xSemaphoreTake(mutex,portMAX_DELAY);s=find(id);s->status.busy=false;xSemaphoreGive(mutex);
        return ret;
    }
    /* A queued request owns an identity, but no durable slot until old runs
     * have been checked. Save before any remote launch can be attempted. */
    xSemaphoreTake(mutex,portMAX_DELAY);s=find(id);
    if(!s->durable){
        service_t *destination=free_slot();
        if(!destination){ret=ESP_ERR_INVALID_SIZE;reason="service_record_limit";}
        else {
            ret=ts_ssh_run_save(index_of(destination),&s->record);
            if(ret==ESP_OK){*destination=*s;destination->durable=true;memset(s,0,sizeof(*s));s=destination;}
            else reason="service_record_save_failed";
        }
    }
    if(ret!=ESP_OK){s->status.busy=false;snprintf(s->status.operation_reason,sizeof(s->status.operation_reason),"%s",reason);}
    xSemaphoreGive(mutex);if(ret!=ESP_OK){if(failure)*failure=reason;return ret;}
    bool running=true;ret=probe(session,&record,false,&running,NULL);
    xSemaphoreTake(mutex,portMAX_DELAY);s=find(id);
    /* Only a real launch may own the command variables. Local rejected requests
     * never acquire ownership; older runs lose it when a new launch takes over. */
    if(ret==ESP_OK){
        for(unsigned n=0;n<TS_SSH_RUN_SLOTS;++n)
            if(services[n].origin.epoch==s->origin.epoch &&
               services[n].origin.variables[0]==s->origin.variables[0])services[n].owns_variables=false;
        s->owns_variables=true;
    }
    if(ret==ESP_OK&&!running){s->launch_attempted=true;*generation=s->status.generation;state_set(s,"starting","request");}
    else {s->status.busy=false;state_set(s,ret==ESP_OK?"running":"unknown","pid");if(ret==ESP_OK){s->launch_attempted=true;ret=ESP_ERR_INVALID_STATE;}}
    if(ret!=ESP_OK&&failure)*failure=running&&ret==ESP_ERR_INVALID_STATE?"service_already_running":"service_process_unconfirmed";
    uint32_t gen=s->status.generation;xSemaphoreGive(mutex);publish(id,gen);return ret;
}
esp_err_t ts_ssh_service_finish(const char *id,uint32_t generation,const char *receipt,ts_ssh_session_t session)
{
    char expected[96],actual[96]={0};bool launched=instance_receipt(receipt,"STARTED ",expected);
    ts_ssh_run_record_t record;
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool current=s&&s->operation==generation&&s->status.generation==generation;
    if(current)record=s->record;
    xSemaphoreGive(mutex);if(!current)return ESP_ERR_INVALID_STATE;
    bool running=false;esp_err_t ret=launched?probe(session,&record,false,&running,actual):ESP_ERR_INVALID_RESPONSE;
    launched=ret==ESP_OK&&running&&!strcmp(actual,expected);
    xSemaphoreTake(mutex,portMAX_DELAY);s=find(id);s->status.busy=false;s->operation=0;
    if(launched){strcpy(s->record.instance,actual);ret=ts_ssh_run_save(index_of(s),&s->record);state_set(s,"running","launch");}
    else if(ret==ESP_OK&&!running){
        ret=ts_ssh_run_remove(index_of(s));
        if(ret==ESP_OK){s->durable=false;s->record.instance[0]=0;state_set(s,"stopped","launch");}
        else state_set(s,"unknown","launch");
    }
    else {state_set(s,"unknown","launch");if(ret==ESP_OK)ret=ESP_ERR_INVALID_RESPONSE;}
    xSemaphoreGive(mutex);publish(id,generation);return ret;
}
bool ts_ssh_service_launch_command(const char *id,const char *expanded,char *out,size_t cap)
{
    char name[32];xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool valid=s&&(s->durable||s->pending_control)&&s->record.kind==TS_SSH_RUN_GROUP;
    if(valid)strcpy(name,s->record.locator);
    xSemaphoreGive(mutex);if(!valid)return false;
    int n=snprintf(out,cap,"mkdir /tmp/ts_nohup_%s.lock || exit 2; nohup setsid sh -c ",name);
    if(n<0||(size_t)n>=cap)return false;
    /* The group leader records its identity before the user command can exit. */
    char child[1024];
    int length=snprintf(child,sizeof(child),
        "cmd=$1; p=$$; info=$(sed 's/^.*) //' /proc/$p/stat) || exit 2; "
        "set -- $info; [ \"$3\" = \"$p\" ] || exit 2; shift 19; "
        "boot=$(cat /proc/sys/kernel/random/boot_id) || exit 2; "
        "printf '%%s %%s\\n' \"$p\" \"$1\" > /tmp/ts_nohup_%s.lock/pid || exit 2; "
        "printf 'STARTED %%s:%%s:%%s\\n' \"$boot\" \"$p\" \"$1\"; "
        "exec sh -c \"$cmd\" > /tmp/ts_nohup_%s.log 2>&1",name,name);
    if(length<0||(size_t)length>=sizeof(child)||!ts_ssh_shell_quote(child,out+n,cap-n))return false;
    n+=strlen(out+n);
    int more=snprintf(out+n,cap-n," sh ");
    if(more<0||(size_t)more>=cap-n)return false;
    n+=more;
    if(!ts_ssh_shell_quote(expanded,out+n,cap-n))return false;
    n+=strlen(out+n);
    more=snprintf(out+n,cap-n," < /dev/null 2> /tmp/ts_nohup_%s.log &",name);
    return more>=0&&(size_t)more<cap-n;
}
esp_err_t ts_ssh_service_connect(const ts_ssh_host_config_t *host,ts_ssh_session_t *session,char **key,
    bool (*cancelled)(void *),void *context)
{
    if(host->auth_type!=TS_SSH_HOST_AUTH_KEY||!host->keyid[0])return ESP_ERR_INVALID_STATE;
    size_t length=0;esp_err_t ret=ts_keystore_load_private_key(host->keyid,key,&length);
    if(ret!=ESP_OK)return ret;
    ts_ssh_config_t cfg=TS_SSH_DEFAULT_CONFIG();cfg.host=host->host;cfg.port=host->port;cfg.username=host->username;
    cfg.auth_method=TS_SSH_AUTH_PUBLICKEY;cfg.auth.key.private_key=(const uint8_t *)*key;cfg.auth.key.private_key_len=length;
    cfg.timeout_ms=5000;cfg.max_output_bytes=128;cfg.cancelled=cancelled;cfg.cancel_context=context;
    ret=ts_ssh_session_create(&cfg,session);return ret==ESP_OK?ts_ssh_connect(*session):ret;
}
esp_err_t ts_ssh_service_execute_control(const char *id,uint32_t operation_id,ts_service_operation_t kind,ts_ssh_service_status_t *out)
{
    if(!id||!operation_id||!out||(kind!=TS_SERVICE_VERIFY&&kind!=TS_SERVICE_STOP))return ESP_ERR_INVALID_ARG;
    ts_ssh_run_record_t record;uint32_t generation=0;
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool current=s&&s->pending_control==operation_id&&!s->status.busy;
    if(current){s->operation=operation_id;s->status.busy=true;
        if(kind==TS_SERVICE_STOP){
            s->status.generation=operation_id;
            if(!strcmp(s->readiness,"checking"))s->readiness[0]=0;
        }
        record=s->record;state_set(s,kind==TS_SERVICE_STOP?"stopping":"checking","request");}
    xSemaphoreGive(mutex);if(!current)return ESP_ERR_INVALID_STATE;
    /* Verification is an operation on a run, not a new observation generation. */
    if(kind==TS_SERVICE_STOP)ts_ssh_log_watch_cancel_run(id);
    ts_ssh_session_t session=NULL;char *key=NULL;bool running=true;char identity[96]={0};
    esp_err_t ret=ts_ssh_service_connect(&record.host,&session,&key,NULL,NULL);
    const char *reason=NULL,*stage=NULL;
    if(ret!=ESP_OK)reason=ret==ESP_ERR_NO_MEM?"out_of_memory":!key?"ssh_key_unavailable":
        session?ts_ssh_connect_reason(session,&stage):"ssh_connect_failed";
    if(ret==ESP_OK){ret=probe(session,&record,kind==TS_SERVICE_STOP,&running,identity);
        if(ret!=ESP_OK)reason=ret==ESP_ERR_NO_MEM?"out_of_memory":"service_process_unconfirmed";}

    if(session)ts_ssh_session_destroy(session);
    free(key);
    xSemaphoreTake(mutex,portMAX_DELAY);s=find(id);
    s->operation=0;s->status.busy=false;
    bool cancel_watch=false;
    ret=apply_probe(s,ret,running,identity,&reason,&cancel_watch);
    generation=s->status.generation;
    if(reason)snprintf(s->status.operation_reason,sizeof(s->status.operation_reason),"%s",reason);
    *out=s->status;xSemaphoreGive(mutex);
    if(cancel_watch&&kind!=TS_SERVICE_STOP)ts_ssh_log_watch_cancel_run(id);
    publish(id,generation);return ret;
}
esp_err_t ts_ssh_service_cached(const char *id,ts_ssh_service_status_t *out)
{
    if(!initialized||!id||!out)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    if(s)*out=s->status;
    else {*out=(ts_ssh_service_status_t){0};strcpy(out->state,"unknown");strcpy(out->source,"none");}
    xSemaphoreGive(mutex);return s?ESP_OK:ESP_ERR_NOT_FOUND;
}
bool ts_ssh_service_current(const char *command,const ts_ssh_host_config_t *host,
    uint64_t command_instance,const char *command_object_id,
    char run_id[TS_SSH_RUN_ID_MAX],ts_ssh_service_status_t *status)
{
    if(!initialized)return false;
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *selected=NULL;unsigned rank=0;
    for(unsigned i=0;i<TS_SSH_RUN_SLOTS;++i){
        service_t *s=&services[i];
        if(!s->record.run_id[0]||strcmp(s->record.command_id,command)||!same_target(&s->record.host,host))continue;
        /* A restored legacy record has no proof of which definition created it.
         * It remains independently controllable, but is not projected as current. */
        if(command_instance){
            if(s->record.command_object_id[0]||(command_object_id&&command_object_id[0])){
                if(!command_object_id||strcmp(s->record.command_object_id,command_object_id))continue;
            }else if(!s->origin.command_instance||s->origin.command_instance!=command_instance)continue;
        }
        unsigned candidate=s->durable?2:s->pending_control?1:0;
        if(!selected||candidate>rank||(candidate==rank&&s->status.operation_id>selected->status.operation_id)){
            selected=s;rank=candidate;
        }
    }
    if(selected){strcpy(run_id,selected->record.run_id);*status=selected->status;}
    xSemaphoreGive(mutex);return selected!=NULL;
}
esp_err_t ts_ssh_service_get(unsigned index,ts_ssh_run_record_t *record,ts_ssh_service_status_t *status)
{
    if(!initialized||index>=TS_SSH_RUN_SLOTS||!record||!status)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=&services[index];bool found=s->record.run_id[0];
    if(found){*record=s->record;*status=s->status;}
    xSemaphoreGive(mutex);return found?ESP_OK:ESP_ERR_NOT_FOUND;
}
esp_err_t ts_ssh_service_target(const char *id,uint32_t generation,ts_ssh_host_config_t *host,char log_file[256])
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool valid=s&&s->status.generation==generation&&s->record.instance[0]&&strcmp(s->status.state,"stopped")&&strcmp(s->status.state,"stopping");
    if(valid){*host=s->record.host;snprintf(log_file,256,"/tmp/ts_nohup_%s.log",s->record.locator);}
    xSemaphoreGive(mutex);return valid?ESP_OK:ESP_ERR_INVALID_STATE;
}
esp_err_t ts_ssh_service_verify_instance(const char *id,uint32_t generation,ts_ssh_session_t session)
{
    ts_ssh_run_record_t record;
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool valid=s&&s->status.generation==generation&&s->record.instance[0]&&strcmp(s->status.state,"stopped")&&strcmp(s->status.state,"stopping");
    if(valid)record=s->record;
    xSemaphoreGive(mutex);if(!valid)return ESP_ERR_INVALID_STATE;
    bool running=false;char actual[96]={0};esp_err_t ret=probe(session,&record,false,&running,actual);
    return ret==ESP_OK&&(!running||strcmp(actual,record.instance))?ESP_ERR_INVALID_STATE:ret;
}
bool ts_ssh_service_observation_valid(const char *id,uint32_t generation)
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool valid=s&&s->status.generation==generation&&s->record.instance[0]&&strcmp(s->status.state,"stopped")&&strcmp(s->status.state,"stopping");
    xSemaphoreGive(mutex);return valid;
}
bool ts_ssh_service_observation_begin(const char *id,uint32_t generation,uint64_t *revision)
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool valid=s&&s->status.generation==generation&&s->record.instance[0]&&strcmp(s->status.state,"stopped")&&strcmp(s->status.state,"stopping");
    if(valid)*revision=s->observation_revision;
    xSemaphoreGive(mutex);return valid;
}
bool ts_ssh_service_observe_at(const char *id,uint32_t generation,uint64_t revision,const char *state)
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool valid=s&&s->status.generation==generation&&s->record.instance[0]&&
        s->observation_revision==revision&&strcmp(s->status.state,"stopped")&&strcmp(s->status.state,"stopping");
    if(valid){snprintf(s->readiness,sizeof(s->readiness),"%s",state);state_set(s,state,"log");}
    xSemaphoreGive(mutex);if(valid)publish(id,generation);return valid;
}
void ts_ssh_service_observe(const char *id,uint32_t generation,const char *state)
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool valid=s&&s->status.generation==generation&&s->record.instance[0]&&strcmp(s->status.state,"stopped")&&strcmp(s->status.state,"stopping");
    if(valid){snprintf(s->readiness,sizeof(s->readiness),"%s",state);state_set(s,state,"log");}
    xSemaphoreGive(mutex);if(valid)publish(id,generation);
}
void ts_ssh_service_cancel_observation(const char *id,uint32_t generation)
{
    xSemaphoreTake(mutex,portMAX_DELAY);service_t *s=find(id);
    bool valid=s&&s->status.generation==generation&&strcmp(s->status.state,"stopped")&&strcmp(s->status.state,"stopping");
    uint32_t next=valid?ticket():0;
    if(next){
        s->status.generation=next;
        if(!strcmp(s->readiness,"checking"))s->readiness[0]=0;
        state_set(s,"unknown","cancelled");
    }
    xSemaphoreGive(mutex);if(next)publish(id,next);
}
