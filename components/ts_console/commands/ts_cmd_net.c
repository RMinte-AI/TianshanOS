/**
 * @file ts_cmd_net.c
 * @brief Network Console Commands
 * 
 * 实现 net 命令族：
 * - net --status           显示网络状态
 * - net --config           显示当前配置
 * - net --set              设置网络参数
 * - net --start/--stop     启动/停止接口
 * - net --restart          重启接口
 * - net --save             保存配置
 * - net --reset            重置为默认配置
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 * @date 2026-01-16
 */

#include "ts_console.h"
#include "ts_net_manager.h"
#include "ts_config_module.h"
#include "ts_api.h"
#include "ts_log.h"
#include "argtable3/argtable3.h"
#include <string.h>
#include <stdio.h>

#define TAG "cmd_net"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *status;
    struct arg_lit *config;
    struct arg_lit *set;
    struct arg_lit *start;
    struct arg_lit *stop;
    struct arg_lit *restart;
    struct arg_lit *save;
    struct arg_lit *load;
    struct arg_lit *reset;
    struct arg_lit *show_ip;     /* --ip: 快速显示IP配置 */
    struct arg_str *iface;
    struct arg_str *ip;
    struct arg_str *netmask;
    struct arg_str *gateway;
    struct arg_str *dns;
    struct arg_str *mode;
    struct arg_str *hostname;
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_net_args;

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static ts_net_if_t parse_iface(const char *str)
{
    if (!str || str[0] == '\0') return TS_NET_IF_ETH;  /* 默认以太网 */
    if (strcmp(str, "eth") == 0 || strcmp(str, "ethernet") == 0) return TS_NET_IF_ETH;
    if (strcmp(str, "wifi") == 0 || strcmp(str, "wlan") == 0) return TS_NET_IF_WIFI_STA;
    if (strcmp(str, "ap") == 0) return TS_NET_IF_WIFI_AP;
    return TS_NET_IF_ETH;
}

static const char *state_color(ts_net_state_t state)
{
    switch (state) {
        case TS_NET_STATE_GOT_IP:      return "\033[32m";  /* 绿色 */
        case TS_NET_STATE_CONNECTED:   return "\033[33m";  /* 黄色 */
        case TS_NET_STATE_CONNECTING:
        case TS_NET_STATE_STARTING:    return "\033[33m";  /* 黄色 */
        case TS_NET_STATE_ERROR:       return "\033[31m";  /* 红色 */
        default:                       return "\033[90m";  /* 灰色 */
    }
}

/*===========================================================================*/
/*                          Command: net --ip                                 */
/*===========================================================================*/

static int do_net_ip(const char *iface_str, bool json_out)
{
    ts_net_manager_status_t status;
    esp_err_t ret = ts_net_manager_get_status(&status);
    
    if (ret != ESP_OK) {
        ts_console_error("Failed to get network status\n");
        return 1;
    }
    
    ts_net_if_t iface = parse_iface(iface_str);
    
    /* 获取要显示的接口状态 */
    ts_net_if_status_t *if_status = NULL;
    const char *if_name = NULL;
    
    switch (iface) {
        case TS_NET_IF_ETH:
            if_status = &status.eth;
            if_name = "eth";
            break;
        case TS_NET_IF_WIFI_STA:
            if_status = &status.wifi_sta;
            if_name = "wifi";
            break;
        case TS_NET_IF_WIFI_AP:
            if_status = &status.wifi_ap;
            if_name = "ap";
            break;
        default:
            if_status = &status.eth;
            if_name = "eth";
            break;
    }
    
    if (json_out) {
        ts_console_printf("{\n");
        ts_console_printf("  \"interface\": \"%s\",\n", if_name);
        ts_console_printf("  \"has_ip\": %s,\n", if_status->has_ip ? "true" : "false");
        if (if_status->has_ip) {
            ts_console_printf("  \"ip\": \"%s\",\n", if_status->ip_info.ip);
            ts_console_printf("  \"netmask\": \"%s\",\n", if_status->ip_info.netmask);
            ts_console_printf("  \"gateway\": \"%s\",\n", if_status->ip_info.gateway);
            ts_console_printf("  \"dns\": \"%s\"\n", if_status->ip_info.dns1);
        }
        ts_console_printf("}\n");
    } else {
        if (if_status->has_ip) {
            ts_console_printf("%s: %s/%s gw %s\n",
                if_name,
                if_status->ip_info.ip,
                if_status->ip_info.netmask,
                if_status->ip_info.gateway);
        } else {
            ts_console_printf("%s: no IP\n", if_name);
        }
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: net --status                             */
/*===========================================================================*/

static int do_net_status(bool json_out)
{
    /* JSON 模式使用 API */
    if (json_out) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("network.status", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"error\":\"%s\"}\n", result.message ? result.message : "Unknown error");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    ts_net_manager_status_t status;
    esp_err_t ret = ts_net_manager_get_status(&status);
    
    if (ret != ESP_OK) {
        ts_console_error("Failed to get network status\n");
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("╔══════════════════════════════════════════════════════════════╗\n");
    ts_console_printf("║                      Network Status                          ║\n");
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    ts_console_printf("║ Hostname: %-50s ║\n", status.hostname);
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    /* 以太网状态 */
    ts_console_printf("║ \033[1mEthernet (W5500)\033[0m                                            ║\n");
    ts_console_printf("║   State:    %s%-12s\033[0m                                   ║\n",
        state_color(status.eth.state), ts_net_state_to_str(status.eth.state));
    ts_console_printf("║   Link:     %-12s                                   ║\n",
        status.eth.link_up ? "Up" : "Down");
    ts_console_printf("║   MAC:      %02x:%02x:%02x:%02x:%02x:%02x                            ║\n",
        status.eth.mac[0], status.eth.mac[1], status.eth.mac[2],
        status.eth.mac[3], status.eth.mac[4], status.eth.mac[5]);
    
    if (status.eth.has_ip) {
        ts_console_printf("║   IP:       %-15s                              ║\n", status.eth.ip_info.ip);
        ts_console_printf("║   Netmask:  %-15s                              ║\n", status.eth.ip_info.netmask);
        ts_console_printf("║   Gateway:  %-15s                              ║\n", status.eth.ip_info.gateway);
        ts_console_printf("║   DNS:      %-15s                              ║\n", status.eth.ip_info.dns1);
        ts_console_printf("║   Uptime:   %lu sec                                         ║\n", 
            (unsigned long)status.eth.uptime_sec);
    }
    
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    /* WiFi 状态 */
    ts_console_printf("║ \033[1mWiFi Station\033[0m                                                ║\n");
    ts_console_printf("║   State:    %s%-12s\033[0m                                   ║\n",
        state_color(status.wifi_sta.state), ts_net_state_to_str(status.wifi_sta.state));
    
    if (status.wifi_sta.has_ip) {
        ts_console_printf("║   IP:       %-15s                              ║\n", status.wifi_sta.ip_info.ip);
    }
    
    ts_console_printf("╚══════════════════════════════════════════════════════════════╝\n");
    ts_console_printf("\n");
    
    return 0;
}

/*===========================================================================*/
/*                          Command: net --config                             */
/*===========================================================================*/

static int do_net_config(const char *iface_str, bool json_out)
{
    ts_net_if_t iface = parse_iface(iface_str);
    ts_net_if_config_t config;
    
    esp_err_t ret = ts_net_manager_get_config(iface, &config);
    if (ret != ESP_OK) {
        ts_console_error("Failed to get config for %s\n", ts_net_if_to_str(iface));
        return 1;
    }
    
    if (json_out) {
        ts_console_printf("{\n");
        ts_console_printf("  \"interface\": \"%s\",\n", ts_net_if_to_str(iface));
        ts_console_printf("  \"enabled\": %s,\n", config.enabled ? "true" : "false");
        ts_console_printf("  \"ip_mode\": \"%s\",\n", 
            config.ip_mode == TS_NET_IP_MODE_DHCP ? "dhcp" : "static");
        ts_console_printf("  \"auto_start\": %s,\n", config.auto_start ? "true" : "false");
        ts_console_printf("  \"static_ip\": {\n");
        ts_console_printf("    \"ip\": \"%s\",\n", config.static_ip.ip);
        ts_console_printf("    \"netmask\": \"%s\",\n", config.static_ip.netmask);
        ts_console_printf("    \"gateway\": \"%s\",\n", config.static_ip.gateway);
        ts_console_printf("    \"dns1\": \"%s\"\n", config.static_ip.dns1);
        ts_console_printf("  }\n");
        ts_console_printf("}\n");
    } else {
        ts_console_printf("\nConfiguration for %s:\n\n", ts_net_if_to_str(iface));
        ts_console_printf("  Enabled:    %s\n", config.enabled ? "Yes" : "No");
        ts_console_printf("  IP Mode:    %s\n", 
            config.ip_mode == TS_NET_IP_MODE_DHCP ? "DHCP" : "Static");
        ts_console_printf("  Auto Start: %s\n", config.auto_start ? "Yes" : "No");
        ts_console_printf("\n  Static IP Configuration:\n");
        ts_console_printf("    IP:       %s\n", config.static_ip.ip);
        ts_console_printf("    Netmask:  %s\n", config.static_ip.netmask);
        ts_console_printf("    Gateway:  %s\n", config.static_ip.gateway);
        ts_console_printf("    DNS:      %s\n", config.static_ip.dns1);
        ts_console_printf("\n");
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: net --set                                */
/*===========================================================================*/

static int do_net_set(const char *iface_str, const char *ip, const char *netmask,
                       const char *gateway, const char *dns, const char *mode,
                       const char *hostname)
{
    ts_net_if_t iface = parse_iface(iface_str);
    bool changed = false;
    
    /* 设置 IP 模式 */
    if (mode && mode[0]) {
        ts_net_ip_mode_t ip_mode;
        if (strcmp(mode, "dhcp") == 0) {
            ip_mode = TS_NET_IP_MODE_DHCP;
        } else if (strcmp(mode, "static") == 0) {
            ip_mode = TS_NET_IP_MODE_STATIC;
        } else {
            ts_console_error("Invalid mode: %s (use 'dhcp' or 'static')\n", mode);
            return 1;
        }
        ts_net_manager_set_ip_mode(iface, ip_mode);
        ts_console_printf("IP mode set to: %s\n", mode);
        changed = true;
    }
    
    /* 设置静态 IP 配置 */
    if (ip || netmask || gateway || dns) {
        ts_net_if_config_t config;
        ts_net_manager_get_config(iface, &config);
        
        if (ip && ip[0]) {
            strncpy(config.static_ip.ip, ip, TS_NET_IP_STR_MAX_LEN - 1);
            ts_console_printf("IP set to: %s\n", ip);
        }
        if (netmask && netmask[0]) {
            strncpy(config.static_ip.netmask, netmask, TS_NET_IP_STR_MAX_LEN - 1);
            ts_console_printf("Netmask set to: %s\n", netmask);
        }
        if (gateway && gateway[0]) {
            strncpy(config.static_ip.gateway, gateway, TS_NET_IP_STR_MAX_LEN - 1);
            ts_console_printf("Gateway set to: %s\n", gateway);
        }
        if (dns && dns[0]) {
            strncpy(config.static_ip.dns1, dns, TS_NET_IP_STR_MAX_LEN - 1);
            ts_console_printf("DNS set to: %s\n", dns);
        }
        
        ts_net_manager_set_static_ip(iface, &config.static_ip);
        changed = true;
    }
    
    /* 设置主机名 */
    if (hostname && hostname[0]) {
        ts_net_manager_set_hostname(hostname);
        ts_console_printf("Hostname set to: %s\n", hostname);
        changed = true;
    }
    
    if (changed) {
        ts_console_printf("\nNote: Use 'net --save' to persist, 'net --restart' to apply\n");
    } else {
        ts_console_printf("No changes made. Use --ip, --netmask, --gateway, --dns, --mode, or --hostname\n");
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: net --start/stop/restart                 */
/*===========================================================================*/

static int do_net_start(const char *iface_str)
{
    ts_net_if_t iface = parse_iface(iface_str);
    
    ts_console_printf("Starting %s...\n", ts_net_if_to_str(iface));
    esp_err_t ret = ts_net_manager_start(iface);
    
    if (ret == ESP_OK) {
        ts_console_printf("%s started successfully\n", ts_net_if_to_str(iface));
        return 0;
    } else {
        ts_console_error("Failed to start %s: %s\n", ts_net_if_to_str(iface), esp_err_to_name(ret));
        return 1;
    }
}

static int do_net_stop(const char *iface_str)
{
    ts_net_if_t iface = parse_iface(iface_str);
    
    ts_console_printf("Stopping %s...\n", ts_net_if_to_str(iface));
    esp_err_t ret = ts_net_manager_stop(iface);
    
    if (ret == ESP_OK) {
        ts_console_printf("%s stopped\n", ts_net_if_to_str(iface));
        return 0;
    } else {
        ts_console_error("Failed to stop %s: %s\n", ts_net_if_to_str(iface), esp_err_to_name(ret));
        return 1;
    }
}

static int do_net_restart(const char *iface_str)
{
    ts_net_if_t iface = parse_iface(iface_str);
    
    ts_console_printf("Restarting %s...\n", ts_net_if_to_str(iface));
    esp_err_t ret = ts_net_manager_restart(iface);
    
    if (ret == ESP_OK) {
        ts_console_printf("%s restarted successfully\n", ts_net_if_to_str(iface));
        return 0;
    } else {
        ts_console_error("Failed to restart %s: %s\n", ts_net_if_to_str(iface), esp_err_to_name(ret));
        return 1;
    }
}

/*===========================================================================*/
/*                          Command: net --save/load/reset                    */
/*===========================================================================*/

static int do_net_save(void)
{
    ts_console_printf("Saving network configuration...\n");
    
    /* 调用原有保存方法 */
    esp_err_t ret = ts_net_manager_save_config();
    if (ret != ESP_OK) {
        ts_console_error("Failed to save to NVS: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    /* 同时保存模块化的系统配置 */
    ret = ts_config_module_persist(TS_CONFIG_MODULE_NET);
    if (ret == ESP_OK) {
        ts_console_success("Configuration saved to NVS");
        ts_console_printf("\n");
    } else {
        ts_console_printf("Configuration saved to NVS\n");
        ts_console_printf("(Module persist skipped: %s)\n", esp_err_to_name(ret));
    }
    
    return 0;
}

static int do_net_load(void)
{
    ts_console_printf("Loading network configuration...\n");
    esp_err_t ret = ts_net_manager_load_config();
    
    if (ret == ESP_OK) {
        ts_console_printf("Configuration loaded from NVS\n");
        ts_console_printf("Use 'net --restart' to apply changes\n");
        return 0;
    } else {
        ts_console_error("Failed to load configuration: %s\n", esp_err_to_name(ret));
        return 1;
    }
}

static int do_net_reset(void)
{
    ts_console_printf("Resetting network configuration to defaults...\n");
    esp_err_t ret = ts_net_manager_reset_config();
    
    if (ret == ESP_OK) {
        ts_console_printf("Configuration reset to defaults\n");
        ts_console_printf("Use 'net --restart' to apply changes\n");
        return 0;
    } else {
        ts_console_error("Failed to reset configuration: %s\n", esp_err_to_name(ret));
        return 1;
    }
}

/*===========================================================================*/
/*                          Main Command Handler                              */
/*===========================================================================*/

static int cmd_net(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_net_args);
    
    /* 显示帮助 */
    if (s_net_args.help->count > 0) {
        ts_console_printf("Usage: net [options]\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  --status            Show network status\n");
        ts_console_printf("  --config            Show interface configuration\n");
        ts_console_printf("  --set               Set network parameters\n");
        ts_console_printf("  --start             Start network interface\n");
        ts_console_printf("  --stop              Stop network interface\n");
        ts_console_printf("  --restart           Restart network interface\n");
        ts_console_printf("  --save              Save configuration to NVS\n");
        ts_console_printf("  --load              Load configuration from NVS\n");
        ts_console_printf("  --reset             Reset to default configuration\n");
        ts_console_printf("  --ip                Show IP address (quick view)\n");
        ts_console_printf("\n");
        ts_console_printf("Parameters:\n");
        ts_console_printf("  --iface <if>        Interface: eth, wifi (default: eth)\n");
        ts_console_printf("  --ip <addr>         IP address (e.g., 192.168.1.100)\n");
        ts_console_printf("  --netmask <mask>    Netmask (e.g., 255.255.255.0)\n");
        ts_console_printf("  --gateway <addr>    Gateway address\n");
        ts_console_printf("  --dns <addr>        DNS server address\n");
        ts_console_printf("  --mode <mode>       IP mode: dhcp, static\n");
        ts_console_printf("  --hostname <name>   Set hostname\n");
        ts_console_printf("  --json              Output in JSON format\n");
        ts_console_printf("\n");
        ts_console_printf("Examples:\n");
        ts_console_printf("  net --status                          Show current status\n");
        ts_console_printf("  net --ip                              Quick IP check\n");
        ts_console_printf("  net --ip --iface wifi                 Quick WiFi IP check\n");
        ts_console_printf("  net --config --iface eth              Show ethernet config\n");
        ts_console_printf("  net --set --mode static --ip 10.0.0.100\n");
        ts_console_printf("  net --set --mode dhcp\n");
        ts_console_printf("  net --save                            Persist configuration\n");
        ts_console_printf("  net --restart                         Apply changes\n");
        return 0;
    }
    
    if (nerrors > 0 && argc > 1) {
        arg_print_errors(stderr, s_net_args.end, "net");
        return 1;
    }
    
    bool json_out = s_net_args.json->count > 0;
    const char *iface_str = s_net_args.iface->count > 0 ? s_net_args.iface->sval[0] : NULL;
    
    /* 处理各个操作 */
    if (s_net_args.status->count > 0 || argc == 1) {
        return do_net_status(json_out);
    }
    
    if (s_net_args.config->count > 0) {
        return do_net_config(iface_str, json_out);
    }
    
    if (s_net_args.show_ip->count > 0) {
        return do_net_ip(iface_str, json_out);
    }
    
    if (s_net_args.set->count > 0) {
        return do_net_set(
            iface_str,
            s_net_args.ip->count > 0 ? s_net_args.ip->sval[0] : NULL,
            s_net_args.netmask->count > 0 ? s_net_args.netmask->sval[0] : NULL,
            s_net_args.gateway->count > 0 ? s_net_args.gateway->sval[0] : NULL,
            s_net_args.dns->count > 0 ? s_net_args.dns->sval[0] : NULL,
            s_net_args.mode->count > 0 ? s_net_args.mode->sval[0] : NULL,
            s_net_args.hostname->count > 0 ? s_net_args.hostname->sval[0] : NULL
        );
    }
    
    if (s_net_args.start->count > 0) {
        return do_net_start(iface_str);
    }
    
    if (s_net_args.stop->count > 0) {
        return do_net_stop(iface_str);
    }
    
    if (s_net_args.restart->count > 0) {
        return do_net_restart(iface_str);
    }
    
    if (s_net_args.save->count > 0) {
        return do_net_save();
    }
    
    if (s_net_args.load->count > 0) {
        return do_net_load();
    }
    
    if (s_net_args.reset->count > 0) {
        return do_net_reset();
    }
    
    /* 默认显示状态 */
    return do_net_status(json_out);
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_net_register(void)
{
    /* 初始化参数表 */
    s_net_args.status   = arg_lit0("s", "status", "Show network status");
    s_net_args.config   = arg_lit0("c", "config", "Show interface configuration");
    s_net_args.set      = arg_lit0(NULL, "set", "Set network parameters");
    s_net_args.start    = arg_lit0(NULL, "start", "Start network interface");
    s_net_args.stop     = arg_lit0(NULL, "stop", "Stop network interface");
    s_net_args.restart  = arg_lit0(NULL, "restart", "Restart network interface");
    s_net_args.save     = arg_lit0(NULL, "save", "Save configuration to NVS");
    s_net_args.load     = arg_lit0(NULL, "load", "Load configuration from NVS");
    s_net_args.reset    = arg_lit0(NULL, "reset", "Reset to default configuration");
    s_net_args.show_ip  = arg_lit0(NULL, "ip", "Show IP address (quick view)");
    s_net_args.iface    = arg_str0(NULL, "iface", "<if>", "Interface: eth, wifi");
    s_net_args.ip       = arg_str0(NULL, "ip", "<addr>", "IP address");
    s_net_args.netmask  = arg_str0(NULL, "netmask", "<mask>", "Netmask");
    s_net_args.gateway  = arg_str0(NULL, "gateway", "<addr>", "Gateway address");
    s_net_args.dns      = arg_str0(NULL, "dns", "<addr>", "DNS server");
    s_net_args.mode     = arg_str0(NULL, "mode", "<mode>", "IP mode: dhcp, static");
    s_net_args.hostname = arg_str0(NULL, "hostname", "<name>", "Hostname");
    s_net_args.json     = arg_lit0("j", "json", "Output in JSON format");
    s_net_args.help     = arg_lit0("h", "help", "Show help");
    s_net_args.end      = arg_end(5);
    
    const ts_console_cmd_t cmd = {
        .command = "net",
        .help = "Network management (status, config, start/stop)",
        .hint = NULL,
        .category = TS_CMD_CAT_NETWORK,
        .func = cmd_net,
        .argtable = &s_net_args
    };
    
    esp_err_t ret = ts_console_register_cmd(&cmd);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Network commands registered");
    }
    
    return ret;
}
