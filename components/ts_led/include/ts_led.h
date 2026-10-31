/**
 * @file ts_led.h
 * @brief TianShanOS LED Control System
 * 
 * Complete LED control system supporting WS2812 strips and matrices
 * with layer-based rendering, animations, effects, and image display.
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#ifndef TS_LED_H
#define TS_LED_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "ts_led_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Version                                       */
/*===========================================================================*/

#define TS_LED_VERSION_MAJOR    1
#define TS_LED_VERSION_MINOR    0
#define TS_LED_VERSION_PATCH    0

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

#define TS_LED_MAX_BRIGHTNESS   255
#define TS_LED_MAX_DEVICES      8
#define TS_LED_MAX_LAYERS       8
#define TS_LED_MAX_NAME         32

/*===========================================================================*/
/*                              Color Types                                   */
/*===========================================================================*/

#ifndef TS_LED_RGB_DEFINED
#define TS_LED_RGB_DEFINED
/**
 * @brief RGB color (24-bit)
 */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ts_led_rgb_t;
#endif

/**
 * @brief RGBW color (32-bit)
 */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t w;
} ts_led_rgbw_t;

/**
 * @brief HSV color
 */
typedef struct {
    uint16_t h;   /**< Hue 0-359 */
    uint8_t s;    /**< Saturation 0-255 */
    uint8_t v;    /**< Value 0-255 */
} ts_led_hsv_t;

/*===========================================================================*/
/*                              Color Macros                                  */
/*===========================================================================*/

#define TS_LED_RGB(r, g, b)     ((ts_led_rgb_t){(r), (g), (b)})
#define TS_LED_RGBW(r,g,b,w)    ((ts_led_rgbw_t){(r), (g), (b), (w)})
#define TS_LED_HSV(h, s, v)     ((ts_led_hsv_t){(h), (s), (v)})

/* Common colors */
#define TS_LED_BLACK            TS_LED_RGB(0, 0, 0)
#define TS_LED_WHITE            TS_LED_RGB(255, 255, 255)
#define TS_LED_RED              TS_LED_RGB(255, 0, 0)
#define TS_LED_GREEN            TS_LED_RGB(0, 255, 0)
#define TS_LED_BLUE             TS_LED_RGB(0, 0, 255)
#define TS_LED_YELLOW           TS_LED_RGB(255, 255, 0)
#define TS_LED_CYAN             TS_LED_RGB(0, 255, 255)
#define TS_LED_MAGENTA          TS_LED_RGB(255, 0, 255)
#define TS_LED_ORANGE           TS_LED_RGB(255, 165, 0)
#define TS_LED_PURPLE           TS_LED_RGB(128, 0, 128)

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief LED device type
 */
typedef enum {
    TS_LED_TYPE_WS2812 = 0,      /**< WS2812/WS2812B */
    TS_LED_TYPE_WS2815,          /**< WS2815 */
    TS_LED_TYPE_SK6812,          /**< SK6812 RGBW */
    TS_LED_TYPE_APA102,          /**< APA102/SK9822 (SPI) */
    TS_LED_TYPE_MAX
} ts_led_type_t;

/**
 * @brief LED layout type
 */
#ifndef TS_LED_LAYOUT_DEFINED
#define TS_LED_LAYOUT_DEFINED
typedef enum {
    TS_LED_LAYOUT_STRIP = 0,     /**< Linear strip */
    TS_LED_LAYOUT_MATRIX,        /**< 2D matrix */
    TS_LED_LAYOUT_RING           /**< Circular ring */
} ts_led_layout_t;
#endif

/* Forward declaration for layer */
#ifndef TS_LED_LAYER_DEFINED
#define TS_LED_LAYER_DEFINED
typedef struct ts_led_layer *ts_led_layer_t;
#endif

/* Include animation types early so they're available for API declarations */
#include "ts_led_animation.h"

/**
 * @brief Matrix origin position
 */
typedef enum {
    TS_LED_ORIGIN_TOP_LEFT = 0,
    TS_LED_ORIGIN_TOP_RIGHT,
    TS_LED_ORIGIN_BOTTOM_LEFT,
    TS_LED_ORIGIN_BOTTOM_RIGHT
} ts_led_origin_t;

/**
 * @brief Matrix scan direction
 */
typedef enum {
    TS_LED_SCAN_ROWS = 0,        /**< Row by row */
    TS_LED_SCAN_COLUMNS,         /**< Column by column */
    TS_LED_SCAN_ZIGZAG_ROWS,     /**< Zigzag row by row */
    TS_LED_SCAN_ZIGZAG_COLS      /**< Zigzag column by column */
} ts_led_scan_t;

/**
 * @brief Layer blend mode
 */
typedef enum {
    TS_LED_BLEND_NORMAL = 0,     /**< Normal (alpha blend) */
    TS_LED_BLEND_ADD,            /**< Additive */
    TS_LED_BLEND_MULTIPLY,       /**< Multiply */
    TS_LED_BLEND_SCREEN,         /**< Screen */
    TS_LED_BLEND_OVERLAY         /**< Overlay */
} ts_led_blend_t;

/**
 * @brief Animation state
 */
typedef enum {
    TS_LED_ANIM_STOPPED = 0,
    TS_LED_ANIM_PLAYING,
    TS_LED_ANIM_PAUSED
} ts_led_anim_state_t;

/**
 * @brief LED device handle
 */
typedef struct ts_led_device *ts_led_device_t;

/* ts_led_layer_t already defined above with TS_LED_LAYER_DEFINED guard */

/**
 * @brief Animation handle
 */
typedef struct ts_led_animation *ts_led_animation_t;

/**
 * @brief LED device configuration
 */
typedef struct {
    const char *name;            /**< Device name */
    ts_led_type_t type;          /**< LED type */
    ts_led_layout_t layout;      /**< Layout type */
    int gpio_pin;                /**< Data GPIO pin */
    uint16_t led_count;          /**< Number of LEDs */
    
    /* Matrix-specific */
    uint16_t width;              /**< Matrix width */
    uint16_t height;             /**< Matrix height */
    ts_led_origin_t origin;      /**< Matrix origin */
    ts_led_scan_t scan;          /**< Matrix scan pattern */
    
    /* Options */
    uint8_t brightness;          /**< Initial brightness (0-255) */
    int rmt_channel;             /**< RMT channel (-1 for auto) */
    bool use_dma;                /**< Use DMA for RMT */
} ts_led_config_t;

/**
 * @brief Layer configuration
 */
typedef struct {
    ts_led_blend_t blend_mode;   /**< Blend mode */
    uint8_t opacity;             /**< Opacity (0-255) */
    bool visible;                /**< Visibility */
} ts_led_layer_config_t;

/* Note: Effect/Animation types moved to ts_led_animation.h */
/* For backward compatibility, include ts_led_animation.h below */

/*===========================================================================*/
/*                         Default Configuration                              */
/*===========================================================================*/

#define TS_LED_DEFAULT_CONFIG() { \
    .name = "led", \
    .type = TS_LED_TYPE_WS2812, \
    .layout = TS_LED_LAYOUT_STRIP, \
    .gpio_pin = -1, \
    .led_count = 0, \
    .width = 0, \
    .height = 0, \
    .origin = TS_LED_ORIGIN_TOP_LEFT, \
    .scan = TS_LED_SCAN_ROWS, \
    .brightness = 128, \
    .rmt_channel = -1, \
    .use_dma = true \
}

#define TS_LED_LAYER_DEFAULT_CONFIG() { \
    .blend_mode = TS_LED_BLEND_NORMAL, \
    .opacity = 255, \
    .visible = true \
}

/*===========================================================================*/
/*                              Core API                                      */
/*===========================================================================*/

/**
 * @brief Initialize LED subsystem
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_led_init(void);

/**
 * @brief Deinitialize LED subsystem
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_led_deinit(void);

/** Whether the retained LED subsystem resources are initialized. */
bool ts_led_is_initialized(void);

/*===========================================================================*/
/*                           Device Management                                */
/*===========================================================================*/

/**
 * @brief Create LED device
 * 
 * @param config Device configuration
 * @param device Output device handle
 * @return ESP_OK on success
 */
esp_err_t ts_led_device_create(const ts_led_config_t *config, ts_led_device_t *device);

/**
 * @brief Destroy LED device
 * 
 * @param device Device handle
 * @return ESP_OK on success
 */
esp_err_t ts_led_device_destroy(ts_led_device_t device);

/**
 * @brief Get device by name
 * 
 * @param name Device name
 * @return Device handle or NULL
 */
ts_led_device_t ts_led_device_get(const char *name);

/**
 * @brief Set device brightness
 * 
 * @param device Device handle
 * @param brightness Brightness (0-255)
 * @return ESP_OK on success
 */
esp_err_t ts_led_device_set_brightness(ts_led_device_t device, uint8_t brightness);

/**
 * @brief Get device brightness
 * 
 * @param device Device handle
 * @return Brightness value
 */
uint8_t ts_led_device_get_brightness(ts_led_device_t device);

/**
 * @brief Get LED count
 * 
 * @param device Device handle
 * @return Number of LEDs
 */
uint16_t ts_led_device_get_count(ts_led_device_t device);

/**
 * @brief Get device layout type
 * 
 * @param device Device handle
 * @return Layout type (STRIP, RING, MATRIX)
 */
ts_led_layout_t ts_led_device_get_layout(ts_led_device_t device);

/**
 * @brief Refresh device (send data to LEDs)
 * 
 * @param device Device handle
 * @return ESP_OK on success
 */
esp_err_t ts_led_device_refresh(ts_led_device_t device);

/**
 * @brief Clear all LEDs (set to black)
 * 
 * @param device Device handle
 * @return ESP_OK on success
 */
esp_err_t ts_led_device_clear(ts_led_device_t device);

/**
 * @brief Fill all LEDs with a color (direct framebuffer access)
 * 
 * @param device Device handle
 * @param color Color to fill
 * @return ESP_OK on success
 */
esp_err_t ts_led_device_fill(ts_led_device_t device, ts_led_rgb_t color);

/**
 * @brief Set a single LED pixel (direct framebuffer access)
 * 
 * @param device Device handle
 * @param index LED index
 * @param color Color to set
 * @return ESP_OK on success
 */
esp_err_t ts_led_device_set_pixel(ts_led_device_t device, uint16_t index, ts_led_rgb_t color);

/*===========================================================================*/
/*                           Layer Management                                 */
/*===========================================================================*/

/**
 * @brief Create layer on device
 * 
 * @param device Device handle
 * @param config Layer configuration
 * @param layer Output layer handle
 * @return ESP_OK on success
 */
esp_err_t ts_led_layer_create(ts_led_device_t device, 
                               const ts_led_layer_config_t *config,
                               ts_led_layer_t *layer);

/**
 * @brief Get existing layer by index
 * 
 * Creates layer 0 automatically if it doesn't exist.
 * 
 * @param device Device handle
 * @param index Layer index (0 = base layer)
 * @return Layer handle or NULL if not found
 */
ts_led_layer_t ts_led_layer_get(ts_led_device_t device, uint8_t index);

/**
 * @brief Destroy layer
 * 
 * @param layer Layer handle
 * @return ESP_OK on success
 */
esp_err_t ts_led_layer_destroy(ts_led_layer_t layer);

/**
 * @brief Set layer blend mode
 * 
 * @param layer Layer handle
 * @param mode Blend mode
 * @return ESP_OK on success
 */
esp_err_t ts_led_layer_set_blend(ts_led_layer_t layer, ts_led_blend_t mode);

/**
 * @brief Set layer opacity
 * 
 * @param layer Layer handle
 * @param opacity Opacity (0-255)
 * @return ESP_OK on success
 */
esp_err_t ts_led_layer_set_opacity(ts_led_layer_t layer, uint8_t opacity);

/**
 * @brief Set layer visibility
 * 
 * @param layer Layer handle
 * @param visible Visibility
 * @return ESP_OK on success
 */
esp_err_t ts_led_layer_set_visible(ts_led_layer_t layer, bool visible);

/**
 * @brief Clear layer
 * 
 * @param layer Layer handle
 * @return ESP_OK on success
 */
esp_err_t ts_led_layer_clear(ts_led_layer_t layer);

/**
 * @brief Apply a post-processing effect to a layer
 * 
 * Post-processing effects modify the final output after animations
 * have rendered their content. Examples include pulse, blink, fade, etc.
 * 
 * @param layer Layer handle
 * @param config Effect configuration
 * @return ESP_OK on success
 */
esp_err_t ts_led_layer_set_effect(ts_led_layer_t layer, const ts_led_effect_config_t *config);

/**
 * @brief Remove post-processing effect from layer
 * 
 * @param layer Layer handle
 * @return ESP_OK on success
 */
esp_err_t ts_led_layer_clear_effect(ts_led_layer_t layer);

/**
 * @brief Check if layer has an active post-processing effect
 * 
 * @param layer Layer handle
 * @return true if effect is active
 */
bool ts_led_layer_has_effect(ts_led_layer_t layer);

/**
 * @brief Get current post-processing effect type
 * 
 * @param layer Layer handle
 * @return Effect type, or TS_LED_EFFECT_NONE if no effect
 */
ts_led_effect_type_t ts_led_layer_get_effect_type(ts_led_layer_t layer);

/*===========================================================================*/
/*                           Drawing Operations                               */
/*===========================================================================*/

/**
 * @brief Set single LED color
 * 
 * @param layer Layer handle
 * @param index LED index
 * @param color RGB color
 * @return ESP_OK on success
 */
esp_err_t ts_led_set_pixel(ts_led_layer_t layer, uint16_t index, ts_led_rgb_t color);

/**
 * @brief Set LED color at matrix position
 * 
 * @param layer Layer handle
 * @param x X coordinate
 * @param y Y coordinate
 * @param color RGB color
 * @return ESP_OK on success
 */
esp_err_t ts_led_set_pixel_xy(ts_led_layer_t layer, uint16_t x, uint16_t y, 
                               ts_led_rgb_t color);

/**
 * @brief Fill all LEDs with color
 * 
 * @param layer Layer handle
 * @param color RGB color
 * @return ESP_OK on success
 */
esp_err_t ts_led_fill(ts_led_layer_t layer, ts_led_rgb_t color);

/**
 * @brief Fill range of LEDs
 * 
 * @param layer Layer handle
 * @param start Start index
 * @param count Number of LEDs
 * @param color RGB color
 * @return ESP_OK on success
 */
esp_err_t ts_led_fill_range(ts_led_layer_t layer, uint16_t start, 
                             uint16_t count, ts_led_rgb_t color);

/**
 * @brief Fill rectangle on matrix
 * 
 * @param layer Layer handle
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param color RGB color
 * @return ESP_OK on success
 */
esp_err_t ts_led_fill_rect(ts_led_layer_t layer, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h, ts_led_rgb_t color);

/**
 * @brief Draw line on matrix
 * 
 * @param layer Layer handle
 * @param x0 Start X
 * @param y0 Start Y
 * @param x1 End X
 * @param y1 End Y
 * @param color RGB color
 * @return ESP_OK on success
 */
esp_err_t ts_led_draw_line(ts_led_layer_t layer, int16_t x0, int16_t y0,
                            int16_t x1, int16_t y1, ts_led_rgb_t color);

/**
 * @brief Draw circle on matrix
 * 
 * @param layer Layer handle
 * @param cx Center X
 * @param cy Center Y
 * @param r Radius
 * @param color RGB color
 * @return ESP_OK on success
 */
esp_err_t ts_led_draw_circle(ts_led_layer_t layer, int16_t cx, int16_t cy,
                              int16_t r, ts_led_rgb_t color);

/**
 * @brief Apply gradient to range
 * 
 * @param layer Layer handle
 * @param start Start index
 * @param count Number of LEDs
 * @param color1 Start color
 * @param color2 End color
 * @return ESP_OK on success
 */
esp_err_t ts_led_gradient(ts_led_layer_t layer, uint16_t start, uint16_t count,
                           ts_led_rgb_t color1, ts_led_rgb_t color2);

/*===========================================================================*/
/*                            Animations                                      */
/*===========================================================================*/

/**
 * @brief Start animation on layer
 * 
 * @note For backward compatibility, ts_led_effect_* macros are provided
 *       in ts_led_animation.h
 * 
 * @param layer Layer handle
 * @param animation Animation definition
 * @return ESP_OK on success
 */
esp_err_t ts_led_animation_start(ts_led_layer_t layer, const ts_led_animation_def_t *animation);

/**
 * @brief Stop animation on layer
 * 
 * @param layer Layer handle
 * @return ESP_OK on success
 */
esp_err_t ts_led_animation_stop(ts_led_layer_t layer);

/**
 * @brief Get built-in animation by name
 * 
 * @param name Animation name
 * @return Animation definition or NULL
 */
const ts_led_animation_def_t *ts_led_animation_get_builtin(const char *name);

/**
 * @brief List built-in animation names
 * 
 * @param names Array to store names
 * @param max_names Maximum names to retrieve
 * @return Number of animations
 */
size_t ts_led_animation_list_builtin(const char **names, size_t max_names);

/**
 * @brief List animations suitable for a specific device layout
 * 
 * 不同形态的LED设备支持不同动画：
 * - TS_LED_LAYOUT_STRIP: 点光源动画 (pulse, heartbeat, color_cycle)
 * - TS_LED_LAYOUT_RING: 环形动画 (chase, comet, spin, breathe_wave)
 * - TS_LED_LAYOUT_MATRIX: 矩阵动画 (fire, rain, plasma, ripple)
 * 
 * @param layout Device layout type
 * @param names Array to store animation names (NULL to just count)
 * @param max_names Maximum names to retrieve
 * @return Number of suitable animations
 */
size_t ts_led_animation_list_for_device(ts_led_layout_t layout, const char **names, size_t max_names);

/*===========================================================================*/
/*                            Color Utilities                                 */
/*===========================================================================*/

/**
 * @brief Convert HSV to RGB
 */
ts_led_rgb_t ts_led_hsv_to_rgb(ts_led_hsv_t hsv);

/**
 * @brief Convert RGB to HSV
 */
ts_led_hsv_t ts_led_rgb_to_hsv(ts_led_rgb_t rgb);

/**
 * @brief Blend two colors
 */
ts_led_rgb_t ts_led_blend_colors(ts_led_rgb_t c1, ts_led_rgb_t c2, uint8_t amount);

/**
 * @brief Scale color brightness
 */
ts_led_rgb_t ts_led_scale_color(ts_led_rgb_t color, uint8_t scale);

/**
 * @brief Get color from wheel (0-255)
 */
ts_led_rgb_t ts_led_color_wheel(uint8_t pos);

/**
 * @brief Parse color string (#RRGGBB or name)
 */
esp_err_t ts_led_parse_color(const char *str, ts_led_rgb_t *color);

#ifdef __cplusplus
}
#endif

#endif /* TS_LED_H */
