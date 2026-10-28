#include "ts_source_runtime.h"
#include "ts_automation_runtime.h"
#include "ts_automation_config.h"
#include "ts_action_codec.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_TS_AUTOMATION_MAX_SOURCES
#define CONFIG_TS_AUTOMATION_MAX_SOURCES 16
#endif

ts_source_runtime_t ts_source_runtime;

struct ts_source_config_view {
    bool keep_runtime;
    ts_auto_source_t *sources;
    int count, removed_count;
    char removed[CONFIG_TS_AUTOMATION_MAX_SOURCES][TS_AUTO_NAME_MAX_LEN];
};

esp_err_t ts_source_runtime_init(void)
{
    if (ts_source_runtime.initialized) return ESP_OK;
    ts_source_runtime.capacity = CONFIG_TS_AUTOMATION_MAX_SOURCES;
    ts_source_runtime.sources = heap_caps_calloc(ts_source_runtime.capacity, sizeof(ts_auto_source_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ts_source_runtime.sources) return ESP_ERR_NO_MEM;
    ts_source_runtime.mutex = xSemaphoreCreateMutex();
    if (!ts_source_runtime.mutex) { free(ts_source_runtime.sources);ts_source_runtime.sources = NULL;return ESP_ERR_NO_MEM; }
    ts_source_runtime.initialized = true;
    return ESP_OK;
}

void ts_source_runtime_deinit(void)
{
    free(ts_source_runtime.sources);
    if (ts_source_runtime.mutex) vSemaphoreDelete(ts_source_runtime.mutex);
    ts_source_runtime = (ts_source_runtime_t){0};
}

esp_err_t ts_source_config_prepare(ts_auto_document_t *document, bool keep_runtime, ts_source_config_view_t **out)
{
    if (!document || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!ts_source_runtime.initialized) return ESP_ERR_INVALID_STATE;
    if (document->source_count > ts_source_runtime.capacity) return ESP_ERR_INVALID_SIZE;
    int connections = 0;
    for (int i = 0; i < document->source_count; ++i)
        connections += document->sources[i].enabled && document->sources[i].type == TS_AUTO_SRC_SOCKETIO;
    if (connections > TS_SOURCE_MAX_SIO_CONNECTIONS) return ESP_ERR_INVALID_SIZE;
    ts_source_config_view_t *view = heap_caps_calloc(1, sizeof(*view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view) return ESP_ERR_NO_MEM;
    if (document->source_count) {
        view->sources = heap_caps_malloc(document->source_count * sizeof(*view->sources), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!view->sources) { free(view);return ESP_ERR_NO_MEM; }
        memcpy(view->sources, document->sources, document->source_count * sizeof(*view->sources));
    }
    view->count = document->source_count;view->keep_runtime=keep_runtime;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    for (int i = 0; i < view->count; ++i) view->sources[i].instance = 0;
    for (int i = 0; i < ts_source_runtime.count; ++i) {
        const ts_auto_source_t *old = &ts_source_runtime.sources[i];
        bool retained = false;
        for (int j = 0; keep_runtime && j < view->count; ++j) {
            if (!ts_source_config_same(old, &view->sources[j])) continue;
            view->sources[j].instance = old->instance;
            retained = true;
        }
        if (!retained) strcpy(view->removed[view->removed_count++], old->id);
    }
    /* Reserve identity before the immutable definitions become readable. The
     * transport table and API view must refer to the same source instance. */
    for (int i = 0; i < view->count; ++i) {
        if (!view->sources[i].instance) view->sources[i].instance = ++ts_source_runtime.next_instance;
        document->sources[i].instance = view->sources[i].instance;
    }
    xSemaphoreGive(ts_source_runtime.mutex);
    *out = view;
    return ESP_OK;
}

bool ts_source_config_keeps_variables(const ts_source_config_view_t *view, const char *source)
{
    for (int i = 0; view && i < view->removed_count; ++i)
        if (!strcmp(view->removed[i], source)) return false;
    return true;
}

void ts_source_config_discard(ts_source_config_view_t *view)
{
    if (!view) return;
    free(view->sources);free(view);
}

void ts_source_config_publish(ts_source_config_view_t *view)
{
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    for (int i = 0; i < view->count; ++i) {
        ts_auto_source_t *next = &view->sources[i];
        for (int j = 0; view->keep_runtime && j < ts_source_runtime.count; ++j) {
            const ts_auto_source_t *old = &ts_source_runtime.sources[j];
            if (!ts_source_config_same(old, next)) continue;
            next->connected = old->connected;
            next->auto_discovered = old->auto_discovered;
            next->last_value = old->last_value;
            next->last_update_ms = old->last_update_ms;
            break;
        }
    }
    memset(ts_source_runtime.sources, 0, ts_source_runtime.capacity * sizeof(*ts_source_runtime.sources));
    if (view->count) memcpy(ts_source_runtime.sources, view->sources, view->count * sizeof(*view->sources));
    ts_source_runtime.count = view->count;
    xSemaphoreGive(ts_source_runtime.mutex);
    ts_source_config_discard(view);
    ts_source_transport_changed();
}

void ts_source_config_clear(void)
{
    if(!ts_source_runtime.initialized)return;
    xSemaphoreTake(ts_source_runtime.mutex,portMAX_DELAY);
    ts_source_runtime.count=0;
    memset(ts_source_runtime.sources,0,ts_source_runtime.capacity*sizeof(*ts_source_runtime.sources));
    xSemaphoreGive(ts_source_runtime.mutex);
    ts_source_transport_changed();
}

esp_err_t ts_source_register(const ts_auto_source_t *source)
{
    if (!source || !source->id[0]) return ESP_ERR_INVALID_ARG;
    if (!ts_source_runtime.initialized) return ESP_ERR_INVALID_STATE;
    char *raw = ts_source_config_encode(source);
    cJSON *json = raw ? cJSON_Parse(raw) : NULL;
    esp_err_t ret = json ? ts_auto_config_set(TS_AUTO_CONFIG_SOURCE, json, NULL, NULL) : ESP_ERR_NO_MEM;
    cJSON_Delete(json);free(raw);return ret;
}

esp_err_t ts_source_unregister(const char *id)
{
    if (!ts_source_runtime.initialized) return ESP_ERR_INVALID_STATE;
    return ts_auto_config_delete(TS_AUTO_CONFIG_SOURCE, id, TS_AUTO_DELETE_NORMAL, NULL, false, NULL, NULL, NULL);
}

esp_err_t ts_source_get_copy(const char *id, ts_auto_source_t *out)
{
    if (!id || !out) return ESP_ERR_INVALID_ARG;
    if (!ts_source_runtime.initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    for (int i = 0; i < ts_source_runtime.count; ++i) {
        if (strcmp(id, ts_source_runtime.sources[i].id)) continue;
        *out = ts_source_runtime.sources[i];ret = ESP_OK;break;
    }
    xSemaphoreGive(ts_source_runtime.mutex);
    return ret;
}

static esp_err_t set_source_enabled(const char *id, bool enabled)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    ts_auto_config_status_t status;ts_auto_config_status(&status);
    esp_err_t ret = ts_auto_config_edit_begin(status.active_version, NULL);
    if (ret != ESP_OK) return ret;
    ts_auto_source_t *source = heap_caps_malloc(sizeof(*source), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ret = source ? ts_source_get_copy(id, source) : ESP_ERR_NO_MEM;
    if (ret == ESP_OK) { source->enabled = enabled;ret = ts_source_register(source); }
    free(source);ts_auto_config_edit_end();return ret;
}
esp_err_t ts_source_enable(const char *id) { return set_source_enabled(id, true); }
esp_err_t ts_source_disable(const char *id) { return set_source_enabled(id, false); }

int ts_source_count(void)
{
    if (!ts_source_runtime.initialized) return 0;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    int count = ts_source_runtime.count;
    xSemaphoreGive(ts_source_runtime.mutex);
    return count;
}

esp_err_t ts_source_get_by_index_copy(int index, ts_auto_source_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!ts_source_runtime.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    bool found = index >= 0 && index < ts_source_runtime.count;
    if (found) *out = ts_source_runtime.sources[index];
    xSemaphoreGive(ts_source_runtime.mutex);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int ts_source_enumerate(int filter, ts_source_enum_cb_t callback, void *context)
{
    if (!callback || !ts_source_runtime.initialized) return -1;
    ts_auto_source_t *copy = heap_caps_malloc(ts_source_runtime.capacity * sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return -1;
    xSemaphoreTake(ts_source_runtime.mutex, portMAX_DELAY);
    int count = ts_source_runtime.count;
    memcpy(copy, ts_source_runtime.sources, count * sizeof(*copy));
    xSemaphoreGive(ts_source_runtime.mutex);
    int visited = 0;
    for (int i = 0; i < count; ++i) {
        if (filter >= 0 && copy[i].type != filter) continue;
        ++visited;
        if (!callback(&copy[i], context)) break;
    }
    free(copy);
    return visited;
}
