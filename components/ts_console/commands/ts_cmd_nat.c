/**
 * @file ts_cmd_nat.c
 * @brief NAT Gateway CLI Command
 * 
 * 命令语法：
 *   nat --status           显示 NAT 状态
 *   nat --enable           启用 NAT 网关
 *   nat --disable          禁用 NAT 网关
 *   nat --save             保存配置到 NVS
 *   nat --json             JSON 格式输出
 */

#include "ts_cmd_all.h"
#include "ts_console.h"
#include "ts_nat.h"
#include "ts_net_manager.h"
#include "ts_api.h"
#include "ts_config_module.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include <stdio.h>
#include <string.h>

/* 命令参数定义 */
static struct {
    struct arg_lit *status;
    struct arg_lit *enable;
    struct arg_lit *disable;
    struct arg_lit *save;
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_args;

/* ============================================================================
 * 命令处理函数
 * ========================================================================== */

static int do_nat_status(bool json_output)
{
    /* JSON 模式使用 API */
    if (json_output) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("nat.status", NULL, &result);
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
    ts_nat_status_t status;
    ts_nat_get_status(&status);
    
    ts_console_printf("\n");
    ts_console_printf("NAT Gateway Status\n");
    ts_console_printf("==================\n");
    ts_console_printf("  State:          %s\n", 
           status.state == TS_NAT_STATE_ENABLED ? "ENABLED" :
           status.state == TS_NAT_STATE_ERROR ? "ERROR" : "DISABLED");
    ts_console_printf("  WiFi STA:       %s\n", 
           status.wifi_connected ? "Connected" : "Disconnected");
    ts_console_printf("  Ethernet:       %s\n", 
           status.eth_up ? "Link Up" : "Link Down");
    ts_console_printf("\n");
    
    if (status.state == TS_NAT_STATE_ENABLED) {
        ts_console_printf("  NAT is active: ETH devices can access internet via WiFi\n");
    } else if (status.wifi_connected) {
        ts_console_printf("  WiFi connected. Run 'nat --enable' to start NAT gateway.\n");
    } else {
        ts_console_printf("  Connect to WiFi first with 'wifi --connect'\n");
    }
    ts_console_printf("\n");
    
    return 0;
}

static int do_nat_enable(bool json_output)
{
    /* JSON 模式使用 API */
    if (json_output) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("nat.enable", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"success\":false,\"error\":\"%s\"}\n", result.message ? result.message : "Failed");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    esp_err_t ret = ts_nat_enable();
    
    if (ret == ESP_OK) {
        ts_console_printf("NAT gateway enabled\n");
        ts_console_printf("ETH devices (e.g. Jetson) can now access internet via WiFi\n");
        ts_console_printf("\nEnsure ETH device gateway is set to ESP32's ETH IP (e.g. 10.10.99.97)\n");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ts_console_printf("Error: WiFi STA not connected\n");
        ts_console_printf("Connect to WiFi first: wifi --connect --ssid <SSID> --pass <password>\n");
    } else {
        ts_console_printf("Error: %s\n", esp_err_to_name(ret));
    }
    
    return ret == ESP_OK ? 0 : 1;
}

static int do_nat_disable(bool json_output)
{
    /* JSON 模式使用 API */
    if (json_output) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("nat.disable", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"success\":false}\n");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    esp_err_t ret = ts_nat_disable();
    
    if (ret == ESP_OK) {
        ts_console_printf("NAT gateway disabled\n");
    } else {
        ts_console_printf("Error: %s\n", esp_err_to_name(ret));
    }
    
    return ret == ESP_OK ? 0 : 1;
}

static int do_nat_save(bool json_output)
{
    ts_console_printf("Saving NAT configuration...\n");
    
    /* 调用原有保存方法 */
    esp_err_t ret = ts_nat_save_config();
    if (ret != ESP_OK) {
        if (json_output) {
            ts_console_printf("{\"success\":false,\"error\":\"%s\"}\n", esp_err_to_name(ret));
        } else {
            ts_console_error("Failed to save to NVS: %s\n", esp_err_to_name(ret));
        }
        return 1;
    }
    
    /* 同时保存模块化的系统配置 */
    ret = ts_config_module_persist(TS_CONFIG_MODULE_NAT);
    
    if (json_output) {
        ts_console_printf("{\"success\":true}\n");
    } else {
        if (ret == ESP_OK) {
            ts_console_success("Configuration saved to NVS");
            ts_console_printf("\n");
        } else {
            ts_console_success("Configuration saved to NVS\n");
        }
    }
    
    return 0;
}

/* ============================================================================
 * 命令入口
 * ========================================================================== */

static int cmd_nat_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_args);
    
    /* 帮助 */
    if (s_args.help->count > 0) {
        ts_console_printf("Usage: nat [options]\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  --status        Show NAT gateway status\n");
        ts_console_printf("  --enable        Enable NAT gateway (WiFi -> ETH)\n");
        ts_console_printf("  --disable       Disable NAT gateway\n");
        ts_console_printf("  --save          Save configuration to NVS\n");
        ts_console_printf("  --json, -j      JSON output format\n");
        ts_console_printf("  --help, -h      Show this help\n");
        ts_console_printf("\n");
        ts_console_printf("NAT gateway allows ETH devices (e.g. Jetson AGX) to access\n");
        ts_console_printf("the internet through ESP32's WiFi connection.\n");
        ts_console_printf("\n");
        ts_console_printf("Example:\n");
        ts_console_printf("  1. Connect WiFi: wifi --connect --ssid MyWiFi --pass secret\n");
        ts_console_printf("  2. Enable NAT:   nat --enable\n");
        ts_console_printf("  3. On Jetson:    Set gateway to ESP32's ETH IP (10.10.99.97)\n");
        return 0;
    }
    
    if (nerrors > 0) {
        arg_print_errors(stderr, s_args.end, "nat");
        return 1;
    }
    
    bool json = s_args.json->count > 0;
    
    /* 处理命令 */
    if (s_args.enable->count > 0) {
        return do_nat_enable(json);
    }
    
    if (s_args.disable->count > 0) {
        return do_nat_disable(json);
    }
    
    if (s_args.save->count > 0) {
        return do_nat_save(json);
    }
    
    /* 默认显示状态 */
    return do_nat_status(json);
}

/* ============================================================================
 * 命令注册
 * ========================================================================== */

void ts_cmd_nat_register(void)
{
    /* 初始化 NAT 模块 */
    ts_nat_init();
    
    /* 定义参数 */
    s_args.status = arg_lit0("s", "status", "Show NAT status");
    s_args.enable = arg_lit0(NULL, "enable", "Enable NAT gateway");
    s_args.disable = arg_lit0(NULL, "disable", "Disable NAT gateway");
    s_args.save = arg_lit0(NULL, "save", "Save config to NVS");
    s_args.json = arg_lit0("j", "json", "JSON output");
    s_args.help = arg_lit0("h", "help", "Show help");
    s_args.end = arg_end(5);
    
    /* 注册命令 */
    const esp_console_cmd_t cmd = {
        .command = "nat",
        .help = "NAT gateway management (ETH <-> WiFi)",
        .hint = NULL,
        .func = &cmd_nat_handler,
        .argtable = &s_args,
    };
    
    esp_console_cmd_register(&cmd);
}
