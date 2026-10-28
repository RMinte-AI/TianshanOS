/**
 * @file ts_config_module.c
 * @brief TianShanOS Unified Configuration Module System Implementation
 *
 * 统一配置模块系统实现
 * System modules persist only in the default system NVS.
 * SD import/export are explicit operations; legacy priority is migration-only.
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "ts_config_module.h"
#include "ts_config_meta.h"
#include "ts_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

/* PSRAM 优先分配宏 */
#define TS_MOD_MALLOC(size) ({ void *p = heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); p ? p : malloc(size); })

static const char *TAG = "ts_config_module";

/* ============================================================================
 * 常量定义
 * ========================================================================== */

/** 模块名称到文件名映射 */
static const char *const MODULE_FILENAMES[] = {
    [TS_CONFIG_MODULE_NET]     = "net.json",
    [TS_CONFIG_MODULE_DHCP]    = "dhcp.json",
    [TS_CONFIG_MODULE_WIFI]    = "wifi.json",
    [TS_CONFIG_MODULE_NAT]     = "nat.json",
    [TS_CONFIG_MODULE_LED]     = "led.json",
    [TS_CONFIG_MODULE_FAN]     = "fan.json",
    [TS_CONFIG_MODULE_DEVICE]  = "device.json",
    [TS_CONFIG_MODULE_SYSTEM]  = "system.json",
    [TS_CONFIG_MODULE_TEMP]    = "temp.json",
    [TS_CONFIG_MODULE_RULES]   = "rules.json",
    [TS_CONFIG_MODULE_ACTIONS] = "actions.json",
    [TS_CONFIG_MODULE_SOURCES] = "sources.json",
};

/** 模块名称 */
static const char *const MODULE_NAMES[] = {
    [TS_CONFIG_MODULE_NET]     = "NET",
    [TS_CONFIG_MODULE_DHCP]    = "DHCP",
    [TS_CONFIG_MODULE_WIFI]    = "WIFI",
    [TS_CONFIG_MODULE_NAT]     = "NAT",
    [TS_CONFIG_MODULE_LED]     = "LED",
    [TS_CONFIG_MODULE_FAN]     = "FAN",
    [TS_CONFIG_MODULE_DEVICE]  = "DEVICE",
    [TS_CONFIG_MODULE_SYSTEM]  = "SYSTEM",
    [TS_CONFIG_MODULE_TEMP]    = "TEMP",
    [TS_CONFIG_MODULE_RULES]   = "RULES",
    [TS_CONFIG_MODULE_ACTIONS] = "ACTIONS",
    [TS_CONFIG_MODULE_SOURCES] = "SOURCES",
};

/** JSON 元数据键名 */
#define JSON_KEY_META       "_meta"
#define JSON_KEY_SEQ        "seq"
#define JSON_KEY_VERSION    "version"

/* ============================================================================
 * 私有变量
 * ========================================================================== */

static struct {
    bool                    initialized;
    SemaphoreHandle_t       mutex;
    
    /* 模块信息 */
    ts_config_module_info_t modules[TS_CONFIG_MODULE_MAX];
    
    /* 内存缓存：使用 ts_config 的链表存储 */
    /* 每个模块的缓存前缀为 "{module}." 如 "net.eth.ip" */
} s_mgr = {0};

/* ============================================================================
 * 辅助函数声明
 * ========================================================================== */

static bool is_sdcard_mounted(void);
static esp_err_t ensure_config_dir(void);
static esp_err_t read_json_file(const char *path, cJSON **root);
static esp_err_t write_json_file(const char *path, cJSON *root);
static esp_err_t module_to_json(ts_config_module_t module, cJSON **root);
static esp_err_t json_to_module(ts_config_module_t module, cJSON *root);
static char *make_cache_key(ts_config_module_t module, const char *key, char *buf, size_t len);
static const ts_config_schema_entry_t *find_schema_entry(ts_config_module_t module, const char *key);

/* ============================================================================
 * 初始化
 * ========================================================================== */

esp_err_t ts_config_module_system_init(void)
{
    if (s_mgr.initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing configuration module system...");
    
    /* 创建互斥锁 */
    s_mgr.mutex = xSemaphoreCreateMutex();
    if (s_mgr.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    /* 初始化元配置管理 */
    esp_err_t ret = ts_config_meta_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init meta: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_mgr.mutex);
        s_mgr.mutex = NULL;
        return ret;
    }
    
    /* 清空模块信息 */
    memset(s_mgr.modules, 0, sizeof(s_mgr.modules));
    
    s_mgr.initialized = true;
    ESP_LOGI(TAG, "Configuration module system initialized");
    
    return ESP_OK;
}

/* ============================================================================
 * 模块注册
 * ========================================================================== */

esp_err_t ts_config_module_register(
    ts_config_module_t module,
    const char *nvs_namespace,
    const ts_config_module_schema_t *schema)
{
    if (module >= TS_CONFIG_MODULE_MAX || nvs_namespace == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strlen(nvs_namespace) >= sizeof(s_mgr.modules[0].nvs_namespace)) {
        ESP_LOGE(TAG, "NVS namespace too long: %s", nvs_namespace);
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_mgr.mutex, portMAX_DELAY);
    
    if (s_mgr.modules[module].registered) {
        xSemaphoreGive(s_mgr.mutex);
        ESP_LOGW(TAG, "Module %s already registered", MODULE_NAMES[module]);
        return TS_CONFIG_ERR_ALREADY_REGISTERED;
    }
    
    ts_config_module_info_t *info = &s_mgr.modules[module];
    info->registered = true;
    strncpy(info->nvs_namespace, nvs_namespace, sizeof(info->nvs_namespace) - 1);
    info->schema = schema;
    info->loaded_version = 0;
    info->seq = 0;
    info->dirty = false;
    
    xSemaphoreGive(s_mgr.mutex);
    
    ESP_LOGI(TAG, "Registered module %s (nvs=%s, schema_v=%d)", 
             MODULE_NAMES[module], nvs_namespace, schema ? schema->version : 0);
    
    return ESP_OK;
}

bool ts_config_module_is_registered(ts_config_module_t module)
{
    if (module >= TS_CONFIG_MODULE_MAX) {
        return false;
    }
    return s_mgr.modules[module].registered;
}

const char *ts_config_module_get_name(ts_config_module_t module)
{
    if (module >= TS_CONFIG_MODULE_MAX) {
        return "UNKNOWN";
    }
    return MODULE_NAMES[module];
}

/* ============================================================================
 * 配置加载
 * ========================================================================== */

esp_err_t ts_config_module_load(ts_config_module_t module)
{
    if (module == TS_CONFIG_MODULE_MAX) {
        /* 加载所有已注册模块 */
        for (int i = 0; i < TS_CONFIG_MODULE_MAX; i++) {
            if (s_mgr.modules[i].registered) {
                esp_err_t ret = ts_config_module_load((ts_config_module_t)i);
                if (ret != ESP_OK && ret != TS_CONFIG_ERR_MODULE_NOT_FOUND) {
                    ESP_LOGE(TAG, "Failed to load module %s: %s",
                             MODULE_NAMES[i], esp_err_to_name(ret));
                    return ret;
                }
            }
        }
        return ESP_OK;
    }
    
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return TS_CONFIG_ERR_MODULE_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Loading module %s...", MODULE_NAMES[module]);
    
    xSemaphoreTake(s_mgr.mutex, portMAX_DELAY);
    
    esp_err_t ret = ts_config_module_load_from_nvs(module);
    if (ret == TS_CONFIG_ERR_NOT_FOUND)
        ret = ts_config_module_reset(module, false);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_mgr.mutex);
        return ret;
    }

    /* Schema版本迁移 */
    const ts_config_module_schema_t *schema = s_mgr.modules[module].schema;
    if (schema != NULL && schema->migrate != NULL) {
        uint16_t stored_version = ts_config_meta_get_schema_version(module);
        if (stored_version < schema->version) {
            ESP_LOGI(TAG, "Migrating module %s from v%d to v%d", 
                     MODULE_NAMES[module], stored_version, schema->version);
            esp_err_t migrate_ret = schema->migrate(stored_version);
            if (migrate_ret == ESP_OK) {
                ts_config_meta_set_schema_version(module, schema->version);
                s_mgr.modules[module].loaded_version = schema->version;
            } else {
                ESP_LOGW(TAG, "Migration failed for %s: %s", 
                         MODULE_NAMES[module], esp_err_to_name(migrate_ret));
            }
        }
    }
    
    s_mgr.modules[module].dirty = false;
    xSemaphoreGive(s_mgr.mutex);
    
    return ret;
}

esp_err_t ts_config_module_load_from_sdcard(ts_config_module_t module)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return TS_CONFIG_ERR_MODULE_NOT_FOUND;
    }
    
    if (!is_sdcard_mounted()) {
        return TS_CONFIG_ERR_SD_NOT_MOUNTED;
    }
    
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", TS_CONFIG_SDCARD_PATH, MODULE_FILENAMES[module]);
    
    cJSON *root = NULL;
    esp_err_t ret = read_json_file(path, &root);
    if (ret != ESP_OK || root == NULL) {
        return ret == ESP_OK ? TS_CONFIG_ERR_NOT_FOUND : ret;
    }
    
    ret = json_to_module(module, root);
    cJSON_Delete(root);
    
    return ret;
}

esp_err_t ts_config_module_load_from_nvs(ts_config_module_t module)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return TS_CONFIG_ERR_MODULE_NOT_FOUND;
    }
    
    ts_config_module_info_t *info = &s_mgr.modules[module];
    
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(info->nvs_namespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "NVS namespace %s not found", info->nvs_namespace);
        return TS_CONFIG_ERR_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 使用 JSON blob 方式存储，键名固定为 "config" */
    size_t blob_len = 0;
    ret = nvs_get_blob(handle, "config", NULL, &blob_len);
    if (ret == ESP_OK && blob_len > 0) {
        char *json_str = TS_MOD_MALLOC(blob_len + 1);
        if (json_str != NULL) {
            ret = nvs_get_blob(handle, "config", json_str, &blob_len);
            if (ret == ESP_OK) {
                json_str[blob_len] = '\0';
                cJSON *root = cJSON_Parse(json_str);
                if (root != NULL) {
                    ret = json_to_module(module, root);
                    cJSON_Delete(root);
                    ESP_LOGD(TAG, "Module %s loaded from NVS blob (%s)", 
                             MODULE_NAMES[module], info->nvs_namespace);
                } else {
                    ESP_LOGW(TAG, "NVS blob parse failed for %s", MODULE_NAMES[module]);
                    ret = TS_CONFIG_ERR_PARSE_FAILED;
                }
            }
            free(json_str);
        } else {
            ret = ESP_ERR_NO_MEM;
        }
        nvs_close(handle);
        return ret;
    }
    
    /* 如果没有 blob，尝试兼容旧的单键存储方式 */
    const ts_config_module_schema_t *schema = info->schema;
    if (schema == NULL || schema->entries == NULL) {
        nvs_close(handle);
        return ESP_OK;
    }
    
    char cache_key[64];
    bool has_legacy_data = false;
    
    /* 遍历Schema条目，从NVS加载（兼容旧格式，键名需≤15字符） */
    for (size_t i = 0; i < schema->entry_count; i++) {
        const ts_config_schema_entry_t *entry = &schema->entries[i];
        
        /* 跳过超长键名（旧格式不支持） */
        if (strlen(entry->key) > 15) {
            continue;
        }
        
        make_cache_key(module, entry->key, cache_key, sizeof(cache_key));
        
        switch (entry->type) {
            case TS_CONFIG_TYPE_BOOL: {
                uint8_t val;
                if (nvs_get_u8(handle, entry->key, &val) == ESP_OK) {
                    ts_config_set_bool(cache_key, val != 0);
                    has_legacy_data = true;
                }
                break;
            }
            case TS_CONFIG_TYPE_INT32: {
                int32_t val;
                if (nvs_get_i32(handle, entry->key, &val) == ESP_OK) {
                    ts_config_set_int32(cache_key, val);
                    has_legacy_data = true;
                }
                break;
            }
            case TS_CONFIG_TYPE_UINT32: {
                uint32_t val;
                if (nvs_get_u32(handle, entry->key, &val) == ESP_OK) {
                    ts_config_set_uint32(cache_key, val);
                    has_legacy_data = true;
                }
                break;
            }
            case TS_CONFIG_TYPE_STRING: {
                size_t len = 0;
                if (nvs_get_str(handle, entry->key, NULL, &len) == ESP_OK && len > 0) {
                    char *buf = TS_MOD_MALLOC(len);
                    if (buf && nvs_get_str(handle, entry->key, buf, &len) == ESP_OK) {
                        ts_config_set_string(cache_key, buf);
                        has_legacy_data = true;
                    }
                    free(buf);
                }
                break;
            }
            case TS_CONFIG_TYPE_FLOAT: {
                /* NVS不直接支持float，存为blob */
                float val;
                size_t len = sizeof(val);
                if (nvs_get_blob(handle, entry->key, &val, &len) == ESP_OK) {
                    ts_config_set_float(cache_key, val);
                    has_legacy_data = true;
                }
                break;
            }
            default:
                break;
        }
    }
    
    nvs_close(handle);
    
    if (has_legacy_data) {
        ESP_LOGI(TAG, "Module %s loaded from NVS (legacy format), will migrate on next persist", 
                 MODULE_NAMES[module]);
    } else {
        return TS_CONFIG_ERR_NOT_FOUND;
    }
    
    return ESP_OK;
}

/* ============================================================================
 * 配置读取
 * ========================================================================== */

esp_err_t ts_config_module_get_bool(ts_config_module_t module, const char *key, bool *value)
{
    if (module >= TS_CONFIG_MODULE_MAX || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    /* 先查找Schema默认值 */
    bool default_val = false;
    const ts_config_schema_entry_t *entry = find_schema_entry(module, key);
    if (entry && entry->type == TS_CONFIG_TYPE_BOOL) {
        default_val = entry->default_bool;
    }
    
    return ts_config_get_bool(cache_key, value, default_val);
}

esp_err_t ts_config_module_get_int(ts_config_module_t module, const char *key, int32_t *value)
{
    if (module >= TS_CONFIG_MODULE_MAX || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    /* 先查找Schema默认值 */
    int32_t default_val = 0;
    const ts_config_schema_entry_t *entry = find_schema_entry(module, key);
    if (entry && entry->type == TS_CONFIG_TYPE_INT32) {
        default_val = entry->default_int32;
    }
    
    return ts_config_get_int32(cache_key, value, default_val);
}

esp_err_t ts_config_module_get_uint(ts_config_module_t module, const char *key, uint32_t *value)
{
    if (module >= TS_CONFIG_MODULE_MAX || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    /* 先查找Schema默认值 */
    uint32_t default_val = 0;
    const ts_config_schema_entry_t *entry = find_schema_entry(module, key);
    if (entry && entry->type == TS_CONFIG_TYPE_UINT32) {
        default_val = entry->default_uint32;
    }
    
    return ts_config_get_uint32(cache_key, value, default_val);
}

esp_err_t ts_config_module_get_string(ts_config_module_t module, const char *key, 
                                       char *buf, size_t len)
{
    if (module >= TS_CONFIG_MODULE_MAX || key == NULL || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    /* 先查找Schema默认值 */
    const char *default_val = NULL;
    const ts_config_schema_entry_t *entry = find_schema_entry(module, key);
    if (entry && entry->type == TS_CONFIG_TYPE_STRING) {
        default_val = entry->default_str;
    }
    
    return ts_config_get_string(cache_key, buf, len, default_val);
}

esp_err_t ts_config_module_get_float(ts_config_module_t module, const char *key, float *value)
{
    if (module >= TS_CONFIG_MODULE_MAX || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    /* 先查找Schema默认值 */
    float default_val = 0.0f;
    const ts_config_schema_entry_t *entry = find_schema_entry(module, key);
    if (entry && entry->type == TS_CONFIG_TYPE_FLOAT) {
        default_val = entry->default_float;
    }
    
    return ts_config_get_float(cache_key, value, default_val);
}

/* ============================================================================
 * 配置写入（临时）
 * ========================================================================== */

esp_err_t ts_config_module_set_bool(ts_config_module_t module, const char *key, bool value)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    esp_err_t ret = ts_config_set_bool(cache_key, value);
    if (ret == ESP_OK) {
        s_mgr.modules[module].dirty = true;
    }
    return ret;
}

esp_err_t ts_config_module_set_int(ts_config_module_t module, const char *key, int32_t value)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    esp_err_t ret = ts_config_set_int32(cache_key, value);
    if (ret == ESP_OK) {
        s_mgr.modules[module].dirty = true;
    }
    return ret;
}

esp_err_t ts_config_module_set_uint(ts_config_module_t module, const char *key, uint32_t value)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    esp_err_t ret = ts_config_set_uint32(cache_key, value);
    if (ret == ESP_OK) {
        s_mgr.modules[module].dirty = true;
    }
    return ret;
}

esp_err_t ts_config_module_set_string(ts_config_module_t module, const char *key, 
                                       const char *value)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered || 
        key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    esp_err_t ret = ts_config_set_string(cache_key, value);
    if (ret == ESP_OK) {
        s_mgr.modules[module].dirty = true;
    }
    return ret;
}

esp_err_t ts_config_module_set_float(ts_config_module_t module, const char *key, float value)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char cache_key[64];
    make_cache_key(module, key, cache_key, sizeof(cache_key));
    
    esp_err_t ret = ts_config_set_float(cache_key, value);
    if (ret == ESP_OK) {
        s_mgr.modules[module].dirty = true;
    }
    return ret;
}

/* ============================================================================
 * 持久化
 * ========================================================================== */

esp_err_t ts_config_module_persist(ts_config_module_t module)
{
    if (module == TS_CONFIG_MODULE_MAX) {
        /* 持久化所有已注册且有更改的模块 */
        for (int i = 0; i < TS_CONFIG_MODULE_MAX; i++) {
            if (s_mgr.modules[i].registered && s_mgr.modules[i].dirty) {
                ts_config_module_persist((ts_config_module_t)i);
            }
        }
        return ESP_OK;
    }
    
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return TS_CONFIG_ERR_MODULE_NOT_FOUND;
    }
    
    xSemaphoreTake(s_mgr.mutex, portMAX_DELAY);
    
    ts_config_module_info_t *info = &s_mgr.modules[module];
    esp_err_t ret = ESP_OK;
    
    /* 1. 递增全局序列号 */
    uint32_t seq = ts_config_meta_increment_global_seq();
    if (!seq) { xSemaphoreGive(s_mgr.mutex); return ESP_FAIL; }
    info->seq = seq;
    
    /* 2. 生成 JSON */
    cJSON *root = NULL;
    ret = module_to_json(module, &root);
    if (ret != ESP_OK || root == NULL) {
        ESP_LOGE(TAG, "Failed to generate JSON for %s", MODULE_NAMES[module]);
        xSemaphoreGive(s_mgr.mutex);
        return ret;
    }
    
    /* 序列化为字符串 */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON for %s", MODULE_NAMES[module]);
        xSemaphoreGive(s_mgr.mutex);
        return ESP_ERR_NO_MEM;
    }
    
    size_t json_len = strlen(json_str);
    
    /* 3. 写入NVS（使用 blob 存储整个 JSON） */
    nvs_handle_t handle;
    ret = nvs_open(info->nvs_namespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS %s: %s", info->nvs_namespace, esp_err_to_name(ret));
        free(json_str);
        xSemaphoreGive(s_mgr.mutex);
        return ret;
    }
    
    ret = nvs_set_blob(handle, "config", json_str, json_len);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    free(json_str);
    if (ret == ESP_OK) info->dirty = false;
    xSemaphoreGive(s_mgr.mutex);
    return ret;
}

esp_err_t ts_config_module_export_to_sdcard(ts_config_module_t module)
{
    if (module == TS_CONFIG_MODULE_MAX) {
        for (int i = 0; i < TS_CONFIG_MODULE_MAX; i++) {
            if (s_mgr.modules[i].registered) {
                ts_config_module_export_to_sdcard((ts_config_module_t)i);
            }
        }
        return ESP_OK;
    }
    
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return TS_CONFIG_ERR_MODULE_NOT_FOUND;
    }
    
    if (!is_sdcard_mounted()) {
        return TS_CONFIG_ERR_SD_NOT_MOUNTED;
    }
    
    /* 确保目录存在 */
    esp_err_t ret = ensure_config_dir();
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 生成JSON */
    cJSON *root = NULL;
    ret = module_to_json(module, &root);
    if (ret != ESP_OK || root == NULL) {
        return ret;
    }
    
    /* 写入文件 */
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", TS_CONFIG_SDCARD_PATH, MODULE_FILENAMES[module]);
    
    ret = write_json_file(path, root);
    cJSON_Delete(root);
    
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Module %s exported to %s", MODULE_NAMES[module], path);
    }
    
    return ret;
}

esp_err_t ts_config_module_export_custom_json(ts_config_module_t module, const cJSON *json)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return TS_CONFIG_ERR_MODULE_NOT_FOUND;
    }
    
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!is_sdcard_mounted()) {
        return TS_CONFIG_ERR_SD_NOT_MOUNTED;
    }
    
    /* 确保目录存在 */
    esp_err_t ret = ensure_config_dir();
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 构建文件路径 */
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", TS_CONFIG_SDCARD_PATH, MODULE_FILENAMES[module]);
    
    /* 写入文件（传入的是 const cJSON*，write_json_file 需要非 const，需要复制） */
    cJSON *json_copy = cJSON_Duplicate(json, true);
    if (json_copy == NULL) {
        ESP_LOGE(TAG, "Failed to duplicate JSON for %s", MODULE_NAMES[module]);
        return ESP_ERR_NO_MEM;
    }
    
    ret = write_json_file(path, json_copy);
    cJSON_Delete(json_copy);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Module %s custom JSON exported to %s", MODULE_NAMES[module], path);
    } else {
        ESP_LOGW(TAG, "Failed to export %s to SD card: %s", MODULE_NAMES[module], esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t ts_config_module_import_from_sdcard(ts_config_module_t module)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return TS_CONFIG_ERR_MODULE_NOT_FOUND;
    }
    
    /* 加载SD卡配置 */
    esp_err_t ret = ts_config_module_load_from_sdcard(module);
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 持久化到NVS */
    return ts_config_module_persist(module);
}

/* ============================================================================
 * 重置
 * ========================================================================== */

esp_err_t ts_config_module_reset(ts_config_module_t module, bool persist)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return TS_CONFIG_ERR_MODULE_NOT_FOUND;
    }
    
    ts_config_module_info_t *info = &s_mgr.modules[module];
    const ts_config_module_schema_t *schema = info->schema;
    
    if (schema == NULL || schema->entries == NULL) {
        return ESP_OK;
    }
    
    char cache_key[64];
    
    /* 应用Schema默认值 */
    for (size_t i = 0; i < schema->entry_count; i++) {
        const ts_config_schema_entry_t *entry = &schema->entries[i];
        make_cache_key(module, entry->key, cache_key, sizeof(cache_key));
        
        switch (entry->type) {
            case TS_CONFIG_TYPE_BOOL:
                ts_config_set_bool(cache_key, entry->default_bool);
                break;
            case TS_CONFIG_TYPE_INT32:
                ts_config_set_int32(cache_key, entry->default_int32);
                break;
            case TS_CONFIG_TYPE_UINT32:
                ts_config_set_uint32(cache_key, entry->default_uint32);
                break;
            case TS_CONFIG_TYPE_STRING:
                if (entry->default_str) {
                    ts_config_set_string(cache_key, entry->default_str);
                }
                break;
            case TS_CONFIG_TYPE_FLOAT:
                ts_config_set_float(cache_key, entry->default_float);
                break;
            default:
                break;
        }
    }
    
    info->dirty = true;
    
    if (persist) {
        /* 清除NVS（只删除 config blob） */
        nvs_handle_t handle;
        if (nvs_open(info->nvs_namespace, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_key(handle, "config");
            nvs_commit(handle);
            nvs_close(handle);
        }
        
        /* 删除SD卡配置文件 */
        if (is_sdcard_mounted()) {
            char path[64];
            snprintf(path, sizeof(path), "%s/%s", TS_CONFIG_SDCARD_PATH, MODULE_FILENAMES[module]);
            unlink(path);
        }
        
        /* 持久化默认值 */
        return ts_config_module_persist(module);
    }
    
    return ESP_OK;
}

/* ============================================================================
 * 查询
 * ========================================================================== */

uint16_t ts_config_module_get_schema_version(ts_config_module_t module)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return 0;
    }
    const ts_config_module_schema_t *schema = s_mgr.modules[module].schema;
    return schema ? schema->version : 0;
}

uint32_t ts_config_module_get_global_seq(void)
{
    return ts_config_meta_get_global_seq();
}

bool ts_config_module_is_dirty(ts_config_module_t module)
{
    if (module >= TS_CONFIG_MODULE_MAX) {
        return false;
    }
    return s_mgr.modules[module].dirty;
}

const char *ts_config_module_get_nvs_namespace(ts_config_module_t module)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return NULL;
    }
    return s_mgr.modules[module].nvs_namespace;
}

esp_err_t ts_config_module_get_sdcard_path(ts_config_module_t module, char *path, size_t len)
{
    if (module >= TS_CONFIG_MODULE_MAX || path == NULL || len < 32) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(path, len, "%s/%s", TS_CONFIG_SDCARD_PATH, MODULE_FILENAMES[module]);
    return ESP_OK;
}

/* ============================================================================
 * 辅助函数实现
 * ========================================================================== */

static bool is_sdcard_mounted(void)
{
    /* 检查SD卡挂载点是否存在 */
    struct stat st;
    return (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode));
}

static esp_err_t ensure_config_dir(void)
{
    struct stat st;
    if (stat(TS_CONFIG_SDCARD_PATH, &st) != 0) {
        /* 目录不存在，创建 */
        if (mkdir(TS_CONFIG_SDCARD_PATH, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create config dir: %s", TS_CONFIG_SDCARD_PATH);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t read_json_file(const char *path, cJSON **root)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return TS_CONFIG_ERR_NOT_FOUND;
    }
    
    /* 获取文件大小 */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 65536) {
        fclose(f);
        return TS_CONFIG_ERR_PARSE_FAILED;
    }
    
    /* 读取内容（优先使用 PSRAM）*/
    char *buf = TS_MOD_MALLOC(size + 1);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    if (fread(buf, 1, size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return ESP_FAIL;
    }
    buf[size] = '\0';
    fclose(f);
    
    /* 解析JSON */
    *root = cJSON_Parse(buf);
    free(buf);
    
    if (*root == NULL) {
        ESP_LOGE(TAG, "JSON parse failed: %s", path);
        return TS_CONFIG_ERR_PARSE_FAILED;
    }
    
    return ESP_OK;
}

static esp_err_t write_json_file(const char *path, cJSON *root)
{
    char *str = cJSON_Print(root);
    if (str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        free(str);
        ESP_LOGE(TAG, "Failed to open for write: %s", path);
        return ESP_FAIL;
    }
    
    size_t len = strlen(str);
    size_t written = fwrite(str, 1, len, f);
    fclose(f);
    free(str);
    
    return (written == len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t module_to_json(ts_config_module_t module, cJSON **root)
{
    *root = cJSON_CreateObject();
    if (*root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    ts_config_module_info_t *info = &s_mgr.modules[module];
    const ts_config_module_schema_t *schema = info->schema;
    
    /* 添加元数据 */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, JSON_KEY_SEQ, info->seq);
    cJSON_AddNumberToObject(meta, JSON_KEY_VERSION, schema ? schema->version : 1);
    cJSON_AddItemToObject(*root, JSON_KEY_META, meta);
    
    if (schema == NULL || schema->entries == NULL) {
        return ESP_OK;
    }
    
    char cache_key[64];
    
    /* 导出所有Schema条目 */
    for (size_t i = 0; i < schema->entry_count; i++) {
        const ts_config_schema_entry_t *entry = &schema->entries[i];
        make_cache_key(module, entry->key, cache_key, sizeof(cache_key));
        
        switch (entry->type) {
            case TS_CONFIG_TYPE_BOOL: {
                bool val;
                ts_config_get_bool(cache_key, &val, entry->default_bool);
                cJSON_AddBoolToObject(*root, entry->key, val);
                break;
            }
            case TS_CONFIG_TYPE_INT32: {
                int32_t val;
                ts_config_get_int32(cache_key, &val, entry->default_int32);
                cJSON_AddNumberToObject(*root, entry->key, val);
                break;
            }
            case TS_CONFIG_TYPE_UINT32: {
                uint32_t val;
                ts_config_get_uint32(cache_key, &val, entry->default_uint32);
                cJSON_AddNumberToObject(*root, entry->key, val);
                break;
            }
            case TS_CONFIG_TYPE_STRING: {
                char buf[256];
                ts_config_get_string(cache_key, buf, sizeof(buf), entry->default_str);
                if (buf[0] != '\0') {
                    cJSON_AddStringToObject(*root, entry->key, buf);
                } else if (entry->default_str) {
                    cJSON_AddStringToObject(*root, entry->key, entry->default_str);
                }
                break;
            }
            case TS_CONFIG_TYPE_FLOAT: {
                float val;
                ts_config_get_float(cache_key, &val, entry->default_float);
                cJSON_AddNumberToObject(*root, entry->key, val);
                break;
            }
            default:
                break;
        }
    }
    
    return ESP_OK;
}

static esp_err_t json_to_module(ts_config_module_t module, cJSON *root)
{
    ts_config_module_info_t *info = &s_mgr.modules[module];
    const ts_config_module_schema_t *schema = info->schema;
    
    /* 读取元数据 */
    cJSON *meta = cJSON_GetObjectItem(root, JSON_KEY_META);
    if (meta) {
        cJSON *seq_item = cJSON_GetObjectItem(meta, JSON_KEY_SEQ);
        if (cJSON_IsNumber(seq_item)) {
            info->seq = (uint32_t)seq_item->valuedouble;
        }
        cJSON *ver_item = cJSON_GetObjectItem(meta, JSON_KEY_VERSION);
        if (cJSON_IsNumber(ver_item)) {
            info->loaded_version = (uint16_t)ver_item->valuedouble;
        }
    }
    
    if (schema == NULL || schema->entries == NULL) {
        return ESP_OK;
    }
    
    char cache_key[64];
    
    /* 导入所有Schema条目 */
    for (size_t i = 0; i < schema->entry_count; i++) {
        const ts_config_schema_entry_t *entry = &schema->entries[i];
        cJSON *item = cJSON_GetObjectItem(root, entry->key);
        make_cache_key(module, entry->key, cache_key, sizeof(cache_key));
        
        if (item == NULL) {
            /* 使用默认值 */
            switch (entry->type) {
                case TS_CONFIG_TYPE_BOOL:
                    ts_config_set_bool(cache_key, entry->default_bool);
                    break;
                case TS_CONFIG_TYPE_INT32:
                    ts_config_set_int32(cache_key, entry->default_int32);
                    break;
                case TS_CONFIG_TYPE_UINT32:
                    ts_config_set_uint32(cache_key, entry->default_uint32);
                    break;
                case TS_CONFIG_TYPE_STRING:
                    if (entry->default_str) {
                        ts_config_set_string(cache_key, entry->default_str);
                    }
                    break;
                case TS_CONFIG_TYPE_FLOAT:
                    ts_config_set_float(cache_key, entry->default_float);
                    break;
                default:
                    break;
            }
            continue;
        }
        
        /* 从JSON读取值 */
        switch (entry->type) {
            case TS_CONFIG_TYPE_BOOL:
                if (cJSON_IsBool(item)) {
                    ts_config_set_bool(cache_key, cJSON_IsTrue(item));
                }
                break;
            case TS_CONFIG_TYPE_INT32:
                if (cJSON_IsNumber(item)) {
                    ts_config_set_int32(cache_key, (int32_t)item->valuedouble);
                }
                break;
            case TS_CONFIG_TYPE_UINT32:
                if (cJSON_IsNumber(item)) {
                    ts_config_set_uint32(cache_key, (uint32_t)item->valuedouble);
                }
                break;
            case TS_CONFIG_TYPE_STRING:
                if (cJSON_IsString(item) && item->valuestring) {
                    ts_config_set_string(cache_key, item->valuestring);
                }
                break;
            case TS_CONFIG_TYPE_FLOAT:
                if (cJSON_IsNumber(item)) {
                    ts_config_set_float(cache_key, (float)item->valuedouble);
                }
                break;
            default:
                break;
        }
    }
    
    return ESP_OK;
}

static char *make_cache_key(ts_config_module_t module, const char *key, char *buf, size_t len)
{
    /* 格式: "{module_name}.{key}" 如 "net.eth.ip" */
    const char *name = MODULE_NAMES[module];
    
    /* 转小写 */
    char lower_name[16];
    size_t i;
    for (i = 0; name[i] && i < sizeof(lower_name) - 1; i++) {
        lower_name[i] = (name[i] >= 'A' && name[i] <= 'Z') ? 
                        (name[i] + 32) : name[i];
    }
    lower_name[i] = '\0';
    
    snprintf(buf, len, "%s.%s", lower_name, key);
    return buf;
}

static const ts_config_schema_entry_t *find_schema_entry(ts_config_module_t module, const char *key)
{
    if (module >= TS_CONFIG_MODULE_MAX || !s_mgr.modules[module].registered) {
        return NULL;
    }
    
    const ts_config_module_schema_t *schema = s_mgr.modules[module].schema;
    if (schema == NULL || schema->entries == NULL) {
        return NULL;
    }
    
    for (size_t i = 0; i < schema->entry_count; i++) {
        if (strcmp(schema->entries[i].key, key) == 0) {
            return &schema->entries[i];
        }
    }
    
    return NULL;
}
