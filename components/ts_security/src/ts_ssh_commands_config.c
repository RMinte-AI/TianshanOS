/* SSH command definitions are a view of the single active configuration. */
#include "ts_ssh_commands_config.h"
#include "ts_ssh_config_codec.h"
#include "ts_automation_config.h"
#include "ts_variable.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool initialized;

esp_err_t ts_ssh_commands_config_init(void) { initialized = true; return ESP_OK; }
void ts_ssh_commands_config_deinit(void) { initialized = false; }
bool ts_ssh_commands_config_is_initialized(void) { return initialized; }

esp_err_t ts_ssh_commands_config_load_state(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    bool ready = ts_auto_config_definitions(view) != NULL;
    ts_auto_config_release(view);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ts_ssh_commands_config_get(const char *id, ts_ssh_command_config_t *out)
{
    if (!id || !out) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    esp_err_t ret = d ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    for (int i = 0; d && i < d->command_count; ++i) {
        if (strcmp(d->commands[i].id, id)) continue;
        *out = d->commands[i];
        out->last_exec_time = ts_auto_config_command_time(view, i);
        ret = ESP_OK;
        break;
    }
    ts_auto_config_release(view);
    return ret;
}

esp_err_t ts_ssh_commands_config_add(const ts_ssh_command_config_t *config, char *out, size_t size)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    ts_auto_config_status_t status;
    ts_auto_config_status(&status);
    esp_err_t ret = ts_auto_config_edit_begin(status.active_version, NULL);
    if (ret != ESP_OK) return ret;
    ts_ssh_command_config_t *next = heap_caps_malloc(sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) { ts_auto_config_edit_end(); return ESP_ERR_NO_MEM; }
    *next = *config;
    if (!next->id[0]) {
        uint32_t random[2]; esp_fill_random(random, sizeof(random));
        snprintf(next->id, sizeof(next->id), "cmd_%08" PRIx32 "%08" PRIx32, random[0], random[1]);
    }
    if (out && size <= strlen(next->id)) ret = ESP_ERR_INVALID_SIZE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    bool existing = false;
    for (int i = 0; d && i < d->command_count; ++i) {
        if (strcmp(d->commands[i].id, next->id)) continue;
        next->created_time = d->commands[i].created_time;
        existing = true;
        break;
    }
    ts_auto_config_release(view);
    if (!existing) { time_t now = time(NULL); next->created_time = now > 0 ? now : 0; }
    cJSON *json = ret == ESP_OK ? ts_ssh_command_encode(next) : NULL;
    if (ret == ESP_OK) ret = json ? ts_auto_config_set(TS_AUTO_CONFIG_COMMAND, json, NULL, NULL) : ESP_ERR_NO_MEM;
    if (ret == ESP_OK && out) strcpy(out, next->id);
    cJSON_Delete(json); free(next); ts_auto_config_edit_end();
    return ret;
}

esp_err_t ts_ssh_commands_config_remove(const char *id)
{
    return !initialized ? ESP_ERR_INVALID_STATE :
        ts_auto_config_delete(TS_AUTO_CONFIG_COMMAND, id, TS_AUTO_DELETE_NORMAL, NULL, false, NULL, NULL, NULL);
}

esp_err_t ts_ssh_commands_config_clear(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    cJSON *empty = cJSON_CreateArray();
    if (!empty) return ESP_ERR_NO_MEM;
    esp_err_t ret = ts_auto_config_replace_collection(TS_AUTO_CONFIG_COMMAND, empty, NULL, NULL);
    cJSON_Delete(empty); return ret;
}

esp_err_t ts_ssh_commands_config_iterate_by_host(const char *host_id, ts_ssh_cmd_iterator_cb_t callback,
                                                void *context, size_t offset, size_t limit, size_t *total)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    bool ready = d != NULL;
    size_t matched = 0, returned = 0;
    bool stopped = false;
    for (int i = 0; d && i < d->command_count; ++i) {
        if (host_id && strcmp(host_id, d->commands[i].host_id)) continue;
        size_t position = matched++;
        if (stopped || position < offset || (limit && returned >= limit)) continue;
        ts_ssh_command_config_t copy = d->commands[i];
        copy.last_exec_time = ts_auto_config_command_time(view, i);
        stopped = !callback(&copy, returned++, context);
    }
    if (total) *total = matched;
    ts_auto_config_release(view);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ts_ssh_commands_config_iterate(ts_ssh_cmd_iterator_cb_t callback, void *context,
                                        size_t offset, size_t limit, size_t *total)
{
    return ts_ssh_commands_config_iterate_by_host(NULL, callback, context, offset, limit, total);
}

typedef struct { ts_ssh_command_config_t *items; size_t count; } list_context_t;
static bool list_item(const ts_ssh_command_config_t *item, size_t index, void *context)
{
    list_context_t *list = context; list->items[index] = *item; ++list->count; return true;
}

esp_err_t ts_ssh_commands_config_list_by_host(const char *host_id, ts_ssh_command_config_t *out,
                                             size_t capacity, size_t *count)
{
    if (!out || !capacity || !count) return ESP_ERR_INVALID_ARG;
    list_context_t list = {.items=out};
    esp_err_t ret = ts_ssh_commands_config_iterate_by_host(host_id, list_item, &list, 0, capacity, NULL);
    *count = list.count; return ret;
}
esp_err_t ts_ssh_commands_config_list(ts_ssh_command_config_t *out, size_t capacity, size_t *count)
{
    return ts_ssh_commands_config_list_by_host(NULL, out, capacity, count);
}

size_t ts_ssh_commands_config_count(void)
{
    if (!initialized) return 0;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    size_t count = d ? d->command_count : 0;
    ts_auto_config_release(view); return count;
}

esp_err_t ts_ssh_commands_config_update_exec_time(const ts_ssh_command_config_t *executed)
{
    if (!executed) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    time_t now = time(NULL);
    ts_auto_config_command_executed(executed->id, executed->instance, now > 0 ? now : 0);
    return ESP_OK;
}

esp_err_t ts_ssh_commands_precreate_variables(void)
{
    if (!initialized || !ts_variable_is_initialized()) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    const struct { const char *suffix; ts_auto_value_type_t type; } fields[] = {
        {"status", TS_AUTO_VAL_STRING}, {"exit_code", TS_AUTO_VAL_INT}, {"extracted", TS_AUTO_VAL_STRING},
        {"expect_matched", TS_AUTO_VAL_BOOL}, {"fail_matched", TS_AUTO_VAL_BOOL},
        {"host", TS_AUTO_VAL_STRING}, {"timestamp", TS_AUTO_VAL_INT}
    };
    esp_err_t ret = d ? ESP_OK : ESP_ERR_INVALID_STATE;
    for (int i = 0; ret == ESP_OK && i < d->command_count; ++i) {
        if (!d->commands[i].var_name[0]) continue;
        ts_auto_variable_t var = {0};
        strcpy(var.source_id, d->commands[i].var_name);
        for (unsigned j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j) {
            snprintf(var.name, sizeof(var.name), "%s.%s", d->commands[i].var_name, fields[j].suffix);
            ts_variable_info_t old;
            ret = ts_variable_get_info(var.name, &old);
            if (ret == ESP_OK) continue;
            if (ret != ESP_ERR_NOT_FOUND) break;
            var.value.type = fields[j].type; var.default_value = var.value;
            ret = ts_variable_register(&var);
            if (ret != ESP_OK) break;
        }
    }
    ts_auto_config_release(view); return ret;
}
