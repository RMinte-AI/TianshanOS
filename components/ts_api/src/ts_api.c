/**
 * @file ts_api.c
 * @brief TianShanOS Core API Layer Implementation
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_api.h"
#include "ts_config_pack.h"
#include "ts_core.h"  /* TS_STRDUP_PSRAM, TS_CALLOC_PSRAM */
#include "ts_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

#define TAG "ts_api"

/*===========================================================================*/
/*                          Name Tables                                       */
/*===========================================================================*/

static const char *s_code_names[] = {
    [TS_API_OK]              = "OK",
    [TS_API_ERR_INVALID_ARG] = "INVALID_ARG",
    [TS_API_ERR_NOT_FOUND]   = "NOT_FOUND",
    [TS_API_ERR_NO_PERMISSION] = "NO_PERMISSION",
    [TS_API_ERR_BUSY]        = "BUSY",
    [TS_API_ERR_TIMEOUT]     = "TIMEOUT",
    [TS_API_ERR_NO_MEM]      = "NO_MEM",
    [TS_API_ERR_INTERNAL]    = "INTERNAL",
    [TS_API_ERR_NOT_SUPPORTED] = "NOT_SUPPORTED",
    [TS_API_ERR_HARDWARE]    = "HARDWARE",
    [TS_API_ERR_CONNECTION]  = "CONNECTION",
    [TS_API_ERR_AUTH]        = "AUTH"
};

static const char *s_category_names[] = {
    [TS_API_CAT_SYSTEM]  = "system",
    [TS_API_CAT_CONFIG]  = "config",
    [TS_API_CAT_HAL]     = "hal",
    [TS_API_CAT_LED]     = "led",
    [TS_API_CAT_FAN]     = "fan",
    [TS_API_CAT_POWER]   = "power",
    [TS_API_CAT_NETWORK] = "network",
    [TS_API_CAT_DEVICE]  = "device",
    [TS_API_CAT_STORAGE] = "storage"
};

/*===========================================================================*/
/*                          Endpoint Registry                                 */
/*===========================================================================*/

#ifndef CONFIG_TS_API_MAX_ENDPOINTS
#define CONFIG_TS_API_MAX_ENDPOINTS 128
#endif

#ifndef CONFIG_TS_API_MAX_NAME_LENGTH
#define CONFIG_TS_API_MAX_NAME_LENGTH 64
#endif

typedef struct {
    char name[CONFIG_TS_API_MAX_NAME_LENGTH];
    char *description;
    ts_api_category_t category;
    ts_api_handler_t handler;
    bool requires_auth;
    char *permission;
    bool used;
} api_entry_t;

/*===========================================================================*/
/*                          Private Data                                      */
/*===========================================================================*/

static struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    api_entry_t *endpoints;
    size_t endpoint_count;
} s_api = {0};

/*===========================================================================*/
/*                          Private Functions                                 */
/*===========================================================================*/

static api_entry_t *find_endpoint(const char *name)
{
    for (size_t i = 0; i < CONFIG_TS_API_MAX_ENDPOINTS; i++) {
        if (s_api.endpoints[i].used && 
            strcmp(s_api.endpoints[i].name, name) == 0) {
            return &s_api.endpoints[i];
        }
    }
    return NULL;
}

static api_entry_t *find_free_slot(void)
{
    for (size_t i = 0; i < CONFIG_TS_API_MAX_ENDPOINTS; i++) {
        if (!s_api.endpoints[i].used) {
            return &s_api.endpoints[i];
        }
    }
    return NULL;
}

/*===========================================================================*/
/*                          Core API Implementation                           */
/*===========================================================================*/

esp_err_t ts_api_init(void)
{
    if (s_api.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Create mutex */
    s_api.mutex = xSemaphoreCreateMutex();
    if (s_api.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    /* Allocate endpoint array in PSRAM */
    s_api.endpoints = heap_caps_calloc(CONFIG_TS_API_MAX_ENDPOINTS, sizeof(api_entry_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_api.endpoints == NULL) {
        TS_LOGW(TAG, "PSRAM not available, using DRAM for API endpoints");
        s_api.endpoints = calloc(CONFIG_TS_API_MAX_ENDPOINTS, sizeof(api_entry_t));
        if (s_api.endpoints == NULL) {
            vSemaphoreDelete(s_api.mutex);
            return ESP_ERR_NO_MEM;
        }
    } else {
        TS_LOGI(TAG, "API endpoints allocated in PSRAM (%zu bytes)",
                CONFIG_TS_API_MAX_ENDPOINTS * sizeof(api_entry_t));
    }
    
    s_api.endpoint_count = 0;
    s_api.initialized = true;
    
    TS_LOGI(TAG, "API layer initialized (max %d endpoints)", CONFIG_TS_API_MAX_ENDPOINTS);
    
    return ESP_OK;
}

esp_err_t ts_api_deinit(void)
{
    if (!s_api.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Free descriptions and permissions */
    for (size_t i = 0; i < CONFIG_TS_API_MAX_ENDPOINTS; i++) {
        if (s_api.endpoints[i].used) {
            free(s_api.endpoints[i].description);
            free(s_api.endpoints[i].permission);
        }
    }
    
    free(s_api.endpoints);
    vSemaphoreDelete(s_api.mutex);
    
    s_api.endpoints = NULL;
    s_api.mutex = NULL;
    s_api.endpoint_count = 0;
    s_api.initialized = false;
    
    TS_LOGI(TAG, "API layer deinitialized");
    
    return ESP_OK;
}

esp_err_t ts_api_register(const ts_api_endpoint_t *endpoint)
{
    if (!s_api.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (endpoint == NULL || endpoint->name == NULL || endpoint->handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strlen(endpoint->name) >= CONFIG_TS_API_MAX_NAME_LENGTH) {
        TS_LOGE(TAG, "API name too long: %s", endpoint->name);
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_api.mutex, portMAX_DELAY);
    
    /* Check for duplicate */
    if (find_endpoint(endpoint->name) != NULL) {
        xSemaphoreGive(s_api.mutex);
        TS_LOGE(TAG, "API already registered: %s", endpoint->name);
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Find free slot */
    api_entry_t *entry = find_free_slot();
    if (entry == NULL) {
        xSemaphoreGive(s_api.mutex);
        TS_LOGE(TAG, "No free API slots");
        return ESP_ERR_NO_MEM;
    }
    
    /* Fill entry */
    strncpy(entry->name, endpoint->name, CONFIG_TS_API_MAX_NAME_LENGTH - 1);
    entry->name[CONFIG_TS_API_MAX_NAME_LENGTH - 1] = '\0';
    
    entry->description = endpoint->description ? TS_STRDUP_PSRAM(endpoint->description) : NULL;
    entry->category = endpoint->category;
    entry->handler = endpoint->handler;
    entry->requires_auth = endpoint->requires_auth;
    entry->permission = endpoint->permission ? TS_STRDUP_PSRAM(endpoint->permission) : NULL;
    entry->used = true;
    
    s_api.endpoint_count++;
    
    xSemaphoreGive(s_api.mutex);
    
    TS_LOGD(TAG, "Registered API: %s", endpoint->name);
    
    return ESP_OK;
}

esp_err_t ts_api_register_multiple(const ts_api_endpoint_t *endpoints, size_t count)
{
    if (endpoints == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    for (size_t i = 0; i < count; i++) {
        esp_err_t ret = ts_api_register(&endpoints[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    return ESP_OK;
}

esp_err_t ts_api_unregister(const char *name)
{
    if (!s_api.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_api.mutex, portMAX_DELAY);
    
    api_entry_t *entry = find_endpoint(name);
    if (entry == NULL) {
        xSemaphoreGive(s_api.mutex);
        return ESP_ERR_NOT_FOUND;
    }
    
    free(entry->description);
    free(entry->permission);
    memset(entry, 0, sizeof(api_entry_t));
    s_api.endpoint_count--;
    
    xSemaphoreGive(s_api.mutex);
    
    TS_LOGD(TAG, "Unregistered API: %s", name);
    
    return ESP_OK;
}

esp_err_t ts_api_call(const char *name, const cJSON *params, ts_api_result_t *result)
{
    if (!s_api.initialized) {
        if (result) {
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "API not initialized");
        }
        return ESP_ERR_INVALID_STATE;
    }
    
    if (name == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_api_result_init(result);
    
    xSemaphoreTake(s_api.mutex, portMAX_DELAY);
    
    api_entry_t *entry = find_endpoint(name);
    if (entry == NULL) {
        xSemaphoreGive(s_api.mutex);
        TS_LOGW(TAG, "API not found: %s (total registered: %d)", name, (int)s_api.endpoint_count);
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "API not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    /* Copy handler to call outside mutex */
    ts_api_handler_t handler = entry->handler;
    
    xSemaphoreGive(s_api.mutex);
    
    /* Call handler */
    TS_LOGD(TAG, "Calling API: %s", name);
    esp_err_t ret = handler(params, result);
    
    if (ret != ESP_OK && result->code == TS_API_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Handler error");
    }
    
    return ret;
}

esp_err_t ts_api_call_str(const char *name, const char *params_json, ts_api_result_t *result)
{
    cJSON *params = NULL;
    
    if (params_json && strlen(params_json) > 0) {
        params = name&&!strcmp(name,"automation.rules.import") ?
            ts_config_pack_parse_import_request(params_json,strlen(params_json)) : cJSON_Parse(params_json);
        if (params == NULL) {
            if (result) {
                ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid JSON");
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    
    esp_err_t ret = ts_api_call(name, params, result);
    
    if (params) {
        cJSON_Delete(params);
    }
    
    return ret;
}

cJSON *ts_api_list(ts_api_category_t category)
{
    if (!s_api.initialized) {
        return NULL;
    }
    
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }
    
    xSemaphoreTake(s_api.mutex, portMAX_DELAY);
    
    for (size_t i = 0; i < CONFIG_TS_API_MAX_ENDPOINTS; i++) {
        if (s_api.endpoints[i].used) {
            if (category >= TS_API_CAT_MAX || 
                s_api.endpoints[i].category == category) {
                cJSON_AddItemToArray(arr, 
                    cJSON_CreateString(s_api.endpoints[i].name));
            }
        }
    }
    
    xSemaphoreGive(s_api.mutex);
    
    return arr;
}

cJSON *ts_api_get_info(const char *name)
{
    if (!s_api.initialized || name == NULL) {
        return NULL;
    }
    
    xSemaphoreTake(s_api.mutex, portMAX_DELAY);
    
    api_entry_t *entry = find_endpoint(name);
    if (entry == NULL) {
        xSemaphoreGive(s_api.mutex);
        return NULL;
    }
    
    cJSON *info = cJSON_CreateObject();
    if (info) {
        cJSON_AddStringToObject(info, "name", entry->name);
        if (entry->description) {
            cJSON_AddStringToObject(info, "description", entry->description);
        }
        cJSON_AddStringToObject(info, "category", 
            ts_api_category_name(entry->category));
        cJSON_AddBoolToObject(info, "requires_auth", entry->requires_auth);
        if (entry->permission) {
            cJSON_AddStringToObject(info, "permission", entry->permission);
        }
    }
    
    xSemaphoreGive(s_api.mutex);
    
    return info;
}

/*===========================================================================*/
/*                          Result Helpers                                    */
/*===========================================================================*/

void ts_api_result_init(ts_api_result_t *result)
{
    if (result) {
        result->code = TS_API_OK;
        result->message = NULL;
        result->data = NULL;
    }
}

void ts_api_result_free(ts_api_result_t *result)
{
    if (result) {
        free(result->message);
        if (result->data) {
            cJSON_Delete(result->data);
        }
        result->message = NULL;
        result->data = NULL;
    }
}

void ts_api_result_ok(ts_api_result_t *result, cJSON *data)
{
    if (result) {
        result->code = TS_API_OK;
        free(result->message);
        result->message = NULL;
        if (result->data) {
            cJSON_Delete(result->data);
        }
        result->data = data;
    }
}

void ts_api_result_error(ts_api_result_t *result, ts_api_result_code_t code,
                          const char *message)
{
    if (result) {
        result->code = code;
        free(result->message);
        result->message = message ? TS_STRDUP_PSRAM(message) : NULL;
        if (result->data) {
            cJSON_Delete(result->data);
            result->data = NULL;
        }
    }
}

cJSON *ts_api_result_to_json(const ts_api_result_t *result)
{
    if (result == NULL) {
        return NULL;
    }
    
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return NULL;
    }
    
    cJSON_AddBoolToObject(json, "success", result->code == TS_API_OK);
    cJSON_AddStringToObject(json, "code", ts_api_code_name(result->code));
    
    if (result->message) {
        cJSON_AddStringToObject(json, "message", result->message);
    }
    
    if (result->data) {
        cJSON_AddItemToObject(json, "data", cJSON_Duplicate(result->data, 1));
    }
    
    return json;
}

char *ts_api_result_to_string(const ts_api_result_t *result)
{
    cJSON *json = ts_api_result_to_json(result);
    if (json == NULL) {
        return NULL;
    }
    
    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    return str;
}

const char *ts_api_code_name(ts_api_result_code_t code)
{
    if (code >= TS_API_ERR_MAX) {
        return "UNKNOWN";
    }
    return s_code_names[code];
}

/*===========================================================================*/
/*                          Category Info                                     */
/*===========================================================================*/

const char *ts_api_category_name(ts_api_category_t category)
{
    if (category >= TS_API_CAT_MAX) {
        return "unknown";
    }
    return s_category_names[category];
}

ts_api_category_t ts_api_category_by_name(const char *name)
{
    if (name == NULL) {
        return TS_API_CAT_MAX;
    }
    
    for (int i = 0; i < TS_API_CAT_MAX; i++) {
        if (strcmp(s_category_names[i], name) == 0) {
            return (ts_api_category_t)i;
        }
    }
    
    return TS_API_CAT_MAX;
}

/*===========================================================================*/
/*                      Register All API Modules                              */
/*===========================================================================*/

esp_err_t ts_api_register_all(void)
{
    esp_err_t ret;
    
    TS_LOGI(TAG, "Registering all API modules...");
    
    /* System APIs */
    ret = ts_api_system_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register system APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Config APIs */
    ret = ts_api_config_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register config APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Device APIs */
    ret = ts_api_device_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register device APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* LED APIs */
    ret = ts_api_led_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register LED APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Network APIs */
    ret = ts_api_network_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register network APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Fan APIs */
    ret = ts_api_fan_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register fan APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Power APIs */
    ret = ts_api_power_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register power APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Temperature APIs */
    ret = ts_api_temp_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register temp APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Service APIs */
    ret = ts_api_service_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register service APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Storage APIs */
    ret = ts_api_storage_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register storage APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* GPIO APIs */
    ret = ts_api_gpio_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register GPIO APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Compute Monitor APIs */
    ret = ts_api_monitor_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register monitor APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* WiFi APIs */
    ret = ts_api_wifi_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register WiFi APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* DHCP Server APIs */
    ret = ts_api_dhcp_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register DHCP APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* NAT Gateway APIs */
    ret = ts_api_nat_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register NAT APIs: %s", esp_err_to_name(ret));
        return ret;
    }

    /* LPMU upper network access APIs */
    ret = ts_api_lpmu_access_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register LPMU access APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* SSH Known Hosts APIs */
    ret = ts_api_hosts_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register Hosts APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Key Management APIs */
    ret = ts_api_key_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register Key APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* SSH APIs */
    ret = ts_api_ssh_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register SSH APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* SFTP APIs */
    ret = ts_api_sftp_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register SFTP APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Time APIs */
    ret = ts_api_time_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register Time APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* OTA APIs */
    ret = ts_api_ota_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register OTA APIs: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Log APIs */
    ts_api_log_register();
    
    /* Automation APIs */
    ret = ts_api_automation_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register Automation APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Certificate (PKI) APIs */
    ret = ts_api_cert_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register Certificate APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Authentication APIs */
    ret = ts_api_auth_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register Auth APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* UI Configuration APIs */
    ret = ts_api_ui_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register UI APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Config Pack APIs */
    ret = ts_api_config_pack_register();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register Config Pack APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Variable APIs 已废弃，统一使用 automation.variables.* API */
    
    TS_LOGI(TAG, "All API modules registered (%zu endpoints)", s_api.endpoint_count);
    
    return ESP_OK;
}
