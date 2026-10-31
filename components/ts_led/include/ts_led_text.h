/**
 * @file ts_led_text.h
 * @brief TianShanOS LED Text Rendering
 * 
 * Text rendering API for LED matrix display. Supports:
 * - Multi-font rendering (ASCII, CJK via dynamic font loading)
 * - Basic text alignment (left, center, right)
 * - Color and background control
 * - UTF-8 input
 * - Text overlay layer with scrolling and invert-on-overlap
 * 
 * @author TianShanOS Team
 * @version 1.1.0
 */

#ifndef TS_LED_TEXT_H
#define TS_LED_TEXT_H

#include "esp_err.h"
#include "ts_led.h"
#include "ts_led_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief Text alignment
 */
typedef enum {
    TS_TEXT_ALIGN_LEFT = 0,     /**< Left-aligned */
    TS_TEXT_ALIGN_CENTER,        /**< Center-aligned */
    TS_TEXT_ALIGN_RIGHT          /**< Right-aligned */
} ts_text_align_t;

/**
 * @brief Text scroll direction
 */
typedef enum {
    TS_TEXT_SCROLL_NONE = 0,    /**< No scrolling (static) */
    TS_TEXT_SCROLL_LEFT,         /**< Scroll from right to left */
    TS_TEXT_SCROLL_RIGHT,        /**< Scroll from left to right */
    TS_TEXT_SCROLL_UP,           /**< Scroll from bottom to top */
    TS_TEXT_SCROLL_DOWN          /**< Scroll from top to bottom */
} ts_text_scroll_t;

/**
 * @brief Text blend mode (how text interacts with background)
 */
typedef enum {
    TS_TEXT_BLEND_NORMAL = 0,   /**< Normal: text color on transparent bg */
    TS_TEXT_BLEND_INVERT,        /**< Invert: text inverts underlying pixels */
    TS_TEXT_BLEND_REPLACE        /**< Replace: text color replaces underlying */
} ts_text_blend_t;

/**
 * @brief Text rendering options
 */
typedef struct {
    ts_led_rgb_t color;          /**< Text color (default: white) */
    ts_led_rgb_t bg_color;       /**< Background color (default: transparent/black) */
    ts_text_align_t align;       /**< Horizontal alignment (default: left) */
    int16_t x_offset;            /**< X offset from aligned position */
    int16_t y_offset;            /**< Y offset from top */
    uint8_t spacing;             /**< Extra character spacing (default: 0) */
    bool wrap;                   /**< Enable word wrap (default: false) */
    bool transparent_bg;         /**< Transparent background (default: true) */
    bool proportional;           /**< Use proportional width based on glyph content (default: true) */
} ts_text_options_t;

/** Default text options */
#define TS_TEXT_DEFAULT_OPTIONS() { \
    .color = TS_LED_WHITE, \
    .bg_color = TS_LED_BLACK, \
    .align = TS_TEXT_ALIGN_LEFT, \
    .x_offset = 0, \
    .y_offset = 0, \
    .spacing = 1, \
    .wrap = false, \
    .transparent_bg = true, \
    .proportional = true \
}

/**
 * @brief Text overlay layer configuration
 * 
 * Text overlay is a special layer that floats above the base content,
 * applying invert blending where text overlaps non-black pixels.
 */
typedef struct {
    const char *text;            /**< UTF-8 text to display (must remain valid) */
    ts_font_t *font;             /**< Font to use (must be loaded) */
    ts_led_rgb_t color;          /**< Text color (for non-inverted areas) */
    int16_t x;                   /**< Start X position */
    int16_t y;                   /**< Start Y position */
    ts_text_align_t align;       /**< Text alignment (left/center/right) */
    ts_text_scroll_t scroll;     /**< Scroll direction */
    uint8_t scroll_speed;        /**< Scroll speed 1-100 (pixels per 100ms) */
    bool invert_on_overlap;      /**< Invert text where it overlaps content */
    bool loop_scroll;            /**< Loop scrolling when text goes off screen */
} ts_text_overlay_config_t;

/** Default overlay config */
#define TS_TEXT_OVERLAY_DEFAULT_CONFIG() { \
    .text = NULL, \
    .font = NULL, \
    .color = TS_LED_WHITE, \
    .x = 0, \
    .y = 0, \
    .align = TS_TEXT_ALIGN_LEFT, \
    .scroll = TS_TEXT_SCROLL_NONE, \
    .scroll_speed = 30, \
    .invert_on_overlap = false, \
    .loop_scroll = false \
}

/**
 * @brief Text measurement result
 */
typedef struct {
    uint16_t width;              /**< Total text width in pixels */
    uint16_t height;             /**< Text height in pixels */
    uint8_t char_count;          /**< Number of characters */
    uint8_t line_count;          /**< Number of lines (with wrap) */
} ts_text_metrics_t;

/*===========================================================================*/
/*                              API Functions                                 */
/*===========================================================================*/

/**
 * @brief Draw text on layer using specified font
 * 
 * Renders UTF-8 text on the layer at the specified position.
 * Characters not in the font are rendered as empty space.
 * 
 * @param layer Target layer
 * @param text UTF-8 encoded text string
 * @param x X position (pixels from left)
 * @param y Y position (pixels from top)
 * @param font Font handle (from ts_font_load)
 * @param options Rendering options (NULL for defaults)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if layer, text, or font is NULL
 */
esp_err_t ts_led_text_draw(ts_led_layer_t layer, const char *text,
                            int16_t x, int16_t y, ts_font_t *font,
                            const ts_text_options_t *options);

/**
 * @brief Draw text on device by name
 * 
 * Convenience function that gets the device layer and draws text.
 * 
 * @param device_name Device name (e.g., "led_matrix")
 * @param text UTF-8 encoded text
 * @param font Font handle
 * @param options Rendering options (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t ts_led_text_draw_on_device(const char *device_name, const char *text,
                                      ts_font_t *font, 
                                      const ts_text_options_t *options);

/**
 * @brief Measure text dimensions without rendering
 * 
 * @param text UTF-8 encoded text
 * @param font Font handle
 * @param options Rendering options (NULL for defaults)
 * @param metrics Output: text metrics
 * @return ESP_OK on success
 */
esp_err_t ts_led_text_measure(const char *text, ts_font_t *font,
                               const ts_text_options_t *options,
                               ts_text_metrics_t *metrics);

/**
 * @brief Draw a single character
 * 
 * @param layer Target layer
 * @param codepoint Unicode codepoint
 * @param x X position
 * @param y Y position
 * @param font Font handle
 * @param color Text color
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if glyph missing
 */
esp_err_t ts_led_text_draw_char(ts_led_layer_t layer, uint16_t codepoint,
                                 int16_t x, int16_t y, ts_font_t *font,
                                 ts_led_rgb_t color);

/**
 * @brief Get number of characters that fit in width
 * 
 * @param text UTF-8 encoded text
 * @param font Font handle
 * @param max_width Maximum width in pixels
 * @return Number of characters that fit
 */
int ts_led_text_chars_in_width(const char *text, ts_font_t *font, 
                                uint16_t max_width);

/*===========================================================================*/
/*                       Text Overlay Layer API                               */
/*===========================================================================*/

/**
 * @brief Start text overlay on device
 * 
 * Creates a text overlay layer that floats above the base content.
 * The overlay runs as a separate layer and can scroll independently.
 * Text is rendered with invert-on-overlap to ensure readability.
 * 
 * @note Does NOT clear base layer content - text floats above it
 * @note Only one overlay per device is supported
 * 
 * @param device_name Device name (e.g., "led_matrix", "matrix")
 * @param config Overlay configuration
 * @return ESP_OK on success
 */
esp_err_t ts_led_text_overlay_start(const char *device_name, 
                                     const ts_text_overlay_config_t *config);

/**
 * @brief Stop text overlay on device
 * 
 * Stops the overlay and removes text layer, restoring original content.
 * 
 * @param device_name Device name
 * @return ESP_OK on success
 */
esp_err_t ts_led_text_overlay_stop(const char *device_name);

/** Detach this font from its actual overlays after current glyph readers finish. */
void ts_led_text_overlay_release_font(ts_font_t *font);

/**
 * @brief Update overlay text (for dynamic content)
 * 
 * Updates the text displayed in the overlay without restarting.
 * 
 * @param device_name Device name
 * @param text New text (must remain valid while overlay is active)
 * @return ESP_OK on success
 */
esp_err_t ts_led_text_overlay_update(const char *device_name, const char *text);

/**
 * @brief Check if overlay is active on device
 * 
 * @param device_name Device name
 * @return true if overlay is running
 */
bool ts_led_text_overlay_is_active(const char *device_name);

/**
 * @brief Set overlay scroll position manually
 * 
 * @param device_name Device name
 * @param x X scroll position
 * @param y Y scroll position
 * @return ESP_OK on success
 */
esp_err_t ts_led_text_overlay_set_position(const char *device_name, 
                                            int16_t x, int16_t y);

/*===========================================================================*/
/*                         UTF-8 Helper Functions                             */
/*===========================================================================*/

/**
 * @brief Decode next UTF-8 character from string
 * 
 * @param text Input text pointer (advanced to next char)
 * @param codepoint Output: decoded codepoint
 * @return Number of bytes consumed, or 0 on end/error
 */
int ts_utf8_decode(const char **text, uint16_t *codepoint);

/**
 * @brief Count UTF-8 characters in string
 * 
 * @param text UTF-8 encoded string
 * @return Number of Unicode characters
 */
int ts_utf8_strlen(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* TS_LED_TEXT_H */
