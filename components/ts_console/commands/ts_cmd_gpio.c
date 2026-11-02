/**
 * @file ts_cmd_gpio.c
 * @brief GPIO 直接控制命令（高优先级）
 *
 * 提供底层 GPIO 引脚的直接控制能力，用于调试和硬件测试。
 * 此命令优先级高于其他驱动程序，可以直接覆盖 GPIO 状态。
 *
 * 用法:
 *   gpio <pin> high [ms]        设置高电平（可选保持时间后恢复）
 *   gpio <pin> low [ms]         设置低电平（可选保持时间后恢复）
 *   gpio <pin> pulse <ms>       输出正脉冲（HIGH 持续 ms 后恢复 LOW）
 *   gpio <pin> pulse <ms> -n    输出负脉冲（LOW 持续 ms 后恢复 HIGH）
 *   gpio <pin> toggle           切换当前电平
 *   gpio <pin> input            切换为输入模式并读取
 *   gpio <pin> reset            重置引脚到默认状态
 *   gpio --list                 列出系统已知引脚
 *   gpio --info <pin>           显示引脚详情
 *
 * @version 2.0.0
 * @date 2025-01-19
 */

#include "ts_cmd_all.h"
#include "ts_console.h"
#include "ts_api.h"
#include "ts_console.h"
#include "argtable3/argtable3.h"
#include "ts_console.h"
#include "driver/gpio.h"
#include "ts_console.h"
#include "esp_console.h"
#include "ts_console.h"
#include "esp_log.h"
#include "ts_console.h"
#include "freertos/FreeRTOS.h"
#include "ts_console.h"
#include "freertos/task.h"
#include "ts_console.h"
#include <stdio.h>
#include "ts_console.h"
#include <stdlib.h>
#include "ts_console.h"
#include <string.h>
#include "ts_console.h"

static const char *TAG = "cmd_gpio";

/* ============================================================================
 * 参数定义
 * ============================================================================ */

static struct {
    struct arg_lit *list;       // --list: 列出已配置的引脚
    struct arg_int *info;       // --info <pin>: 显示引脚详情
    struct arg_int *pin;        // <pin>: GPIO 引脚号
    struct arg_str *action;     // <action>: high|low|pulse|toggle|input|reset
    struct arg_int *duration;   // [ms]: 持续时间（毫秒）
    struct arg_lit *negative;   // -n/--negative: 负脉冲（pulse 时先 LOW 后 HIGH）
    struct arg_lit *no_restore; // --no-restore: 不恢复原电平
    struct arg_lit *json;       // --json: JSON 格式输出
    struct arg_lit *help;       // --help: 显示帮助
    struct arg_end *end;
} s_gpio_args;

/* ============================================================================
 * 私有函数声明
 * ============================================================================ */

static void print_gpio_usage(void);
static int cmd_gpio_handler(int argc, char **argv);
static esp_err_t gpio_set_output_level(int pin, int level);
static esp_err_t gpio_output_pulse(int pin, int duration_ms, bool negative);
static esp_err_t gpio_hold_level(int pin, int level, int duration_ms, bool restore);
static esp_err_t gpio_toggle(int pin);
static void print_pin_info(int pin, bool json);
static void print_configured_pins(bool json);

/* ============================================================================
 * 可控引脚定义
 * ============================================================================ */

typedef struct {
    int pin;
    const char *name;
    const char *description;
    int default_level;  // 默认电平 (0=LOW, 1=HIGH)
} controllable_pin_t;

// 允许通过 GPIO 命令控制的引脚（仅限设备控制相关）
static const controllable_pin_t s_controllable_pins[] = {
    {1,  "AGX_RESET",         "推理计算机 复位 (HIGH=复位, LOW=正常)",           0},
    {2,  "LPMU_RESET",        "应用计算机 复位 (HIGH=复位, LOW=正常)",          0},
    {3,  "AGX_FORCE_SHUTDOWN", "推理计算机 强制关机 (LOW=开机, HIGH=关机)",       0},
    {8,  "USB_MUX_0",         "USB MUX 选择位0",                          0},
    {17, "RTL8367_RST",       "网络交换机复位 (HIGH=复位, LOW=正常)",     0},
    {39, "ETH_RST",           "W5500 以太网复位 (LOW=复位, HIGH=正常)",   1},
    {40, "AGX_RECOVERY",      "推理计算机 恢复模式 (HIGH=恢复, LOW=正常)",       0},
    {46, "LPMU_POWER",        "应用计算机 电源键 (HIGH=按下, LOW=释放)",        0},
    {48, "USB_MUX_1",         "USB MUX 选择位1",                          0},
};

#define NUM_CONTROLLABLE_PINS (sizeof(s_controllable_pins) / sizeof(s_controllable_pins[0]))

/* ============================================================================
 * GPIO 操作实现
 * ============================================================================ */

/**
 * @brief 查找可控引脚信息
 */
static const controllable_pin_t* find_controllable_pin(int pin)
{
    for (int i = 0; i < NUM_CONTROLLABLE_PINS; i++) {
        if (s_controllable_pins[i].pin == pin) {
            return &s_controllable_pins[i];
        }
    }
    return NULL;
}

/**
 * @brief 设置 GPIO 为输出模式并设置电平（高优先级，直接覆盖）
 */
static esp_err_t gpio_set_output_level(int pin, int level)
{
    // 重要：先设置电平，再配置方向（避免毛刺）
    gpio_set_level(pin, level);
    
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO%d as output: %s", pin, esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

/**
 * @brief 输出脉冲信号
 * 
 * @param pin GPIO 引脚号
 * @param duration_ms 脉冲持续时间（毫秒）
 * @param negative true=负脉冲（LOW->HIGH），false=正脉冲（HIGH->LOW）
 */
static esp_err_t gpio_output_pulse(int pin, int duration_ms, bool negative)
{
    esp_err_t ret;
    int pulse_level = negative ? 0 : 1;
    int restore_level = negative ? 1 : 0;
    
    // 1. 先设置恢复电平，确保起始状态正确
    ret = gpio_set_output_level(pin, restore_level);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // 2. 切换到脉冲电平
    gpio_set_level(pin, pulse_level);
    
    // 3. 保持指定时间
    if (duration_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
    }
    
    // 4. 恢复原电平
    gpio_set_level(pin, restore_level);
    
    return ESP_OK;
}

/**
 * @brief 保持指定电平一段时间
 * 
 * @param pin GPIO 引脚号
 * @param level 电平（0=LOW, 1=HIGH）
 * @param duration_ms 持续时间（毫秒）
 * @param restore 是否在结束后恢复原电平
 */
static esp_err_t gpio_hold_level(int pin, int level, int duration_ms, bool restore)
{
    // 先读取当前电平（用于恢复）
    int original_level = gpio_get_level(pin);
    
    // 设置目标电平
    esp_err_t ret = gpio_set_output_level(pin, level);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // 保持指定时间
    if (duration_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        
        // 恢复原电平
        if (restore) {
            gpio_set_level(pin, original_level);
        }
    }
    
    return ESP_OK;
}

/**
 * @brief 切换 GPIO 电平
 */
static esp_err_t gpio_toggle(int pin)
{
    // 读取当前电平
    int current_level = gpio_get_level(pin);
    int new_level = current_level ? 0 : 1;
    
    // 设置新电平
    return gpio_set_output_level(pin, new_level);
}

/**
 * @brief 打印引脚信息
 */
static void print_pin_info(int pin, bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        cJSON *params = cJSON_CreateObject();
        cJSON_AddNumberToObject(params, "pin", pin);
        
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("gpio.info", params, &result);
        cJSON_Delete(params);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"gpio\":%d,\"error\":\"%s\"}\n", pin,
                result.message ? result.message : esp_err_to_name(ret));
        }
        ts_api_result_free(&result);
        return;
    }
    
    /* 格式化输出模式 */
    const controllable_pin_t *info = find_controllable_pin(pin);
    int level = gpio_get_level(pin);
    
    if (info) {
        const char *status = (level == info->default_level) ? "默认" : "已修改";
        ts_console_printf("GPIO%d (%s):\n", pin, info->name);
        ts_console_printf("  当前电平: %d (%s)\n", level, level ? "HIGH" : "LOW");
        ts_console_printf("  默认电平: %d (%s)\n", info->default_level, info->default_level ? "HIGH" : "LOW");
        ts_console_printf("  状态: %s\n", status);
        ts_console_printf("  说明: %s\n", info->description);
    } else {
        ts_console_printf("GPIO%d 不在可控引脚列表中\n", pin);
        ts_console_printf("当前电平: %d (%s)\n", level, level ? "HIGH" : "LOW");
    }
}

/**
 * @brief 列出可控引脚状态
 */
static void print_configured_pins(bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("gpio.list", NULL, &result);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"error\":\"%s\"}\n", 
                result.message ? result.message : esp_err_to_name(ret));
        }
        ts_api_result_free(&result);
        return;
    }
    
    /* 格式化输出模式 */
    ts_console_printf("可控引脚列表:\n");
    ts_console_printf("──────────────────────────────────────────────────────────────────────────\n");
    ts_console_printf("  GPIO   名称                 当前   默认   说明\n");
    ts_console_printf("──────────────────────────────────────────────────────────────────────────\n");
    for (int i = 0; i < NUM_CONTROLLABLE_PINS; i++) {
        int level = gpio_get_level(s_controllable_pins[i].pin);
        const char *status = (level == s_controllable_pins[i].default_level) ? " " : "*";
        ts_console_printf("  %2d     %-20s %s%s   %s     %s\n", 
               s_controllable_pins[i].pin, 
               s_controllable_pins[i].name,
               status,
               level ? "HIGH" : "LOW ",
               s_controllable_pins[i].default_level ? "HIGH" : "LOW ",
               s_controllable_pins[i].description);
    }
    ts_console_printf("──────────────────────────────────────────────────────────────────────────\n");
    ts_console_printf("  * 表示当前电平与默认值不同\n");
}

/**
 * @brief 打印帮助信息
 */
static void print_gpio_usage(void)
{
    ts_console_printf("GPIO 直接控制命令（高优先级）\n\n");
    ts_console_printf("用法:\n");
    ts_console_printf("  gpio <pin|name> high [ms]   - 设置高电平（可选保持时间后恢复）\n");
    ts_console_printf("  gpio <pin|name> low [ms]    - 设置低电平（可选保持时间后恢复）\n");
    ts_console_printf("  gpio <pin|name> pulse <ms>  - 输出正脉冲 (HIGH 持续 ms 后恢复 LOW)\n");
    ts_console_printf("  gpio <pin|name> pulse <ms> -n - 输出负脉冲 (LOW 持续 ms 后恢复 HIGH)\n");
    ts_console_printf("  gpio <pin|name> toggle      - 切换当前电平\n");
    ts_console_printf("  gpio <pin|name> input       - 读取当前电平（不改变模式）\n");
    ts_console_printf("  gpio <pin|name> reset       - 重置引脚到默认电平\n");
    ts_console_printf("  gpio --list                 - 列出所有可控引脚\n");
    ts_console_printf("  gpio --info <pin>           - 显示引脚详情\n");
    ts_console_printf("\n");
    ts_console_printf("可控引脚:\n");
    ts_console_printf("  GPIO  名称\n");
    for (int i = 0; i < NUM_CONTROLLABLE_PINS; i++) {
        ts_console_printf("   %2d   %s\n", s_controllable_pins[i].pin, s_controllable_pins[i].name);
    }
    ts_console_printf("\n");
    ts_console_printf("选项:\n");
    ts_console_printf("  -n, --negative              - 负脉冲模式（与 pulse 配合使用）\n");
    ts_console_printf("  --no-restore                - 不恢复原电平（与 high/low 配合使用）\n");
    ts_console_printf("  -j, --json                  - JSON 格式输出\n");
    ts_console_printf("\n");
    ts_console_printf("示例:\n");
    ts_console_printf("  gpio 1 pulse 1000           # AGX_RESET: 1秒正脉冲（复位推理计算机）\n");
    ts_console_printf("  gpio AGX_RESET pulse 1000   # 同上，使用名称\n");
    ts_console_printf("  gpio 3 high 8000            # 推理计算机 强制关机 8 秒\n");
    ts_console_printf("  gpio 46 high 300            # 应用计算机 电源键脉冲 300ms\n");
    ts_console_printf("  gpio --list                 # 查看所有引脚状态\n");
    ts_console_printf("\n");
    ts_console_printf("⚠️  警告: 此命令直接操作硬件，优先级高于其他驱动！\n");
}

/* ============================================================================
 * 命令处理函数
 * ============================================================================ */

static int cmd_gpio_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_gpio_args);
    
    // 显示帮助
    if (s_gpio_args.help->count > 0) {
        print_gpio_usage();
        return 0;
    }
    
    bool json = (s_gpio_args.json->count > 0);
    bool negative = (s_gpio_args.negative->count > 0);
    bool no_restore = (s_gpio_args.no_restore->count > 0);
    int duration_ms = (s_gpio_args.duration->count > 0) ? s_gpio_args.duration->ival[0] : 0;
    
    // 列出已配置的引脚
    if (s_gpio_args.list->count > 0) {
        print_configured_pins(json);
        return 0;
    }
    
    // 显示引脚详情
    if (s_gpio_args.info->count > 0) {
        int pin = s_gpio_args.info->ival[0];
        print_pin_info(pin, json);
        return 0;
    }
    
    // 需要引脚和操作参数
    if (s_gpio_args.pin->count == 0 || s_gpio_args.action->count == 0) {
        if (nerrors > 0) {
            arg_print_errors(stderr, s_gpio_args.end, "gpio");
        }
        print_gpio_usage();
        return 1;
    }
    
    int pin = s_gpio_args.pin->ival[0];
    const char *action = s_gpio_args.action->sval[0];
    
    // 验证引脚是否在可控列表中
    const controllable_pin_t *pin_info = find_controllable_pin(pin);
    if (!pin_info) {
        ts_console_printf("错误: GPIO%d 不在可控引脚列表中\n", pin);
        ts_console_printf("可控引脚: ");
        for (int i = 0; i < NUM_CONTROLLABLE_PINS; i++) {
            ts_console_printf("%d", s_controllable_pins[i].pin);
            if (i < NUM_CONTROLLABLE_PINS - 1) printf(", ");
        }
        ts_console_printf("\n使用 'gpio --list' 查看完整列表\n");
        return 1;
    }
    
    esp_err_t ret = ESP_OK;
    const char *pin_name = pin_info->name;
    
    // ===== 执行操作 =====
    
    if (strcmp(action, "high") == 0) {
        // 设置高电平
        if (duration_ms > 0 && !no_restore) {
            // 保持指定时间后恢复
            ret = gpio_hold_level(pin, 1, duration_ms, true);
            if (ret == ESP_OK) {
                if (json) {
                    ts_console_printf("{\"gpio\":%d,\"name\":\"%s\",\"action\":\"high\",\"duration_ms\":%d,\"restored\":true}\n", pin, pin_name, duration_ms);
                } else {
                    ts_console_printf("%s (GPIO%d) → HIGH %d ms → 已恢复\n", pin_name, pin, duration_ms);
                }
            }
        } else {
            // 设置并保持
            ret = gpio_set_output_level(pin, 1);
            if (ret == ESP_OK) {
                if (json) {
                    ts_console_printf("{\"gpio\":%d,\"name\":\"%s\",\"action\":\"high\",\"hold\":true}\n", pin, pin_name);
                } else {
                    ts_console_printf("%s (GPIO%d) → HIGH (保持)\n", pin_name, pin);
                }
            }
        }
    } else if (strcmp(action, "low") == 0) {
        // 设置低电平
        if (duration_ms > 0 && !no_restore) {
            ret = gpio_hold_level(pin, 0, duration_ms, true);
            if (ret == ESP_OK) {
                if (json) {
                    ts_console_printf("{\"gpio\":%d,\"name\":\"%s\",\"action\":\"low\",\"duration_ms\":%d,\"restored\":true}\n", pin, pin_name, duration_ms);
                } else {
                    ts_console_printf("%s (GPIO%d) → LOW %d ms → 已恢复\n", pin_name, pin, duration_ms);
                }
            }
        } else {
            ret = gpio_set_output_level(pin, 0);
            if (ret == ESP_OK) {
                if (json) {
                    ts_console_printf("{\"gpio\":%d,\"name\":\"%s\",\"action\":\"low\",\"hold\":true}\n", pin, pin_name);
                } else {
                    ts_console_printf("%s (GPIO%d) → LOW (保持)\n", pin_name, pin);
                }
            }
        }
    } else if (strcmp(action, "pulse") == 0) {
        // 输出脉冲
        if (duration_ms <= 0) {
            ts_console_printf("错误: pulse 操作需要指定持续时间（毫秒）\n");
            ts_console_printf("用法: gpio %d pulse <ms>\n", pin);
            return 1;
        }
        
        ret = gpio_output_pulse(pin, duration_ms, negative);
        if (ret == ESP_OK) {
            if (json) {
                ts_console_printf("{\"gpio\":%d,\"name\":\"%s\",\"action\":\"pulse\",\"duration_ms\":%d,\"negative\":%s}\n", 
                       pin, pin_name, duration_ms, negative ? "true" : "false");
            } else {
                ts_console_printf("%s (GPIO%d) %s脉冲 %d ms 完成\n", pin_name, pin, negative ? "负" : "正", duration_ms);
            }
        }
    } else if (strcmp(action, "toggle") == 0) {
        // 切换电平
        int old_level = gpio_get_level(pin);
        ret = gpio_toggle(pin);
        if (ret == ESP_OK) {
            int new_level = gpio_get_level(pin);
            if (json) {
                ts_console_printf("{\"gpio\":%d,\"name\":\"%s\",\"action\":\"toggle\",\"from\":%d,\"to\":%d}\n", pin, pin_name, old_level, new_level);
            } else {
                ts_console_printf("%s (GPIO%d): %s → %s\n", pin_name, pin, 
                       old_level ? "HIGH" : "LOW", new_level ? "HIGH" : "LOW");
            }
        }
    } else if (strcmp(action, "input") == 0) {
        // 读取电平（不改变模式）
        int level = gpio_get_level(pin);
        if (json) {
            ts_console_printf("{\"gpio\":%d,\"name\":\"%s\",\"level\":%d}\n", pin, pin_name, level);
        } else {
            ts_console_printf("%s (GPIO%d) 当前电平: %s\n", pin_name, pin, level ? "HIGH" : "LOW");
        }
    } else if (strcmp(action, "reset") == 0) {
        // 重置到默认电平
        int default_level = pin_info->default_level;
        ret = gpio_set_output_level(pin, default_level);
        if (ret == ESP_OK) {
            if (json) {
                ts_console_printf("{\"gpio\":%d,\"name\":\"%s\",\"action\":\"reset\",\"level\":%d}\n", pin, pin_name, default_level);
            } else {
                ts_console_printf("%s (GPIO%d) 已重置为默认电平: %s\n", pin_name, pin, default_level ? "HIGH" : "LOW");
            }
        }
    } else {
        ts_console_printf("错误: 无效的操作 '%s'\n", action);
        ts_console_printf("可用操作: high, low, pulse, toggle, input, reset\n");
        return 1;
    }
    
    if (ret != ESP_OK) {
        ts_console_printf("错误: GPIO%d 操作失败: %s\n", pin, esp_err_to_name(ret));
        return 1;
    }
    
    return 0;
}

/* ============================================================================
 * 命令注册
 * ============================================================================ */

esp_err_t ts_cmd_gpio_register(void)
{
    // 初始化参数
    s_gpio_args.list = arg_lit0("l", "list", "列出可控引脚");
    s_gpio_args.info = arg_int0("i", "info", "<pin>", "显示引脚详情");
    s_gpio_args.pin = arg_int0(NULL, NULL, "<pin>", "GPIO 引脚号");
    s_gpio_args.action = arg_str0(NULL, NULL, "<action>", "操作: high|low|pulse|toggle|input|reset");
    s_gpio_args.duration = arg_int0(NULL, NULL, "<ms>", "持续时间（毫秒）");
    s_gpio_args.negative = arg_lit0("n", "negative", "负脉冲模式");
    s_gpio_args.no_restore = arg_lit0(NULL, "no-restore", "不恢复原电平");
    s_gpio_args.json = arg_lit0("j", "json", "JSON 格式输出");
    s_gpio_args.help = arg_lit0("h", "help", "显示帮助");
    s_gpio_args.end = arg_end(5);
    
    const esp_console_cmd_t cmd = {
        .command = "gpio",
        .help = "GPIO 直接控制命令 (gpio <pin> high|low|pulse|toggle|input|reset)",
        .hint = NULL,
        .func = &cmd_gpio_handler,
        .argtable = &s_gpio_args,
    };
    
    esp_err_t ret = esp_console_cmd_register(&cmd);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "GPIO command registered (high priority)");
    }
    
    return ret;
}
