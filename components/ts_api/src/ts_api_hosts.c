/**
 * @file ts_api_hosts.c
 * @brief SSH Known Hosts API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_known_hosts.h"
#include "ts_log.h"
#include <string.h>

#define TAG "api_hosts"

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief hosts.list - List all known hosts
 */
static esp_err_t api_hosts_list(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    TS_LOGI(TAG, "API: hosts.list called");
    
    ts_known_host_t hosts[32];
    size_t count = 0;
    
    esp_err_t ret = ts_known_hosts_list(hosts, 32, &count);
    
    TS_LOGI(TAG, "API: ts_known_hosts_list returned %s, count=%zu", esp_err_to_name(ret), count);
    
    // 如果模块未初始化，返回空列表而不是错误
    if (ret == ESP_ERR_INVALID_STATE) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "count", 0);
        cJSON_AddArrayToObject(data, "hosts");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to list known hosts");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddNumberToObject(data, "count", count);
    cJSON *hosts_array = cJSON_AddArrayToObject(data, "hosts");
    
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "host", hosts[i].host);
        cJSON_AddNumberToObject(item, "port", hosts[i].port);
        cJSON_AddStringToObject(item, "type", ts_host_key_type_str(hosts[i].type));
        cJSON_AddStringToObject(item, "fingerprint", hosts[i].fingerprint);
        cJSON_AddNumberToObject(item, "added", hosts[i].added_time);
        cJSON_AddItemToArray(hosts_array, item);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief hosts.info - Get host info
 * 
 * @param params { "host": "ip", "port": 22 }
 */
static esp_err_t api_hosts_info(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *host = cJSON_GetObjectItem(params, "host");
    if (!host || !cJSON_IsString(host)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'host' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    int port = 22;
    const cJSON *port_json = cJSON_GetObjectItem(params, "port");
    if (port_json && cJSON_IsNumber(port_json)) {
        port = port_json->valueint;
    }
    
    ts_known_host_t info = {0};
    esp_err_t ret = ts_known_hosts_get(host->valuestring, port, &info);
    
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Host not found");
        return ESP_ERR_NOT_FOUND;
    } else if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get host info");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(data, "host", info.host);
    cJSON_AddNumberToObject(data, "port", info.port);
    cJSON_AddStringToObject(data, "type", ts_host_key_type_str(info.type));
    cJSON_AddStringToObject(data, "fingerprint", info.fingerprint);
    cJSON_AddNumberToObject(data, "added", info.added_time);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief hosts.remove - Remove a known host
 * 
 * @param params { "host": "ip", "port": 22 }
 */
static esp_err_t api_hosts_remove(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *host = cJSON_GetObjectItem(params, "host");
    if (!host || !cJSON_IsString(host)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'host' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    int port = 22;
    const cJSON *port_json = cJSON_GetObjectItem(params, "port");
    if (port_json && cJSON_IsNumber(port_json)) {
        port = port_json->valueint;
    }
    
    esp_err_t ret = ts_known_hosts_remove(host->valuestring, port);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to remove host");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "removed", true);
    cJSON_AddStringToObject(data, "host", host->valuestring);
    cJSON_AddNumberToObject(data, "port", port);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief hosts.update - Force update a host fingerprint
 * 
 * 强制更新已知主机的指纹。用于当服务器重装导致指纹变化时。
 * 
 * Params: { "host": "192.168.1.100", "port": 22, "fingerprint": "abc123..." }
 */
static esp_err_t api_hosts_update(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *host = cJSON_GetObjectItem(params, "host");
    if (!host || !cJSON_IsString(host)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing host parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    int port = 22;
    const cJSON *port_json = cJSON_GetObjectItem(params, "port");
    if (port_json && cJSON_IsNumber(port_json)) {
        port = port_json->valueint;
    }
    
    /* 先删除旧的 */
    esp_err_t ret = ts_known_hosts_remove(host->valuestring, port);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to remove old host key");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "updated", true);
    cJSON_AddStringToObject(data, "host", host->valuestring);
    cJSON_AddNumberToObject(data, "port", port);
    cJSON_AddStringToObject(data, "message", "Old host key removed. Reconnect to trust the new key.");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief hosts.clear - Clear all known hosts
 */
static esp_err_t api_hosts_clear(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    esp_err_t ret = ts_known_hosts_clear();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to clear hosts");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "cleared", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_api_hosts_register(void)
{
    static const ts_api_endpoint_t endpoints[] = {
        {
            .name = "hosts.list",
            .description = "List all known SSH hosts",
            .category = TS_API_CAT_NETWORK,
            .handler = api_hosts_list,
            .requires_auth = false,
        },
        {
            .name = "hosts.info",
            .description = "Get known host info",
            .category = TS_API_CAT_NETWORK,
            .handler = api_hosts_info,
            .requires_auth = false,
        },
        {
            .name = "hosts.remove",
            .description = "Remove a known host",
            .category = TS_API_CAT_NETWORK,
            .handler = api_hosts_remove,
            .requires_auth = true,
        },
        {
            .name = "hosts.update",
            .description = "Force update a host fingerprint",
            .category = TS_API_CAT_NETWORK,
            .handler = api_hosts_update,
            .requires_auth = true,
        },
        {
            .name = "hosts.clear",
            .description = "Clear all known hosts",
            .category = TS_API_CAT_NETWORK,
            .handler = api_hosts_clear,
            .requires_auth = true,
        },
    };
    
    return ts_api_register_multiple(endpoints, sizeof(endpoints) / sizeof(endpoints[0]));
}
