/**
 * @file ts_api_service.c
 * @brief Service Management API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_service.h"
#include "ts_log.h"
#include <string.h>

#define TAG "api_service"

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static const char *state_to_str(ts_service_state_t state)
{
    switch (state) {
        case TS_SERVICE_STATE_STOPPED:  return "STOPPED";
        case TS_SERVICE_STATE_STARTING: return "STARTING";
        case TS_SERVICE_STATE_RUNNING:  return "RUNNING";
        case TS_SERVICE_STATE_STOPPING: return "STOPPING";
        case TS_SERVICE_STATE_ERROR:    return "ERROR";
        default:                        return "UNKNOWN";
    }
}

static const char *phase_to_str(ts_service_phase_t phase)
{
    switch (phase) {
        case TS_SERVICE_PHASE_PLATFORM: return "PLATFORM";
        case TS_SERVICE_PHASE_CORE:     return "CORE";
        case TS_SERVICE_PHASE_HAL:      return "HAL";
        case TS_SERVICE_PHASE_DRIVER:   return "DRIVER";
        case TS_SERVICE_PHASE_NETWORK:  return "NETWORK";
        case TS_SERVICE_PHASE_SECURITY: return "SECURITY";
        case TS_SERVICE_PHASE_SERVICE:  return "SERVICE";
        case TS_SERVICE_PHASE_UI:       return "UI";
        default:                        return "UNKNOWN";
    }
}

/*===========================================================================*/
/*                      Service List Enumeration                              */
/*===========================================================================*/

typedef struct {
    cJSON *array;
} service_enum_ctx_t;

static bool service_enum_callback(ts_service_handle_t handle, 
                                   const ts_service_info_t *info, 
                                   void *user_data)
{
    service_enum_ctx_t *ctx = (service_enum_ctx_t *)user_data;
    
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddStringToObject(item, "name", info->name);
        cJSON_AddStringToObject(item, "state", state_to_str(info->state));
        cJSON_AddStringToObject(item, "phase", phase_to_str(info->phase));
        cJSON_AddBoolToObject(item, "healthy", info->healthy);
        cJSON_AddNumberToObject(item, "start_time_ms", info->start_time_ms);
        cJSON_AddNumberToObject(item, "start_duration_ms", info->start_duration_ms);
        cJSON_AddItemToArray(ctx->array, item);
    }
    
    return true;  /* Continue enumeration */
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief service.list - List all services
 */
static esp_err_t api_service_list(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* Get service stats */
    ts_service_stats_t stats;
    esp_err_t ret = ts_service_get_stats(&stats);
    if (ret != ESP_OK) {
        cJSON_Delete(data);
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get service stats");
        return ret;
    }
    
    cJSON_AddNumberToObject(data, "total", stats.total_services);
    cJSON_AddNumberToObject(data, "running", stats.running_services);
    
    /* Enumerate services */
    cJSON *services = cJSON_AddArrayToObject(data, "services");
    if (services == NULL) {
        cJSON_Delete(data);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    service_enum_ctx_t ctx = { .array = services };
    ts_service_enumerate(service_enum_callback, &ctx);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief service.status - Get specific service status
 * 
 * @param params { "name": "service_name" }
 */
static esp_err_t api_service_status(const cJSON *params, ts_api_result_t *result)
{
    /* Validate params */
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *name = cJSON_GetObjectItem(params, "name");
    if (name == NULL || !cJSON_IsString(name)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'name' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Find service */
    ts_service_handle_t handle = ts_service_find(name->valuestring);
    if (handle == NULL) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Service not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    /* Get service info */
    ts_service_info_t info;
    esp_err_t ret = ts_service_get_info(handle, &info);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get service info");
        return ret;
    }
    
    /* Build response */
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(data, "name", info.name);
    cJSON_AddStringToObject(data, "state", state_to_str(info.state));
    cJSON_AddStringToObject(data, "phase", phase_to_str(info.phase));
    cJSON_AddBoolToObject(data, "healthy", info.healthy);
    cJSON_AddNumberToObject(data, "start_time_ms", info.start_time_ms);
    cJSON_AddNumberToObject(data, "start_duration_ms", info.start_duration_ms);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief service.start - Start a service
 * 
 * @param params { "name": "service_name" }
 */
static esp_err_t api_service_start(const cJSON *params, ts_api_result_t *result)
{
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *name = cJSON_GetObjectItem(params, "name");
    if (name == NULL || !cJSON_IsString(name)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'name' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_service_handle_t handle = ts_service_find(name->valuestring);
    if (handle == NULL) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Service not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_err_t ret = ts_service_start(handle);
    if (ret != ESP_OK) {
        ts_api_result_error(result, ret == ESP_ERR_NOT_FINISHED ? TS_API_ERR_BUSY : TS_API_ERR_INTERNAL, "Failed to start service");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "name", name->valuestring);
    cJSON_AddStringToObject(data, "status", "started");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief service.stop - Stop a service
 * 
 * @param params { "name": "service_name" }
 */
static esp_err_t api_service_stop(const cJSON *params, ts_api_result_t *result)
{
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *name = cJSON_GetObjectItem(params, "name");
    if (name == NULL || !cJSON_IsString(name)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'name' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_service_handle_t handle = ts_service_find(name->valuestring);
    if (handle == NULL) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Service not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_err_t ret = ts_service_stop(handle);
    if (ret != ESP_OK) {
        ts_api_result_error(result, ret == ESP_ERR_NOT_FINISHED ? TS_API_ERR_BUSY : TS_API_ERR_INTERNAL, "Failed to stop service");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "name", name->valuestring);
    cJSON_AddStringToObject(data, "status", "stopped");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief service.restart - Restart a service
 * 
 * @param params { "name": "service_name" }
 */
static esp_err_t api_service_restart(const cJSON *params, ts_api_result_t *result)
{
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *name = cJSON_GetObjectItem(params, "name");
    if (name == NULL || !cJSON_IsString(name)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'name' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_service_handle_t handle = ts_service_find(name->valuestring);
    if (handle == NULL) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Service not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_err_t ret = ts_service_restart(handle);
    if (ret != ESP_OK) {
        ts_api_result_error(result, ret == ESP_ERR_NOT_FINISHED ? TS_API_ERR_BUSY : TS_API_ERR_INTERNAL, "Failed to restart service");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "name", name->valuestring);
    cJSON_AddStringToObject(data, "status", "restarted");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t s_service_endpoints[] = {
    {
        .name = "service.list",
        .description = "List all services",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_service_list,
        .requires_auth = false
    },
    {
        .name = "service.status",
        .description = "Get service status",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_service_status,
        .requires_auth = false
    },
    {
        .name = "service.start",
        .description = "Start a service",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_service_start,
        .requires_auth = true
    },
    {
        .name = "service.stop",
        .description = "Stop a service",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_service_stop,
        .requires_auth = true
    },
    {
        .name = "service.restart",
        .description = "Restart a service",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_service_restart,
        .requires_auth = true
    }
};

esp_err_t ts_api_service_register(void)
{
    TS_LOGI(TAG, "Registering service APIs...");
    
    esp_err_t ret = ts_api_register_multiple(
        s_service_endpoints, 
        sizeof(s_service_endpoints) / sizeof(s_service_endpoints[0])
    );
    
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Service APIs registered (%zu endpoints)", 
            sizeof(s_service_endpoints) / sizeof(s_service_endpoints[0]));
    }
    
    return ret;
}
