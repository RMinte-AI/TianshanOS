/**
 * @file ts_wifi.c
 * @brief WiFi Manager Implementation
 * 
 * AP 列表优先分配到 PSRAM
 */

#include "ts_wifi.h"
#include "ts_net.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM */
#include "ts_log.h"
#include "ts_event.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include <string.h>

#define TAG "ts_wifi"

static bool s_initialized = false;
static ts_wifi_mode_t s_mode = TS_WIFI_MODE_OFF;
static bool s_sta_connected = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                TS_LOGI(TAG, "WiFi STA started");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                TS_LOGI(TAG, "WiFi STA connected");
                s_sta_connected = true;
                // 不在事件处理器内发布事件，避免队列锁冲突
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
                TS_LOGI(TAG, "WiFi STA disconnected, reason: %d (%s)", 
                        disconn->reason, 
                        esp_err_to_name(disconn->reason));
                
                // 常见原因码说明
                const char *reason_str = "Unknown";
                switch (disconn->reason) {
                    case WIFI_REASON_AUTH_EXPIRE: reason_str = "AUTH_EXPIRE (authentication timeout)"; break;
                    case WIFI_REASON_AUTH_LEAVE: reason_str = "AUTH_LEAVE (station leaving)"; break;
                    case WIFI_REASON_ASSOC_EXPIRE: reason_str = "ASSOC_EXPIRE (association timeout)"; break;
                    case WIFI_REASON_ASSOC_TOOMANY: reason_str = "ASSOC_TOOMANY (too many stations)"; break;
                    case WIFI_REASON_NOT_AUTHED: reason_str = "NOT_AUTHED (not authenticated)"; break;
                    case WIFI_REASON_NOT_ASSOCED: reason_str = "NOT_ASSOCED (not associated)"; break;
                    case WIFI_REASON_ASSOC_LEAVE: reason_str = "ASSOC_LEAVE (station leaving)"; break;
                    case WIFI_REASON_ASSOC_NOT_AUTHED: reason_str = "ASSOC_NOT_AUTHED (station not authenticated)"; break;
                    case WIFI_REASON_BEACON_TIMEOUT: reason_str = "BEACON_TIMEOUT (beacon timeout)"; break;
                    case WIFI_REASON_NO_AP_FOUND: reason_str = "NO_AP_FOUND (AP not found)"; break;
                    case WIFI_REASON_AUTH_FAIL: reason_str = "AUTH_FAIL (authentication failed)"; break;
                    case WIFI_REASON_HANDSHAKE_TIMEOUT: reason_str = "HANDSHAKE_TIMEOUT (4-way handshake timeout)"; break;
                    case WIFI_REASON_CONNECTION_FAIL: reason_str = "CONNECTION_FAIL (connection failed)"; break;
                }
                TS_LOGI(TAG, "  -> Reason detail: %s", reason_str);
                
                s_sta_connected = false;
                break;
            }
            /* AP 事件由 ts_dhcp_server 处理 */
        }
    }
    /* IP 事件由 ts_net_manager 统一处理 */
}

esp_err_t ts_wifi_init(void)
{
    if (s_initialized) return ESP_OK;
    
    TS_LOGI(TAG, "Initializing WiFi");
    
    // Create default netifs
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    
    // Init WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "esp_wifi_init failed");
        return ret;
    }
    
    // Register event handlers - 只注册 STA 相关事件
    // AP 事件由 ts_dhcp_server 处理，这里不注册避免重复
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, wifi_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL);
    /* IP 事件由 ts_net_manager 统一处理，这里不注册 */
    
    s_initialized = true;
    TS_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

esp_err_t ts_wifi_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    
    // Unregister event handlers (只有 STA 事件)
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler);
    /* IP 事件由 ts_net_manager 处理 */
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
    
    s_initialized = false;
    s_mode = TS_WIFI_MODE_OFF;
    return ESP_OK;
}

esp_err_t ts_wifi_set_mode(ts_wifi_mode_t mode)
{
    wifi_mode_t esp_mode;
    esp_err_t ret;
    
    switch (mode) {
        case TS_WIFI_MODE_OFF:
            ret = esp_wifi_stop();
            if (ret != ESP_OK) return ret;
            s_mode = mode;
            TS_LOGI(TAG, "WiFi stopped");
            return ESP_OK;
        case TS_WIFI_MODE_STA:
            esp_mode = WIFI_MODE_STA;
            break;
        case TS_WIFI_MODE_AP:
            esp_mode = WIFI_MODE_AP;
            break;
        case TS_WIFI_MODE_APSTA:
            esp_mode = WIFI_MODE_APSTA;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    // 先停止 WiFi（如果已启动）
    ret = esp_wifi_stop();
    if (ret != ESP_OK) return ret;
    
    ret = esp_wifi_set_mode(esp_mode);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 启动 WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    s_mode = mode;
    TS_LOGI(TAG, "WiFi mode set to %d and started", mode);
    return ESP_OK;
}

ts_wifi_mode_t ts_wifi_get_mode(void)
{
    return s_mode;
}

esp_err_t ts_wifi_sta_config(const ts_wifi_sta_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, config->password, sizeof(wifi_config.sta.password));
    
    if (config->bssid_set) {
        memcpy(wifi_config.sta.bssid, config->bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    
    // 设置认证模式阈值：如果密码为空，使用 OPEN 模式
    bool is_open = (strlen(config->password) == 0);
    if (is_open) {
        TS_LOGI(TAG, "Configuring for OPEN WiFi (no password)");
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        // 对于开放网络，禁用 PMF
        wifi_config.sta.pmf_cfg.capable = false;
        wifi_config.sta.pmf_cfg.required = false;
    } else {
        TS_LOGI(TAG, "Configuring for encrypted WiFi (password set)");
        // 有密码时，接受 WPA2 及以上的认证方式
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        // PMF 设置为可选
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
    }
    
    TS_LOGI(TAG, "WiFi config: SSID='%s', authmode=%d, PMF capable=%d required=%d",
            wifi_config.sta.ssid, 
            wifi_config.sta.threshold.authmode,
            wifi_config.sta.pmf_cfg.capable,
            wifi_config.sta.pmf_cfg.required);
    
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

esp_err_t ts_wifi_sta_connect(void)
{
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // ESP32 连接前建议先扫描，帮助 WiFi 堆栈找到目标 AP
    // 这可以避免认证超时 (AUTH_EXPIRE) 问题
    TS_LOGI(TAG, "Performing WiFi scan before connect...");
    wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    
    ret = esp_wifi_scan_start(&scan_config, true);  // 阻塞扫描
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "WiFi scan failed: %s, trying to connect anyway", esp_err_to_name(ret));
    } else {
        TS_LOGI(TAG, "WiFi scan completed");
        
        // 获取当前配置的 SSID，检查是否在扫描结果中
        wifi_config_t current_config;
        if (esp_wifi_get_config(WIFI_IF_STA, &current_config) == ESP_OK) {
            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);
            
            if (ap_count > 0) {
                wifi_ap_record_t *ap_list = TS_MALLOC_PSRAM(ap_count * sizeof(wifi_ap_record_t));
                if (ap_list) {
                    uint16_t actual_count = ap_count;
                    if (esp_wifi_scan_get_ap_records(&actual_count, ap_list) == ESP_OK) {
                        bool found = false;
                        for (int i = 0; i < actual_count; i++) {
                            if (strcmp((char *)ap_list[i].ssid, (char *)current_config.sta.ssid) == 0) {
                                TS_LOGI(TAG, "Target AP found: SSID=%s, RSSI=%d dBm, Channel=%d, AuthMode=%d",
                                        ap_list[i].ssid, ap_list[i].rssi, ap_list[i].primary, ap_list[i].authmode);
                                found = true;
                                
                                // 检查信号强度警告
                                if (ap_list[i].rssi < -80) {
                                    TS_LOGW(TAG, "WARNING: Signal is very weak (RSSI=%d), connection may fail", ap_list[i].rssi);
                                }
                                break;
                            }
                        }
                        if (!found) {
                            TS_LOGW(TAG, "Target SSID '%s' NOT found in scan results!", current_config.sta.ssid);
                        }
                    }
                    free(ap_list);
                }
            } else {
                TS_LOGW(TAG, "No APs found in scan");
            }
        }
    }
    
    TS_LOGI(TAG, "Connecting to WiFi...");
    return esp_wifi_connect();
}

esp_err_t ts_wifi_sta_disconnect(void)
{
    return esp_wifi_disconnect();
}

bool ts_wifi_sta_is_connected(void)
{
    return s_sta_connected;
}

int8_t ts_wifi_sta_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

esp_err_t ts_wifi_ap_config(const ts_wifi_ap_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, config->ssid, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, config->password, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len = strlen(config->ssid);
    wifi_config.ap.channel = config->channel;
    wifi_config.ap.max_connection = config->max_connections;
    wifi_config.ap.authmode = config->auth_mode;
    wifi_config.ap.ssid_hidden = config->hidden;
    
    if (strlen(config->password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    return esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
}

esp_err_t ts_wifi_ap_start(void)
{
    return esp_wifi_start();
}

esp_err_t ts_wifi_ap_stop(void)
{
    return esp_wifi_stop();
}

uint8_t ts_wifi_ap_get_sta_count(void)
{
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

esp_err_t ts_wifi_ap_get_sta_list(ts_wifi_sta_info_t *list, uint8_t *count)
{
    if (!list || !count) return ESP_ERR_INVALID_ARG;
    
    wifi_sta_list_t sta_list;
    esp_err_t ret = esp_wifi_ap_get_sta_list(&sta_list);
    if (ret != ESP_OK) return ret;
    
    uint8_t max = *count;
    *count = 0;
    
    for (int i = 0; i < sta_list.num && i < max; i++) {
        memcpy(list[i].mac, sta_list.sta[i].mac, 6);
        list[i].rssi = sta_list.sta[i].rssi;
        (*count)++;
    }
    
    return ESP_OK;
}

esp_err_t ts_wifi_scan_start(bool block)
{
    // 检查 WiFi 是否已初始化
    if (!s_initialized) {
        TS_LOGE(TAG, "WiFi not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 检查当前模式是否支持扫描（需要 STA 或 APSTA 模式）
    if (s_mode != TS_WIFI_MODE_STA && s_mode != TS_WIFI_MODE_APSTA) {
        TS_LOGW(TAG, "WiFi scan requires STA or APSTA mode (current: %d)", s_mode);
        return ESP_ERR_INVALID_STATE;
    }
    
    wifi_scan_config_t scan_config = {0};
    esp_err_t ret = esp_wifi_scan_start(&scan_config, block);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t ts_wifi_scan_get_results(ts_wifi_scan_result_t *results, uint16_t *count)
{
    if (!results || !count) return ESP_ERR_INVALID_ARG;
    
    uint16_t ap_num = *count;
    wifi_ap_record_t *ap_list = TS_MALLOC_PSRAM(ap_num * sizeof(wifi_ap_record_t));
    if (!ap_list) return ESP_ERR_NO_MEM;
    
    esp_err_t ret = esp_wifi_scan_get_ap_records(&ap_num, ap_list);
    if (ret != ESP_OK) {
        free(ap_list);
        return ret;
    }
    
    for (int i = 0; i < ap_num; i++) {
        strncpy(results[i].ssid, (char *)ap_list[i].ssid, 32);
        memcpy(results[i].bssid, ap_list[i].bssid, 6);
        results[i].rssi = ap_list[i].rssi;
        results[i].channel = ap_list[i].primary;
        results[i].auth_mode = ap_list[i].authmode;
    }
    
    *count = ap_num;
    free(ap_list);
    return ESP_OK;
}

bool ts_wifi_is_connected(void)
{
    return s_sta_connected;
}

esp_netif_t *ts_wifi_get_netif(ts_wifi_if_t iface)
{
    switch (iface) {
        case TS_WIFI_IF_STA:
            return s_sta_netif;
        case TS_WIFI_IF_AP:
            return s_ap_netif;
        default:
            return NULL;
    }
}
