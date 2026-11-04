/**
 * @file ts_net.c
 * @brief Network Subsystem Main
 */

#include "ts_net.h"
#include "ts_eth.h"
#include "ts_dhcp_server.h"
#include "ts_wifi.h"
#include "ts_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#ifdef CONFIG_TS_NET_MDNS_ENABLE
#include "mdns.h"
#endif
#include <string.h>
#include <lwip/ip4_addr.h>

#define TAG "ts_net"

static bool s_initialized = false;
static char s_hostname[32] = "TianshanOS";
static char s_ip_str[16];

#ifdef CONFIG_TS_NET_MDNS_ENABLE
static bool s_mdns_initialized = false;

/**
 * @brief 初始化 mDNS 服务（获取 IP 后调用）
 */
esp_err_t ts_net_mdns_start(void)
{
    if (s_mdns_initialized) {
        return ESP_OK;
    }
    
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 设置 mDNS 主机名（从 Kconfig 获取）
    ret = mdns_hostname_set(CONFIG_LWIP_LOCAL_HOSTNAME);
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "mDNS hostname set failed: %s", esp_err_to_name(ret));
    }
    
    ret = mdns_instance_name_set("TianShanOS Rack Manager");
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "mDNS instance name set failed: %s", esp_err_to_name(ret));
    }
    
    // 注册 HTTP 服务 (WebUI)
    ret = mdns_service_add("TianShanOS WebUI", "_http", "_tcp", 80, NULL, 0);
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "mDNS HTTP service add failed: %s", esp_err_to_name(ret));
    }
    
    // 注册 HTTPS 服务 (API)
    ret = mdns_service_add("TianShanOS API", "_https", "_tcp", 443, NULL, 0);
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "mDNS HTTPS service add failed: %s", esp_err_to_name(ret));
    }
    
    s_mdns_initialized = true;
    TS_LOGI(TAG, "mDNS initialized: %s.local", CONFIG_LWIP_LOCAL_HOSTNAME);
    
    return ESP_OK;
}
#else
esp_err_t ts_net_mdns_start(void)
{
    TS_LOGI(TAG, "mDNS disabled by config");
    return ESP_OK;
}
#endif

esp_err_t ts_net_init(void)
{
    if (s_initialized) return ESP_OK;
    
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "esp_netif_init failed");
        return ret;
    }
    
    s_initialized = true;
    TS_LOGI(TAG, "Network subsystem initialized");
    return ESP_OK;
}

esp_err_t ts_net_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    
#ifdef CONFIG_TS_NET_WIFI_ENABLE
    ts_wifi_deinit();
#endif
#ifdef CONFIG_TS_NET_ETHERNET_ENABLE
    ts_eth_deinit();
#endif
    
    s_initialized = false;
    return ESP_OK;
}

ts_net_status_t ts_net_get_status(ts_net_if_t iface)
{
    switch (iface) {
        case TS_NET_IF_ETH: {
#ifdef CONFIG_TS_NET_ETHERNET_ENABLE
            ts_net_if_status_t status;
            if (ts_net_manager_get_if_status(iface, &status) == ESP_OK && status.config_error != ESP_OK) {
                return TS_NET_STATUS_ERROR;
            }
            return ts_eth_is_link_up() ? TS_NET_STATUS_CONNECTED : TS_NET_STATUS_DOWN;
#else
            return TS_NET_STATUS_DOWN;
#endif
        
        }
        case TS_NET_IF_WIFI_STA:
#ifdef CONFIG_TS_NET_WIFI_ENABLE
            return ts_wifi_sta_is_connected() ? TS_NET_STATUS_CONNECTED : TS_NET_STATUS_DOWN;
#else
            return TS_NET_STATUS_DOWN;
#endif
        
        case TS_NET_IF_WIFI_AP:
#ifdef CONFIG_TS_NET_WIFI_ENABLE
            return (ts_wifi_get_mode() == TS_WIFI_MODE_AP || 
                    ts_wifi_get_mode() == TS_WIFI_MODE_APSTA) ? 
                    TS_NET_STATUS_CONNECTED : TS_NET_STATUS_DOWN;
#else
            return TS_NET_STATUS_DOWN;
#endif
        
        default:
            return TS_NET_STATUS_DOWN;
    }
}

static esp_netif_t *get_netif(ts_net_if_t iface)
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

esp_err_t ts_net_get_ip_info(ts_net_if_t iface, ts_net_ip_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    
    esp_netif_t *netif = get_netif(iface);
    if (!netif) return ESP_ERR_NOT_FOUND;
    
    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret != ESP_OK) return ret;
    
    info->ip = ip_info.ip.addr;
    info->netmask = ip_info.netmask.addr;
    info->gateway = ip_info.gw.addr;
    
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        info->dns1 = dns.ip.u_addr.ip4.addr;
    }
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
        info->dns2 = dns.ip.u_addr.ip4.addr;
    }
    
    return ESP_OK;
}

/* ETH host addresses are owned by ts_net, never derived from a DHCP pool. */
esp_err_t ts_net_validate_eth_ip(const esp_netif_ip_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    uint32_t host = ntohl(info->ip.addr), mask = ntohl(info->netmask.addr);
    uint32_t inverse = ~mask, network = host & mask, broadcast = network | inverse;
    if (!mask || inverse < 3 || (inverse & (inverse + 1)) || host <= network ||
        host >= broadcast || host >= 0xe0000000U || (host >> 24) == 127 || (host >> 24) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t gateway = ntohl(info->gw.addr);
    if (gateway && ((gateway & mask) != network || gateway <= network || gateway >= broadcast)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t ts_net_validate_eth_config(const ts_net_if_config_t *config, esp_netif_ip_info_t *info)
{
    if (!config || !info) return ESP_ERR_INVALID_ARG;
    if (config->ip_mode != TS_NET_IP_MODE_STATIC) return ESP_ERR_NOT_SUPPORTED;
    const char *strings[] = {config->static_ip.ip, config->static_ip.netmask,
        config->static_ip.gateway, config->static_ip.dns1, config->static_ip.dns2};
    ip4_addr_t addresses[5] = {0};
    for (size_t i = 0; i < 5; ++i) {
        if (!memchr(strings[i], '\0', TS_NET_IP_STR_MAX_LEN)) return ESP_ERR_INVALID_ARG;
        if (i >= 3 && !strings[i][0]) continue;
        if (!ip4addr_aton(strings[i], &addresses[i])) return ESP_ERR_INVALID_ARG;
    }
    *info = (esp_netif_ip_info_t){.ip.addr = addresses[0].addr,
        .netmask.addr = addresses[1].addr, .gw.addr = addresses[2].addr};
    return ts_net_validate_eth_ip(info);
}

esp_err_t ts_net_set_ip_info(ts_net_if_t iface, const ts_net_ip_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    
    esp_netif_t *netif = get_netif(iface);
    if (!netif) return ESP_ERR_NOT_FOUND;
    
    esp_netif_ip_info_t ip_info = {
        .ip.addr = info->ip,
        .netmask.addr = info->netmask,
        .gw.addr = info->gateway
    };
    
    if (iface == TS_NET_IF_ETH) {
        uint32_t dns[] = {info->dns1, info->dns2};
        return ts_dhcp_server_apply_eth_network(&ip_info, dns);
    }
    esp_err_t ret;
    if (iface == TS_NET_IF_WIFI_STA) {
        ret = esp_netif_dhcpc_stop(netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return ret;
    }
    ret = esp_netif_set_ip_info(netif, &ip_info);
    if (ret != ESP_OK) return ret;
    
    if (info->dns1) {
        esp_netif_dns_info_t dns = {.ip.u_addr.ip4.addr = info->dns1};
        ret = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
        if (ret != ESP_OK) return ret;
    }
    if (info->dns2) {
        esp_netif_dns_info_t dns = {.ip.u_addr.ip4.addr = info->dns2};
        ret = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns);
        if (ret != ESP_OK) return ret;
    }
    
    return ESP_OK;
}

esp_err_t ts_net_enable_dhcp(ts_net_if_t iface)
{
    if (iface == TS_NET_IF_ETH) return ESP_ERR_NOT_SUPPORTED;
    esp_netif_t *netif = get_netif(iface);
    if (!netif) return ESP_ERR_NOT_FOUND;
    
    return esp_netif_dhcpc_start(netif);
}

esp_err_t ts_net_get_stats(ts_net_if_t iface, ts_net_stats_t *stats)
{
    if (!stats) return ESP_ERR_INVALID_ARG;
    memset(stats, 0, sizeof(*stats));
    return ESP_OK;
}

esp_err_t ts_net_get_mac(ts_net_if_t iface, uint8_t mac[6])
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    
    esp_netif_t *netif = get_netif(iface);
    if (!netif) return ESP_ERR_NOT_FOUND;
    
    return esp_netif_get_mac(netif, mac);
}

esp_err_t ts_net_set_hostname(const char *hostname)
{
    if (!hostname) return ESP_ERR_INVALID_ARG;
    strncpy(s_hostname, hostname, sizeof(s_hostname) - 1);
    return ESP_OK;
}

const char *ts_net_get_hostname(void)
{
    return s_hostname;
}

const char *ts_net_ip_to_str(uint32_t ip)
{
    ip4_addr_t addr = {.addr = ip};
    strncpy(s_ip_str, ip4addr_ntoa(&addr), sizeof(s_ip_str) - 1);
    return s_ip_str;
}

esp_err_t ts_net_str_to_ip(const char *str, uint32_t *ip)
{
    if (!str || !ip) return ESP_ERR_INVALID_ARG;
    
    ip4_addr_t addr;
    if (ip4addr_aton(str, &addr)) {
        *ip = addr.addr;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}
