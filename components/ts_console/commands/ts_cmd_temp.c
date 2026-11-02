/**
 * @file ts_cmd_temp.c
 * @brief Temperature Source Console Commands
 * 
 * 实现 temp 命令族：
 * - temp --status           显示当前温度和温度源
 * - temp --set --value V    手动设置温度（调试用）
 * - temp --mode --value M   设置温度源模式（auto/manual）
 * - temp --providers        列出所有温度提供者
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 */

#include "ts_console.h"
#include "ts_temp_source.h"
#include "ts_api.h"
#include "ts_log.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "argtable3/argtable3.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "cmd_temp"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *status;
    struct arg_lit *set;
    struct arg_lit *mode;
    struct arg_lit *providers;
    struct arg_str *value;       // Temperature or auto/manual
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_temp_args;

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static const char *source_to_str(ts_temp_source_type_t source)
{
    switch (source) {
        case TS_TEMP_SOURCE_DEFAULT:     return "default";
        case TS_TEMP_SOURCE_SENSOR_LOCAL: return "sensor_local";
        case TS_TEMP_SOURCE_AGX_AUTO:    return "agx_auto";
        case TS_TEMP_SOURCE_VARIABLE:    return "variable";
        case TS_TEMP_SOURCE_MANUAL:      return "manual";
        default:                         return "unknown";
    }
}

/*===========================================================================*/
/*                          Command: temp --status                            */
/*===========================================================================*/

static int do_temp_status(bool json)
{
    /* JSON 模式通过 API 获取 */
    if (json) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("temp.status", NULL, &result);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_error("Failed to get temperature status\n");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK && result.code == TS_API_OK) ? 0 : 1;
    }
    
    /* 格式化输出：直接调用底层 */
    ts_temp_data_t data;
    int16_t temp = ts_temp_get_effective(&data);
    (void)temp;  /* 避免未使用警告 */
    
    if (!ts_temp_source_is_initialized()) {
        ts_console_error("Temperature source not initialized\n");
        return 1;
    }
    
    bool manual_mode = ts_temp_is_manual_mode();
    
    ts_console_printf("Temperature Status:\n");
    ts_console_printf("  Current:     %.1f°C\n", data.value / 10.0f);
    ts_console_printf("  Source:      %s\n", source_to_str(data.source));
    ts_console_printf("  Mode:        %s\n", manual_mode ? "manual" : "auto");
    ts_console_printf("  Valid:       %s\n", data.valid ? "Yes" : "No");
    
    if (data.valid && data.timestamp_ms > 0) {
        int64_t age_ms = (esp_timer_get_time() / 1000) - data.timestamp_ms;
        if (age_ms < 0) age_ms = 0;
        ts_console_printf("  Updated:     %lld ms ago\n", (long long)age_ms);
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: temp --providers                         */
/*===========================================================================*/

static int do_temp_providers(bool json)
{
    /* JSON 模式通过 API 获取 */
    if (json) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("temp.sources", NULL, &result);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_error("Failed to get temperature providers\n");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK && result.code == TS_API_OK) ? 0 : 1;
    }
    
    /* 格式化输出：直接调用底层 */
    ts_temp_source_type_t sources[] = {
        TS_TEMP_SOURCE_DEFAULT,
        TS_TEMP_SOURCE_SENSOR_LOCAL,
        TS_TEMP_SOURCE_AGX_AUTO,
        TS_TEMP_SOURCE_VARIABLE,
        TS_TEMP_SOURCE_MANUAL
    };
    
    int count = sizeof(sources) / sizeof(sources[0]);
    
    ts_console_printf("Temperature Providers:\n");
    ts_console_printf("  %-15s %-10s %-12s %s\n", "Source", "Priority", "Temperature", "Valid");
    ts_console_printf("  %-15s %-10s %-12s %s\n", "------", "--------", "-----------", "-----");
    
    for (int i = 0; i < count; i++) {
        ts_temp_data_t data;
        if (ts_temp_get_by_source(sources[i], &data) == ESP_OK) {
            ts_console_printf("  %-15s %-10d %-12.1f %s\n",
                source_to_str(sources[i]),
                sources[i],
                data.value / 10.0f,
                data.valid ? "Yes" : "No");
        }
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: temp --set                               */
/*===========================================================================*/

static int do_temp_set(double temp_c)
{
    /* 通过 API 设置温度 */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "temperature_c", temp_c);
    
    ts_api_result_t result;
    esp_err_t ret = ts_api_call("temp.manual", params, &result);
    cJSON_Delete(params);
    
    if (ret != ESP_OK || result.code != TS_API_OK) {
        ts_console_error("Failed to set temperature: %s\n", 
            result.message ? result.message : "API error");
        ts_api_result_free(&result);
        return 1;
    }
    
    ts_api_result_free(&result);
    ts_console_printf("Temperature set to %.1f°C (manual mode enabled)\n", temp_c);
    return 0;
}

/*===========================================================================*/
/*                          Command: temp --mode                              */
/*===========================================================================*/

static int do_temp_mode(const char *mode_str)
{
    if (strcmp(mode_str, "manual") != 0 && strcmp(mode_str, "auto") != 0) {
        ts_console_error("Invalid mode: %s (use 'auto' or 'manual')\n", mode_str);
        return 1;
    }
    
    cJSON *params = cJSON_CreateObject();
    const char *api_name = NULL;
    if (strcmp(mode_str, "manual") == 0) {
        api_name = "temp.manual";
        cJSON_AddBoolToObject(params, "enable", true);
    } else {
        api_name = "temp.select";
        cJSON_AddStringToObject(params, "source", "auto");
    }
    
    ts_api_result_t result;
    esp_err_t ret = ts_api_call(api_name, params, &result);
    cJSON_Delete(params);
    
    if (ret != ESP_OK || result.code != TS_API_OK) {
        ts_console_error("Failed to set mode: %s\n", 
            result.message ? result.message : "API error");
        ts_api_result_free(&result);
        return 1;
    }
    
    ts_api_result_free(&result);
    ts_console_printf("Temperature mode set to %s\n", mode_str);
    return 0;
}

/*===========================================================================*/
/*                          Main Command Handler                              */
/*===========================================================================*/

static int temp_cmd_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_temp_args);
    bool json = s_temp_args.json->count > 0;
    
    if (s_temp_args.help->count > 0) {
        ts_console_printf("Usage: temp [OPTIONS]\n");
        ts_console_printf("\nOptions:\n");
        ts_console_printf("  --status        Show current temperature and source\n");
        ts_console_printf("  --set           Set manual temperature\n");
        ts_console_printf("  --mode          Set temperature mode (auto/manual)\n");
        ts_console_printf("  --providers     List all temperature providers\n");
        ts_console_printf("  --value,-V <n>  Temperature value in °C\n");
        ts_console_printf("  --json,-j       Output in JSON format\n");
        ts_console_printf("  --help,-h       Show this help\n");
        ts_console_printf("\nExamples:\n");
        ts_console_printf("  temp --status             # Show current temperature\n");
        ts_console_printf("  temp --set -V 45.5        # Set to 45.5°C (manual mode)\n");
        ts_console_printf("  temp --mode --value auto  # Switch to auto mode\n");
        ts_console_printf("  temp --providers --json   # List providers in JSON\n");
        return 0;
    }
    
    if (nerrors > 0) {
        arg_print_errors(stderr, s_temp_args.end, "temp");
        return 1;
    }
    
    /* temp --providers */
    if (s_temp_args.providers->count > 0) {
        return do_temp_providers(json);
    }
    
    /* temp --set */
    if (s_temp_args.set->count > 0) {
        if (s_temp_args.value->count == 0) {
            ts_console_error("--set requires --value <temperature>\n");
            return 1;
        }
        char *end;
        double value = strtod(s_temp_args.value->sval[0], &end);
        if (*end != '\0' || end == s_temp_args.value->sval[0]) {
            ts_console_error("Invalid temperature value\n");
            return 1;
        }
        return do_temp_set(value);
    }
    
    /* temp --mode */
    if (s_temp_args.mode->count > 0) {
        if (s_temp_args.value->count == 0) {
            ts_console_error("--mode requires --value <auto|manual>\n");
            return 1;
        }
        return do_temp_mode(s_temp_args.value->sval[0]);
    }
    
    /* Default: temp --status */
    return do_temp_status(json);
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_temp_register(void)
{
    s_temp_args.status    = arg_lit0(NULL, "status", "Show temperature status");
    s_temp_args.set       = arg_lit0(NULL, "set", "Set manual temperature");
    s_temp_args.mode      = arg_lit0(NULL, "mode", "Set temperature mode");
    s_temp_args.providers = arg_lit0(NULL, "providers", "List all providers");
    s_temp_args.value     = arg_str0("V", "value", "<value>", "Temperature or auto/manual");
    s_temp_args.json      = arg_lit0("j", "json", "Output JSON format");
    s_temp_args.help      = arg_lit0("h", "help", "Show help");
    s_temp_args.end       = arg_end(5);
    
    const esp_console_cmd_t cmd = {
        .command = "temp",
        .help = "Temperature source management",
        .hint = NULL,
        .func = temp_cmd_handler,
        .argtable = &s_temp_args,
    };
    
    return esp_console_cmd_register(&cmd);
}
