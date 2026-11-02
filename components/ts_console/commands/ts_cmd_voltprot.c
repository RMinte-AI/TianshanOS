/**
 * @file ts_cmd_voltprot.c
 * @brief Voltage Protection Policy CLI Commands
 * 
 * 电压保护策略命令行接口
 * 
 * 命令格式：
 *   voltprot --status                  # 查看保护状态
 *   voltprot --test                    # 触发测试（模拟低电压）
 *   voltprot --reset                   # 复位保护状态
 *   voltprot --config                  # 查看/修改配置
 *   voltprot --config --low 12.6       # 设置低电压阈值
 *   voltprot --config --recovery 18.0  # 设置恢复电压阈值
 *   voltprot --debug                   # 调试模式（实时电压）
 */

#include <stdio.h>
#include "ts_console.h"
#include <string.h>
#include "ts_console.h"
#include "esp_console.h"
#include "ts_console.h"
#include "argtable3/argtable3.h"
#include "ts_console.h"
#include "ts_power_policy.h"
#include "ts_console.h"
#include "ts_power_monitor.h"
#include "ts_console.h"
#include "ts_api.h"
#include "ts_console.h"
#include "cJSON.h"
#include "ts_console.h"

#define TAG "cmd_voltprot"

/*===========================================================================*/
/*                          Arguments Structure                               */
/*===========================================================================*/

static struct {
    struct arg_lit *status;
    struct arg_lit *test;
    struct arg_lit *reset;
    struct arg_lit *config;
    struct arg_dbl *low_threshold;
    struct arg_dbl *recovery_threshold;
    struct arg_int *delay;
    struct arg_int *recovery_hold;
    struct arg_int *fan_delay;
    struct arg_lit *debug;
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_voltprot_args;

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief 获取状态名称
 */
static const char *get_state_emoji(ts_power_policy_state_t state)
{
    switch (state) {
        case TS_POWER_POLICY_STATE_NORMAL:      return "✅";
        case TS_POWER_POLICY_STATE_LOW_VOLTAGE: return "⚠️";
        case TS_POWER_POLICY_STATE_SHUTDOWN:    return "🔴";
        case TS_POWER_POLICY_STATE_PROTECTED:   return "🛡️";
        case TS_POWER_POLICY_STATE_RECOVERY:    return "🔄";
        default:                                return "❓";
    }
}

/**
 * @brief 打印保护状态
 */
static void print_status(bool json_format)
{
    /* JSON 模式通过 API 获取 */
    if (json_format) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("power.protection.status", NULL, &result);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_Print(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("错误: 无法获取保护状态\n");
        }
        ts_api_result_free(&result);
        return;
    }
    
    /* 格式化输出：直接调用底层 */
    ts_power_policy_status_t status;
    
    if (ts_power_policy_get_status(&status) != ESP_OK) {
        ts_console_printf("错误: 无法获取保护状态\n");
        return;
    }
    
    /* 如果监控任务没有运行，直接从 power_monitor 读取当前电压 */
    float display_voltage = status.current_voltage;
    if (!status.running || display_voltage < 0.01f) {
        ts_power_voltage_data_t voltage_data;
        esp_err_t ret = ts_power_monitor_read_voltage_now(&voltage_data);
        if (ret != ESP_OK) {
            /* 尝试先初始化 power_monitor */
            ts_power_monitor_init(NULL);
            ret = ts_power_monitor_read_voltage_now(&voltage_data);
        }
        if (ret == ESP_OK) {
            display_voltage = voltage_data.supply_voltage;
        }
    }
    
    float low_threshold, recovery_threshold;
    ts_power_policy_get_thresholds(&low_threshold, &recovery_threshold);
    
    ts_console_printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    ts_console_printf("║            ⚡ 电压保护状态 (Voltage Protection)              ║\n");
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    ts_console_printf("║ 状态:  %s %-20s                           ║\n",
           get_state_emoji(status.state),
           ts_power_policy_get_state_name(status.state));
    ts_console_printf("║ 电压:  %.2f V                                               ║\n",
           display_voltage);
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    ts_console_printf("║ 阈值配置:                                                    ║\n");
    ts_console_printf("║   低电压阈值:   %.1f V                                      ║\n",
           low_threshold);
    ts_console_printf("║   恢复电压阈值: %.1f V                                      ║\n",
           recovery_threshold);
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    if (status.state == TS_POWER_POLICY_STATE_LOW_VOLTAGE) {
        ts_console_printf("║ ⏳ 关机倒计时: %lu 秒                                       ║\n",
               (unsigned long)status.countdown_remaining_sec);
    }
    
    if (status.state == TS_POWER_POLICY_STATE_RECOVERY) {
        ts_console_printf("║ 🔄 恢复计时器: %lu 秒                                       ║\n",
               (unsigned long)status.recovery_timer_sec);
    }
    
    ts_console_printf("║ 统计:                                                        ║\n");
    ts_console_printf("║   保护触发次数: %lu                                          ║\n",
           (unsigned long)status.protection_count);
    ts_console_printf("║   运行时间: %lu ms                                           ║\n",
           (unsigned long)status.uptime_ms);
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    ts_console_printf("║ 设备状态:                                                    ║\n");
    ts_console_printf("║   AGX 电源:  %s    LPMU 电源:  %s    AGX 连接:  %s       ║\n",
           status.device_status.agx_powered ? "✅" : "❌",
           status.device_status.lpmu_powered ? "✅" : "❌",
           status.device_status.agx_connected ? "✅" : "❌");
    ts_console_printf("╚══════════════════════════════════════════════════════════════╝\n");
}

/**
 * @brief 打印配置信息
 */
static void print_config(void)
{
    ts_power_policy_config_t config;
    if (ts_power_policy_get_config(&config) != ESP_OK) {
        ts_console_printf("错误: 无法获取配置\n");
        return;
    }
    
    ts_console_printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    ts_console_printf("║              ⚙️  电压保护配置                                 ║\n");
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    ts_console_printf("║ 电压阈值:                                                    ║\n");
    ts_console_printf("║   低电压阈值:       %.2f V  (默认: %.1f V)                  ║\n",
           config.low_voltage_threshold, TS_POWER_POLICY_LOW_VOLTAGE_DEFAULT);
    ts_console_printf("║   恢复电压阈值:     %.2f V  (默认: %.1f V)                  ║\n",
           config.recovery_voltage_threshold, TS_POWER_POLICY_RECOVERY_VOLTAGE_DEFAULT);
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    ts_console_printf("║ 时间配置:                                                    ║\n");
    ts_console_printf("║   关机倒计时:       %u 秒   (默认: %u 秒)                    ║\n",
           config.shutdown_delay_sec, TS_POWER_POLICY_SHUTDOWN_DELAY_DEFAULT);
    ts_console_printf("║   恢复稳定等待:     %u 秒   (默认: %u 秒)                     ║\n",
           config.recovery_hold_sec, TS_POWER_POLICY_RECOVERY_HOLD_DEFAULT);
    ts_console_printf("║   风扇关闭延迟:     %u 秒   (默认: %u 秒)                    ║\n",
           config.fan_stop_delay_sec, TS_POWER_POLICY_FAN_STOP_DELAY_DEFAULT);
    ts_console_printf("╠══════════════════════════════════════════════════════════════╣\n");
    ts_console_printf("║ 修改配置:                                                    ║\n");
    ts_console_printf("║   voltprot -c --low <V>        设置低电压阈值                ║\n");
    ts_console_printf("║   voltprot -c --recovery <V>   设置恢复电压阈值              ║\n");
    ts_console_printf("║   voltprot -c --delay <sec>    设置关机倒计时                ║\n");
    ts_console_printf("║   voltprot -c --hold <sec>     设置恢复等待时间              ║\n");
    ts_console_printf("║   voltprot -c --fan <sec>      设置风扇关闭延迟              ║\n");
    ts_console_printf("╚══════════════════════════════════════════════════════════════╝\n");
}

/*===========================================================================*/
/*                          Command Handler                                   */
/*===========================================================================*/

static int cmd_voltprot_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_voltprot_args);
    
    /* 显示帮助 */
    if (s_voltprot_args.help->count > 0) {
        ts_console_printf("\n电压保护策略命令\n\n");
        ts_console_printf("用法: voltprot [选项]\n\n");
        arg_print_glossary(stdout, (void **)&s_voltprot_args, "  %-25s %s\n");
        ts_console_printf("\n示例:\n");
        ts_console_printf("  voltprot --status                显示保护状态\n");
        ts_console_printf("  voltprot --test                  触发测试模式\n");
        ts_console_printf("  voltprot --reset                 复位保护状态\n");
        ts_console_printf("  voltprot --config                显示配置\n");
        ts_console_printf("  voltprot --config --low 12.0     设置低电压阈值为 12.0V\n");
        ts_console_printf("  voltprot --debug                 实时监控模式\n");
        return 0;
    }
    
    if (nerrors > 0) {
        arg_print_errors(stderr, s_voltprot_args.end, "voltprot");
        return 1;
    }
    
    bool json = s_voltprot_args.json->count > 0;
    
    /* 检查是否已初始化 */
    if (!ts_power_policy_is_initialized()) {
        /* 尝试初始化 */
        esp_err_t ret = ts_power_policy_init(NULL);
        if (ret != ESP_OK) {
            ts_console_printf("错误: 电压保护未初始化且初始化失败: %s\n", esp_err_to_name(ret));
            return 1;
        }
        ts_console_printf("电压保护已自动初始化\n");
    }
    
    /* --status: 显示状态 */
    if (s_voltprot_args.status->count > 0) {
        print_status(json);
        return 0;
    }
    
    /* --test: 触发测试 */
    if (s_voltprot_args.test->count > 0) {
        if (!ts_power_policy_is_running()) {
            /* 启动监控 */
            esp_err_t ret = ts_power_policy_start();
            if (ret != ESP_OK) {
                ts_console_printf("错误: 无法启动保护监控: %s\n", esp_err_to_name(ret));
                return 1;
            }
            ts_console_printf("保护监控已启动\n");
        }
        
        ts_console_printf("⚠️  触发测试模式...\n");
        ts_console_printf("将模拟低电压状态，开始 %u 秒倒计时\n", TS_POWER_POLICY_SHUTDOWN_DELAY_DEFAULT);
        ts_console_printf("使用 'voltprot --reset' 取消测试\n\n");
        
        esp_err_t ret = ts_power_policy_trigger_test();
        if (ret == ESP_OK) {
            ts_console_printf("✅ 测试已触发\n");
            print_status(false);
        } else {
            ts_console_printf("❌ 触发失败: %s\n", esp_err_to_name(ret));
            return 1;
        }
        return 0;
    }
    
    /* --reset: 复位保护 */
    if (s_voltprot_args.reset->count > 0) {
        ts_console_printf("🔄 复位保护状态...\n");
        ts_console_printf("⚠️  注意: 这将重启 ESP32\n");
        
        esp_err_t ret = ts_power_policy_reset();
        if (ret != ESP_OK) {
            ts_console_printf("❌ 复位失败: %s\n", esp_err_to_name(ret));
            return 1;
        }
        /* 不会到达这里，因为会重启 */
        return 0;
    }
    
    /* --config: 配置管理 */
    if (s_voltprot_args.config->count > 0) {
        bool modified = false;
        
        /* 读取完整阈值对，再合并本次明确指定的值 */
        if (s_voltprot_args.low_threshold->count > 0 || s_voltprot_args.recovery_threshold->count > 0) {
            float low, recovery;
            esp_err_t ret = ts_power_policy_get_thresholds(&low, &recovery);
            if (ret != ESP_OK) {
                ts_console_printf("❌ 读取阈值失败: %s\n", esp_err_to_name(ret));
                return 1;
            }
            if (s_voltprot_args.low_threshold->count > 0)
                low = (float)s_voltprot_args.low_threshold->dval[0];
            if (s_voltprot_args.recovery_threshold->count > 0)
                recovery = (float)s_voltprot_args.recovery_threshold->dval[0];
            
            ret = ts_power_policy_set_thresholds(low, recovery);
            if (ret == ESP_OK) {
                if (s_voltprot_args.low_threshold->count > 0)
                    ts_console_printf("✅ 低电压阈值已设置为 %.2f V\n", low);
                if (s_voltprot_args.recovery_threshold->count > 0)
                    ts_console_printf("✅ 恢复电压阈值已设置为 %.2f V\n", recovery);
                modified = true;
            } else {
                ts_console_printf("❌ 设置失败: %s\n", esp_err_to_name(ret));
            }
        }
        
        /* 修改关机延迟 */
        if (s_voltprot_args.delay->count > 0) {
            uint32_t delay = (uint32_t)s_voltprot_args.delay->ival[0];
            esp_err_t ret = ts_power_policy_set_shutdown_delay(delay);
            if (ret == ESP_OK) {
                ts_console_printf("✅ 关机倒计时已设置为 %lu 秒\n", (unsigned long)delay);
                modified = true;
            } else {
                ts_console_printf("❌ 设置失败: %s\n", esp_err_to_name(ret));
            }
        }
        
        /* 修改恢复等待时间 */
        if (s_voltprot_args.recovery_hold->count > 0) {
            uint32_t hold = (uint32_t)s_voltprot_args.recovery_hold->ival[0];
            esp_err_t ret = ts_power_policy_set_recovery_hold(hold);
            if (ret == ESP_OK) {
                ts_console_printf("✅ 恢复等待时间已设置为 %lu 秒\n", (unsigned long)hold);
                modified = true;
            } else {
                ts_console_printf("❌ 设置失败: %s\n", esp_err_to_name(ret));
            }
        }
        
        /* 修改风扇关闭延迟 */
        if (s_voltprot_args.fan_delay->count > 0) {
            uint32_t fan = (uint32_t)s_voltprot_args.fan_delay->ival[0];
            esp_err_t ret = ts_power_policy_set_fan_stop_delay(fan);
            if (ret == ESP_OK) {
                ts_console_printf("✅ 风扇关闭延迟已设置为 %lu 秒\n", (unsigned long)fan);
                modified = true;
            } else {
                ts_console_printf("❌ 设置失败: %s\n", esp_err_to_name(ret));
            }
        }
        
        if (!modified) {
            print_config();
        }
        return 0;
    }
    
    /* --debug: 调试模式（非阻塞，串口通过日志输出，Web 通过 WebSocket 推送）*/
    if (s_voltprot_args.debug->count > 0) {
        if (!ts_power_policy_is_running()) {
            esp_err_t ret = ts_power_policy_start();
            if (ret != ESP_OK) {
                ts_console_printf("错误: 无法启动保护监控: %s\n", esp_err_to_name(ret));
                return 1;
            }
        }
        
        if (ts_power_policy_is_debug_mode()) {
            /* 已在调试模式，关闭它 */
            ts_power_policy_set_debug_mode(false, 0);
            ts_console_printf("🔍 调试模式已关闭\n");
        } else {
            /* 启用调试模式，30秒后自动关闭 */
            ts_power_policy_set_debug_mode(true, 30);
            ts_console_printf("🔍 调试模式已启用（30秒）\n");
            ts_console_printf("   串口: 通过日志实时输出\n");
            ts_console_printf("   Web:  通过 WebSocket 实时推送\n");
            ts_console_printf("   再次执行 voltprot --debug 可提前关闭\n");
        }
        return 0;
    }
    
    /* 默认显示状态 */
    print_status(json);
    return 0;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_voltprot_register(void)
{
    /* 初始化参数 */
    s_voltprot_args.status = arg_litn("s", "status", 0, 1, "显示保护状态");
    s_voltprot_args.test = arg_litn("t", "test", 0, 1, "触发测试（模拟低电压）");
    s_voltprot_args.reset = arg_litn("r", "reset", 0, 1, "复位保护状态（重启ESP32）");
    s_voltprot_args.config = arg_litn("c", "config", 0, 1, "显示/修改配置");
    s_voltprot_args.low_threshold = arg_dbln("l", "low", "<V>", 0, 1, "低电压阈值 (V)");
    s_voltprot_args.recovery_threshold = arg_dbln("R", "recovery", "<V>", 0, 1, "恢复电压阈值 (V)");
    s_voltprot_args.delay = arg_intn("d", "delay", "<sec>", 0, 1, "关机倒计时 (10-600秒)");
    s_voltprot_args.recovery_hold = arg_intn("H", "hold", "<sec>", 0, 1, "恢复等待时间 (1-300秒)");
    s_voltprot_args.fan_delay = arg_intn("f", "fan", "<sec>", 0, 1, "风扇关闭延迟 (10-600秒)");
    s_voltprot_args.debug = arg_litn(NULL, "debug", 0, 1, "调试模式（30秒实时监控）");
    s_voltprot_args.json = arg_litn("j", "json", 0, 1, "JSON 格式输出");
    s_voltprot_args.help = arg_litn("h", "help", 0, 1, "显示帮助");
    s_voltprot_args.end = arg_end(8);
    
    const esp_console_cmd_t cmd = {
        .command = "voltprot",
        .help = "电压保护策略管理",
        .hint = NULL,
        .func = &cmd_voltprot_handler,
        .argtable = &s_voltprot_args,
    };
    
    return esp_console_cmd_register(&cmd);
}
