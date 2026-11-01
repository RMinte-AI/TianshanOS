/**
 * @file ts_led_preset.c
 * @brief Preset LED Device Instances
 */

#include "ts_led_preset.h"
#include "ts_led.h"
#include "ts_led_effect.h"
#include "ts_led_image.h"
#include "ts_led_qrcode.h"
#include "ts_pin_manager.h"
#include "ts_log.h"
#include "ts_event.h"
#include "ts_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/stat.h>

#define TAG "led_preset"

// NVS 命名空间（专门用于 LED 配置，避免与 ts_config 冲突）
#define LED_NVS_NAMESPACE "led_boot"

static ts_led_device_t s_touch = NULL;
static ts_led_device_t s_board = NULL;
static ts_led_device_t s_matrix = NULL;
static ts_led_layer_t s_status_layer = NULL;

esp_err_t ts_led_touch_init(void)
{
    // 暂时硬编码 GPIO，绕过 pin_manager (NVS 中可能有错误配置)
    // TODO: 清除 NVS 后恢复使用 pin_manager
    int gpio = 45;  // RM01 Touch LED GPIO
    TS_LOGI(TAG, "Touch LED: using hardcoded GPIO %d", gpio);
    
    ts_led_config_t cfg = TS_LED_DEFAULT_CONFIG();
    cfg.name = TS_LED_TOUCH_NAME;
    cfg.gpio_pin = gpio;
#ifdef CONFIG_TS_LED_TOUCH_COUNT
    cfg.led_count = CONFIG_TS_LED_TOUCH_COUNT;
#else
    cfg.led_count = 1;  // RM01: Single WS2812 touch indicator
#endif
#ifdef CONFIG_TS_LED_TOUCH_DEFAULT_BRIGHTNESS
    cfg.brightness = CONFIG_TS_LED_TOUCH_DEFAULT_BRIGHTNESS;
#else
    cfg.brightness = 80;
#endif
    cfg.layout = TS_LED_LAYOUT_STRIP;
    
    esp_err_t ret = ts_led_device_create(&cfg, &s_touch);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Touch LED initialized: %d LEDs on GPIO %d", cfg.led_count, gpio);
    }
    return ret;
}

esp_err_t ts_led_board_init(void)
{
    // 暂时硬编码 GPIO，绕过 pin_manager (NVS 中可能有错误配置)
    int gpio = 42;  // RM01 Board LED GPIO
    TS_LOGI(TAG, "Board LED: using hardcoded GPIO %d", gpio);
    
    ts_led_config_t cfg = TS_LED_DEFAULT_CONFIG();
    cfg.name = TS_LED_BOARD_NAME;
    cfg.gpio_pin = gpio;
#ifdef CONFIG_TS_LED_BOARD_COUNT
    cfg.led_count = CONFIG_TS_LED_BOARD_COUNT;
#else
    cfg.led_count = 28;  // RM01: 28 LED strip
#endif
#ifdef CONFIG_TS_LED_BOARD_DEFAULT_BRIGHTNESS
    cfg.brightness = CONFIG_TS_LED_BOARD_DEFAULT_BRIGHTNESS;
#else
    cfg.brightness = 60;
#endif
    cfg.layout = TS_LED_LAYOUT_STRIP;
    
    esp_err_t ret = ts_led_device_create(&cfg, &s_board);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Board LED initialized: %d LEDs on GPIO %d", cfg.led_count, gpio);
    }
    return ret;
}

esp_err_t ts_led_matrix_init(void)
{
    int gpio = ts_pin_manager_get_gpio(TS_PIN_FUNC_LED_MATRIX);
    TS_LOGI(TAG, "Matrix LED: pin_manager returned GPIO %d", gpio);
    if (gpio < 0 || gpio >= 46) {
        gpio = 9;  // Fallback to valid GPIO
        TS_LOGW(TAG, "Using fallback GPIO %d for Matrix LED", gpio);
    }
    
    ts_led_config_t cfg = TS_LED_DEFAULT_CONFIG();
    cfg.name = TS_LED_MATRIX_NAME;
    cfg.gpio_pin = gpio;
    cfg.layout = TS_LED_LAYOUT_MATRIX;
    
#ifdef CONFIG_TS_LED_MATRIX_WIDTH
    cfg.width = CONFIG_TS_LED_MATRIX_WIDTH;
#else
    cfg.width = 32;  // RM01: 32x32 matrix
#endif
#ifdef CONFIG_TS_LED_MATRIX_HEIGHT
    cfg.height = CONFIG_TS_LED_MATRIX_HEIGHT;
#else
    cfg.height = 32;  // RM01: 32x32 matrix
#endif
    cfg.led_count = cfg.width * cfg.height;
    cfg.scan = TS_LED_SCAN_ROWS;  // 非蛇形布线，所有行从左到右
    
#ifdef CONFIG_TS_LED_MATRIX_DEFAULT_BRIGHTNESS
    cfg.brightness = CONFIG_TS_LED_MATRIX_DEFAULT_BRIGHTNESS;
#else
    cfg.brightness = 50;
#endif
    
    esp_err_t ret = ts_led_device_create(&cfg, &s_matrix);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Matrix LED initialized: %dx%d on GPIO %d", 
                cfg.width, cfg.height, gpio);
    }
    return ret;
}

esp_err_t ts_led_preset_init_all(void)
{
    esp_err_t ret;
    
    ret = ts_led_touch_init();
    if (ret != ESP_OK) TS_LOGW(TAG, "Touch LED init failed: %s", esp_err_to_name(ret));
    
    ret = ts_led_board_init();
    if (ret != ESP_OK) TS_LOGW(TAG, "Board LED init failed: %s", esp_err_to_name(ret));
    
    ret = ts_led_matrix_init();
    if (ret != ESP_OK) TS_LOGW(TAG, "Matrix LED init failed: %s", esp_err_to_name(ret));
    
    return ESP_OK;
}

ts_led_device_t ts_led_touch_get(void) { return s_touch; }
ts_led_device_t ts_led_board_get(void) { return s_board; }
ts_led_device_t ts_led_matrix_get(void) { return s_matrix; }

/* Status indicator colors */
static const ts_led_rgb_t s_status_colors[] = {
    [TS_LED_STATUS_IDLE]    = {0, 0, 64},
    [TS_LED_STATUS_BUSY]    = {64, 64, 0},
    [TS_LED_STATUS_SUCCESS] = {0, 64, 0},
    [TS_LED_STATUS_ERROR]   = {64, 0, 0},
    [TS_LED_STATUS_WARNING] = {64, 32, 0},
    [TS_LED_STATUS_NETWORK] = {0, 32, 64},
    [TS_LED_STATUS_USB]     = {32, 0, 64},
    [TS_LED_STATUS_BOOT]    = {64, 64, 64},
};

esp_err_t ts_led_set_status(ts_led_status_t status)
{
    if (status >= TS_LED_STATUS_MAX) return ESP_ERR_INVALID_ARG;
    
    ts_led_device_t dev = ts_led_touch_get();
    if (!dev) return ESP_ERR_INVALID_STATE;
    
    if (!s_status_layer) {
        ts_led_layer_config_t cfg = TS_LED_LAYER_DEFAULT_CONFIG();
        ts_led_layer_create(dev, &cfg, &s_status_layer);
    }
    
    if (s_status_layer) {
        ts_led_fill(s_status_layer, s_status_colors[status]);
    }
    
    return ESP_OK;
}

esp_err_t ts_led_clear_status(void)
{
    return ts_led_set_status(TS_LED_STATUS_IDLE);
}

esp_err_t ts_led_bind_event_status(uint32_t event_id, ts_led_status_t status,
                                    uint32_t duration_ms)
{
    return ESP_ERR_NOT_SUPPORTED;
}

/*===========================================================================*/
/*                          Boot Configuration                                */
/*===========================================================================*/

/* 当前运行的动画名称、速度和颜色（每设备一个） */
static char s_current_animation[3][32] = {{0}, {0}, {0}};
static char s_current_filter[3][32] = {{0}, {0}, {0}};
static uint8_t s_current_speed[3] = {0, 0, 0};
static uint8_t s_current_filter_speed[3] = {50, 50, 50};
static ts_led_rgb_t s_current_color[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
static bool s_current_color_valid[3] = {false, false, false};
static char s_current_image_path[3][128] = {{0}, {0}, {0}};
static char s_current_qrcode[3][128] = {{0}, {0}, {0}};
static char s_current_qrcode_bg[3][128] = {{0}, {0}, {0}};
static char s_current_text[3][64] = {{0}, {0}, {0}};

/* 滤镜配置存储（每设备一个） */
static ts_led_effect_config_t s_current_filter_config[3] = {0};

/* 延迟加载图像的定时器 */
static esp_timer_handle_t s_delayed_image_timer = NULL;
static char s_delayed_image_path[128] = {0};
static int s_delayed_device_idx = -1;
static uint8_t s_delayed_brightness = 128;

/**
 * @brief 根据 filter 名称和保存的参数应用后处理效果到 layer
 */
static void apply_filter_to_layer_with_params(ts_led_layer_t layer, const char *filter, int dev_idx)
{
    if (!layer || !filter || !filter[0] || dev_idx < 0 || dev_idx >= 3) return;
    
    ts_led_effect_config_t cfg = s_current_filter_config[dev_idx];  // 使用保存的配置
    if (cfg.type != TS_LED_EFFECT_NONE) {
        ts_led_layer_set_effect(layer, &cfg);
        return;
    }
    cfg.type = TS_LED_EFFECT_NONE;  // 先重置 type
    
    if (strcmp(filter, "pulse") == 0) {
        cfg.type = TS_LED_EFFECT_PULSE;
        cfg.params.pulse.frequency = 0.5f;
        cfg.params.pulse.min_level = 20;
        cfg.params.pulse.max_level = 255;
    } else if (strcmp(filter, "blink") == 0) {
        cfg.type = TS_LED_EFFECT_BLINK;
        cfg.params.blink.on_time_ms = 500;
        cfg.params.blink.off_time_ms = 500;
    } else if (strcmp(filter, "breathing") == 0) {
        cfg.type = TS_LED_EFFECT_BREATHING;
        cfg.params.breathing.frequency = 0.3f;
        cfg.params.breathing.min_level = 10;
        cfg.params.breathing.max_level = 255;
    } else if (strcmp(filter, "color-shift") == 0) {
        cfg.type = TS_LED_EFFECT_COLOR_SHIFT;
        cfg.params.color_shift.speed = 90.0f;
    } else if (strcmp(filter, "scanline") == 0) {
        cfg.type = TS_LED_EFFECT_SCANLINE;
        // 使用保存的参数，如果参数为0则使用默认值
        if (cfg.params.scanline.speed == 0) cfg.params.scanline.speed = 50.0f;
        if (cfg.params.scanline.width == 0) cfg.params.scanline.width = 3;
        if (cfg.params.scanline.intensity == 0) cfg.params.scanline.intensity = 200;
    } else if (strcmp(filter, "wave") == 0) {
        cfg.type = TS_LED_EFFECT_WAVE;
        if (cfg.params.wave.speed == 0) cfg.params.wave.speed = 50.0f;
        if (cfg.params.wave.wavelength == 0) cfg.params.wave.wavelength = 8.0f;
        if (cfg.params.wave.amplitude == 0) cfg.params.wave.amplitude = 128;
    } else if (strcmp(filter, "glitch") == 0) {
        cfg.type = TS_LED_EFFECT_GLITCH;
        if (cfg.params.glitch.intensity == 0) cfg.params.glitch.intensity = 50;
        if (cfg.params.glitch.frequency == 0) cfg.params.glitch.frequency = 10;
    } else if (strcmp(filter, "grayscale") == 0) {
        cfg.type = TS_LED_EFFECT_GRAYSCALE;
    } else if (strcmp(filter, "invert") == 0) {
        cfg.type = TS_LED_EFFECT_INVERT;
    } else if (strcmp(filter, "rainbow") == 0) {
        cfg.type = TS_LED_EFFECT_RAINBOW;
        if (cfg.params.rainbow.speed == 0) cfg.params.rainbow.speed = 50.0f;
        if (cfg.params.rainbow.saturation == 0) cfg.params.rainbow.saturation = 255;
    } else if (strcmp(filter, "sparkle") == 0) {
        cfg.type = TS_LED_EFFECT_SPARKLE;
        if (cfg.params.sparkle.density == 0) cfg.params.sparkle.density = 50;
        if (cfg.params.sparkle.decay == 0) cfg.params.sparkle.decay = 230;
    } else if (strcmp(filter, "plasma") == 0) {
        cfg.type = TS_LED_EFFECT_PLASMA;
        if (cfg.params.plasma.speed == 0) cfg.params.plasma.speed = 5.0f;
        if (cfg.params.plasma.scale == 0) cfg.params.plasma.scale = 20;
    } else if (strcmp(filter, "sepia") == 0) {
        cfg.type = TS_LED_EFFECT_SEPIA;
    } else if (strcmp(filter, "posterize") == 0) {
        cfg.type = TS_LED_EFFECT_POSTERIZE;
        if (cfg.params.posterize.levels == 0) cfg.params.posterize.levels = 4;
    } else if (strcmp(filter, "contrast") == 0) {
        cfg.type = TS_LED_EFFECT_CONTRAST;
        if (cfg.params.contrast.amount == 0) cfg.params.contrast.amount = 50;
    }
    
    if (cfg.type != TS_LED_EFFECT_NONE) {
        ts_led_layer_set_effect(layer, &cfg);
    }
}

/* 延迟加载图像的回调 */
static void delayed_image_load_callback(void *arg)
{
    (void)arg;
    
    if (s_delayed_device_idx < 0 || !s_delayed_image_path[0]) {
        return;
    }
    
    ts_led_device_t dev = NULL;
    const char *device_name = NULL;
    switch (s_delayed_device_idx) {
        case 0: dev = s_touch; device_name = "touch"; break;
        case 1: dev = s_board; device_name = "board"; break;
        case 2: dev = s_matrix; device_name = "matrix"; break;
        default: return;
    }
    
    if (!dev) return;
    
    // 检查文件是否存在
    struct stat st;
    if (stat(s_delayed_image_path, &st) != 0) {
        TS_LOGW(TAG, "Delayed load: file still not available: %s", s_delayed_image_path);
        s_delayed_image_path[0] = '\0';
        s_delayed_device_idx = -1;
        return;
    }
    
    // 加载图像
    ts_led_image_t image = NULL;
    esp_err_t ret = ts_led_image_load(s_delayed_image_path, TS_LED_IMG_FMT_AUTO, &image);
    if (ret == ESP_OK && image) {
        ts_led_layer_t layer = ts_led_layer_get(dev, 0);
        if (layer) {
            ts_led_image_options_t opts = TS_LED_IMAGE_DEFAULT_OPTIONS();
            opts.scale = TS_LED_IMG_SCALE_FIT;
            opts.center = TS_LED_IMG_CENTER_IMAGE;
            
            ts_led_image_info_t info;
            ts_led_image_get_info(image, &info);
            
            if (info.frame_count > 1) {
                ts_led_image_animate_start(layer, image, &opts);
                TS_LOGI(TAG, "Delayed restored %s: animation=%s (%d frames)",
                        device_name, s_delayed_image_path, info.frame_count);
            } else {
                ts_led_image_display(layer, image, &opts);
                TS_LOGI(TAG, "Delayed restored %s: image=%s", device_name, s_delayed_image_path);
            }
            
            ts_led_preset_set_current_image(device_name, s_delayed_image_path);
            
            // 应用之前保存的 filter（如果有）
            if (s_current_filter[s_delayed_device_idx][0]) {
                apply_filter_to_layer_with_params(layer, s_current_filter[s_delayed_device_idx], s_delayed_device_idx);
                TS_LOGI(TAG, "Applied filter '%s' to %s", 
                        s_current_filter[s_delayed_device_idx], device_name);
            }
        } else {
            ts_led_image_free(image);
        }
    } else {
        TS_LOGW(TAG, "Delayed load failed: %s", esp_err_to_name(ret));
    }
    
    s_delayed_image_path[0] = '\0';
    s_delayed_device_idx = -1;
}

/* 设备名称到索引的映射 */
static int get_device_index(const char *name)
{
    if (!name) return -1;
    if (strcmp(name, "touch") == 0 || strcmp(name, TS_LED_TOUCH_NAME) == 0) return 0;
    if (strcmp(name, "board") == 0 || strcmp(name, TS_LED_BOARD_NAME) == 0) return 1;
    if (strcmp(name, "matrix") == 0 || strcmp(name, TS_LED_MATRIX_NAME) == 0) return 2;
    return -1;
}

/**
 * @brief Get short name for LED device by index
 * @note Reserved for NVS preset keys
 */
__attribute__((unused))
static const char *get_device_short_name(int idx)
{
    static const char *names[] = {"touch", "board", "matrix"};
    if (idx >= 0 && idx < 3) return names[idx];
    return NULL;
}

/* NVS 键名缩写（最长15字符限制）*/
static const char *get_nvs_prefix(int idx)
{
    static const char *prefixes[] = {"led.tch", "led.brd", "led.mat"};
    if (idx >= 0 && idx < 3) return prefixes[idx];
    return "led.unk";
}

static ts_led_device_t get_device_by_index(int idx)
{
    switch (idx) {
        case 0: return s_touch;
        case 1: return s_board;
        case 2: return s_matrix;
        default: return NULL;
    }
}

/* 记录当前运行的动画（供保存用） */
void ts_led_preset_set_current_animation(const char *device_name, const char *animation, uint8_t speed)
{
    int idx = get_device_index(device_name);
    if (idx >= 0) {
        if (animation) {
            strncpy(s_current_animation[idx], animation, sizeof(s_current_animation[idx]) - 1);
            if (animation[0]) {
                s_current_image_path[idx][0] = '\0';
                s_current_qrcode[idx][0] = '\0';
                s_current_qrcode_bg[idx][0] = '\0';
            }
        } else {
            s_current_animation[idx][0] = '\0';
        }
        s_current_speed[idx] = speed;
    }
}

/* 记录当前运行的后处理效果（供保存用） */
void ts_led_preset_set_current_filter(const char *device_name, const char *filter, uint8_t speed)
{
    int idx = get_device_index(device_name);
    if (idx >= 0) {
        if (filter) {
            strncpy(s_current_filter[idx], filter, sizeof(s_current_filter[idx]) - 1);
            s_current_filter_speed[idx] = speed > 0 ? speed : 50;
        } else {
            s_current_filter[idx][0] = '\0';
            s_current_filter_speed[idx] = 50;
            memset(&s_current_filter_config[idx], 0, sizeof(s_current_filter_config[idx]));
        }
    }
}

/* 设置当前滤镜的完整配置（供API调用） */
void ts_led_preset_set_current_filter_config(const char *device_name, const ts_led_effect_config_t *config)
{
    int idx = get_device_index(device_name);
    if (idx >= 0 && config) {
        s_current_filter_config[idx] = *config;
        TS_LOGI(TAG, "Updated filter config for %s: type=%d", device_name, config->type);
    }
}

/* 记录当前显示的 QR 码内容 */
void ts_led_preset_set_current_qrcode(const char *device_name, const char *text)
{
    int idx = get_device_index(device_name);
    if (idx >= 0) {
        if (text) {
            strncpy(s_current_qrcode[idx], text, sizeof(s_current_qrcode[idx]) - 1);
            s_current_animation[idx][0] = '\0';
            s_current_image_path[idx][0] = '\0';
        } else {
            s_current_qrcode[idx][0] = '\0';
            s_current_qrcode_bg[idx][0] = '\0';  // 清除 QR Code 时也清除背景图
        }
    }
}

/* 记录当前 QR 码的背景图 */
void ts_led_preset_set_current_qrcode_bg(const char *device_name, const char *bg_path)
{
    int idx = get_device_index(device_name);
    if (idx >= 0) {
        if (bg_path) {
            strncpy(s_current_qrcode_bg[idx], bg_path, sizeof(s_current_qrcode_bg[idx]) - 1);
        } else {
            s_current_qrcode_bg[idx][0] = '\0';
        }
    }
}

/* 记录当前显示的文本内容 */
void ts_led_preset_set_current_text(const char *device_name, const char *text)
{
    int idx = get_device_index(device_name);
    if (idx >= 0) {
        if (text) {
            strncpy(s_current_text[idx], text, sizeof(s_current_text[idx]) - 1);
        } else {
            s_current_text[idx][0] = '\0';
        }
    }
}

/* 兼容旧 API：set_current_effect 映射到 set_current_animation */
void ts_led_preset_set_current_effect(const char *device_name, const char *effect, uint8_t speed)
{
    ts_led_preset_set_current_animation(device_name, effect, speed);
}

/* 记录当前运行特效的颜色 */
void ts_led_preset_set_current_color(const char *device_name, ts_led_rgb_t color)
{
    int idx = get_device_index(device_name);
    if (idx >= 0) {
        s_current_color[idx] = color;
        s_current_color_valid[idx] = true;
    }
}

/* 清除当前特效颜色记录 */
void ts_led_preset_clear_current_color(const char *device_name)
{
    int idx = get_device_index(device_name);
    if (idx >= 0) {
        s_current_color_valid[idx] = false;
    }
}

/* 记录当前图像路径 */
void ts_led_preset_set_current_image(const char *device_name, const char *path)
{
    int idx = get_device_index(device_name);
    if (idx >= 0) {
        if (path) {
            strncpy(s_current_image_path[idx], path, sizeof(s_current_image_path[idx]) - 1);
            s_current_image_path[idx][sizeof(s_current_image_path[idx]) - 1] = '\0';
            // 设置图像时清除动画记录
            s_current_animation[idx][0] = '\0';
            s_current_qrcode[idx][0] = '\0';
            s_current_qrcode_bg[idx][0] = '\0';
        } else {
            s_current_image_path[idx][0] = '\0';
        }
    }
}

/* 清除当前图像路径记录 */
void ts_led_preset_clear_current_image(const char *device_name)
{
    int idx = get_device_index(device_name);
    if (idx >= 0) {
        s_current_image_path[idx][0] = '\0';
    }
}

esp_err_t ts_led_save_boot_config(const char *device_name)
{
    int idx = get_device_index(device_name);
    if (idx < 0) {
        TS_LOGE(TAG, "Unknown device: %s", device_name ? device_name : "NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_device_t dev = get_device_by_index(idx);
    if (!dev) {
        TS_LOGE(TAG, "Device %s not initialized", device_name);
        return ESP_ERR_INVALID_STATE;
    }
    
    const char *nvs_prefix = get_nvs_prefix(idx);
    char key[16];  // NVS 最大 15 字符 + null
    
    // 直接使用 NVS API 保存（绕过 ts_config，确保立即持久化）
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LED_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 保存动画名称
    snprintf(key, sizeof(key), "%s.ef", nvs_prefix);
    nvs_set_str(nvs, key, s_current_animation[idx]);
    
    // 保存后处理效果（filter）名称
    snprintf(key, sizeof(key), "%s.flt", nvs_prefix);
    if (s_current_filter[idx][0]) {
        nvs_set_str(nvs, key, s_current_filter[idx]);
    } else {
        nvs_erase_key(nvs, key);  // 无 filter 时删除键
    }
    
    // 保存速度
    snprintf(key, sizeof(key), "%s.sp", nvs_prefix);
    nvs_set_u8(nvs, key, s_current_speed[idx]);
    
    // 保存亮度
    snprintf(key, sizeof(key), "%s.br", nvs_prefix);
    nvs_set_u8(nvs, key, ts_led_device_get_brightness(dev));
    
    // 保存启用标志
    snprintf(key, sizeof(key), "%s.en", nvs_prefix);
    nvs_set_u8(nvs, key, ts_led_device_is_enabled(dev));
    
    // 保存本次颜色选择；默认色不能沿用上一次的显式颜色键。
    snprintf(key, sizeof(key), "%s.clr", nvs_prefix);
    if (s_current_color_valid[idx]) {
        uint32_t color_val = (s_current_color[idx].r << 16) | 
                             (s_current_color[idx].g << 8) | 
                             s_current_color[idx].b;
        nvs_set_u32(nvs, key, color_val);
    } else {
        nvs_erase_key(nvs, key);
    }
    
    // 保存图像路径（如果有）
    snprintf(key, sizeof(key), "%s.img", nvs_prefix);
    if (s_current_image_path[idx][0]) {
        nvs_set_str(nvs, key, s_current_image_path[idx]);
    } else {
        nvs_erase_key(nvs, key);  // 无图像时删除键
    }
    
    // 保存 QR Code 文本（如果有）
    snprintf(key, sizeof(key), "%s.qr", nvs_prefix);
    if (s_current_qrcode[idx][0]) {
        nvs_set_str(nvs, key, s_current_qrcode[idx]);
    } else {
        nvs_erase_key(nvs, key);  // 无 QR Code 时删除键
    }
    
    // 保存 QR Code 背景图路径（如果有）
    snprintf(key, sizeof(key), "%s.qrbg", nvs_prefix);
    if (s_current_qrcode_bg[idx][0]) {
        nvs_set_str(nvs, key, s_current_qrcode_bg[idx]);
    } else {
        nvs_erase_key(nvs, key);  // 无背景图时删除键
    }
    
    // 保存滤镜速度
    snprintf(key, sizeof(key), "%s.fsp", nvs_prefix);
    nvs_set_u8(nvs, key, s_current_filter_speed[idx]);
    
    // 保存滤镜配置（作为 blob）
    snprintf(key, sizeof(key), "%s.fpm", nvs_prefix);
    if (s_current_filter[idx][0]) {
        esp_err_t blob_ret = nvs_set_blob(nvs, key, &s_current_filter_config[idx], sizeof(ts_led_effect_config_t));
        TS_LOGI(TAG, "Saving filter config for %s: filter='%s', type=%d, ret=%s",
                device_name, s_current_filter[idx], s_current_filter_config[idx].type, esp_err_to_name(blob_ret));
    } else {
        nvs_erase_key(nvs, key);  // 无滤镜时删除参数
    }
    
    // 提交更改
    ret = nvs_commit(nvs);
    nvs_close(nvs);
    
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (s_current_qrcode[idx][0]) {
        TS_LOGI(TAG, "Saved boot config for %s: qrcode='%s', filter=%s, brightness=%d",
                device_name, s_current_qrcode[idx],
                s_current_filter[idx][0] ? s_current_filter[idx] : "(none)",
                ts_led_device_get_brightness(dev));
    } else if (s_current_image_path[idx][0]) {
        TS_LOGI(TAG, "Saved boot config for %s: image=%s, brightness=%d",
                device_name, s_current_image_path[idx], ts_led_device_get_brightness(dev));
    } else if (s_current_color_valid[idx]) {
        TS_LOGI(TAG, "Saved boot config for %s: animation=%s, filter=%s, speed=%d, brightness=%d, color=#%02X%02X%02X",
                device_name, 
                s_current_animation[idx][0] ? s_current_animation[idx] : "(none)",
                s_current_filter[idx][0] ? s_current_filter[idx] : "(none)",
                s_current_speed[idx],
                ts_led_device_get_brightness(dev),
                s_current_color[idx].r, s_current_color[idx].g, s_current_color[idx].b);
    } else {
        TS_LOGI(TAG, "Saved boot config for %s: animation=%s, filter=%s, speed=%d, brightness=%d",
                device_name, 
                s_current_animation[idx][0] ? s_current_animation[idx] : "(none)",
                s_current_filter[idx][0] ? s_current_filter[idx] : "(none)",
                s_current_speed[idx],
                ts_led_device_get_brightness(dev));
    }
    
    return ESP_OK;
}

esp_err_t ts_led_get_current_state(const char *device_name, ts_led_boot_config_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    
    int idx = get_device_index(device_name);
    if (idx < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_device_t dev = get_device_by_index(idx);
    if (!dev) {
        return ESP_ERR_INVALID_STATE;
    }
    
    memset(state, 0, sizeof(*state));
    
    // 复制当前动画名
    strncpy(state->animation, s_current_animation[idx], sizeof(state->animation) - 1);
    
    // 复制当前 filter
    strncpy(state->filter, s_current_filter[idx], sizeof(state->filter) - 1);
    
    // 复制当前图像路径
    strncpy(state->image_path, s_current_image_path[idx], sizeof(state->image_path) - 1);
    
    // 复制当前 QR Code 文本和背景图
    strncpy(state->qrcode_text, s_current_qrcode[idx], sizeof(state->qrcode_text) - 1);
    strncpy(state->qrcode_bg, s_current_qrcode_bg[idx], sizeof(state->qrcode_bg) - 1);
    
    // 速度
    state->speed = s_current_speed[idx];
    state->filter_speed = s_current_filter_speed[idx];
    
    // 亮度
    state->brightness = ts_led_device_get_brightness(dev);
    
    // 颜色
    if (s_current_color_valid[idx]) {
        state->color = s_current_color[idx];
    }
    
    state->enabled = ts_led_device_is_enabled(dev);
    
    return ESP_OK;
}

esp_err_t ts_led_save_all_boot_config(void)
{
    esp_err_t ret = ESP_OK;
    
    if (s_touch) {
        esp_err_t r = ts_led_save_boot_config("touch");
        if (r != ESP_OK) ret = r;
    }
    if (s_board) {
        esp_err_t r = ts_led_save_boot_config("board");
        if (r != ESP_OK) ret = r;
    }
    if (s_matrix) {
        esp_err_t r = ts_led_save_boot_config("matrix");
        if (r != ESP_OK) ret = r;
    }
    
    return ret;
}

esp_err_t ts_led_load_boot_config(const char *device_name)
{
    int idx = get_device_index(device_name);
    if (idx < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_device_t dev = get_device_by_index(idx);
    if (!dev) {
        return ESP_ERR_INVALID_STATE;
    }
    
    const char *nvs_prefix = get_nvs_prefix(idx);
    char key[16];  // NVS 最大 15 字符 + null
    
    // 直接使用 NVS API 加载
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LED_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        TS_LOGD(TAG, "No boot config namespace for LED");
        return ESP_ERR_NOT_FOUND;
    }
    
    // 检查是否有保存的配置
    snprintf(key, sizeof(key), "%s.en", nvs_prefix);
    uint8_t enabled = 0;
    ret = nvs_get_u8(nvs, key, &enabled);
    if (ret != ESP_OK) {
        nvs_close(nvs);
        TS_LOGD(TAG, "No boot config for %s", device_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    // A saved off state is valid; low-level restore never re-enables output.
    ts_led_device_set_enabled(dev, enabled != 0);

    // 加载亮度
    snprintf(key, sizeof(key), "%s.br", nvs_prefix);
    uint8_t brightness = 128;
    if (nvs_get_u8(nvs, key, &brightness) == ESP_OK) {
        ts_led_device_set_brightness(dev, brightness);
    }
    
    snprintf(key, sizeof(key), "%s.fsp", nvs_prefix);
    uint8_t filter_speed = 50;
    nvs_get_u8(nvs, key, &filter_speed);
    s_current_filter_speed[idx] = filter_speed;

    // 首先检查 QR Code（QR Code 优先级最高，仅限 matrix）
    if (idx == 2) {  // matrix
        snprintf(key, sizeof(key), "%s.qr", nvs_prefix);
        char qrcode_text[128] = {0};
        size_t qr_len = sizeof(qrcode_text);
        if (nvs_get_str(nvs, key, qrcode_text, &qr_len) == ESP_OK && qrcode_text[0]) {
            // 读取 QR Code 背景图
            snprintf(key, sizeof(key), "%s.qrbg", nvs_prefix);
            char qrcode_bg[128] = {0};
            size_t qrbg_len = sizeof(qrcode_bg);
            bool has_bg = (nvs_get_str(nvs, key, qrcode_bg, &qrbg_len) == ESP_OK && qrcode_bg[0]);
            
            // 读取 filter
            snprintf(key, sizeof(key), "%s.flt", nvs_prefix);
            char filter[32] = {0};
            size_t flt_len = sizeof(filter);
            bool has_filter = (nvs_get_str(nvs, key, filter, &flt_len) == ESP_OK && filter[0]);
            
            // 读取滤镜配置（如果有）
            if (has_filter) {
                snprintf(key, sizeof(key), "%s.fpm", nvs_prefix);
                size_t config_len = sizeof(ts_led_effect_config_t);
                esp_err_t blob_ret = nvs_get_blob(nvs, key, &s_current_filter_config[idx], &config_len);
                if (blob_ret == ESP_OK) {
                    TS_LOGI(TAG, "Loaded filter config for %s: filter='%s', type=%d, config_len=%d",
                            device_name, filter, s_current_filter_config[idx].type, config_len);
                } else {
                    TS_LOGW(TAG, "Failed to load filter config for %s: %s", device_name, esp_err_to_name(blob_ret));
                    memset(&s_current_filter_config[idx], 0, sizeof(ts_led_effect_config_t));
                }
            }
            
            nvs_close(nvs);
            
            // 加载背景图（如果有）
            ts_led_image_t bg_image = NULL;
            if (has_bg) {
                esp_err_t img_ret = ts_led_image_load(qrcode_bg, TS_LED_IMG_FMT_AUTO, &bg_image);
                if (img_ret != ESP_OK) {
                    TS_LOGW(TAG, "Failed to load QR background image: %s", qrcode_bg);
                    // 继续，只是没有背景图
                }
            }
            
            // 生成并显示 QR Code
            ts_led_qr_config_t qr_cfg = TS_LED_QR_DEFAULT_CONFIG();
            qr_cfg.text = qrcode_text;
            qr_cfg.bg_image = bg_image;
            qr_cfg.version_min = 1;
            qr_cfg.version_max = 4;
            qr_cfg.center = true;
            
            ts_led_qr_result_t qr_result;
            esp_err_t qr_ret = ts_led_qrcode_show_on_device(TS_LED_MATRIX_NAME, &qr_cfg, &qr_result);
            
            // 释放背景图
            if (bg_image) {
                ts_led_image_free(bg_image);
            }
            
            if (qr_ret == ESP_OK) {
                // 记录当前 QR Code
                ts_led_preset_set_current_qrcode(device_name, qrcode_text);
                ts_led_preset_set_current_qrcode_bg(device_name, has_bg ? qrcode_bg : NULL);
                
                // 应用 filter（如果有）
                if (has_filter) {
                    ts_led_layer_t layer = ts_led_layer_get(dev, 0);
                    if (layer) {
                        strncpy(s_current_filter[idx], filter, sizeof(s_current_filter[idx]) - 1);
                        apply_filter_to_layer_with_params(layer, filter, idx);
                    }
                }
                
                TS_LOGI(TAG, "Restored %s: qrcode='%s', bg=%s, filter=%s, brightness=%d",
                        device_name, qrcode_text,
                        has_bg ? qrcode_bg : "(none)",
                        has_filter ? filter : "(none)", (int)brightness);
            } else {
                TS_LOGW(TAG, "Failed to restore QR code for %s: %s", 
                        device_name, esp_err_to_name(qr_ret));
            }
            return ESP_OK;
        }
    }
    
    // 其次检查图像路径（图像优先于特效）
    snprintf(key, sizeof(key), "%s.img", nvs_prefix);
    char image_path[128] = {0};
    size_t img_len = sizeof(image_path);
    if (nvs_get_str(nvs, key, image_path, &img_len) == ESP_OK && image_path[0]) {
        // 在关闭 NVS 之前先读取 filter
        snprintf(key, sizeof(key), "%s.flt", nvs_prefix);
        char filter[32] = {0};
        size_t flt_len = sizeof(filter);
        bool has_filter = (nvs_get_str(nvs, key, filter, &flt_len) == ESP_OK && filter[0]);
        
        // 读取滤镜配置（如果有）
        if (has_filter) {
            snprintf(key, sizeof(key), "%s.fpm", nvs_prefix);
            size_t config_len = sizeof(ts_led_effect_config_t);
            if (nvs_get_blob(nvs, key, &s_current_filter_config[idx], &config_len) != ESP_OK) {
                memset(&s_current_filter_config[idx], 0, sizeof(ts_led_effect_config_t));
            }
        }
        
        nvs_close(nvs);
        
        // 检查文件是否存在（SD卡可能还未挂载）
        struct stat st;
        if (stat(image_path, &st) != 0) {
            // 文件不存在，可能SD卡还未挂载，延迟加载
            TS_LOGI(TAG, "Image file not ready, scheduling delayed load: %s", image_path);
            
            // 保存延迟加载信息（包括 filter）
            strncpy(s_delayed_image_path, image_path, sizeof(s_delayed_image_path) - 1);
            s_delayed_device_idx = idx;
            s_delayed_brightness = brightness;
            if (has_filter) {
                strncpy(s_current_filter[idx], filter, sizeof(s_current_filter[idx]) - 1);
            }
            
            // 创建一次性定时器，1秒后尝试加载
            if (s_delayed_image_timer == NULL) {
                esp_timer_create_args_t timer_args = {
                    .callback = delayed_image_load_callback,
                    .arg = NULL,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "led_img_delay"
                };
                esp_timer_create(&timer_args, &s_delayed_image_timer);
            }
            esp_timer_start_once(s_delayed_image_timer, 1000000);  // 1秒后
            
            return ESP_OK;
        }
        
        // 加载并显示图像
        ts_led_image_t image = NULL;
        esp_err_t img_ret = ts_led_image_load(image_path, TS_LED_IMG_FMT_AUTO, &image);
        if (img_ret == ESP_OK && image) {
            ts_led_layer_t layer = ts_led_layer_get(dev, 0);
            if (layer) {
                ts_led_image_options_t opts = TS_LED_IMAGE_DEFAULT_OPTIONS();
                opts.scale = TS_LED_IMG_SCALE_FIT;
                opts.center = TS_LED_IMG_CENTER_IMAGE;
                
                ts_led_image_info_t info;
                ts_led_image_get_info(image, &info);
                
                if (info.frame_count > 1) {
                    ts_led_image_animate_start(layer, image, &opts);
                    TS_LOGI(TAG, "Restored %s: animation=%s (%d frames), brightness=%d",
                            device_name, image_path, info.frame_count, (int)brightness);
                } else {
                    ts_led_image_display(layer, image, &opts);
                    TS_LOGI(TAG, "Restored %s: image=%s, brightness=%d",
                            device_name, image_path, (int)brightness);
                }
                
                // 记录当前图像路径
                ts_led_preset_set_current_image(device_name, image_path);
                
                // 应用 filter（如果有）
                if (has_filter) {
                    strncpy(s_current_filter[idx], filter, sizeof(s_current_filter[idx]) - 1);
                    apply_filter_to_layer_with_params(layer, filter, idx);
                    TS_LOGI(TAG, "Applied filter '%s' to %s", filter, device_name);
                }
            } else {
                ts_led_image_free(image);
                TS_LOGW(TAG, "Failed to get layer for %s", device_name);
            }
        } else {
            TS_LOGW(TAG, "Failed to load image '%s' for %s: %s", 
                    image_path, device_name, esp_err_to_name(img_ret));
        }
        return ESP_OK;
    }
    
    // 加载动画
    snprintf(key, sizeof(key), "%s.ef", nvs_prefix);
    char animation[32] = {0};
    size_t len = sizeof(animation);
    if (nvs_get_str(nvs, key, animation, &len) == ESP_OK && animation[0]) {
        // 获取速度
        snprintf(key, sizeof(key), "%s.sp", nvs_prefix);
        uint8_t speed = 0;
        nvs_get_u8(nvs, key, &speed);
        
        // 获取颜色（如果有）
        snprintf(key, sizeof(key), "%s.clr", nvs_prefix);
        uint32_t color_val = 0;
        bool has_color = (nvs_get_u32(nvs, key, &color_val) == ESP_OK);
        s_current_color_valid[idx] = has_color;
        if (has_color) {
            s_current_color[idx].r = (color_val >> 16) & 0xFF;
            s_current_color[idx].g = (color_val >> 8) & 0xFF;
            s_current_color[idx].b = color_val & 0xFF;
            s_current_color_valid[idx] = true;
        }
        
        // 加载后处理效果（filter）
        snprintf(key, sizeof(key), "%s.flt", nvs_prefix);
        char filter[32] = {0};
        size_t flt_len = sizeof(filter);
        bool has_filter = (nvs_get_str(nvs, key, filter, &flt_len) == ESP_OK && filter[0]);
        
        // 读取滤镜配置（如果有）
        if (has_filter) {
            snprintf(key, sizeof(key), "%s.fpm", nvs_prefix);
            size_t config_len = sizeof(ts_led_effect_config_t);
            if (nvs_get_blob(nvs, key, &s_current_filter_config[idx], &config_len) != ESP_OK) {
                memset(&s_current_filter_config[idx], 0, sizeof(ts_led_effect_config_t));
            }
        }
        
        nvs_close(nvs);
        
        // 启动动画
        const ts_led_animation_def_t *eff = ts_led_animation_get_builtin(animation);
        if (eff) {
            ts_led_layer_t layer = ts_led_layer_get(dev, 0);
            if (layer) {
                ts_led_animation_def_t modified = *eff;
                if (speed > 0 && speed <= 100) {
                    // 速度映射：1->200ms, 100->5ms
                    modified.frame_interval_ms = 200 - (speed - 1) * 195 / 99;
                }
                // 如果有保存的颜色，传递给动画
                if (has_color) {
                    modified.user_data = &s_current_color[idx];
                }
                ts_led_animation_start(layer, &modified);
                
                // 记录当前动画
                ts_led_preset_set_current_animation(device_name, animation, speed);
                
                // 应用后处理效果（如果有）
                if (has_filter) {
                    strncpy(s_current_filter[idx], filter, sizeof(s_current_filter[idx]) - 1);
                    apply_filter_to_layer_with_params(layer, filter, idx);
                    TS_LOGI(TAG, "Restored %s: animation=%s, filter=%s, speed=%d, brightness=%d",
                            device_name, animation, filter, (int)speed, (int)brightness);
                } else if (has_color) {
                    TS_LOGI(TAG, "Restored %s: animation=%s, speed=%d, brightness=%d, color=#%02X%02X%02X",
                            device_name, animation, (int)speed, (int)brightness,
                            s_current_color[idx].r, s_current_color[idx].g, s_current_color[idx].b);
                } else {
                    TS_LOGI(TAG, "Restored %s: animation=%s, speed=%d, brightness=%d",
                            device_name, animation, (int)speed, (int)brightness);
                }
            }
        } else {
            TS_LOGW(TAG, "Animation '%s' not found for %s", animation, device_name);
        }
    } else {
        nvs_close(nvs);
        TS_LOGI(TAG, "Restored %s: brightness=%d (no animation)", device_name, (int)brightness);
    }
    
    return ESP_OK;
}

esp_err_t ts_led_load_all_boot_config(void)
{
    TS_LOGI(TAG, "Loading LED boot configurations...");
    
    ts_led_load_boot_config("touch");
    ts_led_load_boot_config("board");
    ts_led_load_boot_config("matrix");
    
    return ESP_OK;
}

esp_err_t ts_led_clear_boot_config(const char *device_name)
{
    if (device_name) {
        int idx = get_device_index(device_name);
        if (idx < 0) return ESP_ERR_INVALID_ARG;
        
        const char *nvs_prefix = get_nvs_prefix(idx);
        char key[16];  // NVS 最大 15 字符 + null
        
        nvs_handle_t nvs;
        esp_err_t ret = nvs_open(LED_NVS_NAMESPACE, NVS_READWRITE, &nvs);
        if (ret != ESP_OK) {
            return ret;
        }
        
        snprintf(key, sizeof(key), "%s.en", nvs_prefix);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s.ef", nvs_prefix);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s.flt", nvs_prefix);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s.sp", nvs_prefix);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s.br", nvs_prefix);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s.img", nvs_prefix);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s.clr", nvs_prefix);
        nvs_erase_key(nvs, key);
        
        nvs_commit(nvs);
        nvs_close(nvs);
        TS_LOGI(TAG, "Cleared boot config for %s", device_name);
    } else {
        // 清除所有
        ts_led_clear_boot_config("touch");
        ts_led_clear_boot_config("board");
        ts_led_clear_boot_config("matrix");
    }
    
    return ESP_OK;
}

esp_err_t ts_led_get_boot_config(const char *device_name, ts_led_boot_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    int idx = get_device_index(device_name);
    if (idx < 0) return ESP_ERR_INVALID_ARG;
    
    const char *nvs_prefix = get_nvs_prefix(idx);
    char key[16];  // NVS 最大 15 字符 + null
    
    memset(config, 0, sizeof(*config));
    
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LED_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        config->enabled = false;
        return ESP_ERR_NOT_FOUND;
    }
    
    // 检查是否启用
    snprintf(key, sizeof(key), "%s.en", nvs_prefix);
    uint8_t enabled = 0;
    ret = nvs_get_u8(nvs, key, &enabled);
    config->enabled = (ret == ESP_OK && enabled);
    
    if (ret != ESP_OK) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }
    
    // 加载动画
    snprintf(key, sizeof(key), "%s.ef", nvs_prefix);
    size_t len = sizeof(config->animation);
    nvs_get_str(nvs, key, config->animation, &len);
    
    // 加载后处理效果
    snprintf(key, sizeof(key), "%s.flt", nvs_prefix);
    len = sizeof(config->filter);
    nvs_get_str(nvs, key, config->filter, &len);
    
    // 加载图像路径
    snprintf(key, sizeof(key), "%s.img", nvs_prefix);
    len = sizeof(config->image_path);
    nvs_get_str(nvs, key, config->image_path, &len);
    
    // 加载速度
    snprintf(key, sizeof(key), "%s.sp", nvs_prefix);
    nvs_get_u8(nvs, key, &config->speed);
    
    // 加载亮度
    snprintf(key, sizeof(key), "%s.br", nvs_prefix);
    nvs_get_u8(nvs, key, &config->brightness);
    
    nvs_close(nvs);
    return ESP_OK;
}
