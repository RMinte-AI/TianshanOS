/**
 * @file ts_api_dhcp.c
 * @brief DHCP Server API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_dhcp_server.h"
#include "ts_log.h"
#include <string.h>

#define TAG "api_dhcp"

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static ts_dhcp_if_t parse_iface(const char *str)
{
    if (!str || str[0] == '\0') return TS_DHCP_IF_AP;
    if (strcmp(str, "ap") == 0 || strcmp(str, "wifi") == 0) return TS_DHCP_IF_AP;
    if (strcmp(str, "eth") == 0 || strcmp(str, "ethernet") == 0) return TS_DHCP_IF_ETH;
    return TS_DHCP_IF_AP;
}

static const char *iface_name(ts_dhcp_if_t iface)
{
    switch (iface) {
        case TS_DHCP_IF_AP:  return "WiFi AP";
        case TS_DHCP_IF_ETH: return "Ethernet";
        default:             return "Unknown";
    }
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief dhcp.status - Get DHCP server status
 * 
 * @param params { "interface": "ap" | "eth" } (optional)
 */
static void add_recovery_status(cJSON *data, ts_dhcp_if_t iface, const ts_dhcp_status_t *status)
{
    cJSON_AddBoolToObject(data, "interface_present", status->interface_present);
    cJSON_AddNumberToObject(data, "restore_error", status->restore_error);
    cJSON_AddNumberToObject(data, "apply_error", status->apply_error);
    if (iface == TS_DHCP_IF_ETH) {
        cJSON_AddBoolToObject(data, "want_running", status->want_running);
        cJSON_AddBoolToObject(data, "link_up", status->link_up);
        cJSON_AddNumberToObject(data, "resume_error", status->resume_error);
    }
}

static esp_err_t api_dhcp_status(const cJSON *params, ts_api_result_t *result)
{
    const char *iface_str = NULL;
    if (params) {
        const cJSON *iface = cJSON_GetObjectItem(params, "interface");
        if (iface && cJSON_IsString(iface)) {
            iface_str = iface->valuestring;
        }
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* If interface specified, return single status; else return all */
    if (iface_str && strcmp(iface_str, "all") != 0) {
        ts_dhcp_if_t iface = parse_iface(iface_str);
        ts_dhcp_status_t status;
        ts_dhcp_config_t config;
        
        esp_err_t ret = ts_dhcp_server_get_status(iface, &status);
        if (ret != ESP_OK) {
            cJSON_Delete(data);
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get status");
            return ret;
        }
        ts_dhcp_server_get_config(iface, &config);
        
        add_recovery_status(data, iface, &status);
        cJSON_AddStringToObject(data, "interface", ts_dhcp_if_to_str(iface));
        cJSON_AddStringToObject(data, "display_name", iface_name(iface));
        cJSON_AddStringToObject(data, "state", ts_dhcp_state_to_str(status.state));
        cJSON_AddBoolToObject(data, "running", status.state == TS_DHCP_STATE_RUNNING);
        cJSON_AddNumberToObject(data, "active_leases", status.active_leases);
        cJSON_AddNumberToObject(data, "total_offers", status.total_offers);
        cJSON_AddNumberToObject(data, "pool_size", status.total_pool_size);
        cJSON_AddNumberToObject(data, "available", status.available_count);
        cJSON_AddNumberToObject(data, "uptime_sec", status.uptime_sec);
        
        cJSON *pool = cJSON_AddObjectToObject(data, "pool");
        cJSON_AddStringToObject(pool, "start", config.pool.start_ip);
        cJSON_AddStringToObject(pool, "end", config.pool.end_ip);
        cJSON_AddStringToObject(pool, "gateway", config.pool.gateway);
        cJSON_AddStringToObject(pool, "netmask", config.pool.netmask);
        cJSON_AddStringToObject(pool, "dns", config.pool.dns1);
        cJSON_AddNumberToObject(pool, "lease_min", config.lease_time_min);
    } else {
        /* Return all interfaces */
        const ts_dhcp_if_t interfaces[] = { TS_DHCP_IF_AP, TS_DHCP_IF_ETH };
        cJSON *ifaces = cJSON_AddArrayToObject(data, "interfaces");
        
        for (size_t i = 0; i < sizeof(interfaces)/sizeof(interfaces[0]); i++) {
            ts_dhcp_status_t status;
            ts_dhcp_config_t config;
            
            esp_err_t ret = ts_dhcp_server_get_status(interfaces[i], &status);
            if (ret != ESP_OK) {
                memset(&status, 0, sizeof(status));
                status.state = TS_DHCP_STATE_STOPPED;
            }
            ts_dhcp_server_get_config(interfaces[i], &config);
            
            cJSON *item = cJSON_CreateObject();
            add_recovery_status(item, interfaces[i], &status);
            cJSON_AddStringToObject(item, "interface", ts_dhcp_if_to_str(interfaces[i]));
            cJSON_AddStringToObject(item, "display_name", iface_name(interfaces[i]));
            cJSON_AddStringToObject(item, "state", ts_dhcp_state_to_str(status.state));
            cJSON_AddBoolToObject(item, "running", status.state == TS_DHCP_STATE_RUNNING);
            cJSON_AddNumberToObject(item, "active_leases", status.active_leases);
            cJSON_AddStringToObject(item, "pool_start", config.pool.start_ip);
            cJSON_AddStringToObject(item, "pool_end", config.pool.end_ip);
            
            cJSON_AddItemToArray(ifaces, item);
        }
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief dhcp.clients - List DHCP clients
 * 
 * @param params { "interface": "ap" | "eth" } (optional)
 */
static esp_err_t api_dhcp_clients(const cJSON *params, ts_api_result_t *result)
{
    ts_dhcp_if_t iface = TS_DHCP_IF_AP;
    if (params) {
        const cJSON *iface_json = cJSON_GetObjectItem(params, "interface");
        if (iface_json && cJSON_IsString(iface_json)) {
            iface = parse_iface(iface_json->valuestring);
        }
    }
    
    ts_dhcp_client_t clients[TS_DHCP_MAX_CLIENTS];
    size_t count = 0;
    
    esp_err_t ret = ts_dhcp_server_get_clients(iface, clients, TS_DHCP_MAX_CLIENTS, &count);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get clients");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(data, "interface", ts_dhcp_if_to_str(iface));
    cJSON_AddNumberToObject(data, "count", count);
    
    cJSON *clients_array = cJSON_AddArrayToObject(data, "clients");
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ip", clients[i].ip);
        
        char mac[18];
        ts_dhcp_mac_array_to_str(clients[i].mac, mac, sizeof(mac));
        cJSON_AddStringToObject(item, "mac", mac);
        
        cJSON_AddStringToObject(item, "hostname", clients[i].hostname);
        cJSON_AddBoolToObject(item, "is_static", clients[i].is_static);
        cJSON_AddNumberToObject(item, "lease_start", clients[i].lease_start);
        cJSON_AddNumberToObject(item, "lease_expire", clients[i].lease_expire);
        
        cJSON_AddItemToArray(clients_array, item);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief dhcp.start - Start DHCP server
 */
static esp_err_t api_dhcp_start(const cJSON *params, ts_api_result_t *result)
{
    ts_dhcp_if_t iface = TS_DHCP_IF_AP;
    if (params) {
        const cJSON *iface_json = cJSON_GetObjectItem(params, "interface");
        if (iface_json && cJSON_IsString(iface_json)) {
            iface = parse_iface(iface_json->valuestring);
        }
    }
    
    esp_err_t ret = ts_dhcp_server_start(iface);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to start DHCP server");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "started", true);
    cJSON_AddStringToObject(data, "interface", ts_dhcp_if_to_str(iface));
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief dhcp.stop - Stop DHCP server
 */
static esp_err_t api_dhcp_stop(const cJSON *params, ts_api_result_t *result)
{
    ts_dhcp_if_t iface = TS_DHCP_IF_AP;
    if (params) {
        const cJSON *iface_json = cJSON_GetObjectItem(params, "interface");
        if (iface_json && cJSON_IsString(iface_json)) {
            iface = parse_iface(iface_json->valuestring);
        }
    }
    
    esp_err_t ret = ts_dhcp_server_stop(iface);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to stop DHCP server");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "stopped", true);
    cJSON_AddStringToObject(data, "interface", ts_dhcp_if_to_str(iface));
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_api_dhcp_register(void)
{
    static const ts_api_endpoint_t endpoints[] = {
        {
            .name = "dhcp.status",
            .description = "Get DHCP server status",
            .category = TS_API_CAT_NETWORK,
            .handler = api_dhcp_status,
            .requires_auth = false,
        },
        {
            .name = "dhcp.clients",
            .description = "List DHCP clients",
            .category = TS_API_CAT_NETWORK,
            .handler = api_dhcp_clients,
            .requires_auth = false,
        },
        {
            .name = "dhcp.start",
            .description = "Start DHCP server",
            .category = TS_API_CAT_NETWORK,
            .handler = api_dhcp_start,
            .requires_auth = true,
        },
        {
            .name = "dhcp.stop",
            .description = "Stop DHCP server",
            .category = TS_API_CAT_NETWORK,
            .handler = api_dhcp_stop,
            .requires_auth = true,
        },
    };
    
    return ts_api_register_multiple(endpoints, sizeof(endpoints) / sizeof(endpoints[0]));
}
