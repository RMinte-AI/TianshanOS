/**
 * @file ts_cmd_config.c
 * @brief Configuration Console Commands (Unified)
 * 
 * 实现 config 命令族（统一配置管理）：
 * 
 * 基础操作:
 * - config --get -k key           获取配置值
 * - config --set -k key -v val    设置配置值
 * - config --list                 列出所有配置/模块
 * - config --reset                重置配置
 * 
 * 模块操作:
 * - config --show --module net    显示模块配置
 * - config --module net -k eth.ip 获取模块配置
 * - config --allsave              保存所有模块配置
 * - config --export --module net  导出模块到SD卡
 * - config --meta                 显示元配置信息
 * 
 * 配置包操作（加密分发）:
 * - config --pack-export --file <path> --cert-file <cert>  导出加密配置包
 * - config --pack-import --file <path>                     导入加密配置包
 * - config --pack-verify --file <path>                     验证配置包签名
 * - config --pack-info                                     显示配置包系统信息
 * 
 * @author TianShanOS Team
 * @version 2.1.0
 * @date 2026-02-04
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ts_console.h"
#include "ts_config.h"
#include "ts_api.h"
#include "ts_config_module.h"
#include "ts_config_meta.h"
#include "ts_config_pack.h"
#include "ts_cert.h"
#include "ts_log.h"
#include "argtable3/argtable3.h"

static const char *TAG = "cmd_config";

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    /* 基础操作 */
    struct arg_lit *get;
    struct arg_lit *set;
    struct arg_lit *list;
    struct arg_lit *reset;
    /* 模块操作 */
    struct arg_lit *show;
    struct arg_lit *allsave;
    struct arg_lit *export_sd;
    struct arg_lit *meta;
    /* 配置包操作 */
    struct arg_lit *pack_export;
    struct arg_lit *pack_import;
    struct arg_lit *pack_verify;
    struct arg_lit *pack_info;
    struct arg_str *file;
    struct arg_str *cert_file;
    /* 参数 */
    struct arg_str *module;
    struct arg_str *key;
    struct arg_str *value;
    struct arg_str *ns;
    struct arg_lit *persist;
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_config_args;

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief 解析模块名称
 */
static ts_config_module_t parse_module_name(const char *name)
{
    if (!name) return TS_CONFIG_MODULE_MAX + 1;
    if (strcasecmp(name, "net") == 0) return TS_CONFIG_MODULE_NET;
    if (strcasecmp(name, "dhcp") == 0) return TS_CONFIG_MODULE_DHCP;
    if (strcasecmp(name, "wifi") == 0) return TS_CONFIG_MODULE_WIFI;
    if (strcasecmp(name, "led") == 0) return TS_CONFIG_MODULE_LED;
    if (strcasecmp(name, "fan") == 0) return TS_CONFIG_MODULE_FAN;
    if (strcasecmp(name, "device") == 0) return TS_CONFIG_MODULE_DEVICE;
    if (strcasecmp(name, "system") == 0) return TS_CONFIG_MODULE_SYSTEM;
    if (strcasecmp(name, "all") == 0) return TS_CONFIG_MODULE_MAX;
    return TS_CONFIG_MODULE_MAX + 1;
}

static void print_separator(void)
{
    ts_console_printf("─────────────────────────────────────────────────\n");
}

/*===========================================================================*/
/*                          Command: config --get                             */
/*===========================================================================*/

static int do_config_get(const char *key, bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "key", key);
        
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("config.get", params, &result);
        cJSON_Delete(params);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_error("Key '%s' not found\n", key);
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出模式 */
    int32_t i32_val;
    bool bool_val;
    char str_buf[128];
    
    if (ts_config_get_int32(key, &i32_val, 0) == ESP_OK) {
        ts_console_printf("%s = %d\n", key, i32_val);
        return 0;
    }
    
    if (ts_config_get_bool(key, &bool_val, false) == ESP_OK) {
        ts_console_printf("%s = %s\n", key, bool_val ? "true" : "false");
        return 0;
    }
    
    if (ts_config_get_string(key, str_buf, sizeof(str_buf), "") == ESP_OK) {
        ts_console_printf("%s = \"%s\"\n", key, str_buf);
        return 0;
    }
    
    ts_console_error("Key '%s' not found\n", key);
    return 1;
}

/*===========================================================================*/
/*                  Command: config --get --module <mod>                      */
/*===========================================================================*/

static int do_module_get(ts_config_module_t module, const char *key, bool json)
{
    bool bool_val;
    int32_t int_val;
    uint32_t uint_val;
    char str_val[256];
    const char *mod_name = ts_config_module_get_name(module);
    
    if (ts_config_module_get_bool(module, key, &bool_val) == ESP_OK) {
        if (json) {
            ts_console_printf("{\"module\":\"%s\",\"key\":\"%s\",\"type\":\"bool\",\"value\":%s}\n",
                mod_name, key, bool_val ? "true" : "false");
        } else {
            ts_console_printf("%s.%s = %s (bool)\n", mod_name, key, bool_val ? "true" : "false");
        }
        return 0;
    }
    
    if (ts_config_module_get_int(module, key, &int_val) == ESP_OK) {
        if (json) {
            ts_console_printf("{\"module\":\"%s\",\"key\":\"%s\",\"type\":\"int\",\"value\":%ld}\n",
                mod_name, key, (long)int_val);
        } else {
            ts_console_printf("%s.%s = %ld (int)\n", mod_name, key, (long)int_val);
        }
        return 0;
    }
    
    if (ts_config_module_get_uint(module, key, &uint_val) == ESP_OK) {
        if (json) {
            ts_console_printf("{\"module\":\"%s\",\"key\":\"%s\",\"type\":\"uint\",\"value\":%lu}\n",
                mod_name, key, (unsigned long)uint_val);
        } else {
            ts_console_printf("%s.%s = %lu (uint)\n", mod_name, key, (unsigned long)uint_val);
        }
        return 0;
    }
    
    if (ts_config_module_get_string(module, key, str_val, sizeof(str_val)) == ESP_OK) {
        if (json) {
            ts_console_printf("{\"module\":\"%s\",\"key\":\"%s\",\"type\":\"string\",\"value\":\"%s\"}\n",
                mod_name, key, str_val);
        } else {
            ts_console_printf("%s.%s = \"%s\" (string)\n", mod_name, key, str_val);
        }
        return 0;
    }
    
    ts_console_error("Key '%s' not found in module '%s'\n", key, mod_name);
    return 1;
}

/*===========================================================================*/
/*                          Command: config --set                             */
/*===========================================================================*/

static int do_config_set(const char *key, const char *value_str, bool persist)
{
    esp_err_t ret = ESP_FAIL;
    
    if (strcasecmp(value_str, "true") == 0 || strcmp(value_str, "1") == 0) {
        ret = ts_config_set_bool(key, true);
    } else if (strcasecmp(value_str, "false") == 0 || strcmp(value_str, "0") == 0) {
        ret = ts_config_set_bool(key, false);
    } else {
        char *endptr;
        long lval = strtol(value_str, &endptr, 10);
        if (*endptr == '\0') {
            ret = ts_config_set_int32(key, (int32_t)lval);
        } else {
            ret = ts_config_set_string(key, value_str);
        }
    }
    
    if (ret != ESP_OK) {
        ts_console_error("Failed to set '%s'\n", key);
        return 1;
    }
    
    if (persist) {
        ret = ts_config_save();
        if (ret != ESP_OK) {
            ts_console_warn("Value set but failed to persist: %s\n", esp_err_to_name(ret));
        } else {
            ts_console_success("Configuration saved: %s\n", key);
        }
    } else {
        ts_console_success("Configuration set: %s (not persisted)\n", key);
    }
    
    return 0;
}

/*===========================================================================*/
/*                  Command: config --set --module <mod>                      */
/*===========================================================================*/

static int do_module_set(ts_config_module_t module, const char *key, const char *value)
{
    esp_err_t ret = ESP_FAIL;
    const char *mod_name = ts_config_module_get_name(module);
    
    /* 布尔值 */
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "1") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
        ret = ts_config_module_set_bool(module, key, true);
    } else if (strcasecmp(value, "false") == 0 || strcasecmp(value, "0") == 0 ||
               strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
        ret = ts_config_module_set_bool(module, key, false);
    } else {
        char *endptr;
        long num = strtol(value, &endptr, 0);
        if (*endptr == '\0') {
            if (num >= 0) {
                ret = ts_config_module_set_uint(module, key, (uint32_t)num);
            } else {
                ret = ts_config_module_set_int(module, key, (int32_t)num);
            }
        } else {
            ret = ts_config_module_set_string(module, key, value);
        }
    }
    
    if (ret == ESP_OK) {
        ts_console_success("Set %s.%s = %s (temporary)\n", mod_name, key, value);
        ts_console_printf("Hint: use '%s --save' or 'config --allsave' to persist\n", mod_name);
    } else {
        ts_console_error("Failed to set: %s\n", esp_err_to_name(ret));
        return 1;
    }
    return 0;
}

/*===========================================================================*/
/*                          Command: config --list                            */
/*===========================================================================*/

static int do_config_list(bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("config.list", NULL, &result);
        
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
    
    /* 格式化输出模式 */
    ts_console_printf("\nConfiguration Modules:\n");
    print_separator();
    ts_console_printf("%-10s %-12s %-8s %-8s %s\n", "Module", "NVS NS", "Version", "Status", "Dirty");
    print_separator();
    

    for (int i = 0; i < TS_CONFIG_MODULE_MAX; i++) {
        ts_config_module_t mod = (ts_config_module_t)i;
        bool registered = ts_config_module_is_registered(mod);
        
        if (registered) {
            const char *nvs_ns = ts_config_module_get_nvs_namespace(mod);
            uint16_t version = ts_config_module_get_schema_version(mod);
            bool dirty = ts_config_module_is_dirty(mod);
            ts_console_printf("%-10s %-12s v%-7d %-8s %s\n",
                ts_config_module_get_name(mod), nvs_ns ? nvs_ns : "-", version,
                "system", dirty ? "YES" : "-");
        } else {
            ts_console_printf("%-10s %-12s %-8s %-8s %s\n", ts_config_module_get_name(mod), "-", "-", "N/A", "-");
        }
    }
    
    print_separator();
    ts_console_printf("global_seq: %lu\n",
        (unsigned long)ts_config_meta_get_global_seq());
    
    return 0;
}

/*===========================================================================*/
/*                          Command: config --show                            */
/*===========================================================================*/

static void show_module_config(ts_config_module_t module)
{
    if (!ts_config_module_is_registered(module)) {
        ts_console_error("Module not registered\n");
        return;
    }
    
    const char *name = ts_config_module_get_name(module);
    ts_console_printf("\n[%s] Configuration:\n", name);
    print_separator();
    
    switch (module) {
        case TS_CONFIG_MODULE_NET: {
            bool eth_enabled, eth_dhcp;
            char ip[16], netmask[16], gateway[16], hostname[32];
            ts_config_module_get_bool(module, "eth.enabled", &eth_enabled);
            ts_config_module_get_bool(module, "eth.dhcp", &eth_dhcp);
            ts_config_module_get_string(module, "eth.ip", ip, sizeof(ip));
            ts_config_module_get_string(module, "eth.netmask", netmask, sizeof(netmask));
            ts_config_module_get_string(module, "eth.gateway", gateway, sizeof(gateway));
            ts_config_module_get_string(module, "hostname", hostname, sizeof(hostname));
            ts_console_printf("eth.enabled:  %s\n", eth_enabled ? "true" : "false");
            ts_console_printf("eth.dhcp:     %s\n", eth_dhcp ? "true" : "false");
            ts_console_printf("eth.ip:       %s\n", ip);
            ts_console_printf("eth.netmask:  %s\n", netmask);
            ts_console_printf("eth.gateway:  %s\n", gateway);
            ts_console_printf("hostname:     %s\n", hostname);
            break;
        }
        case TS_CONFIG_MODULE_DHCP: {
            bool enabled;
            char start_ip[16], end_ip[16];
            uint32_t lease_time;
            ts_config_module_get_bool(module, "enabled", &enabled);
            ts_config_module_get_string(module, "start_ip", start_ip, sizeof(start_ip));
            ts_config_module_get_string(module, "end_ip", end_ip, sizeof(end_ip));
            ts_config_module_get_uint(module, "lease_time", &lease_time);
            ts_console_printf("enabled:      %s\n", enabled ? "true" : "false");
            ts_console_printf("start_ip:     %s\n", start_ip);
            ts_console_printf("end_ip:       %s\n", end_ip);
            ts_console_printf("lease_time:   %lu s\n", (unsigned long)lease_time);
            break;
        }
        case TS_CONFIG_MODULE_WIFI: {
            char mode[8], ap_ssid[32], ap_pass[32];
            uint32_t channel, max_conn;
            bool hidden;
            ts_config_module_get_string(module, "mode", mode, sizeof(mode));
            ts_config_module_get_string(module, "ap.ssid", ap_ssid, sizeof(ap_ssid));
            ts_config_module_get_string(module, "ap.password", ap_pass, sizeof(ap_pass));
            ts_config_module_get_uint(module, "ap.channel", &channel);
            ts_config_module_get_uint(module, "ap.max_conn", &max_conn);
            ts_config_module_get_bool(module, "ap.hidden", &hidden);
            ts_console_printf("mode:         %s\n", mode);
            ts_console_printf("ap.ssid:      %s\n", ap_ssid);
            ts_console_printf("ap.password:  %s\n", ap_pass);
            ts_console_printf("ap.channel:   %lu\n", (unsigned long)channel);
            ts_console_printf("ap.max_conn:  %lu\n", (unsigned long)max_conn);
            ts_console_printf("ap.hidden:    %s\n", hidden ? "true" : "false");
            break;
        }
        case TS_CONFIG_MODULE_LED: {
            uint32_t brightness, speed;
            char power_effect[16], idle_effect[16];
            ts_config_module_get_uint(module, "brightness", &brightness);
            ts_config_module_get_uint(module, "effect_speed", &speed);
            ts_config_module_get_string(module, "power_on_effect", power_effect, sizeof(power_effect));
            ts_config_module_get_string(module, "idle_effect", idle_effect, sizeof(idle_effect));
            ts_console_printf("brightness:      %lu\n", (unsigned long)brightness);
            ts_console_printf("effect_speed:    %lu\n", (unsigned long)speed);
            ts_console_printf("power_on_effect: %s\n", power_effect);
            ts_console_printf("idle_effect:     %s\n", idle_effect);
            break;
        }
        case TS_CONFIG_MODULE_FAN: {
            char mode[16];
            uint32_t min_duty, max_duty, target_temp;
            ts_config_module_get_string(module, "mode", mode, sizeof(mode));
            ts_config_module_get_uint(module, "min_duty", &min_duty);
            ts_config_module_get_uint(module, "max_duty", &max_duty);
            ts_config_module_get_uint(module, "target_temp", &target_temp);
            ts_console_printf("mode:        %s\n", mode);
            ts_console_printf("min_duty:    %lu%%\n", (unsigned long)min_duty);
            ts_console_printf("max_duty:    %lu%%\n", (unsigned long)max_duty);
            ts_console_printf("target_temp: %luC\n", (unsigned long)target_temp);
            break;
        }
        case TS_CONFIG_MODULE_DEVICE: {
            bool auto_power_on, monitor_enabled;
            uint32_t power_on_delay, force_off_timeout, monitor_interval;
            ts_config_module_get_bool(module, "agx.auto_power_on", &auto_power_on);
            ts_config_module_get_uint(module, "agx.power_on_delay", &power_on_delay);
            ts_config_module_get_uint(module, "agx.force_off_timeout", &force_off_timeout);
            ts_config_module_get_bool(module, "monitor.enabled", &monitor_enabled);
            ts_config_module_get_uint(module, "monitor.interval", &monitor_interval);
            ts_console_printf("agx.auto_power_on:     %s\n", auto_power_on ? "true" : "false");
            ts_console_printf("agx.power_on_delay:    %lu ms\n", (unsigned long)power_on_delay);
            ts_console_printf("agx.force_off_timeout: %lu ms\n", (unsigned long)force_off_timeout);
            ts_console_printf("monitor.enabled:       %s\n", monitor_enabled ? "true" : "false");
            ts_console_printf("monitor.interval:      %lu ms\n", (unsigned long)monitor_interval);
            break;
        }
        case TS_CONFIG_MODULE_SYSTEM: {
            char timezone[16], log_level[16];
            bool console_enabled, webui_enabled;
            uint32_t baudrate, webui_port;
            ts_config_module_get_string(module, "timezone", timezone, sizeof(timezone));
            ts_config_module_get_string(module, "log_level", log_level, sizeof(log_level));
            ts_config_module_get_bool(module, "console.enabled", &console_enabled);
            ts_config_module_get_uint(module, "console.baudrate", &baudrate);
            ts_config_module_get_bool(module, "webui.enabled", &webui_enabled);
            ts_config_module_get_uint(module, "webui.port", &webui_port);
            ts_console_printf("timezone:         %s\n", timezone);
            ts_console_printf("log_level:        %s\n", log_level);
            ts_console_printf("console.enabled:  %s\n", console_enabled ? "true" : "false");
            ts_console_printf("console.baudrate: %lu\n", (unsigned long)baudrate);
            ts_console_printf("webui.enabled:    %s\n", webui_enabled ? "true" : "false");
            ts_console_printf("webui.port:       %lu\n", (unsigned long)webui_port);
            break;
        }
        default:
            ts_console_printf("Unknown module\n");
            break;
    }
    
    print_separator();
    ts_console_printf("Dirty: %s\n", ts_config_module_is_dirty(module) ? "YES (unsaved changes)" : "NO");
}

/*===========================================================================*/
/*                          Command: config --allsave                         */
/*===========================================================================*/

static int do_config_allsave(void)
{
    ts_console_printf("Saving all module configurations...\n");
    
    int success = 0, failed = 0;

    for (int i = 0; i < TS_CONFIG_MODULE_MAX; i++) {
        ts_config_module_t mod = (ts_config_module_t)i;
        if (ts_config_module_is_registered(mod)) {
            esp_err_t ret = ts_config_module_persist(mod);
            if (ret == ESP_OK) {
                ts_console_printf("  [OK] %s\n", ts_config_module_get_name(mod));
                success++;
            } else {
                ts_console_printf("  [FAIL] %s: %s\n", ts_config_module_get_name(mod), esp_err_to_name(ret));
                failed++;
            }
        }
    }
    
    ts_console_printf("\n");
    if (failed == 0) {
        ts_console_success("All %d modules saved to NVS", success);
        ts_console_printf("\n");
    } else {
        ts_console_warn("Saved %d modules, %d failed\n", success, failed);
    }
    
    return failed > 0 ? 1 : 0;
}

/*===========================================================================*/
/*                          Command: config --reset                           */
/*===========================================================================*/

static int do_config_reset(const char *key, ts_config_module_t module)
{
    if (module <= TS_CONFIG_MODULE_MAX) {
        /* 重置模块 */
        const char *mod_name = module == TS_CONFIG_MODULE_MAX ? "ALL" : ts_config_module_get_name(module);
        ts_console_printf("Resetting module %s to defaults...\n", mod_name);
        
        esp_err_t ret = ts_config_module_reset(module, true);
        if (ret == ESP_OK) {
            ts_console_success("Reset complete\n");
            return 0;
        } else {
            ts_console_error("Reset failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
    } else if (key) {
        /* 删除单个键 */
        esp_err_t ret = ts_config_delete(key);
        if (ret == ESP_OK) {
            ts_console_success("Reset: %s\n", key);
            return 0;
        } else {
            ts_console_error("Failed to reset '%s': %s\n", key, esp_err_to_name(ret));
            return 1;
        }
    }
    
    ts_console_error("Specify --key or --module to reset\n");
    return 1;
}

/*===========================================================================*/
/*                    Config Pack Commands (Encrypted Distribution)           */
/*===========================================================================*/

/**
 * @brief 显示配置包系统信息
 */
static int do_pack_info(bool json)
{
    char fingerprint[65] = {0};
    ts_cert_info_t cert_info;
    bool has_cert = (ts_cert_get_info(&cert_info) == ESP_OK);
    bool can_export = ts_config_pack_can_export();
    
    if (has_cert) {
        ts_config_pack_get_cert_fingerprint(fingerprint, sizeof(fingerprint));
    }
    
    if (json) {
        ts_console_printf("{\"has_certificate\":%s,\"can_export\":%s,\"device_cn\":\"%s\","
                          "\"device_ou\":\"%s\",\"fingerprint\":\"%s\"}\n",
                          has_cert ? "true" : "false",
                          can_export ? "true" : "false",
                          has_cert ? cert_info.subject_cn : "",
                          has_cert ? cert_info.subject_ou : "",
                          fingerprint);
        return 0;
    }
    
    ts_console_printf("\n");
    ts_console_printf("╔═══════════════════════════════════════════════════════════╗\n");
    ts_console_printf("║               Config Pack System Status                    ║\n");
    ts_console_printf("╠═══════════════════════════════════════════════════════════╣\n");
    
    if (has_cert) {
        ts_console_printf("║  Device ID     : %-40s ║\n", cert_info.subject_cn);
        ts_console_printf("║  Device Type   : %-40s ║\n", cert_info.subject_ou);
        ts_console_printf("║  Fingerprint   : %.32s...  ║\n", fingerprint);
        ts_console_printf("╠═══════════════════════════════════════════════════════════╣\n");
        
        if (can_export) {
            ts_console_printf("║  Export Status : \033[32m✓ AUTHORIZED (Developer Device)\033[0m        ║\n");
        } else {
            ts_console_printf("║  Export Status : \033[33m✗ Not authorized (User Device)\033[0m          ║\n");
        }
        ts_console_printf("║  Import Status : \033[32m✓ Available\033[0m                              ║\n");
    } else {
        ts_console_printf("║  Certificate   : \033[31mNot installed\033[0m                           ║\n");
        ts_console_printf("║  Export/Import : \033[31mUnavailable (PKI not configured)\033[0m        ║\n");
    }
    
    ts_console_printf("╚═══════════════════════════════════════════════════════════╝\n\n");
    
    if (has_cert && !can_export) {
        ts_console_printf("Note: To export config packs, device must have OU=Developer in certificate.\n");
        ts_console_printf("      Contact PKI administrator to request developer certificate.\n\n");
    }
    
    return 0;
}

/**
 * @brief 导出加密配置包
 */
static int do_pack_export(const char *source_file, const char *cert_file, const char *output_file)
{
    /* 检查导出权限 */
    if (!ts_config_pack_can_export()) {
        ts_console_error("This device is not authorized to export config packs\n");
        ts_console_printf("Only devices with OU=Developer certificate can export.\n");
        return 1;
    }
    
    /* 读取源配置文件 */
    FILE *f = fopen(source_file, "r");
    if (!f) {
        ts_console_error("Cannot open source file: %s\n", source_file);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > 64 * 1024) {
        ts_console_error("Invalid file size: %ld bytes\n", file_size);
        fclose(f);
        return 1;
    }
    
    char *json_content = malloc(file_size + 1);
    if (!json_content) {
        fclose(f);
        return 1;
    }
    
    fread(json_content, 1, file_size, f);
    fclose(f);
    json_content[file_size] = '\0';
    
    /* 读取接收方证书 */
    f = fopen(cert_file, "r");
    if (!f) {
        ts_console_error("Cannot open certificate file: %s\n", cert_file);
        free(json_content);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long cert_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *cert_pem = malloc(cert_size + 1);
    if (!cert_pem) {
        fclose(f);
        free(json_content);
        return 1;
    }
    
    fread(cert_pem, 1, cert_size, f);
    fclose(f);
    cert_pem[cert_size] = '\0';
    
    /* 从源文件名提取配置名 */
    const char *name = strrchr(source_file, '/');
    name = name ? name + 1 : source_file;
    char config_name[64];
    strncpy(config_name, name, sizeof(config_name) - 1);
    config_name[sizeof(config_name) - 1] = '\0';
    /* 去掉 .json 扩展名 */
    char *ext = strstr(config_name, ".json");
    if (ext) *ext = '\0';
    
    /* 创建配置包 */
    ts_config_pack_export_opts_t opts = {
        .recipient_cert_pem = cert_pem,
        .recipient_cert_len = cert_size + 1,
        .description = "Exported from TianShanOS CLI"
    };
    
    char *output = NULL;
    size_t output_len = 0;
    
    ts_console_printf("Creating encrypted config pack...\n");
    ts_console_printf("  Source: %s\n", source_file);
    ts_console_printf("  Target: %s\n", cert_file);
    
    ts_config_pack_result_t result = ts_config_pack_create(
        config_name, json_content, file_size, &opts, &output, &output_len);
    
    free(json_content);
    free(cert_pem);
    
    if (result != TS_CONFIG_PACK_OK) {
        ts_console_error("Failed to create config pack: %s\n", 
                          ts_config_pack_strerror(result));
        return 1;
    }
    
    /* 确定输出文件路径 */
    char out_path[256];
    if (output_file) {
        strncpy(out_path, output_file, sizeof(out_path) - 1);
    } else {
        snprintf(out_path, sizeof(out_path), "/sdcard/config/%s.tscfg", config_name);
    }
    
    /* 保存配置包 */
    esp_err_t ret = ts_config_pack_save(out_path, output, output_len);
    free(output);
    
    if (ret != ESP_OK) {
        ts_console_error("Failed to save config pack: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_success("Config pack created: %s (%zu bytes)\n", out_path, output_len);
    return 0;
}

/**
 * @brief 导入加密配置包
 */
static int do_pack_import(const char *file_path, bool json_output)
{
    ts_console_printf("Loading config pack: %s\n", file_path);
    
    ts_config_pack_t *pack = NULL;
    ts_config_pack_result_t result = ts_config_pack_load(file_path, &pack);
    
    if (result != TS_CONFIG_PACK_OK) {
        ts_console_error("Failed to load config pack: %s\n", 
                          ts_config_pack_strerror(result));
        return 1;
    }
    
    if (json_output) {
        ts_console_printf("{\"name\":\"%s\",\"description\":\"%s\","
                          "\"signer\":\"%s\",\"official\":%s,\"content\":%s}\n",
                          pack->name ? pack->name : "",
                          pack->description ? pack->description : "",
                          pack->sig_info.signer_cn,
                          pack->sig_info.is_official ? "true" : "false",
                          pack->content);
    } else {
        ts_console_printf("\n");
        ts_console_success("Config pack loaded successfully!\n");
        ts_console_printf("  Name: %s\n", pack->name ? pack->name : "unknown");
        if (pack->description) {
            ts_console_printf("  Description: %s\n", pack->description);
        }
        ts_console_printf("  Signed by: %s (%s)\n", 
                          pack->sig_info.signer_cn,
                          pack->sig_info.is_official ? "\033[32mOfficial\033[0m" : "User");
        ts_console_printf("  Content size: %zu bytes\n", pack->content_len);
        ts_console_printf("\n--- Decrypted Content ---\n");
        ts_console_printf("%s\n", pack->content);
        ts_console_printf("--- End ---\n\n");
        
        ts_console_printf("To apply this config, save to appropriate location.\n");
    }
    
    ts_config_pack_free(pack);
    return 0;
}

/**
 * @brief 验证配置包签名
 */
static int do_pack_verify(const char *file_path, bool json_output)
{
    ts_console_printf("Verifying config pack: %s\n", file_path);
    
    ts_config_pack_sig_info_t sig_info = {0};
    ts_config_pack_result_t result = ts_config_pack_verify(file_path, &sig_info);
    
    if (json_output) {
        ts_console_printf("{\"valid\":%s,\"official\":%s,\"signer\":\"%s\","
                          "\"signer_ou\":\"%s\",\"error\":\"%s\"}\n",
                          result == TS_CONFIG_PACK_OK ? "true" : "false",
                          sig_info.is_official ? "true" : "false",
                          sig_info.signer_cn,
                          sig_info.signer_ou,
                          result != TS_CONFIG_PACK_OK ? ts_config_pack_strerror(result) : "");
        return result == TS_CONFIG_PACK_OK ? 0 : 1;
    }
    
    if (result == TS_CONFIG_PACK_OK) {
        ts_console_printf("\n");
        ts_console_success("Signature verification: PASSED\n");
        ts_console_printf("  Signed by: %s\n", sig_info.signer_cn);
        ts_console_printf("  Signer OU: %s\n", sig_info.signer_ou);
        ts_console_printf("  Official:  %s\n", 
                          sig_info.is_official ? "\033[32mYes\033[0m" : "No");
        ts_console_printf("\n");
    } else {
        ts_console_error("Signature verification: FAILED\n");
        ts_console_printf("  Reason: %s\n", ts_config_pack_strerror(result));
        ts_console_printf("\n");
        return 1;
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Main Command Handler                              */
/*===========================================================================*/

static int cmd_config(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_config_args);
    
    if (s_config_args.help->count > 0) {
        ts_console_printf("Usage: config [options]\n\n");
        ts_console_printf("Basic Operations:\n");
        ts_console_printf("  -g, --get           Get configuration value\n");
        ts_console_printf("  -s, --set           Set configuration value\n");
        ts_console_printf("  -l, --list          List all modules\n");
        ts_console_printf("      --reset         Reset to default\n");
        ts_console_printf("\nModule Operations:\n");
        ts_console_printf("      --show          Show module configuration\n");
        ts_console_printf("      --allsave       Save ALL modules to NVS and SD card\n");
        ts_console_printf("      --export        Export module to SD card (plain)\n");
        ts_console_printf("      --meta          Show meta configuration info\n");
        ts_console_printf("\nConfig Pack (Encrypted Distribution):\n");
        ts_console_printf("      --pack-info     Show config pack system status\n");
        ts_console_printf("      --pack-export   Export encrypted .tscfg package\n");
        ts_console_printf("      --pack-import   Import and decrypt .tscfg package\n");
        ts_console_printf("      --pack-verify   Verify .tscfg signature without decrypting\n");
        ts_console_printf("      --file <path>   Source/target file path\n");
        ts_console_printf("      --cert-file <f> Recipient certificate file (for export)\n");
        ts_console_printf("\nParameters:\n");
        ts_console_printf("      --module <name> Module: net,dhcp,wifi,led,fan,device,system,all\n");
        ts_console_printf("  -k, --key <key>     Configuration key\n");
        ts_console_printf("  -v, --value <val>   Configuration value\n");
        ts_console_printf("  -p, --persist       Persist to NVS\n");
        ts_console_printf("  -j, --json          JSON output\n");
        ts_console_printf("  -h, --help          Show this help\n\n");
        ts_console_printf("Examples:\n");
        ts_console_printf("  config --list                           # List all modules\n");
        ts_console_printf("  config --show --module net              # Show net module config\n");
        ts_console_printf("  config --get --module net -k eth.ip     # Get module key\n");
        ts_console_printf("  config --set --module net -k eth.ip -v 192.168.1.100\n");
        ts_console_printf("  config --allsave                        # Save all modules\n");
        ts_console_printf("\nConfig Pack Examples:\n");
        ts_console_printf("  config --pack-info                      # Show pack status\n");
        ts_console_printf("  config --pack-export --file /sdcard/led.json --cert-file /sdcard/device.pem\n");
        ts_console_printf("  config --pack-import --file /sdcard/led.tscfg\n");
        ts_console_printf("  config --pack-verify --file /sdcard/led.tscfg\n");
        return 0;
    }
    
    if (nerrors != 0) {
        arg_print_errors(stderr, s_config_args.end, argv[0]);
        return 1;
    }
    
    bool json = s_config_args.json->count > 0;
    bool persist = s_config_args.persist->count > 0;
    const char *key = s_config_args.key->count > 0 ? s_config_args.key->sval[0] : NULL;
    const char *value = s_config_args.value->count > 0 ? s_config_args.value->sval[0] : NULL;
    const char *mod_str = s_config_args.module->count > 0 ? s_config_args.module->sval[0] : NULL;
    const char *file_path = s_config_args.file->count > 0 ? s_config_args.file->sval[0] : NULL;
    const char *cert_file = s_config_args.cert_file->count > 0 ? s_config_args.cert_file->sval[0] : NULL;
    ts_config_module_t module = parse_module_name(mod_str);
    
    /*========================================================================*/
    /*                     Config Pack Commands                                */
    /*========================================================================*/
    
    /* --pack-info: 显示配置包系统信息 */
    if (s_config_args.pack_info->count > 0) {
        return do_pack_info(json);
    }
    
    /* --pack-export: 导出加密配置包 */
    if (s_config_args.pack_export->count > 0) {
        if (!file_path) {
            ts_console_error("--file required: source JSON file path\n");
            return 1;
        }
        if (!cert_file) {
            ts_console_error("--cert-file required: recipient certificate file\n");
            return 1;
        }
        return do_pack_export(file_path, cert_file, NULL);
    }
    
    /* --pack-import: 导入加密配置包 */
    if (s_config_args.pack_import->count > 0) {
        if (!file_path) {
            ts_console_error("--file required: .tscfg file path\n");
            return 1;
        }
        return do_pack_import(file_path, json);
    }
    
    /* --pack-verify: 验证配置包签名 */
    if (s_config_args.pack_verify->count > 0) {
        if (!file_path) {
            ts_console_error("--file required: .tscfg file path\n");
            return 1;
        }
        return do_pack_verify(file_path, json);
    }
    
    /*========================================================================*/
    /*                     Module Config Commands                              */
    /*========================================================================*/
    
    /* --allsave: 保存所有模块 */
    if (s_config_args.allsave->count > 0) {
        return do_config_allsave();
    }
    
    /* --meta: 显示元配置 */
    if (s_config_args.meta->count > 0) {
        ts_config_meta_dump();
        return 0;
    }
    
    /* --export: 导出到SD卡 */
    if (s_config_args.export_sd->count > 0) {
        if (module > TS_CONFIG_MODULE_MAX) {
            ts_console_error("--module required for --export\n");
            return 1;
        }
        ts_console_printf("Exporting module %s to SD card...\n",
            module == TS_CONFIG_MODULE_MAX ? "ALL" : ts_config_module_get_name(module));
        esp_err_t ret = ts_config_module_export_to_sdcard(module);
        if (ret == ESP_OK) {
            ts_console_success("Export complete\n");
        } else {
            ts_console_error("Export failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        return 0;
    }
    
    /* --show: 显示模块配置 */
    if (s_config_args.show->count > 0) {
        if (module > TS_CONFIG_MODULE_MAX) {
            ts_console_error("--module required for --show\n");
            ts_console_printf("Available: net, dhcp, wifi, led, fan, device, system, all\n");
            return 1;
        }
        if (module == TS_CONFIG_MODULE_MAX) {
            for (int i = 0; i < TS_CONFIG_MODULE_MAX; i++) {
                if (ts_config_module_is_registered((ts_config_module_t)i)) {
                    show_module_config((ts_config_module_t)i);
                }
            }
        } else {
            show_module_config(module);
        }
        return 0;
    }
    
    /* --get: 获取配置 */
    if (s_config_args.get->count > 0) {
        if (!key) {
            ts_console_error("--key required for --get\n");
            return 1;
        }
        if (module <= TS_CONFIG_MODULE_MAX && module != TS_CONFIG_MODULE_MAX) {
            return do_module_get(module, key, json);
        }
        return do_config_get(key, json);
    }
    
    /* --set: 设置配置 */
    if (s_config_args.set->count > 0) {
        if (!key || !value) {
            ts_console_error("--key and --value required for --set\n");
            return 1;
        }
        if (module <= TS_CONFIG_MODULE_MAX && module != TS_CONFIG_MODULE_MAX) {
            return do_module_set(module, key, value);
        }
        return do_config_set(key, value, persist);
    }
    
    /* --reset: 重置 */
    if (s_config_args.reset->count > 0) {
        return do_config_reset(key, module);
    }
    
    /* 默认列出模块 */
    return do_config_list(json);
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_config_register(void)
{
    /* 基础操作 */
    s_config_args.get      = arg_lit0("g", "get", "Get value");
    s_config_args.set      = arg_lit0("s", "set", "Set value");
    s_config_args.list     = arg_lit0("l", "list", "List modules");
    s_config_args.reset    = arg_lit0(NULL, "reset", "Reset to default");
    /* 模块操作 */
    s_config_args.show     = arg_lit0(NULL, "show", "Show module config");
    s_config_args.allsave  = arg_lit0(NULL, "allsave", "Save ALL modules");
    s_config_args.export_sd= arg_lit0(NULL, "export", "Export to SD card (plain)");
    s_config_args.meta     = arg_lit0(NULL, "meta", "Show meta info");
    /* 配置包操作 */
    s_config_args.pack_export = arg_lit0(NULL, "pack-export", "Export encrypted .tscfg");
    s_config_args.pack_import = arg_lit0(NULL, "pack-import", "Import encrypted .tscfg");
    s_config_args.pack_verify = arg_lit0(NULL, "pack-verify", "Verify .tscfg signature");
    s_config_args.pack_info   = arg_lit0(NULL, "pack-info", "Show pack system status");
    s_config_args.file        = arg_str0(NULL, "file", "<path>", "File path");
    s_config_args.cert_file   = arg_str0(NULL, "cert-file", "<path>", "Certificate file");
    /* 参数 */
    s_config_args.module   = arg_str0(NULL, "module", "<name>", "Module name");
    s_config_args.key      = arg_str0("k", "key", "<key>", "Config key");
    s_config_args.value    = arg_str0("v", "value", "<value>", "Config value");
    s_config_args.ns       = arg_str0(NULL, "namespace", "<ns>", "Namespace (legacy)");
    s_config_args.persist  = arg_lit0("p", "persist", "Persist to NVS");
    s_config_args.json     = arg_lit0("j", "json", "JSON output");
    s_config_args.help     = arg_lit0("h", "help", "Show help");
    s_config_args.end      = arg_end(20);
    
    const ts_console_cmd_t cmd = {
        .command = "config",
        .help = "Configuration management (config --help for details)",
        .hint = NULL,
        .category = TS_CMD_CAT_CONFIG,
        .func = cmd_config,
        .argtable = &s_config_args
    };
    
    esp_err_t ret = ts_console_register_cmd(&cmd);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Config commands registered (with pack support)");
    }
    
    return ret;
}
