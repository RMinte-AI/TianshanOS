/**
 * @file ts_api_monitor.c
 * @brief AGX Monitor API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_compute_monitor.h"
#include "ts_log.h"
#include <string.h>

#define TAG "api_monitor"

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief agx.status - Get Compute monitor status
 */
static esp_err_t api_monitor_status(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    if (!ts_compute_monitor_is_initialized()) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Compute monitor not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ts_compute_status_info_t status;
    esp_err_t ret = ts_compute_monitor_get_status(&status);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get status");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddBoolToObject(data, "initialized", status.initialized);
    cJSON_AddBoolToObject(data, "running", status.running);
    cJSON_AddStringToObject(data, "connection", ts_compute_status_to_str(status.connection_status));
    cJSON_AddNumberToObject(data, "reconnects", status.total_reconnects);
    cJSON_AddNumberToObject(data, "messages", status.messages_received);
    cJSON_AddNumberToObject(data, "errors", status.parse_errors);
    cJSON_AddNumberToObject(data, "reliability", status.connection_reliability);
    cJSON_AddNumberToObject(data, "connected_time_ms", status.connected_time_ms);
    
    if (status.last_error[0] != '\0') {
        cJSON_AddStringToObject(data, "last_error", status.last_error);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief agx.data - Get latest AGX data
 */
static esp_err_t api_monitor_data(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    if (!ts_compute_monitor_is_data_valid()) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "No valid compute data");
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_compute_data_t agx_data;
    esp_err_t ret = ts_compute_monitor_get_data(&agx_data);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get data");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(data, "timestamp", agx_data.timestamp);
    
    /* CPU */
    cJSON *cpu = cJSON_AddObjectToObject(data, "cpu");
    cJSON_AddNumberToObject(cpu, "cores", agx_data.cpu.core_count);
    cJSON *cores = cJSON_AddArrayToObject(cpu, "data");
    for (int i = 0; i < agx_data.cpu.core_count; i++) {
        cJSON *core = cJSON_CreateObject();
        cJSON_AddNumberToObject(core, "id", agx_data.cpu.cores[i].id);
        cJSON_AddNumberToObject(core, "usage", agx_data.cpu.cores[i].usage);
        cJSON_AddNumberToObject(core, "freq", agx_data.cpu.cores[i].freq_mhz);
        cJSON_AddItemToArray(cores, core);
    }
    
    /* Memory */
    cJSON *memory = cJSON_AddObjectToObject(data, "memory");
    cJSON *ram = cJSON_AddObjectToObject(memory, "ram");
    cJSON_AddNumberToObject(ram, "used", agx_data.memory.ram.used_mb);
    cJSON_AddNumberToObject(ram, "total", agx_data.memory.ram.total_mb);
    cJSON *swap = cJSON_AddObjectToObject(memory, "swap");
    cJSON_AddNumberToObject(swap, "used", agx_data.memory.swap.used_mb);
    cJSON_AddNumberToObject(swap, "total", agx_data.memory.swap.total_mb);
    
    /* Temperature */
    cJSON *temp = cJSON_AddObjectToObject(data, "temperature");
    cJSON_AddNumberToObject(temp, "cpu", agx_data.temperature.cpu);
    cJSON_AddNumberToObject(temp, "soc0", agx_data.temperature.soc0);
    cJSON_AddNumberToObject(temp, "soc1", agx_data.temperature.soc1);
    cJSON_AddNumberToObject(temp, "soc2", agx_data.temperature.soc2);
    cJSON_AddNumberToObject(temp, "tj", agx_data.temperature.tj);
    
    /* Power */
    cJSON *power = cJSON_AddObjectToObject(data, "power");
    cJSON *gpu_soc = cJSON_AddObjectToObject(power, "gpu_soc");
    cJSON_AddNumberToObject(gpu_soc, "current", agx_data.power.gpu_soc.current_mw);
    cJSON_AddNumberToObject(gpu_soc, "average", agx_data.power.gpu_soc.average_mw);
    cJSON *cpu_cv = cJSON_AddObjectToObject(power, "cpu_cv");
    cJSON_AddNumberToObject(cpu_cv, "current", agx_data.power.cpu_cv.current_mw);
    cJSON_AddNumberToObject(cpu_cv, "average", agx_data.power.cpu_cv.average_mw);
    cJSON *sys_5v = cJSON_AddObjectToObject(power, "sys_5v");
    cJSON_AddNumberToObject(sys_5v, "current", agx_data.power.sys_5v.current_mw);
    cJSON_AddNumberToObject(sys_5v, "average", agx_data.power.sys_5v.average_mw);
    
    /* GPU */
    cJSON *gpu = cJSON_AddObjectToObject(data, "gpu");
    cJSON_AddNumberToObject(gpu, "gr3d_freq_pct", agx_data.gpu.gr3d_freq_pct);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief monitor.config - Get Compute monitor configuration
 */
static esp_err_t api_monitor_config(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    ts_compute_config_t config;
    ts_compute_monitor_get_config(&config);
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(data, "server", config.server_ip);
    cJSON_AddNumberToObject(data, "port", config.server_port);
    cJSON_AddNumberToObject(data, "reconnect_ms", config.reconnect_interval_ms);
    cJSON_AddNumberToObject(data, "startup_delay_ms", config.startup_delay_ms);
    cJSON_AddNumberToObject(data, "heartbeat_timeout_ms", config.heartbeat_timeout_ms);
    cJSON_AddBoolToObject(data, "auto_start", config.auto_start);
    cJSON_AddBoolToObject(data, "update_temp_source", config.update_temp_source);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief monitor.set_server - Set AGX monitoring server IP and port
 * 
 * Params: { "server": "10.10.99.98", "port": 58090 }
 */
static esp_err_t api_monitor_set_server(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *server_item = cJSON_GetObjectItem(params, "server");
    const cJSON *port_item = cJSON_GetObjectItem(params, "port");
    
    if (!cJSON_IsString(server_item) || strlen(server_item->valuestring) == 0) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing or invalid 'server' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *server_ip = server_item->valuestring;
    uint16_t port = cJSON_IsNumber(port_item) ? (uint16_t)port_item->valueint : 0;
    
    esp_err_t ret = ts_compute_monitor_set_server(server_ip, port);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to set server config");
        return ret;
    }
    
    /* 返回更新后的配置 */
    ts_compute_config_t config;
    ts_compute_monitor_get_config(&config);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "server", config.server_ip);
    cJSON_AddNumberToObject(data, "port", config.server_port);
    cJSON_AddBoolToObject(data, "saved", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief monitor.start - Start Compute monitoring
 */
static esp_err_t api_monitor_start(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    if (!ts_compute_monitor_is_initialized()) {
        esp_err_t ret = ts_compute_monitor_init(NULL);
        if (ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to initialize");
            return ret;
        }
    }
    
    if (ts_compute_monitor_is_running()) {
        ts_api_result_ok(result, NULL);
        return ESP_OK;
    }
    
    esp_err_t ret = ts_compute_monitor_start();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to start");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "started", true);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief agx.stop - Stop Compute monitoring
 */
static esp_err_t api_monitor_stop(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    if (!ts_compute_monitor_is_running()) {
        ts_api_result_ok(result, NULL);
        return ESP_OK;
    }
    
    esp_err_t ret = ts_compute_monitor_stop();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to stop");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "stopped", true);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_api_monitor_register(void)
{
    TS_LOGI(TAG, "Registering monitor APIs...");
    
    static const ts_api_endpoint_t endpoints[] = {
        {
            .name = "monitor.status",
            .description = "Get Compute monitor status",
            .category = TS_API_CAT_DEVICE,
            .handler = api_monitor_status,
            .requires_auth = false,
        },
        {
            .name = "monitor.data",
            .description = "Get latest Compute telemetry data",
            .category = TS_API_CAT_DEVICE,
            .handler = api_monitor_data,
            .requires_auth = false,
        },
        {
            .name = "monitor.config",
            .description = "Get Compute monitor configuration",
            .category = TS_API_CAT_DEVICE,
            .handler = api_monitor_config,
            .requires_auth = false,
        },
        {
            .name = "monitor.set_server",
            .description = "Set compute monitoring server IP and port",
            .category = TS_API_CAT_DEVICE,
            .handler = api_monitor_set_server,
            .requires_auth = true,
        },
        {
            .name = "monitor.start",
            .description = "Start Compute monitoring",
            .category = TS_API_CAT_DEVICE,
            .handler = api_monitor_start,
            .requires_auth = true,
        },
        {
            .name = "monitor.stop",
            .description = "Stop Compute monitoring",
            .category = TS_API_CAT_DEVICE,
            .handler = api_monitor_stop,
            .requires_auth = true,
        },
    };
    
    esp_err_t ret = ts_api_register_multiple(endpoints, sizeof(endpoints) / sizeof(endpoints[0]));
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Monitor APIs registered: %d endpoints", (int)(sizeof(endpoints) / sizeof(endpoints[0])));
    } else {
        TS_LOGE(TAG, "Failed to register monitor APIs: %s", esp_err_to_name(ret));
    }
    return ret;
}
