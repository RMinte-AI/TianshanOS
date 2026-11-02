/**
 * @file ts_led_private.h
 * @brief TianShanOS LED Internal Definitions
 * 
 * Internal header with complete structure definitions for LED subsystem.
 * Not for external use.
 */

#ifndef TS_LED_PRIVATE_H
#define TS_LED_PRIVATE_H

#include "ts_led.h"
#include "ts_led_animation.h"
#include "ts_led_effect.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_TS_LED_MAX_DEVICES
#define CONFIG_TS_LED_MAX_DEVICES 8
#endif

/**
 * @brief LED device implementation structure
 */
typedef struct ts_led_device {
    char name[TS_LED_MAX_NAME];
    ts_led_config_t config;
    ts_led_rgb_t *framebuffer;
    ts_led_layer_t layers[TS_LED_MAX_LAYERS];
    uint8_t layer_count;
    uint8_t brightness;
    bool enabled;
    led_strip_handle_t strip_handle;
    bool used;
    SemaphoreHandle_t mutex;
} ts_led_device_impl_t;

/**
 * @brief LED layer implementation structure
 */
typedef struct ts_led_layer {
    int id;                             /**< Layer ID for debugging */
    ts_led_device_impl_t *device;
    ts_led_rgb_t *buffer;
    uint16_t size;
    ts_led_blend_t blend_mode;
    uint8_t opacity;
    bool visible;
    bool dirty;
    
    /* Animation state (procedural animations, formerly "effects") */
    ts_led_animation_fn_t anim_fn;
    void *anim_data;
    ts_led_rgb_t anim_color;             /**< Owned colour for the three RGB built-ins. */
    uint32_t anim_interval;
    uint32_t anim_last_time;
    
    /* Post-processing effect state */
    ts_led_effect_config_t post_effect;    /**< Current post-processing effect */
    uint32_t effect_start_time;            /**< Effect start time (for fade, etc.) */
    
    /* For backward compatibility */
    #define effect_fn       anim_fn
    #define effect_data     anim_data
    #define effect_interval anim_interval
    #define effect_last_time anim_last_time
} ts_led_layer_impl_t;

/**
 * @brief LED animation implementation structure
 */
typedef struct ts_led_animation {
    ts_led_effect_t effect;
    ts_led_anim_state_t state;
    uint32_t duration_ms;
    uint32_t elapsed_ms;
    bool loop;
    ts_led_rgb_t color1;
    ts_led_rgb_t color2;
    uint8_t speed;
    void *user_data;
} ts_led_animation_impl_t;

/**
 * @brief Global LED state
 */
typedef struct {
    bool initialized;
    ts_led_device_impl_t devices[CONFIG_TS_LED_MAX_DEVICES];
    SemaphoreHandle_t mutex;
    TaskHandle_t render_task;
    bool render_running;
} ts_led_state_t;

/* Global state accessor */
extern ts_led_state_t *ts_led_get_state(void);

/* Release only GIF-owned callback data; caller holds the layer device mutex. */
void ts_led_image_release_animation(ts_led_layer_t layer);

/* Driver functions */
esp_err_t ts_led_driver_init(ts_led_device_impl_t *dev);
esp_err_t ts_led_driver_send(ts_led_device_impl_t *dev);
void ts_led_driver_deinit(ts_led_device_impl_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* TS_LED_PRIVATE_H */
