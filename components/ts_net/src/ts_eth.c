/**
 * @file ts_eth.c
 * @brief Ethernet Driver (W5500) Implementation
 * 
 * TianShanOS 以太网配置为 DHCP 服务器模式（静态 IP）
 * 用于给连接的设备（如 Jetson AGX）分配 IP 地址
 * 
 * 参考 ESP-IDF 官方示例: examples/network/sta2eth/main/ethernet_iface.c
 * 
 * 关键实现要点：
 * 1. DHCP 服务器在 ETHERNET_EVENT_CONNECTED 事件中启动（不是 init 阶段）
 * 2. DHCP 服务器在 ETHERNET_EVENT_DISCONNECTED 事件中停止
 * 3. 事件处理器需要传入 netif 作为参数
 */

#include "ts_eth.h"
#include "ts_net.h"
#include "ts_dhcp_server.h"
#include "ts_pin_manager.h"
#include "ts_log.h"
#include "ts_event.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_log.h"  /* for esp_log_level_set */
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lwip/ip4_addr.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include <string.h>
#include <stdatomic.h>

#define TAG "ts_eth"

/* Host configuration is supplied by ts_net; DHCP options belong to ts_dhcp_server. */
static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static atomic_bool s_link_up = false;
static bool s_initialized = false;
/* Synchronous completion evidence, not link state. Events never write it. */
static bool s_driver_stop_complete = false;
static bool s_isr_service_installed = false;

/**
 * @brief DHCP 服务器启动任务
 * 
 * 在独立任务中启动 DHCP 服务器，避免在事件处理器上下文中操作
 */
static void dhcp_start_task(void *arg)
{
    esp_netif_t *netif = (esp_netif_t *)arg;
    
    /* 等待一小段时间让网络层稳定 */
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (netif && s_link_up) {
        /* 通过 ts_dhcp_server 启动 DHCP 服务器
         * 这样可以正确追踪状态和统计信息 */
        esp_err_t ret = ts_dhcp_server_eth_resume();
        if (ret == ESP_OK) {
            TS_LOGI(TAG, "DHCP server started via ts_dhcp_server");
        } else if (ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            TS_LOGW(TAG, "DHCP start failed: %s", esp_err_to_name(ret));
        }
    }
    
    vTaskDeleteWithCaps(NULL);
}

/**
 * @brief 事件处理器 - 处理以太网事件
 * 
 * 参考 ESP-IDF sta2eth 示例:
 * - ETHERNET_EVENT_CONNECTED: 启动 DHCP 服务器
 * - ETHERNET_EVENT_DISCONNECTED: 停止 DHCP 服务器
 * 
 * @param arg 用户参数，这里传入 esp_netif_t* 
 */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    esp_netif_t *netif = (esp_netif_t *)arg;
    
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            TS_LOGI(TAG, "Ethernet link up");
            s_link_up = true;
            
            /* 
             * 在独立任务中启动 DHCP 服务器
             * 避免在事件处理器上下文中操作导致崩溃
             * 使用 PSRAM 栈以减少 DRAM 压力（纯网络操作）
             */
            if (netif && xTaskCreateWithCaps(dhcp_start_task, "dhcp_start", 4096, netif, 5, NULL,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
                TS_LOGE(TAG, "Unable to schedule ETH DHCP resume");
            }
            
            esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
            TS_LOGI(TAG, "Ethernet MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                    mac_addr[0], mac_addr[1], mac_addr[2], 
                    mac_addr[3], mac_addr[4], mac_addr[5]);
            break;
            
        case ETHERNET_EVENT_DISCONNECTED:
            TS_LOGI(TAG, "Ethernet link down");
            s_link_up = false;
            ts_dhcp_server_eth_suspend();
            break;
            
        case ETHERNET_EVENT_START:
            TS_LOGI(TAG, "Ethernet started");
            break;
            
        case ETHERNET_EVENT_STOP:
            s_link_up = false;
            ts_dhcp_server_eth_suspend();
            TS_LOGI(TAG, "Ethernet stopped");
            break;
    }
}

/**
 * @brief IP 事件处理器 - 处理 DHCP 服务器分配 IP 事件
 * 
 * 注意：只做简单日志，客户端追踪由 ts_dhcp_server 模块处理
 * @note Reserved for DHCP event handling feature
 */
__attribute__((unused))
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_AP_STAIPASSIGNED) {
        /* DHCP 服务器给客户端分配了 IP - 由 ts_dhcp_server 模块追踪 */
        ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
        TS_LOGI(TAG, "DHCP assigned " IPSTR " to %02x:%02x:%02x:%02x:%02x:%02x",
                IP2STR(&event->ip),
                event->mac[0], event->mac[1], event->mac[2],
                event->mac[3], event->mac[4], event->mac[5]);
    }
}

/**
 * @brief 创建支持 DHCP 服务器的以太网 netif
 * 
 * 参考 ESP-IDF sta2eth 示例 ethernet_iface.c:329-372
 * 
 * 配置说明：
 * - 使用 ESP_NETIF_DHCP_SERVER 标志（非 DHCP_CLIENT）
 * - 设置静态 IP 配置
 * - 使用标准以太网网络栈
 */
static esp_err_t create_eth_netif_with_dhcps(const ts_eth_config_t *config, esp_netif_t **out)
{
    *out = NULL;
    /* 
     * 基础配置 - 参考 sta2eth/ethernet_iface.c:347-353
     * 关键：使用 ESP_NETIF_DHCP_SERVER 标志而不是 ESP_NETIF_DHCP_CLIENT
     * 注意：if_key 必须与 ts_dhcp_server.c 中的查找键一致
     */
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER,      /* DHCP 服务器模式 */
        .ip_info = &config->ip_info,         /* 静态 IP */
        .get_ip_event = 0,                   /* 不需要 get IP 事件 */
        .lost_ip_event = 0,                  /* 不需要 lost IP 事件 */
        .if_key = "ETH_DHCPS",               /* 接口标识 - ts_dhcp_server.c 通过此 key 查找 */
        .if_desc = "ethernet dhcp server",   /* 描述 */
        .route_prio = 10                     /* 路由优先级 */
    };
    
    /* netif 配置 */
    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = NULL,  /* 稍后通过 esp_eth_new_netif_glue 附加 */
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    
    esp_netif_t *netif = esp_netif_new(&cfg);
    if (!netif) {
        TS_LOGE(TAG, "Failed to create Ethernet netif");
        return ESP_ERR_NO_MEM;
    }

    /* Complete host initialization before DHCP restores its pool DNS override. */
    for (unsigned i = 0; i < 2; ++i) {
        if (!config->dns[i]) continue;
        esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4};
        dns.ip.u_addr.ip4.addr = config->dns[i];
        esp_err_t ret = esp_netif_set_dns_info(netif, i ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &dns);
        if (ret != ESP_OK) {
            esp_netif_destroy(netif);
            return ret;
        }
    }
    
    TS_LOGI(TAG, "Created Ethernet netif with DHCP server mode");
    
    *out = netif;
    return ESP_OK;
}

esp_err_t ts_eth_init(const ts_eth_config_t *config)
{
    if (s_initialized) return ESP_OK;
    if (!config) return ESP_ERR_INVALID_ARG;
    
    TS_LOGI(TAG, "Initializing Ethernet (W5500)");
    
    // Install GPIO ISR service (required for W5500 interrupt)
    if (!s_isr_service_installed) {
        esp_err_t isr_ret = gpio_install_isr_service(0);
        if (isr_ret == ESP_OK || isr_ret == ESP_ERR_INVALID_STATE) {
            // ESP_ERR_INVALID_STATE means it's already installed
            s_isr_service_installed = true;
            TS_LOGD(TAG, "GPIO ISR service ready");
        } else {
            TS_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(isr_ret));
            return isr_ret;
        }
    }
    
    // Create netif with DHCP server support - 必须在注册事件处理器前创建
    esp_err_t ret = create_eth_netif_with_dhcps(config, &s_eth_netif);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to initialize eth netif: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 
     * 注册事件处理器
     * eth_event_handler: 处理以太网状态（link up/down）
     * 
     * 只注册特定事件，避免 ESP_EVENT_ANY_ID 导致与 ts_net_manager 重复处理
     */
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED, eth_event_handler, s_eth_netif);
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, eth_event_handler, s_eth_netif);
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_START, eth_event_handler, s_eth_netif);
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_STOP, eth_event_handler, s_eth_netif);
    
    // SPI bus configuration
    spi_bus_config_t buscfg = {
        .mosi_io_num = config->gpio_mosi,
        .miso_io_num = config->gpio_miso,
        .sclk_io_num = config->gpio_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ret = spi_bus_initialize(config->spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        TS_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // W5500 SPI device configuration (ESP-IDF 5.5 API)
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = config->spi_clock_mhz * 1000 * 1000,
        .spics_io_num = config->gpio_cs,
        .queue_size = 20,
    };
    
    // W5500 MAC driver (ESP-IDF 5.5 API)
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(config->spi_host, &spi_devcfg);
    w5500_config.int_gpio_num = config->gpio_int;
    
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        TS_LOGE(TAG, "Failed to create W5500 MAC");
        return ESP_FAIL;
    }
    
    // W5500 PHY driver
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = config->gpio_rst;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        TS_LOGE(TAG, "Failed to create W5500 PHY");
        return ESP_FAIL;
    }
    
    // Create Ethernet driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Ethernet driver install failed");
        return ret;
    }
    s_driver_stop_complete = true; /* A newly installed driver has not started. */
    
    // Generate and set MAC address from chip efuse
    // W5500 doesn't have built-in MAC, we derive from ESP32's base MAC
    uint8_t eth_mac[6];
    esp_read_mac(eth_mac, ESP_MAC_ETH);
    esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
    TS_LOGI(TAG, "Ethernet MAC: %02x:%02x:%02x:%02x:%02x:%02x",
            eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
    
    // Attach to netif
    esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle));
    
    /* Pool, leases, bindings and DNS offers belong to ts_dhcp_server. */
    ret = esp_netif_set_mac(s_eth_netif, eth_mac);
    if (ret != ESP_OK) return ret;

    s_initialized = true;
    TS_LOGI(TAG, "Ethernet initialized");
    return ESP_OK;
}

esp_err_t ts_eth_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    
    ts_eth_stop();
    
    // Unregister event handlers
    esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_CONNECTED, eth_event_handler);
    esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, eth_event_handler);
    esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_START, eth_event_handler);
    esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_STOP, eth_event_handler);
    
    if (s_eth_handle) {
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    }
    
    if (s_eth_netif) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }
    
    s_initialized = false;
    return ESP_OK;
}

esp_err_t ts_eth_start(void)
{
    if (!s_eth_handle) return ESP_ERR_INVALID_STATE;
    s_driver_stop_complete = false; /* A failed start may already have changed the SDK. */
    return esp_eth_start(s_eth_handle);
}

esp_err_t ts_eth_stop(void)
{
    if (!s_eth_handle) return ESP_ERR_INVALID_STATE;
    if (!s_driver_stop_complete) {
        esp_err_t ret = esp_eth_stop(s_eth_handle);
        if (ret != ESP_OK) return ret;
        s_driver_stop_complete = true;
    }
    s_link_up = false;
    return ts_dhcp_server_is_initialized() ? ts_dhcp_server_eth_suspend() : ESP_OK;
}

bool ts_eth_is_link_up(void)
{
    return s_link_up;
}

esp_netif_t *ts_eth_get_netif(void)
{
    return s_eth_netif;
}
