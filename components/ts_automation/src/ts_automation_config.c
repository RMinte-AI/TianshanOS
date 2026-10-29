#include "ts_automation_config.h"
#include "ts_variable.h"
#include "ts_automation_file.h"
#include "ts_automation_control.h"
#include "ts_automation_migrate.h"
#include "ts_automation_runtime.h"
#include "ts_rule_codec.h"
#include "ts_ssh_service.h"
#include "ts_storage.h"
#include "ts_crypto.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

struct ts_auto_config_view {
    unsigned refs;
    bool configured;
    char fingerprint[65];
    cJSON *json;
    ts_auto_document_t *definitions;
    ts_auto_config_error_t error;
    atomic_uint command_times[TS_SSH_COMMANDS_MAX];
    uint64_t template_instances[TS_ACTION_TEMPLATE_MAX];
    uint64_t host_instances[TS_SSH_HOSTS_MAX];
    int64_t template_times[TS_ACTION_TEMPLATE_MAX];
    uint32_t template_counts[TS_ACTION_TEMPLATE_MAX];
};

typedef struct {
    ts_rule_config_view_t *rules;
    ts_source_config_view_t *sources;
    ts_variable_config_view_t *variables;
} prepared_runtime_t;

typedef struct {
    bool exists,valid;
    uint32_t saved_at;
    char fingerprint[65];
    const char *error;
} internal_info_t;

static void discard_runtime(prepared_runtime_t *runtime)
{
    ts_variable_config_discard(runtime->variables);
    ts_source_config_discard(runtime->sources);
    ts_rule_config_discard(runtime->rules);
    *runtime = (prepared_runtime_t){0};
}

static struct {
    StaticSemaphore_t writer_storage, state_storage;
    SemaphoreHandle_t writer, state;
    ts_auto_config_view_t *current;
    ts_auto_config_source_t source;
    bool paused, uncertain, closing;
    bool control_loaded;
    bool sd_available;
    internal_info_t internal;
    ts_auto_config_error_t load_error;
    ts_auto_control_t control;
    uint32_t nonce[2];
    uint64_t generation, execution_epoch, binding_epoch;
    uint64_t next_command_instance;
    uint64_t next_template_instance;
    uint64_t next_host_instance;
    uint64_t sd_generation;
    char version[33];
} s_config;

static esp_err_t refresh_mode_locked(bool probe,ts_auto_config_error_t *error);

static esp_err_t fail(ts_auto_config_error_t *error, const char *reason, esp_err_t code)
{
    if (error) { error->reason = reason;error->saved=false; }
    return code;
}

static esp_err_t load_control(ts_auto_config_error_t *error)
{
    if(s_config.control_loaded)return ESP_OK;
    esp_err_t ret=ts_auto_control_load(&s_config.control);
    if(ret==ESP_OK)s_config.control_loaded=true;
    else fail(error,"configuration_settings_unavailable",ret);
    return ret;
}

static esp_err_t save_control(ts_auto_control_t control,ts_auto_config_error_t *error)
{
    esp_err_t ret=ts_auto_control_save(&control);
    if(ret==ESP_OK)s_config.control=control;
    else fail(error,"configuration_settings_not_saved",ret);
    return ret;
}

static void free_view(ts_auto_config_view_t *view)
{
    if (!view) return;
    cJSON_Delete(view->json);
    ts_auto_document_free(view->definitions);
    free(view);
}

static void next_version(void)
{
    ++s_config.generation;
    snprintf(s_config.version, sizeof(s_config.version), "%08" PRIx32 "%08" PRIx32 "%016" PRIx64,
             s_config.nonce[0], s_config.nonce[1], s_config.generation);
}

static void initialize(void)
{
    if (s_config.writer) return; /* Boot owns first initialization. Locks outlive late readers. */
    s_config.writer = xSemaphoreCreateRecursiveMutexStatic(&s_config.writer_storage);
    s_config.state = xSemaphoreCreateMutexStatic(&s_config.state_storage);
    esp_fill_random(s_config.nonce, sizeof(s_config.nonce));
    next_version();s_config.execution_epoch=1;s_config.binding_epoch=1;
}

static void status_locked(ts_auto_config_status_t *out)
{
    memset(out,0,sizeof(*out));
    out->loaded = s_config.current != NULL;
    out->source = s_config.source;
    out->paused = s_config.paused;
    out->uncertain = s_config.uncertain;
    out->configured = s_config.current && s_config.current->configured;
    out->sd_available = s_config.sd_available;
    out->internal_exists = s_config.internal.exists;
    out->internal_valid = s_config.internal.valid;
    out->internal_saved_at = s_config.internal.saved_at;
    out->internal_error = s_config.internal.error;
    out->awaiting_sd_choice = s_config.source==TS_AUTO_CONFIG_INTERNAL && s_config.sd_available;
    out->comparison_known = s_config.current && s_config.current->fingerprint[0] && s_config.internal.fingerprint[0];
    out->backup_matches = out->comparison_known && !strcmp(s_config.current->fingerprint,s_config.internal.fingerprint);
    out->error = s_config.load_error;
    memcpy(out->active_version, s_config.version, sizeof(out->active_version));
    if (s_config.current) {
        if(!out->error.reason)out->error = s_config.current->error;
        out->enabled = !s_config.paused && s_config.current->definitions &&
                       s_config.current->definitions->enabled;
    }
}

const ts_auto_config_view_t *ts_auto_config_acquire_status(ts_auto_config_status_t *status)
{
    if (!s_config.state) { if (status) memset(status,0,sizeof(*status));return NULL; }
    xSemaphoreTake(s_config.state, portMAX_DELAY);
    ts_auto_config_view_t *view = s_config.current;
    if (view) ++view->refs;
    if (status) status_locked(status);
    xSemaphoreGive(s_config.state);
    return view;
}

const ts_auto_config_view_t *ts_auto_config_acquire(void) { return ts_auto_config_acquire_status(NULL); }

void ts_auto_config_release(const ts_auto_config_view_t *borrowed)
{
    if (!borrowed) return;
    ts_auto_config_view_t *view = (ts_auto_config_view_t *)borrowed;
    xSemaphoreTake(s_config.state, portMAX_DELAY);
    bool destroy = --view->refs == 0;
    xSemaphoreGive(s_config.state);
    if (destroy) free_view(view);
}

const ts_auto_document_t *ts_auto_config_definitions(const ts_auto_config_view_t *view)
{
    return view ? view->definitions : NULL;
}

const cJSON *ts_auto_config_json(const ts_auto_config_view_t *view)
{
    return view ? view->json : NULL;
}

uint64_t ts_auto_config_execution_epoch(void)
{
    if(!s_config.state)return 0;
    xSemaphoreTake(s_config.state,portMAX_DELAY);uint64_t epoch=s_config.execution_epoch;
    xSemaphoreGive(s_config.state);return epoch;
}
uint64_t ts_auto_config_binding_epoch(void)
{
    if(!s_config.state)return 0;
    xSemaphoreTake(s_config.state,portMAX_DELAY);uint64_t epoch=s_config.binding_epoch;
    xSemaphoreGive(s_config.state);return epoch;
}
bool ts_auto_config_template_current(uint64_t instance)
{
    if(!instance||!s_config.state)return false;
    xSemaphoreTake(s_config.state,portMAX_DELAY);
    const ts_auto_config_view_t *view=s_config.current;bool current=false;
    for(int i=0;view&&view->definitions&&i<view->definitions->action_count;++i)
        current|=view->template_instances[i]==instance&&view->definitions->actions[i].enabled;
    xSemaphoreGive(s_config.state);return current;
}
uint64_t ts_auto_config_template_instance(const char *id)
{
    if(!id||!s_config.state)return 0;
    xSemaphoreTake(s_config.state,portMAX_DELAY);
    const ts_auto_config_view_t *view=s_config.current;uint64_t instance=0;
    for(int i=0;view&&view->definitions&&i<view->definitions->action_count;++i)
        if(!strcmp(view->definitions->actions[i].id,id)){instance=view->template_instances[i];break;}
    xSemaphoreGive(s_config.state);return instance;
}
bool ts_auto_config_command_current(const char *id, uint64_t instance)
{
    if(!id||!s_config.state)return false;
    xSemaphoreTake(s_config.state,portMAX_DELAY);
    const ts_auto_config_view_t *view=s_config.current;bool current=false;
    for(int i=0;view&&view->definitions&&i<view->definitions->command_count;++i)
        if(!strcmp(view->definitions->commands[i].id,id)){
            current=view->definitions->commands[i].instance==instance;break;
        }
    xSemaphoreGive(s_config.state);return current;
}
uint64_t ts_auto_config_host_instance(const char *id)
{
    if(!id||!s_config.state)return 0;
    xSemaphoreTake(s_config.state,portMAX_DELAY);
    const ts_auto_config_view_t *view=s_config.current;uint64_t instance=0;
    for(int i=0;view&&view->definitions&&i<view->definitions->host_count;++i)
        if(!strcmp(view->definitions->hosts[i].id,id)){instance=view->host_instances[i];break;}
    xSemaphoreGive(s_config.state);return instance;
}

uint32_t ts_auto_config_command_time(const ts_auto_config_view_t *view, int index)
{
    return view && view->definitions && index >= 0 && index < view->definitions->command_count ?
        atomic_load(&view->command_times[index]) : 0;
}

void ts_auto_config_command_executed(const char *id, uint64_t instance, uint32_t timestamp)
{
    if (!s_config.state || !id || !instance) return;
    xSemaphoreTake(s_config.state, portMAX_DELAY);
    ts_auto_config_view_t *view = s_config.current;
    const ts_auto_document_t *d = view ? view->definitions : NULL;
    for (int i = 0; d && i < d->command_count; ++i) {
        if (d->commands[i].instance == instance && !strcmp(d->commands[i].id, id)) {
            atomic_store(&view->command_times[i], timestamp);
            break;
        }
    }
    xSemaphoreGive(s_config.state);
}

void ts_auto_config_template_stats(const ts_auto_config_view_t *view, int index, ts_action_template_t *out)
{
    if (!view || !view->definitions || index < 0 || index >= view->definitions->action_count || !out) return;
    xSemaphoreTake(s_config.state, portMAX_DELAY);
    out->last_used_at = view->template_times[index];
    out->use_count = view->template_counts[index];
    xSemaphoreGive(s_config.state);
}

void ts_auto_config_template_used(const ts_auto_config_view_t *view, int index, int64_t timestamp)
{
    if (!view || !view->definitions || index < 0 || index >= view->definitions->action_count) return;
    ts_auto_config_template_used_instance(view->template_instances[index],timestamp);
}
void ts_auto_config_template_used_instance(uint64_t instance,int64_t timestamp)
{
    if(!instance || !s_config.state)return;
    xSemaphoreTake(s_config.state, portMAX_DELAY);
    ts_auto_config_view_t *current = s_config.current;
    const ts_auto_document_t *d = current ? current->definitions : NULL;
    for (int i = 0; d && i < d->action_count; ++i) {
        if (current->template_instances[i] != instance) continue;
        current->template_times[i] = timestamp;
        ++current->template_counts[i];
        break;
    }
    xSemaphoreGive(s_config.state);
}

void ts_auto_config_status(ts_auto_config_status_t *out)
{
    if (!out) return;
    memset(out,0,sizeof(*out));
    if (!s_config.state) return;
    xSemaphoreTake(s_config.state,portMAX_DELAY);
    status_locked(out);
    xSemaphoreGive(s_config.state);
}

static esp_err_t check_version(const char *expected, ts_auto_config_error_t *error)
{
    if(s_config.closing)return fail(error,"configuration_unavailable",ESP_ERR_INVALID_STATE);
    if (s_config.uncertain) return fail(error, "save_outcome_unknown", ESP_ERR_NOT_FINISHED);
    if (!s_config.current) return fail(error, "configuration_unavailable", ESP_ERR_INVALID_STATE);
    if (expected && strcmp(expected, s_config.version))
        return fail(error, "configuration_changed", ESP_ERR_INVALID_STATE);
    return ESP_OK;
}

esp_err_t ts_auto_config_edit_begin(const char *active_version, ts_auto_config_error_t *error)
{
    if (!s_config.writer || !active_version || !active_version[0])
        return fail(error, "configuration_version_required", ESP_ERR_INVALID_ARG);
    xSemaphoreTakeRecursive(s_config.writer, portMAX_DELAY);
    esp_err_t ret = check_version(active_version, error);
    if (ret != ESP_OK) xSemaphoreGiveRecursive(s_config.writer);
    return ret;
}

void ts_auto_config_edit_end(void)
{
    xSemaphoreGiveRecursive(s_config.writer);
}

static bool set(cJSON *json, const char *name, cJSON *value)
{
    if (!value) return false;
    bool ok = cJSON_HasObjectItem(json, name) ?
        cJSON_ReplaceItemInObjectCaseSensitive(json, name, value) : cJSON_AddItemToObject(json, name, value);
    if (!ok) cJSON_Delete(value);
    return ok;
}

static bool explicit_pause(const cJSON *json)
{
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(json, "pause_reason");
    return cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "enabled")) &&
           cJSON_IsString(reason) && reason->valuestring[0];
}

typedef enum { PREPARE_EDIT, PREPARE_LOAD, PREPARE_INSTALL, PREPARE_FORCE } prepare_mode_t;

static void strip_row_metadata(cJSON *row)
{
    const char *fields[]={"revision","_package","created_at","created_time"};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i)cJSON_DeleteItemFromObjectCaseSensitive(row,fields[i]);
}

static bool definition_fingerprint(const ts_auto_document_t *document,char digest[65])
{
    cJSON *json=ts_auto_document_encode(document);
    if(!json)return false;
    const char *metadata[]={"revision","saved_at","migration_version"};
    for(unsigned i=0;i<sizeof(metadata)/sizeof(metadata[0]);++i)cJSON_DeleteItemFromObjectCaseSensitive(json,metadata[i]);
    for(unsigned kind=0;kind<TS_AUTO_CONFIG_KIND_COUNT;++kind){
        cJSON *row;
        cJSON_ArrayForEach(row,cJSON_GetObjectItemCaseSensitive(json,ts_auto_config_collection(kind))){
            strip_row_metadata(row);
        }
    }
    char *bytes=cJSON_PrintUnformatted(json);cJSON_Delete(json);
    unsigned char hash[32];
    bool ok=bytes && ts_crypto_hash(TS_HASH_SHA256,bytes,strlen(bytes),hash,sizeof(hash))==ESP_OK &&
        ts_crypto_hex_encode(hash,sizeof(hash),digest,65)==ESP_OK;
    free(bytes);return ok;
}

/* Takes ownership of json. A loaded/paused management view may retain invalid
 * definitions. Only confirmed force bypasses runtime preparation for a save. */
static esp_err_t prepare(cJSON *json, prepare_mode_t mode, ts_auto_config_source_t source,
                          ts_auto_config_view_t **out, prepared_runtime_t *runtime,
                          ts_auto_config_error_t *error)
{
    *out = NULL;
    *runtime = (prepared_runtime_t){0};
    ts_auto_config_view_t *view = heap_caps_calloc(1, sizeof(*view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view) { cJSON_Delete(json); return fail(error, "out_of_memory", ESP_ERR_NO_MEM); }
    view->json = json;
    view->refs = 1;
    bool keep_runtime=s_config.current && source==s_config.source;
    bool preserve_paused = mode == PREPARE_LOAD || mode == PREPARE_INSTALL || explicit_pause(json);
    if (mode != PREPARE_FORCE) {
        esp_err_t ret = ts_auto_document_decode(json, &view->definitions, &view->error);
        if (ret == ESP_OK && view->definitions->enabled)
            ret = ts_auto_document_validate(view->definitions, &view->error);
        if (ret != ESP_OK && (!preserve_paused || (ret == ESP_ERR_NO_MEM && mode != PREPARE_LOAD))) {
            if (error) *error = view->error;
            free_view(view);
            return ret;
        }
        if (ret == ESP_OK) {
            if(!definition_fingerprint(view->definitions,view->fingerprint)){
                view->error.reason="out_of_memory";
                if(mode!=PREPARE_LOAD){if(error)*error=view->error;free_view(view);return ESP_ERR_NO_MEM;}
                *out=view;return ESP_OK;
            }
            if (!preserve_paused || mode == PREPARE_INSTALL) {
                cJSON *normalized = ts_auto_document_encode(view->definitions);
                if (!normalized) { free_view(view); return fail(error, "out_of_memory", ESP_ERR_NO_MEM); }
                cJSON_Delete(view->json);
                view->json = normalized;
            }
            ret = ts_rule_config_prepare(view->definitions->rules, view->definitions->rule_count, keep_runtime, &runtime->rules);
            if (ret == ESP_OK) {
                ret = ts_source_config_prepare(view->definitions, keep_runtime, &runtime->sources);
                if (ret == ESP_ERR_INVALID_SIZE) view->error.reason = "source_connection_limit";
            }
            if (ret == ESP_OK) {
                ret = ts_variable_config_prepare(view->definitions, runtime->sources, keep_runtime?s_config.current->definitions:NULL, &runtime->variables);
                if (ret != ESP_OK) view->error.reason = ret == ESP_ERR_INVALID_SIZE ? "variable_capacity_reached" :
                    ret == ESP_ERR_INVALID_ARG ? "variable_name_reserved" : NULL;
            }
            if (ret != ESP_OK) {
                if (!view->error.reason) view->error.reason = ret == ESP_ERR_NO_MEM ? "out_of_memory" : "runtime_unavailable";
                discard_runtime(runtime);
                if (mode != PREPARE_LOAD) {
                    if (error) *error = view->error;
                    free_view(view);
                    return ret;
                }
            }
        }
    }
    *out = view;
    return ESP_OK;
}

/* Caller owns the writer; only this short publication takes the SSH admission
 * gate. File I/O and all allocations have already finished. */
static void publish(ts_auto_config_view_t *view, prepared_runtime_t *runtime, ts_auto_config_source_t source)
{
    bool enabled = !s_config.control.paused && runtime->rules && runtime->sources && runtime->variables &&
                   view->definitions && view->definitions->enabled;
    bool clear_runtime=runtime->variables==NULL;
    if(clear_runtime)ts_variable_registration_begin();
    ts_ssh_binding_lock();
    xSemaphoreTake(s_config.state, portMAX_DELAY);
    ts_auto_config_view_t *old = s_config.current;
    if(!old || source!=s_config.source || !view->definitions){
        ++s_config.execution_epoch;
        ++s_config.binding_epoch;
    }
    ts_auto_document_t *next = view->definitions;
    const ts_auto_document_t *previous = old ? old->definitions : NULL;
    for(int i=0;next&&i<next->host_count;++i){
        for(int j=0;source==s_config.source&&previous&&j<previous->host_count;++j)
            if(!memcmp(&next->hosts[i],&previous->hosts[j],sizeof(next->hosts[i]))){view->host_instances[i]=old->host_instances[j];break;}
        if(!view->host_instances[i])view->host_instances[i]=++s_config.next_host_instance;
    }
    for (int i = 0; next && i < next->action_count; ++i) {
        for (int j = 0; source == s_config.source && previous && j < previous->action_count; ++j) {
            if (!memcmp(&next->actions[i], &previous->actions[j], offsetof(ts_action_template_t, last_used_at))) {
                view->template_instances[i] = old->template_instances[j];
                view->template_times[i] = old->template_times[j];
                view->template_counts[i] = old->template_counts[j];
                break;
            }
        }
        if (!view->template_instances[i]) view->template_instances[i] = ++s_config.next_template_instance;
    }
    for (int i = 0; next && i < next->command_count; ++i) {
        ts_ssh_command_config_t *command = &next->commands[i];
        uint32_t timestamp = 0;
        for (int j = 0; source == s_config.source && previous && j < previous->command_count; ++j) {
            /* Both structures came from zero-initialized definition decoders.
             * Runtime time/identity fields follow this definition prefix. */
            if (!memcmp(command, &previous->commands[j], offsetof(ts_ssh_command_config_t, last_exec_time))) {
                uint64_t before_host=0,after_host=0;
                for(int h=0;h<previous->host_count;++h)if(!strcmp(previous->hosts[h].id,command->host_id))before_host=old->host_instances[h];
                for(int h=0;h<next->host_count;++h)if(!strcmp(next->hosts[h].id,command->host_id))after_host=view->host_instances[h];
                if(!before_host||before_host!=after_host)continue;
                command->instance = previous->commands[j].instance;
                timestamp = atomic_load(&old->command_times[j]);
                break;
            }
        }
        if (!command->instance) command->instance = ++s_config.next_command_instance;
        atomic_init(&view->command_times[i], timestamp);
    }
    s_config.current = view;
    s_config.source = source;
    s_config.load_error=(ts_auto_config_error_t){0};
    if(source==TS_AUTO_CONFIG_SD)s_config.sd_available=true;
    else {
        s_config.internal=(internal_info_t){.exists=view->configured,.valid=view->configured && view->fingerprint[0]};
        if(view->configured)memcpy(s_config.internal.fingerprint,view->fingerprint,sizeof(view->fingerprint));
        const cJSON *saved_at=cJSON_GetObjectItemCaseSensitive(view->json,"saved_at");
        if(cJSON_IsNumber(saved_at))s_config.internal.saved_at=saved_at->valuedouble;
        if(!s_config.internal.valid)s_config.internal.error=view->error.reason;
    }
    s_config.sd_generation = source==TS_AUTO_CONFIG_SD ? ts_storage_sd_generation() : 0;
    s_config.paused = !enabled;
    s_config.uncertain = false;
    next_version();
    bool destroy = old && --old->refs == 0;
    xSemaphoreGive(s_config.state);
    if(clear_runtime)ts_variable_config_clear(previous);
    if (runtime->sources) ts_source_config_publish(runtime->sources);
    else ts_source_config_clear();
    if (runtime->variables) ts_variable_config_publish(runtime->variables);
    if (runtime->rules) ts_rule_config_publish(runtime->rules, enabled, previous, next);
    else ts_rule_config_clear();
    *runtime = (prepared_runtime_t){0};
    ts_ssh_binding_unlock();
    if(clear_runtime)ts_variable_registration_end();
    if (destroy) free_view(old);
    ts_source_config_activate();
}

static void pause_runtime(void)
{
    ts_ssh_binding_lock();
    ts_rule_config_pause();
    xSemaphoreTake(s_config.state, portMAX_DELAY);
    ++s_config.execution_epoch;
    if (!s_config.paused) {
        s_config.paused = true;
        next_version();
    }
    xSemaphoreGive(s_config.state);
    ts_ssh_binding_unlock();
}

/* Consumes the prepared views on every path. */
static esp_err_t save_prepared(ts_auto_config_view_t *view, prepared_runtime_t *runtime,
                                ts_auto_config_source_t source, bool force_pause, bool resume,
                                ts_auto_config_error_t *error)
{
    char *bytes = cJSON_PrintUnformatted(view->json);
    if(source==TS_AUTO_CONFIG_SD)ts_storage_sd_lock();
    bool changed=source==TS_AUTO_CONFIG_SD && s_config.current && s_config.source==source &&
                 s_config.sd_generation!=ts_storage_sd_generation();
    bool unavailable=source==TS_AUTO_CONFIG_SD ? !ts_storage_sd_mounted() : !ts_storage_auto_mounted();
    esp_err_t ret = !bytes ? ESP_ERR_NO_MEM : changed || unavailable ? ESP_ERR_INVALID_STATE :
        ts_auto_file_save(source == TS_AUTO_CONFIG_SD ? TS_AUTO_FILE_SD : TS_AUTO_FILE_INTERNAL,bytes);
    free(bytes);
    if (ret == ESP_OK) {
        view->configured=true;
        if(resume && s_config.control.paused){
            ts_auto_control_t control=s_config.control;control.paused=false;
            ret=save_control(control,error);
            if(ret!=ESP_OK && error){error->reason="saved_automation_paused";error->saved=true;}
        }
        /* A committed file remains the displayed configuration even if clearing
         * the independent pause failed. Publication still respects that pause. */
        publish(view,runtime,source);
    }
    else {
        discard_runtime(runtime);
        free_view(view);
        if(changed || unavailable)pause_runtime();
        if (ret == ESP_ERR_NOT_FINISHED) {
            xSemaphoreTake(s_config.state, portMAX_DELAY);
            s_config.uncertain = true;
            xSemaphoreGive(s_config.state);
        }
        fail(error, changed ? "sd_card_changed" : unavailable ? "storage_unavailable" :
                    ret == ESP_ERR_NO_MEM ? "out_of_memory" :
                    ret == ESP_ERR_NOT_FINISHED ? "save_outcome_unknown" :
                    ret == ESP_ERR_INVALID_SIZE ? "storage_full" :
                    force_pause ? "save_failed_automation_paused" : "storage_write_failed", ret);
        if(source==TS_AUTO_CONFIG_SD && (changed || unavailable || ret==ESP_FAIL || ret==ESP_ERR_TIMEOUT || ret==ESP_ERR_NOT_FINISHED)){
            ts_auto_config_error_t observed={0};refresh_mode_locked(!changed&&!unavailable,&observed);
        }
    }
    if(source==TS_AUTO_CONFIG_SD)ts_storage_sd_unlock();
    return ret;
}

static esp_err_t commit(cJSON *json, bool force_pause, bool resume, ts_auto_config_error_t *error)
{
    const cJSON *old_revision = cJSON_GetObjectItemCaseSensitive(json, "revision");
    bool revision_ok = true;
    if (cJSON_IsNumber(old_revision) && old_revision->valuedouble >= 0 &&
        old_revision->valuedouble < UINT32_MAX)
        revision_ok = set(json, "revision", cJSON_CreateNumber(old_revision->valuedouble + 1));
    time_t now = time(NULL);
    if (!revision_ok || !set(json, "saved_at", cJSON_CreateNumber(now > 0 ? now : 0))) {
        cJSON_Delete(json);
        return fail(error, "out_of_memory", ESP_ERR_NO_MEM);
    }
    if (s_config.paused && !force_pause && !resume &&
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "enabled"))) {
        /* An unrelated edit must not undo a previous explicit pause. */
        if (!set(json, "enabled", cJSON_CreateFalse()) ||
            !set(json, "pause_reason", cJSON_CreateString("user"))) {
            cJSON_Delete(json);
            return fail(error, "out_of_memory", ESP_ERR_NO_MEM);
        }
    }
    ts_auto_config_view_t *view = NULL;
    prepared_runtime_t runtime = {0};
    esp_err_t ret = prepare(json, force_pause ? PREPARE_FORCE : PREPARE_EDIT, s_config.source, &view, &runtime, error);
    if (ret != ESP_OK) return ret;
    return save_prepared(view, &runtime, s_config.source, force_pause, resume, error);
}

esp_err_t ts_auto_config_install(ts_auto_config_source_t source, const cJSON *json,
                                ts_auto_config_error_t *error)
{
    if ((unsigned)source > TS_AUTO_CONFIG_INTERNAL || !cJSON_IsObject(json)) return ESP_ERR_INVALID_ARG;
    initialize();
    xSemaphoreTakeRecursive(s_config.writer, portMAX_DELAY);
    if(source==TS_AUTO_CONFIG_SD)ts_storage_sd_lock();
    esp_err_t ret=load_control(error);
    if(ret==ESP_OK && s_config.closing)ret=fail(error,"configuration_unavailable",ESP_ERR_INVALID_STATE);
    cJSON *copy = ret==ESP_OK ? cJSON_Duplicate(json, true) : NULL;
    ts_auto_config_view_t *view = NULL;
    prepared_runtime_t runtime = {0};
    if(ret==ESP_OK)ret = copy ? prepare(copy, PREPARE_INSTALL, source, &view, &runtime, error) :
                              fail(error, "out_of_memory", ESP_ERR_NO_MEM);
    if (ret == ESP_OK) ret = save_prepared(view, &runtime, source, false, false, error);
    if(source==TS_AUTO_CONFIG_SD)ts_storage_sd_unlock();
    xSemaphoreGiveRecursive(s_config.writer);
    return ret;
}

esp_err_t ts_auto_config_open(ts_auto_config_source_t source, ts_auto_config_error_t *error)
{
    if ((unsigned)source > TS_AUTO_CONFIG_INTERNAL) return ESP_ERR_INVALID_ARG;
    if(error)*error=(ts_auto_config_error_t){0};
    initialize();
    xSemaphoreTakeRecursive(s_config.writer, portMAX_DELAY);
    if(source==TS_AUTO_CONFIG_SD)ts_storage_sd_lock();
    char *bytes = NULL;
    esp_err_t ret=load_control(error);
    if(ret==ESP_OK && s_config.closing)ret=fail(error,"configuration_unavailable",ESP_ERR_INVALID_STATE);
    if(ret==ESP_OK && !(source==TS_AUTO_CONFIG_SD ? ts_storage_sd_mounted() : ts_storage_auto_mounted()))
        ret=ESP_ERR_INVALID_STATE;
    if(ret==ESP_OK)ret = ts_auto_file_load(source == TS_AUTO_CONFIG_SD ? TS_AUTO_FILE_SD : TS_AUTO_FILE_INTERNAL, &bytes);
    bool configured=ret==ESP_OK;
    cJSON *json = NULL;
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_auto_document_t empty = {.enabled=true};
        json = ts_auto_document_encode(&empty);
        ret = json ? ESP_OK : ESP_ERR_NO_MEM;
    } else if (ret == ESP_OK) {
        json = cJSON_Parse(bytes);
        if (!json) ret = ESP_ERR_NO_MEM; /* File transport already verified the same JSON syntax. */
    }
    free(bytes);
    if (ret == ESP_OK) {
        ts_auto_config_view_t *view = NULL;
        prepared_runtime_t runtime = {0};
        ret = prepare(json, PREPARE_LOAD, source, &view, &runtime, error);
        if (ret == ESP_OK) {
            view->configured=configured;
            publish(view, &runtime, source);
        }
    }
    if(ret!=ESP_OK && (!error||!error->reason))fail(error,ret==ESP_ERR_NO_MEM?"out_of_memory":
        ret==ESP_ERR_INVALID_ARG||ret==ESP_ERR_INVALID_SIZE?"configuration_invalid":"configuration_unavailable",ret);
    if(source==TS_AUTO_CONFIG_SD)ts_storage_sd_unlock();
    xSemaphoreGiveRecursive(s_config.writer);
    return ret;
}

void ts_auto_config_close(void)
{
    if(!s_config.writer)return;
    xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);
    ts_variable_registration_begin();ts_ssh_binding_lock();
    xSemaphoreTake(s_config.state,portMAX_DELAY);
    ts_auto_config_view_t *old=s_config.current;
    s_config.current=NULL;s_config.paused=true;s_config.uncertain=false;
    ++s_config.execution_epoch;++s_config.binding_epoch;next_version();
    bool destroy=old&&--old->refs==0;
    xSemaphoreGive(s_config.state);
    ts_variable_config_clear(old?old->definitions:NULL);
    ts_source_config_clear();ts_rule_config_clear();
    ts_ssh_binding_unlock();ts_variable_registration_end();
    if(destroy)free_view(old);
    ts_source_config_activate();
    xSemaphoreGiveRecursive(s_config.writer);
}

void ts_auto_config_shutdown(void)
{
    if(!s_config.writer)return;
    xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);
    s_config.closing=true;ts_auto_config_close();
    xSemaphoreGiveRecursive(s_config.writer);
}

static cJSON *copy_current(void)
{
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    cJSON *json = view ? cJSON_Duplicate(view->json, true) : NULL;
    ts_auto_config_release(view);
    return json;
}

static esp_err_t set_item(ts_auto_config_kind_t kind, const cJSON *value, const cJSON *package,
                          const char *active_version, ts_auto_config_error_t *error)
{
    if (!s_config.writer || (unsigned)kind >= TS_AUTO_CONFIG_KIND_COUNT || !cJSON_IsObject(value))
        return ESP_ERR_INVALID_ARG;
    const char *id_key = kind == TS_AUTO_CONFIG_VARIABLE ? "name" : "id";
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(value, id_key);
    if (!cJSON_IsString(id) || !ts_rule_id_valid(id->valuestring)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTakeRecursive(s_config.writer, portMAX_DELAY);
    esp_err_t ret = check_version(active_version, error);
    if (ret == ESP_OK) ret = ts_auto_document_check_item(kind, value, error);
    cJSON *json = ret == ESP_OK ? copy_current() : NULL;
    if (ret == ESP_OK && !json) ret = fail(error, "out_of_memory", ESP_ERR_NO_MEM);
    if (ret == ESP_OK) {
        cJSON *array = cJSON_GetObjectItemCaseSensitive(json, ts_auto_config_collection(kind));
        if (!cJSON_IsArray(array)) ret = fail(error, "configuration_invalid", ESP_ERR_INVALID_ARG);
        int found = -1;
        for (int i = 0; ret == ESP_OK && i < cJSON_GetArraySize(array); ++i) {
            const cJSON *old = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(array, i), id_key);
            if (cJSON_IsString(old) && !strcmp(old->valuestring, id->valuestring)) { found = i; break; }
        }
        if (ret == ESP_OK && found < 0 && cJSON_GetArraySize(array) >= ts_auto_config_limit(kind))
            ret = fail(error, kind == TS_AUTO_CONFIG_RULE ? "too_many_rules" : "too_many_items", ESP_ERR_INVALID_SIZE);
        cJSON *next = ret == ESP_OK ? cJSON_Duplicate(value, true) : NULL;
        if (ret == ESP_OK && !next) ret = fail(error, "out_of_memory", ESP_ERR_NO_MEM);
        if (ret == ESP_OK && kind == TS_AUTO_CONFIG_RULE) {
            cJSON_DeleteItemFromObjectCaseSensitive(next, "_package");
            if (package && !set(next, "_package", cJSON_Duplicate(package, true))) {
                cJSON_Delete(next);cJSON_Delete(json);
                xSemaphoreGiveRecursive(s_config.writer);
                return fail(error, "out_of_memory", ESP_ERR_NO_MEM);
            }
            const cJSON *old = found < 0 ? NULL :
                cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(array, found), "revision");
            double revision = cJSON_IsNumber(old) ? old->valuedouble + 1 : 1;
            if (!set(next, "revision", cJSON_CreateNumber(revision))) {
                cJSON_Delete(next);
                ret = fail(error, "out_of_memory", ESP_ERR_NO_MEM);
            }
        }
        if (ret == ESP_OK) {
            bool changed = found < 0 ? cJSON_AddItemToArray(array, next) :
                                      cJSON_ReplaceItemInArray(array, found, next);
            if (!changed) { cJSON_Delete(next); ret = fail(error, "out_of_memory", ESP_ERR_NO_MEM); }
        }
        if (ret == ESP_OK) { ret = commit(json, false, false, error); json = NULL; }
    }
    cJSON_Delete(json);
    xSemaphoreGiveRecursive(s_config.writer);
    return ret;
}

esp_err_t ts_auto_config_set(ts_auto_config_kind_t kind, const cJSON *value,
                             const char *version, ts_auto_config_error_t *error)
{
    return set_item(kind, value, NULL, version, error);
}

esp_err_t ts_auto_config_set_rule_from_pack(const cJSON *value, const cJSON *package,
                                            const char *version, ts_auto_config_error_t *error)
{
    if (!cJSON_IsObject(package)) return ESP_ERR_INVALID_ARG;
    return set_item(TS_AUTO_CONFIG_RULE, value, package, version, error);
}

esp_err_t ts_auto_config_delete_preview(ts_auto_config_kind_t kind, const char *id,
                                        cJSON **impact, char version[33], ts_auto_config_error_t *error)
{
    if (!s_config.writer) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_config.writer, portMAX_DELAY);
    esp_err_t ret = check_version(NULL, error);
    const ts_auto_config_view_t *view = ret == ESP_OK ? ts_auto_config_acquire() : NULL;
    if (ret == ESP_OK) {
        ret = ts_auto_document_impact(view->json, kind, id, impact, error);
        if (version) memcpy(version, s_config.version, sizeof(s_config.version));
    }
    ts_auto_config_release(view);
    xSemaphoreGiveRecursive(s_config.writer);
    return ret;
}

esp_err_t ts_auto_config_replace_collection(ts_auto_config_kind_t kind, const cJSON *array,
                                            const char *active_version, ts_auto_config_error_t *error)
{
    if (!s_config.writer || (unsigned)kind >= TS_AUTO_CONFIG_KIND_COUNT || !cJSON_IsArray(array))
        return ESP_ERR_INVALID_ARG;
    if (cJSON_GetArraySize(array) > ts_auto_config_limit(kind))
        return fail(error, kind == TS_AUTO_CONFIG_RULE ? "too_many_rules" : "too_many_items", ESP_ERR_INVALID_SIZE);
    xSemaphoreTakeRecursive(s_config.writer, portMAX_DELAY);
    esp_err_t ret = check_version(active_version, error);
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (ret != ESP_OK) break;
        ret = ts_auto_document_check_item(kind, item, error);
    }
    cJSON *json = ret == ESP_OK ? copy_current() : NULL;
    if (ret == ESP_OK && (!json || !set(json, ts_auto_config_collection(kind), cJSON_Duplicate(array, true))))
        ret = fail(error, "out_of_memory", ESP_ERR_NO_MEM);
    if (ret == ESP_OK) { ret = commit(json, false, false, error); json = NULL; }
    cJSON_Delete(json);
    xSemaphoreGiveRecursive(s_config.writer);
    return ret;
}

esp_err_t ts_auto_config_import(cJSON *document,const char *active_version,ts_auto_config_error_t *error)
{
    if(!s_config.writer||!cJSON_IsObject(document)){cJSON_Delete(document);return ESP_ERR_INVALID_ARG;}
    xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);
    esp_err_t ret=check_version(active_version,error);
    if(ret==ESP_OK){ret=commit(document,false,false,error);document=NULL;}
    cJSON_Delete(document);xSemaphoreGiveRecursive(s_config.writer);return ret;
}

esp_err_t ts_auto_config_delete(ts_auto_config_kind_t kind, const char *id,
                                ts_auto_delete_mode_t mode, const char *replacement,
                                bool pause_confirmed, const char *active_version,
                                cJSON **impact, ts_auto_config_error_t *error)
{
    if (!s_config.writer) return ESP_ERR_INVALID_STATE;
    if (impact) *impact = NULL;
    xSemaphoreTakeRecursive(s_config.writer, portMAX_DELAY);
    esp_err_t ret = check_version(active_version, error);
    cJSON *json = ret == ESP_OK ? copy_current() : NULL;
    if (ret == ESP_OK && !json) ret = fail(error, "out_of_memory", ESP_ERR_NO_MEM);
    if (ret == ESP_OK)
        ret = ts_auto_document_remove(json, kind, id, mode, replacement, pause_confirmed, impact, error);
    if (ret == ESP_OK) {
        bool force_pause = mode == TS_AUTO_DELETE_FORCE && pause_confirmed;
        if (force_pause) {
            pause_runtime();
            ts_auto_control_t control=s_config.control;control.paused=true;
            ret=save_control(control,error);
        }
        if(ret==ESP_OK){ret = commit(json, force_pause, false, error);json = NULL;}
        if (ret != ESP_OK && mode == TS_AUTO_DELETE_FORCE && !pause_confirmed && error && error->reason) {
            const char *reason = error->reason;
            if (!strcmp(reason, "configuration_invalid") || !strcmp(reason, "duplicate_id") ||
                !strcmp(reason, "dependency_missing") || !strcmp(reason, "dependency_disabled") ||
                !strcmp(reason, "too_many_rules") || !strcmp(reason, "too_many_items") ||
                !strcmp(reason, "out_of_memory") || !strcmp(reason, "runtime_unavailable") ||
                !strcmp(reason, "variable_capacity_reached") || !strcmp(reason, "variable_name_reserved") ||
                !strcmp(reason, "source_connection_limit")) {
                if (impact && *impact)
                    cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(*impact, "requires_pause"), true);
                ret = fail(error, "pause_required", ESP_ERR_INVALID_STATE);
            }
        }
    }
    cJSON_Delete(json);
    xSemaphoreGiveRecursive(s_config.writer);
    return ret;
}

esp_err_t ts_auto_config_enable(bool enabled, const char *active_version, ts_auto_config_error_t *error)
{
    if (!s_config.writer) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_config.writer, portMAX_DELAY);
    esp_err_t ret = check_version(active_version, error);
    cJSON *json = ret == ESP_OK ? copy_current() : NULL;
    if (ret == ESP_OK && !json) ret = fail(error, "out_of_memory", ESP_ERR_NO_MEM);
    if (ret == ESP_OK &&
        (!set(json, "enabled", cJSON_CreateBool(enabled)) ||
         !set(json, "pause_reason", cJSON_CreateString(enabled ? "" : "user"))))
        ret = fail(error, "out_of_memory", ESP_ERR_NO_MEM);
    if (ret == ESP_OK) {
        if (!enabled) {
            pause_runtime();
            ts_auto_control_t control=s_config.control;control.paused=true;
            ret=save_control(control,error);
        }
        if(ret==ESP_OK){ret = commit(json, false, enabled, error);json = NULL;}
    }
    cJSON_Delete(json);
    xSemaphoreGiveRecursive(s_config.writer);
    return ret;
}

/* Source operations own the same writer and SD gate as ordinary edits. */
static void refresh_internal_info(void)
{
    internal_info_t info={0};char *bytes=NULL;
    esp_err_t ret=ts_storage_auto_mounted()?ts_auto_file_load(TS_AUTO_FILE_INTERNAL,&bytes):ESP_ERR_INVALID_STATE;
    if(ret!=ESP_ERR_NOT_FOUND){
        info.exists=ret==ESP_OK || ret==ESP_ERR_INVALID_ARG;
        info.error=ret==ESP_OK?NULL:ret==ESP_ERR_NO_MEM?"out_of_memory":"internal_configuration_unavailable";
    }
    if(ret==ESP_OK){
        cJSON *json=cJSON_Parse(bytes);ts_auto_document_t *document=NULL;ts_auto_config_error_t error={0};
        ret=json?ts_auto_document_decode(json,&document,&error):ESP_ERR_NO_MEM;
        if(ret==ESP_OK && document->enabled)ret=ts_auto_document_validate(document,&error);
        if(ret==ESP_OK){
            info.valid=true;info.saved_at=document->saved_at;
            if(!definition_fingerprint(document,info.fingerprint))ret=ESP_ERR_NO_MEM;
        }
        if(ret!=ESP_OK)info.error=ret==ESP_ERR_NO_MEM?"out_of_memory":error.reason?error.reason:"configuration_invalid";
        ts_auto_document_free(document);cJSON_Delete(json);
    }
    free(bytes);
    xSemaphoreTake(s_config.state,portMAX_DELAY);s_config.internal=info;xSemaphoreGive(s_config.state);
}

static void unload_source(ts_auto_config_source_t source,const ts_auto_config_error_t *error)
{
    ts_auto_config_close();
    xSemaphoreTake(s_config.state,portMAX_DELAY);
    s_config.source=source;
    s_config.load_error=error?*error:(ts_auto_config_error_t){.reason="configuration_unavailable"};
    xSemaphoreGive(s_config.state);
}

static esp_err_t use_internal(ts_auto_config_error_t *error)
{
    pause_runtime();
    ts_auto_control_t control=s_config.control;control.source=TS_AUTO_CONFIG_INTERNAL;
    esp_err_t ret=control.source==s_config.control.source?ESP_OK:save_control(control,error);
    if(ret==ESP_OK){
        ret=ts_auto_config_open(TS_AUTO_CONFIG_INTERNAL,error);
        if(ret!=ESP_OK)unload_source(TS_AUTO_CONFIG_INTERNAL,error);
    }
    return ret;
}

/* A missing/unmounted card is normal while already using internal storage. */
static esp_err_t refresh_mode_locked(bool probe,ts_auto_config_error_t *error)
{
    if(s_config.closing)return fail(error,"configuration_unavailable",ESP_ERR_INVALID_STATE);
    bool available=probe?ts_storage_sd_refresh()==ESP_OK:ts_storage_sd_mounted();
    xSemaphoreTake(s_config.state,portMAX_DELAY);s_config.sd_available=available;xSemaphoreGive(s_config.state);
    esp_err_t ret=ESP_OK;
    if(s_config.source==TS_AUTO_CONFIG_SD && (!available ||
        (s_config.current && s_config.sd_generation!=ts_storage_sd_generation())))ret=use_internal(error);
    refresh_internal_info();return ret;
}

esp_err_t ts_auto_config_startup(ts_auto_config_error_t *error)
{
    initialize();xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);ts_storage_sd_lock();
    s_config.closing=false;
    esp_err_t ret=load_control(error);bool installed=false;
    bool sd=ts_storage_sd_mounted();
    xSemaphoreTake(s_config.state,portMAX_DELAY);s_config.sd_available=sd;xSemaphoreGive(s_config.state);
    if(ret==ESP_OK)ret=ts_auto_config_migrate(sd,&installed,error);
    if(ret==ESP_OK){
        ts_auto_config_source_t source=!sd||s_config.control.source==TS_AUTO_CONFIG_INTERNAL?TS_AUTO_CONFIG_INTERNAL:TS_AUTO_CONFIG_SD;
        if(source==TS_AUTO_CONFIG_INTERNAL)ret=use_internal(error);
        else if(!installed || s_config.source!=source)ret=ts_auto_config_open(source,error);
        /* Missing files are an empty configuration. Invalid JSON is damaged
         * content, not an unavailable card and not an invitation to restore. */
        if(source==TS_AUTO_CONFIG_SD && (ret==ESP_FAIL || ret==ESP_ERR_TIMEOUT || ret==ESP_ERR_NOT_FINISHED)){
            source=TS_AUTO_CONFIG_INTERNAL;ret=use_internal(error);
        }
        if(ret!=ESP_OK && s_config.current==NULL)unload_source(source,error);
    }
    if(ret!=ESP_OK){
        pause_runtime();
        xSemaphoreTake(s_config.state,portMAX_DELAY);
        s_config.load_error=error?*error:(ts_auto_config_error_t){.reason="configuration_unavailable"};
        xSemaphoreGive(s_config.state);
    }
    refresh_internal_info();ts_storage_sd_unlock();xSemaphoreGiveRecursive(s_config.writer);return ret;
}

esp_err_t ts_auto_config_refresh(ts_auto_config_error_t *error)
{
    if(!s_config.writer)return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);ts_storage_sd_lock();
    esp_err_t ret=refresh_mode_locked(true,error);
    ts_storage_sd_unlock();xSemaphoreGiveRecursive(s_config.writer);return ret;
}

esp_err_t ts_auto_config_storage_changed(ts_auto_config_error_t *error)
{
    if(!s_config.writer)return ESP_OK;
    xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);ts_storage_sd_lock();
    esp_err_t ret=refresh_mode_locked(false,error);
    ts_storage_sd_unlock();xSemaphoreGiveRecursive(s_config.writer);return ret;
}

esp_err_t ts_auto_config_reload(ts_auto_config_error_t *error)
{
    if(!s_config.writer)return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);ts_storage_sd_lock();
    ts_auto_config_source_t source=s_config.source;
    esp_err_t ret=refresh_mode_locked(true,error);
    if(ret==ESP_OK && source==s_config.source)ret=ts_auto_config_open(source,error);
    ts_storage_sd_unlock();xSemaphoreGiveRecursive(s_config.writer);return ret;
}

static esp_err_t mode_version(const char *version,ts_auto_config_error_t *error)
{
    if(s_config.closing)return fail(error,"configuration_unavailable",ESP_ERR_INVALID_STATE);
    if(!version || !version[0])return fail(error,"configuration_version_required",ESP_ERR_INVALID_ARG);
    return strcmp(version,s_config.version)?fail(error,"configuration_changed",ESP_ERR_INVALID_STATE):ESP_OK;
}

esp_err_t ts_auto_config_backup(const char *version,ts_auto_config_error_t *error)
{
    if(!s_config.writer)return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);ts_storage_sd_lock();
    esp_err_t ret=refresh_mode_locked(true,error);
    if(ret==ESP_OK)ret=mode_version(version,error);
    if(ret==ESP_OK)ret=check_version(version,error);
    if(ret==ESP_OK && s_config.source!=TS_AUTO_CONFIG_SD)ret=fail(error,"backup_requires_sd_mode",ESP_ERR_INVALID_STATE);
    if(ret==ESP_OK && !ts_storage_auto_mounted())ret=fail(error,"storage_unavailable",ESP_ERR_INVALID_STATE);
    cJSON *json=ret==ESP_OK?copy_current():NULL;
    if(ret==ESP_OK && !json)ret=fail(error,"out_of_memory",ESP_ERR_NO_MEM);
    time_t now=time(NULL);
    if(ret==ESP_OK && !set(json,"saved_at",cJSON_CreateNumber(now>0?now:0)))ret=fail(error,"out_of_memory",ESP_ERR_NO_MEM);
    char *bytes=ret==ESP_OK?cJSON_PrintUnformatted(json):NULL;
    if(ret==ESP_OK && !bytes)ret=fail(error,"out_of_memory",ESP_ERR_NO_MEM);
    if(ret==ESP_OK){
        ret=ts_auto_file_save(TS_AUTO_FILE_INTERNAL,bytes);
        if(ret==ESP_OK){
            internal_info_t info={.exists=true,.valid=s_config.current->fingerprint[0],.saved_at=now>0?now:0};
            memcpy(info.fingerprint,s_config.current->fingerprint,sizeof(info.fingerprint));
            if(!info.valid)info.error=s_config.current->error.reason;
            xSemaphoreTake(s_config.state,portMAX_DELAY);s_config.internal=info;xSemaphoreGive(s_config.state);
        }else {
            if(ret==ESP_ERR_NOT_FINISHED){
                xSemaphoreTake(s_config.state,portMAX_DELAY);
                s_config.internal.valid=false;s_config.internal.fingerprint[0]=0;s_config.internal.error="save_outcome_unknown";
                xSemaphoreGive(s_config.state);
            }
            fail(error,ret==ESP_ERR_INVALID_SIZE?"storage_full":ret==ESP_ERR_NOT_FINISHED?"save_outcome_unknown":"storage_write_failed",ret);
        }
    }
    free(bytes);cJSON_Delete(json);ts_storage_sd_unlock();xSemaphoreGiveRecursive(s_config.writer);return ret;
}

typedef struct {
    cJSON *target,*before;
    bool configured,before_known;
    char version[147];
} mode_input_t;
static void free_mode_input(mode_input_t *input){cJSON_Delete(input->target);cJSON_Delete(input->before);}

static esp_err_t document_at(ts_auto_file_location_t location,bool empty_allowed,cJSON **json,bool *configured)
{
    *json=NULL;*configured=false;char *bytes=NULL;
    esp_err_t ret=ts_auto_file_load(location,&bytes);
    if(ret==ESP_ERR_NOT_FOUND && empty_allowed){ts_auto_document_t empty={.enabled=true};*json=ts_auto_document_encode(&empty);ret=*json?ESP_OK:ESP_ERR_NO_MEM;}
    else if(ret==ESP_OK){*json=cJSON_Parse(bytes);*configured=true;if(!*json)ret=ESP_ERR_NO_MEM;}
    free(bytes);return ret;
}

static esp_err_t mode_input(ts_auto_config_operation_t operation,bool include_diff,mode_input_t *input,ts_auto_config_error_t *error)
{
    *input=(mode_input_t){0};
    if((unsigned)operation>TS_AUTO_RESTORE_INTERNAL)return ESP_ERR_INVALID_ARG;
    if(!s_config.sd_available)return fail(error,"sd_card_unavailable",ESP_ERR_INVALID_STATE);
    if(operation!=TS_AUTO_RESTORE_INTERNAL && s_config.source!=TS_AUTO_CONFIG_INTERNAL)
        return fail(error,"source_selection_not_needed",ESP_ERR_INVALID_STATE);
    if(operation==TS_AUTO_RESTORE_INTERNAL && s_config.source!=TS_AUTO_CONFIG_SD)
        return fail(error,"restore_requires_sd_mode",ESP_ERR_INVALID_STATE);
    char sd[65],internal[65]="-";
    esp_err_t ret=ts_auto_file_fingerprint(TS_AUTO_FILE_SD,sd);
    if(ret==ESP_ERR_NOT_FOUND){strcpy(sd,"missing");ret=ESP_OK;}
    if(ret==ESP_OK && operation==TS_AUTO_RESTORE_INTERNAL){
        ret=ts_storage_auto_mounted()?ts_auto_file_fingerprint(TS_AUTO_FILE_INTERNAL,internal):ESP_ERR_INVALID_STATE;
        if(ret==ESP_ERR_NOT_FOUND)return fail(error,"emergency_backup_missing",ret);
    }
    if(ret!=ESP_OK)return fail(error,ret==ESP_ERR_NO_MEM?"out_of_memory":"configuration_unavailable",ret);
    snprintf(input->version,sizeof(input->version),"%016" PRIx64 ":%s:%s",ts_storage_sd_generation(),sd,internal);
    if(operation==TS_AUTO_SAVE_TO_SD){
        if(s_config.uncertain)return fail(error,"save_outcome_unknown",ESP_ERR_NOT_FINISHED);
        input->target=copy_current();input->configured=true;
        if(!input->target)return fail(error,s_config.current?"out_of_memory":"configuration_unavailable",s_config.current?ESP_ERR_NO_MEM:ESP_ERR_INVALID_STATE);
        if(include_diff){
            bool configured;
            ret=document_at(TS_AUTO_FILE_SD,true,&input->before,&configured);
            input->before_known=ret==ESP_OK;
        }
        /* An explicit overwrite may replace malformed content; the raw digest
         * above still detects changes after the user sees the preview. */
        if(ret==ESP_ERR_INVALID_ARG)ret=ESP_OK;
    }else{
        ret=document_at(operation==TS_AUTO_USE_SD?TS_AUTO_FILE_SD:TS_AUTO_FILE_INTERNAL,
                        operation==TS_AUTO_USE_SD,&input->target,&input->configured);
        if(include_diff){
            input->before=copy_current();input->before_known=s_config.current!=NULL;
            if(s_config.current && !input->before && ret==ESP_OK)ret=ESP_ERR_NO_MEM;
        }
    }
    return ret==ESP_OK?ret:fail(error,ret==ESP_ERR_NO_MEM?"out_of_memory":"configuration_unavailable",ret);
}

static const cJSON *row_id(const cJSON *array,const char *key,const char *id)
{
    const cJSON *row;
    cJSON_ArrayForEach(row,array){const cJSON *value=cJSON_GetObjectItemCaseSensitive(row,key);if(cJSON_IsString(value)&&!strcmp(value->valuestring,id))return row;}
    return NULL;
}
static bool diff_item(cJSON *array,const char *collection,const cJSON *row,const char *id)
{
    const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row,"name"));
    if(!name)name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row,"label"));
    cJSON *item=cJSON_CreateObject();
    bool ok=item&&cJSON_AddStringToObject(item,"collection",collection)&&cJSON_AddStringToObject(item,"id",id)&&
        cJSON_AddStringToObject(item,"name",name?name:id)&&cJSON_AddItemToArray(array,item);
    if(!ok)cJSON_Delete(item);
    return ok;
}
static cJSON *mode_diff(const mode_input_t *input)
{
    cJSON *diff=cJSON_CreateObject(),*added=diff?cJSON_AddArrayToObject(diff,"added"):NULL,
          *changed=diff?cJSON_AddArrayToObject(diff,"changed"):NULL,*removed=diff?cJSON_AddArrayToObject(diff,"removed"):NULL;
    if(!diff||!added||!changed||!removed)goto failed;
    bool complete=input->before_known;
    for(unsigned kind=0;kind<TS_AUTO_CONFIG_KIND_COUNT;++kind){
        const char *collection=ts_auto_config_collection(kind),*key=kind==TS_AUTO_CONFIG_VARIABLE?"name":"id";
        const cJSON *before=cJSON_GetObjectItemCaseSensitive(input->before,collection),*after=cJSON_GetObjectItemCaseSensitive(input->target,collection),*row;
        if(!cJSON_IsArray(before)||!cJSON_IsArray(after)){complete=false;continue;}
        cJSON_ArrayForEach(row,after){
            const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row,key));
            if(!id){complete=false;continue;}
            const cJSON *old=row_id(before,key,id);
            if(!old){if(!diff_item(added,collection,row,id))goto failed;continue;}
            cJSON *a=cJSON_Duplicate(old,true),*b=cJSON_Duplicate(row,true);
            if(!a||!b){cJSON_Delete(a);cJSON_Delete(b);goto failed;}
            strip_row_metadata(a);strip_row_metadata(b);bool equal=cJSON_Compare(a,b,true);cJSON_Delete(a);cJSON_Delete(b);
            if(!equal&&!diff_item(changed,collection,row,id))goto failed;
        }
        cJSON_ArrayForEach(row,before){
            const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row,key));
            if(!id){complete=false;continue;}
            if(!row_id(after,key,id)&&!diff_item(removed,collection,row,id))goto failed;
        }
    }
    const char *before_pause=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input->before,"pause_reason")),
               *after_pause=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input->target,"pause_reason"));
    if(!cJSON_AddBoolToObject(diff,"complete",complete)||
       !cJSON_AddBoolToObject(diff,"settings_changed",
            !cJSON_Compare(cJSON_GetObjectItemCaseSensitive(input->before,"enabled"),cJSON_GetObjectItemCaseSensitive(input->target,"enabled"),true)||
            strcmp(before_pause?before_pause:"",after_pause?after_pause:"")))goto failed;
    return diff;
failed:cJSON_Delete(diff);return NULL;
}

esp_err_t ts_auto_config_preview(ts_auto_config_operation_t operation,cJSON **out,ts_auto_config_error_t *error)
{
    if(!out||!s_config.writer)return ESP_ERR_INVALID_ARG;
    *out=NULL;xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);ts_storage_sd_lock();
    esp_err_t ret=refresh_mode_locked(true,error);mode_input_t input={0};
    if(ret==ESP_OK)ret=mode_input(operation,true,&input,error);
    if(ret==ESP_OK){
        ts_auto_document_t *document=NULL;ts_auto_config_error_t validation={0};
        esp_err_t checked=ts_auto_document_decode(input.target,&document,&validation);
        if(checked==ESP_OK && document->enabled)checked=ts_auto_document_validate(document,&validation);
        bool enabled=document&&document->enabled;
        ts_auto_document_free(document);
        if(checked==ESP_ERR_NO_MEM){ret=fail(error,"out_of_memory",checked);goto done;}
        cJSON *reply=cJSON_CreateObject(),*diff=mode_diff(&input);
        bool ok=reply&&diff&&cJSON_AddStringToObject(reply,"active_version",s_config.version)&&
            cJSON_AddStringToObject(reply,"target_version",input.version)&&
            cJSON_AddBoolToObject(reply,"configuration_valid",checked==ESP_OK)&&
            cJSON_AddBoolToObject(reply,"will_remain_paused",s_config.control.paused||checked!=ESP_OK||!enabled)&&
            cJSON_AddStringToObject(reply,"configuration_error",checked==ESP_OK?"":validation.reason?validation.reason:"configuration_invalid")&&
            cJSON_AddItemToObject(reply,"diff",diff);
        if(ok)*out=reply;
        else {cJSON_Delete(diff);cJSON_Delete(reply);ret=fail(error,"out_of_memory",ESP_ERR_NO_MEM);}
    }
done:
    free_mode_input(&input);ts_storage_sd_unlock();xSemaphoreGiveRecursive(s_config.writer);return ret;
}

esp_err_t ts_auto_config_apply(ts_auto_config_operation_t operation,const char *version,const char *target_version,ts_auto_config_error_t *error)
{
    if(!s_config.writer)return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_config.writer,portMAX_DELAY);ts_storage_sd_lock();
    esp_err_t ret=refresh_mode_locked(true,error);
    if(ret==ESP_OK)ret=mode_version(version,error);
    mode_input_t input={0};prepared_runtime_t runtime={0};ts_auto_config_view_t *view=NULL;
    if(ret==ESP_OK)ret=mode_input(operation,false,&input,error);
    if(ret==ESP_OK && (!target_version||strcmp(target_version,input.version)))ret=fail(error,"configuration_target_changed",ESP_ERR_INVALID_STATE);
    if(ret==ESP_OK && operation!=TS_AUTO_USE_SD){
        time_t now=time(NULL);
        if(!set(input.target,"saved_at",cJSON_CreateNumber(now>0?now:0)))ret=fail(error,"out_of_memory",ESP_ERR_NO_MEM);
    }
    if(ret==ESP_OK){
        cJSON *json=input.target;input.target=NULL;
        ret=prepare(json,operation==TS_AUTO_USE_SD?PREPARE_LOAD:PREPARE_INSTALL,TS_AUTO_CONFIG_SD,&view,&runtime,error);
        if(ret==ESP_OK)view->configured=operation!=TS_AUTO_USE_SD||input.configured;
    }
    bool saved=false;
    if(ret==ESP_OK && operation!=TS_AUTO_USE_SD){
        char *bytes=cJSON_PrintUnformatted(view->json);
        ret=bytes?ts_auto_file_save(TS_AUTO_FILE_SD,bytes):ESP_ERR_NO_MEM;free(bytes);
        saved=ret==ESP_OK;
        if(ret!=ESP_OK){
            if(operation==TS_AUTO_RESTORE_INTERNAL && ret==ESP_ERR_NOT_FINISHED){xSemaphoreTake(s_config.state,portMAX_DELAY);s_config.uncertain=true;xSemaphoreGive(s_config.state);}
            fail(error,ret==ESP_ERR_NO_MEM?"out_of_memory":ret==ESP_ERR_INVALID_SIZE?"storage_full":ret==ESP_ERR_NOT_FINISHED?"save_outcome_unknown":"storage_write_failed",ret);
        }
    }
    if(ret==ESP_OK && operation!=TS_AUTO_RESTORE_INTERNAL){
        ts_auto_control_t control=s_config.control;control.source=TS_AUTO_CONFIG_SD;
        ret=save_control(control,error);
        if(ret!=ESP_OK && saved && error){error->reason="saved_to_sd_not_switched";error->saved=true;}
    }
    if(ret==ESP_OK){publish(view,&runtime,TS_AUTO_CONFIG_SD);view=NULL;}
    discard_runtime(&runtime);free_view(view);free_mode_input(&input);
    if(operation!=TS_AUTO_USE_SD && !saved && (ret==ESP_FAIL || ret==ESP_ERR_TIMEOUT || ret==ESP_ERR_NOT_FINISHED)){
        ts_auto_config_error_t observed={0};refresh_mode_locked(true,&observed);
    }
    ts_storage_sd_unlock();xSemaphoreGiveRecursive(s_config.writer);return ret;
}
