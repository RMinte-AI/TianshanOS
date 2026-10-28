/**
 * @file ts_cmd_device.c
 * @brief Device Control Console Commands (API Layer)
 * 
 * 实现 device 命令族（通过 ts_api 调用）：
 * - device --agx          AGX 控制
 * - device --lpmu         LPMU 控制
 * - device --usb-mux      USB MUX 控制
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 * @date 2026-01-20
 */

#include "ts_console.h"
#include "ts_api.h"
#include "ts_log.h"
#include "ts_device_ctrl.h"
#include "ts_usb_mux.h"
#include "ts_config_module.h"
#include "argtable3/argtable3.h"
#include <string.h>
#include <stdio.h>

#define TAG "cmd_device"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *agx;
    struct arg_lit *lpmu;
    struct arg_lit *usb_mux;
    struct arg_str *power;
    struct arg_str *target;
    struct arg_lit *status;
    struct arg_lit *reset;
    struct arg_lit *save;
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_device_args;

/*===========================================================================*/
/*                          Command: device --agx                             */
/*===========================================================================*/

static int do_agx_control(const char *power, bool status_only, bool reset, bool json)
{
    // 检查 AGX 是否配置
    if (!ts_device_is_configured(TS_DEVICE_AGX)) {
        ts_console_error("AGX not configured\n");
        return 1;
    }
    
    if (status_only || (!power && !reset)) {
        /* 查询状态 - JSON 模式使用 API */
        if (json) {
            cJSON *params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "device", "agx");
            
            ts_api_result_t result;
            esp_err_t ret = ts_api_call("device.status", params, &result);
            cJSON_Delete(params);
            
            if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
                char *json_str = cJSON_PrintUnformatted(result.data);
                if (json_str) {
                    ts_console_printf("%s\n", json_str);
                    free(json_str);
                }
            } else {
                ts_console_error("API call failed: %s\n", 
                    result.message ? result.message : esp_err_to_name(ret));
            }
            ts_api_result_free(&result);
            return (ret == ESP_OK) ? 0 : 1;
        }
        
        /* 格式化输出 */
        ts_device_status_t status;
        esp_err_t ret = ts_device_get_status(TS_DEVICE_AGX, &status);
        if (ret != ESP_OK) {
            ts_console_error("Failed to get AGX status: %s\n", esp_err_to_name(ret));
            return 1;
        }
        
        const char *state_str = ts_device_state_to_str(status.state);
        bool powered = ts_device_is_powered(TS_DEVICE_AGX);
        
        ts_console_printf("AGX Status:\n");
        ts_console_printf("  Power:      %s%s\033[0m\n",
            powered ? "\033[32m" : "\033[33m",
            powered ? "ON" : "OFF");
        ts_console_printf("  State:      %s\n", state_str);
        ts_console_printf("  Uptime:     %lu ms\n", status.uptime_ms);
        ts_console_printf("  Boot count: %lu\n", status.boot_count);
        return 0;
    }
    
    if (reset) {
        /* 使用 API 重置 */
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "device", "agx");
        
        ts_api_result_t result;
        ts_console_printf("Resetting AGX...\n");
        esp_err_t ret = ts_api_call("device.reset", params, &result);
        cJSON_Delete(params);
        
        if (ret != ESP_OK || result.code != TS_API_OK) {
            ts_console_error("Failed to reset AGX: %s\n", 
                result.message ? result.message : esp_err_to_name(ret));
            ts_api_result_free(&result);
            return 1;
        }
        
        ts_api_result_free(&result);
        ts_console_success("AGX reset complete\n");
        return 0;
    }
    
    if (power) {
        /* 使用 API 电源控制 */
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "device", "agx");
        cJSON_AddStringToObject(params, "action", power);
        
        ts_api_result_t result;
        ts_console_printf("AGX power %s...\n", power);
        esp_err_t ret = ts_api_call("device.power", params, &result);
        cJSON_Delete(params);
        
        if (ret != ESP_OK || result.code != TS_API_OK) {
            ts_console_error("Failed: %s\n", 
                result.message ? result.message : esp_err_to_name(ret));
            ts_api_result_free(&result);
            return 1;
        }
        
        ts_api_result_free(&result);
        ts_console_success("AGX power %s complete\n", power);
        return 0;
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: device --lpmu                            */
/*===========================================================================*/

static int do_lpmu_control(const char *power, bool status_only, bool reset, bool json)
{
    // 检查 LPMU 是否配置
    if (!ts_device_is_configured(TS_DEVICE_LPMU)) {
        ts_console_error("LPMU not configured\n");
        return 1;
    }
    
    if (status_only || (!power && !reset)) {
        /* 查询状态 - JSON 模式使用 API */
        if (json) {
            cJSON *params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "device", "lpmu");
            
            ts_api_result_t result;
            esp_err_t ret = ts_api_call("device.status", params, &result);
            cJSON_Delete(params);
            
            if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
                char *json_str = cJSON_PrintUnformatted(result.data);
                if (json_str) {
                    ts_console_printf("%s\n", json_str);
                    free(json_str);
                }
            } else {
                ts_console_error("API call failed: %s\n", 
                    result.message ? result.message : esp_err_to_name(ret));
            }
            ts_api_result_free(&result);
            return (ret == ESP_OK) ? 0 : 1;
        }
        
        /* 格式化输出 */
        ts_device_status_t status;
        esp_err_t ret = ts_device_get_status(TS_DEVICE_LPMU, &status);
        if (ret != ESP_OK) {
            ts_console_error("Failed to get LPMU status: %s\n", esp_err_to_name(ret));
            return 1;
        }
        
        const char *state_str = ts_device_state_to_str(status.state);
        bool powered = ts_device_is_powered(TS_DEVICE_LPMU);
        
        ts_console_printf("LPMU Status:\n");
        ts_console_printf("  Power:      %s%s\033[0m\n",
            powered ? "\033[32m" : "\033[33m",
            powered ? "ON" : "OFF");
        ts_console_printf("  State:      %s\n", state_str);
        ts_console_printf("  Uptime:     %lu ms\n", status.uptime_ms);
        ts_console_printf("  Boot count: %lu\n", status.boot_count);
        return 0;
    }
    
    if (reset) {
        /* 使用 API 重置 */
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "device", "lpmu");
        
        ts_api_result_t result;
        ts_console_printf("Resetting LPMU...\n");
        esp_err_t ret = ts_api_call("device.reset", params, &result);
        cJSON_Delete(params);
        
        if (ret != ESP_OK || result.code != TS_API_OK) {
            ts_console_error("Failed to reset LPMU: %s\n", 
                result.message ? result.message : esp_err_to_name(ret));
            ts_api_result_free(&result);
            return 1;
        }
        
        ts_api_result_free(&result);
        ts_console_success("LPMU reset complete\n");
        return 0;
    }
    
    if (power) {
        /* 使用 API 电源控制 */
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "device", "lpmu");
        cJSON_AddStringToObject(params, "action", power);
        
        ts_api_result_t result;
        ts_console_printf("LPMU power %s...\n", power);
        esp_err_t ret = ts_api_call("device.power", params, &result);
        cJSON_Delete(params);
        
        if (ret != ESP_OK || result.code != TS_API_OK) {
            ts_console_error("Failed: %s\n", 
                result.message ? result.message : esp_err_to_name(ret));
            ts_api_result_free(&result);
            return 1;
        }
        
        ts_api_result_free(&result);
        ts_console_success("LPMU power %s complete\n", power);
        return 0;
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: device --usb-mux                         */
/*===========================================================================*/

static int do_usb_mux_control(const char *target, bool status_only, bool json)
{
    /* 使用 API 获取 USB MUX 状态 */
    if (json && (status_only || !target)) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("device.usb_mux", NULL, &result);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_error("API call failed: %s\n", 
                result.message ? result.message : esp_err_to_name(ret));
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    ts_usb_mux_target_t current = ts_usb_mux_get_target();
    const char *current_str;
    
    switch (current) {
        case TS_USB_MUX_ESP32: current_str = "esp32"; break;
        case TS_USB_MUX_AGX:   current_str = "agx";   break;
        case TS_USB_MUX_LPMU:  current_str = "lpmu";  break;
        default:              current_str = "unknown"; break;
    }
    
    if (status_only || !target) {
        // 显示状态
        if (json) {
            ts_console_printf("{\"device\":\"usb-mux\",\"target\":\"%s\"}\n", current_str);
        } else {
            ts_console_printf("USB MUX Status:\n");
            ts_console_printf("  Target: %s\n", current_str);
            ts_console_printf("\n");
            ts_console_printf("Available targets: esp32, agx, lpmu\n");
        }
        return 0;
    }
    
    // 验证并设置目标
    ts_usb_mux_target_t new_target;
    if (strcmp(target, "esp32") == 0) {
        new_target = TS_USB_MUX_ESP32;
    } else if (strcmp(target, "agx") == 0) {
        new_target = TS_USB_MUX_AGX;
    } else if (strcmp(target, "lpmu") == 0) {
        new_target = TS_USB_MUX_LPMU;
    } else {
        ts_console_error("Invalid target: %s (use: esp32, agx, lpmu)\n", target);
        return 1;
    }
    
    ts_console_printf("Switching USB MUX to %s...\n", target);
    esp_err_t ret = ts_usb_mux_set_target(new_target);
    if (ret != ESP_OK) {
        ts_console_error("Failed to switch USB MUX: %s\n", esp_err_to_name(ret));
        return 1;
    }
    ts_console_success("USB MUX switched to %s\n", target);
    
    return 0;
}

/*===========================================================================*/
/*                          Command: device --save                            */
/*===========================================================================*/

static int do_device_save(void)
{
    ts_console_printf("Saving device configuration...\n");
    
    esp_err_t ret = ts_config_module_persist(TS_CONFIG_MODULE_DEVICE);
    if (ret == ESP_OK) {
        ts_console_success("Device configuration saved to NVS");
        ts_console_printf("\n");
        return 0;
    } else {
        ts_console_error("Failed to save configuration: %s\n", esp_err_to_name(ret));
        return 1;
    }
}

/*===========================================================================*/
/*                          Main Command Handler                              */
/*===========================================================================*/

static int cmd_device(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_device_args);
    
    if (s_device_args.help->count > 0) {
        ts_console_printf("Usage: device [options]\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("      --agx           AGX control\n");
        ts_console_printf("      --lpmu          LPMU control\n");
        ts_console_printf("      --usb-mux       USB MUX control\n");
        ts_console_printf("      --power <op>    Power: on, off, restart\n");
        ts_console_printf("      --target <dev>  Target: esp32, agx, lpmu\n");
        ts_console_printf("  -s, --status        Show status\n");
        ts_console_printf("      --reset         Reset device\n");
        ts_console_printf("      --save          Save configuration to NVS and SD\n");
        ts_console_printf("  -j, --json          JSON output\n");
        ts_console_printf("  -h, --help          Show this help\n\n");
        ts_console_printf("Examples:\n");
        ts_console_printf("  device --agx --status\n");
        ts_console_printf("  device --agx --power on\n");
        ts_console_printf("  device --lpmu --reset\n");
        ts_console_printf("  device --usb-mux --target agx\n");
        return 0;
    }
    
    if (nerrors != 0) {
        arg_print_errors(stderr, s_device_args.end, argv[0]);
        return 1;
    }
    
    bool json = s_device_args.json->count > 0;
    bool status_only = s_device_args.status->count > 0;
    bool reset = s_device_args.reset->count > 0;
    const char *power = s_device_args.power->count > 0 ? 
                        s_device_args.power->sval[0] : NULL;
    const char *target = s_device_args.target->count > 0 ? 
                         s_device_args.target->sval[0] : NULL;
    
    if (s_device_args.agx->count > 0) {
        return do_agx_control(power, status_only, reset, json);
    }
    
    if (s_device_args.lpmu->count > 0) {
        return do_lpmu_control(power, status_only, reset, json);
    }
    
    if (s_device_args.usb_mux->count > 0) {
        return do_usb_mux_control(target, status_only, json);
    }
    
    if (s_device_args.save->count > 0) {
        return do_device_save();
    }
    
    // 无设备选择时显示所有设备状态
    ts_device_status_t agx_status, lpmu_status;
    bool agx_configured = ts_device_is_configured(TS_DEVICE_AGX);
    bool lpmu_configured = ts_device_is_configured(TS_DEVICE_LPMU);
    
    if (agx_configured) ts_device_get_status(TS_DEVICE_AGX, &agx_status);
    if (lpmu_configured) ts_device_get_status(TS_DEVICE_LPMU, &lpmu_status);
    
    ts_usb_mux_target_t usb_target = ts_usb_mux_get_target();
    const char *usb_str;
    switch (usb_target) {
        case TS_USB_MUX_ESP32: usb_str = "esp32"; break;
        case TS_USB_MUX_AGX:   usb_str = "agx";   break;
        case TS_USB_MUX_LPMU:  usb_str = "lpmu";  break;
        default:              usb_str = "unknown"; break;
    }
    
    if (json) {
        ts_console_printf("{\"devices\":[");
        if (agx_configured) {
            ts_console_printf("{\"name\":\"agx\",\"power\":%s,\"state\":\"%s\"},",
                ts_device_is_powered(TS_DEVICE_AGX) ? "true" : "false",
                ts_device_state_to_str(agx_status.state));
        } else {
            ts_console_printf("{\"name\":\"agx\",\"configured\":false},");
        }
        if (lpmu_configured) {
            ts_console_printf("{\"name\":\"lpmu\",\"power\":%s,\"state\":\"%s\"},",
                ts_device_is_powered(TS_DEVICE_LPMU) ? "true" : "false",
                ts_device_state_to_str(lpmu_status.state));
        } else {
            ts_console_printf("{\"name\":\"lpmu\",\"configured\":false},");
        }
        ts_console_printf("{\"name\":\"usb-mux\",\"target\":\"%s\"}", usb_str);
        ts_console_printf("]}\n");
    } else {
        ts_console_printf("Device Status:\n\n");
        ts_console_printf("%-12s  %-8s  %s\n", "DEVICE", "POWER", "STATE");
        ts_console_printf("────────────────────────────────────────\n");
        if (agx_configured) {
            ts_console_printf("%-12s  %-8s  %s\n", "AGX", 
                ts_device_is_powered(TS_DEVICE_AGX) ? "ON" : "OFF",
                ts_device_state_to_str(agx_status.state));
        } else {
            ts_console_printf("%-12s  %-8s  %s\n", "AGX", "-", "not configured");
        }
        if (lpmu_configured) {
            ts_console_printf("%-12s  %-8s  %s\n", "LPMU", 
                ts_device_is_powered(TS_DEVICE_LPMU) ? "ON" : "OFF",
                ts_device_state_to_str(lpmu_status.state));
        } else {
            ts_console_printf("%-12s  %-8s  %s\n", "LPMU", "-", "not configured");
        }
        ts_console_printf("%-12s  %-8s  %s\n", "USB-MUX", "-", usb_str);
        ts_console_printf("\n");
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_device_register(void)
{
    s_device_args.agx     = arg_lit0(NULL, "agx", "AGX control");
    s_device_args.lpmu    = arg_lit0(NULL, "lpmu", "LPMU control");
    s_device_args.usb_mux = arg_lit0(NULL, "usb-mux", "USB MUX control");
    s_device_args.power   = arg_str0(NULL, "power", "<op>", "on/off/restart");
    s_device_args.target  = arg_str0(NULL, "target", "<dev>", "esp32/agx/lpmu");
    s_device_args.status  = arg_lit0("s", "status", "Show status");
    s_device_args.reset   = arg_lit0(NULL, "reset", "Reset device");
    s_device_args.save    = arg_lit0(NULL, "save", "Save config to NVS/SD");
    s_device_args.json    = arg_lit0("j", "json", "JSON output");
    s_device_args.help    = arg_lit0("h", "help", "Show help");
    s_device_args.end     = arg_end(10);
    
    const ts_console_cmd_t cmd = {
        .command = "device",
        .help = "Device control (AGX, LPMU, USB-MUX)",
        .hint = NULL,
        .category = TS_CMD_CAT_DEVICE,
        .func = cmd_device,
        .argtable = &s_device_args
    };
    
    esp_err_t ret = ts_console_register_cmd(&cmd);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Device commands registered");
    }
    
    return ret;
}
