/**
 * @file ts_cmd_monitor.c
 * @brief Compute Monitor Console Commands
 * 
 * 实现 monitor 命令族（算力设备监控）：
 * - monitor --status           显示算力设备监控状态和连接信息
 * - monitor --data             显示最新算力设备数据
 * - monitor --start            启动算力设备监控
 * - monitor --stop             停止算力设备监控
 * - monitor --config           显示/设置配置
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 */

#include "ts_console.h"
#include "ts_compute_monitor.h"
#include "ts_api.h"
#include "ts_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "cmd_monitor"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *status;
    struct arg_lit *data;
    struct arg_lit *start;
    struct arg_lit *stop;
    struct arg_lit *config;
    struct arg_str *server;      // 服务器 IP
    struct arg_int *port;        // 服务器端口
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_monitor_args;

/*===========================================================================*/
/*                          Command: monitor --status                             */
/*===========================================================================*/

static int do_monitor_status(bool json)
{
    if (!ts_compute_monitor_is_initialized()) {
        if (json) {
            ts_console_printf("{\"error\":\"Monitor monitor not initialized\"}\n");
        } else {
            ts_console_error("Monitor monitor not initialized\n");
        }
        return 1;
    }
    
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("monitor.status", NULL, &result);
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
    ts_compute_status_info_t status;
    esp_err_t ret = ts_compute_monitor_get_status(&status);
    if (ret != ESP_OK) {
        ts_console_error("Failed to get status: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("Monitor Monitor Status:\n");
    ts_console_printf("  Initialized:    %s\n", status.initialized ? "Yes" : "No");
    ts_console_printf("  Running:        %s\n", status.running ? "Yes" : "No");
    ts_console_printf("  Connection:     %s\n", ts_compute_status_to_str(status.connection_status));
    ts_console_printf("  Reconnects:     %lu\n", status.total_reconnects);
    ts_console_printf("  Messages:       %lu\n", status.messages_received);
    ts_console_printf("  Parse Errors:   %lu\n", status.parse_errors);
    ts_console_printf("  Reliability:    %.1f%%\n", status.connection_reliability);
    
    if (status.connection_status == TS_COMPUTE_STATUS_CONNECTED) {
        ts_console_printf("  Connected:      %llu ms\n", status.connected_time_ms);
    }
    
    if (status.last_error[0] != '\0') {
        ts_console_printf("  Last Error:     %s\n", status.last_error);
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: monitor --data                               */
/*===========================================================================*/

static int do_monitor_data(bool json)
{
    if (!ts_compute_monitor_is_data_valid()) {
        if (json) {
            ts_console_printf("{\"error\":\"No valid Monitor data\"}\n");
        } else {
            ts_console_error("No valid Monitor data available\n");
        }
        return 1;
    }
    
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("monitor.data", NULL, &result);
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
    ts_compute_data_t data;
    esp_err_t ret = ts_compute_monitor_get_data(&data);
    if (ret != ESP_OK) {
        ts_console_error("Failed to get data: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("Monitor Data:\n");
    ts_console_printf("  Timestamp:     %s\n", data.timestamp);
    ts_console_printf("\n  CPU (%d cores):\n", data.cpu.core_count);
    for (int i = 0; i < data.cpu.core_count; i++) {
        ts_console_printf("    Core %d: %3d%% @ %4d MHz\n",
            data.cpu.cores[i].id,
            data.cpu.cores[i].usage,
            data.cpu.cores[i].freq_mhz);
    }
    
    ts_console_printf("\n  Memory:\n");
    ts_console_printf("    RAM:  %lu / %lu MB\n", 
        data.memory.ram.used_mb, data.memory.ram.total_mb);
    ts_console_printf("    SWAP: %lu / %lu MB\n",
        data.memory.swap.used_mb, data.memory.swap.total_mb);
    
    ts_console_printf("\n  Temperature:\n");
    ts_console_printf("    CPU:   %.1f°C\n", data.temperature.cpu);
    ts_console_printf("    SoC0:  %.1f°C\n", data.temperature.soc0);
    ts_console_printf("    SoC1:  %.1f°C\n", data.temperature.soc1);
    ts_console_printf("    SoC2:  %.1f°C\n", data.temperature.soc2);
    ts_console_printf("    Tj:    %.1f°C\n", data.temperature.tj);
    
    ts_console_printf("\n  Power:\n");
    ts_console_printf("    GPU+SoC: %lu mW (avg: %lu mW)\n",
        data.power.gpu_soc.current_mw, data.power.gpu_soc.average_mw);
    ts_console_printf("    CPU:     %lu mW (avg: %lu mW)\n",
        data.power.cpu_cv.current_mw, data.power.cpu_cv.average_mw);
    ts_console_printf("    SYS 5V:  %lu mW (avg: %lu mW)\n",
        data.power.sys_5v.current_mw, data.power.sys_5v.average_mw);
    
    ts_console_printf("\n  GPU:\n");
    ts_console_printf("    GR3D Freq: %d%%\n", data.gpu.gr3d_freq_pct);
    
    return 0;
}

/*===========================================================================*/
/*                          Command: monitor --start/stop                         */
/*===========================================================================*/

static int do_monitor_start(void)
{
    if (!ts_compute_monitor_is_initialized()) {
        /* 初始化默认配置 */
        esp_err_t ret = ts_compute_monitor_init(NULL);
        if (ret != ESP_OK) {
            ts_console_error("Failed to initialize: %s\n", esp_err_to_name(ret));
            return 1;
        }
    }
    
    if (ts_compute_monitor_is_running()) {
        ts_console_printf("Monitor monitor already running\n");
        return 0;
    }
    
    esp_err_t ret = ts_compute_monitor_start();
    if (ret != ESP_OK) {
        ts_console_error("Failed to start: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("Monitor monitor started\n");
    return 0;
}

static int do_monitor_stop(void)
{
    if (!ts_compute_monitor_is_running()) {
        ts_console_printf("Monitor monitor not running\n");
        return 0;
    }
    
    esp_err_t ret = ts_compute_monitor_stop();
    if (ret != ESP_OK) {
        ts_console_error("Failed to stop: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("Monitor monitor stopped\n");
    return 0;
}

/*===========================================================================*/
/*                          Command: monitor --config                             */
/*===========================================================================*/

static int do_monitor_config(const char *server, int port, bool json)
{
    ts_compute_config_t config;
    ts_compute_monitor_get_default_config(&config);
    
    bool changed = false;
    
    if (server != NULL) {
        strncpy(config.server_ip, server, sizeof(config.server_ip) - 1);
        changed = true;
    }
    
    if (port > 0) {
        config.server_port = (uint16_t)port;
        changed = true;
    }
    
    if (changed) {
        /* 需要重新初始化才能生效 */
        if (ts_compute_monitor_is_running()) {
            esp_err_t ret = ts_compute_monitor_stop();
            if (ret != ESP_OK) {
                ts_console_error("Failed to stop: %s\n", esp_err_to_name(ret));
                return 1;
            }
        }
        if (ts_compute_monitor_is_initialized()) {
            esp_err_t ret = ts_compute_monitor_deinit();
            if (ret != ESP_OK) {
                ts_console_error("Failed to deinit: %s\n", esp_err_to_name(ret));
                return 1;
            }
        }
        
        esp_err_t ret = ts_compute_monitor_init(&config);
        if (ret != ESP_OK) {
            ts_console_error("Failed to reinit with new config: %s\n", esp_err_to_name(ret));
            return 1;
        }
        
        ts_console_printf("Configuration updated: %s:%d\n", config.server_ip, config.server_port);
    } else {
        /* 显示当前配置 */
        if (json) {
            ts_api_result_t result;
            ts_api_result_init(&result);
            
            esp_err_t ret = ts_api_call("monitor.config", NULL, &result);
            if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
                char *json_str = cJSON_PrintUnformatted(result.data);
                if (json_str) {
                    ts_console_printf("%s\n", json_str);
                    free(json_str);
                }
            }
            ts_api_result_free(&result);
            return (ret == ESP_OK) ? 0 : 1;
        } else {
            ts_console_printf("Monitor Monitor Configuration:\n");
            ts_console_printf("  Server:           %s\n", config.server_ip);
            ts_console_printf("  Port:             %d\n", config.server_port);
            ts_console_printf("  Reconnect:        %lu ms\n", config.reconnect_interval_ms);
            ts_console_printf("  Startup Delay:    %lu ms\n", config.startup_delay_ms);
            ts_console_printf("  Heartbeat Timeout: %lu ms\n", config.heartbeat_timeout_ms);
        }
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Main Command Handler                              */
/*===========================================================================*/

static int monitor_cmd_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_monitor_args);
    bool json = s_monitor_args.json->count > 0;
    
    if (s_monitor_args.help->count > 0) {
        ts_console_printf("Usage: monitor [OPTIONS]\n");
        ts_console_printf("\nOptions:\n");
        ts_console_printf("  --status        Show monitor monitor status\n");
        ts_console_printf("  --data          Show latest monitor data\n");
        ts_console_printf("  --start         Start monitor monitoring\n");
        ts_console_printf("  --stop          Stop monitor monitoring\n");
        ts_console_printf("  --config        Show/set configuration\n");
        ts_console_printf("  --server <ip>   Set server IP\n");
        ts_console_printf("  --port <n>      Set server port\n");
        ts_console_printf("  --json,-j       Output in JSON format\n");
        ts_console_printf("  --help,-h       Show this help\n");
        ts_console_printf("\nExamples:\n");
        ts_console_printf("  monitor --status              # Show connection status\n");
        ts_console_printf("  monitor --data --json         # Get Monitor data in JSON\n");
        ts_console_printf("  monitor --start               # Start monitoring\n");
        ts_console_printf("  monitor --config --server 10.10.99.98 --port 58090\n");
        return 0;
    }
    
    if (nerrors > 0) {
        arg_print_errors(stderr, s_monitor_args.end, "monitor");
        return 1;
    }
    
    /* monitor --start */
    if (s_monitor_args.start->count > 0) {
        return do_monitor_start();
    }
    
    /* monitor --stop */
    if (s_monitor_args.stop->count > 0) {
        return do_monitor_stop();
    }
    
    /* monitor --data */
    if (s_monitor_args.data->count > 0) {
        return do_monitor_data(json);
    }
    
    /* monitor --config */
    if (s_monitor_args.config->count > 0 || 
        s_monitor_args.server->count > 0 || 
        s_monitor_args.port->count > 0) {
        const char *server = s_monitor_args.server->count > 0 ? s_monitor_args.server->sval[0] : NULL;
        int port = s_monitor_args.port->count > 0 ? s_monitor_args.port->ival[0] : 0;
        return do_monitor_config(server, port, json);
    }
    
    /* Default: monitor --status */
    return do_monitor_status(json);
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_monitor_register(void)
{
    s_monitor_args.status = arg_lit0(NULL, "status", "Show monitor status");
    s_monitor_args.data   = arg_lit0(NULL, "data", "Show latest monitor data");
    s_monitor_args.start  = arg_lit0(NULL, "start", "Start monitor monitoring");
    s_monitor_args.stop   = arg_lit0(NULL, "stop", "Stop monitor monitoring");
    s_monitor_args.config = arg_lit0(NULL, "config", "Show/set configuration");
    s_monitor_args.server = arg_str0(NULL, "server", "<ip>", "Server IP address");
    s_monitor_args.port   = arg_int0(NULL, "port", "<n>", "Server port");
    s_monitor_args.json   = arg_lit0("j", "json", "Output JSON format");
    s_monitor_args.help   = arg_lit0("h", "help", "Show help");
    s_monitor_args.end    = arg_end(5);
    
    const esp_console_cmd_t cmd = {
        .command = "monitor",
        .help = "Monitor device monitoring (Jetson AGX, servers, etc.)",
        .hint = NULL,
        .func = monitor_cmd_handler,
        .argtable = &s_monitor_args,
    };
    
    return esp_console_cmd_register(&cmd);
}
