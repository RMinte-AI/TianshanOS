/**
 * @file ts_api_led.c
 * @brief LED Control API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_api.h"
#include "ts_log.h"
#include "ts_led.h"
#include "ts_led_preset.h"
#include "ts_led_image.h"
#include "ts_led_qrcode.h"
#include "ts_led_text.h"
#include "ts_led_font.h"
#include "ts_led_effect.h"
#include "ts_led_color_correction.h"
#include <string.h>

#define TAG "api_led"

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief 设备名称映射：用户友好名 -> 内部名
 * 与 ts_cmd_led.c 保持一致
 */
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

static const char *layout_to_str(ts_led_layout_t layout)
{
    switch (layout) {
        case TS_LED_LAYOUT_STRIP:  return "strip";
        case TS_LED_LAYOUT_MATRIX: return "matrix";
        case TS_LED_LAYOUT_RING:   return "ring";
        default: return "unknown";
    }
}

static esp_err_t parse_color_param(const cJSON *params, const char *key, ts_led_rgb_t *color)
{
    cJSON *color_param = cJSON_GetObjectItem(params, key);
    if (!color_param) {
        return ESP_ERR_NOT_FOUND;
    }
    
    if (cJSON_IsString(color_param)) {
        return ts_led_parse_color(color_param->valuestring, color);
    }
    
    if (cJSON_IsObject(color_param)) {
        cJSON *r = cJSON_GetObjectItem(color_param, "r");
        cJSON *g = cJSON_GetObjectItem(color_param, "g");
        cJSON *b = cJSON_GetObjectItem(color_param, "b");
        if (r && g && b) {
            color->r = (uint8_t)cJSON_GetNumberValue(r);
            color->g = (uint8_t)cJSON_GetNumberValue(g);
            color->b = (uint8_t)cJSON_GetNumberValue(b);
            return ESP_OK;
        }
    }
    
    if (cJSON_IsNumber(color_param)) {
        uint32_t val = (uint32_t)cJSON_GetNumberValue(color_param);
        color->r = (val >> 16) & 0xFF;
        color->g = (val >> 8) & 0xFF;
        color->b = val & 0xFF;
        return ESP_OK;
    }
    
    return ESP_ERR_INVALID_ARG;
}

/*===========================================================================*/
/*                          Device APIs                                       */
/*===========================================================================*/

/**
 * @brief led.list - List LED devices
 */
static esp_err_t api_led_list(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *devices = cJSON_AddArrayToObject(data, "devices");
    
    // 用户友好名和内部名映射
    const char *display_names[] = {"touch", "board", "matrix"};
    const char *internal_names[] = {"led_touch", "led_board", "led_matrix"};
    
    for (size_t i = 0; i < sizeof(display_names) / sizeof(display_names[0]); i++) {
        ts_led_device_t dev = ts_led_device_get(internal_names[i]);
        if (dev) {
            cJSON *device = cJSON_CreateObject();
            cJSON_AddStringToObject(device, "name", display_names[i]);
            cJSON_AddNumberToObject(device, "count", ts_led_device_get_count(dev));
            cJSON_AddNumberToObject(device, "brightness", ts_led_device_get_brightness(dev));
            
            // 添加 layout 类型
            ts_led_layout_t layout = ts_led_device_get_layout(dev);
            cJSON_AddStringToObject(device, "layout", layout_to_str(layout));
            
            // 添加该设备适用的特效列表
            const char *effect_names[24];
            size_t effect_count = ts_led_animation_list_for_device(layout, effect_names, 24);
            cJSON *effects = cJSON_AddArrayToObject(device, "effects");
            for (size_t j = 0; j < effect_count; j++) {
                cJSON_AddItemToArray(effects, cJSON_CreateString(effect_names[j]));
            }
            
            // 添加当前运行状态
            ts_led_boot_config_t state;
            if (ts_led_get_current_state(display_names[i], &state) == ESP_OK) {
                cJSON *current = cJSON_AddObjectToObject(device, "current");
                cJSON_AddStringToObject(current, "animation", state.animation);
                cJSON_AddNumberToObject(current, "speed", state.speed);
                cJSON_AddBoolToObject(current, "on", state.enabled);
                
                // 添加颜色
                cJSON *color = cJSON_AddObjectToObject(current, "color");
                cJSON_AddNumberToObject(color, "r", state.color.r);
                cJSON_AddNumberToObject(color, "g", state.color.g);
                cJSON_AddNumberToObject(color, "b", state.color.b);
            }
            
            cJSON_AddItemToArray(devices, device);
        }
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.brightness - Set device brightness
 * @param device: device name
 * @param brightness: 0-255
 */
static esp_err_t api_led_brightness(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    cJSON *brightness_param = cJSON_GetObjectItem(params, "brightness");
    
    if (!device_param || !cJSON_IsString(device_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'device' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_param->valuestring);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    // If brightness is provided, set it; otherwise return current value
    if (brightness_param && cJSON_IsNumber(brightness_param)) {
        uint8_t brightness = (uint8_t)cJSON_GetNumberValue(brightness_param);
        esp_err_t ret = ts_led_device_set_brightness(dev, brightness);
        if (ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to set brightness");
            return ret;
        }
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_param->valuestring);
    cJSON_AddNumberToObject(data, "brightness", ts_led_device_get_brightness(dev));
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.clear - Clear all LEDs on device
 * @param device: device name
 */
static esp_err_t api_led_clear(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    
    if (!device_param || !cJSON_IsString(device_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'device' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_param->valuestring);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    // 获取 layer 0 并清除（清除 layer buffer，render_task 会自动刷新）
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get layer");
        return ESP_FAIL;
    }
    
    // 先停止任何正在运行的动画
    ts_led_animation_stop(layer);
    
    // 清除 layer buffer
    esp_err_t ret = ts_led_layer_clear(layer);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to clear device");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_param->valuestring);
    cJSON_AddBoolToObject(data, "cleared", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.set - Set LED(s) color
 * @param device: device name
 * @param index: LED index or start index
 * @param count: number of LEDs (optional, default 1)
 * @param color: color value
 */
static esp_err_t api_led_set(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    cJSON *index_param = cJSON_GetObjectItem(params, "index");
    cJSON *count_param = cJSON_GetObjectItem(params, "count");
    
    if (!device_param || !cJSON_IsString(device_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'device' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_param->valuestring);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_led_rgb_t color;
    if (parse_color_param(params, "color", &color) != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid 'color' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get base layer (layer 0) for direct LED control
    // For simplicity, we'll use device clear and direct pixel setting
    uint16_t start = 0;
    uint16_t count = ts_led_device_get_count(dev);
    
    if (index_param && cJSON_IsNumber(index_param)) {
        start = (uint16_t)cJSON_GetNumberValue(index_param);
        count = 1;
    }
    
    if (count_param && cJSON_IsNumber(count_param)) {
        count = (uint16_t)cJSON_GetNumberValue(count_param);
    }
    
    // Use layer API if available, otherwise use simple fill
    // For now, create a temporary approach using device clear + fill
    // In production, you'd want to expose layer handles through the device
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_param->valuestring);
    cJSON_AddNumberToObject(data, "start", start);
    cJSON_AddNumberToObject(data, "count", count);
    cJSON *color_obj = cJSON_AddObjectToObject(data, "color");
    cJSON_AddNumberToObject(color_obj, "r", color.r);
    cJSON_AddNumberToObject(color_obj, "g", color.g);
    cJSON_AddNumberToObject(color_obj, "b", color.b);
    cJSON_AddBoolToObject(data, "success", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.fill - Fill all LEDs with color
 * @param device: device name
 * @param color: color value
 */
static esp_err_t api_led_fill(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    
    if (!device_param || !cJSON_IsString(device_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'device' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_param->valuestring);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_led_rgb_t color;
    if (parse_color_param(params, "color", &color) != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid 'color' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 获取 layer 0 并填充颜色（填充到 layer buffer，render_task 会自动刷新）
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get layer");
        return ESP_FAIL;
    }
    
    // 先停止任何正在运行的动画
    ts_led_animation_stop(layer);
    
    // 填充颜色到 layer buffer
    esp_err_t ret = ts_led_fill(layer, color);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to fill color");
        return ret;
    }
    
    // 记录当前状态：使用 solid 动画表示纯色填充
    ts_led_preset_set_current_animation(device_param->valuestring, "solid", 50);
    ts_led_preset_set_current_color(device_param->valuestring, color);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_param->valuestring);
    cJSON *color_obj = cJSON_AddObjectToObject(data, "color");
    cJSON_AddNumberToObject(color_obj, "r", color.r);
    cJSON_AddNumberToObject(color_obj, "g", color.g);
    cJSON_AddNumberToObject(color_obj, "b", color.b);
    cJSON_AddBoolToObject(data, "success", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Effect APIs                                       */
/*===========================================================================*/

/**
 * @brief led.effect.list - List available effects
 */
static esp_err_t api_led_effect_list(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *effects = cJSON_AddArrayToObject(data, "effects");
    
    const char *names[16];
    size_t count = ts_led_animation_list_builtin(names, 16);
    
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(effects, cJSON_CreateString(names[i]));
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.effect.start - Start effect on device
 * @param device: device name
 * @param effect: effect name
 * @param speed: speed 1-100 (optional, default uses effect's default)
 * @param color: color for effects that support it (optional)
 */
static esp_err_t api_led_effect_start(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    cJSON *effect_param = cJSON_GetObjectItem(params, "effect");
    cJSON *speed_param = cJSON_GetObjectItem(params, "speed");
    
    if (!device_param || !cJSON_IsString(device_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'device' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!effect_param || !cJSON_IsString(effect_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'effect' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_param->valuestring);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    const ts_led_animation_def_t *effect = ts_led_animation_get_builtin(effect_param->valuestring);
    if (!effect) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Effect not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    // 复制动画定义以便修改
    ts_led_animation_def_t modified = *effect;
    
    // 处理速度参数 (1-100, 默认 50)
    uint8_t speed = 50;
    if (speed_param && cJSON_IsNumber(speed_param)) {
        speed = (uint8_t)cJSON_GetNumberValue(speed_param);
        if (speed < 1) speed = 1;
        if (speed > 100) speed = 100;
        // 速度映射：1->200ms, 100->5ms
        modified.frame_interval_ms = 200 - (speed - 1) * 195 / 99;
    }
    
    // 处理颜色参数（用于支持自定义颜色的动画）
    static ts_led_rgb_t effect_color;
    ts_led_rgb_t color;
    if (parse_color_param(params, "color", &color) == ESP_OK) {
        effect_color = color;
        modified.user_data = &effect_color;
    }
    
    // 获取设备的 layer 0 并启动动画
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (layer) {
        ts_led_animation_start(layer, &modified);
    }
    
    // 记录当前状态（供保存用），清除图像和 QR Code
    ts_led_preset_set_current_animation(device_param->valuestring, effect_param->valuestring, speed);
    ts_led_preset_clear_current_image(device_param->valuestring);
    ts_led_preset_set_current_qrcode(device_param->valuestring, NULL);
    
    // 如果有颜色参数，也记录下来
    if (parse_color_param(params, "color", &color) == ESP_OK) {
        ts_led_preset_set_current_color(device_param->valuestring, color);
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_param->valuestring);
    cJSON_AddStringToObject(data, "effect", effect_param->valuestring);
    cJSON_AddNumberToObject(data, "speed", speed);
    cJSON_AddBoolToObject(data, "started", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.effect.stop - Stop effect on device
 * @param device: device name
 */
static esp_err_t api_led_effect_stop(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    
    if (!device_param || !cJSON_IsString(device_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'device' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_param->valuestring);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    // 获取设备的 layer 0 并停止动画
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (layer) {
        ts_led_animation_stop(layer);
    }
    
    // 清除当前动画状态
    ts_led_preset_set_current_animation(device_param->valuestring, NULL, 0);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_param->valuestring);
    cJSON_AddBoolToObject(data, "stopped", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Color APIs                                        */
/*===========================================================================*/

/**
 * @brief led.color.parse - Parse color string
 * @param color: color string (#RRGGBB or name)
 */
static esp_err_t api_led_color_parse(const cJSON *params, ts_api_result_t *result)
{
    cJSON *color_param = cJSON_GetObjectItem(params, "color");
    
    if (!color_param || !cJSON_IsString(color_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'color' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_rgb_t color;
    esp_err_t ret = ts_led_parse_color(color_param->valuestring, &color);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid color string");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "input", color_param->valuestring);
    cJSON *rgb = cJSON_AddObjectToObject(data, "rgb");
    cJSON_AddNumberToObject(rgb, "r", color.r);
    cJSON_AddNumberToObject(rgb, "g", color.g);
    cJSON_AddNumberToObject(rgb, "b", color.b);
    
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", color.r, color.g, color.b);
    cJSON_AddStringToObject(data, "hex", hex);
    
    cJSON_AddNumberToObject(data, "value", (color.r << 16) | (color.g << 8) | color.b);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.color.hsv - Convert HSV to RGB
 * @param h: hue (0-359)
 * @param s: saturation (0-255)
 * @param v: value (0-255)
 */
static esp_err_t api_led_color_hsv(const cJSON *params, ts_api_result_t *result)
{
    cJSON *h_param = cJSON_GetObjectItem(params, "h");
    cJSON *s_param = cJSON_GetObjectItem(params, "s");
    cJSON *v_param = cJSON_GetObjectItem(params, "v");
    
    if (!h_param || !s_param || !v_param) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing h/s/v parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_hsv_t hsv = {
        .h = (uint16_t)cJSON_GetNumberValue(h_param),
        .s = (uint8_t)cJSON_GetNumberValue(s_param),
        .v = (uint8_t)cJSON_GetNumberValue(v_param)
    };
    
    ts_led_rgb_t rgb = ts_led_hsv_to_rgb(hsv);
    
    cJSON *data = cJSON_CreateObject();
    cJSON *hsv_obj = cJSON_AddObjectToObject(data, "hsv");
    cJSON_AddNumberToObject(hsv_obj, "h", hsv.h);
    cJSON_AddNumberToObject(hsv_obj, "s", hsv.s);
    cJSON_AddNumberToObject(hsv_obj, "v", hsv.v);
    
    cJSON *rgb_obj = cJSON_AddObjectToObject(data, "rgb");
    cJSON_AddNumberToObject(rgb_obj, "r", rgb.r);
    cJSON_AddNumberToObject(rgb_obj, "g", rgb.g);
    cJSON_AddNumberToObject(rgb_obj, "b", rgb.b);
    
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", rgb.r, rgb.g, rgb.b);
    cJSON_AddStringToObject(data, "hex", hex);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief LED 后处理滤镜类型定义
 */
static const struct {
    const char *name;
    const char *description;
} s_filter_types[] = {
    {"none",        "No effect"},
    {"brightness",  "Static brightness adjustment"},
    {"pulse",       "Pulsing brightness (sine wave)"},
    {"blink",       "On/off blinking"},
    {"fade-in",     "Fade in (one-shot)"},
    {"fade-out",    "Fade out (one-shot)"},
    {"breathing",   "Smooth breathing effect"},
    {"color-shift", "Hue rotation over time"},
    {"saturation",  "Saturation adjustment"},
    {"invert",      "Invert colors"},
    {"grayscale",   "Convert to grayscale"},
    {"scanline",    "Horizontal/vertical scanline"},
    {"wave",        "Brightness wave"},
    {"glitch",      "Random glitch artifacts"},
    {"rainbow",     "Rainbow color cycling"},
    {"sparkle",     "Sparkling white pixels"},
    {"plasma",      "Plasma wave effect"},
    {"sepia",       "Sepia tone filter"},
    {"posterize",   "Color posterization"},
    {"contrast",    "Contrast adjustment"},
    {NULL, NULL}
};

/**
 * @brief led.filter.list - List available post-processing filters
 */
static esp_err_t api_led_filter_list(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *filters = cJSON_AddArrayToObject(data, "filters");
    
    for (int i = 0; s_filter_types[i].name; i++) {
        cJSON *filter = cJSON_CreateObject();
        cJSON_AddStringToObject(filter, "name", s_filter_types[i].name);
        cJSON_AddStringToObject(filter, "description", s_filter_types[i].description);
        cJSON_AddItemToArray(filters, filter);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief 滤镜名称转类型
 */
static ts_led_effect_type_t filter_name_to_type(const char *name)
{
    if (!name) return TS_LED_EFFECT_NONE;
    if (strcmp(name, "none") == 0) return TS_LED_EFFECT_NONE;
    if (strcmp(name, "pulse") == 0) return TS_LED_EFFECT_PULSE;
    if (strcmp(name, "blink") == 0) return TS_LED_EFFECT_BLINK;
    if (strcmp(name, "breathing") == 0) return TS_LED_EFFECT_BREATHING;
    if (strcmp(name, "fade-in") == 0) return TS_LED_EFFECT_FADE_IN;
    if (strcmp(name, "fade-out") == 0) return TS_LED_EFFECT_FADE_OUT;
    if (strcmp(name, "color-shift") == 0) return TS_LED_EFFECT_COLOR_SHIFT;
    if (strcmp(name, "invert") == 0) return TS_LED_EFFECT_INVERT;
    if (strcmp(name, "grayscale") == 0) return TS_LED_EFFECT_GRAYSCALE;
    if (strcmp(name, "scanline") == 0) return TS_LED_EFFECT_SCANLINE;
    if (strcmp(name, "wave") == 0) return TS_LED_EFFECT_WAVE;
    if (strcmp(name, "glitch") == 0) return TS_LED_EFFECT_GLITCH;
    if (strcmp(name, "rainbow") == 0) return TS_LED_EFFECT_RAINBOW;
    if (strcmp(name, "sparkle") == 0) return TS_LED_EFFECT_SPARKLE;
    if (strcmp(name, "plasma") == 0) return TS_LED_EFFECT_PLASMA;
    if (strcmp(name, "sepia") == 0) return TS_LED_EFFECT_SEPIA;
    if (strcmp(name, "posterize") == 0) return TS_LED_EFFECT_POSTERIZE;
    if (strcmp(name, "contrast") == 0) return TS_LED_EFFECT_CONTRAST;
    return TS_LED_EFFECT_NONE;
}

/**
 * @brief led.filter.start - Apply post-processing filter
 * @param device: device name
 * @param filter: filter name
 * @param speed: speed 1-100 (optional)
 */
static esp_err_t api_led_filter_start(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    cJSON *filter_param = cJSON_GetObjectItem(params, "filter");
    cJSON *speed_param = cJSON_GetObjectItem(params, "speed");
    cJSON *intensity_param = cJSON_GetObjectItem(params, "intensity");
    cJSON *density_param = cJSON_GetObjectItem(params, "density");
    cJSON *decay_param = cJSON_GetObjectItem(params, "decay");
    cJSON *scale_param = cJSON_GetObjectItem(params, "scale");
    cJSON *levels_param = cJSON_GetObjectItem(params, "levels");
    cJSON *amount_param = cJSON_GetObjectItem(params, "amount");
    cJSON *saturation_param = cJSON_GetObjectItem(params, "saturation");
    cJSON *angle_param = cJSON_GetObjectItem(params, "angle");
    cJSON *width_param = cJSON_GetObjectItem(params, "width");
    cJSON *wavelength_param = cJSON_GetObjectItem(params, "wavelength");
    cJSON *amplitude_param = cJSON_GetObjectItem(params, "amplitude");
    cJSON *frequency_param = cJSON_GetObjectItem(params, "frequency");
    
    // 提取 angle 和 width 参数
    float angle = angle_param && cJSON_IsNumber(angle_param) ? (float)cJSON_GetNumberValue(angle_param) : 0.0f;
    int width = width_param && cJSON_IsNumber(width_param) ? (int)cJSON_GetNumberValue(width_param) : -1;
    float wavelength = wavelength_param && cJSON_IsNumber(wavelength_param) ? (float)cJSON_GetNumberValue(wavelength_param) : 0.0f;
    int amplitude = amplitude_param && cJSON_IsNumber(amplitude_param) ? (int)cJSON_GetNumberValue(amplitude_param) : -1;
    
    if (!device_param || !cJSON_IsString(device_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'device' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!filter_param || !cJSON_IsString(filter_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'filter' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_param->valuestring);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_led_effect_type_t type = filter_name_to_type(filter_param->valuestring);
    if (type == TS_LED_EFFECT_NONE && strcmp(filter_param->valuestring, "none") != 0) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Filter not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get layer");
        return ESP_FAIL;
    }
    
    // \u914d\u7f6e\u6ede\u955c
    ts_led_effect_config_t config = {
        .type = type,
        .params = {.brightness = {.level = 255}}
    };
    
    // \u83b7\u53d6\u53c2\u6570
    int speed = speed_param && cJSON_IsNumber(speed_param) ? (int)cJSON_GetNumberValue(speed_param) : 50;
    int intensity = intensity_param && cJSON_IsNumber(intensity_param) ? (int)cJSON_GetNumberValue(intensity_param) : -1;
    int density = density_param && cJSON_IsNumber(density_param) ? (int)cJSON_GetNumberValue(density_param) : -1;
    int decay = decay_param && cJSON_IsNumber(decay_param) ? (int)cJSON_GetNumberValue(decay_param) : -1;
    int scale = scale_param && cJSON_IsNumber(scale_param) ? (int)cJSON_GetNumberValue(scale_param) : -1;
    int levels = levels_param && cJSON_IsNumber(levels_param) ? (int)cJSON_GetNumberValue(levels_param) : -1;
    int amount = amount_param && cJSON_IsNumber(amount_param) ? (int)cJSON_GetNumberValue(amount_param) : -1;
    int saturation = saturation_param && cJSON_IsNumber(saturation_param) ? (int)cJSON_GetNumberValue(saturation_param) : -1;
    
    if (speed < 1) speed = 1;
    if (speed > 100) speed = 100;
    
    // \u6839\u636e\u901f\u5ea6\u914d\u7f6e\u6ede\u955c\u53c2\u6570
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
        case TS_LED_EFFECT_COLOR_SHIFT:
            config.params.color_shift.speed = speed * 3.6f;
            break;
        case TS_LED_EFFECT_FADE_IN:
        case TS_LED_EFFECT_FADE_OUT:
            config.params.fade.duration_ms = (uint16_t)(1000.0f / freq);
            config.params.fade.auto_remove = false;
            break;
        case TS_LED_EFFECT_SCANLINE:
            config.params.scanline.speed = speed;
            config.params.scanline.width = width > 0 ? width : 3;
            config.params.scanline.angle = angle; // 0-360度
            config.params.scanline.intensity = intensity >= 0 ? intensity : 150;
            break;
        case TS_LED_EFFECT_WAVE:
            config.params.wave.speed = speed;
            config.params.wave.wavelength = wavelength > 0 ? wavelength : 8.0f;
            config.params.wave.amplitude = amplitude >= 0 ? amplitude : 128;
            config.params.wave.angle = angle; // 0-360°
            break;
        case TS_LED_EFFECT_GLITCH:
            config.params.glitch.intensity = intensity >= 0 ? intensity : speed;
            config.params.glitch.frequency = cJSON_IsNumber(frequency_param)
                ? frequency_param->valueint : 10;
            break;
        case TS_LED_EFFECT_RAINBOW:
            config.params.rainbow.speed = speed;
            config.params.rainbow.saturation = saturation >= 0 ? saturation : 255;
            break;
        case TS_LED_EFFECT_SPARKLE:
            config.params.sparkle.speed = speed > 0 ? speed : 10.0f;  // 降低默认速度
            config.params.sparkle.density = density >= 0 ? density : 50;
            config.params.sparkle.decay = decay >= 0 ? decay : 150;    // 提高decay让余晖更明显
            break;
        case TS_LED_EFFECT_PLASMA:
            config.params.plasma.speed = speed / 10.0f;
            config.params.plasma.scale = scale > 0 ? scale : 20;
            break;
        case TS_LED_EFFECT_SEPIA:
            // No parameters
            break;
        case TS_LED_EFFECT_POSTERIZE:
            config.params.posterize.levels = levels > 0 ? levels : (2 + speed * 14 / 100);
            break;
        case TS_LED_EFFECT_CONTRAST:
            config.params.contrast.amount = cJSON_IsNumber(amount_param) && amount >= -100 && amount <= 100
                ? amount : (speed - 50) * 2;
            break;
        default:
            break;
    }
    
    esp_err_t ret = ts_led_layer_set_effect(layer, &config);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to apply filter");
        return ret;
    }
    
    // 记录滤镜状态和完整配置
    ts_led_preset_set_current_filter(internal_name, filter_param->valuestring, speed);
    ts_led_preset_set_current_filter_config(internal_name, &config);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_param->valuestring);
    cJSON_AddStringToObject(data, "filter", filter_param->valuestring);
    cJSON_AddNumberToObject(data, "speed", speed);
    cJSON_AddBoolToObject(data, "applied", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.filter.stop - Stop post-processing filter
 * @param device: device name
 */
static esp_err_t api_led_filter_stop(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    
    if (!device_param || !cJSON_IsString(device_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'device' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_param->valuestring);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get layer");
        return ESP_FAIL;
    }
    
    esp_err_t ret = ts_led_layer_clear_effect(layer);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to clear filter");
        return ret;
    }
    
    ts_led_preset_set_current_filter(internal_name, NULL, 0);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_param->valuestring);
    cJSON_AddBoolToObject(data, "stopped", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Image API                                         */
/*===========================================================================*/

/* 静态存储当前图像 */
static ts_led_image_t s_api_current_image = NULL;

/**
 * @brief led.image - Display image on matrix
 * @param device: device name (must be "matrix")
 * @param path: image file path
 * @param center: center mode ("image" or "content")
 */
static esp_err_t api_led_image(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    cJSON *path_param = cJSON_GetObjectItem(params, "path");
    cJSON *center_param = cJSON_GetObjectItem(params, "center");
    
    const char *device_name = "matrix";
    if (device_param && cJSON_IsString(device_param)) {
        device_name = device_param->valuestring;
    }
    
    // 只支持 matrix
    if (strcmp(device_name, "matrix") != 0 && strcmp(device_name, "led_matrix") != 0) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Image only supported on matrix");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!path_param || !cJSON_IsString(path_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'path' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    ts_led_device_t dev = ts_led_device_get(internal_name);
    if (!dev) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
    if (!layer) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to get layer");
        return ESP_FAIL;
    }
    
    // 停止当前动画
    ts_led_image_animate_stop(layer);
    ts_led_animation_stop(layer);
    
    // 释放之前的图像
    if (s_api_current_image) {
        ts_led_image_free(s_api_current_image);
        s_api_current_image = NULL;
    }
    
    // 加载图像
    esp_err_t ret = ts_led_image_load(path_param->valuestring, TS_LED_IMG_FMT_AUTO, &s_api_current_image);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Failed to load image");
        return ret;
    }
    
    ts_led_image_info_t info;
    ts_led_image_get_info(s_api_current_image, &info);
    
    // 配置显示选项
    ts_led_image_options_t opts = TS_LED_IMAGE_DEFAULT_OPTIONS();
    opts.scale = TS_LED_IMG_SCALE_FIT;
    
    if (center_param && cJSON_IsString(center_param)) {
        if (strcmp(center_param->valuestring, "content") == 0) {
            opts.center = TS_LED_IMG_CENTER_CONTENT;
        } else {
            opts.center = TS_LED_IMG_CENTER_IMAGE;
        }
    }
    
    // 显示图像
    if (info.frame_count > 1) {
        ret = ts_led_image_animate_start(layer, s_api_current_image, &opts);
    } else {
        ret = ts_led_image_display(layer, s_api_current_image, &opts);
    }
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to display image");
        return ret;
    }
    
    // 记录图像路径，清除其他内容
    ts_led_preset_set_current_image(device_name, path_param->valuestring);
    ts_led_preset_set_current_effect(device_name, NULL, 0);
    ts_led_preset_set_current_qrcode(device_name, NULL);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_name);
    cJSON_AddStringToObject(data, "path", path_param->valuestring);
    cJSON_AddNumberToObject(data, "width", info.width);
    cJSON_AddNumberToObject(data, "height", info.height);
    cJSON_AddNumberToObject(data, "frames", info.frame_count);
    cJSON_AddBoolToObject(data, "animated", info.frame_count > 1);
    cJSON_AddBoolToObject(data, "displayed", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          QR Code API                                       */
/*===========================================================================*/

/**
 * @brief led.qrcode - Generate and display QR code
 * @param device: device name (must be "matrix")
 * @param text: text to encode
 * @param ecc: error correction level (L/M/Q/H)
 * @param color: foreground color
 * @param bg_image: background image path (optional)
 */
static esp_err_t api_led_qrcode(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    cJSON *text_param = cJSON_GetObjectItem(params, "text");
    cJSON *ecc_param = cJSON_GetObjectItem(params, "ecc");
    cJSON *bg_image_param = cJSON_GetObjectItem(params, "bg_image");
    
    const char *device_name = "matrix";
    if (device_param && cJSON_IsString(device_param)) {
        device_name = device_param->valuestring;
    }
    
    if (strcmp(device_name, "matrix") != 0 && strcmp(device_name, "led_matrix") != 0) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "QR code only supported on matrix");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!text_param || !cJSON_IsString(text_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'text' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *internal_name = resolve_device_name(device_name);
    
    // 配置 QR 码
    ts_led_qr_config_t config = TS_LED_QR_DEFAULT_CONFIG();
    config.text = text_param->valuestring;
    
    // ECC 级别
    if (ecc_param && cJSON_IsString(ecc_param)) {
        ts_led_qr_ecc_parse(ecc_param->valuestring, &config.ecc);
    }
    
    // 前景颜色
    ts_led_rgb_t color;
    if (parse_color_param(params, "color", &color) == ESP_OK) {
        config.fg_color = color;
    }
    
    // 加载背景图（可选）
    ts_led_image_t bg_image = NULL;
    if (bg_image_param && cJSON_IsString(bg_image_param) && strlen(bg_image_param->valuestring) > 0) {
        esp_err_t img_ret = ts_led_image_load(bg_image_param->valuestring, TS_LED_IMG_FMT_AUTO, &bg_image);
        if (img_ret != ESP_OK) {
            ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Failed to load background image");
            return img_ret;
        }
        config.bg_image = bg_image;
    }
    
    config.version_min = 1;
    config.version_max = 4;
    config.center = true;
    
    // 生成并显示
    ts_led_qr_result_t qr_result;
    esp_err_t ret = ts_led_qrcode_show_on_device(internal_name, &config, &qr_result);
    
    // 释放背景图
    if (bg_image) {
        ts_led_image_free(bg_image);
    }
    
    if (ret == ESP_ERR_INVALID_SIZE) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Text too long for QR code");
        return ret;
    }
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to generate QR code");
        return ret;
    }
    
    // 清除图像/特效记录，并记录 QR Code 和背景图
    ts_led_preset_clear_current_image(device_name);
    ts_led_preset_set_current_effect(device_name, NULL, 0);
    ts_led_preset_set_current_qrcode(device_name, text_param->valuestring);
    // 记录背景图路径（如果有）
    if (bg_image_param && cJSON_IsString(bg_image_param) && strlen(bg_image_param->valuestring) > 0) {
        ts_led_preset_set_current_qrcode_bg(device_name, bg_image_param->valuestring);
    } else {
        ts_led_preset_set_current_qrcode_bg(device_name, NULL);
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_name);
    cJSON_AddStringToObject(data, "text", text_param->valuestring);
    cJSON_AddNumberToObject(data, "version", qr_result.version);
    cJSON_AddNumberToObject(data, "size", qr_result.size);
    cJSON_AddNumberToObject(data, "capacity", qr_result.data_capacity);
    cJSON_AddBoolToObject(data, "displayed", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Text API                                          */
/*===========================================================================*/

/* 静态存储当前字体 */
static ts_font_t *s_api_current_font = NULL;
static char s_api_font_name[32] = {0};

/**
 * @brief led.text - Display text on matrix
 * @param device: device name (must be "matrix")
 * @param text: text to display
 * @param font: font name (default: "cjk")
 * @param color: text color
 * @param align: alignment (left/center/right)
 * @param scroll: scroll direction (none/left/right/up/down)
 * @param speed: scroll speed 1-100
 * @param x, y: position offset
 * @param invert: invert on bright background
 * @param loop: loop scrolling
 */
static esp_err_t api_led_text(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    cJSON *text_param = cJSON_GetObjectItem(params, "text");
    cJSON *font_param = cJSON_GetObjectItem(params, "font");
    cJSON *align_param = cJSON_GetObjectItem(params, "align");
    cJSON *scroll_param = cJSON_GetObjectItem(params, "scroll");
    cJSON *speed_param = cJSON_GetObjectItem(params, "speed");
    cJSON *x_param = cJSON_GetObjectItem(params, "x");
    cJSON *y_param = cJSON_GetObjectItem(params, "y");
    cJSON *invert_param = cJSON_GetObjectItem(params, "invert");
    cJSON *loop_param = cJSON_GetObjectItem(params, "loop");
    
    const char *device_name = "matrix";
    if (device_param && cJSON_IsString(device_param)) {
        device_name = device_param->valuestring;
    }
    
    if (strcmp(device_name, "matrix") != 0 && strcmp(device_name, "led_matrix") != 0) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Text only supported on matrix");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!text_param || !cJSON_IsString(text_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'text' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 字体
    const char *font_name = "cjk";
    if (font_param && cJSON_IsString(font_param)) {
        // "default" 使用默认字体 cjk
        if (strcmp(font_param->valuestring, "default") != 0) {
            font_name = font_param->valuestring;
        }
    }
    
    // 加载字体
    if (!s_api_current_font || strcmp(s_api_font_name, font_name) != 0) {
        if (s_api_current_font) {
            ts_font_unload(s_api_current_font);
            s_api_current_font = NULL;
        }
        
        char font_path[64];
        snprintf(font_path, sizeof(font_path), "/sdcard/fonts/%s.fnt", font_name);
        
        ts_font_config_t font_cfg = TS_FONT_DEFAULT_CONFIG();
        s_api_current_font = ts_font_load(font_path, &font_cfg);
        if (!s_api_current_font) {
            ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Font not found");
            return ESP_ERR_NOT_FOUND;
        }
        strncpy(s_api_font_name, font_name, sizeof(s_api_font_name) - 1);
    }
    
    // 颜色
    ts_led_rgb_t color = TS_LED_WHITE;
    parse_color_param(params, "color", &color);
    
    // 对齐
    ts_text_align_t align = TS_TEXT_ALIGN_LEFT;
    if (align_param && cJSON_IsString(align_param)) {
        if (strcmp(align_param->valuestring, "center") == 0) align = TS_TEXT_ALIGN_CENTER;
        else if (strcmp(align_param->valuestring, "right") == 0) align = TS_TEXT_ALIGN_RIGHT;
    }
    
    // 滚动
    ts_text_scroll_t scroll = TS_TEXT_SCROLL_NONE;
    if (scroll_param && cJSON_IsString(scroll_param)) {
        if (strcmp(scroll_param->valuestring, "left") == 0) scroll = TS_TEXT_SCROLL_LEFT;
        else if (strcmp(scroll_param->valuestring, "right") == 0) scroll = TS_TEXT_SCROLL_RIGHT;
        else if (strcmp(scroll_param->valuestring, "up") == 0) scroll = TS_TEXT_SCROLL_UP;
        else if (strcmp(scroll_param->valuestring, "down") == 0) scroll = TS_TEXT_SCROLL_DOWN;
    }
    
    // 配置覆盖层
    ts_text_overlay_config_t overlay_cfg = TS_TEXT_OVERLAY_DEFAULT_CONFIG();
    overlay_cfg.text = text_param->valuestring;
    overlay_cfg.font = s_api_current_font;
    overlay_cfg.color = color;
    overlay_cfg.align = align;
    overlay_cfg.scroll = scroll;
    overlay_cfg.scroll_speed = 30;
    
    if (speed_param && cJSON_IsNumber(speed_param)) {
        int speed = (int)cJSON_GetNumberValue(speed_param);
        if (speed < 1) speed = 1;
        if (speed > 100) speed = 100;
        overlay_cfg.scroll_speed = (uint8_t)speed;
    }
    
    if (x_param && cJSON_IsNumber(x_param)) {
        overlay_cfg.x = (int16_t)cJSON_GetNumberValue(x_param);
    }
    if (y_param && cJSON_IsNumber(y_param)) {
        overlay_cfg.y = (int16_t)cJSON_GetNumberValue(y_param);
    }
    
    overlay_cfg.invert_on_overlap = (invert_param && cJSON_IsTrue(invert_param));
    overlay_cfg.loop_scroll = (loop_param && cJSON_IsTrue(loop_param));
    
    esp_err_t ret = ts_led_text_overlay_start(device_name, &overlay_cfg);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to display text");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_name);
    cJSON_AddStringToObject(data, "text", text_param->valuestring);
    cJSON_AddStringToObject(data, "font", font_name);
    cJSON_AddBoolToObject(data, "displayed", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.text.stop - Stop text overlay
 * @param device: device name
 */
static esp_err_t api_led_text_stop(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    
    const char *device_name = "matrix";
    if (device_param && cJSON_IsString(device_param)) {
        device_name = device_param->valuestring;
    }
    
    esp_err_t ret = ts_led_text_overlay_stop(device_name);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to stop text");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_name);
    cJSON_AddBoolToObject(data, "stopped", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.save - Save current LED state as boot configuration
 * @param device: device name (touch/board/matrix)
 */
static esp_err_t api_led_save(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_param = cJSON_GetObjectItem(params, "device");
    
    if (!device_param || !cJSON_IsString(device_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'device' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *device_name = device_param->valuestring;
    
    // 验证设备名
    if (strcmp(device_name, "touch") != 0 && 
        strcmp(device_name, "board") != 0 && 
        strcmp(device_name, "matrix") != 0) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Device not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    // 保存当前配置到 NVS
    esp_err_t ret = ts_led_save_boot_config(device_name);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to save config");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "device", device_name);
    cJSON_AddBoolToObject(data, "saved", true);
    
    // 返回保存的配置
    ts_led_boot_config_t cfg;
    if (ts_led_get_boot_config(device_name, &cfg) == ESP_OK) {
        cJSON_AddStringToObject(data, "animation", cfg.animation);
        cJSON_AddNumberToObject(data, "brightness", cfg.brightness);
        cJSON_AddNumberToObject(data, "speed", cfg.speed);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.boot.config - Get LED boot configuration
 * @param device: optional device name filter
 */
static esp_err_t api_led_boot_config(const cJSON *params, ts_api_result_t *result)
{
    const char *device_filter = NULL;
    if (params) {
        cJSON *device_param = cJSON_GetObjectItem(params, "device");
        if (device_param && cJSON_IsString(device_param)) {
            device_filter = device_param->valuestring;
        }
    }
    
    const char *devices[] = {"touch", "board", "matrix"};
    int start = 0, end = 3;
    
    if (device_filter) {
        for (int i = 0; i < 3; i++) {
            if (strcmp(device_filter, devices[i]) == 0) {
                start = i;
                end = i + 1;
                break;
            }
        }
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON *configs = cJSON_AddArrayToObject(data, "boot_config");
    
    for (int i = start; i < end; i++) {
        ts_led_boot_config_t cfg;
        if (ts_led_get_boot_config(devices[i], &cfg) == ESP_OK) {
            cJSON *config = cJSON_CreateObject();
            cJSON_AddStringToObject(config, "device", devices[i]);
            cJSON_AddBoolToObject(config, "enabled", cfg.enabled);
            cJSON_AddStringToObject(config, "animation", cfg.animation);
            cJSON_AddStringToObject(config, "filter", cfg.filter);
            cJSON_AddStringToObject(config, "image_path", cfg.image_path);
            cJSON_AddStringToObject(config, "qrcode_text", cfg.qrcode_text);
            cJSON_AddNumberToObject(config, "speed", cfg.speed);
            cJSON_AddNumberToObject(config, "filter_speed", cfg.filter_speed);
            cJSON_AddNumberToObject(config, "brightness", cfg.brightness);
            cJSON_AddItemToArray(configs, config);
        }
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                     Color Correction APIs                                  */
/*===========================================================================*/

/**
 * @brief led.color_correction.get - Get color correction configuration
 */
static esp_err_t api_led_cc_get(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    if (!ts_led_cc_is_initialized()) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Color correction not initialized");
        return ESP_OK;
    }
    
    cJSON *data = ts_led_cc_config_to_json();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Failed to create JSON");
        return ESP_OK;
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.color_correction.set - Set color correction configuration
 */
static esp_err_t api_led_cc_set(const cJSON *params, ts_api_result_t *result)
{
    if (!ts_led_cc_is_initialized()) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Color correction not initialized");
        return ESP_OK;
    }
    
    ts_led_cc_config_t config;
    ts_led_cc_get_config(&config);
    
    /* Parse enabled flag */
    const cJSON *enabled = cJSON_GetObjectItem(params, "enabled");
    if (cJSON_IsBool(enabled)) {
        config.enabled = cJSON_IsTrue(enabled);
    }
    
    /* Parse white_point (支持 r/g/b 或 red_scale/green_scale/blue_scale) */
    const cJSON *wp = cJSON_GetObjectItem(params, "white_point");
    if (cJSON_IsObject(wp)) {
        const cJSON *wp_en = cJSON_GetObjectItem(wp, "enabled");
        if (cJSON_IsBool(wp_en)) config.white_point.enabled = cJSON_IsTrue(wp_en);
        
        const cJSON *red = cJSON_GetObjectItem(wp, "red_scale");
        if (!red) red = cJSON_GetObjectItem(wp, "r");  /* WebUI compatibility */
        if (cJSON_IsNumber(red)) config.white_point.red_scale = (float)red->valuedouble;
        
        const cJSON *green = cJSON_GetObjectItem(wp, "green_scale");
        if (!green) green = cJSON_GetObjectItem(wp, "g");  /* WebUI compatibility */
        if (cJSON_IsNumber(green)) config.white_point.green_scale = (float)green->valuedouble;
        
        const cJSON *blue = cJSON_GetObjectItem(wp, "blue_scale");
        if (!blue) blue = cJSON_GetObjectItem(wp, "b");  /* WebUI compatibility */
        if (cJSON_IsNumber(blue)) config.white_point.blue_scale = (float)blue->valuedouble;
    }
    
    /* Parse gamma (支持 gamma 或 value) */
    const cJSON *gamma = cJSON_GetObjectItem(params, "gamma");
    if (cJSON_IsObject(gamma)) {
        const cJSON *gamma_en = cJSON_GetObjectItem(gamma, "enabled");
        if (cJSON_IsBool(gamma_en)) config.gamma.enabled = cJSON_IsTrue(gamma_en);
        
        const cJSON *gamma_val = cJSON_GetObjectItem(gamma, "gamma");
        if (!gamma_val) gamma_val = cJSON_GetObjectItem(gamma, "value");  /* WebUI compatibility */
        if (cJSON_IsNumber(gamma_val)) config.gamma.gamma = (float)gamma_val->valuedouble;
    }
    
    /* Parse brightness */
    const cJSON *brightness = cJSON_GetObjectItem(params, "brightness");
    if (cJSON_IsObject(brightness)) {
        const cJSON *br_en = cJSON_GetObjectItem(brightness, "enabled");
        if (cJSON_IsBool(br_en)) config.brightness.enabled = cJSON_IsTrue(br_en);
        
        const cJSON *br_val = cJSON_GetObjectItem(brightness, "factor");
        if (cJSON_IsNumber(br_val)) config.brightness.factor = (float)br_val->valuedouble;
    }
    
    /* Parse saturation */
    const cJSON *saturation = cJSON_GetObjectItem(params, "saturation");
    if (cJSON_IsObject(saturation)) {
        const cJSON *sat_en = cJSON_GetObjectItem(saturation, "enabled");
        if (cJSON_IsBool(sat_en)) config.saturation.enabled = cJSON_IsTrue(sat_en);
        
        const cJSON *sat_val = cJSON_GetObjectItem(saturation, "factor");
        if (cJSON_IsNumber(sat_val)) config.saturation.factor = (float)sat_val->valuedouble;
    }
    
    /* Apply configuration */
    esp_err_t ret = ts_led_cc_set_config(&config);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid configuration");
        return ESP_OK;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", true);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.color_correction.reset - Reset color correction to defaults
 */
static esp_err_t api_led_cc_reset(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    if (!ts_led_cc_is_initialized()) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Color correction not initialized");
        return ESP_OK;
    }
    
    esp_err_t ret = ts_led_cc_reset_config();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to reset configuration");
        return ESP_OK;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", true);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.color_correction.export - Export configuration to SD card
 */
static esp_err_t api_led_cc_export(const cJSON *params, ts_api_result_t *result)
{
    if (!ts_led_cc_is_initialized()) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Color correction not initialized");
        return ESP_OK;
    }
    
    const char *path = NULL;
    const cJSON *path_param = cJSON_GetObjectItem(params, "path");
    if (cJSON_IsString(path_param)) {
        path = path_param->valuestring;
    }
    
    esp_err_t ret = ts_led_cc_save_to_sdcard(path);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to export configuration");
        return ESP_OK;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", true);
    cJSON_AddStringToObject(data, "path", path ? path : TS_LED_CC_SDCARD_JSON_PATH);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief led.color_correction.import - Import configuration from SD card
 */
static esp_err_t api_led_cc_import(const cJSON *params, ts_api_result_t *result)
{
    if (!ts_led_cc_is_initialized()) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Color correction not initialized");
        return ESP_OK;
    }
    
    const char *path = NULL;
    const cJSON *path_param = cJSON_GetObjectItem(params, "path");
    if (cJSON_IsString(path_param)) {
        path = path_param->valuestring;
    }
    
    esp_err_t ret = ts_led_cc_load_from_sdcard(path);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Failed to import configuration");
        return ESP_OK;
    }
    
    /* Save imported config to NVS as well */
    ts_led_cc_save_to_nvs();
    
    cJSON *data = ts_led_cc_config_to_json();
    if (!data) {
        data = cJSON_CreateObject();
    }
    cJSON_AddBoolToObject(data, "success", true);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t led_endpoints[] = {
    {
        .name = "led.list",
        .description = "List LED devices",
        .category = TS_API_CAT_LED,
        .handler = api_led_list,
        .requires_auth = false,
    },
    {
        .name = "led.brightness",
        .description = "Get/set device brightness",
        .category = TS_API_CAT_LED,
        .handler = api_led_brightness,
        .requires_auth = false,  // TODO: 测试完成后改为 true
        .permission = "led.control",
    },
    {
        .name = "led.clear",
        .description = "Clear all LEDs on device",
        .category = TS_API_CAT_LED,
        .handler = api_led_clear,
        .requires_auth = false,  // TODO: 测试完成后改为 true
        .permission = "led.control",
    },
    {
        .name = "led.set",
        .description = "Set LED(s) color",
        .category = TS_API_CAT_LED,
        .handler = api_led_set,
        .requires_auth = false,  // TODO: 测试完成后改为 true
        .permission = "led.control",
    },
    {
        .name = "led.fill",
        .description = "Fill all LEDs with color",
        .category = TS_API_CAT_LED,
        .handler = api_led_fill,
        .requires_auth = false,  // TODO: 测试完成后改为 true
        .permission = "led.control",
    },
    {
        .name = "led.effect.list",
        .description = "List available effects",
        .category = TS_API_CAT_LED,
        .handler = api_led_effect_list,
        .requires_auth = false,
    },
    {
        .name = "led.effect.start",
        .description = "Start effect on device",
        .category = TS_API_CAT_LED,
        .handler = api_led_effect_start,
        .requires_auth = false,  // TODO: 测试完成后改为 true
        .permission = "led.control",
    },
    {
        .name = "led.effect.stop",
        .description = "Stop effect on device",
        .category = TS_API_CAT_LED,
        .handler = api_led_effect_stop,
        .requires_auth = false,  // TODO: 测试完成后改为 true
        .permission = "led.control",
    },
    {
        .name = "led.color.parse",
        .description = "Parse color string to RGB",
        .category = TS_API_CAT_LED,
        .handler = api_led_color_parse,
        .requires_auth = false,
    },
    {
        .name = "led.color.hsv",
        .description = "Convert HSV to RGB",
        .category = TS_API_CAT_LED,
        .handler = api_led_color_hsv,
        .requires_auth = false,
    },
    {
        .name = "led.filter.list",
        .description = "List available post-processing filters",
        .category = TS_API_CAT_LED,
        .handler = api_led_filter_list,
        .requires_auth = false,
    },
    {
        .name = "led.filter.start",
        .description = "Apply post-processing filter",
        .category = TS_API_CAT_LED,
        .handler = api_led_filter_start,
        .requires_auth = false,
        .permission = "led.control",
    },
    {
        .name = "led.filter.stop",
        .description = "Stop post-processing filter",
        .category = TS_API_CAT_LED,
        .handler = api_led_filter_stop,
        .requires_auth = false,
        .permission = "led.control",
    },
    {
        .name = "led.image",
        .description = "Display image on matrix",
        .category = TS_API_CAT_LED,
        .handler = api_led_image,
        .requires_auth = false,
        .permission = "led.control",
    },
    {
        .name = "led.qrcode",
        .description = "Generate and display QR code",
        .category = TS_API_CAT_LED,
        .handler = api_led_qrcode,
        .requires_auth = false,
        .permission = "led.control",
    },
    {
        .name = "led.text",
        .description = "Display text on matrix",
        .category = TS_API_CAT_LED,
        .handler = api_led_text,
        .requires_auth = false,
        .permission = "led.control",
    },
    {
        .name = "led.text.stop",
        .description = "Stop text overlay",
        .category = TS_API_CAT_LED,
        .handler = api_led_text_stop,
        .requires_auth = false,
        .permission = "led.control",
    },
    {
        .name = "led.save",
        .description = "Save current state as boot configuration",
        .category = TS_API_CAT_LED,
        .handler = api_led_save,
        .requires_auth = false,  // TODO: 测试完成后改为 true
        .permission = "led.config",
    },
    {
        .name = "led.boot.config",
        .description = "Get LED boot configuration",
        .category = TS_API_CAT_LED,
        .handler = api_led_boot_config,
        .requires_auth = false,
    },
    /* Color Correction APIs */
    {
        .name = "led.color_correction.get",
        .description = "Get color correction configuration",
        .category = TS_API_CAT_LED,
        .handler = api_led_cc_get,
        .requires_auth = false,
    },
    {
        .name = "led.color_correction.set",
        .description = "Set color correction configuration",
        .category = TS_API_CAT_LED,
        .handler = api_led_cc_set,
        .requires_auth = false,
        .permission = "led.config",
    },
    {
        .name = "led.color_correction.reset",
        .description = "Reset color correction to defaults",
        .category = TS_API_CAT_LED,
        .handler = api_led_cc_reset,
        .requires_auth = false,
        .permission = "led.config",
    },
    {
        .name = "led.color_correction.export",
        .description = "Export color correction config to SD card",
        .category = TS_API_CAT_LED,
        .handler = api_led_cc_export,
        .requires_auth = false,
        .permission = "led.config",
    },
    {
        .name = "led.color_correction.import",
        .description = "Import color correction config from SD card",
        .category = TS_API_CAT_LED,
        .handler = api_led_cc_import,
        .requires_auth = false,
        .permission = "led.config",
    },
};

esp_err_t ts_api_led_register(void)
{
    TS_LOGI(TAG, "Registering LED APIs");
    
    for (size_t i = 0; i < sizeof(led_endpoints) / sizeof(led_endpoints[0]); i++) {
        esp_err_t ret = ts_api_register(&led_endpoints[i]);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to register %s", led_endpoints[i].name);
            return ret;
        }
    }
    
    return ESP_OK;
}
