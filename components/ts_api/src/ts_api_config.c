/**
 * @file ts_api_config.c
 * @brief Configuration API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_api.h"
#include "ts_config.h"
#include "ts_config_module.h"
#include "ts_config_meta.h"
#include "ts_log.h"
#include <string.h>

#define TAG "api_config"

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief 解析模块名称
 */
static ts_config_module_t parse_module_name(const char *name)
{
    if (!name) return TS_CONFIG_MODULE_MAX + 1;
    if (strcasecmp(name, "net") == 0) return TS_CONFIG_MODULE_NET;
    if (strcasecmp(name, "dhcp") == 0) return TS_CONFIG_MODULE_DHCP;
    if (strcasecmp(name, "wifi") == 0) return TS_CONFIG_MODULE_WIFI;
    if (strcasecmp(name, "nat") == 0) return TS_CONFIG_MODULE_NAT;
    if (strcasecmp(name, "led") == 0) return TS_CONFIG_MODULE_LED;
    if (strcasecmp(name, "fan") == 0) return TS_CONFIG_MODULE_FAN;
    if (strcasecmp(name, "device") == 0) return TS_CONFIG_MODULE_DEVICE;
    if (strcasecmp(name, "system") == 0) return TS_CONFIG_MODULE_SYSTEM;
    if (strcasecmp(name, "all") == 0) return TS_CONFIG_MODULE_MAX;
    return TS_CONFIG_MODULE_MAX + 1;
}

static const char* module_to_name(ts_config_module_t mod)
{
    static const char *names[] = {"net", "dhcp", "wifi", "nat", "led", "fan", "device", "system"};
    if (mod < TS_CONFIG_MODULE_MAX) return names[mod];
    return "unknown";
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief config.get - Get configuration value
 */
static esp_err_t api_config_get(const cJSON *params, ts_api_result_t *result)
{
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *key = cJSON_GetObjectItem(params, "key");
    if (key == NULL || !cJSON_IsString(key)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'key' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "key", key->valuestring);
    
    /* Try to get as different types */
    int64_t int_val;
    bool bool_val;
    double dbl_val;
    char str_val[256];
    
    if (ts_config_get_int64(key->valuestring, &int_val, 0) == ESP_OK) {
        cJSON_AddNumberToObject(data, "value", (double)int_val);
        cJSON_AddStringToObject(data, "type", "int");
    } else if (ts_config_get_bool(key->valuestring, &bool_val, false) == ESP_OK) {
        cJSON_AddBoolToObject(data, "value", bool_val);
        cJSON_AddStringToObject(data, "type", "bool");
    } else if (ts_config_get_double(key->valuestring, &dbl_val, 0.0) == ESP_OK) {
        cJSON_AddNumberToObject(data, "value", dbl_val);
        cJSON_AddStringToObject(data, "type", "double");
    } else if (ts_config_get_string(key->valuestring, str_val, sizeof(str_val), NULL) == ESP_OK) {
        cJSON_AddStringToObject(data, "value", str_val);
        cJSON_AddStringToObject(data, "type", "string");
    } else {
        cJSON_Delete(data);
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Key not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.set - Set configuration value
 */
static esp_err_t api_config_set(const cJSON *params, ts_api_result_t *result)
{
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *key = cJSON_GetObjectItem(params, "key");
    const cJSON *value = cJSON_GetObjectItem(params, "value");
    
    if (key == NULL || !cJSON_IsString(key)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'key' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (value == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'value' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ESP_OK;
    
    if (cJSON_IsBool(value)) {
        ret = ts_config_set_bool(key->valuestring, cJSON_IsTrue(value));
    } else if (cJSON_IsNumber(value)) {
        /* Check if it's an integer or float */
        double d = value->valuedouble;
        if (d == (int64_t)d) {
            ret = ts_config_set_int64(key->valuestring, (int64_t)d);
        } else {
            ret = ts_config_set_double(key->valuestring, d);
        }
    } else if (cJSON_IsString(value)) {
        ret = ts_config_set_string(key->valuestring, value->valuestring);
    } else {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Unsupported value type");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set config");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "key", key->valuestring);
    cJSON_AddBoolToObject(data, "success", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.delete - Delete configuration value
 */
static esp_err_t api_config_delete(const cJSON *params, ts_api_result_t *result)
{
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *key = cJSON_GetObjectItem(params, "key");
    if (key == NULL || !cJSON_IsString(key)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'key' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_config_delete(key->valuestring);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Key not found or delete failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "key", key->valuestring);
    cJSON_AddBoolToObject(data, "deleted", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.list - List all configuration keys
 */
static esp_err_t api_config_list(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    cJSON *data = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(data, "items");
    
    /* Get config statistics */
    size_t total_count = 0, nvs_count = 0, file_count = 0;
    ts_config_get_stats(&total_count, &nvs_count, &file_count);
    cJSON_AddNumberToObject(data, "total_keys", (double)total_count);
    cJSON_AddNumberToObject(data, "nvs_keys", (double)nvs_count);
    cJSON_AddNumberToObject(data, "file_keys", (double)file_count);
    
    /* Note: Full iteration would require iterator API */
    /* For now just return stats */
    (void)items;
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.save - Save configuration to persistent storage
 */
static esp_err_t api_config_save(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    esp_err_t ret = ts_config_save();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to save config");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "saved", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Module API Handlers                               */
/*===========================================================================*/

/**
 * @brief config.module.list - 列出所有配置模块
 */
static esp_err_t api_config_module_list(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    cJSON *data = cJSON_CreateObject();
    cJSON *modules = cJSON_AddArrayToObject(data, "modules");
    
    for (int i = 0; i < TS_CONFIG_MODULE_MAX; i++) {
        ts_config_module_t mod = (ts_config_module_t)i;
        cJSON *item = cJSON_CreateObject();
        
        cJSON_AddStringToObject(item, "name", module_to_name(mod));
        cJSON_AddBoolToObject(item, "registered", ts_config_module_is_registered(mod));
        
        if (ts_config_module_is_registered(mod)) {
            const char *nvs_ns = ts_config_module_get_nvs_namespace(mod);
            cJSON_AddStringToObject(item, "nvs_namespace", nvs_ns ? nvs_ns : "");
            cJSON_AddNumberToObject(item, "version", ts_config_module_get_schema_version(mod));
            cJSON_AddBoolToObject(item, "dirty", ts_config_module_is_dirty(mod));
        }
        
        cJSON_AddItemToArray(modules, item);
    }
    
    /* 元数据 */
    cJSON_AddNumberToObject(data, "global_seq", (double)ts_config_meta_get_global_seq());
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief 添加模块配置到 JSON 对象
 */
static void add_module_config_to_json(ts_config_module_t mod, cJSON *config)
{
    bool bool_val;
    uint32_t uint_val;
    char str_val[256];
    
    switch (mod) {
        case TS_CONFIG_MODULE_NET:
            if (ts_config_module_get_bool(mod, "eth.enabled", &bool_val) == ESP_OK)
                cJSON_AddBoolToObject(config, "eth.enabled", bool_val);
            if (ts_config_module_get_bool(mod, "eth.dhcp", &bool_val) == ESP_OK)
                cJSON_AddBoolToObject(config, "eth.dhcp", bool_val);
            if (ts_config_module_get_string(mod, "eth.ip", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "eth.ip", str_val);
            if (ts_config_module_get_string(mod, "eth.netmask", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "eth.netmask", str_val);
            if (ts_config_module_get_string(mod, "eth.gateway", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "eth.gateway", str_val);
            if (ts_config_module_get_string(mod, "hostname", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "hostname", str_val);
            break;
            
        case TS_CONFIG_MODULE_DHCP:
            if (ts_config_module_get_bool(mod, "enabled", &bool_val) == ESP_OK)
                cJSON_AddBoolToObject(config, "enabled", bool_val);
            if (ts_config_module_get_string(mod, "start_ip", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "start_ip", str_val);
            if (ts_config_module_get_string(mod, "end_ip", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "end_ip", str_val);
            if (ts_config_module_get_uint(mod, "lease_time", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "lease_time", uint_val);
            break;
            
        case TS_CONFIG_MODULE_WIFI:
            if (ts_config_module_get_string(mod, "mode", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "mode", str_val);
            if (ts_config_module_get_string(mod, "ap.ssid", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "ap.ssid", str_val);
            if (ts_config_module_get_string(mod, "ap.password", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "ap.password", str_val);
            if (ts_config_module_get_uint(mod, "ap.channel", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "ap.channel", uint_val);
            if (ts_config_module_get_uint(mod, "ap.max_conn", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "ap.max_conn", uint_val);
            if (ts_config_module_get_bool(mod, "ap.hidden", &bool_val) == ESP_OK)
                cJSON_AddBoolToObject(config, "ap.hidden", bool_val);
            break;
            
        case TS_CONFIG_MODULE_LED:
            if (ts_config_module_get_uint(mod, "brightness", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "brightness", uint_val);
            if (ts_config_module_get_uint(mod, "effect_speed", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "effect_speed", uint_val);
            if (ts_config_module_get_string(mod, "power_on_effect", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "power_on_effect", str_val);
            if (ts_config_module_get_string(mod, "idle_effect", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "idle_effect", str_val);
            break;
            
        case TS_CONFIG_MODULE_FAN:
            if (ts_config_module_get_string(mod, "mode", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "mode", str_val);
            if (ts_config_module_get_uint(mod, "min_duty", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "min_duty", uint_val);
            if (ts_config_module_get_uint(mod, "max_duty", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "max_duty", uint_val);
            if (ts_config_module_get_uint(mod, "target_temp", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "target_temp", uint_val);
            break;
            
        case TS_CONFIG_MODULE_DEVICE:
            if (ts_config_module_get_bool(mod, "agx.auto_power_on", &bool_val) == ESP_OK)
                cJSON_AddBoolToObject(config, "agx.auto_power_on", bool_val);
            if (ts_config_module_get_uint(mod, "agx.power_on_delay", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "agx.power_on_delay", uint_val);
            if (ts_config_module_get_uint(mod, "agx.force_off_timeout", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "agx.force_off_timeout", uint_val);
            if (ts_config_module_get_bool(mod, "monitor.enabled", &bool_val) == ESP_OK)
                cJSON_AddBoolToObject(config, "monitor.enabled", bool_val);
            if (ts_config_module_get_uint(mod, "monitor.interval", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "monitor.interval", uint_val);
            break;
            
        case TS_CONFIG_MODULE_SYSTEM:
            if (ts_config_module_get_string(mod, "timezone", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "timezone", str_val);
            if (ts_config_module_get_string(mod, "log_level", str_val, sizeof(str_val)) == ESP_OK)
                cJSON_AddStringToObject(config, "log_level", str_val);
            if (ts_config_module_get_bool(mod, "console.enabled", &bool_val) == ESP_OK)
                cJSON_AddBoolToObject(config, "console.enabled", bool_val);
            if (ts_config_module_get_uint(mod, "console.baudrate", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "console.baudrate", uint_val);
            if (ts_config_module_get_bool(mod, "webui.enabled", &bool_val) == ESP_OK)
                cJSON_AddBoolToObject(config, "webui.enabled", bool_val);
            if (ts_config_module_get_uint(mod, "webui.port", &uint_val) == ESP_OK)
                cJSON_AddNumberToObject(config, "webui.port", uint_val);
            break;
            
        default:
            break;
    }
}

/**
 * @brief config.module.show - 显示模块配置
 */
static esp_err_t api_config_module_show(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *module_name = cJSON_GetObjectItem(params, "module");
    if (!module_name || !cJSON_IsString(module_name)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'module' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_config_module_t mod = parse_module_name(module_name->valuestring);
    if (mod > TS_CONFIG_MODULE_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid module name");
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *data = cJSON_CreateObject();
    
    if (mod == TS_CONFIG_MODULE_MAX) {
        /* 返回所有模块配置 */
        for (int i = 0; i < TS_CONFIG_MODULE_MAX; i++) {
            if (ts_config_module_is_registered((ts_config_module_t)i)) {
                cJSON *mod_config = cJSON_CreateObject();
                add_module_config_to_json((ts_config_module_t)i, mod_config);
                cJSON_AddBoolToObject(mod_config, "_dirty", ts_config_module_is_dirty((ts_config_module_t)i));
                cJSON_AddItemToObject(data, module_to_name((ts_config_module_t)i), mod_config);
            }
        }
    } else {
        if (!ts_config_module_is_registered(mod)) {
            cJSON_Delete(data);
            ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Module not registered");
            return ESP_ERR_NOT_FOUND;
        }
        
        cJSON_AddStringToObject(data, "module", module_name->valuestring);
        cJSON *config = cJSON_AddObjectToObject(data, "config");
        add_module_config_to_json(mod, config);
        cJSON_AddBoolToObject(data, "dirty", ts_config_module_is_dirty(mod));
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.module.set - 设置模块配置
 */
static esp_err_t api_config_module_set(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *module_name = cJSON_GetObjectItem(params, "module");
    const cJSON *key = cJSON_GetObjectItem(params, "key");
    const cJSON *value = cJSON_GetObjectItem(params, "value");
    
    if (!module_name || !cJSON_IsString(module_name)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'module' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!key || !cJSON_IsString(key)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'key' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!value) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'value' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_config_module_t mod = parse_module_name(module_name->valuestring);
    if (mod >= TS_CONFIG_MODULE_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid module name");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!ts_config_module_is_registered(mod)) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Module not registered");
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_err_t ret = ESP_FAIL;
    
    if (cJSON_IsBool(value)) {
        ret = ts_config_module_set_bool(mod, key->valuestring, cJSON_IsTrue(value));
    } else if (cJSON_IsNumber(value)) {
        double d = value->valuedouble;
        if (d >= 0 && d == (uint32_t)d) {
            ret = ts_config_module_set_uint(mod, key->valuestring, (uint32_t)d);
        } else {
            ret = ts_config_module_set_int(mod, key->valuestring, (int32_t)d);
        }
    } else if (cJSON_IsString(value)) {
        ret = ts_config_module_set_string(mod, key->valuestring, value->valuestring);
    } else {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Unsupported value type");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set config");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "module", module_name->valuestring);
    cJSON_AddStringToObject(data, "key", key->valuestring);
    cJSON_AddBoolToObject(data, "success", true);
    cJSON_AddBoolToObject(data, "dirty", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.module.save - 保存模块配置到 NVS
 */
static esp_err_t api_config_module_save(const cJSON *params, ts_api_result_t *result)
{
    ts_config_module_t mod = TS_CONFIG_MODULE_MAX;
    
    if (params) {
        const cJSON *module_name = cJSON_GetObjectItem(params, "module");
        if (module_name && cJSON_IsString(module_name)) {
            mod = parse_module_name(module_name->valuestring);
            if (mod > TS_CONFIG_MODULE_MAX) {
                ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid module name");
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON *results_arr = cJSON_AddArrayToObject(data, "results");
    int success_count = 0, fail_count = 0;
    
    if (mod == TS_CONFIG_MODULE_MAX) {
        /* 保存所有模块 */
        for (int i = 0; i < TS_CONFIG_MODULE_MAX; i++) {
            if (ts_config_module_is_registered((ts_config_module_t)i)) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "module", module_to_name((ts_config_module_t)i));
                
                esp_err_t ret = ts_config_module_persist((ts_config_module_t)i);
                cJSON_AddBoolToObject(item, "success", ret == ESP_OK);
                if (ret == ESP_OK) {
                    success_count++;
                } else {
                    cJSON_AddStringToObject(item, "error", esp_err_to_name(ret));
                    fail_count++;
                }
                cJSON_AddItemToArray(results_arr, item);
            }
        }
    } else {
        /* 保存单个模块 */
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "module", module_to_name(mod));
        
        esp_err_t ret = ts_config_module_persist(mod);
        cJSON_AddBoolToObject(item, "success", ret == ESP_OK);
        if (ret == ESP_OK) {
            success_count++;
        } else {
            cJSON_AddStringToObject(item, "error", esp_err_to_name(ret));
            fail_count++;
        }
        cJSON_AddItemToArray(results_arr, item);
    }
    
    cJSON_AddNumberToObject(data, "success_count", success_count);
    cJSON_AddNumberToObject(data, "fail_count", fail_count);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.module.reset - 重置模块配置
 */
static esp_err_t api_config_module_reset(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *module_name = cJSON_GetObjectItem(params, "module");
    if (!module_name || !cJSON_IsString(module_name)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'module' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_config_module_t mod = parse_module_name(module_name->valuestring);
    if (mod > TS_CONFIG_MODULE_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid module name");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *persist_json = cJSON_GetObjectItem(params, "persist");
    bool persist = persist_json ? cJSON_IsTrue(persist_json) : true;
    
    esp_err_t ret = ts_config_module_reset(mod, persist);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to reset module");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "module", module_name->valuestring);
    cJSON_AddBoolToObject(data, "reset", true);
    cJSON_AddBoolToObject(data, "persisted", persist);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_api_config_register(void)
{
    static const ts_api_endpoint_t config_apis[] = {
        /* 基础配置 API */
        {
            .name = "config.get",
            .description = "Get configuration value",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_get,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "config.set",
            .description = "Set configuration value",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_set,
            .requires_auth = true,
            .permission = "config.write"
        },
        {
            .name = "config.delete",
            .description = "Delete configuration value",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_delete,
            .requires_auth = true,
            .permission = "config.admin"
        },
        {
            .name = "config.list",
            .description = "List configuration keys",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_list,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "config.save",
            .description = "Save configuration to storage",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_save,
            .requires_auth = true,
            .permission = "config.write"
        },
        /* 模块化配置 API */
        {
            .name = "config.module.list",
            .description = "List all config modules",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_module_list,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "config.module.show",
            .description = "Show module configuration",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_module_show,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "config.module.set",
            .description = "Set module configuration",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_module_set,
            .requires_auth = true,
            .permission = "config.write"
        },
        {
            .name = "config.module.save",
            .description = "Save module configuration to NVS",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_module_save,
            .requires_auth = true,
            .permission = "config.write"
        },
        {
            .name = "config.module.reset",
            .description = "Reset module configuration",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_module_reset,
            .requires_auth = true,
            .permission = "config.admin"
        },
    };
    
    esp_err_t ret = ts_api_register_multiple(config_apis, 
                                              sizeof(config_apis) / sizeof(config_apis[0]));
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Config API registered (%d endpoints)", 
                (int)(sizeof(config_apis) / sizeof(config_apis[0])));
    }
    return ret;
}
