/**
 * @file ts_api_network.c
 * @brief Network API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_api.h"
#include "ts_log.h"
#include "ts_net.h"
#include "ts_wifi.h"
#include "ts_eth.h"
#include "ts_dhcp_server.h"
#include <string.h>

#define TAG "api_network"

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static void add_eth_config_status(cJSON *data)
{
    ts_net_if_status_t status;
    if (ts_net_manager_get_if_status(TS_NET_IF_ETH, &status) != ESP_OK) return;
    cJSON_AddNumberToObject(data, "config_error", status.config_error);
    cJSON_AddStringToObject(data, "config_error_name", esp_err_to_name(status.config_error));
    cJSON_AddBoolToObject(data, "interface_present", ts_eth_get_netif() != NULL);
}

static const char *status_to_str(ts_net_status_t status)
{
    switch (status) {
        case TS_NET_STATUS_DOWN:       return "down";
        case TS_NET_STATUS_CONNECTING: return "connecting";
        case TS_NET_STATUS_CONNECTED:  return "connected";
        case TS_NET_STATUS_ERROR:      return "error";
        default: return "unknown";
    }
}

static const char *wifi_mode_to_str(ts_wifi_mode_t mode)
{
    switch (mode) {
        case TS_WIFI_MODE_OFF:   return "off";
        case TS_WIFI_MODE_STA:   return "sta";
        case TS_WIFI_MODE_AP:    return "ap";
        case TS_WIFI_MODE_APSTA: return "apsta";
        default: return "unknown";
    }
}

static const char *auth_mode_to_str(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa";
        case WIFI_AUTH_WPA2_PSK:        return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa_wpa2";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2_wpa3";
        default: return "unknown";
    }
}

static void add_ip_info_to_json(cJSON *obj, const ts_net_ip_info_t *info)
{
    cJSON_AddStringToObject(obj, "ip", ts_net_ip_to_str(info->ip));
    cJSON_AddStringToObject(obj, "netmask", ts_net_ip_to_str(info->netmask));
    cJSON_AddStringToObject(obj, "gateway", ts_net_ip_to_str(info->gateway));
    if (info->dns1) {
        cJSON_AddStringToObject(obj, "dns1", ts_net_ip_to_str(info->dns1));
    }
    if (info->dns2) {
        cJSON_AddStringToObject(obj, "dns2", ts_net_ip_to_str(info->dns2));
    }
}

static void add_mac_to_json(cJSON *obj, const char *key, const uint8_t mac[6])
{
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(obj, key, mac_str);
}

/*===========================================================================*/
/*                          Network Status APIs                               */
/*===========================================================================*/

/**
 * @brief network.status - Get overall network status
 */
static esp_err_t api_network_status(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    
    cJSON_AddStringToObject(data, "hostname", ts_net_get_hostname());
    
    // Ethernet status
    cJSON *eth = cJSON_AddObjectToObject(data, "ethernet");
    ts_net_status_t eth_status = ts_net_get_status(TS_NET_IF_ETH);
    cJSON_AddStringToObject(eth, "status", status_to_str(eth_status));
    add_eth_config_status(eth);
    cJSON_AddBoolToObject(eth, "link_up", ts_eth_is_link_up());
    
    if (eth_status == TS_NET_STATUS_CONNECTED) {
        ts_net_ip_info_t ip_info;
        if (ts_net_get_ip_info(TS_NET_IF_ETH, &ip_info) == ESP_OK) {
            add_ip_info_to_json(eth, &ip_info);
        }
    }
    
    uint8_t mac[6];
    if (ts_net_get_mac(TS_NET_IF_ETH, mac) == ESP_OK) {
        add_mac_to_json(eth, "mac", mac);
    }
    
    // WiFi STA status
    cJSON *wifi_sta = cJSON_AddObjectToObject(data, "wifi_sta");
    ts_net_status_t sta_status = ts_net_get_status(TS_NET_IF_WIFI_STA);
    cJSON_AddStringToObject(wifi_sta, "status", status_to_str(sta_status));
    cJSON_AddBoolToObject(wifi_sta, "connected", ts_wifi_sta_is_connected());
    
    if (sta_status == TS_NET_STATUS_CONNECTED) {
        ts_net_ip_info_t ip_info;
        if (ts_net_get_ip_info(TS_NET_IF_WIFI_STA, &ip_info) == ESP_OK) {
            add_ip_info_to_json(wifi_sta, &ip_info);
        }
        cJSON_AddNumberToObject(wifi_sta, "rssi", ts_wifi_sta_get_rssi());
    }
    
    if (ts_net_get_mac(TS_NET_IF_WIFI_STA, mac) == ESP_OK) {
        add_mac_to_json(wifi_sta, "mac", mac);
    }
    
    // WiFi AP status
    cJSON *wifi_ap = cJSON_AddObjectToObject(data, "wifi_ap");
    ts_net_status_t ap_status = ts_net_get_status(TS_NET_IF_WIFI_AP);
    cJSON_AddStringToObject(wifi_ap, "status", status_to_str(ap_status));
    cJSON_AddNumberToObject(wifi_ap, "sta_count", ts_wifi_ap_get_sta_count());
    
    if (ap_status == TS_NET_STATUS_CONNECTED) {
        ts_net_ip_info_t ip_info;
        if (ts_net_get_ip_info(TS_NET_IF_WIFI_AP, &ip_info) == ESP_OK) {
            add_ip_info_to_json(wifi_ap, &ip_info);
        }
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          WiFi APIs                                         */
/*===========================================================================*/

/**
 * @brief network.wifi.mode - Get/set WiFi mode
 * @param mode: "off", "sta", "ap", "apsta" (optional)
 */
static esp_err_t api_network_wifi_mode(const cJSON *params, ts_api_result_t *result)
{
    cJSON *mode_param = cJSON_GetObjectItem(params, "mode");
    
    if (mode_param && cJSON_IsString(mode_param)) {
        ts_wifi_mode_t mode;
        const char *mode_str = mode_param->valuestring;
        
        if (strcmp(mode_str, "off") == 0) {
            mode = TS_WIFI_MODE_OFF;
        } else if (strcmp(mode_str, "sta") == 0) {
            mode = TS_WIFI_MODE_STA;
        } else if (strcmp(mode_str, "ap") == 0) {
            mode = TS_WIFI_MODE_AP;
        } else if (strcmp(mode_str, "apsta") == 0) {
            mode = TS_WIFI_MODE_APSTA;
        } else {
            ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid mode");
            return ESP_ERR_INVALID_ARG;
        }
        
        esp_err_t ret = ts_wifi_set_mode(mode);
        if (ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to set mode");
            return ret;
        }
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "mode", wifi_mode_to_str(ts_wifi_get_mode()));
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief network.wifi.scan - Scan for WiFi networks
 * 
 * NOTE: WiFi must be in STA or APSTA mode to scan.
 */
static esp_err_t api_network_wifi_scan(const cJSON *params, ts_api_result_t *result)
{
    esp_err_t ret = ts_wifi_scan_start(true);
    if (ret == ESP_ERR_INVALID_STATE) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, 
            "WiFi scan requires STA or APSTA mode. Use network.wifi.mode to enable.");
        return ret;
    }
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Scan failed");
        return ret;
    }
    
    ts_wifi_scan_result_t results[20];
    uint16_t count = 20;
    ret = ts_wifi_scan_get_results(results, &count);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get results");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(data, "networks");
    
    for (uint16_t i = 0; i < count; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(net, "channel", results[i].channel);
        cJSON_AddStringToObject(net, "auth", auth_mode_to_str(results[i].auth_mode));
        add_mac_to_json(net, "bssid", results[i].bssid);
        cJSON_AddItemToArray(networks, net);
    }
    
    cJSON_AddNumberToObject(data, "count", count);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief network.wifi.connect - Connect to WiFi network
 * @param ssid: network SSID
 * @param password: network password (optional for open networks)
 */
static esp_err_t api_network_wifi_connect(const cJSON *params, ts_api_result_t *result)
{
    cJSON *ssid_param = cJSON_GetObjectItem(params, "ssid");
    cJSON *password_param = cJSON_GetObjectItem(params, "password");
    
    if (!ssid_param || !cJSON_IsString(ssid_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'ssid' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_wifi_sta_config_t config = {0};
    strncpy(config.ssid, ssid_param->valuestring, sizeof(config.ssid) - 1);
    
    if (password_param && cJSON_IsString(password_param)) {
        strncpy(config.password, password_param->valuestring, sizeof(config.password) - 1);
    }
    
    esp_err_t ret = ts_wifi_sta_config(&config);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to configure");
        return ret;
    }
    
    ret = ts_wifi_sta_connect();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to connect");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "ssid", config.ssid);
    cJSON_AddBoolToObject(data, "connecting", true);
    
    ts_api_result_ok(result, data);
    TS_LOGI(TAG, "Connecting to WiFi: %s", config.ssid);
    return ESP_OK;
}

/**
 * @brief network.wifi.disconnect - Disconnect from WiFi
 */
static esp_err_t api_network_wifi_disconnect(const cJSON *params, ts_api_result_t *result)
{
    esp_err_t ret = ts_wifi_sta_disconnect();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Disconnect failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "disconnected", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief network.wifi.ap.config - Configure WiFi AP
 * @param ssid: AP SSID
 * @param password: AP password (empty for open)
 * @param channel: channel number (optional)
 * @param hidden: hide SSID (optional)
 */
static esp_err_t api_network_wifi_ap_config(const cJSON *params, ts_api_result_t *result)
{
    // 检查 WiFi 模式
    ts_wifi_mode_t mode = ts_wifi_get_mode();
    if (mode != TS_WIFI_MODE_AP && mode != TS_WIFI_MODE_APSTA) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, 
            "AP config requires AP or APSTA mode. Use network.wifi.mode to enable.");
        return ESP_ERR_INVALID_STATE;
    }
    
    cJSON *ssid_param = cJSON_GetObjectItem(params, "ssid");
    
    if (!ssid_param || !cJSON_IsString(ssid_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'ssid' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_wifi_ap_config_t config = {
        .channel = 6,
        .max_connections = 4,
        .hidden = false,
        .auth_mode = WIFI_AUTH_WPA2_PSK
    };
    
    strncpy(config.ssid, ssid_param->valuestring, sizeof(config.ssid) - 1);
    
    cJSON *password_param = cJSON_GetObjectItem(params, "password");
    if (password_param && cJSON_IsString(password_param)) {
        strncpy(config.password, password_param->valuestring, sizeof(config.password) - 1);
        if (strlen(config.password) == 0) {
            config.auth_mode = WIFI_AUTH_OPEN;
        }
    } else {
        config.auth_mode = WIFI_AUTH_OPEN;
    }
    
    cJSON *channel_param = cJSON_GetObjectItem(params, "channel");
    if (channel_param && cJSON_IsNumber(channel_param)) {
        config.channel = (uint8_t)cJSON_GetNumberValue(channel_param);
    }
    
    cJSON *hidden_param = cJSON_GetObjectItem(params, "hidden");
    if (hidden_param && cJSON_IsBool(hidden_param)) {
        config.hidden = cJSON_IsTrue(hidden_param);
    }
    
    esp_err_t ret = ts_wifi_ap_config(&config);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to configure AP");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "ssid", config.ssid);
    cJSON_AddNumberToObject(data, "channel", config.channel);
    cJSON_AddBoolToObject(data, "hidden", config.hidden);
    cJSON_AddStringToObject(data, "auth", auth_mode_to_str(config.auth_mode));
    cJSON_AddBoolToObject(data, "configured", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief network.wifi.ap.stations - Get connected stations
 */
static esp_err_t api_network_wifi_ap_stations(const cJSON *params, ts_api_result_t *result)
{
    // 检查 WiFi 模式
    ts_wifi_mode_t mode = ts_wifi_get_mode();
    if (mode != TS_WIFI_MODE_AP && mode != TS_WIFI_MODE_APSTA) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, 
            "AP stations requires AP or APSTA mode. Use network.wifi.mode to enable.");
        return ESP_ERR_INVALID_STATE;
    }
    
    ts_wifi_sta_info_t stations[8];
    uint8_t count = 8;
    
    esp_err_t ret = ts_wifi_ap_get_sta_list(stations, &count);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get station list");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON *sta_list = cJSON_AddArrayToObject(data, "stations");
    
    for (uint8_t i = 0; i < count; i++) {
        cJSON *sta = cJSON_CreateObject();
        add_mac_to_json(sta, "mac", stations[i].mac);
        cJSON_AddNumberToObject(sta, "rssi", stations[i].rssi);
        cJSON_AddItemToArray(sta_list, sta);
    }
    
    cJSON_AddNumberToObject(data, "count", count);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Ethernet APIs                                     */
/*===========================================================================*/

/**
 * @brief network.eth.status - Get Ethernet status
 */
static esp_err_t api_network_eth_status(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    
    add_eth_config_status(data);
    cJSON_AddBoolToObject(data, "link_up", ts_eth_is_link_up());
    cJSON_AddStringToObject(data, "status", status_to_str(ts_net_get_status(TS_NET_IF_ETH)));
    
    ts_net_ip_info_t ip_info;
    if (ts_net_get_ip_info(TS_NET_IF_ETH, &ip_info) == ESP_OK) {
        add_ip_info_to_json(data, &ip_info);
    }
    
    uint8_t mac[6];
    if (ts_net_get_mac(TS_NET_IF_ETH, mac) == ESP_OK) {
        add_mac_to_json(data, "mac", mac);
    }
    
    ts_net_stats_t stats;
    if (ts_net_get_stats(TS_NET_IF_ETH, &stats) == ESP_OK) {
        cJSON *stats_obj = cJSON_AddObjectToObject(data, "stats");
        cJSON_AddNumberToObject(stats_obj, "tx_bytes", stats.tx_bytes);
        cJSON_AddNumberToObject(stats_obj, "rx_bytes", stats.rx_bytes);
        cJSON_AddNumberToObject(stats_obj, "tx_packets", stats.tx_packets);
        cJSON_AddNumberToObject(stats_obj, "rx_packets", stats.rx_packets);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief network.hostname - Get/set hostname
 * @param hostname: new hostname (optional)
 */
static esp_err_t api_network_hostname(const cJSON *params, ts_api_result_t *result)
{
    cJSON *hostname_param = cJSON_GetObjectItem(params, "hostname");
    
    if (hostname_param && cJSON_IsString(hostname_param)) {
        esp_err_t ret = ts_net_set_hostname(hostname_param->valuestring);
        if (ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to set hostname");
            return ret;
        }
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "hostname", ts_net_get_hostname());
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/* NOTE: DHCP Server APIs moved to ts_api_dhcp.c */

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t network_endpoints[] = {
    {
        .name = "network.status",
        .description = "Get overall network status",
        .category = TS_API_CAT_NETWORK,
        .handler = api_network_status,
        .requires_auth = false,
    },
    {
        .name = "network.wifi.mode",
        .description = "Get/set WiFi mode",
        .category = TS_API_CAT_NETWORK,
        .handler = api_network_wifi_mode,
        .requires_auth = true,
        .permission = "network.config",
    },
    {
        .name = "network.wifi.scan",
        .description = "Scan for WiFi networks",
        .category = TS_API_CAT_NETWORK,
        .handler = api_network_wifi_scan,
        .requires_auth = false,
    },
    {
        .name = "network.wifi.connect",
        .description = "Connect to WiFi network",
        .category = TS_API_CAT_NETWORK,
        .handler = api_network_wifi_connect,
        .requires_auth = true,
        .permission = "network.config",
    },
    {
        .name = "network.wifi.disconnect",
        .description = "Disconnect from WiFi",
        .category = TS_API_CAT_NETWORK,
        .handler = api_network_wifi_disconnect,
        .requires_auth = true,
        .permission = "network.config",
    },
    {
        .name = "network.wifi.ap.config",
        .description = "Configure WiFi AP",
        .category = TS_API_CAT_NETWORK,
        .handler = api_network_wifi_ap_config,
        .requires_auth = true,
        .permission = "network.config",
    },
    {
        .name = "network.wifi.ap.stations",
        .description = "Get connected AP stations",
        .category = TS_API_CAT_NETWORK,
        .handler = api_network_wifi_ap_stations,
        .requires_auth = false,
    },
    {
        .name = "network.eth.status",
        .description = "Get Ethernet status",
        .category = TS_API_CAT_NETWORK,
        .handler = api_network_eth_status,
        .requires_auth = false,
    },
    {
        .name = "network.hostname",
        .description = "Get/set hostname",
        .category = TS_API_CAT_NETWORK,
        .handler = api_network_hostname,
        .requires_auth = true,
        .permission = "network.config",
    },
    /* DHCP APIs are registered separately in ts_api_dhcp.c */
};

esp_err_t ts_api_network_register(void)
{
    TS_LOGI(TAG, "Registering network APIs");
    
    for (size_t i = 0; i < sizeof(network_endpoints) / sizeof(network_endpoints[0]); i++) {
        esp_err_t ret = ts_api_register(&network_endpoints[i]);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to register %s", network_endpoints[i].name);
            return ret;
        }
    }
    
    return ESP_OK;
}
