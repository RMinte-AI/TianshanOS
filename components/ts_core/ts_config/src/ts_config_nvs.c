/**
 * @file ts_config_nvs.c
 * @brief TianShanOS Configuration - NVS Backend Implementation
 *
 * NVS (Non-Volatile Storage) 配置后端实现
 * 提供基于 ESP-IDF NVS 的持久化配置存储
 *
 * @author TianShanOS Team
 * @version 0.1.0
 */

#include <string.h>
#include "ts_config.h"
#include "ts_config_nvs.h"
#include "ts_config_partitions.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

/* PSRAM 优先分配宏 */
#define TS_NVS_MALLOC(size) ({ void *p = heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); p ? p : malloc(size); })

static const char *TAG = "ts_config_nvs";

/* ============================================================================
 * 私有变量
 * ========================================================================== */

#ifndef CONFIG_TS_CONFIG_NVS_NAMESPACE
#define CONFIG_TS_CONFIG_NVS_NAMESPACE "ts_config"
#endif

static nvs_handle_t s_nvs_handle = 0;
static bool s_nvs_initialized = false;

/* ============================================================================
 * 后端操作函数
 * ========================================================================== */

/**
 * @brief 初始化 NVS 后端
 */
static esp_err_t nvs_backend_init(void)
{
    if (s_nvs_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing NVS configuration backend...");

    esp_err_t ret = ts_config_partitions_init();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %s", esp_err_to_name(ret));
        return ret;
    }

    // 打开 NVS 命名空间
    ret = nvs_open(CONFIG_TS_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", 
                 CONFIG_TS_CONFIG_NVS_NAMESPACE, esp_err_to_name(ret));
        return ret;
    }

    s_nvs_initialized = true;
    ESP_LOGI(TAG, "NVS backend initialized (namespace: %s)", CONFIG_TS_CONFIG_NVS_NAMESPACE);
    return ESP_OK;
}

/**
 * @brief 反初始化 NVS 后端
 */
static esp_err_t nvs_backend_deinit(void)
{
    if (!s_nvs_initialized) {
        return ESP_OK;
    }

    nvs_close(s_nvs_handle);
    s_nvs_handle = 0;
    s_nvs_initialized = false;

    ESP_LOGI(TAG, "NVS backend deinitialized");
    return ESP_OK;
}

/**
 * @brief 从 NVS 读取配置
 */
static esp_err_t nvs_backend_get(const char *key, ts_config_type_t type,
                                  ts_config_value_t *value, size_t *size)
{
    if (!s_nvs_initialized || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;

    switch (type) {
        case TS_CONFIG_TYPE_BOOL: {
            uint8_t val;
            ret = nvs_get_u8(s_nvs_handle, key, &val);
            if (ret == ESP_OK) {
                value->val_bool = (val != 0);
            }
            break;
        }
        case TS_CONFIG_TYPE_INT8: {
            ret = nvs_get_i8(s_nvs_handle, key, &value->val_i8);
            break;
        }
        case TS_CONFIG_TYPE_UINT8: {
            ret = nvs_get_u8(s_nvs_handle, key, &value->val_u8);
            break;
        }
        case TS_CONFIG_TYPE_INT16: {
            ret = nvs_get_i16(s_nvs_handle, key, &value->val_i16);
            break;
        }
        case TS_CONFIG_TYPE_UINT16: {
            ret = nvs_get_u16(s_nvs_handle, key, &value->val_u16);
            break;
        }
        case TS_CONFIG_TYPE_INT32: {
            ret = nvs_get_i32(s_nvs_handle, key, &value->val_i32);
            break;
        }
        case TS_CONFIG_TYPE_UINT32: {
            ret = nvs_get_u32(s_nvs_handle, key, &value->val_u32);
            break;
        }
        case TS_CONFIG_TYPE_INT64: {
            ret = nvs_get_i64(s_nvs_handle, key, &value->val_i64);
            break;
        }
        case TS_CONFIG_TYPE_UINT64: {
            ret = nvs_get_u64(s_nvs_handle, key, &value->val_u64);
            break;
        }
        case TS_CONFIG_TYPE_FLOAT: {
            // NVS 不直接支持 float，使用 blob
            size_t len = sizeof(float);
            ret = nvs_get_blob(s_nvs_handle, key, &value->val_float, &len);
            break;
        }
        case TS_CONFIG_TYPE_DOUBLE: {
            // NVS 不直接支持 double，使用 blob
            size_t len = sizeof(double);
            ret = nvs_get_blob(s_nvs_handle, key, &value->val_double, &len);
            break;
        }
        case TS_CONFIG_TYPE_STRING: {
            // 先获取长度
            size_t len = 0;
            ret = nvs_get_str(s_nvs_handle, key, NULL, &len);
            if (ret == ESP_OK && len > 0) {
                if (size != NULL && *size > 0 && *size < len) {
                    *size = len;
                    return ESP_ERR_INVALID_SIZE;
                }
                value->val_string = TS_NVS_MALLOC(len);
                if (value->val_string == NULL) {
                    return ESP_ERR_NO_MEM;
                }
                ret = nvs_get_str(s_nvs_handle, key, value->val_string, &len);
                if (size != NULL) {
                    *size = len;
                }
            }
            break;
        }
        case TS_CONFIG_TYPE_BLOB: {
            // 先获取长度
            size_t len = 0;
            ret = nvs_get_blob(s_nvs_handle, key, NULL, &len);
            if (ret == ESP_OK && len > 0) {
                if (size != NULL && *size > 0 && *size < len) {
                    *size = len;
                    return ESP_ERR_INVALID_SIZE;
                }
                value->val_blob.data = TS_NVS_MALLOC(len);
                if (value->val_blob.data == NULL) {
                    return ESP_ERR_NO_MEM;
                }
                ret = nvs_get_blob(s_nvs_handle, key, value->val_blob.data, &len);
                value->val_blob.size = len;
                if (size != NULL) {
                    *size = len;
                }
            }
            break;
        }
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return ret;
}

/**
 * @brief 写入配置到 NVS
 */
static esp_err_t nvs_backend_set(const char *key, ts_config_type_t type,
                                  const ts_config_value_t *value, size_t size)
{
    if (!s_nvs_initialized || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // NVS 键名长度限制为 15 字符
    // 超长键名跳过存储（这些键通常属于模块化配置系统，由 ts_config_module 以 JSON blob 方式单独存储）
    if (strlen(key) > 15) {
        ESP_LOGD(TAG, "Skipping long key '%s' (handled by module system)", key);
        return ESP_OK;  // 返回成功，避免上层报错
    }

    esp_err_t ret = ESP_OK;

    switch (type) {
        case TS_CONFIG_TYPE_BOOL:
            ret = nvs_set_u8(s_nvs_handle, key, value->val_bool ? 1 : 0);
            break;
        case TS_CONFIG_TYPE_INT8:
            ret = nvs_set_i8(s_nvs_handle, key, value->val_i8);
            break;
        case TS_CONFIG_TYPE_UINT8:
            ret = nvs_set_u8(s_nvs_handle, key, value->val_u8);
            break;
        case TS_CONFIG_TYPE_INT16:
            ret = nvs_set_i16(s_nvs_handle, key, value->val_i16);
            break;
        case TS_CONFIG_TYPE_UINT16:
            ret = nvs_set_u16(s_nvs_handle, key, value->val_u16);
            break;
        case TS_CONFIG_TYPE_INT32:
            ret = nvs_set_i32(s_nvs_handle, key, value->val_i32);
            break;
        case TS_CONFIG_TYPE_UINT32:
            ret = nvs_set_u32(s_nvs_handle, key, value->val_u32);
            break;
        case TS_CONFIG_TYPE_INT64:
            ret = nvs_set_i64(s_nvs_handle, key, value->val_i64);
            break;
        case TS_CONFIG_TYPE_UINT64:
            ret = nvs_set_u64(s_nvs_handle, key, value->val_u64);
            break;
        case TS_CONFIG_TYPE_FLOAT:
            ret = nvs_set_blob(s_nvs_handle, key, &value->val_float, sizeof(float));
            break;
        case TS_CONFIG_TYPE_DOUBLE:
            ret = nvs_set_blob(s_nvs_handle, key, &value->val_double, sizeof(double));
            break;
        case TS_CONFIG_TYPE_STRING:
            if (value->val_string != NULL) {
                ret = nvs_set_str(s_nvs_handle, key, value->val_string);
            } else {
                ret = ESP_ERR_INVALID_ARG;
            }
            break;
        case TS_CONFIG_TYPE_BLOB:
            if (value->val_blob.data != NULL && value->val_blob.size > 0) {
                ret = nvs_set_blob(s_nvs_handle, key, value->val_blob.data, value->val_blob.size);
            } else {
                ret = ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set NVS key '%s': %s", key, esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief 从 NVS 删除配置
 */
static esp_err_t nvs_backend_erase(const char *key)
{
    if (!s_nvs_initialized || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = nvs_erase_key(s_nvs_handle, key);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    return ret;
}

/**
 * @brief 检查 NVS 中是否存在配置
 */
static esp_err_t nvs_backend_exists(const char *key, bool *exists)
{
    if (!s_nvs_initialized || key == NULL || exists == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 尝试获取字符串长度来检查是否存在
    size_t len = 0;
    esp_err_t ret = nvs_get_str(s_nvs_handle, key, NULL, &len);
    
    if (ret == ESP_OK || ret == ESP_ERR_NVS_INVALID_LENGTH) {
        *exists = true;
        return ESP_OK;
    }
    
    // 也检查 blob
    ret = nvs_get_blob(s_nvs_handle, key, NULL, &len);
    if (ret == ESP_OK || ret == ESP_ERR_NVS_INVALID_LENGTH) {
        *exists = true;
        return ESP_OK;
    }

    // 检查整数类型
    int32_t dummy;
    ret = nvs_get_i32(s_nvs_handle, key, &dummy);
    if (ret == ESP_OK) {
        *exists = true;
        return ESP_OK;
    }

    *exists = false;
    return ESP_OK;
}

/**
 * @brief 清空 NVS 命名空间
 */
static esp_err_t nvs_backend_clear(void)
{
    if (!s_nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = nvs_erase_all(s_nvs_handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(s_nvs_handle);
    }
    
    ESP_LOGI(TAG, "NVS namespace cleared");
    return ret;
}

/**
 * @brief 提交 NVS 更改
 */
static esp_err_t nvs_backend_commit(void)
{
    if (!s_nvs_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return nvs_commit(s_nvs_handle);
}

/* ============================================================================
 * 后端操作函数集
 * ========================================================================== */

static const ts_config_backend_ops_t s_nvs_backend_ops = {
    .init = nvs_backend_init,
    .deinit = nvs_backend_deinit,
    .get = nvs_backend_get,
    .set = nvs_backend_set,
    .erase = nvs_backend_erase,
    .exists = nvs_backend_exists,
    .clear = nvs_backend_clear,
    .commit = nvs_backend_commit,
};

/* ============================================================================
 * 公共 API
 * ========================================================================== */

esp_err_t ts_config_nvs_register(void)
{
#ifdef CONFIG_TS_CONFIG_NVS_BACKEND
    uint8_t priority = CONFIG_TS_CONFIG_PRIORITY_NVS;
#else
    uint8_t priority = 80;
#endif

    return ts_config_register_backend(TS_CONFIG_BACKEND_NVS, &s_nvs_backend_ops, priority);
}

const ts_config_backend_ops_t *ts_config_nvs_get_ops(void)
{
    return &s_nvs_backend_ops;
}
