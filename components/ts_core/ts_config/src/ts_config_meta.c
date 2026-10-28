#include "ts_config_meta.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>

static struct {
    bool initialized;
    nvs_handle_t nvs;
    SemaphoreHandle_t mutex;
    uint32_t seq;
    uint16_t versions[TS_CONFIG_MODULE_MAX];
} s_meta;

esp_err_t ts_config_meta_init(void)
{
    if (s_meta.initialized) return ESP_OK;
    s_meta.mutex = xSemaphoreCreateMutex();
    if (!s_meta.mutex) return ESP_ERR_NO_MEM;
    esp_err_t ret = nvs_open(TS_CONFIG_META_NAMESPACE, NVS_READWRITE, &s_meta.nvs);
    if (ret == ESP_OK) {
        ret = nvs_get_u32(s_meta.nvs, "global_seq", &s_meta.seq);
        if (ret == ESP_ERR_NVS_NOT_FOUND) { s_meta.seq = 0; ret = ESP_OK; }
    }
    for (int i = 0; ret == ESP_OK && i < TS_CONFIG_MODULE_MAX; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "schema_v%d", i);
        ret = nvs_get_u16(s_meta.nvs, key, &s_meta.versions[i]);
        if (ret == ESP_ERR_NVS_NOT_FOUND) { s_meta.versions[i] = 0; ret = ESP_OK; }
    }
    if (ret != ESP_OK) {
        if (s_meta.nvs) nvs_close(s_meta.nvs);
        s_meta.nvs = 0;
        vSemaphoreDelete(s_meta.mutex);
        s_meta.mutex = NULL;
        return ret;
    }
    s_meta.initialized = true;
    return ESP_OK;
}

esp_err_t ts_config_meta_deinit(void)
{
    if (!s_meta.initialized) return ESP_ERR_INVALID_STATE;
    nvs_close(s_meta.nvs);
    s_meta.nvs = 0;
    vSemaphoreDelete(s_meta.mutex);
    s_meta.mutex = NULL;
    s_meta.initialized = false;
    return ESP_OK;
}

uint32_t ts_config_meta_get_global_seq(void)
{
    return s_meta.seq;
}

uint32_t ts_config_meta_increment_global_seq(void)
{
    if (!s_meta.initialized) return 0;
    xSemaphoreTake(s_meta.mutex, portMAX_DELAY);
    uint32_t next = s_meta.seq + 1;
    esp_err_t ret = next ? nvs_set_u32(s_meta.nvs, "global_seq", next) : ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK) ret = nvs_commit(s_meta.nvs);
    if (ret == ESP_OK) s_meta.seq = next;
    xSemaphoreGive(s_meta.mutex);
    return ret == ESP_OK ? next : 0;
}

uint16_t ts_config_meta_get_schema_version(ts_config_module_t module)
{
    return (unsigned)module < TS_CONFIG_MODULE_MAX ? s_meta.versions[module] : 0;
}

esp_err_t ts_config_meta_set_schema_version(ts_config_module_t module, uint16_t version)
{
    if (!s_meta.initialized || (unsigned)module >= TS_CONFIG_MODULE_MAX)
        return ESP_ERR_INVALID_ARG;
    char key[16];
    snprintf(key, sizeof(key), "schema_v%d", module);
    xSemaphoreTake(s_meta.mutex, portMAX_DELAY);
    esp_err_t ret = nvs_set_u16(s_meta.nvs, key, version);
    if (ret == ESP_OK) ret = nvs_commit(s_meta.nvs);
    if (ret == ESP_OK) s_meta.versions[module] = version;
    xSemaphoreGive(s_meta.mutex);
    return ret;
}

void ts_config_meta_dump(void)
{
    ESP_LOGI("config_meta", "System configuration sequence: %lu", (unsigned long)s_meta.seq);
    for (int i = 0; i < TS_CONFIG_MODULE_MAX; ++i) {
        if (s_meta.versions[i])
            ESP_LOGI("config_meta", "%s: v%u", ts_config_module_get_name(i), s_meta.versions[i]);
    }
}
