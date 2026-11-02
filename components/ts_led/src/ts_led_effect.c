/**
 * @file ts_led_effect.c
 * @brief TianShanOS LED Post-Processing Effects Implementation
 * 
 * Implements post-processing effects (filters) that modify layer output.
 * These effects are applied during composition, after content rendering.
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_led_effect.h"
#include "ts_led.h"
#include "ts_led_private.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ts_led_effect";

/*===========================================================================*/
/*                              Effect Name Table                             */
/*===========================================================================*/

static const struct {
    ts_led_effect_type_t type;
    const char *name;
} s_effect_names[] = {
    { TS_LED_EFFECT_NONE,         "none" },
    { TS_LED_EFFECT_BRIGHTNESS,   "brightness" },
    { TS_LED_EFFECT_PULSE,        "pulse" },
    { TS_LED_EFFECT_BLINK,        "blink" },
    { TS_LED_EFFECT_FADE_IN,      "fade_in" },
    { TS_LED_EFFECT_FADE_OUT,     "fade_out" },
    { TS_LED_EFFECT_BREATHING,    "breathing" },
    { TS_LED_EFFECT_COLOR_SHIFT,  "color_shift" },
    { TS_LED_EFFECT_SATURATION,   "saturation" },
    { TS_LED_EFFECT_INVERT,       "invert" },
    { TS_LED_EFFECT_GRAYSCALE,    "grayscale" },
    { TS_LED_EFFECT_COLOR_TEMP,   "color_temp" },
    { TS_LED_EFFECT_SCANLINE,     "scanline" },
    { TS_LED_EFFECT_WAVE,         "wave" },
    { TS_LED_EFFECT_BLUR,         "blur" },
    { TS_LED_EFFECT_PIXELATE,     "pixelate" },
    { TS_LED_EFFECT_MIRROR,       "mirror" },
    { TS_LED_EFFECT_GLITCH,       "glitch" },
    { TS_LED_EFFECT_NOISE,        "noise" },
    { TS_LED_EFFECT_STROBE,       "strobe" },
    { TS_LED_EFFECT_RAINBOW,      "rainbow" },
    { TS_LED_EFFECT_SPARKLE,      "sparkle" },
    { TS_LED_EFFECT_PLASMA,       "plasma" },
    { TS_LED_EFFECT_SEPIA,        "sepia" },
    { TS_LED_EFFECT_POSTERIZE,    "posterize" },
    { TS_LED_EFFECT_CONTRAST,     "contrast" },
};

#define NUM_EFFECTS (sizeof(s_effect_names) / sizeof(s_effect_names[0]))

/*===========================================================================*/
/*                              Helper Functions                              */
/*===========================================================================*/

/**
 * @brief Scale RGB color by brightness
 */
static inline ts_led_rgb_t scale_rgb(ts_led_rgb_t color, uint8_t scale)
{
    return (ts_led_rgb_t){
        .r = (uint8_t)((color.r * scale) >> 8),
        .g = (uint8_t)((color.g * scale) >> 8),
        .b = (uint8_t)((color.b * scale) >> 8)
    };
}

/**
 * @brief Clamp value to 0-255
 */
static inline uint8_t clamp_u8(int32_t val)
{
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint8_t)val;
}

/**
 * @brief Simple pseudo-random number generator
 */
static uint32_t s_random_state = 12345;
static inline uint32_t effect_random(void)
{
    s_random_state = s_random_state * 1103515245 + 12345;
    return (s_random_state >> 16) & 0xFFFF;
}

/**
 * @brief Convert RGB to HSV
 */
static void rgb_to_hsv(ts_led_rgb_t rgb, uint16_t *h, uint8_t *s, uint8_t *v)
{
    uint8_t max = rgb.r > rgb.g ? (rgb.r > rgb.b ? rgb.r : rgb.b) 
                                : (rgb.g > rgb.b ? rgb.g : rgb.b);
    uint8_t min = rgb.r < rgb.g ? (rgb.r < rgb.b ? rgb.r : rgb.b) 
                                : (rgb.g < rgb.b ? rgb.g : rgb.b);
    uint8_t delta = max - min;
    
    *v = max;
    
    if (max == 0) {
        *s = 0;
        *h = 0;
        return;
    }
    
    *s = (uint8_t)((delta * 255) / max);
    
    if (delta == 0) {
        *h = 0;
        return;
    }
    
    int16_t hue;
    if (max == rgb.r) {
        hue = 60 * ((rgb.g - rgb.b) * 256 / delta) / 256;
    } else if (max == rgb.g) {
        hue = 120 + 60 * ((rgb.b - rgb.r) * 256 / delta) / 256;
    } else {
        hue = 240 + 60 * ((rgb.r - rgb.g) * 256 / delta) / 256;
    }
    
    if (hue < 0) hue += 360;
    *h = (uint16_t)hue;
}

/**
 * @brief Convert HSV to RGB
 */
static ts_led_rgb_t hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v)
{
    if (s == 0) {
        return (ts_led_rgb_t){v, v, v};
    }
    
    h = h % 360;
    uint8_t region = h / 60;
    uint8_t remainder = (h % 60) * 255 / 60;
    
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    
    switch (region) {
        case 0: return (ts_led_rgb_t){v, t, p};
        case 1: return (ts_led_rgb_t){q, v, p};
        case 2: return (ts_led_rgb_t){p, v, t};
        case 3: return (ts_led_rgb_t){p, q, v};
        case 4: return (ts_led_rgb_t){t, p, v};
        default: return (ts_led_rgb_t){v, p, q};
    }
}

/*===========================================================================*/
/*                              Effect Processors                             */
/*===========================================================================*/

/**
 * @brief Process brightness effect
 */
static void process_brightness(ts_led_rgb_t *buffer, size_t count,
                               const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)time_ms;
    uint8_t level = config->params.brightness.level;
    
    for (size_t i = 0; i < count; i++) {
        buffer[i] = scale_rgb(buffer[i], level);
    }
}

/**
 * @brief Process pulse effect (sine wave brightness modulation)
 */
static void process_pulse(ts_led_rgb_t *buffer, size_t count,
                          const ts_led_effect_config_t *config, uint32_t time_ms)
{
    float freq = config->params.pulse.frequency;
    uint8_t min_level = config->params.pulse.min_level;
    uint8_t max_level = config->params.pulse.max_level;
    
    // Calculate brightness using sine wave
    float phase = (float)time_ms * freq * 0.001f * 2.0f * 3.14159f;
    float sine = (sinf(phase) + 1.0f) * 0.5f; // 0.0 to 1.0
    uint8_t level = min_level + (uint8_t)(sine * (max_level - min_level));
    
    for (size_t i = 0; i < count; i++) {
        buffer[i] = scale_rgb(buffer[i], level);
    }
}

/**
 * @brief Process blink effect
 */
static void process_blink(ts_led_rgb_t *buffer, size_t count,
                          const ts_led_effect_config_t *config, uint32_t time_ms)
{
    uint32_t period = config->params.blink.on_time_ms + config->params.blink.off_time_ms;
    uint32_t phase = time_ms % period;
    
    if (phase >= config->params.blink.on_time_ms) {
        // Off phase - set all to black
        memset(buffer, 0, count * sizeof(ts_led_rgb_t));
    }
    // On phase - keep original colors
}

/**
 * @brief Process fade in effect
 */
static void process_fade_in(ts_led_layer_t layer, ts_led_rgb_t *buffer, size_t count,
                            const ts_led_effect_config_t *config, uint32_t time_ms)
{
    uint32_t elapsed = time_ms;  // Renderer already supplies time since effect start.
    uint16_t duration = config->params.fade.duration_ms;
    
    uint8_t level;
    if (elapsed >= duration) {
        level = 255;
        if (config->params.fade.auto_remove) {
            layer->post_effect.type = TS_LED_EFFECT_NONE;
        }
    } else {
        level = (uint8_t)((elapsed * 255) / duration);
    }
    
    for (size_t i = 0; i < count; i++) {
        buffer[i] = scale_rgb(buffer[i], level);
    }
}

/**
 * @brief Process fade out effect
 */
static void process_fade_out(ts_led_layer_t layer, ts_led_rgb_t *buffer, size_t count,
                             const ts_led_effect_config_t *config, uint32_t time_ms)
{
    uint32_t elapsed = time_ms;  // Renderer already supplies time since effect start.
    uint16_t duration = config->params.fade.duration_ms;
    
    uint8_t level;
    if (elapsed >= duration) {
        level = 0;
        if (config->params.fade.auto_remove) {
            layer->post_effect.type = TS_LED_EFFECT_NONE;
        }
    } else {
        level = 255 - (uint8_t)((elapsed * 255) / duration);
    }
    
    for (size_t i = 0; i < count; i++) {
        buffer[i] = scale_rgb(buffer[i], level);
    }
}

/**
 * @brief Process breathing effect (smoother than pulse)
 */
static void process_breathing(ts_led_rgb_t *buffer, size_t count,
                              const ts_led_effect_config_t *config, uint32_t time_ms)
{
    float freq = config->params.breathing.frequency;
    uint8_t min_level = config->params.breathing.min_level;
    uint8_t max_level = config->params.breathing.max_level;
    
    // Breathing uses (1 - cos) / 2 for smoother feel
    float phase = (float)time_ms * freq * 0.001f * 2.0f * 3.14159f;
    float breath = (1.0f - cosf(phase)) * 0.5f; // 0.0 to 1.0
    uint8_t level = min_level + (uint8_t)(breath * (max_level - min_level));
    
    for (size_t i = 0; i < count; i++) {
        buffer[i] = scale_rgb(buffer[i], level);
    }
}

/**
 * @brief Process color shift (hue rotation)
 */
static void process_color_shift(ts_led_rgb_t *buffer, size_t count,
                                const ts_led_effect_config_t *config, uint32_t time_ms)
{
    int16_t static_shift = config->params.color_shift.static_shift;
    float speed = config->params.color_shift.speed;
    
    // Calculate total hue shift
    int16_t shift = static_shift + (int16_t)(speed * time_ms / 1000.0f);
    shift = ((shift % 360) + 360) % 360; // Normalize to 0-359
    
    for (size_t i = 0; i < count; i++) {
        uint16_t h;
        uint8_t s, v;
        rgb_to_hsv(buffer[i], &h, &s, &v);
        h = (h + shift) % 360;
        buffer[i] = hsv_to_rgb(h, s, v);
    }
}

/**
 * @brief Process saturation adjustment
 */
static void process_saturation(ts_led_rgb_t *buffer, size_t count,
                               const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)time_ms;
    float level = config->params.saturation.level;
    
    for (size_t i = 0; i < count; i++) {
        uint16_t h;
        uint8_t s, v;
        rgb_to_hsv(buffer[i], &h, &s, &v);
        
        float new_s = s * level;
        if (new_s > 255) new_s = 255;
        if (new_s < 0) new_s = 0;
        
        buffer[i] = hsv_to_rgb(h, (uint8_t)new_s, v);
    }
}

/**
 * @brief Process invert effect
 */
static void process_invert(ts_led_rgb_t *buffer, size_t count,
                           const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)config;
    (void)time_ms;
    
    for (size_t i = 0; i < count; i++) {
        buffer[i].r = 255 - buffer[i].r;
        buffer[i].g = 255 - buffer[i].g;
        buffer[i].b = 255 - buffer[i].b;
    }
}

/**
 * @brief Process grayscale effect
 */
static void process_grayscale(ts_led_rgb_t *buffer, size_t count,
                              const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)config;
    (void)time_ms;
    
    for (size_t i = 0; i < count; i++) {
        // Use luminance formula: 0.299*R + 0.587*G + 0.114*B
        uint8_t gray = (uint8_t)((buffer[i].r * 77 + buffer[i].g * 150 + buffer[i].b * 29) >> 8);
        buffer[i].r = gray;
        buffer[i].g = gray;
        buffer[i].b = gray;
    }
}

/**
 * @brief Process color temperature adjustment
 */
static void process_color_temp(ts_led_rgb_t *buffer, size_t count,
                               const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)time_ms;
    int8_t temp = config->params.color_temp.temperature;
    
    // Warm: increase red, decrease blue
    // Cool: increase blue, decrease red
    int16_t r_adj = temp;      // -100 to +100
    int16_t b_adj = -temp;
    
    for (size_t i = 0; i < count; i++) {
        buffer[i].r = clamp_u8((int32_t)buffer[i].r + r_adj);
        buffer[i].b = clamp_u8((int32_t)buffer[i].b + b_adj);
    }
}

/**
 * @brief Process scanline effect (matrix only)
 * 
 * 扫描线沿着指定角度移动，参数说明：
 * - angle: 旋转角度（0度=水平向右，90度=垂直向上）
 * - width: 扫描线宽度（1-16像素），影响渐变范围，值越大光晕越宽
 * - intensity: 中心亮度增益（0-255），值越大中心越亮，推荐100-200产生明显对比
 */
static void process_scanline(ts_led_rgb_t *buffer, size_t count,
                             uint16_t width, uint16_t height,
                             const ts_led_effect_config_t *config, uint32_t time_ms)
{
    if (width == 0 || height == 0) {
        ESP_LOGW(TAG, "Scanline effect: invalid dimensions (width=%d, height=%d)", width, height);
        return;
    }
    
    float speed = config->params.scanline.speed;
    uint8_t line_width = config->params.scanline.width > 0 ? config->params.scanline.width : 3;
    uint8_t intensity = config->params.scanline.intensity;
    float angle_deg = config->params.scanline.angle;
    
    // 转换为弧度
    float angle_rad = angle_deg * M_PI / 180.0f;
    float cos_a = cosf(angle_rad);
    float sin_a = sinf(angle_rad);
    
    // 计算扫描线移动的最大距离（对角线长度）
    float diag = sqrtf((float)(width * width + height * height));
    float pos = fmodf(speed * time_ms / 1000.0f, diag);
    
    // 中心点
    float cx = width / 2.0f;
    float cy = height / 2.0f;
    
    for (size_t i = 0; i < count; i++) {
        uint16_t x = i % width;
        uint16_t y = i / width;
        
        // 计算像素相对中心的坐标
        float dx = x - cx;
        float dy = y - cy;
        
        // 投影到垂直于扫描线的轴上（点到线的距离）
        float perp_dist = dx * cos_a + dy * sin_a;
        
        // 扫描线当前位置（从中心偏移）
        float line_pos = pos - diag / 2.0f;
        
        // 计算像素到扫描线的距离
        float dist = fabsf(perp_dist - line_pos);
        
        if (dist < line_width) {
            // 渐变因子：中心=1.0，边缘=0.0
            float fade = 1.0f - (dist / line_width);
            
            // 强度参数影响亮度增益：intensity=0时boost=1.0（无变化），intensity=255时最大boost=4.0
            // 使用非线性曲线增强中心亮度
            float boost = 1.0f + (float)intensity / 255.0f * 3.0f * fade * fade;
            
            buffer[i].r = clamp_u8((int32_t)(buffer[i].r * boost));
            buffer[i].g = clamp_u8((int32_t)(buffer[i].g * boost));
            buffer[i].b = clamp_u8((int32_t)(buffer[i].b * boost));
        }
    }
}

/**
 * @brief Process wave effect (matrix only)
 */
static void process_wave(ts_led_rgb_t *buffer, size_t count,
                         uint16_t width, uint16_t height,
                         const ts_led_effect_config_t *config, uint32_t time_ms)
{
    if (width == 0 || height == 0) {
        ESP_LOGW(TAG, "Wave effect: invalid dimensions (width=%d, height=%d)", width, height);
        return;
    }
    
    float angle_deg = config->params.wave.angle;
    float wavelength = config->params.wave.wavelength;
    float speed = config->params.wave.speed;
    uint8_t amplitude = config->params.wave.amplitude;
    
    // 防止 wavelength 为 0
    if (wavelength < 1.0f) wavelength = 8.0f;
    
    ESP_LOGD(TAG, "Wave: w=%d h=%d speed=%.1f wavelength=%.1f amplitude=%d angle=%.1f°",
             width, height, speed, wavelength, amplitude, angle_deg);
    
    // 波浪沿角度方向传播的时间偏移
    float time_offset = speed * time_ms / 1000.0f;
    
    // 将角度转换为弧度并计算方向向量
    float angle_rad = angle_deg * M_PI / 180.0f;
    float cos_a = cosf(angle_rad);
    float sin_a = sinf(angle_rad);
    
    // 计算中心点
    float cx = width / 2.0f;
    float cy = height / 2.0f;
    
    for (size_t i = 0; i < count; i++) {
        uint16_t x = i % width;
        uint16_t y = i / width;
        
        // 计算相对中心的坐标
        float dx = (float)x - cx;
        float dy = (float)y - cy;
        
        // 沿波浪传播方向的投影距离
        float coord = dx * cos_a + dy * sin_a;
        
        // 计算波浪值
        float phase = (coord + time_offset) * 2.0f * M_PI / wavelength;
        float wave = (sinf(phase) + 1.0f) * 0.5f; // 0.0 to 1.0
        
        // 根据波浪调整亮度
        uint8_t level = 255 - amplitude + (uint8_t)(wave * amplitude);
        buffer[i] = scale_rgb(buffer[i], level);
    }
}

/**
 * @brief Process strobe effect
 */
static void process_strobe(ts_led_rgb_t *buffer, size_t count,
                           const ts_led_effect_config_t *config, uint32_t time_ms)
{
    uint8_t freq = config->params.strobe.frequency;
    if (freq == 0) freq = 1;
    
    uint32_t period = 1000 / freq;
    uint32_t phase = time_ms % period;
    
    // 10% on-time for strobe effect
    if (phase > period / 10) {
        memset(buffer, 0, count * sizeof(ts_led_rgb_t));
    }
}

/**
 * @brief Process noise effect
 */
static void process_noise(ts_led_rgb_t *buffer, size_t count,
                          const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)time_ms;
    uint8_t amount = config->params.noise.amount;
    bool mono = config->params.noise.monochrome;
    
    for (size_t i = 0; i < count; i++) {
        if (mono) {
            int8_t noise = (int8_t)((effect_random() & 0xFF) - 128) * amount / 255;
            buffer[i].r = clamp_u8((int32_t)buffer[i].r + noise);
            buffer[i].g = clamp_u8((int32_t)buffer[i].g + noise);
            buffer[i].b = clamp_u8((int32_t)buffer[i].b + noise);
        } else {
            buffer[i].r = clamp_u8((int32_t)buffer[i].r + 
                                   (int8_t)((effect_random() & 0xFF) - 128) * amount / 255);
            buffer[i].g = clamp_u8((int32_t)buffer[i].g + 
                                   (int8_t)((effect_random() & 0xFF) - 128) * amount / 255);
            buffer[i].b = clamp_u8((int32_t)buffer[i].b + 
                                   (int8_t)((effect_random() & 0xFF) - 128) * amount / 255);
        }
    }
}

/**
 * @brief Process glitch effect
 */
static void process_glitch(ts_led_rgb_t *buffer, size_t count,
                           uint16_t width, uint16_t height,
                           const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)time_ms;
    (void)width;
    (void)height;
    uint8_t intensity = config->params.glitch.intensity;
    uint8_t frequency = config->params.glitch.frequency;
    
    ESP_LOGD(TAG, "Glitch: intensity=%d frequency=%d count=%d", intensity, frequency, count);
    
    // Decide if glitch happens this frame
    if ((effect_random() & 0xFF) > frequency) return;
    
    if (width == 0 || height == 0) {
        ESP_LOGW(TAG, "Glitch effect: using linear mode (width=%d, height=%d)", width, height);
        // Linear glitch - random block
        size_t start = (effect_random() * count) >> 16;
        size_t len = (effect_random() * intensity) >> 16;
        if (start + len > count) len = count - start;
        
        // Color shift
        int16_t shift = (int16_t)(effect_random() & 0xFF) - 128;
        for (size_t i = start; i < start + len; i++) {
            buffer[i].r = clamp_u8((int32_t)buffer[i].r + shift);
        }
    } else {
        // Matrix glitch - random row offset
        uint16_t row = (effect_random() * height) >> 16;
        int16_t offset = (int16_t)((effect_random() & 0x0F) - 8);
        
        // Shift row
        ts_led_rgb_t temp[width];
        memcpy(temp, &buffer[row * width], width * sizeof(ts_led_rgb_t));
        
        for (uint16_t x = 0; x < width; x++) {
            int16_t src = x + offset;
            if (src < 0) src += width;
            if (src >= width) src -= width;
            buffer[row * width + x] = temp[src];
        }
    }
}

/**
 * @brief Process rainbow effect
 * 
 * Shifts hue over time to create rainbow color cycling effect.
 * Saturation parameter controls how much to boost the saturation (0-255).
 * 255 = maximum saturation boost, 0 = keep original saturation.
 */
static void process_rainbow(ts_led_rgb_t *buffer, size_t count,
                            const ts_led_effect_config_t *config, uint32_t time_ms)
{
    float speed = config->params.rainbow.speed;
    uint8_t sat_boost = config->params.rainbow.saturation;
    
    float hue_offset = fmodf(speed * time_ms / 1000.0f, 360.0f);
    
    for (size_t i = 0; i < count; i++) {
        // Convert to HSV
        uint16_t h;
        uint8_t s, v;
        rgb_to_hsv(buffer[i], &h, &s, &v);
        
        // Shift hue
        h = (uint16_t)((h + (int)hue_offset)) % 360;
        
        // Boost saturation: blend original with full saturation
        // sat_boost=255 means full saturation, sat_boost=0 means keep original
        s = s + (((255 - s) * sat_boost) >> 8);
        
        // Convert back to RGB
        buffer[i] = hsv_to_rgb(h, s, v);
    }
}

/**
 * @brief Sparkle state for each pixel
 */
typedef struct {
    uint8_t brightness;    // Current sparkle brightness (0-255)
    uint8_t phase;         // 0=off, 1=fade-in, 2=hold, 3=fade-out
    uint8_t target;        // Target brightness
    uint8_t fade_speed;    // Fade speed (random per sparkle)
} sparkle_state_t;

/**
 * @brief Process sparkle effect
 * 
 * Creates realistic star-like sparkles with individual lifecycle:
 * - Random triggering (not synchronized)
 * - Fade in → Hold → Fade out (afterglow effect)
 * - Each sparkle has unique timing
 */
static void process_sparkle(ts_led_rgb_t *buffer, size_t count,
                            const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)time_ms;
    
    float speed = config->params.sparkle.speed;
    uint8_t density = config->params.sparkle.density;
    uint8_t decay = config->params.sparkle.decay;
    
    /* Dynamically allocated sparkle state array in PSRAM to reduce DRAM usage
     * Each sparkle_state_t is 4 bytes, 1024 pixels = 4KB
     */
    static sparkle_state_t *sparkle_states = NULL;
    static size_t sparkle_states_capacity = 0;
    static bool initialized = false;
    
    /* Allocate or reallocate if needed */
    size_t required = count < 1024 ? count : 1024;
    if (!sparkle_states || sparkle_states_capacity < required) {
        /* Free old buffer if exists */
        if (sparkle_states) {
            free(sparkle_states);
        }
        
        /* Allocate in PSRAM first, fallback to DRAM */
        size_t alloc_size = 1024 * sizeof(sparkle_state_t);  /* Always allocate max size */
        sparkle_states = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!sparkle_states) {
            sparkle_states = malloc(alloc_size);
        }
        
        if (sparkle_states) {
            sparkle_states_capacity = 1024;
            memset(sparkle_states, 0, alloc_size);
            initialized = true;
        } else {
            ESP_LOGW(TAG, "Failed to allocate sparkle states buffer");
            sparkle_states_capacity = 0;
            return;
        }
    }
    
    if (!initialized || count > sparkle_states_capacity) {
        memset(sparkle_states, 0, sparkle_states_capacity * sizeof(sparkle_state_t));
        initialized = true;
    }
    
    // Use count as limit
    size_t max_idx = count < sparkle_states_capacity ? count : sparkle_states_capacity;
    
    // 简化的概率计算：
    // speed: 0.1-100 直接使用，不再使用平方根
    // density: 0-255，影响同时闪烁数量
    // spawn_chance 范围: 0-25500 对应 0-65535 的概率范围
    uint32_t spawn_chance = (uint32_t)(speed * density); // 0.1*0 ~ 100*255 = 0~25500
    if (spawn_chance > 65535) spawn_chance = 65535;
    
    // Process each pixel's sparkle state
    for (size_t i = 0; i < max_idx; i++) {
        sparkle_state_t *state = &sparkle_states[i];
        
        // Skip black pixels (no content)
        if (buffer[i].r < 5 && buffer[i].g < 5 && buffer[i].b < 5) {
            state->brightness = 0;
            state->phase = 0;
            continue;
        }
        
        // Try to spawn new sparkle if currently off
        if (state->phase == 0) {
            // Random chance to start sparkling
            if ((effect_random() & 0xFFFF) < spawn_chance) {
                state->phase = 1; // Start fade-in
                state->brightness = 0;
                state->target = 200 + (effect_random() & 0x37); // 200-255 random peak
                state->fade_speed = 15 + (effect_random() & 0x1F); // Random fade speed 15-46
            }
        }
        
        // Update sparkle lifecycle
        switch (state->phase) {
            case 1: // Fade in
                state->brightness += state->fade_speed;
                if (state->brightness >= state->target) {
                    state->brightness = state->target;
                    state->phase = 2; // Move to hold phase
                }
                break;
                
            case 2: // Hold at peak (short duration)
                if ((effect_random() & 0x7F) < 10) { // ~8% chance per frame
                    state->phase = 3; // Start fade out
                }
                break;
                
            case 3: // Fade out (afterglow)
                {
                    // 增强 decay 参数影响:
                    // - decay=0:   极慢衰减，长余晖
                    // - decay=128: 中速衰减
                    // - decay=255: 快速消失
                    // 使用非线性映射增强低 decay 值的效果
                    uint16_t decay_factor = decay * decay / 255; // 0-255 非线性
                    uint8_t fade_amount = (state->fade_speed * decay_factor) >> 7;
                    if (fade_amount < 1) fade_amount = 1; // 最小衰减速度
                    
                    if (state->brightness > fade_amount) {
                        state->brightness -= fade_amount;
                    } else {
                        state->brightness = 0;
                        state->phase = 0; // Back to off
                    }
                }
                break;
        }
        
        // Apply sparkle overlay only if active
        if (state->brightness > 0) {
            // Blend white sparkle with original pixel
            uint16_t r = buffer[i].r + ((255 - buffer[i].r) * state->brightness >> 8);
            uint16_t g = buffer[i].g + ((255 - buffer[i].g) * state->brightness >> 8);
            uint16_t b = buffer[i].b + ((255 - buffer[i].b) * state->brightness >> 8);
            
            buffer[i].r = r > 255 ? 255 : r;
            buffer[i].g = g > 255 ? 255 : g;
            buffer[i].b = b > 255 ? 255 : b;
        }
    }
}

/**
 * @brief Process plasma effect
 */
static void process_plasma(ts_led_rgb_t *buffer, size_t count,
                          uint16_t width, uint16_t height,
                          const ts_led_effect_config_t *config, uint32_t time_ms)
{
    if (width == 0 || height == 0) return;
    
    float speed = config->params.plasma.speed;
    uint8_t scale = config->params.plasma.scale;
    float time = speed * time_ms / 1000.0f;
    
    for (size_t i = 0; i < count; i++) {
        uint16_t x = i % width;
        uint16_t y = i / width;
        
        // Plasma calculation
        float v1 = sinf((x + time) * scale / 10.0f);
        float v2 = sinf((y + time) * scale / 8.0f);
        float v3 = sinf((x + y + time) * scale / 6.0f);
        float plasma = (v1 + v2 + v3) / 3.0f; // -1.0 to 1.0
        
        // Convert to 0-1
        plasma = (plasma + 1.0f) * 0.5f;
        
        // Modulate brightness
        uint8_t level = (uint8_t)(plasma * 255);
        buffer[i] = scale_rgb(buffer[i], level);
    }
}

/**
 * @brief Process sepia effect
 */
static void process_sepia(ts_led_rgb_t *buffer, size_t count,
                         const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)config;
    (void)time_ms;
    
    for (size_t i = 0; i < count; i++) {
        uint8_t r = buffer[i].r;
        uint8_t g = buffer[i].g;
        uint8_t b = buffer[i].b;
        
        buffer[i].r = clamp_u8((r * 393 + g * 769 + b * 189) >> 10);
        buffer[i].g = clamp_u8((r * 349 + g * 686 + b * 168) >> 10);
        buffer[i].b = clamp_u8((r * 272 + g * 534 + b * 131) >> 10);
    }
}

/**
 * @brief Process posterize effect
 */
static void process_posterize(ts_led_rgb_t *buffer, size_t count,
                              const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)time_ms;
    uint8_t levels = config->params.posterize.levels;
    if (levels < 2) levels = 2;
    if (levels > 16) levels = 16;
    
    uint8_t step = 256 / levels;
    
    for (size_t i = 0; i < count; i++) {
        buffer[i].r = (buffer[i].r / step) * step;
        buffer[i].g = (buffer[i].g / step) * step;
        buffer[i].b = (buffer[i].b / step) * step;
    }
}

/**
 * @brief Process contrast effect
 */
static void process_contrast(ts_led_rgb_t *buffer, size_t count,
                            const ts_led_effect_config_t *config, uint32_t time_ms)
{
    (void)time_ms;
    int8_t amount = config->params.contrast.amount;
    
    // Convert amount (-100 to +100) to factor (0.5 to 2.0)
    float factor = 1.0f + amount / 100.0f;
    
    for (size_t i = 0; i < count; i++) {
        int32_t r = ((int32_t)buffer[i].r - 128) * factor + 128;
        int32_t g = ((int32_t)buffer[i].g - 128) * factor + 128;
        int32_t b = ((int32_t)buffer[i].b - 128) * factor + 128;
        
        buffer[i].r = clamp_u8(r);
        buffer[i].g = clamp_u8(g);
        buffer[i].b = clamp_u8(b);
    }
}

/*===========================================================================*/
/*                              Public API                                    */
/*===========================================================================*/

esp_err_t ts_led_effect_apply(ts_led_layer_t layer, const ts_led_effect_config_t *config)
{
    if (!layer || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (config->type >= TS_LED_EFFECT_MAX) {
        ESP_LOGE(TAG, "Invalid effect type: %d", config->type);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Copy configuration
    memcpy(&layer->post_effect, config, sizeof(ts_led_effect_config_t));
    layer->effect_start_time = esp_log_timestamp();
    
    ESP_LOGI(TAG, "Applied effect '%s' to layer %d", 
             ts_led_effect_type_to_name(config->type), layer->id);
    
    return ESP_OK;
}

esp_err_t ts_led_effect_remove(ts_led_layer_t layer)
{
    if (!layer) {
        return ESP_ERR_INVALID_ARG;
    }
    
    layer->post_effect.type = TS_LED_EFFECT_NONE;
    
    return ESP_OK;
}

bool ts_led_effect_is_active(ts_led_layer_t layer)
{
    if (!layer) return false;
    return layer->post_effect.type != TS_LED_EFFECT_NONE;
}

ts_led_effect_type_t ts_led_effect_get_type(ts_led_layer_t layer)
{
    if (!layer) return TS_LED_EFFECT_NONE;
    return layer->post_effect.type;
}

const char *ts_led_effect_type_to_name(ts_led_effect_type_t type)
{
    for (size_t i = 0; i < NUM_EFFECTS; i++) {
        if (s_effect_names[i].type == type) {
            return s_effect_names[i].name;
        }
    }
    return "unknown";
}

ts_led_effect_type_t ts_led_effect_name_to_type(const char *name)
{
    if (!name) return TS_LED_EFFECT_NONE;
    
    for (size_t i = 0; i < NUM_EFFECTS; i++) {
        if (strcasecmp(s_effect_names[i].name, name) == 0) {
            return s_effect_names[i].type;
        }
    }
    return TS_LED_EFFECT_NONE;
}

size_t ts_led_effect_list(const char **names, size_t max_names)
{
    if (!names) {
        return NUM_EFFECTS - 1; // Exclude "none"
    }
    
    size_t count = 0;
    for (size_t i = 1; i < NUM_EFFECTS && count < max_names; i++) {
        names[count++] = s_effect_names[i].name;
    }
    
    return count;
}

void ts_led_effect_process(ts_led_layer_t layer, ts_led_rgb_t *buffer,
                           size_t count, uint16_t width, uint16_t height,
                           uint32_t time_ms)
{
    if (!layer || !buffer || count == 0) return;
    
    const ts_led_effect_config_t *config = &layer->post_effect;
    
    switch (config->type) {
        case TS_LED_EFFECT_NONE:
            break;
            
        case TS_LED_EFFECT_BRIGHTNESS:
            process_brightness(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_PULSE:
            process_pulse(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_BLINK:
            process_blink(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_FADE_IN:
            process_fade_in(layer, buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_FADE_OUT:
            process_fade_out(layer, buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_BREATHING:
            process_breathing(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_COLOR_SHIFT:
            process_color_shift(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_SATURATION:
            process_saturation(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_INVERT:
            process_invert(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_GRAYSCALE:
            process_grayscale(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_COLOR_TEMP:
            process_color_temp(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_SCANLINE:
            process_scanline(buffer, count, width, height, config, time_ms);
            break;
            
        case TS_LED_EFFECT_WAVE:
            process_wave(buffer, count, width, height, config, time_ms);
            break;
            
        case TS_LED_EFFECT_STROBE:
            process_strobe(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_NOISE:
            process_noise(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_GLITCH:
            process_glitch(buffer, count, width, height, config, time_ms);
            break;
            
        case TS_LED_EFFECT_RAINBOW:
            process_rainbow(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_SPARKLE:
            process_sparkle(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_PLASMA:
            process_plasma(buffer, count, width, height, config, time_ms);
            break;
            
        case TS_LED_EFFECT_SEPIA:
            process_sepia(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_POSTERIZE:
            process_posterize(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_CONTRAST:
            process_contrast(buffer, count, config, time_ms);
            break;
            
        case TS_LED_EFFECT_BLUR:
        case TS_LED_EFFECT_PIXELATE:
        case TS_LED_EFFECT_MIRROR:
            // These require additional implementation with 2D buffer access
            ESP_LOGW(TAG, "Effect '%s' not yet implemented", 
                     ts_led_effect_type_to_name(config->type));
            break;
            
        default:
            break;
    }
}
