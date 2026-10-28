/**
 * @file ts_cmd_led.c
 * @brief LED Console Commands (API Layer)
 * 
 * 实现 led 命令族（通过 ts_api 调用）：
 * - led --status         显示 LED 设备状态
 * - led --brightness     设置亮度
 * - led --clear          清除 LED
 * - led --list-effects   列出特效
 * - led --parse-color    解析颜色
 * - led --save           保存当前状态为开机配置
 * - led --image          显示图像文件
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 */

#include "ts_console.h"
#include "ts_api.h"
#include "ts_led.h"
#include "ts_led_effect.h"
#include "ts_led_preset.h"
#include "ts_led_image.h"
#include "ts_led_qrcode.h"
#include "ts_led_font.h"
#include "ts_led_text.h"
#include "ts_led_color_correction.h"
#include "ts_config_module.h"
#include "ts_log.h"
#include "argtable3/argtable3.h"
#include <string.h>
#include <stdlib.h>

#define TAG "cmd_led"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *status;
    struct arg_lit *brightness;
    struct arg_lit *clear;
    struct arg_lit *on;
    struct arg_lit *off;
    struct arg_lit *effect;
    struct arg_lit *stop_effect;
    struct arg_lit *list_effects;
    struct arg_lit *filter;
    struct arg_lit *stop_filter;
    struct arg_lit *list_filters;
    struct arg_str *filter_name;
    struct arg_lit *parse_color;
    struct arg_lit *save;
    struct arg_lit *clear_boot;
    struct arg_lit *show_boot;
    struct arg_lit *image;
    struct arg_lit *qrcode;
    struct arg_lit *draw_text;
    struct arg_lit *stop_text;        /**< 停止文本覆盖层 */
    struct arg_str *text;
    struct arg_str *text_file;
    struct arg_str *font;
    struct arg_str *align;
    struct arg_str *scroll;           /**< 滚动方向：left, right, up, down, none */
    struct arg_int *text_x;           /**< 文本起始 X 位置 */
    struct arg_int *text_y;           /**< 文本起始 Y 位置 */
    struct arg_lit *invert;           /**< 反色显示（与底层图像） */
    struct arg_lit *loop;             /**< 循环滚动 */
    struct arg_str *ecc;
    struct arg_str *qr_bg;            /**< QR码背景图路径 */
    struct arg_lit *test;
    struct arg_str *file;
    struct arg_str *device;
    struct arg_str *center;
    struct arg_int *value;
    struct arg_str *color_val;
    struct arg_str *effect_name;
    struct arg_int *speed;
    struct arg_int *intensity;        /**< 滞镜强度/密度参数 (0-100) */
    struct arg_int *scale;            /**< 滞镜缩放参数 (1-100) */
    struct arg_int *levels;           /**< 色阶等级 (2-16) */
    struct arg_int *saturation;       /**< 饱和度 (0-255) */
    struct arg_int *wavelength;       /**< 波长 (1-32) */
    struct arg_int *amplitude;        /**< 振幅 (1-16) */
    struct arg_int *direction;        /**< 方向 (0-3: 横/纵/对角) */
    struct arg_int *width;            /**< 宽度 (1-8) */
    struct arg_int *frequency;        /**< 频率 (0-100) */
    struct arg_int *density;          /**< 密度 (0-100) */
    struct arg_int *decay;            /**< 衰减 (0-255) */
    struct arg_int *amount;           /**< 程度/量 (-100 to +100) */
    /* Color Correction */
    struct arg_lit *cc;               /**< 显示色彩校正配置 */
    struct arg_lit *cc_set;           /**< 设置色彩校正 */
    struct arg_lit *cc_reset;         /**< 重置色彩校正 */
    struct arg_lit *cc_export;        /**< 导出到SD卡 */
    struct arg_lit *cc_import;        /**< 从SD卡导入 */
    struct arg_lit *cc_enabled;       /**< 启用/禁用全局色彩校正 */
    struct arg_dbl *cc_wp_r;          /**< 白点红色缩放 */
    struct arg_dbl *cc_wp_g;          /**< 白点绿色缩放 */
    struct arg_dbl *cc_wp_b;          /**< 白点蓝色缩放 */
    struct arg_dbl *cc_gamma;         /**< Gamma值 */
    struct arg_dbl *cc_brightness;    /**< 亮度因子 */
    struct arg_dbl *cc_saturation;    /**< 饱和度因子 */
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_led_args;

/* 当前加载的图像（用于持续显示） */
static ts_led_image_t s_current_image = NULL;

/*===========================================================================*/
/*                          Command: led --status                             */
/*===========================================================================*/

/* 设备名称映射：用户友好名 -> 内部名 */
static const char *resolve_device_name(const char *name)
{
    if (!name) return NULL;
    
    /* 支持简短别名 */
    if (strcmp(name, "touch") == 0) return "led_touch";
    if (strcmp(name, "board") == 0) return "led_board";
    if (strcmp(name, "matrix") == 0) return "led_matrix";
    
    /* 也支持完整名 */
    return name;
}

static int do_led_status(const char *device_name, bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        cJSON *params = NULL;
        if (device_name) {
            params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "device", device_name);
        }
        
        esp_err_t ret = ts_api_call("led.list", params, &result);
        if (params) cJSON_Delete(params);
        
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
    
    /* 格式化输出 - 使用内部设备名 */
    const char *device_names[] = {"led_touch", "led_board", "led_matrix"};
    const char *display_names[] = {"touch", "board", "matrix"};
    size_t num_devices = sizeof(device_names) / sizeof(device_names[0]);
    
    if (device_name) {
        // 单个设备状态
        const char *internal_name = resolve_device_name(device_name);
        ts_led_device_t dev = ts_led_device_get(internal_name);
        if (!dev) {
            ts_console_error("Device '%s' not found\n", device_name);
            return 1;
        }
        
        ts_console_printf("LED Device: %s\n", device_name);
        ts_console_printf("  Count:      %u\n", ts_led_device_get_count(dev));
        ts_console_printf("  Brightness: %u\n", ts_led_device_get_brightness(dev));
    } else {
        // 所有设备状态
        ts_console_printf("LED Devices:\n\n");
        ts_console_printf("%-12s  %6s  %10s\n", "NAME", "COUNT", "BRIGHTNESS");
        ts_console_printf("------------------------------------\n");
        
        bool found = false;
        for (size_t i = 0; i < num_devices; i++) {
            ts_led_device_t dev = ts_led_device_get(device_names[i]);
            if (dev) {
                ts_console_printf("%-12s  %6u  %10u\n", 
                    display_names[i],
                    ts_led_device_get_count(dev),
                    ts_led_device_get_brightness(dev));
                found = true;
            }
        }
        
        if (!found) {
            ts_console_printf("  (no devices initialized)\n");
        }
        ts_console_printf("\n");
    }
    
    return 0;
}

/*===========================================================================*/
/*                       Command: led --brightness                            */
/*===========================================================================*/

static int do_led_brightness(const char *device_name, int value, bool set)
{
    if (!device_name) {
        ts_console_error("--device required\n");
        return 1;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    if (set) {
        if (value < 0 || value > 255) {
            ts_console_error("Brightness must be 0-255\n");
            return 1;
        }
        
        esp_err_t ret = ts_led_device_set_brightness(dev, (uint8_t)value);
        if (ret != ESP_OK) {
            ts_console_error("Failed to set brightness\n");
            return 1;
        }
        ts_console_success("Brightness set to %d\n", value);
    } else {
        ts_console_printf("Brightness: %u\n", ts_led_device_get_brightness(dev));
    }
    
    return 0;
}

/*===========================================================================*/
/*                         Command: led --clear                               */
/*===========================================================================*/

static int do_led_clear(const char *device_name)
{
    if (!device_name) {
        ts_console_error("--device required\n");
        return 1;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    /* Stop any running effect/animation first */
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (layer) {
        ts_led_animation_stop(layer);
        ts_led_image_animate_stop(layer);
        /* Clear layer buffer too */
        ts_led_layer_clear(layer);
    }
    
    /* Free current image if any */
    if (s_current_image) {
        ts_led_image_free(s_current_image);
        s_current_image = NULL;
    }
    
    /* Clear image path record */
    ts_led_preset_clear_current_image(device_name);
    
    esp_err_t ret = ts_led_device_clear(dev);
    if (ret != ESP_OK) {
        ts_console_error("Failed to clear device\n");
        return 1;
    }
    
    ret = ts_led_device_refresh(dev);
    if (ret != ESP_OK) {
        ts_console_error("Failed to refresh device\n");
        return 1;
    }
    
    ts_console_success("Device '%s' cleared\n", device_name);
    return 0;
}

/*===========================================================================*/
/*                         Command: led --effect                              */
/*===========================================================================*/

/* 静态存储用于特效颜色参数（因为特效使用指针） */
static ts_led_rgb_t s_effect_color;
static bool s_effect_color_valid = false;

static int do_led_effect(const char *device_name, const char *effect_name, int speed, const char *color_str)
{
    if (!device_name) {
        ts_console_error("--device required\n");
        return 1;
    }
    
    if (!effect_name) {
        ts_console_error("--name required (e.g. rainbow, breathing, chase, fire, sparkle, solid)\n");
        return 1;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    // 获取内置动画
    const ts_led_animation_def_t *effect = ts_led_animation_get_builtin(effect_name);
    if (!effect) {
        ts_console_error("Effect '%s' not found\n", effect_name);
        ts_console_printf("Use 'led --list-effects' to see available effects\n");
        return 1;
    }
    
    // 获取设备的默认层（layer 0）
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_console_error("Failed to get layer\n");
        return 1;
    }
    
    // 解析颜色参数（用于传递给特效）
    if (color_str) {
        if (ts_led_parse_color(color_str, &s_effect_color) == ESP_OK) {
            s_effect_color_valid = true;
        } else {
            ts_console_error("Invalid color: %s\n", color_str);
            return 1;
        }
    } else {
        s_effect_color_valid = false;
    }
    
    // 创建修改后的动画（速度和/或颜色）
    ts_led_animation_def_t modified_effect = *effect;
    
    if (speed > 0) {
        // 速度 1-100 映射到帧间隔：速度1=200ms, 速度100=5ms
        uint32_t interval = 200 - (speed - 1) * 195 / 99;
        if (interval < 5) interval = 5;
        modified_effect.frame_interval_ms = interval;
    }
    
    // 如果有颜色参数，传递给特效
    if (s_effect_color_valid) {
        modified_effect.user_data = &s_effect_color;
    }
    
    // 启动动画
    esp_err_t ret = ts_led_animation_start(layer, &modified_effect);
    if (ret != ESP_OK) {
        ts_console_error("Failed to start effect: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    // 记录当前特效（供后续保存使用）
    ts_led_preset_set_current_effect(device_name, effect_name, speed > 0 ? (uint8_t)speed : 0);
    
    // 清除图像路径记录（特效和图像互斥）
    ts_led_preset_clear_current_image(device_name);
    
    // 记录颜色（如果有）
    if (s_effect_color_valid) {
        ts_led_preset_set_current_color(device_name, s_effect_color);
    } else {
        ts_led_preset_clear_current_color(device_name);
    }
    
    if (speed > 0 && color_str) {
        ts_console_success("Effect '%s' started on '%s' (speed=%d, color=%s)\n", effect_name, device_name, speed, color_str);
    } else if (speed > 0) {
        ts_console_success("Effect '%s' started on '%s' (speed=%d)\n", effect_name, device_name, speed);
    } else if (color_str) {
        ts_console_success("Effect '%s' started on '%s' (color=%s)\n", effect_name, device_name, color_str);
    } else {
        ts_console_success("Effect '%s' started on '%s'\n", effect_name, device_name);
    }
    return 0;
}

static int do_led_stop_effect(const char *device_name)
{
    if (!device_name) {
        ts_console_error("--device required\n");
        return 1;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_console_error("Failed to get layer\n");
        return 1;
    }
    
    esp_err_t ret = ts_led_animation_stop(layer);
    if (ret != ESP_OK) {
        ts_console_error("Failed to stop animation\n");
        return 1;
    }
    
    // 清除记录的特效
    ts_led_preset_set_current_effect(device_name, NULL, 0);
    
    ts_console_success("Effect stopped on '%s'\n", device_name);
    return 0;
}

/*===========================================================================*/
/*                      Command: led --filter (Post-Processing)               */
/*===========================================================================*/

/**
 * @brief 后处理效果类型名称映射
 */
static const struct {
    const char *name;
    ts_led_effect_type_t type;
    const char *description;
} s_filter_types[] = {
    {"none",        TS_LED_EFFECT_NONE,        "No effect"},
    {"brightness",  TS_LED_EFFECT_BRIGHTNESS,  "Static brightness adjustment"},
    {"pulse",       TS_LED_EFFECT_PULSE,       "Pulsing brightness (sine wave)"},
    {"blink",       TS_LED_EFFECT_BLINK,       "On/off blinking"},
    {"fade-in",     TS_LED_EFFECT_FADE_IN,     "Fade in (one-shot)"},
    {"fade-out",    TS_LED_EFFECT_FADE_OUT,    "Fade out (one-shot)"},
    {"breathing",   TS_LED_EFFECT_BREATHING,   "Smooth breathing effect"},
    {"color-shift", TS_LED_EFFECT_COLOR_SHIFT, "Hue rotation over time"},
    {"saturation",  TS_LED_EFFECT_SATURATION,  "Saturation adjustment"},
    {"invert",      TS_LED_EFFECT_INVERT,      "Invert colors"},
    {"grayscale",   TS_LED_EFFECT_GRAYSCALE,   "Convert to grayscale"},
    {"scanline",    TS_LED_EFFECT_SCANLINE,    "Horizontal/vertical scanline"},
    {"wave",        TS_LED_EFFECT_WAVE,        "Brightness wave"},
    {"glitch",      TS_LED_EFFECT_GLITCH,      "Random glitch artifacts"},
    {"rainbow",     TS_LED_EFFECT_RAINBOW,     "Rainbow color shift"},
    {"sparkle",     TS_LED_EFFECT_SPARKLE,     "Random sparkles"},
    {"plasma",      TS_LED_EFFECT_PLASMA,      "Plasma wave effect"},
    {"sepia",       TS_LED_EFFECT_SEPIA,       "Sepia tone (vintage)"},
    {"posterize",   TS_LED_EFFECT_POSTERIZE,   "Posterization (color levels)"},
    {"contrast",    TS_LED_EFFECT_CONTRAST,    "Contrast adjustment"},
    {NULL, 0, NULL}
};

static ts_led_effect_type_t filter_name_to_type(const char *name)
{
    if (!name) return TS_LED_EFFECT_NONE;
    for (int i = 0; s_filter_types[i].name; i++) {
        if (strcmp(s_filter_types[i].name, name) == 0) {
            return s_filter_types[i].type;
        }
    }
    return TS_LED_EFFECT_NONE;
}

static int do_led_filter(const char *device_name, const char *filter_name, int speed)
{
    if (!device_name) {
        ts_console_error("--device required\n");
        return 1;
    }
    
    if (!filter_name) {
        ts_console_error("--filter-name required (e.g. pulse, blink, breathing, fade-in)\n");
        return 1;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    ts_led_effect_type_t type = filter_name_to_type(filter_name);
    if (type == TS_LED_EFFECT_NONE && strcmp(filter_name, "none") != 0) {
        ts_console_error("Filter '%s' not found\n", filter_name);
        ts_console_printf("Use 'led --list-filters' to see available filters\n");
        return 1;
    }
    
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_console_error("Failed to get layer\n");
        return 1;
    }
    
    // \u83b7\u53d6\u53c2\u6570
    int intensity = s_led_args.intensity->count > 0 ? s_led_args.intensity->ival[0] : -1;
    int scale = s_led_args.scale->count > 0 ? s_led_args.scale->ival[0] : -1;
    int levels = s_led_args.levels->count > 0 ? s_led_args.levels->ival[0] : -1;
    int saturation = s_led_args.saturation->count > 0 ? s_led_args.saturation->ival[0] : -1;
    int wavelength = s_led_args.wavelength->count > 0 ? s_led_args.wavelength->ival[0] : -1;
    int amplitude = s_led_args.amplitude->count > 0 ? s_led_args.amplitude->ival[0] : -1;
    int direction = s_led_args.direction->count > 0 ? s_led_args.direction->ival[0] : -1;
    int width = s_led_args.width->count > 0 ? s_led_args.width->ival[0] : -1;
    int frequency = s_led_args.frequency->count > 0 ? s_led_args.frequency->ival[0] : -1;
    int density = s_led_args.density->count > 0 ? s_led_args.density->ival[0] : -1;
    int decay = s_led_args.decay->count > 0 ? s_led_args.decay->ival[0] : -1;
    int amount = s_led_args.amount->count > 0 ? s_led_args.amount->ival[0] : -1;
    
    // \u914d\u7f6e\u540e\u5904\u7406\u6548\u679c
    ts_led_effect_config_t config = {
        .type = type,
        .params = {
            .brightness = {.level = 255}
        }
    };
    
    // 根据速度参数调整效果参数
    if (speed > 0) {
        // 速度 1-100 映射到频率：速度1=0.2Hz, 速度100=5Hz
        float freq = 0.2f + (speed - 1) * 4.8f / 99.0f;
        
        switch (type) {
            case TS_LED_EFFECT_PULSE:
                config.params.pulse.frequency = freq;
                config.params.pulse.min_level = 20;
                config.params.pulse.max_level = 255;
                break;
            case TS_LED_EFFECT_BLINK: {
                uint16_t period_ms = (uint16_t)(1000.0f / freq);
                config.params.blink.on_time_ms = period_ms / 2;
                config.params.blink.off_time_ms = period_ms / 2;
                break;
            }
            case TS_LED_EFFECT_BREATHING:
                config.params.breathing.frequency = freq;
                config.params.breathing.min_level = 10;
                config.params.breathing.max_level = 255;
                break;
            case TS_LED_EFFECT_FADE_IN:
            case TS_LED_EFFECT_FADE_OUT:
                config.params.fade.duration_ms = (uint16_t)(1000.0f / freq);
                config.params.fade.auto_remove = false;
                break;
            case TS_LED_EFFECT_COLOR_SHIFT:
                config.params.color_shift.speed = speed * 3.6f;  // 度/秒
                config.params.color_shift.static_shift = 0;
                break;
            case TS_LED_EFFECT_SCANLINE:
                config.params.scanline.speed = (float)speed;
                config.params.scanline.width = width > 0 ? width : 3;
                config.params.scanline.angle = direction >= 0 && direction <= 360 ? (float)direction : 0.0f;
                config.params.scanline.intensity = intensity > 0 ? intensity * 2.55 : 200;
                break;
            case TS_LED_EFFECT_WAVE:
                config.params.wave.speed = (float)speed;
                config.params.wave.wavelength = wavelength > 0 ? (float)wavelength : 8.0f;
                config.params.wave.amplitude = amplitude > 0 ? amplitude * 16 : 128;
                config.params.wave.angle = direction >= 0 && direction <= 360 ? (float)direction : 0.0f; // 0-360°
                break;
            case TS_LED_EFFECT_GLITCH:
                config.params.glitch.intensity = intensity > 0 ? intensity : speed;
                config.params.glitch.frequency = frequency > 0 ? frequency * 2.55 : 10;
                break;
            case TS_LED_EFFECT_RAINBOW:
                config.params.rainbow.speed = (float)speed;
                config.params.rainbow.saturation = saturation > 0 ? saturation : 255;
                break;
            case TS_LED_EFFECT_SPARKLE:
                config.params.sparkle.speed = speed > 0 ? (float)speed * 0.2f : 10.0f;  // CLI速度映射：1-100 → 0.2-20
                config.params.sparkle.density = density > 0 ? density * 2.55 : (intensity > 0 ? intensity * 2.55 : 127);
                config.params.sparkle.decay = decay > 0 ? decay : 150;  // 提高decay让余晖更明显
                break;
            case TS_LED_EFFECT_PLASMA:
                config.params.plasma.speed = speed / 10.0f;
                config.params.plasma.scale = scale > 0 ? scale : 20;
                break;
            case TS_LED_EFFECT_POSTERIZE:
                config.params.posterize.levels = levels > 0 ? levels : (2 + speed * 14 / 100);
                break;
            case TS_LED_EFFECT_CONTRAST:
                config.params.contrast.amount = amount > -101 ? amount : (intensity > 0 ? (intensity - 50) * 2 : (speed - 50) * 2);
                break;
            default:
                break;
        }
    } else {
        // 使用默认参数
        switch (type) {
            case TS_LED_EFFECT_PULSE:
                config.params.pulse.frequency = 0.5f;
                config.params.pulse.min_level = 20;
                config.params.pulse.max_level = 255;
                break;
            case TS_LED_EFFECT_BLINK:
                config.params.blink.on_time_ms = 500;
                config.params.blink.off_time_ms = 500;
                break;
            case TS_LED_EFFECT_BREATHING:
                config.params.breathing.frequency = 0.3f;
                config.params.breathing.min_level = 10;
                config.params.breathing.max_level = 255;
                break;
            case TS_LED_EFFECT_FADE_IN:
            case TS_LED_EFFECT_FADE_OUT:
                config.params.fade.duration_ms = 1000;
                config.params.fade.auto_remove = false;
                break;
            case TS_LED_EFFECT_COLOR_SHIFT:
                config.params.color_shift.speed = 90.0f;
                config.params.color_shift.static_shift = 0;
                break;
            case TS_LED_EFFECT_SCANLINE:
                config.params.scanline.speed = 50.0f;
                config.params.scanline.width = 3;
                config.params.scanline.angle = 0.0f;  // 0° = 水平向右
                config.params.scanline.intensity = 200;
                break;
            case TS_LED_EFFECT_WAVE:
                config.params.wave.speed = 50.0f;
                config.params.wave.wavelength = 8.0f;
                config.params.wave.amplitude = 128;
                config.params.wave.angle = 0.0f;  // 0° = 水平向右
                break;
            case TS_LED_EFFECT_GLITCH:
                config.params.glitch.intensity = 50;
                config.params.glitch.frequency = 10;
                break;
            default:
                break;
        }
    }
    
    esp_err_t ret = ts_led_layer_set_effect(layer, &config);
    if (ret != ESP_OK) {
        ts_console_error("Failed to apply filter: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    // 记录当前 filter（供保存用）
    ts_led_preset_set_current_filter(internal_name, filter_name, speed > 0 ? speed : 50);
    
    if (speed > 0) {
        ts_console_success("Filter '%s' applied on '%s' (speed=%d)\n", filter_name, device_name, speed);
    } else {
        ts_console_success("Filter '%s' applied on '%s'\n", filter_name, device_name);
    }
    return 0;
}

static int do_led_stop_filter(const char *device_name)
{
    if (!device_name) {
        ts_console_error("--device required\n");
        return 1;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_console_error("Failed to get layer\n");
        return 1;
    }
    
    esp_err_t ret = ts_led_layer_clear_effect(layer);
    if (ret != ESP_OK) {
        ts_console_error("Failed to clear filter\n");
        return 1;
    }
    
    // 清除 filter 记录
    ts_led_preset_set_current_filter(internal_name, NULL, 0);
    
    ts_console_success("Filter cleared on '%s'\n", device_name);
    return 0;
}

static int do_led_list_filters(bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("led.filter.list", NULL, &result);
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
    ts_console_printf("\n╭─ Post-Processing Filters ───────────────────────────────╮\n");
    for (int i = 0; s_filter_types[i].name; i++) {
        ts_console_printf("│ %-14s  %-40s │\n", 
                          s_filter_types[i].name, 
                          s_filter_types[i].description);
    }
    ts_console_printf("╰──────────────────────────────────────────────────────────╯\n");
    ts_console_printf("\nUsage: led --filter -d <device> --filter-name <name> [--speed <1-100>]\n");
    ts_console_printf("       led --stop-filter -d <device>\n");
    return 0;
}

/*===========================================================================*/
/*                         Command: led --on/--off                            */
/*===========================================================================*/

static int do_led_on(const char *device_name, const char *color_str)
{
    if (!device_name) {
        ts_console_error("--device required\n");
        return 1;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    // 解析颜色，默认白色
    ts_led_rgb_t color = {255, 255, 255};  // 默认白色
    if (color_str) {
        if (ts_led_parse_color(color_str, &color) != ESP_OK) {
            ts_console_error("Invalid color: %s\n", color_str);
            return 1;
        }
    }
    
    // 直接填充设备 framebuffer（绕过 layer 系统）
    esp_err_t ret = ts_led_device_fill(dev, color);
    if (ret != ESP_OK) {
        ts_console_error("Failed to fill color\n");
        return 1;
    }
    
    // 立即刷新到硬件
    ret = ts_led_device_refresh(dev);
    if (ret != ESP_OK) {
        ts_console_error("Failed to refresh LED\n");
        return 1;
    }
    
    ts_console_success("LED '%s' on: #%02X%02X%02X\n", 
        device_name, color.r, color.g, color.b);
    return 0;
}

static int do_led_off(const char *device_name)
{
    return do_led_clear(device_name);
}

/*===========================================================================*/
/*                        Command: led --list-effects                         */
/*===========================================================================*/

static int do_led_list_effects(const char *device_name, bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        cJSON *params = NULL;
        if (device_name) {
            params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "device", device_name);
        }
        
        esp_err_t ret = ts_api_call("led.effect.list", params, &result);
        if (params) cJSON_Delete(params);
        
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
    const char *names[32];
    
    if (device_name) {
        // 按设备类型列出适用的特效
        ts_led_layout_t layout;
        
        if (strcmp(device_name, "touch") == 0) {
            layout = TS_LED_LAYOUT_STRIP;  // 点光源视为 strip
        } else if (strcmp(device_name, "board") == 0) {
            layout = TS_LED_LAYOUT_RING;
        } else if (strcmp(device_name, "matrix") == 0) {
            layout = TS_LED_LAYOUT_MATRIX;
        } else {
            ts_console_error("Unknown device: %s\n", device_name);
            return 1;
        }
        
        size_t count = ts_led_animation_list_for_device(layout, names, 32);
        
        ts_console_printf("Effects for '%s':\n", device_name);
        for (size_t i = 0; i < count; i++) {
            ts_console_printf("  - %s\n", names[i]);
        }
    } else {
        // 分类列出所有特效
        ts_console_printf("Available Effects by Device Type:\n\n");
        
        // Touch 特效 (点光源)
        ts_console_printf("Touch (point light, 1 LED):\n");
        size_t count = ts_led_animation_list_for_device(TS_LED_LAYOUT_STRIP, names, 32);
        for (size_t i = 0; i < count; i++) {
            ts_console_printf("  - %s\n", names[i]);
        }
        
        // Board 特效 (环形)
        ts_console_printf("\nBoard (ring, 28 LEDs):\n");
        count = ts_led_animation_list_for_device(TS_LED_LAYOUT_RING, names, 32);
        for (size_t i = 0; i < count; i++) {
            ts_console_printf("  - %s\n", names[i]);
        }
        
        // Matrix 特效 (矩阵)
        ts_console_printf("\nMatrix (32x32 panel):\n");
        count = ts_led_animation_list_for_device(TS_LED_LAYOUT_MATRIX, names, 32);
        for (size_t i = 0; i < count; i++) {
            ts_console_printf("  - %s\n", names[i]);
        }
        ts_console_printf("\n");
    }
    
    return 0;
}

/*===========================================================================*/
/*                        Command: led --parse-color                          */
/*===========================================================================*/

static int do_led_parse_color(const char *color_str, bool json)
{
    if (!color_str) {
        ts_console_error("--color required\n");
        return 1;
    }
    
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "color", color_str);
        
        esp_err_t ret = ts_api_call("led.color.parse", params, &result);
        cJSON_Delete(params);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"error\":\"%s\"}\n", result.message ? result.message : "Invalid color");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    ts_led_rgb_t color;
    if (ts_led_parse_color(color_str, &color) != ESP_OK) {
        ts_console_error("Invalid color: %s\n", color_str);
        ts_console_printf("Use format: #RRGGBB or color name (red, green, blue, etc.)\n");
        return 1;
    }
    
    // 转换为 HSV
    ts_led_hsv_t hsv = ts_led_rgb_to_hsv(color);
    
    ts_console_printf("Color: %s\n", color_str);
    ts_console_printf("  RGB: (%3u, %3u, %3u)\n", color.r, color.g, color.b);
    ts_console_printf("  Hex: #%02X%02X%02X\n", color.r, color.g, color.b);
    ts_console_printf("  HSV: (%3u, %3u, %3u)\n", hsv.h, hsv.s, hsv.v);
    
    return 0;
}

/*===========================================================================*/
/*                       Command: led --save/--show-boot                      */
/*===========================================================================*/

static int do_led_save(const char *device_name)
{
    esp_err_t ret;
    
    ts_console_printf("Saving LED configuration...\n");
    
    /* 调用原有保存方法 */
    if (device_name) {
        ret = ts_led_save_boot_config(device_name);
        if (ret != ESP_OK) {
            ts_console_error("Failed to save boot config: %s\n", esp_err_to_name(ret));
            return 1;
        }
    } else {
        ret = ts_led_save_all_boot_config();
        if (ret != ESP_OK) {
            ts_console_error("Failed to save boot config\n");
            return 1;
        }
    }
    
    /* 同时保存模块化的系统配置 */
    ret = ts_config_module_persist(TS_CONFIG_MODULE_LED);
    if (ret == ESP_OK) {
        ts_console_success("Configuration saved to NVS");
        ts_console_printf("\n");
    } else {
        ts_console_success("Boot config saved for %s\n", 
            device_name ? device_name : "all LED devices");
    }
    
    return 0;
}

static int do_led_clear_boot(const char *device_name)
{
    esp_err_t ret = ts_led_clear_boot_config(device_name);
    if (ret != ESP_OK) {
        ts_console_error("Failed to clear boot config\n");
        return 1;
    }
    
    if (device_name) {
        ts_console_success("Boot config cleared for '%s'\n", device_name);
    } else {
        ts_console_success("Boot config cleared for all devices\n");
    }
    return 0;
}

static int do_led_show_boot(const char *device_name, bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        cJSON *params = NULL;
        if (device_name) {
            params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "device", device_name);
        }
        
        esp_err_t ret = ts_api_call("led.boot.config", params, &result);
        if (params) cJSON_Delete(params);
        
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
    const char *devices[] = {"touch", "board", "matrix"};
    int start = 0, end = 3;
    
    if (device_name) {
        // 单个设备
        for (int i = 0; i < 3; i++) {
            if (strcmp(device_name, devices[i]) == 0) {
                start = i;
                end = i + 1;
                break;
            }
        }
    }
    
    ts_console_printf("Boot Configuration:\n\n");
    ts_console_printf("%-10s  %-8s  %-15s  %-12s  %-25s  %6s  %10s\n", 
        "DEVICE", "ENABLED", "ANIMATION", "FILTER", "IMAGE", "SPEED", "BRIGHTNESS");
    ts_console_printf("------------------------------------------------------------------------------------------------------\n");
    
    for (int i = start; i < end; i++) {
        ts_led_boot_config_t cfg;
        if (ts_led_get_boot_config(devices[i], &cfg) == ESP_OK && cfg.enabled) {
            ts_console_printf("%-10s  %-8s  %-15s  %-12s  %-25s  %6d  %10d\n",
                devices[i],
                cfg.enabled ? "yes" : "no",
                cfg.animation[0] ? cfg.animation : "(none)",
                cfg.filter[0] ? cfg.filter : "(none)",
                cfg.image_path[0] ? cfg.image_path : "(none)",
                cfg.speed,
                cfg.brightness);
        } else {
            ts_console_printf("%-10s  %-8s  %-15s  %-12s  %-25s  %6s  %10s\n",
                devices[i], "no", "-", "-", "-", "-", "-");
        }
    }
    ts_console_printf("\n");
    
    return 0;
}

/*===========================================================================*/
/*                         Command: led --test                                */
/*===========================================================================*/

static int do_led_test(const char *device_name, int mode)
{
    if (!device_name) {
        device_name = "matrix";
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_console_error("Failed to get layer\n");
        return 1;
    }
    
    // 停止当前动画
    ts_led_animation_stop(layer);
    
    // 获取设备信息（假设矩阵32x32）
    uint16_t w = 32, h = 32;
    uint16_t count = ts_led_device_get_count(dev);
    if (count == 1024) { w = h = 32; }
    else if (count == 28) { w = 28; h = 1; }
    else { w = count; h = 1; }
    
    ts_console_printf("Testing %s (%ux%u, %u LEDs), mode=%d\n", 
                      device_name, w, h, count, mode);
    
    // 测试模式：
    // mode 0: 渐变（左上角红，右下角蓝）- 检查整体方向
    // mode 1: 前3行不同颜色 - 检查行方向
    // mode 2: 前3列不同颜色 - 检查列方向
    // mode 3: 左上角4x4红块 - 检查原点
    
    for (uint16_t y = 0; y < h; y++) {
        for (uint16_t x = 0; x < w; x++) {
            ts_led_rgb_t color = {0, 0, 0};
            
            switch (mode) {
                case 0: // 渐变
                    color.r = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
                    color.g = 0;
                    color.b = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
                    break;
                    
                case 1: // 行测试：前3行不同颜色
                    if (y == 0) color = (ts_led_rgb_t){255, 0, 0};       // 第0行=红
                    else if (y == 1) color = (ts_led_rgb_t){0, 255, 0};  // 第1行=绿
                    else if (y == 2) color = (ts_led_rgb_t){0, 0, 255};  // 第2行=蓝
                    break;
                    
                case 2: // 列测试：前3列不同颜色
                    if (x == 0) color = (ts_led_rgb_t){255, 0, 0};       // 第0列=红
                    else if (x == 1) color = (ts_led_rgb_t){0, 255, 0};  // 第1列=绿
                    else if (x == 2) color = (ts_led_rgb_t){0, 0, 255};  // 第2列=蓝
                    break;
                    
                case 3: // 原点测试：左上角4x4红块
                    if (x < 4 && y < 4) color = (ts_led_rgb_t){255, 0, 0};
                    break;
                    
                default:
                    color.r = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
                    color.b = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
                    break;
            }
            
            ts_led_set_pixel_xy(layer, x, y, color);
        }
    }
    
    ts_led_device_refresh(dev);
    
    const char *mode_desc[] = {
        "渐变: 左上红->右上粉->左下紫->右下蓝",
        "行测试: 顶部3行=红/绿/蓝",
        "列测试: 左侧3列=红/绿/蓝", 
        "原点: 左上角4x4红块"
    };
    ts_console_success("Test pattern %d: %s\n", mode, mode_desc[mode % 4]);
    
    return 0;
}

/*===========================================================================*/
/*                         Command: led --image                               */
/*===========================================================================*/

static int do_led_image(const char *device_name, const char *file_path, const char *center_mode)
{
    if (!device_name) {
        ts_console_error("--device required (only matrix supported)\n");
        return 1;
    }
    
    if (!file_path) {
        ts_console_error("--file required\n");
        return 1;
    }
    
    // 只支持 matrix 设备
    if (strcmp(device_name, "matrix") != 0 && strcmp(device_name, "led_matrix") != 0) {
        ts_console_error("Image display only supported on matrix device\n");
        return 1;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    // 获取 layer 并停止当前动画/特效（必须在释放图像前！）
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (layer) {
        ts_led_image_animate_stop(layer);
        ts_led_animation_stop(layer);
    }
    
    // 释放之前的图像（现在安全了，动画已停止）
    if (s_current_image) {
        ts_led_image_free(s_current_image);
        s_current_image = NULL;
    }
    
    // 加载图像
    ts_console_printf("Loading image: %s\n", file_path);
    esp_err_t ret = ts_led_image_load(file_path, TS_LED_IMG_FMT_AUTO, &s_current_image);
    if (ret != ESP_OK) {
        ts_console_error("Failed to load image: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    // 获取图像信息
    ts_led_image_info_t info;
    ts_led_image_get_info(s_current_image, &info);
    ts_console_printf("Image: %ux%u, %d frame(s)\n", info.width, info.height, info.frame_count);
    
    // 显示图像
    ts_led_image_options_t opts = TS_LED_IMAGE_DEFAULT_OPTIONS();
    opts.scale = TS_LED_IMG_SCALE_FIT;
    
    // 解析居中模式
    if (center_mode) {
        if (strcmp(center_mode, "image") == 0 || strcmp(center_mode, "img") == 0) {
            opts.center = TS_LED_IMG_CENTER_IMAGE;
            ts_console_printf("Center mode: image\n");
        } else if (strcmp(center_mode, "content") == 0 || strcmp(center_mode, "auto") == 0) {
            opts.center = TS_LED_IMG_CENTER_CONTENT;
            ts_console_printf("Center mode: content\n");
        } else {
            ts_console_error("Unknown center mode: %s (use 'image' or 'content')\n", center_mode);
            return 1;
        }
    }
    
    if (info.frame_count > 1) {
        // GIF 动画
        ret = ts_led_image_animate_start(layer, s_current_image, &opts);
        if (ret != ESP_OK) {
            ts_console_error("Failed to start animation: %s\n", esp_err_to_name(ret));
            return 1;
        }
        ts_console_success("Animation started on '%s'\n", device_name);
    } else {
        // 静态图像
        ret = ts_led_image_display(layer, s_current_image, &opts);
        if (ret != ESP_OK) {
            ts_console_error("Failed to display image: %s\n", esp_err_to_name(ret));
            return 1;
        }
        ts_console_success("Image displayed on '%s'\n", device_name);
    }
    
    // 记录当前图像路径（供 --save 使用）
    ts_led_preset_set_current_image(device_name, file_path);
    
    return 0;
}

/*===========================================================================*/
/*                        Command: led --draw-text                            */
/*===========================================================================*/

/* 当前加载的字体（避免重复加载） */
static ts_font_t *s_current_font = NULL;
static char s_current_font_name[32] = {0};

/**
 * @brief 解析对齐方式字符串
 */
static ts_text_align_t parse_text_align(const char *align_str)
{
    if (!align_str) return TS_TEXT_ALIGN_LEFT;
    
    if (strcmp(align_str, "left") == 0 || strcmp(align_str, "l") == 0) {
        return TS_TEXT_ALIGN_LEFT;
    }
    if (strcmp(align_str, "center") == 0 || strcmp(align_str, "c") == 0) {
        return TS_TEXT_ALIGN_CENTER;
    }
    if (strcmp(align_str, "right") == 0 || strcmp(align_str, "r") == 0) {
        return TS_TEXT_ALIGN_RIGHT;
    }
    
    return TS_TEXT_ALIGN_LEFT;
}

/**
 * @brief 从原始命令行提取简单参数值（非 UTF-8 敏感）
 * 
 * @param cmdline 原始命令行
 * @param param_name 参数名（如 "--font"）
 * @param output 输出缓冲区
 * @param output_size 缓冲区大小
 * @return true 如果找到并提取成功
 */
static bool extract_param_from_cmdline(const char *cmdline, const char *param_name,
                                       char *output, size_t output_size)
{
    if (!cmdline || !param_name || !output || output_size == 0) return false;
    
    const char *p = strstr(cmdline, param_name);
    if (!p) return false;
    
    p += strlen(param_name);
    
    // 跳过空格
    while (*p == ' ' || *p == '\t') p++;
    
    // 跳过可选的引号
    bool quoted = (*p == '"');
    if (quoted) p++;
    
    const char *end = p;
    if (quoted) {
        while (*end && *end != '"') end++;
    } else {
        while (*end && *end != ' ' && *end != '\t') {
            if (*end == '-' && *(end + 1) == '-') break;
            end++;
        }
    }
    
    size_t len = end - p;
    if (len >= output_size) len = output_size - 1;
    
    memcpy(output, p, len);
    output[len] = '\0';
    return len > 0;
}

/**
 * @brief 从原始命令行提取 --text 参数值（支持 UTF-8 中文）
 * 
 * 绕过 argtable3 解析问题，直接从原始命令行提取带引号的文本。
 * 
 * @param cmdline 原始命令行
 * @param output 输出缓冲区
 * @param output_size 缓冲区大小
 * @return true 如果找到并提取成功
 */
static bool extract_text_from_cmdline(const char *cmdline, char *output, size_t output_size)
{
    if (!cmdline || !output || output_size == 0) return false;
    
    // 查找 --text 
    const char *p = strstr(cmdline, "--text");
    if (!p) return false;
    
    // 确保不是 --text-file
    if (strncmp(p, "--text-file", 11) == 0) {
        // 继续查找下一个 --text
        p = strstr(p + 11, "--text");
        if (!p) return false;
        // 再检查一次
        if (strncmp(p, "--text-file", 11) == 0) return false;
    }
    
    p += 6;  // 跳过 "--text"
    
    // 跳过空格
    while (*p == ' ' || *p == '\t') p++;
    
    // 检查是否是引号开始
    if (*p == '"') {
        p++;  // 跳过开始引号
        const char *end = p;
        
        // 查找结束引号（处理转义）
        while (*end && *end != '"') {
            if (*end == '\\' && *(end + 1)) {
                end += 2;  // 跳过转义字符
            } else {
                end++;
            }
        }
        
        size_t len = end - p;
        if (len >= output_size) len = output_size - 1;
        
        memcpy(output, p, len);
        output[len] = '\0';
        return len > 0;
    } else {
        // 无引号，读取到下一个空格或参数
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t') {
            // 停止于下一个 -- 参数
            if (*end == '-' && *(end + 1) == '-') break;
            end++;
        }
        
        size_t len = end - p;
        if (len >= output_size) len = output_size - 1;
        
        memcpy(output, p, len);
        output[len] = '\0';
        return len > 0;
    }
}

/**
 * @brief 解析文本中的转义序列
 * 
 * 支持 \uXXXX (Unicode) 和 \xHH (十六进制字节) 转义
 * 用于在控制台输入中文等 UTF-8 字符
 * 
 * 例如：
 *   "\\u4f60\\u597d" -> "你好"
 *   "Hello\\u0021"   -> "Hello!"
 * 
 * @param input 输入字符串
 * @param output 输出缓冲区
 * @param output_size 缓冲区大小
 * @return 实际输出长度
 */
static size_t parse_escape_sequences(const char *input, char *output, size_t output_size)
{
    if (!input || !output || output_size == 0) return 0;
    
    const char *p = input;
    char *out = output;
    char *out_end = output + output_size - 1;
    
    while (*p && out < out_end) {
        if (*p == '\\' && *(p + 1)) {
            char next = *(p + 1);
            
            // \uXXXX - Unicode codepoint
            if (next == 'u' && p[2] && p[3] && p[4] && p[5]) {
                char hex[5] = {p[2], p[3], p[4], p[5], '\0'};
                char *endptr;
                unsigned long cp = strtoul(hex, &endptr, 16);
                
                if (endptr == hex + 4 && cp > 0 && cp <= 0xFFFF) {
                    // Encode as UTF-8
                    if (cp < 0x80) {
                        if (out < out_end) *out++ = (char)cp;
                    } else if (cp < 0x800) {
                        if (out + 1 < out_end) {
                            *out++ = 0xC0 | (cp >> 6);
                            *out++ = 0x80 | (cp & 0x3F);
                        }
                    } else {
                        if (out + 2 < out_end) {
                            *out++ = 0xE0 | (cp >> 12);
                            *out++ = 0x80 | ((cp >> 6) & 0x3F);
                            *out++ = 0x80 | (cp & 0x3F);
                        }
                    }
                    p += 6;
                    continue;
                }
            }
            // \xHH - Hex byte
            else if (next == 'x' && p[2] && p[3]) {
                char hex[3] = {p[2], p[3], '\0'};
                char *endptr;
                unsigned long byte = strtoul(hex, &endptr, 16);
                
                if (endptr == hex + 2) {
                    if (out < out_end) *out++ = (char)byte;
                    p += 4;
                    continue;
                }
            }
            /* Special escape sequences: \n, \t, \\ */
            else if (next == 'n') {
                if (out < out_end) *out++ = '\n';
                p += 2;
                continue;
            }
            else if (next == 't') {
                if (out < out_end) *out++ = '\t';
                p += 2;
                continue;
            }
            else if (next == '\\') {
                if (out < out_end) *out++ = '\\';
                p += 2;
                continue;
            }
        }
        
        /* Normal character (copy as-is, including raw UTF-8) */
        *out++ = *p++;
    }
    
    *out = '\0';
    return out - output;
}

/**
 * @brief 构建字体文件路径
 * @param font_name 字体名（如 "boutique9x9"）
 * @param path_buf 输出路径缓冲区
 * @param buf_size 缓冲区大小
 * @return true 成功，false 失败
 */
static bool build_font_path(const char *font_name, char *path_buf, size_t buf_size)
{
    if (!font_name || !path_buf) return false;
    
    // 如果已经是完整路径，直接使用
    if (font_name[0] == '/') {
        strncpy(path_buf, font_name, buf_size - 1);
        path_buf[buf_size - 1] = '\0';
        return true;
    }
    
    // 否则在 /sdcard/fonts/ 目录下查找
    int n = snprintf(path_buf, buf_size, "/sdcard/fonts/%s.fnt", font_name);
    return n > 0 && (size_t)n < buf_size;
}

static int do_led_draw_text(const char *device_name, const char *text, 
                            const char *font_name, const char *color_str,
                            const char *align_str, const char *text_file,
                            const char *scroll_dir_str, int start_x, int start_y,
                            bool invert_overlap, bool loop_scroll, int scroll_speed)
{
    if (!device_name) {
        device_name = "matrix";  // 默认使用 matrix 设备
    }
    
    // 尝试从原始命令行提取 --text 参数（绕过 argtable3 的 UTF-8 问题）
    char raw_text[256] = {0};
    const char *cmdline = ts_console_get_raw_cmdline();
    if (cmdline && extract_text_from_cmdline(cmdline, raw_text, sizeof(raw_text))) {
        TS_LOGI(TAG, "Extracted text from raw cmdline: %s", raw_text);
        text = raw_text;  // 使用从原始命令行提取的文本
        
        // 同时尝试从原始命令行提取其他参数（因为 argtable3 可能解析失败）
        static char raw_font[32], raw_color[32], raw_align[16];
        if (!font_name && extract_param_from_cmdline(cmdline, "--font", raw_font, sizeof(raw_font))) {
            font_name = raw_font;
            TS_LOGI(TAG, "Extracted font from raw cmdline: %s", font_name);
        }
        if (!color_str && extract_param_from_cmdline(cmdline, "--color", raw_color, sizeof(raw_color))) {
            color_str = raw_color;
        }
        if (!align_str && extract_param_from_cmdline(cmdline, "--align", raw_align, sizeof(raw_align))) {
            align_str = raw_align;
        }
    }
    
    // 调试：显示 text 的原始字节
    if (text) {
        char hex_buf[256] = {0};
        int pos = 0;
        for (size_t i = 0; i < strlen(text) && pos < 250; i++) {
            pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", (uint8_t)text[i]);
        }
        TS_LOGI(TAG, "text bytes: %s", hex_buf);
    }
    
    // 文本内容：优先使用文件，其次使用 --text 参数
    char text_buf[256] = {0};
    const char *display_text = text;
    
    if (text_file && strlen(text_file) > 0) {
        // 从文件读取文本（支持中文）
        FILE *f = fopen(text_file, "r");
        if (!f) {
            ts_console_error("Cannot open text file: %s\n", text_file);
            return 1;
        }
        size_t n = fread(text_buf, 1, sizeof(text_buf) - 1, f);
        fclose(f);
        text_buf[n] = '\0';
        // 去除末尾换行
        while (n > 0 && (text_buf[n-1] == '\n' || text_buf[n-1] == '\r')) {
            text_buf[--n] = '\0';
        }
        display_text = text_buf;
        TS_LOGI(TAG, "Read text from file: %s (%zu bytes)", text_file, n);
    }
    
    if (!display_text || strlen(display_text) == 0) {
        ts_console_error("--text or --text-file required for text display\n");
        return 1;
    }
    
    // 解析转义序列（支持 \\uXXXX 输入中文）
    char parsed_text[256];
    parse_escape_sequences(display_text, parsed_text, sizeof(parsed_text));
    
    if (!font_name) {
        font_name = "cjk";  // 默认字体（支持中文）
    }
    
    // 只支持 matrix 设备
    if (strcmp(device_name, "matrix") != 0 && strcmp(device_name, "led_matrix") != 0) {
        ts_console_error("Text display only supported on matrix device (32x32)\n");
        return 1;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_console_error("Device '%s' not found\n", device_name);
        return 1;
    }
    
    // 获取 layer
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_console_error("Failed to get layer\n");
        return 1;
    }
    
    // 加载或复用字体
    char font_path[64];
    if (!build_font_path(font_name, font_path, sizeof(font_path))) {
        ts_console_error("Invalid font name: %s\n", font_name);
        return 1;
    }
    
    // 检查是否需要切换字体
    if (s_current_font && strcmp(s_current_font_name, font_name) != 0) {
        ts_font_unload(s_current_font);
        s_current_font = NULL;
        s_current_font_name[0] = '\0';
    }
    
    // 加载字体
    if (!s_current_font) {
        ts_font_config_t font_cfg = TS_FONT_DEFAULT_CONFIG();
        
        s_current_font = ts_font_load(font_path, &font_cfg);
        if (!s_current_font) {
            ts_console_error("Failed to load font: %s\n", font_path);
            ts_console_printf("Hint: Place font file at /sdcard/fonts/%s.fnt\n", font_name);
            return 1;
        }
        
        strncpy(s_current_font_name, font_name, sizeof(s_current_font_name) - 1);
        TS_LOGI(TAG, "Font loaded: %s (%ux%u, %lu glyphs)", 
                font_path, s_current_font->header.width,
                s_current_font->header.height, (unsigned long)s_current_font->header.glyph_count);
    }
    
    // 解析颜色
    ts_led_rgb_t fg_color = TS_LED_WHITE;
    if (color_str) {
        if (ts_led_parse_color(color_str, &fg_color) != ESP_OK) {
            ts_console_error("Invalid color: %s\n", color_str);
            return 1;
        }
    }
    
    // 解析滚动方向
    ts_text_scroll_t scroll = TS_TEXT_SCROLL_NONE;
    if (scroll_dir_str) {
        if (strcmp(scroll_dir_str, "left") == 0) scroll = TS_TEXT_SCROLL_LEFT;
        else if (strcmp(scroll_dir_str, "right") == 0) scroll = TS_TEXT_SCROLL_RIGHT;
        else if (strcmp(scroll_dir_str, "up") == 0) scroll = TS_TEXT_SCROLL_UP;
        else if (strcmp(scroll_dir_str, "down") == 0) scroll = TS_TEXT_SCROLL_DOWN;
        else if (strcmp(scroll_dir_str, "none") != 0) {
            ts_console_error("Invalid scroll direction: %s (use: left, right, up, down, none)\n", scroll_dir_str);
            return 1;
        }
    }
    
    // 统一使用覆盖层模式（Layer 1），便于 --stop-text 管理
    ts_text_overlay_config_t overlay_cfg = TS_TEXT_OVERLAY_DEFAULT_CONFIG();
    overlay_cfg.text = parsed_text;
    overlay_cfg.font = s_current_font;
    overlay_cfg.color = fg_color;
    overlay_cfg.x = (int16_t)start_x;
    overlay_cfg.y = (int16_t)start_y;
    overlay_cfg.scroll = scroll;
    overlay_cfg.scroll_speed = (scroll_speed > 0) ? (uint8_t)scroll_speed : 30;
    overlay_cfg.invert_on_overlap = invert_overlap;
    overlay_cfg.loop_scroll = loop_scroll;
    overlay_cfg.align = parse_text_align(align_str);
    
    esp_err_t ret = ts_led_text_overlay_start(device_name, &overlay_cfg);
    if (ret != ESP_OK) {
        ts_console_error("Failed to start text overlay: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_success("Text displayed on '%s' (Layer 1)\n", device_name);
    ts_console_printf("  Font: %s (%ux%u)\n", font_name, 
                      s_current_font->header.width,
                      s_current_font->header.height);
    ts_console_printf("  Text: %s\n", parsed_text);
    if (start_x != 0 || start_y != 0) {
        ts_console_printf("  Position: (%d, %d)\n", start_x, start_y);
    }
    if (scroll != TS_TEXT_SCROLL_NONE) {
        ts_console_printf("  Scroll: %s (speed=%d, loop=%s)\n", 
                          scroll_dir_str ? scroll_dir_str : "none", 
                          overlay_cfg.scroll_speed,
                          loop_scroll ? "yes" : "no");
    }
    if (invert_overlap) {
        ts_console_printf("  Invert: on (text inverts over bright pixels)\n");
    }
    ts_console_printf("Use 'led --stop-text' to clear text\n");
    
    // 显示缓存统计
    uint32_t hits, misses;
    ts_font_get_stats(s_current_font, &hits, &misses);
    if (hits + misses > 0) {
        ts_console_printf("  Cache: %lu hits, %lu misses (%.1f%% hit rate)\n",
                          (unsigned long)hits, (unsigned long)misses,
                          100.0f * hits / (hits + misses));
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: led --qrcode                             */
/*===========================================================================*/

static int do_led_qrcode(const char *device_name, const char *text, 
                          const char *ecc_str, const char *fg_color_str,
                          const char *bg_image_path)
{
    if (!device_name) {
        device_name = "matrix";  // 默认使用 matrix 设备
    }
    
    if (!text || strlen(text) == 0) {
        ts_console_error("--text required for QR code generation\n");
        return 1;
    }
    
    // 只支持 matrix 设备
    if (strcmp(device_name, "matrix") != 0 && strcmp(device_name, "led_matrix") != 0) {
        ts_console_error("QR code only supported on matrix device (32x32)\n");
        return 1;
    }
    
    // 解析设备名称（别名 → 内部名称）
    const char *internal_name = resolve_device_name(device_name);
    
    // 解析 ECC 等级
    ts_led_qr_ecc_t ecc = TS_LED_QR_ECC_MEDIUM;  // 默认 Medium
    if (ecc_str) {
        if (ts_led_qr_ecc_parse(ecc_str, &ecc) != ESP_OK) {
            ts_console_error("Invalid ECC level: %s (use L, M, Q, or H)\n", ecc_str);
            return 1;
        }
    }
    
    // 解析前景色
    ts_led_rgb_t fg_color = TS_LED_WHITE;  // 默认白色
    if (fg_color_str) {
        if (ts_led_parse_color(fg_color_str, &fg_color) != ESP_OK) {
            ts_console_error("Invalid color: %s\n", fg_color_str);
            return 1;
        }
    }
    
    // 加载背景图（复用图像加载接口）
    ts_led_image_t bg_image = NULL;
    if (bg_image_path) {
        esp_err_t img_ret = ts_led_image_load(bg_image_path, TS_LED_IMG_FMT_AUTO, &bg_image);
        if (img_ret != ESP_OK) {
            ts_console_error("Failed to load background image: %s (%s)\n", 
                             bg_image_path, esp_err_to_name(img_ret));
            return 1;
        }
        ts_led_image_info_t info;
        ts_led_image_get_info(bg_image, &info);
        ts_console_printf("Loaded background image: %ux%u\n", info.width, info.height);
    }
    
    // 配置 QR 码
    ts_led_qr_config_t config = TS_LED_QR_DEFAULT_CONFIG();
    config.text = text;
    config.ecc = ecc;
    config.fg_color = fg_color;
    config.bg_color = TS_LED_BLACK;
    config.bg_image = bg_image;  // 背景图句柄（可为 NULL）
    config.center = true;
    // 优先使用 v3 (29x29)，放不下时自动升级到 v4 (33x33)
    // v3 刚好能放入 32x32 matrix；v4 需要裁剪 1px 边缘
    config.version_min = 1;
    config.version_max = 4;
    
    // 生成并显示
    ts_led_qr_result_t result;
    esp_err_t ret = ts_led_qrcode_show_on_device(internal_name, &config, &result);
    
    // 释放背景图
    if (bg_image) {
        ts_led_image_free(bg_image);
    }
    
    if (ret == ESP_ERR_INVALID_SIZE) {
        ts_console_error("Text too long for QR code v4 (max ~50 alphanumeric chars)\n");
        return 1;
    }
    if (ret != ESP_OK) {
        ts_console_error("Failed to generate QR code: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_success("QR code v%d (%dx%d) displayed, ECC=%s\n", 
                       result.version, result.size, result.size,
                       ts_led_qr_ecc_name(ecc));
    ts_console_printf("  Text: %s\n", text);
    ts_console_printf("  Remaining capacity: %d chars\n", result.data_capacity);
    
    return 0;
}

/*===========================================================================*/
/*                     Color Correction Commands                              */
/*===========================================================================*/

static int do_led_cc_status(bool json)
{
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("led.color_correction.get", NULL, &result);
    
    if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
        if (json) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            /* Formatted output */
            bool enabled = cJSON_IsTrue(cJSON_GetObjectItem(result.data, "enabled"));
            ts_console_printf("Color Correction: %s\n", enabled ? "✅ Enabled" : "❌ Disabled");
            ts_console_printf("─────────────────────────────────────\n");
            
            cJSON *wp = cJSON_GetObjectItem(result.data, "white_point");
            if (wp) {
                bool wp_en = cJSON_IsTrue(cJSON_GetObjectItem(wp, "enabled"));
                float r = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(wp, "red_scale"));
                float g = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(wp, "green_scale"));
                float b = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(wp, "blue_scale"));
                ts_console_printf("White Point: %s  R:%.2f G:%.2f B:%.2f\n",
                                 wp_en ? "✅" : "❌", r, g, b);
            }
            
            cJSON *gamma = cJSON_GetObjectItem(result.data, "gamma");
            if (gamma) {
                bool gamma_en = cJSON_IsTrue(cJSON_GetObjectItem(gamma, "enabled"));
                float val = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(gamma, "gamma"));
                ts_console_printf("Gamma:       %s  Value: %.2f\n", 
                                 gamma_en ? "✅" : "❌", val);
            }
            
            cJSON *brightness = cJSON_GetObjectItem(result.data, "brightness");
            if (brightness) {
                bool br_en = cJSON_IsTrue(cJSON_GetObjectItem(brightness, "enabled"));
                float factor = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(brightness, "factor"));
                ts_console_printf("Brightness:  %s  Factor: %.2f\n",
                                 br_en ? "✅" : "❌", factor);
            }
            
            cJSON *saturation = cJSON_GetObjectItem(result.data, "saturation");
            if (saturation) {
                bool sat_en = cJSON_IsTrue(cJSON_GetObjectItem(saturation, "enabled"));
                float factor = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(saturation, "factor"));
                ts_console_printf("Saturation:  %s  Factor: %.2f\n",
                                 sat_en ? "✅" : "❌", factor);
            }
        }
        ts_api_result_free(&result);
        return 0;
    }
    
    ts_console_error("Failed to get color correction config\n");
    ts_api_result_free(&result);
    return 1;
}

static int do_led_cc_set(bool enabled, double wp_r, double wp_g, double wp_b,
                         double gamma, double brightness, double saturation)
{
    cJSON *params = cJSON_CreateObject();
    
    /* Global enable if explicitly provided */
    cJSON_AddBoolToObject(params, "enabled", enabled);
    
    /* White point */
    if (wp_r >= 0 || wp_g >= 0 || wp_b >= 0) {
        cJSON *wp = cJSON_CreateObject();
        cJSON_AddBoolToObject(wp, "enabled", true);
        if (wp_r >= 0) cJSON_AddNumberToObject(wp, "red_scale", wp_r);
        if (wp_g >= 0) cJSON_AddNumberToObject(wp, "green_scale", wp_g);
        if (wp_b >= 0) cJSON_AddNumberToObject(wp, "blue_scale", wp_b);
        cJSON_AddItemToObject(params, "white_point", wp);
    }
    
    /* Gamma */
    if (gamma >= 0) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddBoolToObject(g, "enabled", true);
        cJSON_AddNumberToObject(g, "gamma", gamma);
        cJSON_AddItemToObject(params, "gamma", g);
    }
    
    /* Brightness */
    if (brightness >= 0) {
        cJSON *br = cJSON_CreateObject();
        cJSON_AddBoolToObject(br, "enabled", true);
        cJSON_AddNumberToObject(br, "factor", brightness);
        cJSON_AddItemToObject(params, "brightness", br);
    }
    
    /* Saturation */
    if (saturation >= 0) {
        cJSON *sat = cJSON_CreateObject();
        cJSON_AddBoolToObject(sat, "enabled", true);
        cJSON_AddNumberToObject(sat, "factor", saturation);
        cJSON_AddItemToObject(params, "saturation", sat);
    }
    
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("led.color_correction.set", params, &result);
    cJSON_Delete(params);
    
    if (ret == ESP_OK && result.code == TS_API_OK) {
        ts_console_success("Color correction configuration updated\n");
        ts_api_result_free(&result);
        return 0;
    }
    
    ts_console_error("Failed to set color correction: %s\n", 
                    result.message ? result.message : "Unknown error");
    ts_api_result_free(&result);
    return 1;
}

static int do_led_cc_reset(void)
{
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("led.color_correction.reset", NULL, &result);
    
    if (ret == ESP_OK && result.code == TS_API_OK) {
        ts_console_success("Color correction reset to defaults\n");
        ts_api_result_free(&result);
        return 0;
    }
    
    ts_console_error("Failed to reset color correction\n");
    ts_api_result_free(&result);
    return 1;
}

static int do_led_cc_export(void)
{
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("led.color_correction.export", NULL, &result);
    
    if (ret == ESP_OK && result.code == TS_API_OK) {
        cJSON *path = cJSON_GetObjectItem(result.data, "path");
        ts_console_success("Color correction exported to: %s\n",
                          path ? path->valuestring : "SD card");
        ts_api_result_free(&result);
        return 0;
    }
    
    ts_console_error("Failed to export color correction\n");
    ts_api_result_free(&result);
    return 1;
}

static int do_led_cc_import(void)
{
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("led.color_correction.import", NULL, &result);
    
    if (ret == ESP_OK && result.code == TS_API_OK) {
        ts_console_success("Color correction imported from SD card\n");
        ts_api_result_free(&result);
        return 0;
    }
    
    ts_console_error("Failed to import color correction: %s\n",
                    result.message ? result.message : "File not found");
    ts_api_result_free(&result);
    return 1;
}

/*===========================================================================*/
/*                          Main Command Handler                              */
/*===========================================================================*/

static int cmd_led(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_led_args);
    
    // 显示帮助
    if (s_led_args.help->count > 0) {
        ts_console_printf("Usage: led [options]\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  -s, --status           Show LED device status\n");
        ts_console_printf("      --on               Turn on LED with color\n");
        ts_console_printf("      --off              Turn off LED\n");
        ts_console_printf("  -b, --brightness       Get/set brightness\n");
        ts_console_printf("  -c, --clear            Clear all LEDs on device\n");
        ts_console_printf("  -e, --effect           Start LED animation effect\n");
        ts_console_printf("      --stop-effect      Stop running animation\n");
        ts_console_printf("      --list-effects     List available animations\n");
        ts_console_printf("      --filter           Apply post-processing filter\n");
        ts_console_printf("      --stop-filter      Remove post-processing filter\n");
        ts_console_printf("      --list-filters     List available filters\n");
        ts_console_printf("      --filter-name      Filter name (pulse, blink, etc.)\n");
        ts_console_printf("      --parse-color      Parse color info\n");
        ts_console_printf("      --image            Display image on matrix\n");
        ts_console_printf("      --qrcode           Generate and display QR code\n");
        ts_console_printf("      --draw-text        Display text on matrix\n");
        ts_console_printf("      --text <string>    Text content for QR code/text display\n");
        ts_console_printf("      --font <name>      Font name (default: cjk)\n");
        ts_console_printf("      --align <mode>     Text align: left, center, right\n");
        ts_console_printf("      --ecc <L|M|Q|H>    QR error correction level\n");
        ts_console_printf("      --file <path>      Image file path\n");
        ts_console_printf("  -d, --device <name>    Device: touch, board, matrix\n");
        ts_console_printf("  -n, --name <effect>    Animation name\n");
        ts_console_printf("  -v, --value <0-255>    Brightness value\n");
        ts_console_printf("      --color <color>    Color: #RRGGBB or name\n");
        ts_console_printf("      --speed <1-100>    Effect speed (1=slow, 100=fast)\n");
        ts_console_printf("  -j, --json             JSON output\n");
        ts_console_printf("  -h, --help             Show this help\n\n");
        ts_console_printf("Devices:\n");
        ts_console_printf("  touch   - Single indicator LED (point light)\n");
        ts_console_printf("  board   - PCB edge ring LEDs (28 LEDs, circular)\n");
        ts_console_printf("  matrix  - LED matrix panel (32x32, grid)\n\n");
        ts_console_printf("Effects (by device type):\n");
        ts_console_printf("  Common:  rainbow, breathing, solid, sparkle\n");
        ts_console_printf("  Touch:   pulse, heartbeat, color_cycle\n");
        ts_console_printf("  Board:   chase, comet, spin, breathe_wave\n");
        ts_console_printf("  Matrix:  fire, rain, plasma, ripple\n");
        ts_console_printf("\n  Use 'led --list-effects' for all, or\n");
        ts_console_printf("       'led --list-effects --device <name>' for specific device\n\n");
        ts_console_printf("Boot Configuration:\n");
        ts_console_printf("  --save                 Save current state as boot config\n");
        ts_console_printf("  --show-boot            Show saved boot config\n");
        ts_console_printf("  --clear-boot           Clear boot config\n\n");
        ts_console_printf("Examples:\n");
        ts_console_printf("  led --status\n");
        ts_console_printf("  led --on --device touch                  (white)\n");
        ts_console_printf("  led --on --device touch --color red\n");
        ts_console_printf("  led --on --device touch --color #FF0000\n");
        ts_console_printf("  led --off --device touch\n");
        ts_console_printf("  led --brightness --device touch --value 128\n");
        ts_console_printf("  led --effect --device touch --name heartbeat\n");
        ts_console_printf("  led --effect --device board --name spin --speed 50\n");
        ts_console_printf("  led --effect --device matrix --name fire\n");
        ts_console_printf("  led --stop-effect --device touch\n");
        ts_console_printf("  led --save --device touch                (save touch)\n");
        ts_console_printf("  led --save                               (save all)\n");
        ts_console_printf("  led --show-boot                          (show saved)\n");
        ts_console_printf("  led --image --device matrix --file /sdcard/logo.png\n");
        ts_console_printf("  led --filter --device matrix --filter-name pulse\n");
        ts_console_printf("  led --filter --device matrix --filter-name blink --speed 80\n");
        ts_console_printf("  led --stop-filter --device matrix\n");
        ts_console_printf("  led --list-filters\n");
        ts_console_printf("  led --qrcode --text \"https://tianshan.io\"\n");
        ts_console_printf("  led --qrcode --text \"HELLO\" --ecc H\n");
        ts_console_printf("  led --qrcode --text \"192.168.1.1\" --color green\n");
        ts_console_printf("  led --draw-text --text \"Hi\" --font boutique9x9\n");
        ts_console_printf("  led --draw-text --text \"Hello\" --color cyan --align center\n");
        ts_console_printf("  led --draw-text --text-file /sdcard/msg.txt --font cjk\n");
        ts_console_printf("\nSupported image formats: PNG, BMP, JPG, GIF (animated)\n");
        ts_console_printf("\nText display:\n");
        ts_console_printf("  Font files in /sdcard/fonts/*.fnt (use tools/ttf2fnt.py)\n");
        ts_console_printf("  Chinese: use --text-file with UTF-8 file (recommended)\n");
        ts_console_printf("  Or use escape: --text \"\\\\u4f60\\\\u597d\" (你好)\n");
        ts_console_printf("\nQR Code v4 capacity (alphanumeric):\n");
        ts_console_printf("  ECC L (~7%% recovery):  114 chars\n");
        ts_console_printf("  ECC M (~15%% recovery): 90 chars\n");
        ts_console_printf("  ECC Q (~25%% recovery): 67 chars\n");
        ts_console_printf("  ECC H (~30%% recovery): 50 chars\n");
        return 0;
    }
    
    // 检查是否是 --draw-text 命令并且可以从原始命令行提取文本
    // 如果是，跳过 argtable3 的解析错误（UTF-8 中文会导致解析问题）
    // 注意：argtable3 可能因 UTF-8 问题而无法正确解析 --draw-text 标志
    // 所以我们直接从原始命令行检测
    bool can_recover_from_raw_cmdline = false;
    bool is_draw_text_from_cmdline = false;
    const char *cmdline = ts_console_get_raw_cmdline();
    
    // 调试：打印原始命令行
    TS_LOGI(TAG, "nerrors=%d, cmdline=%s", nerrors, cmdline ? cmdline : "(null)");
    if (cmdline) {
        // 打印原始字节
        char hex_buf[128] = {0};
        int pos = 0;
        for (size_t i = 0; i < strlen(cmdline) && i < 40 && pos < 120; i++) {
            pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02X ", (uint8_t)cmdline[i]);
        }
        TS_LOGI(TAG, "cmdline bytes: %s", hex_buf);
    }
    
    if (nerrors != 0 && cmdline) {
        // 直接从原始命令行检查是否包含 --draw-text
        if (strstr(cmdline, "--draw-text")) {
            is_draw_text_from_cmdline = true;
            TS_LOGI(TAG, "Found --draw-text in cmdline");
            // 检查是否有 --text（但不是 --text-file）
            const char *text_pos = strstr(cmdline, "--text");
            while (text_pos) {
                TS_LOGI(TAG, "Found --text at offset %ld", (long)(text_pos - cmdline));
                // 确保不是 --text-file
                if (strncmp(text_pos, "--text-file", 11) != 0) {
                    char test_buf[64];
                    if (extract_text_from_cmdline(cmdline, test_buf, sizeof(test_buf))) {
                        can_recover_from_raw_cmdline = true;
                        TS_LOGI(TAG, "Recovering from UTF-8 parse error, extracted: %s", test_buf);
                        break;
                    } else {
                        TS_LOGW(TAG, "extract_text_from_cmdline failed");
                    }
                }
                // 继续查找下一个 --text
                text_pos = strstr(text_pos + 6, "--text");
            }
        } else {
            TS_LOGI(TAG, "--draw-text not found in cmdline");
        }
    }
    
    if (nerrors != 0 && !can_recover_from_raw_cmdline) {
        arg_print_errors(stderr, s_led_args.end, argv[0]);
        return 1;
    }
    
    bool json = s_led_args.json->count > 0;
    const char *device = s_led_args.device->count > 0 ? 
                         s_led_args.device->sval[0] : NULL;
    int value = s_led_args.value->count > 0 ? 
                s_led_args.value->ival[0] : -1;
    const char *color = s_led_args.color_val->count > 0 ? 
                        s_led_args.color_val->sval[0] : NULL;
    const char *effect_name = s_led_args.effect_name->count > 0 ?
                              s_led_args.effect_name->sval[0] : NULL;
    int speed = s_led_args.speed->count > 0 ?
                s_led_args.speed->ival[0] : -1;
    const char *file_path = s_led_args.file->count > 0 ?
                            s_led_args.file->sval[0] : NULL;
    const char *center_mode = s_led_args.center->count > 0 ?
                              s_led_args.center->sval[0] : NULL;
    const char *filter_name_val = s_led_args.filter_name->count > 0 ?
                                  s_led_args.filter_name->sval[0] : NULL;
    const char *qr_text = s_led_args.text->count > 0 ?
                          s_led_args.text->sval[0] : NULL;
    const char *qr_ecc = s_led_args.ecc->count > 0 ?
                         s_led_args.ecc->sval[0] : NULL;
    const char *qr_bg_path = s_led_args.qr_bg->count > 0 ?
                             s_led_args.qr_bg->sval[0] : NULL;
    const char *font_name = s_led_args.font->count > 0 ?
                            s_led_args.font->sval[0] : NULL;
    const char *text_align = s_led_args.align->count > 0 ?
                             s_led_args.align->sval[0] : NULL;
    const char *text_file_path = s_led_args.text_file->count > 0 ?
                                 s_led_args.text_file->sval[0] : NULL;
    const char *scroll_dir = s_led_args.scroll->count > 0 ?
                             s_led_args.scroll->sval[0] : NULL;
    int text_x = s_led_args.text_x->count > 0 ?
                 s_led_args.text_x->ival[0] : 0;
    int text_y = s_led_args.text_y->count > 0 ?
                 s_led_args.text_y->ival[0] : 0;
    bool invert_overlap = s_led_args.invert->count > 0;
    bool loop_scroll = s_led_args.loop->count > 0;
    
    /* Color correction parameters */
    double cc_wp_r = s_led_args.cc_wp_r->count > 0 ? s_led_args.cc_wp_r->dval[0] : -1;
    double cc_wp_g = s_led_args.cc_wp_g->count > 0 ? s_led_args.cc_wp_g->dval[0] : -1;
    double cc_wp_b = s_led_args.cc_wp_b->count > 0 ? s_led_args.cc_wp_b->dval[0] : -1;
    double cc_gamma = s_led_args.cc_gamma->count > 0 ? s_led_args.cc_gamma->dval[0] : -1;
    double cc_brightness = s_led_args.cc_brightness->count > 0 ? s_led_args.cc_brightness->dval[0] : -1;
    double cc_saturation = s_led_args.cc_saturation->count > 0 ? s_led_args.cc_saturation->dval[0] : -1;
    
    /* Color correction commands */
    if (s_led_args.cc_reset->count > 0) {
        return do_led_cc_reset();
    }
    
    if (s_led_args.cc_export->count > 0) {
        return do_led_cc_export();
    }
    
    if (s_led_args.cc_import->count > 0) {
        return do_led_cc_import();
    }
    
    if (s_led_args.cc_set->count > 0) {
        bool enabled = s_led_args.cc_enabled->count > 0;
        return do_led_cc_set(enabled, cc_wp_r, cc_wp_g, cc_wp_b, 
                            cc_gamma, cc_brightness, cc_saturation);
    }
    
    if (s_led_args.cc->count > 0) {
        return do_led_cc_status(json);
    }
    
    // 停止文本覆盖层
    if (s_led_args.stop_text->count > 0) {
        const char *dev_name = device ? device : "matrix";
        esp_err_t ret = ts_led_text_overlay_stop(dev_name);
        if (ret == ESP_OK) {
            ts_console_success("Text overlay stopped on '%s'\n", dev_name);
            return 0;
        } else {
            ts_console_error("Failed to stop text overlay\n");
            return 1;
        }
    }
    
    // 绘制文本（也支持从原始命令行恢复的情况）
    if (s_led_args.draw_text->count > 0 || is_draw_text_from_cmdline) {
        return do_led_draw_text(device, qr_text, font_name, color, text_align, text_file_path,
                                scroll_dir, text_x, text_y, invert_overlap, loop_scroll, speed);
    }
    
    // 生成 QR 码
    if (s_led_args.qrcode->count > 0) {
        return do_led_qrcode(device, qr_text, qr_ecc, color, qr_bg_path);
    }
    
    // 显示图像
    if (s_led_args.image->count > 0) {
        return do_led_image(device, file_path, center_mode);
    }
    
    // 应用后处理效果
    if (s_led_args.filter->count > 0) {
        return do_led_filter(device, filter_name_val, speed);
    }
    
    // 停止后处理效果
    if (s_led_args.stop_filter->count > 0) {
        return do_led_stop_filter(device);
    }
    
    // 列出后处理效果
    if (s_led_args.list_filters->count > 0) {
        return do_led_list_filters(json);
    }
    
    // 测试模式
    if (s_led_args.test->count > 0) {
        int test_mode = (value >= 0) ? value : 0;
        return do_led_test(device, test_mode);
    }
    
    // 启动特效
    if (s_led_args.effect->count > 0) {
        return do_led_effect(device, effect_name, speed, color);
    }
    
    // 停止特效
    if (s_led_args.stop_effect->count > 0) {
        return do_led_stop_effect(device);
    }
    
    // 点亮 LED
    if (s_led_args.on->count > 0) {
        return do_led_on(device, color);
    }
    
    // 关闭 LED
    if (s_led_args.off->count > 0) {
        return do_led_off(device);
    }
    
    // 亮度
    if (s_led_args.brightness->count > 0) {
        return do_led_brightness(device, value, value >= 0);
    }
    
    // 清除
    if (s_led_args.clear->count > 0) {
        return do_led_clear(device);
    }
    
    // 列出特效 (可选指定设备过滤)
    if (s_led_args.list_effects->count > 0) {
        return do_led_list_effects(device, json);
    }
    
    // 颜色解析
    if (s_led_args.parse_color->count > 0) {
        return do_led_parse_color(color, json);
    }
    
    // 保存启动配置
    if (s_led_args.save->count > 0) {
        return do_led_save(device);
    }
    
    // 清除启动配置
    if (s_led_args.clear_boot->count > 0) {
        return do_led_clear_boot(device);
    }
    
    // 显示启动配置
    if (s_led_args.show_boot->count > 0) {
        return do_led_show_boot(device, json);
    }
    
    // 默认显示状态
    return do_led_status(device, json);
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_led_register(void)
{
    s_led_args.status       = arg_lit0("s", "status", "Show status");
    s_led_args.on           = arg_lit0(NULL, "on", "Turn on LED");
    s_led_args.off          = arg_lit0(NULL, "off", "Turn off LED");
    s_led_args.brightness   = arg_lit0("b", "brightness", "Get/set brightness");
    s_led_args.clear        = arg_lit0("c", "clear", "Clear LEDs");
    s_led_args.effect       = arg_lit0("e", "effect", "Start animation");
    s_led_args.stop_effect  = arg_lit0(NULL, "stop-effect", "Stop animation");
    s_led_args.list_effects = arg_lit0(NULL, "list-effects", "List animations");
    s_led_args.filter       = arg_lit0(NULL, "filter", "Apply post-processing filter");
    s_led_args.stop_filter  = arg_lit0(NULL, "stop-filter", "Remove filter");
    s_led_args.list_filters = arg_lit0(NULL, "list-filters", "List filters");
    s_led_args.filter_name  = arg_str0(NULL, "filter-name", "<name>", "Filter name");
    s_led_args.parse_color  = arg_lit0(NULL, "parse-color", "Parse color info");
    s_led_args.image        = arg_lit0(NULL, "image", "Display image");
    s_led_args.qrcode       = arg_lit0(NULL, "qrcode", "Generate QR code");
    s_led_args.draw_text    = arg_lit0(NULL, "draw-text", "Display text on matrix");
    s_led_args.stop_text    = arg_lit0(NULL, "stop-text", "Stop text overlay");
    s_led_args.text         = arg_str0(NULL, "text", "<string>", "Text content");
    s_led_args.text_file    = arg_str0(NULL, "text-file", "<path>", "Read text from file (UTF-8)");
    s_led_args.font         = arg_str0(NULL, "font", "<name>", "Font name (default: cjk)");
    s_led_args.align        = arg_str0(NULL, "align", "<mode>", "Text align: left, center, right");
    s_led_args.scroll       = arg_str0(NULL, "scroll", "<dir>", "Scroll: left, right, up, down, none");
    s_led_args.text_x       = arg_int0(NULL, "x", "<pos>", "Text X position (default: 0)");
    s_led_args.text_y       = arg_int0(NULL, "y", "<pos>", "Text Y position (default: 0)");
    s_led_args.invert       = arg_lit0(NULL, "invert", "Invert on overlap for readability");
    s_led_args.loop         = arg_lit0(NULL, "loop", "Loop scroll continuously");
    s_led_args.ecc          = arg_str0(NULL, "ecc", "<L|M|Q|H>", "QR error correction");
    s_led_args.qr_bg        = arg_str0(NULL, "bg", "<path>", "QR foreground uses image pixels");
    s_led_args.test         = arg_lit0("t", "test", "Show test pattern");
    s_led_args.save         = arg_lit0(NULL, "save", "Save as boot config");
    s_led_args.clear_boot   = arg_lit0(NULL, "clear-boot", "Clear boot config");
    s_led_args.show_boot    = arg_lit0(NULL, "show-boot", "Show boot config");
    s_led_args.device       = arg_str0("d", "device", "<name>", "Device name");
    s_led_args.file         = arg_str0("f", "file", "<path>", "Image file path");
    s_led_args.center       = arg_str0(NULL, "center", "<mode>", "Center mode: image, content");
    s_led_args.value        = arg_int0("v", "value", "<0-255>", "Value/test mode");
    s_led_args.color_val    = arg_str0(NULL, "color", "<color>", "Color value");
    s_led_args.effect_name  = arg_str0("n", "name", "<effect>", "Effect name");
    s_led_args.speed        = arg_int0(NULL, "speed", "<1-100>", "Effect speed (1=slow, 100=fast)");
    s_led_args.intensity    = arg_int0(NULL, "intensity", "<0-100>", "Intensity/density (0-100)");
    s_led_args.scale        = arg_int0(NULL, "scale", "<1-100>", "Scale parameter (1-100)");
    s_led_args.levels       = arg_int0(NULL, "levels", "<2-16>", "Color levels (2-16)");
    s_led_args.saturation   = arg_int0(NULL, "saturation", "<0-255>", "Saturation (0-255)");
    s_led_args.wavelength   = arg_int0(NULL, "wavelength", "<1-32>", "Wave length in pixels (1-32)");
    s_led_args.amplitude    = arg_int0(NULL, "amplitude", "<1-16>", "Wave amplitude (1-16)");
    s_led_args.direction    = arg_int0(NULL, "direction", "<0-3>", "Direction: 0=horizontal, 1=vertical, 2/3=diagonal");
    s_led_args.width        = arg_int0(NULL, "width", "<1-8>", "Line width in pixels (1-8)");
    s_led_args.frequency    = arg_int0(NULL, "frequency", "<0-100>", "Frequency parameter (0-100)");
    s_led_args.density      = arg_int0(NULL, "density", "<0-100>", "Density parameter (0-100)");
    s_led_args.decay        = arg_int0(NULL, "decay", "<0-255>", "Decay/fade speed (0-255)");
    s_led_args.amount       = arg_int0(NULL, "amount", "<-100 to 100>", "Amount/contrast (-100 to +100)");
    /* Color Correction */
    s_led_args.cc           = arg_lit0(NULL, "cc", "Show color correction config");
    s_led_args.cc_set       = arg_lit0(NULL, "cc-set", "Set color correction parameters");
    s_led_args.cc_reset     = arg_lit0(NULL, "cc-reset", "Reset color correction to defaults");
    s_led_args.cc_export    = arg_lit0(NULL, "cc-export", "Export color correction to SD card");
    s_led_args.cc_import    = arg_lit0(NULL, "cc-import", "Import color correction from SD card");
    s_led_args.cc_enabled   = arg_lit0(NULL, "cc-enabled", "Enable global color correction");
    s_led_args.cc_wp_r      = arg_dbl0(NULL, "cc-wp-r", "<0-2>", "White point red scale");
    s_led_args.cc_wp_g      = arg_dbl0(NULL, "cc-wp-g", "<0-2>", "White point green scale");
    s_led_args.cc_wp_b      = arg_dbl0(NULL, "cc-wp-b", "<0-2>", "White point blue scale");
    s_led_args.cc_gamma     = arg_dbl0(NULL, "cc-gamma", "<0.1-4>", "Gamma value");
    s_led_args.cc_brightness = arg_dbl0(NULL, "cc-brightness", "<0-2>", "Brightness factor");
    s_led_args.cc_saturation = arg_dbl0(NULL, "cc-saturation", "<0-2>", "Saturation factor");
    s_led_args.json         = arg_lit0("j", "json", "JSON output");
    s_led_args.help         = arg_lit0("h", "help", "Show help");
    s_led_args.end          = arg_end(48);  /* 增加错误计数限制 */
    
    const ts_console_cmd_t cmd = {
        .command = "led",
        .help = "LED control and effects",
        .hint = NULL,
        .category = TS_CMD_CAT_LED,
        .func = cmd_led,
        .argtable = &s_led_args
    };
    
    esp_err_t ret = ts_console_register_cmd(&cmd);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "LED commands registered");
    }
    
    return ret;
}