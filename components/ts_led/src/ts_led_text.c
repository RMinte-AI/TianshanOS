/**
 * @file ts_led_text.c
 * @brief TianShanOS LED Text Rendering Implementation
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_led_text.h"
#include "ts_led.h"
#include "ts_led_font.h"
#include "ts_led_private.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include <string.h>

static const char *TAG = "ts_text";

/*===========================================================================*/
/*                          UTF-8 Helpers                                     */
/*===========================================================================*/

int ts_utf8_decode(const char **text, uint16_t *codepoint)
{
    if (!text || !*text || !codepoint) return 0;
    
    const uint8_t *s = (const uint8_t *)*text;
    
    if (*s == 0) {
        *codepoint = 0;
        return 0;
    }
    
    // Single byte (ASCII)
    if ((*s & 0x80) == 0) {
        *codepoint = *s;
        *text += 1;
        return 1;
    }
    
    // Two bytes (0x80 - 0x7FF)
    if ((*s & 0xE0) == 0xC0) {
        if ((s[1] & 0xC0) != 0x80) {
            *codepoint = '?';
            *text += 1;
            return 1;
        }
        *codepoint = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *text += 2;
        return 2;
    }
    
    // Three bytes (0x800 - 0xFFFF) - covers CJK
    if ((*s & 0xF0) == 0xE0) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) {
            *codepoint = '?';
            *text += 1;
            return 1;
        }
        *codepoint = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *text += 3;
        return 3;
    }
    
    // Four bytes (0x10000 - 0x10FFFF) - beyond BMP, skip for now
    if ((*s & 0xF8) == 0xF0) {
        *codepoint = '?';  // We only support BMP (16-bit codepoints)
        *text += 4;
        return 4;
    }
    
    // Invalid UTF-8
    *codepoint = '?';
    *text += 1;
    return 1;
}

int ts_utf8_strlen(const char *text)
{
    if (!text) return 0;
    
    int count = 0;
    uint16_t cp;
    
    while (ts_utf8_decode(&text, &cp) > 0 && cp != 0) {
        count++;
    }
    
    return count;
}

/*===========================================================================*/
/*                          Rendering Functions                               */
/*===========================================================================*/

/**
 * @brief Calculate actual width of a glyph bitmap (excluding trailing empty columns)
 * 
 * @param bitmap Glyph bitmap data
 * @param width  Nominal glyph width
 * @param height Glyph height
 * @return Actual width used by glyph pixels (minimum 1)
 */
static uint8_t get_glyph_actual_width(const uint8_t *bitmap, uint8_t width, uint8_t height)
{
    uint8_t actual_width = 0;
    
    // Scan each column from left to right
    for (int gx = 0; gx < width; gx++) {
        bool col_has_pixel = false;
        
        // Check all rows in this column
        for (int gy = 0; gy < height; gy++) {
            int bit_idx = gy * width + gx;
            int byte_idx = bit_idx / 8;
            int bit_pos = 7 - (bit_idx % 8);
            
            if ((bitmap[byte_idx] >> bit_pos) & 1) {
                col_has_pixel = true;
                break;
            }
        }
        
        if (col_has_pixel) {
            actual_width = gx + 1;  // Update to include this column
        }
    }
    
    // Return at least 1 pixel for space characters, etc.
    return actual_width > 0 ? actual_width : width / 2;
}

/**
 * @brief Draw a single glyph bitmap to layer
 */
static void draw_glyph(ts_led_layer_t layer, const uint8_t *bitmap,
                       int16_t x, int16_t y, uint8_t width, uint8_t height,
                       ts_led_rgb_t color, ts_led_rgb_t bg_color, 
                       bool transparent_bg, uint16_t layer_width, uint16_t layer_height)
{
    int bit_idx = 0;
    
    for (int gy = 0; gy < height; gy++) {
        for (int gx = 0; gx < width; gx++) {
            int px = x + gx;
            int py = y + gy;
            
            // Bounds check
            if (px < 0 || px >= layer_width || py < 0 || py >= layer_height) {
                bit_idx++;
                continue;
            }
            
            // Get bit value
            int byte_idx = bit_idx / 8;
            int bit_pos = 7 - (bit_idx % 8);
            bool pixel_on = (bitmap[byte_idx] >> bit_pos) & 1;
            
            if (pixel_on) {
                ts_led_set_pixel_xy(layer, px, py, color);
            } else if (!transparent_bg) {
                ts_led_set_pixel_xy(layer, px, py, bg_color);
            }
            
            bit_idx++;
        }
    }
}

esp_err_t ts_led_text_draw_char(ts_led_layer_t layer, uint16_t codepoint,
                                 int16_t x, int16_t y, ts_font_t *font,
                                 ts_led_rgb_t color)
{
    if (!layer || !font) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const uint8_t *bitmap;
    esp_err_t err = ts_font_get_glyph(font, codepoint, &bitmap);
    if (err != ESP_OK) {
        return err;
    }
    
    // Get layer dimensions
    // Access layer's device for dimensions
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t layer_width = l->device->config.width;
    uint16_t layer_height = l->device->config.height;
    
    draw_glyph(layer, bitmap, x, y, 
               font->header.width, font->header.height,
               color, TS_LED_BLACK, true,
               layer_width, layer_height);
    
    return ESP_OK;
}

esp_err_t ts_led_text_measure(const char *text, ts_font_t *font,
                               const ts_text_options_t *options,
                               ts_text_metrics_t *metrics)
{
    if (!text || !font || !metrics) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_text_options_t opts = TS_TEXT_DEFAULT_OPTIONS();
    if (options) {
        opts = *options;
    }
    
    memset(metrics, 0, sizeof(ts_text_metrics_t));
    
    const char *p = text;
    uint16_t cp;
    
    while (ts_utf8_decode(&p, &cp) > 0 && cp != 0) {
        metrics->char_count++;
        
        // Calculate character width
        int char_width;
        if (opts.proportional) {
            // Get actual glyph width
            const uint8_t *bitmap;
            if (ts_font_get_glyph(font, cp, &bitmap) == ESP_OK) {
                char_width = get_glyph_actual_width(bitmap, font->header.width, font->header.height);
            } else {
                char_width = font->header.width / 2;  // Missing glyph
            }
        } else {
            char_width = font->header.width;
        }
        
        metrics->width += char_width + opts.spacing;
    }
    
    // Remove trailing spacing
    if (metrics->char_count > 0) {
        metrics->width -= opts.spacing;
    }
    
    metrics->height = font->header.height;
    metrics->line_count = 1;
    
    return ESP_OK;
}

esp_err_t ts_led_text_draw(ts_led_layer_t layer, const char *text,
                            int16_t x, int16_t y, ts_font_t *font,
                            const ts_text_options_t *options)
{
    if (!layer || !text || !font) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_text_options_t opts = TS_TEXT_DEFAULT_OPTIONS();
    if (options) {
        opts = *options;
    }
    
    // Get layer dimensions
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)layer;
    uint16_t layer_width = l->device->config.width;
    uint16_t layer_height = l->device->config.height;
    
    // Calculate text metrics for alignment
    ts_text_metrics_t metrics;
    ts_led_text_measure(text, font, &opts, &metrics);
    
    // Apply alignment
    int16_t draw_x = x + opts.x_offset;
    int16_t draw_y = y + opts.y_offset;
    
    switch (opts.align) {
        case TS_TEXT_ALIGN_CENTER:
            draw_x = (layer_width - metrics.width) / 2 + opts.x_offset;
            break;
        case TS_TEXT_ALIGN_RIGHT:
            draw_x = layer_width - metrics.width + opts.x_offset;
            break;
        case TS_TEXT_ALIGN_LEFT:
        default:
            break;
    }
    
    // Draw each character
    const char *p = text;
    uint16_t cp;
    int chars_drawn = 0;
    
    while (ts_utf8_decode(&p, &cp) > 0 && cp != 0) {
        // Check if character fits
        if (draw_x >= layer_width) {
            if (!opts.wrap) break;
            // Wrap to next line
            draw_x = opts.x_offset;
            draw_y += font->header.height + 1;
            if (draw_y >= layer_height) break;
        }
        
        // Get glyph
        const uint8_t *bitmap;
        esp_err_t err = ts_font_get_glyph(font, cp, &bitmap);
        
        int char_width;
        if (err == ESP_OK) {
            draw_glyph(layer, bitmap, draw_x, draw_y,
                       font->header.width, font->header.height,
                       opts.color, opts.bg_color, opts.transparent_bg,
                       layer_width, layer_height);
            
            // Calculate advance width
            if (opts.proportional) {
                char_width = get_glyph_actual_width(bitmap, font->header.width, font->header.height);
            } else {
                char_width = font->header.width;
            }
        } else {
            // Missing glyph - use half width for space
            char_width = opts.proportional ? font->header.width / 2 : font->header.width;
        }
        
        draw_x += char_width + opts.spacing;
        chars_drawn++;
    }
    
    ESP_LOGD(TAG, "Drew %d characters", chars_drawn);
    
    return ESP_OK;
}

esp_err_t ts_led_text_draw_on_device(const char *device_name, const char *text,
                                      ts_font_t *font, 
                                      const ts_text_options_t *options)
{
    if (!device_name || !text || !font) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_device_t device = ts_led_device_get(device_name);
    if (!device) {
        ESP_LOGE(TAG, "Device '%s' not found", device_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_led_layer_t layer = ts_led_layer_get(device, 0);
    if (!layer) {
        ESP_LOGE(TAG, "Failed to get layer for device '%s'", device_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Use default y position to vertically center text
    ts_text_options_t default_opts = TS_TEXT_DEFAULT_OPTIONS();
    ts_text_options_t opts = options ? *options : default_opts;
    
    uint8_t font_height;
    ts_font_get_size(font, NULL, &font_height);
    
    // Vertical centering if no y_offset specified
    if (opts.y_offset == 0 && options == NULL) {
        uint16_t layer_height = ((ts_led_layer_impl_t *)layer)->device->config.height;
        opts.y_offset = (layer_height - font_height) / 2;
    }
    
    esp_err_t err = ts_led_text_draw(layer, text, 0, 0, font, &opts);
    
    if (err == ESP_OK) {
        ts_led_device_refresh(device);
    }
    
    return err;
}

int ts_led_text_chars_in_width(const char *text, ts_font_t *font, 
                                uint16_t max_width)
{
    if (!text || !font) return 0;
    
    int count = 0;
    int current_width = 0;
    int char_width = font->header.width;
    
    const char *p = text;
    uint16_t cp;
    
    while (ts_utf8_decode(&p, &cp) > 0 && cp != 0) {
        if (current_width + char_width > max_width) {
            break;
        }
        current_width += char_width;
        count++;
    }
    
    return count;
}

/*===========================================================================*/
/*                       Text Overlay Layer Implementation                    */
/*===========================================================================*/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 文本覆盖层使用 layer 1（layer 0 留给动画/图像） */
#define TEXT_OVERLAY_LAYER_INDEX 1

/**
 * @brief 文本覆盖层状态（每设备一个）
 */
typedef struct {
    bool active;
    char text[256];              /**< 文本副本 */
    ts_font_t *font;
    ts_led_rgb_t color;
    int16_t base_x;              /**< 基准 X 位置 */
    int16_t base_y;              /**< 基准 Y 位置 */
    int16_t scroll_x;            /**< 当前滚动 X 偏移 */
    int16_t scroll_y;            /**< 当前滚动 Y 偏移 */
    ts_text_align_t align;       /**< 文本对齐 */
    ts_text_scroll_t scroll_dir;
    uint8_t scroll_speed;
    bool invert_on_overlap;
    bool loop_scroll;
    int16_t text_width;          /**< 预计算的文本宽度 */
    int16_t text_height;         /**< 预计算的文本高度 */
    ts_led_device_t device;
    uint32_t last_scroll_time;
    ts_led_layer_t overlay_layer; /**< 覆盖层句柄（layer 1） */
} ts_text_overlay_state_t;

/* 最多支持 3 个设备的覆盖层 */
#define MAX_OVERLAY_DEVICES 3
static ts_text_overlay_state_t s_overlays[MAX_OVERLAY_DEVICES];

/* 覆盖层渲染任务句柄 */
static TaskHandle_t s_overlay_task = NULL;

/**
 * @brief 根据设备名获取覆盖层状态索引
 */
static int get_overlay_index(const char *device_name)
{
    if (!device_name) return -1;
    
    // 将用户友好名称转换为内部名称
    const char *internal = device_name;
    if (strcmp(device_name, "touch") == 0) internal = "led_touch";
    else if (strcmp(device_name, "board") == 0) internal = "led_board";
    else if (strcmp(device_name, "matrix") == 0) internal = "led_matrix";
    
    if (strcmp(internal, "led_matrix") == 0) return 0;
    if (strcmp(internal, "led_board") == 0) return 1;
    if (strcmp(internal, "led_touch") == 0) return 2;
    return -1;
}

/**
 * @brief 反转颜色
 */
static inline ts_led_rgb_t invert_color(ts_led_rgb_t c)
{
    return (ts_led_rgb_t){255 - c.r, 255 - c.g, 255 - c.b};
}

/**
 * @brief 检测像素是否为"黑色"（亮度阈值）
 */
static inline bool is_dark_pixel(ts_led_rgb_t c)
{
    // 如果总亮度 < 30，认为是暗色
    return (c.r + c.g + c.b) < 30;
}

/**
 * @brief 获取基础图层像素（用于判断是否需要反色）
 */
static ts_led_rgb_t get_base_pixel(ts_led_layer_impl_t *layer, int16_t x, int16_t y)
{
    if (!layer || !layer->buffer) return TS_LED_BLACK;
    
    uint16_t w = layer->device->config.width;
    uint16_t h = layer->device->config.height;
    
    if (x < 0 || x >= w || y < 0 || y >= h) return TS_LED_BLACK;
    
    // 计算索引（与 ts_led_set_pixel_xy 相同的逻辑）
    ts_led_device_impl_t *dev = layer->device;
    uint16_t tx = x, ty = y;
    
    switch (dev->config.origin) {
        case TS_LED_ORIGIN_TOP_RIGHT:
            tx = w - 1 - x;
            break;
        case TS_LED_ORIGIN_BOTTOM_LEFT:
            ty = h - 1 - y;
            break;
        case TS_LED_ORIGIN_BOTTOM_RIGHT:
            tx = w - 1 - x;
            ty = h - 1 - y;
            break;
        case TS_LED_ORIGIN_TOP_LEFT:
        default:
            break;
    }
    
    uint16_t index;
    switch (dev->config.scan) {
        case TS_LED_SCAN_ZIGZAG_ROWS:
            if (ty % 2 == 0) {
                index = ty * w + tx;
            } else {
                index = ty * w + (w - 1 - tx);
            }
            break;
        case TS_LED_SCAN_COLUMNS:
            index = tx * h + ty;
            break;
        case TS_LED_SCAN_ZIGZAG_COLS:
            if (tx % 2 == 0) {
                index = tx * h + ty;
            } else {
                index = tx * h + (h - 1 - ty);
            }
            break;
        case TS_LED_SCAN_ROWS:
        default:
            index = ty * w + tx;
            break;
    }
    
    if (index >= layer->size) return TS_LED_BLACK;
    return layer->buffer[index];
}

/**
 * @brief 绘制带反色混合的字形
 * 
 * @param overlay_layer 覆盖层（用于绘制）
 * @param base_layer 基础层（用于反色计算，可为 NULL）
 */
static void draw_glyph_overlay(ts_led_layer_t overlay_layer, ts_led_layer_t base_layer,
                                const uint8_t *bitmap,
                                int16_t x, int16_t y, uint8_t width, uint8_t height,
                                ts_led_rgb_t color, bool invert_on_overlap,
                                uint16_t layer_width, uint16_t layer_height)
{
    int bit_idx = 0;
    
    for (int gy = 0; gy < height; gy++) {
        for (int gx = 0; gx < width; gx++) {
            int px = x + gx;
            int py = y + gy;
            
            // 边界检查
            if (px < 0 || px >= layer_width || py < 0 || py >= layer_height) {
                bit_idx++;
                continue;
            }
            
            // 获取位值
            int byte_idx = bit_idx / 8;
            int bit_pos = 7 - (bit_idx % 8);
            bool pixel_on = (bitmap[byte_idx] >> bit_pos) & 1;
            
            if (pixel_on) {
                ts_led_rgb_t final_color;
                
                if (invert_on_overlap && base_layer) {
                    // 从基础层获取像素
                    ts_led_layer_impl_t *bl = (ts_led_layer_impl_t *)base_layer;
                    ts_led_rgb_t base = get_base_pixel(bl, px, py);
                    
                    if (is_dark_pixel(base)) {
                        // 暗色背景：使用原色
                        final_color = color;
                    } else {
                        // 亮色背景：反色显示
                        final_color = invert_color(base);
                    }
                } else {
                    final_color = color;
                }
                
                ts_led_set_pixel_xy(overlay_layer, px, py, final_color);
            }
            
            bit_idx++;
        }
    }
}

/**
 * @brief 渲染覆盖层文本（单次）
 * 
 * 在专用覆盖层（layer 1）上绘制文本，不影响 layer 0 的动画
 */
static void render_overlay_text(ts_text_overlay_state_t *state)
{
    if (!state || !state->active || !state->font || !state->device) return;
    if (!state->overlay_layer) return;
    
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)state->overlay_layer;
    uint16_t layer_width = l->device->config.width;
    uint16_t layer_height = l->device->config.height;
    
    // 清除覆盖层（每帧重绘）
    ts_led_layer_clear(state->overlay_layer);
    
    // 计算基准 X 位置（考虑对齐，仅非滚动模式生效）
    int16_t aligned_x = state->base_x;
    if (state->scroll_dir == TS_TEXT_SCROLL_NONE) {
        switch (state->align) {
            case TS_TEXT_ALIGN_CENTER:
                aligned_x = (layer_width - state->text_width) / 2;
                break;
            case TS_TEXT_ALIGN_RIGHT:
                aligned_x = layer_width - state->text_width;
                break;
            case TS_TEXT_ALIGN_LEFT:
            default:
                // 保持 base_x
                break;
        }
    }
    
    // 计算实际绘制位置
    int16_t draw_x = aligned_x + state->scroll_x;
    int16_t draw_y = state->base_y + state->scroll_y;
    
    // 获取 layer 0 用于反色计算
    ts_led_layer_t base_layer = ts_led_layer_get(state->device, 0);
    
    // 字间距（与 TS_TEXT_DEFAULT_OPTIONS 保持一致）
    const uint8_t spacing = 1;
    
    // 逐字符绘制（使用动态间距）
    const char *p = state->text;
    uint16_t cp;
    
    while (ts_utf8_decode(&p, &cp) > 0 && cp != 0) {
        // 获取字形
        const uint8_t *bitmap;
        esp_err_t err = ts_font_get_glyph(state->font, cp, &bitmap);
        
        // 计算字符实际宽度
        int char_width;
        if (err == ESP_OK) {
            char_width = get_glyph_actual_width(bitmap, state->font->header.width, state->font->header.height);
        } else {
            // 缺失字形 - 使用半宽
            char_width = state->font->header.width / 2;
        }
        
        // 检查是否超出屏幕（完全不可见则跳过）
        if (draw_x >= layer_width) break;
        if (draw_x + char_width < 0) {
            draw_x += char_width + spacing;
            continue;
        }
        
        // 绘制字形
        if (err == ESP_OK) {
            draw_glyph_overlay(state->overlay_layer, base_layer, bitmap, draw_x, draw_y,
                               state->font->header.width, state->font->header.height,
                               state->color, state->invert_on_overlap,
                               layer_width, layer_height);
        }
        
        draw_x += char_width + spacing;
    }
}

/**
 * @brief 更新滚动位置
 */
static void update_scroll(ts_text_overlay_state_t *state, uint32_t now_ms)
{
    if (!state || state->scroll_dir == TS_TEXT_SCROLL_NONE) return;
    if (!state->overlay_layer) return;
    
    // 计算时间差
    uint32_t delta = now_ms - state->last_scroll_time;
    
    // 速度 1-100 映射到 10-100ms 每像素
    uint32_t ms_per_pixel = 110 - state->scroll_speed;
    if (ms_per_pixel < 10) ms_per_pixel = 10;
    
    if (delta < ms_per_pixel) return;
    
    state->last_scroll_time = now_ms;
    
    ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)state->overlay_layer;
    if (!l) return;
    
    uint16_t screen_width = l->device->config.width;
    uint16_t screen_height = l->device->config.height;
    
    switch (state->scroll_dir) {
        case TS_TEXT_SCROLL_LEFT:
            state->scroll_x--;
            // 文本完全滚出屏幕后重置
            if (state->scroll_x + state->text_width < 0) {
                if (state->loop_scroll) {
                    state->scroll_x = screen_width;
                } else {
                    state->scroll_x = -state->text_width;
                }
            }
            break;
            
        case TS_TEXT_SCROLL_RIGHT:
            state->scroll_x++;
            if (state->scroll_x > screen_width) {
                if (state->loop_scroll) {
                    state->scroll_x = -state->text_width;
                } else {
                    state->scroll_x = screen_width;
                }
            }
            break;
            
        case TS_TEXT_SCROLL_UP:
            state->scroll_y--;
            if (state->scroll_y + state->text_height < 0) {
                if (state->loop_scroll) {
                    state->scroll_y = screen_height;
                } else {
                    state->scroll_y = -state->text_height;
                }
            }
            break;
            
        case TS_TEXT_SCROLL_DOWN:
            state->scroll_y++;
            if (state->scroll_y > screen_height) {
                if (state->loop_scroll) {
                    state->scroll_y = -state->text_height;
                } else {
                    state->scroll_y = screen_height;
                }
            }
            break;
            
        default:
            break;
    }
}

/**
 * @brief 覆盖层渲染任务
 * 
 * 只更新覆盖层 buffer，不调用 device_refresh
 * 主 render_task 会负责图层合成和刷新
 */
static void overlay_render_task(void *arg)
{
    (void)arg;
    SemaphoreHandle_t mutex = ts_led_get_state()->mutex;
    
    while (1) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        bool any_active = false;
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        for (int i = 0; i < MAX_OVERLAY_DEVICES; i++) {
            ts_text_overlay_state_t *state = &s_overlays[i];
            if (!state->active) continue;
            
            any_active = true;
            
            // 更新滚动
            update_scroll(state, now);
            
            // 渲染覆盖层（只更新 buffer，不刷新设备）
            render_overlay_text(state);
            
            // 标记 layer 为 dirty，让主渲染任务处理
            if (state->overlay_layer) {
                ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)state->overlay_layer;
                l->dirty = true;
            }
        }
        
        // 如果没有活动的覆盖层，删除任务
        if (!any_active) {
            s_overlay_task = NULL;
            xSemaphoreGive(mutex);
            vTaskDeleteWithCaps(NULL);
            return;
        }
        
        xSemaphoreGive(mutex);

        // 约 30fps（主渲染是 60fps，覆盖层可以稍慢）
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

/**
 * @brief 确保渲染任务运行
 */
static void ensure_overlay_task(void)
{
    if (s_overlay_task == NULL) {
        /* 使用 PSRAM 栈以减少 DRAM 压力（纯 LED 渲染，不涉及 NVS/Flash） */
        xTaskCreateWithCaps(overlay_render_task, "text_overlay", 4096, NULL, 5, &s_overlay_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

/*===========================================================================*/
/*                       Text Overlay Public API                              */
/*===========================================================================*/

esp_err_t ts_led_text_overlay_start(const char *device_name, 
                                     const ts_text_overlay_config_t *config)
{
    if (!device_name || !config || !config->text || !config->font) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int idx = get_overlay_index(device_name);
    if (idx < 0) {
        ESP_LOGE(TAG, "Device '%s' not supported for overlay", device_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    // 解析设备名
    const char *internal;
    if (strcmp(device_name, "touch") == 0) internal = "led_touch";
    else if (strcmp(device_name, "board") == 0) internal = "led_board";
    else if (strcmp(device_name, "matrix") == 0) internal = "led_matrix";
    else internal = device_name;
    
    ts_led_device_t dev = ts_led_device_get(internal);
    if (!dev) {
        ESP_LOGE(TAG, "Device '%s' not found", device_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    SemaphoreHandle_t mutex = ts_led_get_state()->mutex;
    xSemaphoreTake(mutex, portMAX_DELAY);
    ts_text_overlay_state_t *state = &s_overlays[idx];
    
    // 停止之前的覆盖层
    if (state->active) {
        state->active = false;
        // 隐藏之前的覆盖层
        if (state->overlay_layer) {
            ts_led_layer_set_visible(state->overlay_layer, false);
        }
    }
    
    // 获取或创建覆盖层（layer 1）
    state->overlay_layer = ts_led_layer_get(dev, TEXT_OVERLAY_LAYER_INDEX);
    if (!state->overlay_layer) {
        // 创建新的覆盖层
        ts_led_layer_config_t layer_cfg = TS_LED_LAYER_DEFAULT_CONFIG();
        layer_cfg.blend_mode = TS_LED_BLEND_NORMAL;  // 正常混合
        layer_cfg.opacity = 255;
        esp_err_t ret = ts_led_layer_create(dev, &layer_cfg, &state->overlay_layer);
        if (ret != ESP_OK || !state->overlay_layer) {
            ESP_LOGE(TAG, "Failed to create overlay layer");
            xSemaphoreGive(mutex);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Created overlay layer for text");
    }
    
    // 确保覆盖层可见并清空
    ts_led_layer_set_visible(state->overlay_layer, true);
    ts_led_layer_clear(state->overlay_layer);
    
    // 初始化状态
    strncpy(state->text, config->text, sizeof(state->text) - 1);
    state->text[sizeof(state->text) - 1] = '\0';
    state->font = config->font;
    state->color = config->color;
    state->base_x = config->x;
    state->base_y = config->y;
    state->scroll_x = 0;
    state->scroll_y = 0;
    state->align = config->align;
    state->scroll_dir = config->scroll;
    state->scroll_speed = config->scroll_speed > 0 ? config->scroll_speed : 30;
    state->invert_on_overlap = config->invert_on_overlap;
    state->loop_scroll = config->loop_scroll;
    state->device = dev;
    state->last_scroll_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // 预计算文本尺寸
    ts_text_metrics_t metrics;
    ts_text_options_t opts = TS_TEXT_DEFAULT_OPTIONS();
    if (ts_led_text_measure(config->text, config->font, &opts, &metrics) == ESP_OK) {
        state->text_width = metrics.width;
        state->text_height = metrics.height;
    } else {
        state->text_width = strlen(config->text) * config->font->header.width;
        state->text_height = config->font->header.height;
    }
    
    // 滚动模式初始位置
    if (config->scroll == TS_TEXT_SCROLL_LEFT) {
        ts_led_layer_impl_t *l = (ts_led_layer_impl_t *)state->overlay_layer;
        if (l) state->scroll_x = l->device->config.width;
    }
    
    state->active = true;
    
    // 启动渲染任务
    ensure_overlay_task();
    
    ESP_LOGI(TAG, "Text overlay started on '%s': \"%s\" (scroll=%d, invert=%d)",
             device_name, state->text, config->scroll, config->invert_on_overlap);
    
    xSemaphoreGive(mutex);
    return ESP_OK;
}

/* Caller owns the existing LED state mutex, also held for a complete glyph frame. */
static void stop_overlay(ts_text_overlay_state_t *state)
{
    state->active = false;
    state->font = NULL;
    if (state->overlay_layer) {
        ts_led_layer_clear(state->overlay_layer);
        ts_led_layer_set_visible(state->overlay_layer, false);
        ((ts_led_layer_impl_t *)state->overlay_layer)->dirty = true;
    }
}

esp_err_t ts_led_text_overlay_stop(const char *device_name)
{
    if (!device_name) return ESP_ERR_INVALID_ARG;
    int idx = get_overlay_index(device_name);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    ts_led_state_t *led = ts_led_get_state();
    if (!led->initialized) return ESP_OK;
    xSemaphoreTake(led->mutex, portMAX_DELAY);
    stop_overlay(&s_overlays[idx]);
    xSemaphoreGive(led->mutex);
    return ESP_OK;
}

void ts_led_text_overlay_release_font(ts_font_t *font)
{
    ts_led_state_t *led = ts_led_get_state();
    if (!font || !led->initialized) return;
    xSemaphoreTake(led->mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_OVERLAY_DEVICES; i++) {
        if (s_overlays[i].font == font) stop_overlay(&s_overlays[i]);
    }
    xSemaphoreGive(led->mutex);
}

esp_err_t ts_led_text_overlay_update(const char *device_name, const char *text)
{
    if (!device_name || !text) return ESP_ERR_INVALID_ARG;
    
    int idx = get_overlay_index(device_name);
    if (idx < 0 || !ts_led_get_state()->initialized) return ESP_ERR_INVALID_STATE;
    SemaphoreHandle_t mutex = ts_led_get_state()->mutex;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!s_overlays[idx].active) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    ts_text_overlay_state_t *state = &s_overlays[idx];
    strncpy(state->text, text, sizeof(state->text) - 1);
    state->text[sizeof(state->text) - 1] = '\0';
    
    // 重新计算宽度
    ts_text_metrics_t metrics;
    ts_text_options_t opts = TS_TEXT_DEFAULT_OPTIONS();
    if (ts_led_text_measure(text, state->font, &opts, &metrics) == ESP_OK) {
        state->text_width = metrics.width;
        state->text_height = metrics.height;
    }
    
    xSemaphoreGive(mutex);
    return ESP_OK;
}

bool ts_led_text_overlay_is_active(const char *device_name)
{
    if (!device_name) return false;
    
    int idx = get_overlay_index(device_name);
    if (idx < 0) return false;
    
    if (!ts_led_get_state()->initialized) return false;
    SemaphoreHandle_t mutex = ts_led_get_state()->mutex;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool active = s_overlays[idx].active;
    xSemaphoreGive(mutex);
    return active;
}

esp_err_t ts_led_text_overlay_set_position(const char *device_name, 
                                            int16_t x, int16_t y)
{
    if (!device_name) return ESP_ERR_INVALID_ARG;
    
    int idx = get_overlay_index(device_name);
    if (idx < 0 || !ts_led_get_state()->initialized) return ESP_ERR_INVALID_STATE;
    SemaphoreHandle_t mutex = ts_led_get_state()->mutex;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!s_overlays[idx].active) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    s_overlays[idx].scroll_x = x - s_overlays[idx].base_x;
    s_overlays[idx].scroll_y = y - s_overlays[idx].base_y;
    
    xSemaphoreGive(mutex);
    return ESP_OK;
}