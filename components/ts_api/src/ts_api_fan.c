/**
 * @file ts_api_fan.c
 * @brief Fan Control API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-20
 */

#include "ts_api.h"
#include "ts_fan.h"
#include "ts_temp_source.h"
#include "ts_config_module.h"
#include "ts_log.h"
#include <string.h>

#define TAG "api_fan"

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static const char *mode_to_string(ts_fan_mode_t mode)
{
    switch (mode) {
        case TS_FAN_MODE_OFF:    return "off";
        case TS_FAN_MODE_MANUAL: return "manual";
        case TS_FAN_MODE_AUTO:   return "auto";
        case TS_FAN_MODE_CURVE:  return "curve";
        default:                 return "unknown";
    }
}

static const char *auto_state_to_string(ts_fan_auto_state_t state)
{
    switch (state) {
        case TS_FAN_AUTO_STATE_IDLE:     return "idle";
        case TS_FAN_AUTO_STATE_BASELINE: return "baseline";
        case TS_FAN_AUTO_STATE_ACTIVE:   return "active";
        case TS_FAN_AUTO_STATE_GUARD:    return "guard";
        case TS_FAN_AUTO_STATE_STALE:    return "stale";
        default:                         return "unknown";
    }
}

static ts_fan_mode_t string_to_mode(const char *str)
{
    if (!str) return TS_FAN_MODE_MANUAL;
    if (strcmp(str, "off") == 0)    return TS_FAN_MODE_OFF;
    if (strcmp(str, "manual") == 0) return TS_FAN_MODE_MANUAL;
    if (strcmp(str, "auto") == 0)   return TS_FAN_MODE_AUTO;
    if (strcmp(str, "curve") == 0)  return TS_FAN_MODE_CURVE;
    return TS_FAN_MODE_MANUAL;
}

static cJSON *status_to_json(ts_fan_id_t fan_id, const ts_fan_status_t *status)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", fan_id);
    cJSON_AddStringToObject(obj, "mode", mode_to_string(status->mode));
    cJSON_AddBoolToObject(obj, "duty_valid", status->duty_valid);
    if (status->duty_valid) cJSON_AddNumberToObject(obj, "duty", status->duty_percent);
    else cJSON_AddNullToObject(obj, "duty");
    cJSON_AddNumberToObject(obj, "target_duty", status->target_duty);
    cJSON_AddNumberToObject(obj, "rpm", status->rpm);
    cJSON_AddNumberToObject(obj, "temperature", status->temp / 10.0);
    cJSON_AddBoolToObject(obj, "enabled", status->enabled);
    if (status->duty_valid) cJSON_AddBoolToObject(obj, "running", status->is_running);
    else cJSON_AddNullToObject(obj, "running");
    cJSON_AddBoolToObject(obj, "fault", status->fault);
    cJSON_AddNumberToObject(obj, "control_temperature", status->control_temp / 10.0);
    cJSON_AddNumberToObject(obj, "guard_temperature", status->guard_temp / 10.0);
    cJSON_AddNumberToObject(obj, "predicted_temperature", status->predicted_temp / 10.0);
    cJSON_AddNumberToObject(obj, "slope_c_per_min", status->slope_c_per_min);
    cJSON_AddNumberToObject(obj, "controller_gain", status->controller_gain);
    cJSON_AddNumberToObject(obj, "cooling_response", status->cooling_response);
    cJSON_AddStringToObject(obj, "auto_state", auto_state_to_string(status->auto_state));
    cJSON_AddBoolToObject(obj, "guard_active", status->guard_active);
    cJSON_AddBoolToObject(obj, "temp_stale", status->temp_stale);
    return obj;
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief fan.status - Get fan status
 * 
 * Params: { "id": 0 } or {} for all fans
 * Returns: single fan status or array of all fans
 */
static esp_err_t api_fan_status(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *id_item = cJSON_GetObjectItem(params, "id");
    
    if (cJSON_IsNumber(id_item)) {
        /* Single fan */
        int fan_id = id_item->valueint;
        if (fan_id < 0 || fan_id >= TS_FAN_MAX) {
            ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid fan ID");
            return ESP_ERR_INVALID_ARG;
        }
        
        ts_fan_status_t status;
        esp_err_t ret = ts_fan_get_status(fan_id, &status);
        if (ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get fan status");
            return ret;
        }
        
        ts_api_result_ok(result, status_to_json(fan_id, &status));
    } else {
        /* All fans */
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < TS_FAN_MAX; i++) {
            ts_fan_status_t status;
            if (ts_fan_get_status(i, &status) == ESP_OK) {
                cJSON_AddItemToArray(arr, status_to_json(i, &status));
            }
        }
        
        cJSON *data = cJSON_CreateObject();
        cJSON_AddItemToObject(data, "fans", arr);
        
        /* 添加全局温度信息（从绑定变量读取） */
        ts_temp_status_t temp_status;
        if (ts_temp_get_status(&temp_status) == ESP_OK) {
            cJSON_AddNumberToObject(data, "temperature", temp_status.current_temp / 10.0f);
            cJSON_AddBoolToObject(data, "temp_valid", temp_status.current_valid);
            if (temp_status.bound_variable[0] != '\0') {
                cJSON_AddStringToObject(data, "temp_source", temp_status.bound_variable);
            }
        }
        
        ts_api_result_ok(result, data);
    }
    
    return ESP_OK;
}

/**
 * @brief fan.set - Set fan speed (manual mode)
 * 
 * Params: { "id": 0, "duty": 50 }
 */
static esp_err_t api_fan_set(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *id_item = cJSON_GetObjectItem(params, "id");
    const cJSON *duty_item = cJSON_GetObjectItem(params, "duty");
    
    if (!cJSON_IsNumber(id_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: id");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsNumber(duty_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: duty");
        return ESP_ERR_INVALID_ARG;
    }
    
    int fan_id = id_item->valueint;
    int duty = duty_item->valueint;
    
    if (fan_id < 0 || fan_id >= TS_FAN_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid fan ID");
        return ESP_ERR_INVALID_ARG;
    }
    if (duty < 0 || duty > 100) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Duty must be 0-100");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_fan_set_duty(fan_id, duty);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to set fan duty");
        return ret;
    }
    
    ts_fan_status_t status;
    ret = ts_fan_get_status(fan_id, &status);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get fan status after setting");
        return ret;
    }
    cJSON *data = status_to_json(fan_id, &status);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief fan.mode - Set fan mode
 * 
 * Params: { "id": 0, "mode": "auto" }
 */
static esp_err_t api_fan_mode(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *id_item = cJSON_GetObjectItem(params, "id");
    const cJSON *mode_item = cJSON_GetObjectItem(params, "mode");
    
    if (!cJSON_IsNumber(id_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: id");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsString(mode_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: mode");
        return ESP_ERR_INVALID_ARG;
    }
    
    int fan_id = id_item->valueint;
    ts_fan_mode_t mode = string_to_mode(mode_item->valuestring);
    
    if (fan_id < 0 || fan_id >= TS_FAN_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid fan ID");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_fan_set_mode(fan_id, mode);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to set fan mode");
        return ret;
    }
    
    // 自动保存模式变更到 NVS
    ts_fan_save_full_config(fan_id);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", fan_id);
    cJSON_AddStringToObject(data, "mode", mode_to_string(mode));
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief fan.enable - Enable or disable a fan
 * 
 * Params: { "id": 0, "enable": true }
 */
static esp_err_t api_fan_enable(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *id_item = cJSON_GetObjectItem(params, "id");
    const cJSON *enable_item = cJSON_GetObjectItem(params, "enable");
    
    if (!cJSON_IsNumber(id_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: id");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsBool(enable_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: enable");
        return ESP_ERR_INVALID_ARG;
    }
    
    int fan_id = id_item->valueint;
    bool enable = cJSON_IsTrue(enable_item);
    
    if (fan_id < 0 || fan_id >= TS_FAN_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid fan ID");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_fan_enable(fan_id, enable);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to enable/disable fan");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", fan_id);
    cJSON_AddBoolToObject(data, "enabled", enable);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief fan.limits - Set duty cycle limits
 * 
 * Params: { "id": 0, "min_duty": 10, "max_duty": 100 }
 */
static esp_err_t api_fan_limits(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *id_item = cJSON_GetObjectItem(params, "id");
    const cJSON *min_item = cJSON_GetObjectItem(params, "min_duty");
    const cJSON *max_item = cJSON_GetObjectItem(params, "max_duty");
    
    if (!cJSON_IsNumber(id_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: id");
        return ESP_ERR_INVALID_ARG;
    }
    
    int fan_id = id_item->valueint;
    if (fan_id < 0 || fan_id >= TS_FAN_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid fan ID");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 获取当前配置作为默认值
    ts_fan_config_t cfg;
    ts_fan_get_config(fan_id, &cfg);
    
    uint8_t min_duty = cfg.min_duty;
    uint8_t max_duty = cfg.max_duty;
    
    if (cJSON_IsNumber(min_item)) {
        min_duty = (uint8_t)min_item->valueint;
    }
    if (cJSON_IsNumber(max_item)) {
        max_duty = (uint8_t)max_item->valueint;
    }
    
    if (min_duty > 100 || max_duty > 100 || min_duty > max_duty) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid duty limits (0-100, min <= max)");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_fan_set_limits(fan_id, min_duty, max_duty);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to set fan limits");
        return ret;
    }
    
    // 自动保存到 NVS + SD 卡
    ts_fan_save_full_config(fan_id);
    ts_config_module_persist(TS_CONFIG_MODULE_FAN);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", fan_id);
    cJSON_AddNumberToObject(data, "min_duty", min_duty);
    cJSON_AddNumberToObject(data, "max_duty", max_duty);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief fan.curve - Set temperature curve
 * 
 * Params: { "id": 0, "curve": [{"temp": 30, "duty": 30}, ...], "hysteresis": 3.0, "min_interval": 2000 }
 * hysteresis (optional): Temperature dead zone in °C
 * min_interval (optional): Minimum interval between speed changes in ms
 */
static esp_err_t api_fan_curve(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *id_item = cJSON_GetObjectItem(params, "id");
    const cJSON *curve_item = cJSON_GetObjectItem(params, "curve");
    const cJSON *hysteresis_item = cJSON_GetObjectItem(params, "hysteresis");
    const cJSON *interval_item = cJSON_GetObjectItem(params, "min_interval");
    
    if (!cJSON_IsNumber(id_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: id");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsArray(curve_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: curve (array)");
        return ESP_ERR_INVALID_ARG;
    }
    
    int fan_id = id_item->valueint;
    if (fan_id < 0 || fan_id >= TS_FAN_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid fan ID");
        return ESP_ERR_INVALID_ARG;
    }
    
    int count = cJSON_GetArraySize(curve_item);
    if (count > TS_FAN_MAX_CURVE_POINTS) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Too many curve points");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_fan_curve_point_t curve[TS_FAN_MAX_CURVE_POINTS];
    for (int i = 0; i < count; i++) {
        cJSON *point = cJSON_GetArrayItem(curve_item, i);
        cJSON *temp = cJSON_GetObjectItem(point, "temp");
        cJSON *duty = cJSON_GetObjectItem(point, "duty");
        
        if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(duty)) {
            ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid curve point format");
            return ESP_ERR_INVALID_ARG;
        }
        
        curve[i].temp = (int16_t)(temp->valuedouble * 10);  /* Convert to 0.1°C */
        curve[i].duty = (uint8_t)duty->valueint;
    }
    
    esp_err_t ret = ts_fan_set_curve(fan_id, curve, count);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to set fan curve");
        return ret;
    }
    
    // 如果提供了 hysteresis 或 min_interval，也一并设置
    if (cJSON_IsNumber(hysteresis_item) || cJSON_IsNumber(interval_item)) {
        // 获取当前配置作为默认值
        ts_fan_config_t cfg;
        ts_fan_get_config(fan_id, &cfg);
        
        int16_t hysteresis = cfg.hysteresis;
        uint32_t min_interval = cfg.min_interval;
        
        if (cJSON_IsNumber(hysteresis_item)) {
            hysteresis = (int16_t)(hysteresis_item->valuedouble * 10);  /* Convert to 0.1°C */
        }
        if (cJSON_IsNumber(interval_item)) {
            min_interval = (uint32_t)interval_item->valueint;
        }
        
        ret = ts_fan_set_hysteresis(fan_id, hysteresis, min_interval);
        if (ret != ESP_OK) {
            TS_LOGW(TAG, "Failed to set hysteresis: %s", esp_err_to_name(ret));
        }
    }
    
    // 自动保存到 NVS + SD 卡
    ts_fan_save_full_config(fan_id);
    ts_config_module_persist(TS_CONFIG_MODULE_FAN);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", fan_id);
    cJSON_AddNumberToObject(data, "points", count);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief fan.save - Save fan configuration to NVS
 * 
 * Params: { "id": 0 } or {} for all fans
 */
static esp_err_t api_fan_save(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *id_item = cJSON_GetObjectItem(params, "id");
    
    esp_err_t ret;
    if (cJSON_IsNumber(id_item)) {
        int fan_id = id_item->valueint;
        if (fan_id < 0 || fan_id >= TS_FAN_MAX) {
            ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid fan ID");
            return ESP_ERR_INVALID_ARG;
        }
        ret = ts_fan_save_full_config(fan_id);
    } else {
        ret = ts_fan_save_config();
    }
    
    // 同步到 SD 卡
    ts_config_module_persist(TS_CONFIG_MODULE_FAN);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to save fan config");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "status", "saved");
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief fan.config - Get fan configuration including curve
 * 
 * Params: { "id": 0 }
 * Returns: full configuration with curve points
 */
static esp_err_t api_fan_config(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *id_item = cJSON_GetObjectItem(params, "id");
    
    if (!cJSON_IsNumber(id_item)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing required parameter: id");
        return ESP_ERR_INVALID_ARG;
    }
    
    int fan_id = id_item->valueint;
    if (fan_id < 0 || fan_id >= TS_FAN_MAX) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid fan ID");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_fan_config_t config;
    esp_err_t ret = ts_fan_get_config(fan_id, &config);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get fan config");
        return ret;
    }
    
    // 获取当前模式
    ts_fan_status_t status;
    ts_fan_get_status(fan_id, &status);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", fan_id);
    cJSON_AddStringToObject(data, "mode", mode_to_string(status.mode));
    cJSON_AddNumberToObject(data, "min_duty", config.min_duty);
    cJSON_AddNumberToObject(data, "max_duty", config.max_duty);
    cJSON_AddNumberToObject(data, "hysteresis", config.hysteresis / 10.0);
    cJSON_AddNumberToObject(data, "min_interval", config.min_interval);
    cJSON_AddBoolToObject(data, "invert_pwm", config.invert_pwm);
    
    // 曲线点
    cJSON *curve_arr = cJSON_CreateArray();
    for (int i = 0; i < config.curve_points; i++) {
        cJSON *point = cJSON_CreateObject();
        cJSON_AddNumberToObject(point, "temp", config.curve[i].temp / 10.0);
        cJSON_AddNumberToObject(point, "duty", config.curve[i].duty);
        cJSON_AddItemToArray(curve_arr, point);
    }
    cJSON_AddItemToObject(data, "curve", curve_arr);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t s_fan_endpoints[] = {
    {
        .name = "fan.status",
        .description = "Get fan status",
        .category = TS_API_CAT_FAN,
        .handler = api_fan_status,
        .requires_auth = false,
    },
    {
        .name = "fan.set",
        .description = "Set fan speed (manual mode)",
        .category = TS_API_CAT_FAN,
        .handler = api_fan_set,
        .requires_auth = false,
    },
    {
        .name = "fan.mode",
        .description = "Set fan operating mode",
        .category = TS_API_CAT_FAN,
        .handler = api_fan_mode,
        .requires_auth = false,
    },
    {
        .name = "fan.enable",
        .description = "Enable or disable a fan",
        .category = TS_API_CAT_FAN,
        .handler = api_fan_enable,
        .requires_auth = false,
    },
    {
        .name = "fan.limits",
        .description = "Set duty cycle limits (min/max)",
        .category = TS_API_CAT_FAN,
        .handler = api_fan_limits,
        .requires_auth = false,
    },
    {
        .name = "fan.curve",
        .description = "Set temperature curve for fan",
        .category = TS_API_CAT_FAN,
        .handler = api_fan_curve,
        .requires_auth = false,
    },
    {
        .name = "fan.save",
        .description = "Save fan configuration to NVS",
        .category = TS_API_CAT_FAN,
        .handler = api_fan_save,
        .requires_auth = false,
    },
    {
        .name = "fan.config",
        .description = "Get fan configuration including curve",
        .category = TS_API_CAT_FAN,
        .handler = api_fan_config,
        .requires_auth = false,
    },
};

esp_err_t ts_api_fan_register(void)
{
    TS_LOGI(TAG, "Registering fan APIs");
    return ts_api_register_multiple(s_fan_endpoints, 
                                    sizeof(s_fan_endpoints) / sizeof(s_fan_endpoints[0]));
}
