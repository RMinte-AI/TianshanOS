/**
 * @file ts_api_ui.c
 * @brief UI Configuration API Handlers
 * 
 * WebUI 配置持久化 API
 * - ui.widgets.get: 获取数据监控组件配置
 * - ui.widgets.set: 保存数据监控组件配置
 * 
 * 监控组件配置保存在系统 NVS：ts_ui / widgets。
 * 升级时旧 SD 配置由系统迁移统一导入；此 API 不再同步 SD。
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_log.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <string.h>

#define TAG "api_ui"

/* 配置文件路径 */
#define UI_WIDGETS_NVS_NS   "ts_ui"
#define UI_WIDGETS_NVS_KEY  "widgets"

/* 最大配置大小 (NVS blob 限制) */
#define UI_WIDGETS_MAX_SIZE 4000

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief 从 NVS 加载 widgets 配置
 * @return ESP_ERR_NVS_NOT_FOUND only when the configuration does not exist.
 */
static esp_err_t load_widgets_from_nvs(cJSON **out)
{
    *out = NULL;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(UI_WIDGETS_NVS_NS, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 获取数据大小 */
    size_t size = 0;
    ret = nvs_get_blob(handle, UI_WIDGETS_NVS_KEY, NULL, &size);
    if (ret != ESP_OK || size == 0) {
        nvs_close(handle);
        return ret == ESP_OK ? ESP_ERR_INVALID_RESPONSE : ret;
    }
    
    /* 读取数据 */
    char *content = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    if (!content) {
        content = malloc(size + 1);
    }
    if (!content) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    
    ret = nvs_get_blob(handle, UI_WIDGETS_NVS_KEY, content, &size);
    nvs_close(handle);
    
    if (ret != ESP_OK) {
        free(content);
        return ret;
    }
    content[size] = '\0';
    
    /* 解析 JSON */
    cJSON *json = cJSON_Parse(content);
    free(content);
    
    if (!cJSON_IsObject(json) || !cJSON_IsArray(cJSON_GetObjectItem(json, "widgets"))) {
        cJSON_Delete(json);
        TS_LOGW(TAG, "Failed to parse NVS config");
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    TS_LOGI(TAG, "Loaded widgets from NVS");
    *out = json;
    return ESP_OK;
}

/**
 * @brief 保存 widgets 配置到 NVS
 * @param json 配置 JSON
 * @return ESP_OK 成功
 */
static esp_err_t save_widgets_to_nvs(cJSON *json)
{
    /* 生成 JSON 字符串 */
    char *content = cJSON_PrintUnformatted(json);
    if (!content) {
        return ESP_ERR_NO_MEM;
    }
    
    size_t len = strlen(content);
    if (len > UI_WIDGETS_MAX_SIZE) {
        TS_LOGW(TAG, "Config too large for NVS: %zu > %d", len, UI_WIDGETS_MAX_SIZE);
        cJSON_free(content);
        return ESP_ERR_INVALID_SIZE;
    }
    
    /* 打开 NVS */
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(UI_WIDGETS_NVS_NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        cJSON_free(content);
        return ret;
    }
    
    /* 写入 blob */
    ret = nvs_set_blob(handle, UI_WIDGETS_NVS_KEY, content, len);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    
    nvs_close(handle);
    cJSON_free(content);
    
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Saved widgets to NVS (%zu bytes)", len);
    }
    
    return ret;
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief ui.widgets.get - 获取数据监控组件配置
 * 
 * 从系统 NVS 读取；首次使用返回空配置
 * 
 * @return JSON:
 * {
 *   "widgets": [...],
 *   "refresh_interval": 5000,
 *   "source": "nvs" | "default"
 * }
 */
static esp_err_t api_ui_widgets_get(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    cJSON *config = NULL;
    const char *source = "default";
    
    esp_err_t ret = load_widgets_from_nvs(&config);
    if (ret == ESP_OK) source = "nvs";
    else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ts_api_result_error(result, ret == ESP_ERR_NO_MEM ? TS_API_ERR_NO_MEM : TS_API_ERR_INTERNAL,
                            "widgets_load_failed");
        return ret;
    }

    /* 构建响应 */
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(config);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "widgets_load_failed");
        return ESP_ERR_NO_MEM;
    }
    
    const cJSON *widgets = config ? cJSON_GetObjectItem(config, "widgets") : NULL;
    const cJSON *interval = config ? cJSON_GetObjectItem(config, "refresh_interval") : NULL;
    cJSON *copy = widgets ? cJSON_Duplicate(widgets, true) : cJSON_CreateArray();
    bool built = copy && cJSON_AddItemToObject(data, "widgets", copy);
    if (!built) cJSON_Delete(copy);
    built = built && cJSON_AddNumberToObject(data, "refresh_interval", cJSON_IsNumber(interval) ? interval->valueint : 5000);
    built = built && cJSON_AddStringToObject(data, "source", source);
    cJSON_Delete(config);
    if (!built) {
        cJSON_Delete(data);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "widgets_load_failed");
        return ESP_ERR_NO_MEM;
    }
    
    ts_api_result_ok(result, data);
    TS_LOGI(TAG, "Loaded UI widgets config (source: %s)", source);
    return ESP_OK;
}

/**
 * @brief ui.widgets.set - 保存数据监控组件配置
 * 
 * 保存到系统 NVS
 * 
 * @param params JSON:
 * {
 *   "widgets": [...],
 *   "refresh_interval": 5000
 * }
 */
static esp_err_t api_ui_widgets_set(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 构建配置对象 */
    cJSON *config = cJSON_CreateObject();
    
    /* 复制 widgets 数组 */
    cJSON *widgets = cJSON_GetObjectItem(params, "widgets");
    if (widgets && cJSON_IsArray(widgets)) {
        cJSON_AddItemToObject(config, "widgets", cJSON_Duplicate(widgets, true));
    } else {
        cJSON_AddArrayToObject(config, "widgets");
    }
    
    /* 复制刷新间隔 */
    cJSON *interval = cJSON_GetObjectItem(params, "refresh_interval");
    if (interval && cJSON_IsNumber(interval)) {
        cJSON_AddNumberToObject(config, "refresh_interval", interval->valueint);
    } else {
        cJSON_AddNumberToObject(config, "refresh_interval", 5000);
    }
    
    /* 系统设置只写系统配置区。 */
    esp_err_t nvs_ret = save_widgets_to_nvs(config);
    
    cJSON_Delete(config);
    
    /* 响应 */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "nvs_saved", nvs_ret == ESP_OK);
    
    if (nvs_ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to save config");
        cJSON_Delete(data);
        return ESP_FAIL;
    }
    
    ts_api_result_ok(result, data);
    TS_LOGI(TAG, "Saved UI widgets config to system NVS");
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t ui_endpoints[] = {
    {
        .name = "ui.widgets.get",
        .description = "Get data widgets configuration",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_ui_widgets_get,
        .requires_auth = false,
    },
    {
        .name = "ui.widgets.set",
        .description = "Save data widgets configuration",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_ui_widgets_set,
        .requires_auth = false,  /* WebUI 内部使用，暂不要求认证 */
    },
};

esp_err_t ts_api_ui_register(void)
{
    TS_LOGI(TAG, "Registering UI APIs");
    
    for (size_t i = 0; i < sizeof(ui_endpoints) / sizeof(ui_endpoints[0]); i++) {
        esp_err_t ret = ts_api_register(&ui_endpoints[i]);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to register %s", ui_endpoints[i].name);
            return ret;
        }
    }
    
    return ESP_OK;
}
