#include "ts_action_manager.h"
#include "ts_action_codec.h"
#include "ts_automation_config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

esp_err_t ts_action_templates_load_state(void)
{
    if (!ts_action_manager_is_initialized()) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    bool ready = ts_auto_config_definitions(view) != NULL;
    ts_auto_config_release(view);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool ts_action_templates_ready(void) { return ts_action_templates_load_state() == ESP_OK; }

esp_err_t ts_action_template_get(const char *id, ts_action_template_t *out)
{
    if (!id || !out) return ESP_ERR_INVALID_ARG;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    esp_err_t ret = d ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    for (int i = 0; d && i < d->action_count; ++i) {
        if (strcmp(d->actions[i].id, id)) continue;
        *out = d->actions[i];
        ts_auto_config_template_stats(view, i, out);
        ret = ESP_OK;
        break;
    }
    ts_auto_config_release(view);
    return ret;
}

int ts_action_template_count(void)
{
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    int count = d ? d->action_count : 0;
    ts_auto_config_release(view);
    return count;
}

esp_err_t ts_action_template_list(ts_action_template_t *out, size_t capacity, size_t *count)
{
    if (!out || !count) return ESP_ERR_INVALID_ARG;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    bool ready = d != NULL;
    *count = d && capacity > (size_t)d->action_count ? d->action_count : d ? capacity : 0;
    for (size_t i = 0; i < *count; ++i) {
        out[i] = d->actions[i];
        ts_auto_config_template_stats(view, i, &out[i]);
    }
    ts_auto_config_release(view);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t write_template(const ts_action_template_t *value, bool updating)
{
    if (!value || !value->id[0]) return ESP_ERR_INVALID_ARG;
    ts_auto_config_status_t status;
    ts_auto_config_status(&status);
    esp_err_t ret = ts_auto_config_edit_begin(status.active_version, NULL);
    if (ret != ESP_OK) return ret;
    ts_action_template_t *next = heap_caps_malloc(sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) { ts_auto_config_edit_end(); return ESP_ERR_NO_MEM; }
    *next = *value;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    bool exists = false;
    for (int i = 0; d && i < d->action_count; ++i) {
        if (strcmp(d->actions[i].id, value->id)) continue;
        exists = true;
        next->created_at = d->actions[i].created_at;
        break;
    }
    ts_auto_config_release(view);
    if (exists != updating) ret = updating ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    if (!exists) { time_t now = time(NULL); next->created_at = now > 0 ? (int64_t)now * 1000 : 0; }
    char *raw = ret == ESP_OK ? ts_action_template_encode(next) : NULL;
    cJSON *json = raw ? cJSON_Parse(raw) : NULL;
    if (ret == ESP_OK) ret = json ? ts_auto_config_set(TS_AUTO_CONFIG_ACTION, json, NULL, NULL) : ESP_ERR_NO_MEM;
    cJSON_Delete(json);
    free(raw); free(next);
    ts_auto_config_edit_end();
    return ret;
}

esp_err_t ts_action_template_add(const ts_action_template_t *value) { return write_template(value, false); }
esp_err_t ts_action_template_update(const char *id, const ts_action_template_t *value)
{
    return !id || !value || strcmp(id, value->id) ? ESP_ERR_INVALID_ARG : write_template(value, true);
}

esp_err_t ts_action_template_remove(const char *id)
{
    return ts_auto_config_delete(TS_AUTO_CONFIG_ACTION, id, TS_AUTO_DELETE_NORMAL, NULL, false, NULL, NULL, NULL);
}

esp_err_t ts_action_template_execute_versioned(const char *id,const char *active_version,
                                              ts_action_result_t *result,const char **reason)
{
    if(reason)*reason=NULL;
    if(result)memset(result,0,sizeof(*result));
    if(!id)return ESP_ERR_INVALID_ARG;
    ts_auto_action_t source={0},frozen={0};
    ts_ssh_binding_lock();
    ts_auto_config_status_t status;
    const ts_auto_config_view_t *view=ts_auto_config_acquire_status(&status);
    const ts_auto_document_t *definitions=ts_auto_config_definitions(view);
    esp_err_t ret=definitions?ESP_ERR_NOT_FOUND:ESP_ERR_INVALID_STATE;
    if(active_version&&strcmp(active_version,status.active_version)){
        if(reason)*reason="configuration_changed";
        ret=ESP_ERR_INVALID_STATE;
    }else for(int i=0;definitions&&i<definitions->action_count;++i){
        const ts_action_template_t *value=&definitions->actions[i];
        if(strcmp(value->id,id))continue;
        source=value->action;source.async|=value->async;strcpy(source.template_id,id);
        ret=value->enabled?ts_action_snapshot(&source,&frozen):ESP_ERR_INVALID_STATE;
        break;
    }
    ts_auto_config_release(view);ts_ssh_binding_unlock();
    bool prepared=ret==ESP_OK;
    if(prepared){
        if(frozen.async){
            ret=ts_action_queue(&frozen,NULL,NULL,0);
            if(result){memset(result,0,sizeof(*result));result->status=ret==ESP_OK?TS_ACTION_STATUS_QUEUED:TS_ACTION_STATUS_FAILED;}
        }else ret=ts_action_manager_execute(&frozen,result);
        if(ret==ESP_OK)ts_auto_config_template_used_instance(frozen.runtime_template_instance,esp_timer_get_time()/1000);
        ts_action_snapshot_release(&frozen);
    }
    if(ret!=ESP_OK&&result){
        if(result->status!=TS_ACTION_STATUS_TIMEOUT&&result->status!=TS_ACTION_STATUS_CANCELLED)result->status=TS_ACTION_STATUS_FAILED;
        if(!prepared)snprintf(result->output,sizeof(result->output),"Action unavailable: %s",id);
    }
    return ret;
}

esp_err_t ts_action_template_execute(const char *id,ts_action_result_t *result)
{
    return ts_action_template_execute_versioned(id,NULL,result,NULL);
}
