/**
 * @file ts_net_manager.c
 * @brief TianShanOS Network Manager Implementation
 *
 * 网络管理器核心实现
 * - 统一管理以太网和 WiFi 接口
 * - 从 pins.json 读取硬件配置
 * - 配置持久化到 NVS
 * - 事件总线集成
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_net_manager.h"
#include "ts_net.h"
#include "ts_eth.h"
#include "ts_dhcp_server.h"
#include "ts_wifi.h"
#include "ts_log.h"
#include "ts_event.h"
#include "ts_config.h"
#include "ts_pin_manager.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"

/* PSRAM 优先分配宏 */
#define TS_NET_MALLOC(size) ({ void *p = heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); p ? p : malloc(size); })

#define TAG "ts_net_mgr"

/* ============================================================================
 * NVS 存储键
 * ========================================================================== */

#define NVS_NAMESPACE           "ts_net"
#define NVS_KEY_ETH_ENABLED     "eth_en"
#define NVS_KEY_ETH_IP_MODE     "eth_ipmode"
#define NVS_KEY_ETH_IP          "eth_ip"
#define NVS_KEY_ETH_NETMASK     "eth_mask"
#define NVS_KEY_ETH_GATEWAY     "eth_gw"
#define NVS_KEY_ETH_DNS1        "eth_dns1"
#define NVS_KEY_ETH_DNS2        "eth_dns2"
#define NVS_KEY_HOSTNAME        "hostname"

/* WiFi AP NVS 键 */
#define NVS_KEY_AP_ENABLED      "ap_en"
#define NVS_KEY_AP_SSID         "ap_ssid"
#define NVS_KEY_AP_PASS         "ap_pass"
#define NVS_KEY_AP_CHANNEL      "ap_chan"
#define NVS_KEY_AP_IP           "ap_ip"

/* WiFi STA NVS 键 */
#define NVS_KEY_STA_ENABLED     "sta_en"
#define NVS_KEY_STA_SSID        "sta_ssid"
#define NVS_KEY_STA_PASS        "sta_pass"

/* ============================================================================
 * 内部状态
 * ========================================================================== */

/** 事件回调链表节点 */
typedef struct ts_net_cb_node {
    ts_net_event_cb_t callback;
    void *user_data;
    struct ts_net_cb_node *next;
} ts_net_cb_node_t;

/** 网络管理器内部状态 */
typedef struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    
    /* 接口状态 */
    ts_net_if_status_t eth_status;
    ts_net_if_status_t wifi_sta_status;
    ts_net_if_status_t wifi_ap_status;
    
    /* 接口配置 */
    ts_net_if_config_t eth_config;
    ts_net_if_config_t wifi_sta_config;
    ts_net_if_config_t wifi_ap_config;
    
    /* 全局配置 */
    char hostname[TS_NET_HOSTNAME_MAX_LEN];
    
    /* 事件回调链表 */
    ts_net_cb_node_t *callbacks;
    
    /* 连接时间戳 */
    uint32_t eth_connect_time;
    uint32_t wifi_connect_time;
} ts_net_manager_state_t;

static ts_net_manager_state_t s_state = {0};

/* ============================================================================
 * 前向声明
 * ========================================================================== */

static void net_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data);
static esp_err_t load_eth_config_from_pins(ts_eth_config_t *eth_hw_config);

/* ============================================================================
 * 工具函数
 * ========================================================================== */

uint32_t ts_net_ip_str_to_u32(const char *ip_str)
{
    if (!ip_str || ip_str[0] == '\0') return 0;
    
    int a, b, c, d;
    if (sscanf(ip_str, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        return 0;
    }
    
    return ((uint32_t)a) | (((uint32_t)b) << 8) | 
           (((uint32_t)c) << 16) | (((uint32_t)d) << 24);
}

const char *ts_net_ip_u32_to_str(uint32_t ip, char *buf, size_t buf_len)
{
    if (!buf || buf_len < 16) return "";
    
    snprintf(buf, buf_len, "%d.%d.%d.%d",
             (int)(ip & 0xFF),
             (int)((ip >> 8) & 0xFF),
             (int)((ip >> 16) & 0xFF),
             (int)((ip >> 24) & 0xFF));
    return buf;
}

const char *ts_net_state_to_str(ts_net_state_t state)
{
    switch (state) {
        case TS_NET_STATE_UNINITIALIZED: return "uninitialized";
        case TS_NET_STATE_INITIALIZED:   return "initialized";
        case TS_NET_STATE_STARTING:      return "starting";
        case TS_NET_STATE_DISCONNECTED:  return "disconnected";
        case TS_NET_STATE_CONNECTING:    return "connecting";
        case TS_NET_STATE_CONNECTED:     return "connected";
        case TS_NET_STATE_GOT_IP:        return "ready";
        case TS_NET_STATE_ERROR:         return "error";
        default:                         return "unknown";
    }
}

const char *ts_net_if_to_str(ts_net_if_t iface)
{
    switch (iface) {
        case TS_NET_IF_ETH:      return "ethernet";
        case TS_NET_IF_WIFI_STA: return "wifi_sta";
        case TS_NET_IF_WIFI_AP:  return "wifi_ap";
        default:                 return "unknown";
    }
}

/* ============================================================================
 * 从 pins.json 加载以太网硬件配置
 * ========================================================================== */

static esp_err_t load_eth_config_from_pins(ts_eth_config_t *eth_hw_config)
{
    if (!eth_hw_config) return ESP_ERR_INVALID_ARG;
    
    /* 设置默认值 */
    *eth_hw_config = (ts_eth_config_t)TS_ETH_DEFAULT_CONFIG();
    
    /* 从 pin_manager 获取引脚配置 */
    int gpio;
    
    gpio = ts_pin_manager_get_gpio(TS_PIN_FUNC_ETH_MOSI);
    if (gpio >= 0) {
        eth_hw_config->gpio_mosi = gpio;
        TS_LOGD(TAG, "ETH_MOSI: GPIO %d", gpio);
    } else {
        TS_LOGW(TAG, "ETH_MOSI not found in pins.json");
        return ESP_ERR_NOT_FOUND;
    }
    
    gpio = ts_pin_manager_get_gpio(TS_PIN_FUNC_ETH_MISO);
    if (gpio >= 0) {
        eth_hw_config->gpio_miso = gpio;
    } else {
        TS_LOGW(TAG, "ETH_MISO not found in pins.json");
        return ESP_ERR_NOT_FOUND;
    }
    
    gpio = ts_pin_manager_get_gpio(TS_PIN_FUNC_ETH_SCLK);
    if (gpio >= 0) {
        eth_hw_config->gpio_sclk = gpio;
    } else {
        TS_LOGW(TAG, "ETH_SCLK not found in pins.json");
        return ESP_ERR_NOT_FOUND;
    }
    
    gpio = ts_pin_manager_get_gpio(TS_PIN_FUNC_ETH_CS);
    if (gpio >= 0) {
        eth_hw_config->gpio_cs = gpio;
    } else {
        TS_LOGW(TAG, "ETH_CS not found in pins.json");
        return ESP_ERR_NOT_FOUND;
    }
    
    gpio = ts_pin_manager_get_gpio(TS_PIN_FUNC_ETH_INT);
    if (gpio >= 0) {
        eth_hw_config->gpio_int = gpio;
    } else {
        TS_LOGW(TAG, "ETH_INT not found in pins.json");
        return ESP_ERR_NOT_FOUND;
    }
    
    gpio = ts_pin_manager_get_gpio(TS_PIN_FUNC_ETH_RST);
    if (gpio >= 0) {
        eth_hw_config->gpio_rst = gpio;
    } else {
        TS_LOGW(TAG, "ETH_RST not found in pins.json");
        return ESP_ERR_NOT_FOUND;
    }
    
    /* SPI 主机从 Kconfig 读取 */
    eth_hw_config->spi_host = CONFIG_TS_NET_ETH_SPI_HOST;
    eth_hw_config->spi_clock_mhz = 20;  /* W5500 默认 20MHz */
    
    TS_LOGI(TAG, "Ethernet pins: MOSI=%d MISO=%d SCLK=%d CS=%d INT=%d RST=%d",
            eth_hw_config->gpio_mosi, eth_hw_config->gpio_miso,
            eth_hw_config->gpio_sclk, eth_hw_config->gpio_cs,
            eth_hw_config->gpio_int, eth_hw_config->gpio_rst);
    
    return ESP_OK;
}

/* ============================================================================
 * 事件处理
 * ========================================================================== */

/* 从 netif 更新 IP 信息（用于静态 IP / DHCP 服务器模式） */
static void update_eth_ip_from_netif(void)
{
    esp_netif_t *netif = ts_eth_get_netif();
    if (!netif) return;
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        s_state.eth_status.has_ip = true;
        s_state.eth_status.state = TS_NET_STATE_GOT_IP;
        
        ts_net_ip_u32_to_str(ip_info.ip.addr, 
                             s_state.eth_status.ip_info.ip, 
                             TS_NET_IP_STR_MAX_LEN);
        ts_net_ip_u32_to_str(ip_info.netmask.addr, 
                             s_state.eth_status.ip_info.netmask, 
                             TS_NET_IP_STR_MAX_LEN);
        ts_net_ip_u32_to_str(ip_info.gw.addr, 
                             s_state.eth_status.ip_info.gateway, 
                             TS_NET_IP_STR_MAX_LEN);
        
        /* 获取 DNS */
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            ts_net_ip_u32_to_str(dns.ip.u_addr.ip4.addr,
                                 s_state.eth_status.ip_info.dns1,
                                 TS_NET_IP_STR_MAX_LEN);
        }
        
        /* Observed addresses must not overwrite pending user configuration. */
        TS_LOGI(TAG, "Ethernet IP (static/DHCPS): %s", s_state.eth_status.ip_info.ip);
        
        /* 静态 IP 模式下，link up 后启动 mDNS（DHCP 模式由 IP_EVENT 触发） */
        ts_net_mdns_start();
    }
}

static void net_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    
    /* 只处理 ETH_EVENT */
    if (event_base != ETH_EVENT) {
        return;
    }
        switch (event_id) {
            case ETHERNET_EVENT_CONNECTED:
                TS_LOGI(TAG, "Ethernet link up");
                s_state.eth_status.link_up = true;
                s_state.eth_status.state = TS_NET_STATE_CONNECTED;
                s_state.eth_connect_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                /* 对于静态 IP / DHCP 服务器模式，直接从 netif 获取 IP */
                update_eth_ip_from_netif();
                break;
                
            case ETHERNET_EVENT_DISCONNECTED:
                TS_LOGI(TAG, "Ethernet link down");
                s_state.eth_status.link_up = false;
                s_state.eth_status.has_ip = false;
                s_state.eth_status.state = TS_NET_STATE_DISCONNECTED;
                memset(&s_state.eth_status.ip_info, 0, sizeof(s_state.eth_status.ip_info));
                break;
                
            case ETHERNET_EVENT_START:
                TS_LOGI(TAG, "Ethernet started");
                s_state.eth_status.state = TS_NET_STATE_DISCONNECTED;
                break;
                
            case ETHERNET_EVENT_STOP:
                TS_LOGI(TAG, "Ethernet stopped");
                s_state.eth_status.state = TS_NET_STATE_INITIALIZED;
                s_state.eth_status.link_up = false;
                s_state.eth_status.has_ip = false;
                break;
        }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    
    /* 只处理我们关心的事件，忽略其他（如 IP_EVENT_AP_STAIPASSIGNED） */
    if (event_id != IP_EVENT_ETH_GOT_IP && 
        event_id != IP_EVENT_ETH_LOST_IP && 
        event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    
    /* 注意：不使用 mutex，事件循环是单线程的，直接更新状态 */
    
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        
        TS_LOGI(TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        s_state.eth_status.has_ip = true;
        s_state.eth_status.state = TS_NET_STATE_GOT_IP;
        
        /* 启动 mDNS 服务（获取 IP 后） */
        ts_net_mdns_start();
        
        /* 更新 IP 信息 */
        ts_net_ip_u32_to_str(event->ip_info.ip.addr, 
                             s_state.eth_status.ip_info.ip, 
                             TS_NET_IP_STR_MAX_LEN);
        ts_net_ip_u32_to_str(event->ip_info.netmask.addr, 
                             s_state.eth_status.ip_info.netmask, 
                             TS_NET_IP_STR_MAX_LEN);
        ts_net_ip_u32_to_str(event->ip_info.gw.addr, 
                             s_state.eth_status.ip_info.gateway, 
                             TS_NET_IP_STR_MAX_LEN);
        
        /* 获取 DNS */
        esp_netif_t *netif = ts_eth_get_netif();
        if (netif) {
            esp_netif_dns_info_t dns;
            if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
                ts_net_ip_u32_to_str(dns.ip.u_addr.ip4.addr,
                                     s_state.eth_status.ip_info.dns1,
                                     TS_NET_IP_STR_MAX_LEN);
            }
        }
    }
    else if (event_id == IP_EVENT_ETH_LOST_IP) {
        TS_LOGW(TAG, "Ethernet lost IP");
        s_state.eth_status.has_ip = false;
        s_state.eth_status.state = TS_NET_STATE_CONNECTED;
        memset(&s_state.eth_status.ip_info, 0, sizeof(s_state.eth_status.ip_info));
    }
    else if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        
        TS_LOGI(TAG, "WiFi STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        s_state.wifi_sta_status.has_ip = true;
        s_state.wifi_sta_status.state = TS_NET_STATE_GOT_IP;
        
        /* 启动 mDNS 服务（获取 IP 后） */
        ts_net_mdns_start();
        
        ts_net_ip_u32_to_str(event->ip_info.ip.addr, 
                             s_state.wifi_sta_status.ip_info.ip, 
                             TS_NET_IP_STR_MAX_LEN);
        ts_net_ip_u32_to_str(event->ip_info.netmask.addr, 
                             s_state.wifi_sta_status.ip_info.netmask, 
                             TS_NET_IP_STR_MAX_LEN);
        ts_net_ip_u32_to_str(event->ip_info.gw.addr, 
                             s_state.wifi_sta_status.ip_info.gateway, 
                             TS_NET_IP_STR_MAX_LEN);
    }
}

/* ============================================================================
 * 初始化和生命周期
 * ========================================================================== */

esp_err_t ts_net_manager_init(void)
{
    if (s_state.initialized) {
        TS_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    TS_LOGI(TAG, "Initializing network manager...");
    
    /* 创建互斥锁 */
    s_state.mutex = xSemaphoreCreateMutex();
    if (!s_state.mutex) {
        TS_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    /* 初始化默认配置 */
    strncpy(s_state.hostname, TS_NET_DEFAULT_HOSTNAME, sizeof(s_state.hostname) - 1);
    
    /* 以太网默认配置 - 作为网关/DHCP服务器，默认使用静态IP */
    s_state.eth_config.enabled = true;
    s_state.eth_config.ip_mode = TS_NET_IP_MODE_STATIC;
    s_state.eth_config.auto_start = true;
    strncpy(s_state.eth_config.static_ip.ip, TS_NET_DEFAULT_IP, TS_NET_IP_STR_MAX_LEN);
    strncpy(s_state.eth_config.static_ip.netmask, TS_NET_DEFAULT_NETMASK, TS_NET_IP_STR_MAX_LEN);
    strncpy(s_state.eth_config.static_ip.gateway, TS_NET_DEFAULT_GATEWAY, TS_NET_IP_STR_MAX_LEN);
    strncpy(s_state.eth_config.static_ip.dns1, TS_NET_DEFAULT_DNS, TS_NET_IP_STR_MAX_LEN);
    
    /* 从 NVS 加载配置（如果有） */
    esp_err_t config_error = ts_net_manager_load_config();
    
    /* 初始化 ESP netif */
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        TS_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_state.mutex);
        return ret;
    }
    
    /* 创建默认事件循环（如果尚未创建）*/
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        TS_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_state.mutex);
        return ret;
    }
    
    /* 注册事件处理器 - 只注册需要处理的事件，避免 ESP_EVENT_ANY_ID */
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED, net_event_handler, NULL);
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, net_event_handler, NULL);
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_START, net_event_handler, NULL);
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_STOP, net_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, ip_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL);
    /* 注意：不再注册 WIFI_EVENT，由 ts_wifi.c 和 ts_dhcp_server.c 处理 */
    
    /* 初始化以太网硬件 */
#ifdef CONFIG_TS_NET_ETHERNET_ENABLE
    if (s_state.eth_config.enabled) {
        ts_eth_config_t eth_hw_config;
        esp_netif_ip_info_t host;
        ret = config_error == ESP_OK ? ts_net_validate_eth_config(&s_state.eth_config, &host) : config_error;
        s_state.eth_status.config_error = ret;
        if (ret != ESP_OK) {
            s_state.eth_status.state = TS_NET_STATE_ERROR;
            TS_LOGE(TAG, "ETH configuration invalid; interface not created: %s", esp_err_to_name(ret));
        } else {
            ret = load_eth_config_from_pins(&eth_hw_config);
        }
        if (ret == ESP_OK) {
            eth_hw_config.ip_info = host;
            ret = ts_eth_init(&eth_hw_config);
            if (ret == ESP_OK) {
                s_state.eth_status.state = TS_NET_STATE_INITIALIZED;
                
                /* 获取 MAC 地址 */
                esp_netif_t *netif = ts_eth_get_netif();
                if (netif) {
                    esp_netif_get_mac(netif, s_state.eth_status.mac);
                }
                
                TS_LOGI(TAG, "Ethernet initialized, MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                        s_state.eth_status.mac[0], s_state.eth_status.mac[1],
                        s_state.eth_status.mac[2], s_state.eth_status.mac[3],
                        s_state.eth_status.mac[4], s_state.eth_status.mac[5]);
            } else {
                TS_LOGE(TAG, "Ethernet init failed: %s", esp_err_to_name(ret));
                s_state.eth_status.state = TS_NET_STATE_ERROR;
            }
        } else {
            if (s_state.eth_status.config_error == ESP_OK) {
                TS_LOGW(TAG, "Ethernet pins not configured, skipping");
                s_state.eth_config.enabled = false;
            }
        }
    }
#endif
    
    /* 初始化 WiFi */
#ifdef CONFIG_TS_NET_WIFI_ENABLE
    TS_LOGI(TAG, "Initializing WiFi...");
    ret = ts_wifi_init();
    if (ret == ESP_OK) {
        s_state.wifi_ap_status.state = TS_NET_STATE_INITIALIZED;
        s_state.wifi_sta_status.state = TS_NET_STATE_INITIALIZED;
        
        /* 设置默认 WiFi AP 配置（如果未从 NVS 加载） */
        if (s_state.wifi_ap_config.ssid[0] == '\0') {
            strncpy(s_state.wifi_ap_config.ssid, CONFIG_TS_NET_WIFI_AP_SSID, 
                    sizeof(s_state.wifi_ap_config.ssid) - 1);
            strncpy(s_state.wifi_ap_config.password, CONFIG_TS_NET_WIFI_AP_PASS,
                    sizeof(s_state.wifi_ap_config.password) - 1);
            s_state.wifi_ap_config.enabled = true;
            s_state.wifi_ap_config.auto_start = true;
            /* AP 默认 IP */
            strncpy(s_state.wifi_ap_config.static_ip.ip, "192.168.4.1", TS_NET_IP_STR_MAX_LEN);
            strncpy(s_state.wifi_ap_config.static_ip.netmask, "255.255.255.0", TS_NET_IP_STR_MAX_LEN);
            strncpy(s_state.wifi_ap_config.static_ip.gateway, "192.168.4.1", TS_NET_IP_STR_MAX_LEN);
        }
        
        TS_LOGI(TAG, "WiFi initialized");
    } else {
        TS_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
    }
#endif
    
    s_state.initialized = true;
    TS_LOGI(TAG, "Network manager initialized");
    
    return ESP_OK;
}

esp_err_t ts_net_manager_deinit(void)
{
    if (!s_state.initialized) return ESP_OK;
    
    TS_LOGI(TAG, "Deinitializing network manager...");
    
    /* 停止所有接口 */
    ts_net_manager_stop(TS_NET_IF_ETH);
    ts_net_manager_stop(TS_NET_IF_WIFI_STA);
    
    /* 注销事件处理器 */
    esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_CONNECTED, net_event_handler);
    esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, net_event_handler);
    esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_START, net_event_handler);
    esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_STOP, net_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, ip_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler);
    /* 注意：WIFI_EVENT 不再由我们注册 */
    
    /* 反初始化以太网 */
#ifdef CONFIG_TS_NET_ETHERNET_ENABLE
    ts_eth_deinit();
#endif
    
    /* 释放回调链表 */
    ts_net_cb_node_t *node = s_state.callbacks;
    while (node) {
        ts_net_cb_node_t *next = node->next;
        free(node);
        node = next;
    }
    
    /* 释放互斥锁 */
    if (s_state.mutex) {
        vSemaphoreDelete(s_state.mutex);
    }
    
    memset(&s_state, 0, sizeof(s_state));
    TS_LOGI(TAG, "Network manager deinitialized");
    
    return ESP_OK;
}

bool ts_net_manager_is_initialized(void)
{
    return s_state.initialized;
}

/* Snapshot pending host configuration without holding the manager lock across
 * DHCP notifications or the driver/event lifecycle. The DHCP transaction validates
 * and applies the complete immutable candidate before an ETH restart can stop it. */
static esp_err_t start_eth(bool restart)
{
#ifdef CONFIG_TS_NET_ETHERNET_ENABLE
    ts_net_if_config_t config;
    char hostname[TS_NET_HOSTNAME_MAX_LEN];
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    config = s_state.eth_config;
    memcpy(hostname, s_state.hostname, sizeof(hostname));
    esp_err_t ret = s_state.eth_status.config_error;
    xSemaphoreGive(s_state.mutex);
    if (ret != ESP_OK) return ret;
    if (!config.enabled) return ESP_ERR_INVALID_STATE;
    esp_netif_ip_info_t host;
    ret = ts_net_validate_eth_config(&config, &host);
    if (ret != ESP_OK) return ret;
    esp_netif_t *netif = ts_eth_get_netif();
    if (!netif) return ESP_ERR_NOT_FOUND; /* Corrected boot error requires reboot. */
    uint32_t dns[] = {config.static_ip.dns1[0] ? ipaddr_addr(config.static_ip.dns1) : 0,
                      config.static_ip.dns2[0] ? ipaddr_addr(config.static_ip.dns2) : 0};
    esp_netif_ip_info_t current;
    ret = esp_netif_get_ip_info(netif, &current);
    if (ret != ESP_OK) return ret;
    if (restart || current.ip.addr != host.ip.addr || current.netmask.addr != host.netmask.addr || current.gw.addr != host.gw.addr) {
        ret = ts_dhcp_server_apply_eth_network(&host, dns);
        if (ret != ESP_OK) return ret;
    }
    /* An already configured host can start even when DHCP recovery is blocked.
     * Link resume owns DHCP startup; no change of address is needed here. */
    ret = esp_netif_set_hostname(netif, hostname);
    if (ret != ESP_OK) return ret;
    if (restart) {
        ret = ts_dhcp_server_eth_suspend();
        if (ret != ESP_OK) return ret;
        ret = ts_eth_stop();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    s_state.eth_status.state = TS_NET_STATE_STARTING;
    ret = ts_eth_start();
    if (ret != ESP_OK) {
        s_state.eth_status.state = TS_NET_STATE_ERROR;
        TS_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(ret));
    }
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t ts_net_manager_start(ts_net_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface == TS_NET_IF_ETH) return start_eth(false);
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    
    switch (iface) {
        case TS_NET_IF_WIFI_STA:
#ifdef CONFIG_TS_NET_WIFI_ENABLE
            if (!s_state.wifi_sta_config.enabled) {
                TS_LOGW(TAG, "WiFi STA is disabled");
                ret = ESP_ERR_INVALID_STATE;
                break;
            }
            
            TS_LOGI(TAG, "Starting WiFi STA...");
            s_state.wifi_sta_status.state = TS_NET_STATE_STARTING;
            
            /* 设置 STA 模式或 APSTA 模式 */
            ts_wifi_mode_t current_mode = ts_wifi_get_mode();
            if (current_mode == TS_WIFI_MODE_AP) {
                ret = ts_wifi_set_mode(TS_WIFI_MODE_APSTA);
            } else if (current_mode == TS_WIFI_MODE_OFF) {
                ret = ts_wifi_set_mode(TS_WIFI_MODE_STA);
            }
            
            if (ret == ESP_OK && s_state.wifi_sta_config.ssid[0] != '\0') {
                /* 配置连接参数 */
                ts_wifi_sta_config_t sta_cfg = {0};
                strncpy(sta_cfg.ssid, s_state.wifi_sta_config.ssid, sizeof(sta_cfg.ssid) - 1);
                strncpy(sta_cfg.password, s_state.wifi_sta_config.password, sizeof(sta_cfg.password) - 1);
                
                ret = ts_wifi_sta_config(&sta_cfg);
                if (ret == ESP_OK) {
                    ret = ts_wifi_sta_connect();
                }
            }
            
            if (ret == ESP_OK) {
                s_state.wifi_sta_status.state = TS_NET_STATE_CONNECTING;
                TS_LOGI(TAG, "WiFi STA connecting to %s...", s_state.wifi_sta_config.ssid);
            } else {
                s_state.wifi_sta_status.state = TS_NET_STATE_ERROR;
                TS_LOGE(TAG, "Failed to start WiFi STA: %s", esp_err_to_name(ret));
            }
#else
            ret = ESP_ERR_NOT_SUPPORTED;
#endif
            break;
            
        case TS_NET_IF_WIFI_AP:
#ifdef CONFIG_TS_NET_WIFI_ENABLE
            if (!s_state.wifi_ap_config.enabled) {
                TS_LOGW(TAG, "WiFi AP is disabled");
                ret = ESP_ERR_INVALID_STATE;
                break;
            }
            
            TS_LOGI(TAG, "Starting WiFi AP...");
            s_state.wifi_ap_status.state = TS_NET_STATE_STARTING;
            
            /* 设置 AP 模式或 APSTA 模式 */
            ts_wifi_mode_t cur_mode = ts_wifi_get_mode();
            if (cur_mode == TS_WIFI_MODE_STA) {
                ret = ts_wifi_set_mode(TS_WIFI_MODE_APSTA);
            } else if (cur_mode == TS_WIFI_MODE_OFF) {
                ret = ts_wifi_set_mode(TS_WIFI_MODE_AP);
            }
            
            if (ret == ESP_OK) {
                /* 配置 AP 参数 */
                ts_wifi_ap_config_t ap_cfg = {
                    .channel = CONFIG_TS_NET_WIFI_AP_CHANNEL,
                    .max_connections = CONFIG_TS_NET_WIFI_AP_MAX_CONN,
                    .auth_mode = WIFI_AUTH_WPA2_PSK,
                    .hidden = false
                };
                strncpy(ap_cfg.ssid, s_state.wifi_ap_config.ssid, sizeof(ap_cfg.ssid) - 1);
                strncpy(ap_cfg.password, s_state.wifi_ap_config.password, sizeof(ap_cfg.password) - 1);
                
                /* 如果没有密码，使用开放认证 */
                if (strlen(ap_cfg.password) == 0) {
                    ap_cfg.auth_mode = WIFI_AUTH_OPEN;
                }
                
                ret = ts_wifi_ap_config(&ap_cfg);
                if (ret == ESP_OK) {
                    ret = ts_wifi_ap_start();
                }
            }
            
            if (ret == ESP_OK) {
                s_state.wifi_ap_status.state = TS_NET_STATE_CONNECTED;
                s_state.wifi_ap_status.has_ip = true;
                
                /* 设置 AP 的 IP 地址 */
                strncpy(s_state.wifi_ap_status.ip_info.ip, 
                        s_state.wifi_ap_config.static_ip.ip, TS_NET_IP_STR_MAX_LEN);
                strncpy(s_state.wifi_ap_status.ip_info.netmask, 
                        s_state.wifi_ap_config.static_ip.netmask, TS_NET_IP_STR_MAX_LEN);
                strncpy(s_state.wifi_ap_status.ip_info.gateway, 
                        s_state.wifi_ap_config.static_ip.gateway, TS_NET_IP_STR_MAX_LEN);
                
                TS_LOGI(TAG, "WiFi AP started: SSID=%s, IP=%s", 
                        s_state.wifi_ap_config.ssid, s_state.wifi_ap_status.ip_info.ip);
            } else {
                s_state.wifi_ap_status.state = TS_NET_STATE_ERROR;
                TS_LOGE(TAG, "Failed to start WiFi AP: %s", esp_err_to_name(ret));
            }
#else
            ret = ESP_ERR_NOT_SUPPORTED;
#endif
            break;
            
        default:
            ret = ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreGive(s_state.mutex);
    return ret;
}

esp_err_t ts_net_manager_stop(ts_net_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    
    switch (iface) {
        case TS_NET_IF_ETH:
#ifdef CONFIG_TS_NET_ETHERNET_ENABLE
            TS_LOGI(TAG, "Stopping Ethernet...");
            ret = ts_eth_stop();
            s_state.eth_status.state = TS_NET_STATE_INITIALIZED;
            s_state.eth_status.link_up = false;
            s_state.eth_status.has_ip = false;
#endif
            break;
            
        case TS_NET_IF_WIFI_STA:
#ifdef CONFIG_TS_NET_WIFI_ENABLE
            TS_LOGI(TAG, "Stopping WiFi STA...");
            ret = ts_wifi_sta_disconnect();
            
            /* 如果 AP 也没启动，关闭 WiFi */
            if (s_state.wifi_ap_status.state == TS_NET_STATE_INITIALIZED ||
                s_state.wifi_ap_status.state == TS_NET_STATE_DISCONNECTED) {
                ts_wifi_set_mode(TS_WIFI_MODE_OFF);
            } else {
                /* 只有 AP 在运行，切换到纯 AP 模式 */
                ts_wifi_set_mode(TS_WIFI_MODE_AP);
            }
            
            s_state.wifi_sta_status.state = TS_NET_STATE_INITIALIZED;
            s_state.wifi_sta_status.has_ip = false;
            memset(&s_state.wifi_sta_status.ip_info, 0, sizeof(s_state.wifi_sta_status.ip_info));
#endif
            break;
            
        case TS_NET_IF_WIFI_AP:
#ifdef CONFIG_TS_NET_WIFI_ENABLE
            TS_LOGI(TAG, "Stopping WiFi AP...");
            ret = ts_wifi_ap_stop();
            
            /* 如果 STA 也没启动，关闭 WiFi */
            if (s_state.wifi_sta_status.state == TS_NET_STATE_INITIALIZED ||
                s_state.wifi_sta_status.state == TS_NET_STATE_DISCONNECTED) {
                ts_wifi_set_mode(TS_WIFI_MODE_OFF);
            } else {
                /* 只有 STA 在运行，切换到纯 STA 模式 */
                ts_wifi_set_mode(TS_WIFI_MODE_STA);
            }
            
            s_state.wifi_ap_status.state = TS_NET_STATE_INITIALIZED;
            s_state.wifi_ap_status.has_ip = false;
#endif
            break;
            
        default:
            ret = ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreGive(s_state.mutex);
    return ret;
}

esp_err_t ts_net_manager_restart(ts_net_if_t iface)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (iface == TS_NET_IF_ETH) return start_eth(true);
    esp_err_t ret = ts_net_manager_stop(iface);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(500));  /* 等待接口完全停止 */
    
    return ts_net_manager_start(iface);
}

/* ============================================================================
 * 状态查询
 * ========================================================================== */

esp_err_t ts_net_manager_get_status(ts_net_manager_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    if (!s_state.initialized) {
        memset(status, 0, sizeof(*status));
        return ESP_OK;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    status->initialized = s_state.initialized;
    memcpy(&status->eth, &s_state.eth_status, sizeof(ts_net_if_status_t));
    memcpy(&status->wifi_sta, &s_state.wifi_sta_status, sizeof(ts_net_if_status_t));
    memcpy(&status->wifi_ap, &s_state.wifi_ap_status, sizeof(ts_net_if_status_t));
    strncpy(status->hostname, s_state.hostname, TS_NET_HOSTNAME_MAX_LEN);
    
    /* Pending host edits never represent the address currently on the wire. */
    esp_netif_t *netif = ts_eth_get_netif();
    if (status->eth.link_up && netif) {
        esp_netif_ip_info_t actual;
        if (esp_netif_get_ip_info(netif, &actual) == ESP_OK) {
            ts_net_ip_u32_to_str(actual.ip.addr, status->eth.ip_info.ip, TS_NET_IP_STR_MAX_LEN);
            ts_net_ip_u32_to_str(actual.netmask.addr, status->eth.ip_info.netmask, TS_NET_IP_STR_MAX_LEN);
            ts_net_ip_u32_to_str(actual.gw.addr, status->eth.ip_info.gateway, TS_NET_IP_STR_MAX_LEN);
        } else {
            status->eth.has_ip = false;
            memset(&status->eth.ip_info, 0, sizeof(status->eth.ip_info));
        }
    }

    /* 计算连接时长 */
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_state.eth_status.state == TS_NET_STATE_GOT_IP) {
        status->eth.uptime_sec = (now - s_state.eth_connect_time) / 1000;
    }
    
    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

esp_err_t ts_net_manager_get_if_status(ts_net_if_t iface, ts_net_if_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    
    ts_net_manager_status_t full_status;
    esp_err_t ret = ts_net_manager_get_status(&full_status);
    if (ret != ESP_OK) return ret;
    
    switch (iface) {
        case TS_NET_IF_ETH:
            memcpy(status, &full_status.eth, sizeof(*status));
            break;
        case TS_NET_IF_WIFI_STA:
            memcpy(status, &full_status.wifi_sta, sizeof(*status));
            break;
        case TS_NET_IF_WIFI_AP:
            memcpy(status, &full_status.wifi_ap, sizeof(*status));
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

ts_net_state_t ts_net_manager_get_state(ts_net_if_t iface)
{
    if (!s_state.initialized) return TS_NET_STATE_UNINITIALIZED;
    
    switch (iface) {
        case TS_NET_IF_ETH:      return s_state.eth_status.state;
        case TS_NET_IF_WIFI_STA: return s_state.wifi_sta_status.state;
        case TS_NET_IF_WIFI_AP:  return s_state.wifi_ap_status.state;
        default:                 return TS_NET_STATE_UNINITIALIZED;
    }
}

bool ts_net_manager_is_ready(ts_net_if_t iface)
{
    return ts_net_manager_get_state(iface) == TS_NET_STATE_GOT_IP;
}

esp_netif_t *ts_net_manager_get_netif(ts_net_if_t iface)
{
    switch (iface) {
        case TS_NET_IF_ETH:
#ifdef CONFIG_TS_NET_ETHERNET_ENABLE
            return ts_eth_get_netif();
#else
            return NULL;
#endif
        case TS_NET_IF_WIFI_STA:
            return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        case TS_NET_IF_WIFI_AP:
            return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        default:
            return NULL;
    }
}

/* ============================================================================
 * 配置管理
 * ========================================================================== */

esp_err_t ts_net_manager_get_config(ts_net_if_t iface, ts_net_if_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    switch (iface) {
        case TS_NET_IF_ETH:
            memcpy(config, &s_state.eth_config, sizeof(*config));
            break;
        case TS_NET_IF_WIFI_STA:
            memcpy(config, &s_state.wifi_sta_config, sizeof(*config));
            break;
        case TS_NET_IF_WIFI_AP:
            memcpy(config, &s_state.wifi_ap_config, sizeof(*config));
            break;
        default:
            xSemaphoreGive(s_state.mutex);
            return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

esp_err_t ts_net_manager_set_config(ts_net_if_t iface, const ts_net_if_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (iface == TS_NET_IF_ETH) {
        esp_netif_ip_info_t host;
        esp_err_t ret = ts_net_validate_eth_config(config, &host);
        if (ret != ESP_OK) return ret;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    switch (iface) {
        case TS_NET_IF_ETH:
            memcpy(&s_state.eth_config, config, sizeof(s_state.eth_config));
            s_state.eth_status.config_error = ESP_OK;
            break;
        case TS_NET_IF_WIFI_STA:
            memcpy(&s_state.wifi_sta_config, config, sizeof(s_state.wifi_sta_config));
            break;
        case TS_NET_IF_WIFI_AP:
            memcpy(&s_state.wifi_ap_config, config, sizeof(s_state.wifi_ap_config));
            break;
        default:
            xSemaphoreGive(s_state.mutex);
            return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "Config updated for %s", ts_net_if_to_str(iface));
    return ESP_OK;
}

esp_err_t ts_net_manager_set_ip_mode(ts_net_if_t iface, ts_net_ip_mode_t mode)
{
    if (mode != TS_NET_IP_MODE_STATIC && mode != TS_NET_IP_MODE_DHCP) return ESP_ERR_INVALID_ARG;
    if (iface == TS_NET_IF_ETH && mode != TS_NET_IP_MODE_STATIC) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    switch (iface) {
        case TS_NET_IF_ETH:
            s_state.eth_config.ip_mode = mode;
            esp_netif_ip_info_t host;
            s_state.eth_status.config_error = ts_net_validate_eth_config(&s_state.eth_config, &host);
            break;
        case TS_NET_IF_WIFI_STA:
            s_state.wifi_sta_config.ip_mode = mode;
            break;
        default:
            xSemaphoreGive(s_state.mutex);
            return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "%s IP mode set to %s", ts_net_if_to_str(iface),
            mode == TS_NET_IP_MODE_DHCP ? "DHCP" : "static");
    return ESP_OK;
}

esp_err_t ts_net_manager_set_static_ip(ts_net_if_t iface, const ts_net_ip_info_str_t *ip_info)
{
    if (!ip_info) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_net_ip_info_str_t *target = NULL;
    switch (iface) {
        case TS_NET_IF_ETH: {
            ts_net_if_config_t candidate = s_state.eth_config;
            candidate.static_ip = *ip_info;
            esp_netif_ip_info_t host;
            esp_err_t ret = ts_net_validate_eth_config(&candidate, &host);
            if (ret != ESP_OK) {
                xSemaphoreGive(s_state.mutex);
                return ret;
            }
            s_state.eth_status.config_error = ESP_OK;
            target = &s_state.eth_config.static_ip;
        }
            break;
        case TS_NET_IF_WIFI_STA:
            target = &s_state.wifi_sta_config.static_ip;
            break;
        default:
            xSemaphoreGive(s_state.mutex);
            return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(target, ip_info, sizeof(*target));
    
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "%s static IP set to %s", ts_net_if_to_str(iface), ip_info->ip);
    return ESP_OK;
}

esp_err_t ts_net_manager_set_hostname(const char *hostname)
{
    if (!hostname) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    strncpy(s_state.hostname, hostname, TS_NET_HOSTNAME_MAX_LEN - 1);
    s_state.hostname[TS_NET_HOSTNAME_MAX_LEN - 1] = '\0';
    xSemaphoreGive(s_state.mutex);
    
    TS_LOGI(TAG, "Hostname set to %s", hostname);
    return ESP_OK;
}

const char *ts_net_manager_get_hostname(void)
{
    return s_state.hostname;
}

/* ============================================================================
 * NVS 配置持久化
 * ========================================================================== */

esp_err_t ts_net_manager_save_config(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    esp_netif_ip_info_t host;
    ret = s_state.eth_status.config_error;
    if (ret == ESP_OK) ret = ts_net_validate_eth_config(&s_state.eth_config, &host);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_state.mutex);
        nvs_close(handle);
        return ret;
    }
    /* 保存以太网配置 */
    if (ret == ESP_OK) ret = nvs_set_u8(handle, NVS_KEY_ETH_ENABLED, s_state.eth_config.enabled ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_set_u8(handle, NVS_KEY_ETH_IP_MODE, (uint8_t)s_state.eth_config.ip_mode);
    if (ret == ESP_OK) ret = nvs_set_str(handle, NVS_KEY_ETH_IP, s_state.eth_config.static_ip.ip);
    if (ret == ESP_OK) ret = nvs_set_str(handle, NVS_KEY_ETH_NETMASK, s_state.eth_config.static_ip.netmask);
    if (ret == ESP_OK) ret = nvs_set_str(handle, NVS_KEY_ETH_GATEWAY, s_state.eth_config.static_ip.gateway);
    if (ret == ESP_OK) ret = nvs_set_str(handle, NVS_KEY_ETH_DNS1, s_state.eth_config.static_ip.dns1);
    if (ret == ESP_OK) ret = nvs_set_str(handle, NVS_KEY_ETH_DNS2, s_state.eth_config.static_ip.dns2);
    if (ret == ESP_OK) ret = nvs_set_str(handle, NVS_KEY_HOSTNAME, s_state.hostname);
    
    if (ret != ESP_OK) {
        xSemaphoreGive(s_state.mutex);
        nvs_close(handle);
        return ret;
    }

    /* 保存 WiFi AP 配置 */
#ifdef CONFIG_TS_NET_WIFI_ENABLE
    nvs_set_u8(handle, NVS_KEY_AP_ENABLED, s_state.wifi_ap_config.enabled ? 1 : 0);
    nvs_set_str(handle, NVS_KEY_AP_SSID, s_state.wifi_ap_config.ssid);
    nvs_set_str(handle, NVS_KEY_AP_PASS, s_state.wifi_ap_config.password);
    nvs_set_u8(handle, NVS_KEY_AP_CHANNEL, s_state.wifi_ap_config.channel);
    nvs_set_str(handle, NVS_KEY_AP_IP, s_state.wifi_ap_config.static_ip.ip);
    
    /* 保存 WiFi STA 配置 */
    nvs_set_u8(handle, NVS_KEY_STA_ENABLED, s_state.wifi_sta_config.enabled ? 1 : 0);
    nvs_set_str(handle, NVS_KEY_STA_SSID, s_state.wifi_sta_config.ssid);
    nvs_set_str(handle, NVS_KEY_STA_PASS, s_state.wifi_sta_config.password);
#endif
    
    xSemaphoreGive(s_state.mutex);
    
    ret = nvs_commit(handle);
    nvs_close(handle);
    
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Configuration saved to NVS");
    } else {
        TS_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

static esp_err_t read_eth_config(nvs_handle_t handle, ts_net_if_config_t *config)
{
    uint8_t value;
    esp_err_t ret = nvs_get_u8(handle, NVS_KEY_ETH_ENABLED, &value);
    if (ret == ESP_OK) {
        if (value > 1) return ESP_ERR_INVALID_ARG;
        config->enabled = value != 0;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) return ret;
    ret = nvs_get_u8(handle, NVS_KEY_ETH_IP_MODE, &value);
    if (ret == ESP_OK) config->ip_mode = (ts_net_ip_mode_t)value;
    else if (ret != ESP_ERR_NVS_NOT_FOUND) return ret;
    const char *keys[] = {NVS_KEY_ETH_IP, NVS_KEY_ETH_NETMASK, NVS_KEY_ETH_GATEWAY, NVS_KEY_ETH_DNS1, NVS_KEY_ETH_DNS2};
    char *fields[] = {config->static_ip.ip, config->static_ip.netmask, config->static_ip.gateway,
                      config->static_ip.dns1, config->static_ip.dns2};
    for (size_t i = 0; i < 5; ++i) {
        size_t length = TS_NET_IP_STR_MAX_LEN;
        ret = nvs_get_str(handle, keys[i], fields[i], &length);
        if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) return ret;
    }
    return ESP_OK;
}

esp_err_t ts_net_manager_load_config(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        TS_LOGI(TAG, "No saved config in NVS, using defaults");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        s_state.eth_status.config_error = ret;
        return ret;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    size_t str_len;
#ifdef CONFIG_TS_NET_WIFI_ENABLE
    uint8_t u8_val;
#endif
    
    ts_net_if_config_t candidate = s_state.eth_config;
    esp_err_t eth_result = read_eth_config(handle, &candidate);
    if (eth_result == ESP_OK) {
        s_state.eth_config = candidate;
        esp_netif_ip_info_t host;
        eth_result = ts_net_validate_eth_config(&candidate, &host);
    }
    s_state.eth_status.config_error = eth_result;

    str_len = TS_NET_HOSTNAME_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_HOSTNAME, s_state.hostname, &str_len);
    
    /* 加载 WiFi AP 配置 */
#ifdef CONFIG_TS_NET_WIFI_ENABLE
    if (nvs_get_u8(handle, NVS_KEY_AP_ENABLED, &u8_val) == ESP_OK) {
        s_state.wifi_ap_config.enabled = (u8_val != 0);
    }
    
    str_len = sizeof(s_state.wifi_ap_config.ssid);
    nvs_get_str(handle, NVS_KEY_AP_SSID, s_state.wifi_ap_config.ssid, &str_len);
    
    str_len = sizeof(s_state.wifi_ap_config.password);
    nvs_get_str(handle, NVS_KEY_AP_PASS, s_state.wifi_ap_config.password, &str_len);
    
    if (nvs_get_u8(handle, NVS_KEY_AP_CHANNEL, &u8_val) == ESP_OK) {
        s_state.wifi_ap_config.channel = u8_val;
    }
    
    str_len = TS_NET_IP_STR_MAX_LEN;
    nvs_get_str(handle, NVS_KEY_AP_IP, s_state.wifi_ap_config.static_ip.ip, &str_len);
    
    /* 加载 WiFi STA 配置 */
    if (nvs_get_u8(handle, NVS_KEY_STA_ENABLED, &u8_val) == ESP_OK) {
        s_state.wifi_sta_config.enabled = (u8_val != 0);
    }
    
    str_len = sizeof(s_state.wifi_sta_config.ssid);
    nvs_get_str(handle, NVS_KEY_STA_SSID, s_state.wifi_sta_config.ssid, &str_len);
    
    str_len = sizeof(s_state.wifi_sta_config.password);
    nvs_get_str(handle, NVS_KEY_STA_PASS, s_state.wifi_sta_config.password, &str_len);
#endif
    
    xSemaphoreGive(s_state.mutex);
    
    nvs_close(handle);
    
    if (eth_result == ESP_OK) TS_LOGI(TAG, "Configuration loaded from NVS");
    else TS_LOGE(TAG, "ETH configuration not usable: %s", esp_err_to_name(eth_result));
    return eth_result;
}

esp_err_t ts_net_manager_reset_config(void)
{
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    /* 重置以太网配置 - 作为网关/DHCP服务器，默认使用静态IP */
    s_state.eth_config.enabled = true;
    s_state.eth_config.ip_mode = TS_NET_IP_MODE_STATIC;
    s_state.eth_config.auto_start = true;
    strncpy(s_state.eth_config.static_ip.ip, TS_NET_DEFAULT_IP, TS_NET_IP_STR_MAX_LEN);
    strncpy(s_state.eth_config.static_ip.netmask, TS_NET_DEFAULT_NETMASK, TS_NET_IP_STR_MAX_LEN);
    strncpy(s_state.eth_config.static_ip.gateway, TS_NET_DEFAULT_GATEWAY, TS_NET_IP_STR_MAX_LEN);
    strncpy(s_state.eth_config.static_ip.dns1, TS_NET_DEFAULT_DNS, TS_NET_IP_STR_MAX_LEN);
    s_state.eth_config.static_ip.dns2[0] = '\0';
    s_state.eth_status.config_error = ESP_OK;
    
    strncpy(s_state.hostname, TS_NET_DEFAULT_HOSTNAME, TS_NET_HOSTNAME_MAX_LEN);
    
    xSemaphoreGive(s_state.mutex);
    
    /* 删除 NVS 数据 */
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    
    TS_LOGI(TAG, "Configuration reset to defaults");
    return ESP_OK;
}

/* ============================================================================
 * 事件回调
 * ========================================================================== */

esp_err_t ts_net_manager_register_event_callback(ts_net_event_cb_t callback, void *user_data)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    
    ts_net_cb_node_t *node = TS_NET_MALLOC(sizeof(ts_net_cb_node_t));
    if (!node) return ESP_ERR_NO_MEM;
    
    node->callback = callback;
    node->user_data = user_data;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    node->next = s_state.callbacks;
    s_state.callbacks = node;
    xSemaphoreGive(s_state.mutex);
    
    return ESP_OK;
}

esp_err_t ts_net_manager_unregister_event_callback(ts_net_event_cb_t callback)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    ts_net_cb_node_t **pp = &s_state.callbacks;
    while (*pp) {
        if ((*pp)->callback == callback) {
            ts_net_cb_node_t *to_free = *pp;
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
