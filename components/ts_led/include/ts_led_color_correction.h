/**
 * @file ts_led_color_correction.h
 * @brief LED Color Correction System for TianShanOS
 * 
 * Provides comprehensive color correction for WS2812 LED matrices:
 * - White point correction (RGB channel scaling)
 * - Gamma correction with lookup table
 * - Brightness enhancement
 * - Saturation enhancement
 * 
 * Configuration: system NVS; legacy SD priority is imported only once.
 * Explicit import/export supports JSON and encrypted TSCFG formats.
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-02-06
 */

#ifndef TS_LED_COLOR_CORRECTION_H
#define TS_LED_COLOR_CORRECTION_H

#include "esp_err.h"
#include "ts_led.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Version                                       */
/*===========================================================================*/

#define TS_LED_CC_VERSION_MAJOR    1
#define TS_LED_CC_VERSION_MINOR    0
#define TS_LED_CC_VERSION_PATCH    0

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

/** Configuration file paths */
#define TS_LED_CC_SDCARD_JSON_PATH    "/sdcard/config/led_color_correction.json"
#define TS_LED_CC_SDCARD_TSCFG_PATH   "/sdcard/config/led_color_correction.tscfg"
#define TS_LED_CC_NVS_NAMESPACE       "led_color"

/** Parameter limits */
#define TS_LED_CC_SCALE_MIN           0.0f
#define TS_LED_CC_SCALE_MAX           2.0f
#define TS_LED_CC_GAMMA_MIN           0.1f
#define TS_LED_CC_GAMMA_MAX           4.0f
#define TS_LED_CC_GAMMA_DEFAULT       1.0f    /**< Default gamma (1.0 = passthrough) */

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief White point correction configuration
 */
typedef struct {
    bool enabled;           /**< Enable white point correction */
    float red_scale;        /**< Red channel scale (0.0-2.0) */
    float green_scale;      /**< Green channel scale (0.0-2.0) */
    float blue_scale;       /**< Blue channel scale (0.0-2.0) */
} ts_led_cc_white_point_t;

/**
 * @brief Gamma correction configuration
 */
typedef struct {
    bool enabled;           /**< Enable gamma correction */
    float gamma;            /**< Gamma value (0.1-4.0, default 2.2) */
} ts_led_cc_gamma_t;

/**
 * @brief Brightness enhancement configuration
 */
typedef struct {
    bool enabled;           /**< Enable brightness enhancement */
    float factor;           /**< Brightness factor (0.0-2.0) */
} ts_led_cc_brightness_t;

/**
 * @brief Saturation enhancement configuration
 */
typedef struct {
    bool enabled;           /**< Enable saturation enhancement */
    float factor;           /**< Saturation factor (0.0-2.0) */
} ts_led_cc_saturation_t;

/**
 * @brief Complete color correction configuration
 */
typedef struct {
    bool enabled;                       /**< Global enable/disable */
    ts_led_cc_white_point_t white_point;
    ts_led_cc_gamma_t gamma;
    ts_led_cc_brightness_t brightness;
    ts_led_cc_saturation_t saturation;
} ts_led_cc_config_t;

/**
 * @brief HSL color (for internal conversion)
 */
typedef struct {
    float h;    /**< Hue (0.0-360.0) */
    float s;    /**< Saturation (0.0-1.0) */
    float l;    /**< Lightness (0.0-1.0) */
} ts_led_cc_hsl_t;

/**
 * @brief Configuration change callback
 */
typedef void (*ts_led_cc_change_callback_t)(void);

/*===========================================================================*/
/*                          Core Functions                                    */
/*===========================================================================*/

/**
 * @brief Initialize color correction system
 * 
 * Loads configuration in priority order: SD Card > NVS > Defaults
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_init(void);

/**
 * @brief Deinitialize color correction system
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_deinit(void);

/**
 * @brief Check if color correction is initialized
 * 
 * @return true if initialized
 */
bool ts_led_cc_is_initialized(void);

/*===========================================================================*/
/*                      Configuration Functions                               */
/*===========================================================================*/

/**
 * @brief Get default configuration
 * 
 * @param config Output configuration structure
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_get_default_config(ts_led_cc_config_t *config);

/**
 * @brief Get current configuration
 * 
 * @param config Output configuration structure
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_get_config(ts_led_cc_config_t *config);

/**
 * @brief Set configuration (and save to NVS)
 * 
 * @param config Configuration to apply
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_set_config(const ts_led_cc_config_t *config);

/**
 * @brief Reset configuration to defaults
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_reset_config(void);

/*===========================================================================*/
/*                      Individual Parameter Setters                          */
/*===========================================================================*/

/**
 * @brief Enable/disable color correction globally
 */
esp_err_t ts_led_cc_set_enabled(bool enabled);

/**
 * @brief Check if color correction is enabled
 */
bool ts_led_cc_is_enabled(void);

/**
 * @brief Set white point correction
 */
esp_err_t ts_led_cc_set_white_point(bool enabled, float red, float green, float blue);

/**
 * @brief Set gamma correction
 */
esp_err_t ts_led_cc_set_gamma(bool enabled, float gamma);

/**
 * @brief Set brightness enhancement
 */
esp_err_t ts_led_cc_set_brightness(bool enabled, float factor);

/**
 * @brief Set saturation enhancement
 */
esp_err_t ts_led_cc_set_saturation(bool enabled, float factor);

/*===========================================================================*/
/*                      Color Correction Application                          */
/*===========================================================================*/

/**
 * @brief Apply color correction to a single pixel
 * 
 * @param input Input RGB color
 * @param output Output corrected RGB color
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_apply_pixel(const ts_led_rgb_t *input, ts_led_rgb_t *output);

/**
 * @brief Apply color correction to an array of pixels
 * 
 * @param input Input RGB array
 * @param output Output RGB array (can be same as input for in-place)
 * @param count Number of pixels
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_apply_array(const ts_led_rgb_t *input, ts_led_rgb_t *output, size_t count);

/**
 * @brief Apply color correction in-place
 * 
 * @param pixels RGB array to correct in-place
 * @param count Number of pixels
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_apply_inplace(ts_led_rgb_t *pixels, size_t count);

/*===========================================================================*/
/*                      Persistence Functions                                 */
/*===========================================================================*/

/**
 * @brief Save configuration to NVS
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_save_to_nvs(void);

/**
 * @brief Load configuration from NVS
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_load_from_nvs(void);

/**
 * @brief Save configuration to SD card (JSON)
 * 
 * @param path File path (NULL for default)
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_save_to_sdcard(const char *path);

/**
 * @brief Load configuration from SD card
 * 
 * Supports both .json and .tscfg (encrypted) formats.
 * 
 * @param path File path (NULL for default, tries .tscfg first)
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_load_from_sdcard(const char *path);

/*===========================================================================*/
/*                      JSON Conversion                                       */
/*===========================================================================*/

/**
 * @brief Export configuration to JSON
 * 
 * @return cJSON object (caller must free with cJSON_Delete)
 */
cJSON *ts_led_cc_config_to_json(void);

/**
 * @brief Import configuration from JSON
 * 
 * @param json JSON object
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_config_from_json(const cJSON *json);

/**
 * @brief Export configuration to JSON string
 * 
 * @param pretty If true, format with indentation
 * @return JSON string (caller must free)
 */
char *ts_led_cc_export_json_string(bool pretty);

/**
 * @brief Import configuration from JSON string
 * 
 * @param json_str JSON string
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_import_json_string(const char *json_str);

/*===========================================================================*/
/*                      Callback Functions                                    */
/*===========================================================================*/

/**
 * @brief Register configuration change callback
 * 
 * @param callback Callback function
 * @return ESP_OK on success
 */
esp_err_t ts_led_cc_register_change_callback(ts_led_cc_change_callback_t callback);

/*===========================================================================*/
/*                      Color Conversion Utilities                            */
/*===========================================================================*/

/**
 * @brief Convert RGB to HSL
 */
void ts_led_cc_rgb_to_hsl(const ts_led_rgb_t *rgb, ts_led_cc_hsl_t *hsl);

/**
 * @brief Convert HSL to RGB
 */
void ts_led_cc_hsl_to_rgb(const ts_led_cc_hsl_t *hsl, ts_led_rgb_t *rgb);

#ifdef __cplusplus
}
#endif

#endif /* TS_LED_COLOR_CORRECTION_H */
