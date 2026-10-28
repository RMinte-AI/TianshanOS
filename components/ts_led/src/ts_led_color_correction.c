/**
 * @file ts_led_color_correction.c
 * @brief LED Color Correction Implementation
 * 
 * 色彩校正系统实现，包含：
 * - Gamma 查找表（预计算提高性能）
 * - RGB ↔ HSL 转换
 * - 系统 NVS 持久化；SD 卡仅用于一次迁移及显式导入／导出
 * - TSCFG 加密配置支持
 */

#include "ts_led_color_correction.h"
#include "ts_log.h"
#include "ts_storage.h"
#include "ts_config_pack.h"
#include "ts_cert.h"
#include <errno.h>
#include "nvs.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>

#define TAG "led_cc"

/*===========================================================================*/
/*                          Global State                                      */
/*===========================================================================*/

static ts_led_cc_config_t g_config = {0};
static bool g_initialized = false;
static ts_led_cc_change_callback_t g_change_callback = NULL;

/* Gamma lookup table for performance */
static uint8_t g_gamma_lut[256];
static bool g_gamma_lut_valid = false;
static float g_gamma_lut_value = 0.0f;

/*===========================================================================*/
/*                          Internal Helpers                                  */
/*===========================================================================*/

/**
 * @brief Clamp float to [0.0, 1.0]
 */
static inline float clamp_float(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

/**
 * @brief Clamp int to [0, 255]
 */
static inline uint8_t clamp_uint8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

/**
 * @brief Validate configuration parameters
 */
static bool validate_config(const ts_led_cc_config_t *config)
{
    if (!config) return false;
    
    /* White point */
    if (config->white_point.red_scale < TS_LED_CC_SCALE_MIN ||
        config->white_point.red_scale > TS_LED_CC_SCALE_MAX ||
        config->white_point.green_scale < TS_LED_CC_SCALE_MIN ||
        config->white_point.green_scale > TS_LED_CC_SCALE_MAX ||
        config->white_point.blue_scale < TS_LED_CC_SCALE_MIN ||
        config->white_point.blue_scale > TS_LED_CC_SCALE_MAX) {
        return false;
    }
    
    /* Gamma */
    if (config->gamma.gamma < TS_LED_CC_GAMMA_MIN ||
        config->gamma.gamma > TS_LED_CC_GAMMA_MAX) {
        return false;
    }
    
    /* Brightness */
    if (config->brightness.factor < TS_LED_CC_SCALE_MIN ||
        config->brightness.factor > TS_LED_CC_SCALE_MAX) {
        return false;
    }
    
    /* Saturation */
    if (config->saturation.factor < TS_LED_CC_SCALE_MIN ||
        config->saturation.factor > TS_LED_CC_SCALE_MAX) {
        return false;
    }
    
    return true;
}

/**
 * @brief Initialize gamma lookup table
 * 
 * Gamma correction formula: output = input^gamma
 * - gamma = 1.0: passthrough (linear)
 * - gamma > 1.0: increase contrast (midtones darker)
 * - gamma < 1.0: decrease contrast (midtones brighter)
 * 
 * For LED displays showing sRGB content, gamma ≈ 2.2 decodes to linear.
 */
static void init_gamma_lut(float gamma)
{
    if (g_gamma_lut_valid && fabsf(g_gamma_lut_value - gamma) < 0.001f) {
        return;  /* Already initialized with same gamma */
    }
    
    for (int i = 0; i < 256; i++) {
        float normalized = i / 255.0f;
        /* Standard gamma: output = input^gamma */
        float corrected = powf(normalized, gamma);
        g_gamma_lut[i] = (uint8_t)(corrected * 255.0f + 0.5f);
    }
    
    g_gamma_lut_value = gamma;
    g_gamma_lut_valid = true;
    TS_LOGD(TAG, "Gamma LUT initialized: gamma=%.2f", gamma);
}

/**
 * @brief Apply gamma using LUT
 */
static inline uint8_t apply_gamma_lut(uint8_t value)
{
    return g_gamma_lut[value];
}

/**
 * @brief Notify configuration change
 */
static void notify_change(void)
{
    if (g_change_callback) {
        g_change_callback();
    }
}

/*===========================================================================*/
/*                      RGB ↔ HSL Conversion                                  */
/*===========================================================================*/

void ts_led_cc_rgb_to_hsl(const ts_led_rgb_t *rgb, ts_led_cc_hsl_t *hsl)
{
    float r = rgb->r / 255.0f;
    float g = rgb->g / 255.0f;
    float b = rgb->b / 255.0f;
    
    float max_val = fmaxf(r, fmaxf(g, b));
    float min_val = fminf(r, fminf(g, b));
    float delta = max_val - min_val;
    
    /* Lightness */
    hsl->l = (max_val + min_val) / 2.0f;
    
    if (delta < 0.0001f) {
        /* Achromatic */
        hsl->h = 0.0f;
        hsl->s = 0.0f;
    } else {
        /* Saturation */
        if (hsl->l < 0.5f) {
            hsl->s = delta / (max_val + min_val);
        } else {
            hsl->s = delta / (2.0f - max_val - min_val);
        }
        
        /* Hue */
        if (max_val == r) {
            hsl->h = ((g - b) / delta) * 60.0f;
            if (g < b) hsl->h += 360.0f;
        } else if (max_val == g) {
            hsl->h = ((b - r) / delta + 2.0f) * 60.0f;
        } else {
            hsl->h = ((r - g) / delta + 4.0f) * 60.0f;
        }
    }
}

static float hue_to_rgb(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f/6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f/2.0f) return q;
    if (t < 2.0f/3.0f) return p + (q - p) * (2.0f/3.0f - t) * 6.0f;
    return p;
}

void ts_led_cc_hsl_to_rgb(const ts_led_cc_hsl_t *hsl, ts_led_rgb_t *rgb)
{
    float h = hsl->h / 360.0f;
    float s = clamp_float(hsl->s);
    float l = clamp_float(hsl->l);
    
    if (s < 0.0001f) {
        /* Achromatic */
        rgb->r = rgb->g = rgb->b = (uint8_t)(l * 255.0f + 0.5f);
    } else {
        float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
        float p = 2.0f * l - q;
        
        float r = hue_to_rgb(p, q, h + 1.0f/3.0f);
        float g = hue_to_rgb(p, q, h);
        float b = hue_to_rgb(p, q, h - 1.0f/3.0f);
        
        rgb->r = (uint8_t)(r * 255.0f + 0.5f);
        rgb->g = (uint8_t)(g * 255.0f + 0.5f);
        rgb->b = (uint8_t)(b * 255.0f + 0.5f);
    }
}

/*===========================================================================*/
/*                      Core Functions                                        */
/*===========================================================================*/

esp_err_t ts_led_cc_get_default_config(ts_led_cc_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    memset(config, 0, sizeof(ts_led_cc_config_t));
    
    config->enabled = false;
    
    /* White point - neutral by default */
    config->white_point.enabled = false;
    config->white_point.red_scale = 1.0f;
    config->white_point.green_scale = 1.0f;
    config->white_point.blue_scale = 1.0f;
    
    /* Gamma - sRGB standard */
    config->gamma.enabled = false;
    config->gamma.gamma = TS_LED_CC_GAMMA_DEFAULT;
    
    /* Brightness - no change */
    config->brightness.enabled = false;
    config->brightness.factor = 1.0f;
    
    /* Saturation - no change */
    config->saturation.enabled = false;
    config->saturation.factor = 1.0f;
    
    return ESP_OK;
}

static esp_err_t store_config(const ts_led_cc_config_t *config);
static esp_err_t decode_config(const cJSON *json, ts_led_cc_config_t *out);
static esp_err_t read_config_file(const char *path, ts_led_cc_config_t *out);

esp_err_t ts_led_cc_init(void)
{
    if (g_initialized) {
        TS_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    /* Start with defaults */
    ts_led_cc_get_default_config(&g_config);
    
    nvs_handle_t handle;
    uint8_t storage_version = 0;
    esp_err_t ret = nvs_open(TS_LED_CC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_OK) {
        ret = nvs_get_u8(handle, "storage_v", &storage_version);
        nvs_close(handle);
    }
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) return ret;
    if (storage_version == 1) {
        ret = ts_led_cc_load_from_nvs();
    } else {
        /* Preserve the previous SD priority once, after generic NVS migration.
         * Subsequent startup and reset never read an old card file again. */
        const char *legacy = NULL;
        struct stat st;
        if (ts_storage_sd_mounted()) {
            if (stat(TS_LED_CC_SDCARD_TSCFG_PATH, &st) == 0) legacy = TS_LED_CC_SDCARD_TSCFG_PATH;
            else if (errno != ENOENT) return ESP_FAIL;
            else if (stat(TS_LED_CC_SDCARD_JSON_PATH, &st) == 0) legacy = TS_LED_CC_SDCARD_JSON_PATH;
            else if (errno != ENOENT) return ESP_FAIL;
        }
        if (legacy) {
            if (!strcmp(legacy, TS_LED_CC_SDCARD_TSCFG_PATH)) {
                ret = ts_cert_init();
                if (ret == ESP_OK) ret = ts_config_pack_init();
                if (ret != ESP_OK) return ret;
            }
            ret = read_config_file(legacy, &g_config);
        } else {
            ret = ts_led_cc_load_from_nvs();
            if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK; /* First installation uses defaults. */
        }
        if (ret == ESP_OK) ret = store_config(&g_config);
    }
    if (ret != ESP_OK) return ret;
    TS_LOGI(TAG, "Loaded color correction from system NVS");

    /* Initialize gamma LUT if enabled */
    if (g_config.gamma.enabled) {
        init_gamma_lut(g_config.gamma.gamma);
    }
    
    g_initialized = true;
    TS_LOGI(TAG, "Color correction initialized (enabled=%s)", 
            g_config.enabled ? "true" : "false");
    
    return ESP_OK;
}

esp_err_t ts_led_cc_deinit(void)
{
    if (!g_initialized) {
        return ESP_OK;
    }
    
    g_initialized = false;
    g_gamma_lut_valid = false;
    g_change_callback = NULL;
    
    TS_LOGI(TAG, "Color correction deinitialized");
    return ESP_OK;
}

bool ts_led_cc_is_initialized(void)
{
    return g_initialized;
}

/*===========================================================================*/
/*                      Configuration Functions                               */
/*===========================================================================*/

esp_err_t ts_led_cc_get_config(ts_led_cc_config_t *config)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(config, &g_config, sizeof(ts_led_cc_config_t));
    return ESP_OK;
}

esp_err_t ts_led_cc_set_config(const ts_led_cc_config_t *config)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!validate_config(config)) {
        TS_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = store_config(config);
    if (ret != ESP_OK) return ret;
    memcpy(&g_config, config, sizeof(ts_led_cc_config_t));
    if (g_config.gamma.enabled) init_gamma_lut(g_config.gamma.gamma);

    notify_change();
    TS_LOGI(TAG, "Configuration updated");
    
    return ESP_OK;
}

esp_err_t ts_led_cc_reset_config(void)
{
    ts_led_cc_config_t config;
    ts_led_cc_get_default_config(&config);
    return ts_led_cc_set_config(&config);
}

/*===========================================================================*/
/*                      Individual Parameter Setters                          */
/*===========================================================================*/

esp_err_t ts_led_cc_set_enabled(bool enabled)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    
    ts_led_cc_config_t candidate = g_config;
    candidate.enabled = enabled;
    esp_err_t ret = ts_led_cc_set_config(&candidate);
    if (ret != ESP_OK) return ret;
    
    TS_LOGI(TAG, "Color correction %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

bool ts_led_cc_is_enabled(void)
{
    return g_initialized && g_config.enabled;
}

esp_err_t ts_led_cc_set_white_point(bool enabled, float red, float green, float blue)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    
    if (red < TS_LED_CC_SCALE_MIN || red > TS_LED_CC_SCALE_MAX ||
        green < TS_LED_CC_SCALE_MIN || green > TS_LED_CC_SCALE_MAX ||
        blue < TS_LED_CC_SCALE_MIN || blue > TS_LED_CC_SCALE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_cc_config_t candidate = g_config;
    candidate.white_point.enabled = enabled;
    candidate.white_point.red_scale = red;
    candidate.white_point.green_scale = green;
    candidate.white_point.blue_scale = blue;
    
    esp_err_t ret = ts_led_cc_set_config(&candidate);
    if (ret != ESP_OK) return ret;
    
    TS_LOGI(TAG, "White point: %s (R:%.2f G:%.2f B:%.2f)",
            enabled ? "enabled" : "disabled", red, green, blue);
    return ESP_OK;
}

esp_err_t ts_led_cc_set_gamma(bool enabled, float gamma)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    
    if (gamma < TS_LED_CC_GAMMA_MIN || gamma > TS_LED_CC_GAMMA_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_cc_config_t candidate = g_config;
    candidate.gamma.enabled = enabled;
    candidate.gamma.gamma = gamma;
    
    
    esp_err_t ret = ts_led_cc_set_config(&candidate);
    if (ret != ESP_OK) return ret;
    
    TS_LOGI(TAG, "Gamma: %s (%.2f)", enabled ? "enabled" : "disabled", gamma);
    return ESP_OK;
}

esp_err_t ts_led_cc_set_brightness(bool enabled, float factor)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    
    if (factor < TS_LED_CC_SCALE_MIN || factor > TS_LED_CC_SCALE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_cc_config_t candidate = g_config;
    candidate.brightness.enabled = enabled;
    candidate.brightness.factor = factor;
    
    esp_err_t ret = ts_led_cc_set_config(&candidate);
    if (ret != ESP_OK) return ret;
    
    TS_LOGI(TAG, "Brightness: %s (%.2f)", enabled ? "enabled" : "disabled", factor);
    return ESP_OK;
}

esp_err_t ts_led_cc_set_saturation(bool enabled, float factor)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    
    if (factor < TS_LED_CC_SCALE_MIN || factor > TS_LED_CC_SCALE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_cc_config_t candidate = g_config;
    candidate.saturation.enabled = enabled;
    candidate.saturation.factor = factor;
    
    esp_err_t ret = ts_led_cc_set_config(&candidate);
    if (ret != ESP_OK) return ret;
    
    TS_LOGI(TAG, "Saturation: %s (%.2f)", enabled ? "enabled" : "disabled", factor);
    return ESP_OK;
}

/*===========================================================================*/
/*                      Color Correction Application                          */
/*===========================================================================*/

esp_err_t ts_led_cc_apply_pixel(const ts_led_rgb_t *input, ts_led_rgb_t *output)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!input || !output) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* If disabled, pass through */
    if (!g_config.enabled) {
        *output = *input;
        return ESP_OK;
    }
    
    ts_led_rgb_t working = *input;
    
    /* Apply white point correction */
    if (g_config.white_point.enabled) {
        float r = working.r * g_config.white_point.red_scale;
        float g = working.g * g_config.white_point.green_scale;
        float b = working.b * g_config.white_point.blue_scale;
        
        working.r = clamp_uint8((int)(r + 0.5f));
        working.g = clamp_uint8((int)(g + 0.5f));
        working.b = clamp_uint8((int)(b + 0.5f));
    }
    
    /* Apply gamma correction */
    if (g_config.gamma.enabled) {
        if (!g_gamma_lut_valid || fabsf(g_gamma_lut_value - g_config.gamma.gamma) > 0.001f) {
            init_gamma_lut(g_config.gamma.gamma);
        }
        working.r = apply_gamma_lut(working.r);
        working.g = apply_gamma_lut(working.g);
        working.b = apply_gamma_lut(working.b);
    }
    
    /* Apply brightness (direct RGB scaling for better accuracy) */
    if (g_config.brightness.enabled && fabsf(g_config.brightness.factor - 1.0f) > 0.001f) {
        float factor = g_config.brightness.factor;
        working.r = clamp_uint8((int)(working.r * factor + 0.5f));
        working.g = clamp_uint8((int)(working.g * factor + 0.5f));
        working.b = clamp_uint8((int)(working.b * factor + 0.5f));
    }
    
    /* Apply saturation (requires HSL conversion) */
    if (g_config.saturation.enabled && fabsf(g_config.saturation.factor - 1.0f) > 0.001f) {
        ts_led_cc_hsl_t hsl;
        ts_led_cc_rgb_to_hsl(&working, &hsl);
        hsl.s = clamp_float(hsl.s * g_config.saturation.factor);
        ts_led_cc_hsl_to_rgb(&hsl, &working);
    }
    
    *output = working;
    return ESP_OK;
}

esp_err_t ts_led_cc_apply_array(const ts_led_rgb_t *input, ts_led_rgb_t *output, size_t count)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!input || !output || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* If disabled, just copy */
    if (!g_config.enabled) {
        memcpy(output, input, count * sizeof(ts_led_rgb_t));
        return ESP_OK;
    }
    
    for (size_t i = 0; i < count; i++) {
        esp_err_t ret = ts_led_cc_apply_pixel(&input[i], &output[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    return ESP_OK;
}

esp_err_t ts_led_cc_apply_inplace(ts_led_rgb_t *pixels, size_t count)
{
    return ts_led_cc_apply_array(pixels, pixels, count);
}

/*===========================================================================*/
/*                      NVS Persistence                                       */
/*===========================================================================*/

static esp_err_t store_config(const ts_led_cc_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(TS_LED_CC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_blob(handle, "config", config, sizeof(*config));
    if (ret == ESP_OK) ret = nvs_set_u8(handle, "storage_v", 1);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    
    nvs_close(handle);
    
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to save to NVS: %s", esp_err_to_name(ret));
    } else {
        TS_LOGD(TAG, "Saved to NVS");
    }
    
    return ret;
}
esp_err_t ts_led_cc_save_to_nvs(void)
{
    return store_config(&g_config);
}

esp_err_t ts_led_cc_load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(TS_LED_CC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    
    ts_led_cc_config_t config;
    size_t size = sizeof(config);
    ret = nvs_get_blob(handle, "config", &config, &size);
    nvs_close(handle);
    
    if (ret != ESP_OK) {
        return ret;
    }
    
    if (!validate_config(&config)) {
        TS_LOGW(TAG, "Invalid config in NVS, using defaults");
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(&g_config, &config, sizeof(config));
    return ESP_OK;
}

/*===========================================================================*/
/*                      SD Card Persistence                                   */
/*===========================================================================*/

esp_err_t ts_led_cc_save_to_sdcard(const char *path)
{
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    
    const char *file_path = path ? path : TS_LED_CC_SDCARD_JSON_PATH;
    
    cJSON *json = ts_led_cc_config_to_json();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    
    char *json_str = cJSON_Print(json);
    cJSON_Delete(json);
    
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t ret = ts_storage_write_file(file_path, json_str, strlen(json_str));
    free(json_str);
    if (ret != ESP_OK) return ret;

    TS_LOGI(TAG, "Saved to SD card: %s", file_path);
    return ESP_OK;
}

static esp_err_t read_config_file(const char *path, ts_led_cc_config_t *out)
{
    const char *file_path = path;
    
    /* Auto-detect if no path specified */
    if (!file_path) {
        struct stat st;
        if (stat(TS_LED_CC_SDCARD_TSCFG_PATH, &st) == 0) {
            file_path = TS_LED_CC_SDCARD_TSCFG_PATH;
        } else if (stat(TS_LED_CC_SDCARD_JSON_PATH, &st) == 0) {
            file_path = TS_LED_CC_SDCARD_JSON_PATH;
        } else {
            return ESP_ERR_NOT_FOUND;
        }
    }
    
    /* Check if it's a tscfg file */
    const char *ext = strrchr(file_path, '.');
    bool is_tscfg = ext && strcmp(ext, ".tscfg") == 0;
    
    char *content = NULL;
    size_t content_len = 0;
    
    if (is_tscfg) {
        /* Load encrypted config */
        ts_config_pack_t *pack = NULL;
        ts_config_pack_result_t res = ts_config_pack_load(file_path, &pack);
        if (res != TS_CONFIG_PACK_OK) {
            TS_LOGE(TAG, "Failed to load tscfg: %s", ts_config_pack_strerror(res));
            return ESP_FAIL;
        }
        
        content = strdup(pack->content);
        content_len = pack->content_len;
        ts_config_pack_free(pack);
        
        if (!content) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        /* Load plain JSON */
        struct stat st;
        if (stat(file_path, &st) != 0) {
            return ESP_ERR_NOT_FOUND;
        }
        
        if (st.st_size > 4096) {
            TS_LOGE(TAG, "File too large: %ld", st.st_size);
            return ESP_ERR_INVALID_SIZE;
        }
        
        content = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!content) {
            content = malloc(st.st_size + 1);
        }
        if (!content) {
            return ESP_ERR_NO_MEM;
        }
        
        FILE *f = fopen(file_path, "r");
        if (!f) {
            free(content);
            return ESP_FAIL;
        }
        
        content_len = fread(content, 1, st.st_size, f);
        fclose(f);
        content[content_len] = '\0';
    }
    
    /* Parse JSON */
    cJSON *json = cJSON_Parse(content);
    free(content);
    
    if (!json) {
        TS_LOGE(TAG, "Failed to parse JSON from: %s", file_path);
        return ESP_FAIL;
    }
    
    esp_err_t ret = decode_config(json, out);
    cJSON_Delete(json);
    
    return ret;
}

esp_err_t ts_led_cc_load_from_sdcard(const char *path)
{
    ts_led_cc_config_t config;
    esp_err_t ret = read_config_file(path, &config);
    return ret == ESP_OK ? ts_led_cc_set_config(&config) : ret;
}

/*===========================================================================*/
/*                      JSON Conversion                                       */
/*===========================================================================*/

cJSON *ts_led_cc_config_to_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    
    /* Metadata */
    cJSON_AddStringToObject(root, "type", "led_color_correction");
    cJSON_AddStringToObject(root, "version", "1.0");
    
    /* Global enable */
    cJSON_AddBoolToObject(root, "enabled", g_config.enabled);
    
    /* White point */
    cJSON *wp = cJSON_CreateObject();
    if (wp) {
        cJSON_AddBoolToObject(wp, "enabled", g_config.white_point.enabled);
        cJSON_AddNumberToObject(wp, "red_scale", g_config.white_point.red_scale);
        cJSON_AddNumberToObject(wp, "green_scale", g_config.white_point.green_scale);
        cJSON_AddNumberToObject(wp, "blue_scale", g_config.white_point.blue_scale);
        cJSON_AddItemToObject(root, "white_point", wp);
    }
    
    /* Gamma */
    cJSON *gamma = cJSON_CreateObject();
    if (gamma) {
        cJSON_AddBoolToObject(gamma, "enabled", g_config.gamma.enabled);
        cJSON_AddNumberToObject(gamma, "gamma", g_config.gamma.gamma);
        cJSON_AddItemToObject(root, "gamma", gamma);
    }
    
    /* Brightness */
    cJSON *brightness = cJSON_CreateObject();
    if (brightness) {
        cJSON_AddBoolToObject(brightness, "enabled", g_config.brightness.enabled);
        cJSON_AddNumberToObject(brightness, "factor", g_config.brightness.factor);
        cJSON_AddItemToObject(root, "brightness", brightness);
    }
    
    /* Saturation */
    cJSON *saturation = cJSON_CreateObject();
    if (saturation) {
        cJSON_AddBoolToObject(saturation, "enabled", g_config.saturation.enabled);
        cJSON_AddNumberToObject(saturation, "factor", g_config.saturation.factor);
        cJSON_AddItemToObject(root, "saturation", saturation);
    }
    
    return root;
}

static esp_err_t decode_config(const cJSON *json, ts_led_cc_config_t *out)
{
    if (!cJSON_IsObject(json)) return ESP_ERR_INVALID_ARG;
    
    ts_led_cc_config_t config;
    memcpy(&config, &g_config, sizeof(config));
    
    /* Validate type */
    const cJSON *type = cJSON_GetObjectItem(json, "type");
    if (type && cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "led_color_correction") != 0) {
            TS_LOGW(TAG, "Invalid config type: %s", type->valuestring);
        }
    }
    
    /* Global enable */
    const cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(enabled)) {
        config.enabled = cJSON_IsTrue(enabled);
    }
    
    /* White point */
    const cJSON *wp = cJSON_GetObjectItem(json, "white_point");
    if (cJSON_IsObject(wp)) {
        const cJSON *wp_en = cJSON_GetObjectItem(wp, "enabled");
        const cJSON *red = cJSON_GetObjectItem(wp, "red_scale");
        const cJSON *green = cJSON_GetObjectItem(wp, "green_scale");
        const cJSON *blue = cJSON_GetObjectItem(wp, "blue_scale");
        
        if (cJSON_IsBool(wp_en)) config.white_point.enabled = cJSON_IsTrue(wp_en);
        if (cJSON_IsNumber(red)) config.white_point.red_scale = (float)red->valuedouble;
        if (cJSON_IsNumber(green)) config.white_point.green_scale = (float)green->valuedouble;
        if (cJSON_IsNumber(blue)) config.white_point.blue_scale = (float)blue->valuedouble;
    }
    
    /* Gamma */
    const cJSON *gamma = cJSON_GetObjectItem(json, "gamma");
    if (cJSON_IsObject(gamma)) {
        const cJSON *gamma_en = cJSON_GetObjectItem(gamma, "enabled");
        const cJSON *gamma_val = cJSON_GetObjectItem(gamma, "gamma");
        
        if (cJSON_IsBool(gamma_en)) config.gamma.enabled = cJSON_IsTrue(gamma_en);
        if (cJSON_IsNumber(gamma_val)) config.gamma.gamma = (float)gamma_val->valuedouble;
    }
    
    /* Brightness */
    const cJSON *brightness = cJSON_GetObjectItem(json, "brightness");
    if (cJSON_IsObject(brightness)) {
        const cJSON *br_en = cJSON_GetObjectItem(brightness, "enabled");
        const cJSON *br_val = cJSON_GetObjectItem(brightness, "factor");
        
        if (cJSON_IsBool(br_en)) config.brightness.enabled = cJSON_IsTrue(br_en);
        if (cJSON_IsNumber(br_val)) config.brightness.factor = (float)br_val->valuedouble;
    }
    
    /* Saturation */
    const cJSON *saturation = cJSON_GetObjectItem(json, "saturation");
    if (cJSON_IsObject(saturation)) {
        const cJSON *sat_en = cJSON_GetObjectItem(saturation, "enabled");
        const cJSON *sat_val = cJSON_GetObjectItem(saturation, "factor");
        
        if (cJSON_IsBool(sat_en)) config.saturation.enabled = cJSON_IsTrue(sat_en);
        if (cJSON_IsNumber(sat_val)) config.saturation.factor = (float)sat_val->valuedouble;
    }
    
    if (!validate_config(&config)) {
        TS_LOGE(TAG, "Invalid configuration from JSON");
        return ESP_ERR_INVALID_ARG;
    }
    
    *out = config;
    return ESP_OK;
}

esp_err_t ts_led_cc_config_from_json(const cJSON *json)
{
    ts_led_cc_config_t config;
    esp_err_t ret = decode_config(json, &config);
    return ret == ESP_OK ? ts_led_cc_set_config(&config) : ret;
}

char *ts_led_cc_export_json_string(bool pretty)
{
    cJSON *json = ts_led_cc_config_to_json();
    if (!json) return NULL;
    
    char *str = pretty ? cJSON_Print(json) : cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    return str;
}

esp_err_t ts_led_cc_import_json_string(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;
    
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        TS_LOGE(TAG, "Failed to parse JSON string");
        return ESP_FAIL;
    }
    
    esp_err_t ret = ts_led_cc_config_from_json(json);
    cJSON_Delete(json);
    
    
    return ret;
}

/*===========================================================================*/
/*                      Callback Functions                                    */
/*===========================================================================*/

esp_err_t ts_led_cc_register_change_callback(ts_led_cc_change_callback_t callback)
{
    g_change_callback = callback;
    return ESP_OK;
}
