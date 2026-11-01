/**
 * @file ts_led_animation.c
 * @brief Built-in LED Animations (Procedural)
 * 
 * 针对三种不同形态的LED设备设计专属动画:
 * - Touch (点光源): 呼吸、脉冲、闪烁、颜色渐变
 * - Board (环形): 追逐、旋转彩虹、流星、呼吸波
 * - Matrix (矩阵): 火焰、雨滴、等离子、波纹
 * 
 * Note: These were previously called "effects" but are actually
 * procedural animations that generate content. True post-processing
 * effects are defined in ts_led_effect.c.
 * 
 * @version 2.0.0 - Renamed from ts_led_effects.c
 */

#include "ts_led_private.h"
#include "ts_led_animation.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>

#define TAG "led_animation"

/*===========================================================================*/
/*                     动画状态缓冲区 (分配到 PSRAM)                           */
/*===========================================================================*/

/* 动画状态结构体 - 集中管理所有动画需要的静态数据 */
typedef struct {
    /* Fire 动画 */
    uint8_t fire_heat[1024];    // 32x32 max
    
    /* Rain 动画 */
    uint8_t rain_drop_y[32];
    uint8_t rain_drop_life[32];
    bool rain_drop_active[32];
    ts_led_rgb_t rain_color;
    
    /* Coderain 动画 */
    int8_t code_drop_y[64];
    uint8_t code_drop_len[64];
    uint8_t code_drop_wait[64];
    uint8_t code_drop_speed[64];
    uint8_t code_drop_life[64];
    bool code_initialized;
} anim_state_t;

static anim_state_t *s_anim_state = NULL;

/* 初始化动画状态（分配到 PSRAM） */
static anim_state_t *get_anim_state(void)
{
    if (!s_anim_state) {
        /* 优先使用 PSRAM */
        s_anim_state = heap_caps_calloc(1, sizeof(anim_state_t), 
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_anim_state) {
            /* 回退到普通内存 */
            s_anim_state = calloc(1, sizeof(anim_state_t));
        }
        if (s_anim_state) {
            /* 初始化 coderain */
            for (int i = 0; i < 64; i++) {
                s_anim_state->code_drop_y[i] = -1;
            }
        }
    }
    return s_anim_state;
}

/*===========================================================================*/
/*                       通用动画 (所有设备)                                   */
/*===========================================================================*/

/* Animation: Rainbow - 彩虹渐变 */
static void anim_rainbow(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t count = l->device->config.led_count;
    uint8_t offset = (time_ms / 20) & 0xFF;
    
    for (int i = 0; i < count; i++) {
        ts_led_rgb_t c = ts_led_color_wheel((i * 256 / count + offset) & 0xFF);
        ts_led_set_pixel(layer, i, c);
    }
}

/* Animation: Breathing - 呼吸灯 */
static void anim_breathing(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_rgb_t *color = (ts_led_rgb_t *)data;
    ts_led_rgb_t c = color ? *color : TS_LED_WHITE;
    
    float phase = (time_ms % 2000) / 2000.0f * 3.14159f * 2;
    uint8_t brightness = (uint8_t)((sinf(phase) + 1.0f) * 127);
    
    ts_led_rgb_t scaled = ts_led_scale_color(c, brightness);
    ts_led_fill(layer, scaled);
}

/* Animation: Solid Color - 纯色填充 */
static void anim_solid(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_rgb_t *color = (ts_led_rgb_t *)data;
    ts_led_fill(layer, color ? *color : TS_LED_WHITE);
}

/*===========================================================================*/
/*                     Touch 专属动画 (点光源)                                 */
/*===========================================================================*/

/* Animation: Pulse - 脉冲闪烁 (快速闪烁后渐灭) */
static void anim_pulse(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    uint32_t cycle = time_ms % 1000;
    uint8_t brightness;
    
    if (cycle < 100) {
        brightness = 255;  // 快速闪亮
    } else if (cycle < 200) {
        brightness = 0;
    } else if (cycle < 300) {
        brightness = 200;
    } else {
        // 渐灭
        brightness = 200 * (1000 - cycle) / 700;
    }
    
    ts_led_fill(layer, ts_led_scale_color(TS_LED_WHITE, brightness));
}

/* Animation: Color Cycle - 颜色循环 (点光源用单色渐变更合适) */
static void anim_color_cycle(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    uint8_t hue = (time_ms / 30) & 0xFF;
    ts_led_rgb_t c = ts_led_color_wheel(hue);
    ts_led_fill(layer, c);
}

/* Animation: Heartbeat - 心跳 */
static void anim_heartbeat(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    uint32_t cycle = time_ms % 1200;
    uint8_t brightness = 0;
    
    if (cycle < 100) {
        brightness = cycle * 255 / 100;
    } else if (cycle < 200) {
        brightness = 255 - (cycle - 100) * 200 / 100;
    } else if (cycle < 300) {
        brightness = 55 + (cycle - 200) * 200 / 100;
    } else if (cycle < 500) {
        brightness = 255 - (cycle - 300) * 255 / 200;
    }
    
    ts_led_fill(layer, ts_led_scale_color(TS_LED_RED, brightness));
}

/*===========================================================================*/
/*                     Board 专属动画 (环形灯带)                               */
/*===========================================================================*/

/* Animation: Chase - 追逐/跑马灯 */
static void anim_chase(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t count = l->device->config.led_count;
    uint16_t pos = (time_ms / 50) % count;
    
    ts_led_fill(layer, TS_LED_BLACK);
    
    // 拖尾效果
    for (int i = 0; i < 5; i++) {
        uint16_t idx = (pos + count - i) % count;
        uint8_t fade = 255 - i * 50;
        ts_led_set_pixel(layer, idx, ts_led_scale_color(TS_LED_CYAN, fade));
    }
}

/* Animation: Comet - 流星 (单向快速移动带长拖尾) */
static void anim_comet(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t count = l->device->config.led_count;
    uint16_t pos = (time_ms / 30) % count;
    
    // 淡化当前画面
    for (int i = 0; i < count; i++) {
        l->buffer[i] = ts_led_scale_color(l->buffer[i], 180);
    }
    
    // 画流星头部
    ts_led_set_pixel(layer, pos, TS_LED_WHITE);
    if (pos > 0) {
        ts_led_set_pixel(layer, pos - 1, ts_led_scale_color(TS_LED_CYAN, 200));
    }
}

/* Animation: Spin - 旋转彩虹 (环形专用) */
static void anim_spin(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t count = l->device->config.led_count;
    uint16_t offset = (time_ms / 25) % count;
    
    for (int i = 0; i < count; i++) {
        uint16_t idx = (i + offset) % count;
        // 渐变色：一半亮一半暗
        uint8_t brightness = (i < count / 2) ? 255 : 50;
        ts_led_rgb_t c = ts_led_color_wheel(i * 256 / count);
        ts_led_set_pixel(layer, idx, ts_led_scale_color(c, brightness));
    }
}

/* Animation: Breathe Wave - 呼吸波 (环形波浪式呼吸) */
static void anim_breathe_wave(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t count = l->device->config.led_count;
    float phase_base = (time_ms % 3000) / 3000.0f * 3.14159f * 2;
    
    for (int i = 0; i < count; i++) {
        float phase = phase_base + (float)i / count * 3.14159f * 2;
        uint8_t brightness = (uint8_t)((sinf(phase) + 1.0f) * 127);
        ts_led_rgb_t c = ts_led_color_wheel(i * 256 / count);
        ts_led_set_pixel(layer, i, ts_led_scale_color(c, brightness));
    }
}

/*===========================================================================*/
/*                     Matrix 专属动画 (矩阵屏)                                */
/*===========================================================================*/

/* Animation: Fire - 火焰 (向上燃烧) */
static void anim_fire(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t width = l->device->config.width > 0 ? l->device->config.width : 32;
    uint16_t height = l->device->config.height > 0 ? l->device->config.height : 32;
    
    anim_state_t *state = get_anim_state();
    if (!state) return;
    uint8_t *heat = state->fire_heat;
    
    // 冷却
    for (int i = 0; i < width * height; i++) {
        heat[i] = (heat[i] > 20) ? heat[i] - (esp_random() & 15) - 5 : 0;
    }
    
    // 热量上升
    for (int y = height - 1; y >= 2; y--) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            heat[idx] = (heat[idx - width] + heat[idx - width * 2] + 
                        heat[idx - width * 2]) / 3;
        }
    }
    
    // 底部点火
    for (int x = 0; x < width; x++) {
        if ((esp_random() & 0xFF) < 150) {
            heat[x] = 180 + (esp_random() & 75);
        }
    }
    
    // 转换为颜色
    for (int i = 0; i < width * height; i++) {
        uint8_t t = heat[i];
        ts_led_rgb_t c;
        if (t < 85) {
            c = TS_LED_RGB(t * 3, 0, 0);
        } else if (t < 170) {
            c = TS_LED_RGB(255, (t - 85) * 3, 0);
        } else {
            c = TS_LED_RGB(255, 255, (t - 170) * 3);
        }
        ts_led_set_pixel(layer, i, c);
    }
}

/* Animation: Rain - 简单雨滴效果 */
/* 雨滴颜色优先级: user_data (通过 --color 参数) > 默认蓝色 */
static void anim_rain(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t width = l->device->config.width > 0 ? l->device->config.width : 32;
    uint16_t height = l->device->config.height > 0 ? l->device->config.height : 32;
    
    anim_state_t *state = get_anim_state();
    if (!state) return;
    
    // 使用 PSRAM 中的状态
    uint8_t *drop_y = state->rain_drop_y;
    uint8_t *drop_life = state->rain_drop_life;
    bool *drop_active = state->rain_drop_active;
    
    // 检测动画刚启动（anim_last_time == 0 表示刚启动）
    if (l->anim_last_time == 0) {
        // 重置状态
        for (int i = 0; i < 32; i++) {
            drop_active[i] = false;
            drop_y[i] = 0;
        }
        // 清空画布
        memset(l->buffer, 0, width * height * sizeof(ts_led_rgb_t));
        
        // 确定雨滴颜色
        if (data != NULL) {
            // 使用传入的颜色（通过 --color 参数）
            state->rain_color = *(ts_led_rgb_t *)data;
        } else {
            // 默认淡蓝色
            state->rain_color = TS_LED_RGB(100, 150, 255);
        }
    }
    
    // 衰减背景（余晖）
    for (int i = 0; i < width * height; i++) {
        l->buffer[i] = ts_led_scale_color(l->buffer[i], 160);
    }
    
    // 更新每列
    for (int x = 0; x < width && x < 32; x++) {
        if (drop_active[x]) {
            drop_y[x]++;
            drop_life[x]--;
            
            // 寿命耗尽或到达底部则消失
            if (drop_life[x] == 0 || drop_y[x] >= height) {
                drop_active[x] = false;
            }
        }
        
        // 随机激活（降低密度：1/80 概率）
        if (!drop_active[x] && (esp_random() % 80) == 0) {
            drop_active[x] = true;
            drop_y[x] = 0;
            // 随机寿命：8-28步，有些落不到底
            drop_life[x] = 8 + (esp_random() % 21);
        }
        
        // 绘制雨滴
        if (drop_active[x] && drop_y[x] < height) {
            int idx = drop_y[x] * width + x;
            ts_led_set_pixel(layer, idx, state->rain_color);
        }
    }
}

/* Animation: Coderain - 黑客帝国代码雨效果 */
static void anim_coderain(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t width = l->device->config.width > 0 ? l->device->config.width : 32;
    uint16_t height = l->device->config.height > 0 ? l->device->config.height : 32;
    
    anim_state_t *state = get_anim_state();
    if (!state) return;
    
    // 使用 PSRAM 中的状态
    int8_t *drop_y = state->code_drop_y;
    uint8_t *drop_len = state->code_drop_len;
    uint8_t *drop_wait = state->code_drop_wait;
    uint8_t *drop_speed = state->code_drop_speed;
    uint8_t *drop_life = state->code_drop_life;
    
    if (!state->code_initialized) {
        for (int i = 0; i < 64; i++) {
            drop_y[i] = -1;
            drop_wait[i] = 0;
        }
        state->code_initialized = true;
    }
    
    // 余晖衰减
    for (int i = 0; i < width * height; i++) {
        l->buffer[i] = ts_led_scale_color(l->buffer[i], 120);  // 保留47%
    }
    
    // 更新并绘制每列（隔列，降低横向密度）
    for (int x = 0; x < width && x < 64; x += 2) {
        // 速度控制
        if (drop_y[x] >= 0) {
            drop_wait[x]++;
            if (drop_wait[x] >= drop_speed[x]) {
                drop_wait[x] = 0;
                drop_y[x]++;
                drop_life[x]--;
                
                // 寿命耗尽或离开屏幕则消失
                if (drop_life[x] == 0 || drop_y[x] > height + drop_len[x]) {
                    drop_y[x] = -1;
                }
            }
        }
        
        // 随机激活新雨滴
        if (drop_y[x] < 0) {
            if ((esp_random() % 180) < 1) {
                drop_y[x] = 0;
                drop_len[x] = 2 + (esp_random() % 4);    // 2-5 长度
                drop_speed[x] = 2 + (esp_random() % 2);  // 2-3 帧/步
                drop_life[x] = 10 + (esp_random() % 25); // 10-34步寿命
                drop_wait[x] = 0;
            }
            continue;
        }
        
        // 绘制雨滴
        int head_y = drop_y[x];
        
        // 计算下落衰减系数（越往下越暗）
        float fall_fade = 1.0f - ((float)head_y / (float)height) * 0.6f;
        
        // 头部
        if (head_y >= 0 && head_y < height) {
            int idx = head_y * width + x;
            uint8_t head_g = (uint8_t)(100 * fall_fade);
            uint8_t head_r = (uint8_t)(25 * fall_fade);
            uint8_t head_b = (uint8_t)(35 * fall_fade);
            ts_led_set_pixel(layer, idx, TS_LED_RGB(head_r, head_g, head_b));
        }
        
        // 短尾巴
        for (int i = 1; i <= drop_len[x]; i++) {
            int y = head_y - i;
            if (y >= 0 && y < height) {
                int idx = y * width + x;
                float ratio = 1.0f - ((float)i / drop_len[x]);
                float tail_fade = 1.0f - ((float)y / (float)height) * 0.6f;
                uint8_t g = (uint8_t)((15 + ratio * 45) * tail_fade);
                uint8_t r = (uint8_t)(ratio * 8 * tail_fade);
                uint8_t b = (uint8_t)(ratio * 10 * tail_fade);
                ts_led_set_pixel(layer, idx, TS_LED_RGB(r, g, b));
            }
        }
    }
}

/* Animation: Plasma - 等离子 */
static void anim_plasma(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t width = l->device->config.width > 0 ? l->device->config.width : 32;
    uint16_t height = l->device->config.height > 0 ? l->device->config.height : 32;
    float t = time_ms / 1000.0f;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float v = sinf(x / 4.0f + t);
            v += sinf(y / 4.0f + t);
            v += sinf((x + y) / 4.0f + t);
            v += sinf(sqrtf(x * x + y * y) / 4.0f + t);
            
            uint8_t color_idx = (uint8_t)((v + 4) * 32) & 0xFF;
            ts_led_rgb_t c = ts_led_color_wheel(color_idx);
            ts_led_set_pixel(layer, y * width + x, c);
        }
    }
}

/* Animation: Ripple - 水波纹 */
static void anim_ripple(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t width = l->device->config.width > 0 ? l->device->config.width : 32;
    uint16_t height = l->device->config.height > 0 ? l->device->config.height : 32;
    float cx = width / 2.0f;
    float cy = height / 2.0f;
    float radius = (time_ms / 50) % 40;
    
    ts_led_fill(layer, TS_LED_BLACK);
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float dist = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
            float diff = fabsf(dist - radius);
            if (diff < 3) {
                uint8_t brightness = 255 - (uint8_t)(diff * 85);
                ts_led_set_pixel(layer, y * width + x, 
                    ts_led_scale_color(TS_LED_BLUE, brightness));
            }
        }
    }
}

/* Animation: Sparkle */
static void anim_sparkle(ts_led_layer_t layer, uint32_t time_ms, void *data)
{
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t count = l->device->config.led_count;
    
    for (int i = 0; i < count; i++) {
        l->buffer[i] = ts_led_scale_color(l->buffer[i], 200);
    }
    
    // 根据LED数量调整闪烁频率
    int sparkle_chance = (count > 100) ? 0x03 : 0x0F;
    if ((esp_random() & sparkle_chance) == 0) {
        int pos = esp_random() % count;
        ts_led_set_pixel(layer, pos, TS_LED_WHITE);
    }
}

/*===========================================================================*/
/*                          动画注册表                                        */
/*===========================================================================*/

/* 设备类型标志 */
#define ANIM_ALL      0x07
#define ANIM_TOUCH    0x01
#define ANIM_BOARD    0x02
#define ANIM_MATRIX   0x04

typedef struct {
    const char *name;
    ts_led_animation_fn_t func;
    uint32_t frame_interval_ms;
    void *user_data;
    uint8_t device_types;  // 适用的设备类型
} ts_led_animation_entry_t;

static const ts_led_animation_entry_t s_animation_registry[] = {
    // 通用动画
    {"rainbow",       anim_rainbow,      20,  NULL, ANIM_ALL},
    {"breathing",     anim_breathing,    20,  NULL, ANIM_ALL},
    {"solid",         anim_solid,       100,  NULL, ANIM_ALL},
    {"sparkle",       anim_sparkle,      30,  NULL, ANIM_ALL},
    
    // Touch 专属 (点光源)
    {"pulse",         anim_pulse,        20,  NULL, ANIM_TOUCH},
    {"color_cycle",   anim_color_cycle,  30,  NULL, ANIM_TOUCH},
    {"heartbeat",     anim_heartbeat,    20,  NULL, ANIM_TOUCH},
    
    // Board 专属 (环形)
    {"chase",         anim_chase,        50,  NULL, ANIM_BOARD},
    {"comet",         anim_comet,        30,  NULL, ANIM_BOARD},
    {"spin",          anim_spin,         25,  NULL, ANIM_BOARD},
    {"breathe_wave",  anim_breathe_wave, 30,  NULL, ANIM_BOARD},
    
    // Matrix 专属 (矩阵)
    {"fire",          anim_fire,         30,  NULL, ANIM_MATRIX},
    {"rain",          anim_rain,         50,  NULL, ANIM_MATRIX},
    {"coderain",      anim_coderain,     50,  NULL, ANIM_MATRIX},
    {"plasma",        anim_plasma,       30,  NULL, ANIM_MATRIX},
    {"ripple",        anim_ripple,       30,  NULL, ANIM_MATRIX},
    
    {NULL, NULL, 0, NULL, 0}
};

/*===========================================================================*/
/*                          公开 API                                          */
/*===========================================================================*/

const ts_led_animation_def_t *ts_led_animation_get_builtin(const char *name)
{
    for (int i = 0; s_animation_registry[i].name; i++) {
        if (strcmp(s_animation_registry[i].name, name) == 0) {
            // 返回一个临时的 ts_led_animation_def_t 结构
            static ts_led_animation_def_t anim;
            anim.name = s_animation_registry[i].name;
            anim.func = s_animation_registry[i].func;
            anim.frame_interval_ms = s_animation_registry[i].frame_interval_ms;
            anim.user_data = s_animation_registry[i].user_data;
            return &anim;
        }
    }
    return NULL;
}

size_t ts_led_animation_list_builtin(const char **names, size_t max_names)
{
    size_t count = 0;
    for (int i = 0; s_animation_registry[i].name && count < max_names; i++) {
        if (names) names[count] = s_animation_registry[i].name;
        count++;
    }
    return count;
}

/**
 * @brief 获取指定设备类型的动画列表
 */
size_t ts_led_animation_list_for_device(ts_led_layout_t layout, const char **names, size_t max_names)
{
    uint8_t type_flag;
    switch (layout) {
        case TS_LED_LAYOUT_STRIP:
            type_flag = ANIM_TOUCH;  // 单颗LED视为点光源
            break;
        case TS_LED_LAYOUT_RING:
            type_flag = ANIM_BOARD;
            break;
        case TS_LED_LAYOUT_MATRIX:
            type_flag = ANIM_MATRIX;
            break;
        default:
            type_flag = ANIM_ALL;
    }
    
    size_t count = 0;
    for (int i = 0; s_animation_registry[i].name && count < max_names; i++) {
        if (s_animation_registry[i].device_types & type_flag) {
            if (names) names[count] = s_animation_registry[i].name;
            count++;
        }
    }
    return count;
}

esp_err_t ts_led_animation_start(ts_led_layer_t layer, const ts_led_animation_def_t *animation)
{
    if (!layer || !animation) return ESP_ERR_INVALID_ARG;
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    
    xSemaphoreTake(l->device->mutex, portMAX_DELAY);
    ts_led_image_release_animation(layer);
    l->anim_fn = animation->func;
    l->anim_data = animation->user_data;
    if (animation->user_data && (animation->func == anim_breathing ||
        animation->func == anim_solid || animation->func == anim_rain)) {
        l->anim_color = *(const ts_led_rgb_t *)animation->user_data;
        l->anim_data = &l->anim_color;
    }
    l->anim_interval = animation->frame_interval_ms;
    l->anim_last_time = 0;
    xSemaphoreGive(l->device->mutex);
    
    return ESP_OK;
}

esp_err_t ts_led_animation_stop(ts_led_layer_t layer)
{
    if (!layer) return ESP_ERR_INVALID_ARG;
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    xSemaphoreTake(l->device->mutex, portMAX_DELAY);
    ts_led_image_release_animation(layer);
    l->anim_fn = NULL;
    l->anim_data = NULL;
    xSemaphoreGive(l->device->mutex);
    return ESP_OK;
}

bool ts_led_animation_is_running(ts_led_layer_t layer)
{
    if (!layer) return false;
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    xSemaphoreTake(l->device->mutex, portMAX_DELAY);
    bool running = l->anim_fn != NULL;
    xSemaphoreGive(l->device->mutex);
    return running;
}

ts_led_animation_state_t ts_led_animation_get_state(ts_led_layer_t layer)
{
    if (!layer) return TS_LED_ANIMATION_STOPPED;
    return ts_led_animation_is_running(layer) ? TS_LED_ANIMATION_PLAYING : TS_LED_ANIMATION_STOPPED;
}
