/**
 * @file ts_api_power.c
 * @brief Power Monitoring API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-20
 */

#include "ts_api.h"
#include "ts_power_monitor.h"
#include "ts_power_policy.h"
#include "ts_log.h"
#include <string.h>

#define TAG "api_power"

/* 配置键定义 */

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief power.status - Get power monitoring status
 * 
 * Returns: voltage, current, power, thresholds, protection status
 */
static esp_err_t api_power_status(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    cJSON *data = cJSON_CreateObject();
    
    /* Voltage data */
    ts_power_voltage_data_t voltage_data;
    if (ts_power_monitor_get_voltage_data(&voltage_data) == ESP_OK) {
        cJSON *voltage = cJSON_AddObjectToObject(data, "voltage");
        cJSON_AddNumberToObject(voltage, "supply_v", voltage_data.supply_voltage);
        cJSON_AddNumberToObject(voltage, "adc_raw", voltage_data.raw_adc);
        cJSON_AddNumberToObject(voltage, "timestamp_ms", voltage_data.timestamp);
    }
    
    /* Power chip data */
    ts_power_chip_data_t chip_data;
    if (ts_power_monitor_get_power_chip_data(&chip_data) == ESP_OK) {
        cJSON *chip = cJSON_AddObjectToObject(data, "power_chip");
        cJSON_AddBoolToObject(chip, "valid", chip_data.valid);
        cJSON_AddNumberToObject(chip, "voltage_v", chip_data.voltage);
        cJSON_AddNumberToObject(chip, "current_a", chip_data.current);
        cJSON_AddNumberToObject(chip, "power_w", chip_data.power);
        cJSON_AddBoolToObject(chip, "crc_valid", chip_data.crc_valid);
    }
    
    /* Protection status */
    ts_power_policy_status_t prot_status;
    if (ts_power_policy_get_status(&prot_status) == ESP_OK) {
        cJSON *protection = cJSON_AddObjectToObject(data, "protection");
        cJSON_AddBoolToObject(protection, "running", prot_status.running);
        cJSON_AddStringToObject(protection, "state", 
            ts_power_policy_get_state_name(prot_status.state));
        cJSON_AddNumberToObject(protection, "countdown_sec", prot_status.countdown_remaining_sec);
        cJSON_AddNumberToObject(protection, "current_voltage_v", prot_status.current_voltage);
    }
    
    /* Monitor stats */
    ts_power_monitor_stats_t stats;
    if (ts_power_monitor_get_stats(&stats) == ESP_OK) {
        cJSON *stat = cJSON_AddObjectToObject(data, "stats");
        cJSON_AddNumberToObject(stat, "samples", stats.voltage_samples);
        cJSON_AddNumberToObject(stat, "avg_voltage_v", stats.avg_voltage);
        cJSON_AddNumberToObject(stat, "avg_current_a", stats.avg_current);
        cJSON_AddNumberToObject(stat, "avg_power_w", stats.avg_power);
        cJSON_AddNumberToObject(stat, "uptime_ms", stats.uptime_ms);
    }
    
    cJSON_AddBoolToObject(data, "monitoring_active", ts_power_monitor_is_running());
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief power.voltage - Read current voltage
 * 
 * Params: { "now": true } for immediate read
 */
static esp_err_t api_power_voltage(const cJSON *params, ts_api_result_t *result)
{
    ts_power_voltage_data_t data;
    esp_err_t ret;
    
    const cJSON *now_item = cJSON_GetObjectItem(params, "now");
    bool immediate = cJSON_IsTrue(now_item);
    
    if (immediate) {
        ret = ts_power_monitor_read_voltage_now(&data);
    } else {
        ret = ts_power_monitor_get_voltage_data(&data);
    }
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to read voltage");
        return ret;
    }
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "voltage_v", data.supply_voltage);
    cJSON_AddNumberToObject(json, "adc_raw", data.raw_adc);
    cJSON_AddNumberToObject(json, "voltage_mv", data.voltage_mv);
    cJSON_AddNumberToObject(json, "timestamp_ms", data.timestamp);
    
    ts_api_result_ok(result, json);
    return ESP_OK;
}

/**
 * @brief power.protection.set - Configure voltage protection
 * 
 * Params: { "enable": true, "low_threshold": 12.6, "recovery_threshold": 18.0, "shutdown_delay": 60, "persist": true }
 */
static esp_err_t api_power_protection_set(const cJSON *params, ts_api_result_t *result)
{
    ts_power_policy_config_t config;
    esp_err_t current = ts_power_policy_get_config(&config);
    if (current != ESP_OK) { ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to read config"); return current; }
    
    /* Parse config params */
    const cJSON *low_thresh = cJSON_GetObjectItem(params, "low_threshold");
    const cJSON *recovery_thresh = cJSON_GetObjectItem(params, "recovery_threshold");
    const cJSON *shutdown_delay = cJSON_GetObjectItem(params, "shutdown_delay");
    const cJSON *recovery_hold = cJSON_GetObjectItem(params, "recovery_hold");
    const cJSON *fan_stop_delay = cJSON_GetObjectItem(params, "fan_stop_delay");
    const cJSON *enable = cJSON_GetObjectItem(params, "enable");
    const cJSON *persist = cJSON_GetObjectItem(params, "persist");
    
    float new_low = config.low_voltage_threshold;
    float new_recovery = config.recovery_voltage_threshold;
    uint32_t new_shutdown_delay = config.shutdown_delay_sec;
    uint32_t new_recovery_hold = config.recovery_hold_sec;
    uint32_t new_fan_delay = config.fan_stop_delay_sec;
    
    if (cJSON_IsNumber(low_thresh)) {
        new_low = (float)low_thresh->valuedouble;
    }
    if (cJSON_IsNumber(recovery_thresh)) {
        new_recovery = (float)recovery_thresh->valuedouble;
    }
    if (cJSON_IsNumber(shutdown_delay)) {
        new_shutdown_delay = (uint32_t)shutdown_delay->valueint;
    }
    if (cJSON_IsNumber(recovery_hold)) {
        new_recovery_hold = (uint32_t)recovery_hold->valueint;
    }
    if (cJSON_IsNumber(fan_stop_delay)) {
        new_fan_delay = (uint32_t)fan_stop_delay->valueint;
    }
    
    /* Update thresholds */
    esp_err_t ret = ts_power_policy_set_thresholds(new_low, new_recovery);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set thresholds");
        return ret;
    }
    
    if (cJSON_IsNumber(shutdown_delay)) {
        ret = ts_power_policy_set_shutdown_delay(new_shutdown_delay);
        if (ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set shutdown delay");
            return ret;
        }
    }
    
    if (cJSON_IsNumber(recovery_hold)) {
        ret = ts_power_policy_set_recovery_hold(new_recovery_hold);
        if (ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set recovery hold time");
            return ret;
        }
    }
    
    if (cJSON_IsNumber(fan_stop_delay)) {
        ret = ts_power_policy_set_fan_stop_delay(new_fan_delay);
        if (ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set fan stop delay");
            return ret;
        }
    }
    
    /* Persist to system NVS when requested. */
    /* Enable/disable if specified */
    if (cJSON_IsBool(enable)) {
        if (cJSON_IsTrue(enable)) {
            ret = ts_power_policy_start();
        } else {
            ret = ts_power_policy_stop();
        }
        if (ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to enable/disable protection");
            return ret;
        }
        /* 启用/禁用状态变化时，总是保存配置（持久化 enabled 状态）*/
        TS_LOGI(TAG, "Protection %s, saving config", cJSON_IsTrue(enable) ? "enabled" : "disabled");
        ret = ts_power_policy_save_config();
        if (ret != ESP_OK) { ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to save config"); return ret; }
    } else if (cJSON_IsTrue(persist)) {
        /* 只有参数变化时才需要显式 persist */
        TS_LOGI(TAG, "Persisting power protection config to system NVS");
        ret = ts_power_policy_save_config();
        if (ret != ESP_OK) { ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to save config"); return ret; }
    }
    
    /* Return current status */
    cJSON *data = cJSON_CreateObject();
    float low, recovery;
    ts_power_policy_get_thresholds(&low, &recovery);
    cJSON_AddNumberToObject(data, "low_threshold_v", low);
    cJSON_AddNumberToObject(data, "recovery_threshold_v", recovery);
    cJSON_AddBoolToObject(data, "running", ts_power_policy_is_running());
    cJSON_AddBoolToObject(data, "persisted", cJSON_IsBool(enable) || cJSON_IsTrue(persist));
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief power.protection.config - Get current protection configuration
 * 
 * Returns: { low_voltage_threshold, recovery_voltage_threshold, shutdown_delay_sec, recovery_hold_sec, fan_stop_delay_sec }
 */
static esp_err_t api_power_protection_config(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    cJSON *data = cJSON_CreateObject();
    
    ts_power_policy_config_t config;
    esp_err_t ret = ts_power_policy_get_config(&config);
    if (ret != ESP_OK) { cJSON_Delete(data); ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to read config"); return ret; }
    cJSON_AddNumberToObject(data, "low_voltage_threshold", config.low_voltage_threshold);
    cJSON_AddNumberToObject(data, "recovery_voltage_threshold", config.recovery_voltage_threshold);
    cJSON_AddNumberToObject(data, "shutdown_delay_sec", config.shutdown_delay_sec);
    cJSON_AddNumberToObject(data, "recovery_hold_sec", config.recovery_hold_sec);
    cJSON_AddNumberToObject(data, "fan_stop_delay_sec", config.fan_stop_delay_sec);

    /* 默认值供参考 */
    cJSON *defaults = cJSON_AddObjectToObject(data, "defaults");
    cJSON_AddNumberToObject(defaults, "low_voltage_threshold", TS_POWER_POLICY_LOW_VOLTAGE_DEFAULT);
    cJSON_AddNumberToObject(defaults, "recovery_voltage_threshold", TS_POWER_POLICY_RECOVERY_VOLTAGE_DEFAULT);
    cJSON_AddNumberToObject(defaults, "shutdown_delay_sec", TS_POWER_POLICY_SHUTDOWN_DELAY_DEFAULT);
    cJSON_AddNumberToObject(defaults, "recovery_hold_sec", TS_POWER_POLICY_RECOVERY_HOLD_DEFAULT);
    cJSON_AddNumberToObject(defaults, "fan_stop_delay_sec", TS_POWER_POLICY_FAN_STOP_DELAY_DEFAULT);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief power.protection.status - Get protection status
 */
static esp_err_t api_power_protection_status(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    cJSON *data = cJSON_CreateObject();
    
    /* Check if power policy is initialized */
    if (!ts_power_policy_is_initialized()) {
        /* Return default/disabled status */
        cJSON_AddBoolToObject(data, "initialized", false);
        cJSON_AddBoolToObject(data, "running", false);
        cJSON_AddStringToObject(data, "state", "disabled");
        cJSON_AddNumberToObject(data, "current_voltage_v", 0);
        cJSON_AddStringToObject(data, "message", "Power protection not initialized");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    ts_power_policy_status_t status;
    esp_err_t ret = ts_power_policy_get_status(&status);
    
    if (ret != ESP_OK) {
        cJSON_AddBoolToObject(data, "initialized", false);
        cJSON_AddBoolToObject(data, "running", false);
        cJSON_AddStringToObject(data, "state", "error");
        cJSON_AddStringToObject(data, "message", "Failed to get status");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    /* Success - populate with actual data */
    cJSON_AddBoolToObject(data, "initialized", status.initialized);
    cJSON_AddBoolToObject(data, "running", status.running);
    cJSON_AddStringToObject(data, "state", ts_power_policy_get_state_name(status.state));
    cJSON_AddNumberToObject(data, "current_voltage_v", status.current_voltage);
    cJSON_AddNumberToObject(data, "countdown_remaining_sec", status.countdown_remaining_sec);
    cJSON_AddNumberToObject(data, "recovery_timer_sec", status.recovery_timer_sec);
    cJSON_AddNumberToObject(data, "protection_count", status.protection_count);
    cJSON_AddNumberToObject(data, "uptime_ms", status.uptime_ms);
    
    /* Device status */
    cJSON *device = cJSON_AddObjectToObject(data, "devices");
    cJSON_AddBoolToObject(device, "agx_powered", status.device_status.agx_powered);
    cJSON_AddBoolToObject(device, "lpmu_powered", status.device_status.lpmu_powered);
    cJSON_AddBoolToObject(device, "agx_connected", status.device_status.agx_connected);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief power.monitor.start - Start power monitoring
 */
static esp_err_t api_power_monitor_start(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    esp_err_t ret = ts_power_monitor_start();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to start monitoring");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "running", true);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief power.monitor.stop - Stop power monitoring
 */
static esp_err_t api_power_monitor_stop(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    esp_err_t ret = ts_power_monitor_stop();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to stop monitoring");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "running", false);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief power.chip - Get power chip data
 */
static esp_err_t api_power_chip(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    ts_power_chip_data_t data;
    esp_err_t ret = ts_power_monitor_get_power_chip_data(&data);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get power chip data");
        return ret;
    }
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "valid", data.valid);
    cJSON_AddNumberToObject(json, "voltage_v", data.voltage);
    cJSON_AddNumberToObject(json, "current_a", data.current);
    cJSON_AddNumberToObject(json, "power_w", data.power);
    cJSON_AddBoolToObject(json, "crc_valid", data.crc_valid);
    cJSON_AddNumberToObject(json, "timestamp_ms", data.timestamp);
    
    cJSON *raw = cJSON_AddArrayToObject(json, "raw_data");
    for (int i = 0; i < 4; i++) {
        cJSON_AddItemToArray(raw, cJSON_CreateNumber(data.raw_data[i]));
    }
    
    ts_api_result_ok(result, json);
    return ESP_OK;
}

/**
 * @brief power.stats - Get monitoring statistics
 */
static esp_err_t api_power_stats(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    ts_power_monitor_stats_t stats;
    esp_err_t ret = ts_power_monitor_get_stats(&stats);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get statistics");
        return ret;
    }
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "uptime_ms", (double)stats.uptime_ms);
    cJSON_AddNumberToObject(json, "voltage_samples", stats.voltage_samples);
    cJSON_AddNumberToObject(json, "power_chip_packets", stats.power_chip_packets);
    cJSON_AddNumberToObject(json, "crc_errors", stats.crc_errors);
    cJSON_AddNumberToObject(json, "timeout_errors", stats.timeout_errors);
    cJSON_AddNumberToObject(json, "threshold_violations", stats.threshold_violations);
    cJSON_AddNumberToObject(json, "avg_voltage_v", stats.avg_voltage);
    cJSON_AddNumberToObject(json, "avg_current_a", stats.avg_current);
    cJSON_AddNumberToObject(json, "avg_power_w", stats.avg_power);
    
    ts_api_result_ok(result, json);
    return ESP_OK;
}

/**
 * @brief power.stats.reset - Reset statistics
 */
static esp_err_t api_power_stats_reset(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    esp_err_t ret = ts_power_monitor_reset_stats();
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to reset statistics");
        return ret;
    }
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "reset", true);
    ts_api_result_ok(result, json);
    return ESP_OK;
}

/**
 * @brief power.threshold.set - Set voltage thresholds
 * 
 * Params: { "min_v": 10.0, "max_v": 28.0 }
 */
static esp_err_t api_power_threshold_set(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *min_v = cJSON_GetObjectItem(params, "min_v");
    const cJSON *max_v = cJSON_GetObjectItem(params, "max_v");
    
    if (!cJSON_IsNumber(min_v) || !cJSON_IsNumber(max_v)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing min_v or max_v");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_power_monitor_set_voltage_thresholds(
        (float)min_v->valuedouble, (float)max_v->valuedouble);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set thresholds");
        return ret;
    }
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "min_v", min_v->valuedouble);
    cJSON_AddNumberToObject(json, "max_v", max_v->valuedouble);
    ts_api_result_ok(result, json);
    return ESP_OK;
}

/**
 * @brief power.interval.set - Set sampling interval
 * 
 * Params: { "interval_ms": 1000 }
 */
static esp_err_t api_power_interval_set(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *interval = cJSON_GetObjectItem(params, "interval_ms");
    if (!cJSON_IsNumber(interval)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing interval_ms");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_power_monitor_set_sample_interval((uint32_t)interval->valueint);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set interval");
        return ret;
    }
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "interval_ms", interval->valueint);
    ts_api_result_ok(result, json);
    return ESP_OK;
}

/**
 * @brief power.debug - Set debug mode
 * 
 * Params: { "enable": true }
 */
static esp_err_t api_power_debug(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *enable = cJSON_GetObjectItem(params, "enable");
    if (!cJSON_IsBool(enable)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing enable parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_power_monitor_set_debug_mode(cJSON_IsTrue(enable));
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set debug mode");
        return ret;
    }
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "debug_enabled", cJSON_IsTrue(enable));
    ts_api_result_ok(result, json);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t s_power_endpoints[] = {
    {
        .name = "power.status",
        .description = "Get power monitoring status",
        .category = TS_API_CAT_POWER,
        .handler = api_power_status,
        .requires_auth = false,
    },
    {
        .name = "power.voltage",
        .description = "Read current voltage",
        .category = TS_API_CAT_POWER,
        .handler = api_power_voltage,
        .requires_auth = false,
    },
    {
        .name = "power.chip",
        .description = "Get power chip data",
        .category = TS_API_CAT_POWER,
        .handler = api_power_chip,
        .requires_auth = false,
    },
    {
        .name = "power.stats",
        .description = "Get monitoring statistics",
        .category = TS_API_CAT_POWER,
        .handler = api_power_stats,
        .requires_auth = false,
    },
    {
        .name = "power.stats.reset",
        .description = "Reset monitoring statistics",
        .category = TS_API_CAT_POWER,
        .handler = api_power_stats_reset,
        .requires_auth = true,
    },
    {
        .name = "power.threshold.set",
        .description = "Set voltage thresholds",
        .category = TS_API_CAT_POWER,
        .handler = api_power_threshold_set,
        .requires_auth = true,
    },
    {
        .name = "power.interval.set",
        .description = "Set sampling interval",
        .category = TS_API_CAT_POWER,
        .handler = api_power_interval_set,
        .requires_auth = true,
    },
    {
        .name = "power.debug",
        .description = "Set debug mode",
        .category = TS_API_CAT_POWER,
        .handler = api_power_debug,
        .requires_auth = true,
    },
    {
        .name = "power.protection.set",
        .description = "Configure voltage protection",
        .category = TS_API_CAT_POWER,
        .handler = api_power_protection_set,
        .requires_auth = true,
    },
    {
        .name = "power.protection.config",
        .description = "Get voltage protection configuration",
        .category = TS_API_CAT_POWER,
        .handler = api_power_protection_config,
        .requires_auth = false,
    },
    {
        .name = "power.protection.status",
        .description = "Get voltage protection status",
        .category = TS_API_CAT_POWER,
        .handler = api_power_protection_status,
        .requires_auth = false,
    },
    {
        .name = "power.monitor.start",
        .description = "Start power monitoring",
        .category = TS_API_CAT_POWER,
        .handler = api_power_monitor_start,
        .requires_auth = true,
    },
    {
        .name = "power.monitor.stop",
        .description = "Stop power monitoring",
        .category = TS_API_CAT_POWER,
        .handler = api_power_monitor_stop,
        .requires_auth = true,
    },
};

esp_err_t ts_api_power_register(void)
{
    TS_LOGI(TAG, "Registering power APIs");
    return ts_api_register_multiple(s_power_endpoints, 
                                    sizeof(s_power_endpoints) / sizeof(s_power_endpoints[0]));
}
