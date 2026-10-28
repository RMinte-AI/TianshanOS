/* SSH host definitions share the active automation configuration. */
#include "ts_ssh_hosts_config.h"
#include "ts_ssh_config_codec.h"
#include "ts_automation_config.h"
#include <string.h>
#include <time.h>

static bool initialized;

esp_err_t ts_ssh_hosts_config_init(void) { initialized = true; return ESP_OK; }
void ts_ssh_hosts_config_deinit(void) { initialized = false; }
bool ts_ssh_hosts_config_is_initialized(void) { return initialized; }
esp_err_t ts_ssh_hosts_config_load_state(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    bool ready = ts_auto_config_definitions(view) != NULL;
    ts_auto_config_release(view); return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ts_ssh_hosts_config_get(const char *id, ts_ssh_host_config_t *out)
{
    if (!id || !out) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    esp_err_t ret = d ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    for (int i = 0; d && i < d->host_count; ++i) {
        if (strcmp(d->hosts[i].id, id)) continue;
        *out = d->hosts[i]; ret = ESP_OK; break;
    }
    ts_auto_config_release(view); return ret;
}

esp_err_t ts_ssh_hosts_config_add(const ts_ssh_host_config_t *config)
{
    if (!config || !config->id[0]) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    ts_auto_config_status_t status; ts_auto_config_status(&status);
    esp_err_t ret = ts_auto_config_edit_begin(status.active_version, NULL);
    if (ret != ESP_OK) return ret;
    ts_ssh_host_config_t next = *config, old;
    if (ts_ssh_hosts_config_get(next.id, &old) == ESP_OK) next.created_time = old.created_time;
    else { time_t now = time(NULL); next.created_time = now > 0 ? now : 0; }
    cJSON *json = ts_ssh_host_encode(&next);
    ret = json ? ts_auto_config_set(TS_AUTO_CONFIG_HOST, json, NULL, NULL) : ESP_ERR_NO_MEM;
    cJSON_Delete(json); ts_auto_config_edit_end(); return ret;
}

esp_err_t ts_ssh_hosts_config_remove(const char *id)
{
    return !initialized ? ESP_ERR_INVALID_STATE :
        ts_auto_config_delete(TS_AUTO_CONFIG_HOST, id, TS_AUTO_DELETE_NORMAL, NULL, false, NULL, NULL, NULL);
}

esp_err_t ts_ssh_hosts_config_clear(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    cJSON *empty = cJSON_CreateArray();
    if (!empty) return ESP_ERR_NO_MEM;
    esp_err_t ret = ts_auto_config_replace_collection(TS_AUTO_CONFIG_HOST, empty, NULL, NULL);
    cJSON_Delete(empty); return ret;
}

esp_err_t ts_ssh_hosts_config_find(const char *host, uint16_t port, const char *username, ts_ssh_host_config_t *out)
{
    if (!host || !out) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    esp_err_t ret = d ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    for (int i = 0; d && i < d->host_count; ++i) {
        const ts_ssh_host_config_t *candidate = &d->hosts[i];
        if (strcmp(host, candidate->host) || port != candidate->port ||
            (username && strcmp(username, candidate->username))) continue;
        *out = *candidate; ret = ESP_OK; break;
    }
    ts_auto_config_release(view); return ret;
}

esp_err_t ts_ssh_hosts_config_iterate(ts_ssh_host_iterator_cb_t callback, void *context,
                                     size_t offset, size_t limit, size_t *total)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    bool ready = d != NULL;
    size_t count = d ? d->host_count : 0, returned = 0;
    for (size_t i = offset; i < count && (!limit || returned < limit); ++i) {
        if (!callback(&d->hosts[i], returned++, context)) break;
    }
    if (total) *total = count;
    ts_auto_config_release(view); return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

typedef struct { ts_ssh_host_config_t *items; size_t count; } list_context_t;
static bool list_item(const ts_ssh_host_config_t *item, size_t index, void *context)
{
    list_context_t *list = context; list->items[index] = *item; ++list->count; return true;
}
esp_err_t ts_ssh_hosts_config_list(ts_ssh_host_config_t *out, size_t capacity, size_t *count)
{
    if (!out || !capacity || !count) return ESP_ERR_INVALID_ARG;
    list_context_t list = {.items=out};
    esp_err_t ret = ts_ssh_hosts_config_iterate(list_item, &list, 0, capacity, NULL);
    *count = list.count; return ret;
}
int ts_ssh_hosts_config_count(void)
{
    if (!initialized) return 0;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const ts_auto_document_t *d = ts_auto_config_definitions(view);
    int count = d ? d->host_count : 0;
    ts_auto_config_release(view); return count;
}
