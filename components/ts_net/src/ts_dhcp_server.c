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
    ts_dhcp_config_t applied_config;
    bool applied_valid;
    esp_err_t config_restore_error;
    esp_err_t bindings_restore_error;
    esp_err_t apply_error;
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

typedef struct {
    ts_dhcp_if_t iface;
    esp_netif_t *netif;
    const ts_dhcp_config_t *config;
    const ts_dhcps_binding_table_t *table;
    bool reconfigure;
    bool explicit_start;
    bool installed;
    bool touched;
    esp_netif_dhcp_status_t status;
    esp_err_t cleanup_error;
} ts_dhcp_apply_t;

static ts_dhcp_module_state_t s_state = {0};

/* ============================================================================
 * 前向声明
 * ========================================================================== */

static void notify_event(ts_dhcp_if_t iface, ts_dhcp_event_t event, 
                          const ts_dhcp_client_t *client);
static void set_default_config(ts_dhcp_config_t *config);
static void set_interface_defaults(ts_dhcp_if_t iface, ts_dhcp_config_t *config);

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


static void set_interface_defaults(ts_dhcp_if_t iface, ts_dhcp_config_t *config)
{
    set_default_config(config);
    if (iface == TS_DHCP_IF_AP) {
        strcpy(config->pool.gateway, TS_DHCP_AP_DEFAULT_GATEWAY);
        strcpy(config->pool.netmask, TS_DHCP_DEFAULT_NETMASK);
        strcpy(config->pool.start_ip, TS_DHCP_AP_DEFAULT_START_IP);
        strcpy(config->pool.end_ip, TS_DHCP_AP_DEFAULT_END_IP);
        config->pool.dns1[0] = '\0'; /* SDK default DNS policy, including ADD_DNS. */
        config->lease_time_min = TS_DHCP_AP_DEFAULT_LEASE_TIME;
    }
}

static bool same_network_config(const ts_dhcp_config_t *a, const ts_dhcp_config_t *b)
{
    return a->lease_time_min == b->lease_time_min &&
        !strcmp(a->pool.start_ip, b->pool.start_ip) && !strcmp(a->pool.end_ip, b->pool.end_ip) &&
        !strcmp(a->pool.gateway, b->pool.gateway) && !strcmp(a->pool.netmask, b->pool.netmask) &&
        !strcmp(a->pool.dns1, b->pool.dns1);
}

/* Storage bytes are validated before reading a bool or parsing a string. */
static esp_err_t build_binding_table(const ts_dhcp_static_binding_t *bindings, size_t count,
                                     ts_dhcps_binding_table_t *table)
{
    if (count > TS_DHCP_MAX_STATIC_BINDINGS) return ESP_ERR_INVALID_SIZE;
    memset(table, 0, sizeof(*table));
    for (size_t i = 0; i < count; ++i) {
        unsigned char enabled;
        memcpy(&enabled, &bindings[i].enabled, sizeof(enabled));
        if (enabled > 1 || !memchr(bindings[i].ip, '\0', sizeof(bindings[i].ip)) ||
            !memchr(bindings[i].hostname, '\0', sizeof(bindings[i].hostname))) return ESP_ERR_INVALID_ARG;
        ip4_addr_t ip;
        if (!ip4addr_aton(bindings[i].ip, &ip) || !ip.addr) return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; ++j) {
            if (!memcmp(bindings[i].mac, bindings[j].mac, 6)) return ESP_ERR_INVALID_STATE;
        }
        if (!enabled) continue;
        for (size_t j = 0; j < table->count; ++j) {
            if (table->entries[j].ip == ip.addr) return ESP_ERR_INVALID_STATE;
        }
        ts_dhcps_binding_t *entry = &table->entries[table->count++];
        memcpy(entry->mac, bindings[i].mac, 6);
        entry->ip = ip.addr;
    }
    return ESP_OK;
}

static esp_err_t validate_config(const ts_dhcp_config_t *config)
{
    const char *strings[] = {config->pool.start_ip, config->pool.end_ip,
                            config->pool.gateway, config->pool.netmask, config->pool.dns1};
    ip4_addr_t ip;
    for (size_t i = 0; i < sizeof(strings) / sizeof(*strings); ++i) {
        if (!memchr(strings[i], '\0', TS_DHCP_IP_STR_MAX_LEN)) return ESP_ERR_INVALID_ARG;
        if (i == 4 && !strings[i][0]) continue;
        if (!ip4addr_aton(strings[i], &ip) || !ip.addr) return ESP_ERR_INVALID_ARG;
    }
    ip4addr_aton(config->pool.netmask, &ip);
    uint32_t host_mask = ~ntohl(ip.addr);
    if (!host_mask || (host_mask & (host_mask + 1))) return ESP_ERR_INVALID_ARG;
    if (!config->lease_time_min || config->lease_time_min > UINT32_MAX / 60) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

/* Validate against the future AP subnet, or the ETH netif's actual subnet. */
static esp_err_t validate_network(ts_dhcp_if_t iface, esp_netif_t *netif,
                                  const ts_dhcp_config_t *config, const ts_dhcps_binding_table_t *table)
{
    esp_netif_ip_info_t info = {0};
    if (iface == TS_DHCP_IF_AP) {
        info.ip.addr = ipaddr_addr(config->pool.gateway);
        info.netmask.addr = ipaddr_addr(config->pool.netmask);
    } else {
        esp_err_t ret = esp_netif_get_ip_info(netif, &info);
        if (ret != ESP_OK) return ret;
    }
    uint32_t server = ntohl(info.ip.addr), mask = ntohl(info.netmask.addr);
    uint32_t first = server & mask, last = first | ~mask;
    uint32_t start = ntohl(ipaddr_addr(config->pool.start_ip));
    uint32_t end = ntohl(ipaddr_addr(config->pool.end_ip));
    if (!mask || server <= first || server >= last || start <= first || end >= last ||
        start >= end || end - start + 1 > DHCPS_MAX_LEASE || (server >= start && server <= end)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < table->count; ++i) {
        uint32_t ip = ntohl(table->entries[i].ip);
        if (ip <= first || ip >= last || ip == server) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t configure_stopped_netif(ts_dhcp_if_t iface, esp_netif_t *netif,
                                        const ts_dhcp_config_t *config)
{
    esp_err_t ret;
    if (iface == TS_DHCP_IF_AP) {
        esp_netif_ip_info_t info = {0};
        info.ip.addr = ipaddr_addr(config->pool.gateway);
        info.netmask.addr = ipaddr_addr(config->pool.netmask);
        info.gw = info.ip;
        ret = esp_netif_set_ip_info(netif, &info);
        if (ret != ESP_OK) return ret;
    }
    dhcps_lease_t lease = {.enable = true};
    lease.start_ip.addr = ipaddr_addr(config->pool.start_ip);
    lease.end_ip.addr = ipaddr_addr(config->pool.end_ip);
    ret = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
    if (ret != ESP_OK) return ret;
    uint32_t minutes = config->lease_time_min;
    ret = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &minutes, sizeof(minutes));
    if (ret != ESP_OK) return ret;
    uint8_t offer = config->pool.dns1[0] ? OFFER_DNS : 0;
    ret = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    if (ret != ESP_OK || !offer) return ret;
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ipaddr_addr(config->pool.dns1);
    return esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
}



/* A single TCP/IP dispatch excludes interface events and DHCP packets between
 * snapshot/stop/config/table/start. No NVS, project lock or user callback here. */
static esp_err_t apply_on_tcpip(void *arg)
{
    ts_dhcp_apply_t *op = arg;
    esp_err_t ret = esp_netif_dhcps_get_status(op->netif, &op->status);
    if (ret != ESP_OK) return ret;
    esp_netif_dhcp_status_t before = op->status;
    ret = validate_network(op->iface, op->netif, op->config, op->table);
    if (ret != ESP_OK) return ret;
    if (op->reconfigure) {
        ret = esp_netif_dhcps_stop(op->netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return ret;
        op->touched = true;
        ret = configure_stopped_netif(op->iface, op->netif, op->config);
        if (ret != ESP_OK) goto failed;
    }
    ret = ts_netif_dhcps_replace_bindings(op->netif, op->table);
    if (ret != ESP_OK) goto failed;
    op->installed = true;
    if ((op->explicit_start || before != ESP_NETIF_DHCP_STOPPED) &&
        (op->reconfigure || before != ESP_NETIF_DHCP_STARTED)) {
        op->touched = true;
        ret = esp_netif_dhcps_start(op->netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) goto failed;
    }
    ret = esp_netif_dhcps_get_status(op->netif, &op->status);
    if (ret == ESP_OK) return ESP_OK;
failed:
    if (op->touched) {
        /* start failure can leave INIT: disarm automatic startup as well. */
        op->cleanup_error = esp_netif_dhcps_stop(op->netif);
        if (op->cleanup_error == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) op->cleanup_error = ESP_OK;
        esp_netif_dhcps_get_status(op->netif, &op->status);
    }
    return ret;
}

/* Caller holds the project mutex. Only the completed SDK result is committed. */
static esp_err_t apply_candidate(ts_dhcp_if_t iface, const ts_dhcp_config_t *config,
                                 const ts_dhcp_static_binding_t *bindings, size_t count,
                                 bool explicit_start, bool *installed)
{
    if (installed) *installed = false;
    ts_dhcp_if_state_t *state = &s_state.iface[iface];
    ts_dhcps_binding_table_t table;
    esp_err_t ret = validate_config(config);
    if (ret == ESP_OK) ret = build_binding_table(bindings, count, &table);
    if (ret != ESP_OK) return ret;
    esp_netif_t *netif = get_netif_for_iface(iface);
    if (!netif) {
        if (explicit_start) return ESP_ERR_NOT_FOUND;
        if (iface == TS_DHCP_IF_AP) {
            ret = validate_network(iface, NULL, config, &table);
            if (ret != ESP_OK) return ret;
        }
        /* No service can offer until a later explicit start validates its subnet. */
        state->config = *config;
        if (count && bindings != state->static_bindings) memcpy(state->static_bindings, bindings, count * sizeof(*bindings));
        state->static_binding_count = count;
        state->applied_valid = false;
        if (installed) *installed = true;
        return ESP_OK;
    }
    ts_dhcp_apply_t op = {.iface = iface, .netif = netif, .config = config, .table = &table,
        .explicit_start = explicit_start,
        .reconfigure = !state->applied_valid || state->apply_error != ESP_OK ||
                       !same_network_config(config, &state->applied_config)};
    ret = esp_netif_tcpip_exec(apply_on_tcpip, &op);
    if (installed) *installed = op.installed;
    if (op.installed) {
        state->config = *config;
        state->applied_config = *config;
        state->applied_valid = true;
        if (count && bindings != state->static_bindings) memcpy(state->static_bindings, bindings, count * sizeof(*bindings));
        state->static_binding_count = count;
    }
    if (op.touched || op.installed) {
        state->apply_error = ret;
        state->netif = netif;
        state->state = ret != ESP_OK ? TS_DHCP_STATE_ERROR :
            op.status == ESP_NETIF_DHCP_STARTED ? TS_DHCP_STATE_RUNNING : TS_DHCP_STATE_STOPPED;
        state->start_tick = ret == ESP_OK && op.status == ESP_NETIF_DHCP_STARTED ? xTaskGetTickCount() : 0;
    }
    if (op.cleanup_error != ESP_OK) TS_LOGE(TAG, "%s cleanup failed: %s", ts_dhcp_if_to_str(iface), esp_err_to_name(op.cleanup_error));
    return ret;
}

static esp_err_t install_static_bindings(ts_dhcp_if_t iface,
                                        const ts_dhcp_static_binding_t *bindings, size_t count)
{
    ts_dhcps_binding_table_t table;
    esp_err_t ret = build_binding_table(bindings, count, &table);
    if (ret != ESP_OK) return ret;
    esp_netif_t *netif = get_netif_for_iface(iface);
    if (netif) return ts_netif_dhcps_replace_bindings(netif, &table);
    return iface == TS_DHCP_IF_AP ? validate_network(iface, NULL, &s_state.iface[iface].config, &table) : ESP_OK;
}

static esp_err_t commit_static_bindings(ts_dhcp_if_t iface,
                                       const ts_dhcp_static_binding_t *bindings, size_t count)
{
    esp_err_t ret = install_static_bindings(iface, bindings, count);
    if (ret != ESP_OK) return ret;
    ts_dhcp_if_state_t *state = &s_state.iface[iface];
    if (count && bindings != state->static_bindings) memcpy(state->static_bindings, bindings, count * sizeof(*bindings));
    state->static_binding_count = count;
    return ESP_OK;
}

static bool same_bindings(const ts_dhcp_if_state_t *state,
                          const ts_dhcp_static_binding_t *bindings, size_t count)
{
    if (state->static_binding_count != count) return false;
    for (size_t i = 0; i < count; ++i) {
        const ts_dhcp_static_binding_t *old = &state->static_bindings[i];
        if (old->enabled != bindings[i].enabled || memcmp(old->mac, bindings[i].mac, 6) ||
            strcmp(old->ip, bindings[i].ip) || strcmp(old->hostname, bindings[i].hostname)) return false;
    }
    return true;
}

static esp_err_t interface_error(const ts_dhcp_if_state_t *state)
{
    if (state->config_restore_error != ESP_OK) return state->config_restore_error;
    if (state->bindings_restore_error != ESP_OK) return state->bindings_restore_error;
    return state->apply_error;
}

static const char *const *dhcp_nvs_keys(ts_dhcp_if_t iface)
{
    static const char *const keys[TS_DHCP_IF_MAX][9] = {
        {NVS_KEY_AP_ENABLED, NVS_KEY_AP_START_IP, NVS_KEY_AP_END_IP, NVS_KEY_AP_GATEWAY,
         NVS_KEY_AP_NETMASK, NVS_KEY_AP_DNS, NVS_KEY_AP_LEASE, NVS_KEY_AP_BIND_CNT, NVS_KEY_AP_BINDINGS},
        {NVS_KEY_ETH_ENABLED, NVS_KEY_ETH_START_IP, NVS_KEY_ETH_END_IP, NVS_KEY_ETH_GATEWAY,
         NVS_KEY_ETH_NETMASK, NVS_KEY_ETH_DNS, NVS_KEY_ETH_LEASE, NVS_KEY_ETH_BIND_CNT, NVS_KEY_ETH_BINDINGS}
    };
    return keys[iface];
}

/* Missing an entire interface is normal; an incomplete saved record is not. */
static esp_err_t read_saved_interface(nvs_handle_t handle, ts_dhcp_if_t iface,
                                     ts_dhcp_config_t *config, ts_dhcp_static_binding_t *bindings,
                                     size_t *count, esp_err_t *config_error, esp_err_t *binding_error)
{
    const char *const *keys = dhcp_nvs_keys(iface);
    set_interface_defaults(iface, config);
    uint8_t enabled = 1, saved_count = 0;
    esp_err_t results[8];
    results[0] = nvs_get_u8(handle, keys[0], &enabled);
    char *strings[] = {config->pool.start_ip, config->pool.end_ip, config->pool.gateway,
                       config->pool.netmask, config->pool.dns1};
    for (size_t i = 0; i < 5; ++i) {
        size_t len = TS_DHCP_IP_STR_MAX_LEN;
        results[i + 1] = nvs_get_str(handle, keys[i + 1], strings[i], &len);
    }
    results[6] = nvs_get_u32(handle, keys[6], &config->lease_time_min);
    results[7] = nvs_get_u8(handle, keys[7], &saved_count);
    bool present = false;
    for (size_t i = 0; i < 8; ++i) if (results[i] != ESP_ERR_NVS_NOT_FOUND) present = true;
    if (!present) {
        size_t len = 0;
        esp_err_t ret = nvs_get_blob(handle, keys[8], NULL, &len);
        if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
        *binding_error = ret == ESP_OK ? ESP_ERR_NVS_NOT_FOUND : ret;
        return *binding_error;
    }
    *config_error = ESP_OK;
    for (size_t i = 0; i < 7; ++i) {
        if ((i == 0 || i == 5) && results[i] == ESP_ERR_NVS_NOT_FOUND) continue;
        if (results[i] != ESP_OK && *config_error == ESP_OK) *config_error = results[i];
    }
    if (results[0] == ESP_OK && enabled > 1) *config_error = ESP_ERR_INVALID_ARG;
    config->enabled = enabled != 0;
    *binding_error = results[7];
    *count = saved_count;
    if (*binding_error == ESP_OK && saved_count > TS_DHCP_MAX_STATIC_BINDINGS) *binding_error = ESP_ERR_INVALID_SIZE;
    if (*binding_error == ESP_OK && saved_count) {
        size_t len = saved_count * sizeof(*bindings);
        *binding_error = nvs_get_blob(handle, keys[8], bindings, &len);
        if (*binding_error == ESP_OK && len != saved_count * sizeof(*bindings)) *binding_error = ESP_ERR_INVALID_SIZE;
    }
    /* Old save wrote the ETH defaults into AP even when AP was never configured.
     * Only a complete, default-valued, explicitly empty AP record is compatible. */
    bool complete = true;
    for (size_t i = 0; i < 8; ++i) if (results[i] != ESP_OK) complete = false;
    ts_dhcp_config_t legacy;
    set_default_config(&legacy);
    if (iface == TS_DHCP_IF_AP && complete && *binding_error == ESP_OK && !saved_count &&
        config->enabled == legacy.enabled && same_network_config(config, &legacy)) {
        set_interface_defaults(iface, config);
    }
    if (*config_error == ESP_OK) *config_error = validate_config(config);
    if (*binding_error == ESP_OK) {
        ts_dhcps_binding_table_t table;
        *binding_error = build_binding_table(bindings, *count, &table);
    }
    return *config_error != ESP_OK ? *config_error : *binding_error;
}

/* Statistics must describe the SDK's active lease, including the initial AP
 * auto-start before any project apply. A read failure is unknown, never pending. */
static uint32_t client_lease_expiry(ts_dhcp_if_t iface, uint32_t start)
{
    esp_netif_t *netif = get_netif_for_iface(iface);
    uint32_t minutes;
    if (!netif || esp_netif_dhcps_option(netif, ESP_NETIF_OP_GET,
        ESP_NETIF_IP_ADDRESS_LEASE_TIME, &minutes, sizeof(minutes)) != ESP_OK) return 0;
    return start + minutes * 60;
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
                client->lease_expire = client_lease_expiry(TS_DHCP_IF_AP, client->lease_start);
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
                        if_state->clients[i].lease_expire = client_lease_expiry(TS_DHCP_IF_ETH, if_state->clients[i].lease_start);
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
                    client->lease_expire = client_lease_expiry(TS_DHCP_IF_ETH, client->lease_start);
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
    if (s_state.initialized) {
        for (size_t i = 0; i < TS_DHCP_IF_MAX; ++i) {
            esp_err_t error = interface_error(&s_state.iface[i]);
            if (error != ESP_OK) return error;
        }
        return ESP_OK;
    }
    
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
        memset(&s_state.iface[i], 0, sizeof(s_state.iface[i]));
        set_interface_defaults((ts_dhcp_if_t)i, &s_state.iface[i].config);
        s_state.iface[i].state = TS_DHCP_STATE_STOPPED;
        s_state.iface[i].client_count = 0;
        s_state.iface[i].static_binding_count = 0;
    }
    
    /* 加载 NVS 配置 */
    esp_err_t load_result = ts_dhcp_server_load_config();
    
    /* 注册事件处理器 - 只注册 AP 相关事件，避免 STA 事件导致不必要的处理 */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, wifi_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, wifi_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, wifi_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, wifi_event_handler, NULL);
    /* IP 事件现在通过队列安全处理，可以重新启用 */
    esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, ip_event_handler, NULL);
    
    s_state.initialized = true;
    if (load_result == ESP_OK) TS_LOGI(TAG, "DHCP server initialized (event queue enabled)");
    else TS_LOGE(TAG, "DHCP resources initialized; configuration restore failed: %s", esp_err_to_name(load_result));
    return load_result;
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
    ts_dhcp_if_state_t *state = &s_state.iface[iface];
    esp_err_t ret = state->config_restore_error != ESP_OK ? state->config_restore_error : state->bindings_restore_error;
    if (ret == ESP_OK) ret = apply_candidate(iface, &state->config, state->static_bindings, state->static_binding_count, true, NULL);
    bool running = ret == ESP_OK && state->state == TS_DHCP_STATE_RUNNING;
    xSemaphoreGive(s_state.mutex);
    if (running) notify_event(iface, TS_DHCP_EVENT_STARTED, NULL);
    return ret;
}

esp_err_t ts_dhcp_server_stop(ts_dhcp_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    ts_dhcp_if_state_t *state = &s_state.iface[iface];
    esp_netif_t *netif = get_netif_for_iface(iface);
    esp_err_t ret = netif ? esp_netif_dhcps_stop(netif) : ESP_ERR_NOT_FOUND;
    if (ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) ret = ESP_OK;
    if (ret == ESP_OK) {
        state->state = TS_DHCP_STATE_STOPPED;
        state->start_tick = 0;
    }
    xSemaphoreGive(s_state.mutex);
    if (ret == ESP_OK) notify_event(iface, TS_DHCP_EVENT_STOPPED, NULL);
    return ret;
}

esp_err_t ts_dhcp_server_restart(ts_dhcp_if_t iface)
{
    /* Reconfiguration itself stops the server, only after complete validation. */
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    ts_dhcp_if_state_t *state = &s_state.iface[iface];
    esp_err_t ret = state->config_restore_error != ESP_OK ? state->config_restore_error : state->bindings_restore_error;
    if (ret == ESP_OK) {
        bool valid = state->applied_valid;
        state->applied_valid = false;
        ret = apply_candidate(iface, &state->config, state->static_bindings, state->static_binding_count, true, NULL);
        if (ret != ESP_OK && state->apply_error == ESP_OK) state->applied_valid = valid;
    }
    bool running = ret == ESP_OK && state->state == TS_DHCP_STATE_RUNNING;
    xSemaphoreGive(s_state.mutex);
    if (running) notify_event(iface, TS_DHCP_EVENT_STARTED, NULL);
    return ret;
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
    
    if (if_state->apply_error != ESP_OK) status->state = TS_DHCP_STATE_ERROR;
    status->active_leases = if_state->client_count;
    status->total_offers = if_state->total_offers;
    
    /* 计算地址池大小 */
    const ts_dhcp_config_t *effective = if_state->applied_valid ? &if_state->applied_config : &if_state->config;
    uint32_t start = ipaddr_addr(effective->pool.start_ip);
    uint32_t end = ipaddr_addr(effective->pool.end_ip);
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
    
    if (s_state.iface[iface].apply_error != ESP_OK) return TS_DHCP_STATE_ERROR;

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
    esp_err_t ret = validate_config(config);
    if (ret == ESP_OK) {
        s_state.iface[iface].config = *config;
        s_state.iface[iface].config_restore_error = ESP_OK;
    }
    xSemaphoreGive(s_state.mutex);
    if (ret != ESP_OK) return ret;
    
    TS_LOGI(TAG, "Config updated for %s", ts_dhcp_if_to_str(iface));
    return ESP_OK;
}

esp_err_t ts_dhcp_server_set_pool(ts_dhcp_if_t iface, const ts_dhcp_pool_t *pool)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX || !pool) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    ts_dhcp_config_t candidate = s_state.iface[iface].config;
    candidate.pool = *pool;
    esp_err_t ret = validate_config(&candidate);
    if (ret == ESP_OK) s_state.iface[iface].config = candidate;
    xSemaphoreGive(s_state.mutex);
    return ret;
}

esp_err_t ts_dhcp_server_set_lease_time(ts_dhcp_if_t iface, uint32_t lease_time_min)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    ts_dhcp_config_t candidate = s_state.iface[iface].config;
    candidate.lease_time_min = lease_time_min;
    esp_err_t ret = validate_config(&candidate);
    if (ret == ESP_OK) s_state.iface[iface].config = candidate;
    xSemaphoreGive(s_state.mutex);
    return ret;
}

esp_err_t ts_dhcp_server_save_config(void)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    for (ts_dhcp_if_t iface = 0; iface < TS_DHCP_IF_MAX; ++iface) {
        ts_dhcp_if_state_t *state = &s_state.iface[iface];
        esp_err_t ret = state->config_restore_error != ESP_OK ? state->config_restore_error : state->bindings_restore_error;
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "%s not saved: unresolved restore error %s", ts_dhcp_if_to_str(iface), esp_err_to_name(ret));
            if (result == ESP_OK) result = ret;
            continue;
        }
        ts_dhcps_binding_table_t table;
        ret = validate_config(&state->config);
        if (ret == ESP_OK) ret = build_binding_table(state->static_bindings, state->static_binding_count, &table);
        esp_netif_t *netif = get_netif_for_iface(iface);
        if (ret == ESP_OK && (netif || iface == TS_DHCP_IF_AP)) ret = validate_network(iface, netif, &state->config, &table);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "%s not saved: invalid configuration/bindings %s", ts_dhcp_if_to_str(iface), esp_err_to_name(ret));
            if (result == ESP_OK) result = ret;
            continue;
        }
        const char *const *keys = dhcp_nvs_keys(iface);
        nvs_handle_t handle;
        ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (ret != ESP_OK) { if (result == ESP_OK) result = ret; continue; }
        const ts_dhcp_config_t *cfg = &state->config;
        ret = nvs_set_u8(handle, keys[0], cfg->enabled ? 1 : 0);
        const char *strings[] = {cfg->pool.start_ip, cfg->pool.end_ip, cfg->pool.gateway, cfg->pool.netmask, cfg->pool.dns1};
        for (size_t i = 0; ret == ESP_OK && i < 5; ++i) ret = nvs_set_str(handle, keys[i + 1], strings[i]);
        if (ret == ESP_OK) ret = nvs_set_u32(handle, keys[6], cfg->lease_time_min);
        if (ret == ESP_OK && state->static_binding_count) ret = nvs_set_blob(handle, keys[8], state->static_bindings, state->static_binding_count * sizeof(*state->static_bindings));
        if (ret == ESP_OK) ret = nvs_set_u8(handle, keys[7], (uint8_t)state->static_binding_count);
        if (ret == ESP_OK) ret = nvs_commit(handle);
        nvs_close(handle);
        TS_LOGI(TAG, "%s save: %s", ts_dhcp_if_to_str(iface), esp_err_to_name(ret));
        if (result == ESP_OK && ret != ESP_OK) result = ret;
    }
    xSemaphoreGive(s_state.mutex);
    return result;
}

esp_err_t ts_dhcp_server_load_config(void)
{
    if (!s_state.mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        for (size_t i = 0; i < TS_DHCP_IF_MAX; ++i) {
            esp_err_t error = result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
            s_state.iface[i].config_restore_error = error;
            s_state.iface[i].bindings_restore_error = error;
        }
        xSemaphoreGive(s_state.mutex);
        return result == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : result;
    }
    result = ESP_OK;
    for (ts_dhcp_if_t iface = 0; iface < TS_DHCP_IF_MAX; ++iface) {
        ts_dhcp_if_state_t *state = &s_state.iface[iface];
        ts_dhcp_config_t candidate;
        ts_dhcp_static_binding_t bindings[TS_DHCP_MAX_STATIC_BINDINGS];
        size_t count = 0;
        esp_err_t config_error = ESP_OK, binding_error = ESP_OK;
        esp_err_t ret = read_saved_interface(handle, iface, &candidate, bindings, &count, &config_error, &binding_error);
        if (ret == ESP_ERR_NOT_FOUND) {
            /* Confirmed absent: no stored override remains unresolved. */
            state->config_restore_error = ESP_OK;
            state->bindings_restore_error = ESP_OK;
            continue;
        }
        if (ret != ESP_OK) {
            /* The pair is not committed on failure. A successfully READ half is
             * still unrestored when it differs from the retained memory state. */
            if (config_error == ESP_OK && (candidate.enabled != state->config.enabled ||
                !same_network_config(&candidate, &state->config))) config_error = ret;
            if (binding_error == ESP_OK && !same_bindings(state, bindings, count)) binding_error = ret;
        }
        if (ret == ESP_OK) {
            bool installed = false;
            ret = apply_candidate(iface, &candidate, bindings, count, false, &installed);
            if (ret != ESP_OK && !installed) {
                /* A rejected/uninstalled saved table must never be saved back as zero. */
                config_error = ret;
                binding_error = ret;
            }
        }
        state->config_restore_error = config_error;
        state->bindings_restore_error = binding_error;
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "%s restore failed: %s", ts_dhcp_if_to_str(iface), esp_err_to_name(ret));
            if (result == ESP_OK) result = ret;
        }
    }
    nvs_close(handle);
    xSemaphoreGive(s_state.mutex);
    return result;
}

esp_err_t ts_dhcp_server_reset_config(void)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    for (int i = 0; i < TS_DHCP_IF_MAX; i++) {
        set_interface_defaults((ts_dhcp_if_t)i, &s_state.iface[i].config);
        s_state.iface[i].config_restore_error = ESP_OK;
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
    if (if_state->bindings_restore_error != ESP_OK) {
        esp_err_t ret = if_state->bindings_restore_error;
        xSemaphoreGive(s_state.mutex);
        return ret;
    }

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
    if (if_state->bindings_restore_error != ESP_OK) {
        esp_err_t ret = if_state->bindings_restore_error;
        xSemaphoreGive(s_state.mutex);
        return ret;
    }

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
    if (if_state->bindings_restore_error != ESP_OK) {
        esp_err_t ret = if_state->bindings_restore_error;
        xSemaphoreGive(s_state.mutex);
        return ret;
    }

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
    if (ret == ESP_OK) s_state.iface[iface].bindings_restore_error = ESP_OK;
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


esp_err_t ts_dhcp_server_get_restore_error(ts_dhcp_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    const ts_dhcp_if_state_t *state = &s_state.iface[iface];
    esp_err_t ret = state->config_restore_error != ESP_OK ? state->config_restore_error : state->bindings_restore_error;
    xSemaphoreGive(s_state.mutex);
    return ret;
}


esp_err_t ts_dhcp_server_get_apply_error(ts_dhcp_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface >= TS_DHCP_IF_MAX) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    esp_err_t ret = s_state.iface[iface].apply_error;
    xSemaphoreGive(s_state.mutex);
    return ret;
}
