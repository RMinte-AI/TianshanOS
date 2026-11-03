/**
 * @file ts_dhcp_server.c
 * @brief TianShanOS DHCP Server Implementation
 *
 * DHCP 服务器核心实现
 * - 基于 ESP-IDF lwIP DHCP Server
 * - 配置持久化到 NVS
 * - 客户端租约跟踪
 * - 事件通知
 * - 静态 MAC-IP 地址归属（独立于活动租约）
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_dhcp_server.h"
#include "ts_log.h"
#include "ts_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "ts_dhcps_bindings.h"
#include "dhcpserver/dhcpserver.h"  /* For dhcps_lease_t */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_heap_caps.h"

/* PSRAM 优先分配宏 */
#define TS_DHCP_MALLOC(size) ({ void *p = heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); p ? p : malloc(size); })

#define TAG "ts_dhcps"

/* ============================================================================
 * 事件队列配置
 * ========================================================================== */

#define DHCP_EVENT_QUEUE_SIZE  8
#define DHCP_EVENT_TASK_STACK  3072
#define DHCP_EVENT_TASK_PRIO   5

/* IP 事件数据结构（用于队列传递） */
typedef struct {
    uint8_t mac[6];
    esp_ip4_addr_t ip;
} dhcp_ip_event_t;

/* ============================================================================
 * NVS 存储键
 * ========================================================================== */

#define NVS_NAMESPACE           "ts_dhcps"
#define NVS_KEY_AP_ENABLED      "ap_en"
#define NVS_KEY_AP_START_IP     "ap_start"
#define NVS_KEY_AP_END_IP       "ap_end"
#define NVS_KEY_AP_GATEWAY      "ap_gw"
#define NVS_KEY_AP_NETMASK      "ap_mask"
#define NVS_KEY_AP_DNS          "ap_dns"
#define NVS_KEY_AP_LEASE        "ap_lease"

/* Ethernet 接口 NVS 键 */
#define NVS_KEY_ETH_ENABLED     "eth_en"
#define NVS_KEY_ETH_START_IP    "eth_start"
#define NVS_KEY_ETH_END_IP      "eth_end"
#define NVS_KEY_ETH_GATEWAY     "eth_gw"
#define NVS_KEY_ETH_NETMASK     "eth_mask"
#define NVS_KEY_ETH_DNS         "eth_dns"
#define NVS_KEY_ETH_LEASE       "eth_lease"

/* 静态绑定 NVS 键（blob 格式存储）*/
#define NVS_KEY_AP_BINDINGS     "ap_bind"
#define NVS_KEY_ETH_BINDINGS    "eth_bind"
#define NVS_KEY_AP_BIND_CNT     "ap_bcnt"
#define NVS_KEY_ETH_BIND_CNT    "eth_bcnt"

/* ============================================================================
 * 内部状态
 * ========================================================================== */

/** 事件回调节点 */
typedef struct ts_dhcp_cb_node {
    ts_dhcp_event_cb_t callback;
    void *user_data;
    struct ts_dhcp_cb_node *next;
} ts_dhcp_cb_node_t;

/** 接口状态 */
typedef struct {
    ts_dhcp_server_state_t state;
    ts_dhcp_config_t config;
    ts_dhcp_client_t clients[TS_DHCP_MAX_CLIENTS];
    size_t client_count;
    ts_dhcp_static_binding_t static_bindings[TS_DHCP_MAX_STATIC_BINDINGS];
    size_t static_binding_count;
    uint32_t total_offers;
    TickType_t start_tick;           /* 启动时的 tick 计数 */
    esp_netif_t *netif;
} ts_dhcp_if_state_t;

/** 模块全局状态 */
typedef struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    ts_dhcp_if_state_t iface[TS_DHCP_IF_MAX];
    ts_dhcp_cb_node_t *callbacks;
    QueueHandle_t event_queue;      /* IP 事件队列 */
    TaskHandle_t event_task;        /* 事件处理任务 */
} ts_dhcp_module_state_t;

static ts_dhcp_module_state_t s_state = {0};

/* ============================================================================
 * 前向声明
 * ========================================================================== */

static void notify_event(ts_dhcp_if_t iface, ts_dhcp_event_t event, 
                          const ts_dhcp_client_t *client);
static void set_default_config(ts_dhcp_config_t *config);
static esp_err_t apply_config_to_netif(ts_dhcp_if_t iface);
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
static void dhcp_event_task(void *arg);

/* ============================================================================
 * 工具函数实现
 * ========================================================================== */

const char *ts_dhcp_state_to_str(ts_dhcp_server_state_t state)
{
    switch (state) {
        case TS_DHCP_STATE_STOPPED:  return "stopped";
        case TS_DHCP_STATE_STARTING: return "starting";
        case TS_DHCP_STATE_RUNNING:  return "running";
        case TS_DHCP_STATE_ERROR:    return "error";
        default:                     return "unknown";
    }
}

const char *ts_dhcp_if_to_str(ts_dhcp_if_t iface)
{
    switch (iface) {
        case TS_DHCP_IF_AP:  return "wifi_ap";
        case TS_DHCP_IF_ETH: return "ethernet";
        default:             return "unknown";
    }
}

const char *ts_dhcp_event_to_str(ts_dhcp_event_t event)
{
    switch (event) {
        case TS_DHCP_EVENT_STARTED:           return "started";
        case TS_DHCP_EVENT_STOPPED:           return "stopped";
        case TS_DHCP_EVENT_LEASE_NEW:         return "lease_new";
        case TS_DHCP_EVENT_LEASE_RENEW:       return "lease_renew";
        case TS_DHCP_EVENT_LEASE_EXPIRE:      return "lease_expire";
        case TS_DHCP_EVENT_CLIENT_CONNECT:    return "client_connect";
        case TS_DHCP_EVENT_CLIENT_DISCONNECT: return "client_disconnect";
        default:                              return "unknown";
    }
}

esp_err_t ts_dhcp_mac_str_to_array(const char *mac_str, uint8_t mac[6])
{
    if (!mac_str || !mac) return ESP_ERR_INVALID_ARG;
    
    int values[6];
    if (sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) {
        return ESP_ERR_INVALID_ARG;
    }
    
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)values[i];
    }
    return ESP_OK;
}

const char *ts_dhcp_mac_array_to_str(const uint8_t mac[6], char *buf, size_t buf_len)
{
    if (!mac || !buf || buf_len < 18) return "??:??:??:??:??:??";
    snprintf(buf, buf_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

/* ============================================================================
 * 内部辅助函数
 * ========================================================================== */

static void set_default_config(ts_dhcp_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->enabled = true;
    config->auto_start = true;
    config->lease_time_min = TS_DHCP_DEFAULT_LEASE_TIME;
    
    strncpy(config->pool.start_ip, TS_DHCP_DEFAULT_START_IP, TS_DHCP_IP_STR_MAX_LEN - 1);
    strncpy(config->pool.end_ip, TS_DHCP_DEFAULT_END_IP, TS_DHCP_IP_STR_MAX_LEN - 1);
    strncpy(config->pool.netmask, TS_DHCP_DEFAULT_NETMASK, TS_DHCP_IP_STR_MAX_LEN - 1);
    strncpy(config->pool.gateway, TS_DHCP_DEFAULT_GATEWAY, TS_DHCP_IP_STR_MAX_LEN - 1);
    strncpy(config->pool.dns1, TS_DHCP_DEFAULT_DNS, TS_DHCP_IP_STR_MAX_LEN - 1);
}

static void notify_event(ts_dhcp_if_t iface, ts_dhcp_event_t event, 
                          const ts_dhcp_client_t *client)
{
    ts_dhcp_cb_node_t *node = s_state.callbacks;
    while (node) {
        if (node->callback) {
            node->callback(iface, event, client, node->user_data);
        }
        node = node->next;
    }
    
    /* 发送到事件总线 */
    // ts_event_post(TS_EVENT_DHCP, event, client, sizeof(ts_dhcp_client_t), 0);
}

static esp_netif_t *get_netif_for_iface(ts_dhcp_if_t iface)
{
    esp_netif_t *netif = NULL;
    
    switch (iface) {
        case TS_DHCP_IF_AP:
            return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        case TS_DHCP_IF_ETH:
            /* 先尝试 DHCP 服务器模式的以太网接口 */
            netif = esp_netif_get_handle_from_ifkey("ETH_DHCPS");
            if (netif) return netif;
            /* 回退到默认以太网接口 */
            return esp_netif_get_handle_from_ifkey("ETH_DEF");
        default:
            return NULL;
    }
}

static esp_err_t apply_config_to_netif(ts_dhcp_if_t iface)
{
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    esp_netif_t *netif = get_netif_for_iface(iface);
    
    if (!netif) {
        TS_LOGW(TAG, "No netif for %s", ts_dhcp_if_to_str(iface));
        return ESP_ERR_NOT_FOUND;
    }
    
    if_state->netif = netif;
    
    /* 先停止 DHCP 服务器（如果正在运行） */
    esp_netif_dhcps_stop(netif);
    
    /* 
     * 对于以太网接口，不修改已经在 ts_eth.c 中配置的 IP 地址
     * ts_eth.c 已经把 ESP32 IP 设置为 10.10.99.97
     * 这里只需要配置 DHCP 服务器参数（地址池、租约、DNS等）
     * 
     * 对于 WiFi AP 接口，可能需要设置 IP（但通常也是在初始化时已经配置好）
     */
    if (iface == TS_DHCP_IF_AP) {
        /* WiFi AP: 可能需要设置 IP */
        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        ip_info.ip.addr = ipaddr_addr(if_state->config.pool.gateway);
        ip_info.netmask.addr = ipaddr_addr(if_state->config.pool.netmask);
        ip_info.gw.addr = ip_info.ip.addr;
        
        esp_err_t ret = esp_netif_set_ip_info(netif, &ip_info);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to set IP info: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        /* 以太网: 保留 ts_eth.c 中配置的 IP，只记录日志 */
        esp_netif_ip_info_t current_ip;
        if (esp_netif_get_ip_info(netif, &current_ip) == ESP_OK) {
            TS_LOGI(TAG, "Ethernet IP preserved: " IPSTR, IP2STR(&current_ip.ip));
        }
    }
    
    /* 
     * 设置 DHCP 地址池 - 使用 dhcps_lease_t 结构
     * 关键：必须设置 enable = true，否则 lwIP 会自动从 server_ip+1 开始计算 IP 池
     */
    dhcps_lease_t dhcp_lease;
    memset(&dhcp_lease, 0, sizeof(dhcp_lease));
    dhcp_lease.enable = true;  /* 必须为 true，否则 IP 池配置被忽略 */
    dhcp_lease.start_ip.addr = ipaddr_addr(if_state->config.pool.start_ip);
    dhcp_lease.end_ip.addr = ipaddr_addr(if_state->config.pool.end_ip);
    
    esp_err_t ret = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                  ESP_NETIF_REQUESTED_IP_ADDRESS,
                                  &dhcp_lease, sizeof(dhcp_lease));
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Failed to set DHCP lease pool: %s (will use auto-calculated)", esp_err_to_name(ret));
        /* 继续执行 - DHCP 服务器会自动计算 IP 池 */
    }
    
    /* 设置租约时间（分钟） */
    uint32_t lease_time = if_state->config.lease_time_min;
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                           &lease_time, sizeof(lease_time));
    
    /* 设置 DNS - 启用 DHCP DNS offer 并设置 DNS 服务器地址 */
    if (if_state->config.pool.dns1[0]) {
        /* 首先启用 DNS offer */
        uint8_t dhcps_offer_dns = OFFER_DNS;
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER,
                               &dhcps_offer_dns, sizeof(dhcps_offer_dns));
        
        /* 设置 DNS 服务器地址 */
        esp_netif_dns_info_t dns;
        dns.ip.u_addr.ip4.addr = ipaddr_addr(if_state->config.pool.dns1);
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    
    TS_LOGI(TAG, "Applied config for %s:", ts_dhcp_if_to_str(iface));
    TS_LOGI(TAG, "  Gateway: %s", if_state->config.pool.gateway);
    TS_LOGI(TAG, "  Pool:    %s - %s", if_state->config.pool.start_ip, if_state->config.pool.end_ip);
    TS_LOGI(TAG, "  Lease:   %lu min", (unsigned long)if_state->config.lease_time_min);
    
    return ESP_OK;
}

/* Caller owns the project state mutex. The synchronous bridge copies a complete
 * candidate table on the TCP/IP thread before project memory is committed. */
static esp_err_t install_static_bindings(ts_dhcp_if_t iface,
                                        const ts_dhcp_static_binding_t *bindings,
                                        size_t count)
{
    ts_dhcps_binding_table_t table = {0};
    for (size_t i = 0; i < count; i++) {
        if (!bindings[i].enabled) continue;
        ip4_addr_t ip;
        if (!ip4addr_aton(bindings[i].ip, &ip) || !ip.addr) return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < table.count; j++) {
            if (table.entries[j].ip == ip.addr ||
                !memcmp(table.entries[j].mac, bindings[i].mac, 6)) return ESP_ERR_INVALID_STATE;
        }
        ts_dhcps_binding_t *entry = &table.entries[table.count++];
        memcpy(entry->mac, bindings[i].mac, 6);
        entry->ip = ip.addr;
    }
    esp_netif_t *netif = get_netif_for_iface(iface);
    /* Startup may load NVS before the netif exists. start() installs this same
     * table on the created DHCP handle, before the server can offer addresses. */
    if (!netif) return ESP_OK;
    return ts_netif_dhcps_replace_bindings(netif, &table);
}

static esp_err_t commit_static_bindings(ts_dhcp_if_t iface,
                                       const ts_dhcp_static_binding_t *bindings,
                                       size_t count)
{
    esp_err_t ret = install_static_bindings(iface, bindings, count);
    if (ret != ESP_OK) return ret;
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    if (count) memcpy(if_state->static_bindings, bindings, count * sizeof(*bindings));
    if_state->static_binding_count = count;
    return ESP_OK;
}

/* WiFi AP 站点连接/断开事件处理 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base != WIFI_EVENT) return;
    
    /* 只处理 AP 相关事件 */
    if (event_id != WIFI_EVENT_AP_STACONNECTED && 
        event_id != WIFI_EVENT_AP_STADISCONNECTED &&
        event_id != WIFI_EVENT_AP_START &&
        event_id != WIFI_EVENT_AP_STOP) {
        return;
    }
    
    /* 注意：不使用 mutex，避免事件循环死锁 */
    ts_dhcp_if_state_t *if_state = &s_state.iface[TS_DHCP_IF_AP];
    
    switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            TS_LOGI(TAG, "Station connected: %02x:%02x:%02x:%02x:%02x:%02x, AID=%d",
                    event->mac[0], event->mac[1], event->mac[2],
                    event->mac[3], event->mac[4], event->mac[5], event->aid);
            
            /* 记录客户端（简化，不使用 mutex） */
            if (if_state->client_count < TS_DHCP_MAX_CLIENTS) {
                ts_dhcp_client_t *client = &if_state->clients[if_state->client_count];
                memset(client, 0, sizeof(*client));
                memcpy(client->mac, event->mac, 6);
                client->lease_start = (uint32_t)time(NULL);
                client->lease_expire = client->lease_start + 
                                       if_state->config.lease_time_min * 60;
                if_state->client_count++;
                if_state->total_offers++;
            }
            break;
        }
        
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            TS_LOGI(TAG, "Station disconnected: %02x:%02x:%02x:%02x:%02x:%02x, AID=%d",
                    event->mac[0], event->mac[1], event->mac[2],
                    event->mac[3], event->mac[4], event->mac[5], event->aid);
            
            /* 从列表移除 */
            for (size_t i = 0; i < if_state->client_count; i++) {
                if (memcmp(if_state->clients[i].mac, event->mac, 6) == 0) {
                    /* 移动后面的元素 */
                    for (size_t j = i; j < if_state->client_count - 1; j++) {
                        if_state->clients[j] = if_state->clients[j + 1];
                    }
                    if_state->client_count--;
                    break;
                }
            }
            break;
        }
        
        case WIFI_EVENT_AP_START:
            TS_LOGI(TAG, "AP started");
            break;
            
        case WIFI_EVENT_AP_STOP:
            TS_LOGI(TAG, "AP stopped");
            if_state->client_count = 0;
            break;
    }
}

/* IP 分配事件处理 - 将事件发送到队列，由独立任务处理
 * 
 * 这样避免在系统事件任务中使用 mutex 导致的崩溃
 */
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_id != IP_EVENT_AP_STAIPASSIGNED) return;
    if (!s_state.event_queue) return;
    
    ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
    
    /* 将事件数据发送到队列（非阻塞） */
    dhcp_ip_event_t ev;
    memcpy(ev.mac, event->mac, 6);
    ev.ip = event->ip;
    
    xQueueSend(s_state.event_queue, &ev, 0);
}

/* DHCP 事件处理任务 - 在独立任务中安全处理 IP 分配事件 */
static void dhcp_event_task(void *arg)
{
    dhcp_ip_event_t ev;
    
    TS_LOGI(TAG, "DHCP event task started");
    
    while (1) {
        /* 等待事件 */
        if (xQueueReceive(s_state.event_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        
        /* 检查模块状态 */
        if (!s_state.initialized || !s_state.mutex) {
            continue;
        }
        
        /* 获取互斥锁 */
        if (xSemaphoreTake(s_state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            TS_LOGW(TAG, "Failed to acquire mutex for client tracking");
            continue;
        }
        
        /* 检查以太网接口 DHCP 服务器状态 */
        ts_dhcp_if_state_t *if_state = &s_state.iface[TS_DHCP_IF_ETH];
        
        /* 查找 netif */
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DHCPS");
        if (!netif) {
            netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        }
        
        if (netif) {
            esp_netif_dhcp_status_t dhcp_status;
            if (esp_netif_dhcps_get_status(netif, &dhcp_status) == ESP_OK &&
                dhcp_status == ESP_NETIF_DHCP_STARTED) {
                
                /* 查找或添加客户端 */
                bool found = false;
                for (size_t i = 0; i < if_state->client_count; i++) {
                    if (memcmp(if_state->clients[i].mac, ev.mac, 6) == 0) {
                        /* 更新现有客户端 */
                        esp_ip4addr_ntoa(&ev.ip, if_state->clients[i].ip, TS_DHCP_IP_STR_MAX_LEN);
                        if_state->clients[i].lease_start = (uint32_t)time(NULL);
                        if_state->clients[i].lease_expire = if_state->clients[i].lease_start + 
                                                            if_state->config.lease_time_min * 60;
                        found = true;
                        
                        char mac_str[18];
                        ts_dhcp_mac_array_to_str(ev.mac, mac_str, sizeof(mac_str));
                        TS_LOGI(TAG, "Client renewed: %s -> %s", mac_str, if_state->clients[i].ip);
                        break;
                    }
                }
                
                /* 添加新客户端 */
                if (!found && if_state->client_count < TS_DHCP_MAX_CLIENTS) {
                    ts_dhcp_client_t *client = &if_state->clients[if_state->client_count];
                    memcpy(client->mac, ev.mac, 6);
                    esp_ip4addr_ntoa(&ev.ip, client->ip, TS_DHCP_IP_STR_MAX_LEN);
                    client->lease_start = (uint32_t)time(NULL);
                    client->lease_expire = client->lease_start + if_state->config.lease_time_min * 60;
                    client->hostname[0] = '\0';
                    if_state->client_count++;
                    if_state->total_offers++;
                    
                    char mac_str[18];
                    ts_dhcp_mac_array_to_str(ev.mac, mac_str, sizeof(mac_str));
                    TS_LOGI(TAG, "New client: %s -> %s (total: %zu)", mac_str, client->ip, if_state->client_count);
                    
                    /* 发送事件通知 */
                    notify_event(TS_DHCP_IF_ETH, TS_DHCP_EVENT_LEASE_NEW, client);
                }
            }
        }
        
        xSemaphoreGive(s_state.mutex);
    }
}

/* ============================================================================
 * 初始化和生命周期
 * ========================================================================== */

esp_err_t ts_dhcp_server_init(void)
{
    if (s_state.initialized) return ESP_OK;
    
    TS_LOGI(TAG, "Initializing DHCP server...");
    
    /* 创建互斥锁 */
    s_state.mutex = xSemaphoreCreateMutex();
    if (!s_state.mutex) {
        TS_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    /* 创建事件队列 - 用于从 IP 事件上下文安全转发事件 */
    s_state.event_queue = xQueueCreate(DHCP_EVENT_QUEUE_SIZE, sizeof(dhcp_ip_event_t));
    if (!s_state.event_queue) {
        TS_LOGE(TAG, "Failed to create event queue");
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    /* 创建事件处理任务 - 在独立任务中处理 IP 事件，避免 ISR 上下文问题 */
    BaseType_t ret = xTaskCreate(dhcp_event_task, "dhcp_evt", DHCP_EVENT_TASK_STACK, 
                                  NULL, DHCP_EVENT_TASK_PRIO, &s_state.event_task);
    if (ret != pdPASS) {
        TS_LOGE(TAG, "Failed to create event task");
        vQueueDelete(s_state.event_queue);
        s_state.event_queue = NULL;
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    /* 初始化接口状态 */
    for (int i = 0; i < TS_DHCP_IF_MAX; i++) {
        set_default_config(&s_state.iface[i].config);
        s_state.iface[i].state = TS_DHCP_STATE_STOPPED;
        s_state.iface[i].client_count = 0;
        s_state.iface[i].static_binding_count = 0;
    }
    
    /* 加载 NVS 配置 */
    ts_dhcp_server_load_config();
    
    /* 注册事件处理器 - 只注册 AP 相关事件，避免 STA 事件导致不必要的处理 */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, wifi_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, wifi_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event_handler, NULL);
    /* IP 事件现在通过队列安全处理，可以重新启用 */
    esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, ip_event_handler, NULL);
    
    s_state.initialized = true;
    TS_LOGI(TAG, "DHCP server initialized (event queue enabled)");
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_deinit(void)
{
    if (!s_state.initialized) return ESP_OK;
    
    /* 停止所有接口 */
    for (int i = 0; i < TS_DHCP_IF_MAX; i++) {
        ts_dhcp_server_stop((ts_dhcp_if_t)i);
    }
    
    /* 注销事件处理器 */
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STOP, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, ip_event_handler);
    
    /* 停止事件处理任务 */
    if (s_state.event_task) {
        vTaskDelete(s_state.event_task);
        s_state.event_task = NULL;
    }
    
    /* 删除事件队列 */
    if (s_state.event_queue) {
        vQueueDelete(s_state.event_queue);
        s_state.event_queue = NULL;
    }
    
    /* 释放回调链表 */
    ts_dhcp_cb_node_t *node = s_state.callbacks;
    while (node) {
        ts_dhcp_cb_node_t *next = node->next;
        free(node);
        node = next;
    }
    s_state.callbacks = NULL;
    
    /* 释放互斥锁 */
    if (s_state.mutex) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }
    
    s_state.initialized = false;
    TS_LOGI(TAG, "DHCP server deinitialized");
    
    return ESP_OK;
}

bool ts_dhcp_server_is_initialized(void)
{
    return s_state.initialized;
}

esp_err_t ts_dhcp_server_start(ts_dhcp_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    
    if (if_state->state == TS_DHCP_STATE_RUNNING) {
        xSemaphoreGive(s_state.mutex);
        return ESP_OK;
    }
    
    if_state->state = TS_DHCP_STATE_STARTING;
    TS_LOGI(TAG, "Starting DHCP server on %s...", ts_dhcp_if_to_str(iface));
    
    /* 应用配置 */
    esp_err_t ret = apply_config_to_netif(iface);
    if (ret != ESP_OK) {
        if_state->state = TS_DHCP_STATE_ERROR;
        xSemaphoreGive(s_state.mutex);
        return ret;
    }
    
    /* Install even an empty table before the first OFFER. */
    ret = install_static_bindings(iface, if_state->static_bindings, if_state->static_binding_count);
    if (ret != ESP_OK) {
        if_state->state = TS_DHCP_STATE_ERROR;
        xSemaphoreGive(s_state.mutex);
        return ret;
    }

    /* 启动 DHCP 服务器 */
    esp_netif_t *netif = if_state->netif;
    if (netif) {
        ret = esp_netif_dhcps_start(netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            TS_LOGE(TAG, "Failed to start DHCP server: %s", esp_err_to_name(ret));
            if_state->state = TS_DHCP_STATE_ERROR;
            xSemaphoreGive(s_state.mutex);
            return ret;
        }
    }
    
    if_state->state = TS_DHCP_STATE_RUNNING;
    if_state->start_tick = xTaskGetTickCount();
    
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "DHCP server started on %s", ts_dhcp_if_to_str(iface));
    notify_event(iface, TS_DHCP_EVENT_STARTED, NULL);
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_stop(ts_dhcp_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    
    if (if_state->state == TS_DHCP_STATE_STOPPED) {
        xSemaphoreGive(s_state.mutex);
        return ESP_OK;
    }
    
    /* 停止 DHCP 服务器 */
    esp_netif_t *netif = if_state->netif;
    if (netif) {
        esp_netif_dhcps_stop(netif);
    }
    
    if_state->state = TS_DHCP_STATE_STOPPED;
    
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "DHCP server stopped on %s", ts_dhcp_if_to_str(iface));
    notify_event(iface, TS_DHCP_EVENT_STOPPED, NULL);
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_restart(ts_dhcp_if_t iface)
{
    esp_err_t ret = ts_dhcp_server_stop(iface);
    if (ret != ESP_OK) return ret;
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    return ts_dhcp_server_start(iface);
}

/* ============================================================================
 * 状态查询
 * ========================================================================== */

esp_err_t ts_dhcp_server_get_status(ts_dhcp_if_t iface, ts_dhcp_status_t *status)
{
    if (!s_state.initialized || !s_state.mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (iface >= TS_DHCP_IF_MAX || !status) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    
    /* 直接查询 netif 的实际 DHCP 服务器状态 */
    esp_netif_t *netif = get_netif_for_iface(iface);
    
    if (netif) {
        esp_netif_dhcp_status_t dhcp_status;
        esp_err_t ret = esp_netif_dhcps_get_status(netif, &dhcp_status);
        
        if (ret == ESP_OK && dhcp_status == ESP_NETIF_DHCP_STARTED) {
            status->state = TS_DHCP_STATE_RUNNING;
        } else {
            status->state = TS_DHCP_STATE_STOPPED;
        }
    } else {
        status->state = if_state->state;
    }
    
    status->active_leases = if_state->client_count;
    status->total_offers = if_state->total_offers;
    
    /* 计算地址池大小 */
    uint32_t start = ipaddr_addr(if_state->config.pool.start_ip);
    uint32_t end = ipaddr_addr(if_state->config.pool.end_ip);
    status->total_pool_size = ntohl(end) - ntohl(start) + 1;
    status->available_count = status->total_pool_size - status->active_leases;
    
    /* 计算运行时间 - 使用 tick 计数，不依赖系统时间 */
    if (if_state->state == TS_DHCP_STATE_RUNNING && if_state->start_tick > 0) {
        TickType_t elapsed_ticks = xTaskGetTickCount() - if_state->start_tick;
        status->uptime_sec = elapsed_ticks / configTICK_RATE_HZ;
    } else {
        status->uptime_sec = 0;
    }
    
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

ts_dhcp_server_state_t ts_dhcp_server_get_state(ts_dhcp_if_t iface)
{
    if (!s_state.initialized || iface >= TS_DHCP_IF_MAX) {
        TS_LOGD(TAG, "get_state: not initialized or invalid iface");
        return TS_DHCP_STATE_STOPPED;
    }
    
    /* 直接查询 netif 的实际 DHCP 服务器状态 */
    esp_netif_t *netif = get_netif_for_iface(iface);
    TS_LOGD(TAG, "get_state: iface=%d netif=%p", iface, netif);
    
    if (netif) {
        esp_netif_dhcp_status_t dhcp_status;
        esp_err_t ret = esp_netif_dhcps_get_status(netif, &dhcp_status);
        TS_LOGD(TAG, "get_state: dhcps_get_status ret=%d status=%d", ret, dhcp_status);
        
        if (ret == ESP_OK) {
            if (dhcp_status == ESP_NETIF_DHCP_STARTED) {
                TS_LOGD(TAG, "get_state: DHCP server is STARTED -> RUNNING");
                return TS_DHCP_STATE_RUNNING;
            } else if (dhcp_status == ESP_NETIF_DHCP_INIT) {
                TS_LOGD(TAG, "get_state: DHCP server is INIT -> STOPPED");
                return TS_DHCP_STATE_STOPPED;
            } else {
                TS_LOGD(TAG, "get_state: DHCP server status=%d -> STOPPED", dhcp_status);
                return TS_DHCP_STATE_STOPPED;
            }
        } else {
            TS_LOGW(TAG, "get_state: dhcps_get_status failed: %s", esp_err_to_name(ret));
        }
    } else {
        TS_LOGW(TAG, "get_state: netif is NULL for iface %d", iface);
    }
    
    /* 回退到内部状态 */
    TS_LOGD(TAG, "get_state: fallback to internal state=%d", s_state.iface[iface].state);
    return s_state.iface[iface].state;
}

bool ts_dhcp_server_is_running(ts_dhcp_if_t iface)
{
    return ts_dhcp_server_get_state(iface) == TS_DHCP_STATE_RUNNING;
}

/* ============================================================================
 * 配置管理
 * ========================================================================== */

esp_err_t ts_dhcp_server_get_config(ts_dhcp_if_t iface, ts_dhcp_config_t *config)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !config) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    *config = s_state.iface[iface].config;
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_set_config(ts_dhcp_if_t iface, const ts_dhcp_config_t *config)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !config) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.iface[iface].config = *config;
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "Config updated for %s", ts_dhcp_if_to_str(iface));
    return ESP_OK;
}

esp_err_t ts_dhcp_server_set_pool(ts_dhcp_if_t iface, const ts_dhcp_pool_t *pool)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !pool) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.iface[iface].config.pool = *pool;
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_set_lease_time(ts_dhcp_if_t iface, uint32_t lease_time_min)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.iface[iface].config.lease_time_min = lease_time_min;
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_save_config(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    /* 保存 AP 接口配置 */
    ts_dhcp_config_t *cfg = &s_state.iface[TS_DHCP_IF_AP].config;
    nvs_set_u8(handle, NVS_KEY_AP_ENABLED, cfg->enabled ? 1 : 0);
    nvs_set_str(handle, NVS_KEY_AP_START_IP, cfg->pool.start_ip);
    nvs_set_str(handle, NVS_KEY_AP_END_IP, cfg->pool.end_ip);
    nvs_set_str(handle, NVS_KEY_AP_GATEWAY, cfg->pool.gateway);
    nvs_set_str(handle, NVS_KEY_AP_NETMASK, cfg->pool.netmask);
    nvs_set_str(handle, NVS_KEY_AP_DNS, cfg->pool.dns1);
    nvs_set_u32(handle, NVS_KEY_AP_LEASE, cfg->lease_time_min);
    
    /* 保存 AP 静态绑定 */
    ts_dhcp_if_state_t *ap_state = &s_state.iface[TS_DHCP_IF_AP];
    nvs_set_u8(handle, NVS_KEY_AP_BIND_CNT, (uint8_t)ap_state->static_binding_count);
    if (ap_state->static_binding_count > 0) {
        nvs_set_blob(handle, NVS_KEY_AP_BINDINGS, ap_state->static_bindings,
                     ap_state->static_binding_count * sizeof(ts_dhcp_static_binding_t));
    }
    
    /* 保存 ETH 接口配置 */
    cfg = &s_state.iface[TS_DHCP_IF_ETH].config;
    nvs_set_u8(handle, NVS_KEY_ETH_ENABLED, cfg->enabled ? 1 : 0);
    nvs_set_str(handle, NVS_KEY_ETH_START_IP, cfg->pool.start_ip);
    nvs_set_str(handle, NVS_KEY_ETH_END_IP, cfg->pool.end_ip);
    nvs_set_str(handle, NVS_KEY_ETH_GATEWAY, cfg->pool.gateway);
    nvs_set_str(handle, NVS_KEY_ETH_NETMASK, cfg->pool.netmask);
    nvs_set_str(handle, NVS_KEY_ETH_DNS, cfg->pool.dns1);
    nvs_set_u32(handle, NVS_KEY_ETH_LEASE, cfg->lease_time_min);
    
    /* 保存 ETH 静态绑定 */
    ts_dhcp_if_state_t *eth_state = &s_state.iface[TS_DHCP_IF_ETH];
    nvs_set_u8(handle, NVS_KEY_ETH_BIND_CNT, (uint8_t)eth_state->static_binding_count);
    if (eth_state->static_binding_count > 0) {
        nvs_set_blob(handle, NVS_KEY_ETH_BINDINGS, eth_state->static_bindings,
                     eth_state->static_binding_count * sizeof(ts_dhcp_static_binding_t));
    }
    
    xSemaphoreGive(s_state.mutex);
    
    nvs_commit(handle);
    nvs_close(handle);
    
    TS_LOGI(TAG, "Configuration saved to NVS (AP: %zu bindings, ETH: %zu bindings)",
            ap_state->static_binding_count, eth_state->static_binding_count);
    return ESP_OK;
}

esp_err_t ts_dhcp_server_load_config(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        TS_LOGI(TAG, "No saved config, using defaults");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    esp_err_t binding_result = ESP_OK;
    ts_dhcp_static_binding_t candidate[TS_DHCP_MAX_STATIC_BINDINGS];
    /* 加载 AP 接口配置 */
    ts_dhcp_config_t *cfg = &s_state.iface[TS_DHCP_IF_AP].config;
    uint8_t enabled;
    if (nvs_get_u8(handle, NVS_KEY_AP_ENABLED, &enabled) == ESP_OK) {
        cfg->enabled = (enabled != 0);
    }
    
    size_t len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_AP_START_IP, cfg->pool.start_ip, &len);
    len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_AP_END_IP, cfg->pool.end_ip, &len);
    len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_AP_GATEWAY, cfg->pool.gateway, &len);
    len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_AP_NETMASK, cfg->pool.netmask, &len);
    len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_AP_DNS, cfg->pool.dns1, &len);
    nvs_get_u32(handle, NVS_KEY_AP_LEASE, &cfg->lease_time_min);
    
    /* 加载 AP 静态绑定：包括已保存的空表，失败保留现有归属。 */
    uint8_t ap_bind_cnt = 0;
    if (nvs_get_u8(handle, NVS_KEY_AP_BIND_CNT, &ap_bind_cnt) == ESP_OK) {
        if (ap_bind_cnt > TS_DHCP_MAX_STATIC_BINDINGS) ap_bind_cnt = TS_DHCP_MAX_STATIC_BINDINGS;
        len = ap_bind_cnt * sizeof(ts_dhcp_static_binding_t);
        ret = ap_bind_cnt ? nvs_get_blob(handle, NVS_KEY_AP_BINDINGS, candidate, &len) : ESP_OK;
        if (ret == ESP_OK) ret = commit_static_bindings(TS_DHCP_IF_AP, candidate, ap_bind_cnt);
        if (ret != ESP_OK && binding_result == ESP_OK) binding_result = ret;
    }
    /* 加载 ETH 接口配置 */
    cfg = &s_state.iface[TS_DHCP_IF_ETH].config;
    if (nvs_get_u8(handle, NVS_KEY_ETH_ENABLED, &enabled) == ESP_OK) {
        cfg->enabled = (enabled != 0);
    }
    
    len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_ETH_START_IP, cfg->pool.start_ip, &len);
    len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_ETH_END_IP, cfg->pool.end_ip, &len);
    len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_ETH_GATEWAY, cfg->pool.gateway, &len);
    len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_ETH_NETMASK, cfg->pool.netmask, &len);
    len = TS_DHCP_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_ETH_DNS, cfg->pool.dns1, &len);
    nvs_get_u32(handle, NVS_KEY_ETH_LEASE, &cfg->lease_time_min);
    
    /* 加载 ETH 静态绑定：包括已保存的空表，失败保留现有归属。 */
    uint8_t eth_bind_cnt = 0;
    if (nvs_get_u8(handle, NVS_KEY_ETH_BIND_CNT, &eth_bind_cnt) == ESP_OK) {
        if (eth_bind_cnt > TS_DHCP_MAX_STATIC_BINDINGS) eth_bind_cnt = TS_DHCP_MAX_STATIC_BINDINGS;
        len = eth_bind_cnt * sizeof(ts_dhcp_static_binding_t);
        ret = eth_bind_cnt ? nvs_get_blob(handle, NVS_KEY_ETH_BINDINGS, candidate, &len) : ESP_OK;
        if (ret == ESP_OK) ret = commit_static_bindings(TS_DHCP_IF_ETH, candidate, eth_bind_cnt);
        if (ret != ESP_OK && binding_result == ESP_OK) binding_result = ret;
    }
    xSemaphoreGive(s_state.mutex);
    
    nvs_close(handle);
    
    TS_LOGI(TAG, "Configuration loaded from NVS");
    return binding_result;
}

esp_err_t ts_dhcp_server_reset_config(void)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    for (int i = 0; i < TS_DHCP_IF_MAX; i++) {
        set_default_config(&s_state.iface[i].config);
    }
    
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "Configuration reset to defaults");
    return ESP_OK;
}

/* ============================================================================
 * 客户端管理
 * ========================================================================== */

esp_err_t ts_dhcp_server_get_clients(ts_dhcp_if_t iface, ts_dhcp_client_t *clients,
                                      size_t max_count, size_t *count)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !clients || !count) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    size_t copy_count = if_state->client_count;
    if (copy_count > max_count) copy_count = max_count;
    
    memcpy(clients, if_state->clients, copy_count * sizeof(ts_dhcp_client_t));
    *count = copy_count;
    
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_get_client_by_mac(ts_dhcp_if_t iface, const uint8_t mac[6],
                                            ts_dhcp_client_t *client)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !mac || !client) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    
    for (size_t i = 0; i < if_state->client_count; i++) {
        if (memcmp(if_state->clients[i].mac, mac, 6) == 0) {
            *client = if_state->clients[i];
            xSemaphoreGive(s_state.mutex);
            return ESP_OK;
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ts_dhcp_server_get_client_by_ip(ts_dhcp_if_t iface, const char *ip,
                                           ts_dhcp_client_t *client)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !ip || !client) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    
    for (size_t i = 0; i < if_state->client_count; i++) {
        if (strcmp(if_state->clients[i].ip, ip) == 0) {
            *client = if_state->clients[i];
            xSemaphoreGive(s_state.mutex);
            return ESP_OK;
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ts_dhcp_server_release_lease(ts_dhcp_if_t iface, const uint8_t mac[6])
{
    /* ESP-IDF 的 DHCP 服务器不直接支持释放单个租约 */
    /* 这里只从我们的跟踪列表中移除 */
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !mac) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    
    for (size_t i = 0; i < if_state->client_count; i++) {
        if (memcmp(if_state->clients[i].mac, mac, 6) == 0) {
            for (size_t j = i; j < if_state->client_count - 1; j++) {
                if_state->clients[j] = if_state->clients[j + 1];
            }
            if_state->client_count--;
            break;
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_release_all_leases(ts_dhcp_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    s_state.iface[iface].client_count = 0;
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

/* ============================================================================
 * 静态绑定
 * ========================================================================== */

esp_err_t ts_dhcp_server_add_static_binding(ts_dhcp_if_t iface,
                                             const ts_dhcp_static_binding_t *binding)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !binding) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    ts_dhcp_static_binding_t candidate[TS_DHCP_MAX_STATIC_BINDINGS];
    size_t count = if_state->static_binding_count;
    memcpy(candidate, if_state->static_bindings, count * sizeof(*candidate));
    size_t index = 0;
    while (index < count && memcmp(candidate[index].mac, binding->mac, 6)) index++;
    if (index == count) {
        if (count == TS_DHCP_MAX_STATIC_BINDINGS) {
            xSemaphoreGive(s_state.mutex);
            return ESP_ERR_NO_MEM;
        }
        count++;
    }
    candidate[index] = *binding;
    esp_err_t ret = commit_static_bindings(iface, candidate, count);
    xSemaphoreGive(s_state.mutex);
    return ret;
}

esp_err_t ts_dhcp_server_remove_static_binding(ts_dhcp_if_t iface, const uint8_t mac[6])
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !mac) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    ts_dhcp_static_binding_t candidate[TS_DHCP_MAX_STATIC_BINDINGS];
    size_t count = if_state->static_binding_count;
    memcpy(candidate, if_state->static_bindings, count * sizeof(*candidate));
    for (size_t i = 0; i < count; i++) {
        if (memcmp(candidate[i].mac, mac, 6) == 0) {
            memmove(&candidate[i], &candidate[i + 1], (count - i - 1) * sizeof(*candidate));
            esp_err_t ret = commit_static_bindings(iface, candidate, count - 1);
            xSemaphoreGive(s_state.mutex);
            return ret;
        }
    }
    xSemaphoreGive(s_state.mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ts_dhcp_server_get_static_bindings(ts_dhcp_if_t iface,
                                              ts_dhcp_static_binding_t *bindings,
                                              size_t max_count, size_t *count)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !bindings || !count) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_dhcp_if_state_t *if_state = &s_state.iface[iface];
    size_t copy_count = if_state->static_binding_count;
    if (copy_count > max_count) copy_count = max_count;
    
    memcpy(bindings, if_state->static_bindings, 
           copy_count * sizeof(ts_dhcp_static_binding_t));
    *count = copy_count;
    
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_clear_static_bindings(ts_dhcp_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    esp_err_t ret = commit_static_bindings(iface, NULL, 0);
    xSemaphoreGive(s_state.mutex);
    return ret;
}

/* ============================================================================
 * 事件回调
 * ========================================================================== */

esp_err_t ts_dhcp_server_register_event_cb(ts_dhcp_event_cb_t callback, void *user_data)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    
    ts_dhcp_cb_node_t *node = TS_DHCP_MALLOC(sizeof(ts_dhcp_cb_node_t));
    if (!node) return ESP_ERR_NO_MEM;
    
    node->callback = callback;
    node->user_data = user_data;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    node->next = s_state.callbacks;
    s_state.callbacks = node;
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_dhcp_server_unregister_event_cb(ts_dhcp_event_cb_t callback)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_dhcp_cb_node_t **pp = &s_state.callbacks;
    while (*pp) {
        if ((*pp)->callback == callback) {
            ts_dhcp_cb_node_t *to_free = *pp;
            *pp = (*pp)->next;
            free(to_free);
            xSemaphoreGive(s_state.mutex);
            return ESP_OK;
        }
        pp = &(*pp)->next;
    }
    
    xSemaphoreGive(s_state.mutex);
    return ESP_ERR_NOT_FOUND;
}
