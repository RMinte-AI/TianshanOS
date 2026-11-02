/**
 * @file ts_led_qrcode.c
 * @brief QR Code generation for LED matrix display
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_led_qrcode.h"
#include "ts_led.h"
#include "ts_led_image.h"
#include "qrcodegen/qrcodegen.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "ts_led_qrcode";

/*===========================================================================*/
/*                          Data Capacity Tables                              */
/*===========================================================================*/

// Data capacity for alphanumeric mode (chars) at each version/ECC level
// Indexed by [ecc][version-1]
static const int16_t ALPHANUMERIC_CAPACITY[4][40] = {
    // LOW
    {25, 47, 77, 114, 154, 195, 224, 279, 335, 395, 468, 535, 619, 667, 758, 854, 938, 1046, 1153, 1249, 1352, 1460, 1588, 1704, 1853, 1990, 2132, 2223, 2369, 2520, 2677, 2840, 3009, 3183, 3351, 3537, 3729, 3927, 4087, 4296},
    // MEDIUM
    {20, 38, 61, 90, 122, 154, 178, 221, 262, 311, 366, 419, 483, 528, 600, 656, 734, 816, 909, 970, 1035, 1134, 1248, 1326, 1451, 1542, 1637, 1732, 1839, 1994, 2113, 2238, 2369, 2506, 2632, 2780, 2894, 3054, 3220, 3391},
    // QUARTILE
    {16, 29, 47, 67, 87, 108, 125, 157, 189, 221, 259, 296, 352, 376, 426, 470, 531, 574, 644, 702, 742, 823, 890, 963, 1041, 1094, 1172, 1263, 1322, 1429, 1499, 1618, 1700, 1787, 1867, 1966, 2071, 2181, 2298, 2420},
    // HIGH
    {10, 20, 35, 50, 64, 84, 93, 122, 143, 174, 200, 227, 259, 283, 321, 365, 408, 452, 493, 557, 587, 640, 672, 744, 779, 864, 910, 958, 1016, 1080, 1150, 1226, 1307, 1394, 1431, 1530, 1591, 1658, 1774, 1852}
};

// Data capacity for byte mode at each version/ECC level
static const int16_t BYTE_CAPACITY[4][40] = {
    // LOW
    {17, 32, 53, 78, 106, 134, 154, 192, 230, 271, 321, 367, 425, 458, 520, 586, 644, 718, 792, 858, 929, 1003, 1091, 1171, 1273, 1367, 1465, 1528, 1628, 1732, 1840, 1952, 2068, 2188, 2303, 2431, 2563, 2699, 2809, 2953},
    // MEDIUM
    {14, 26, 42, 62, 84, 106, 122, 152, 180, 213, 251, 287, 331, 362, 412, 450, 504, 560, 624, 666, 711, 779, 857, 911, 997, 1059, 1125, 1190, 1264, 1370, 1452, 1538, 1628, 1722, 1809, 1911, 1989, 2099, 2213, 2331},
    // QUARTILE
    {11, 20, 32, 46, 60, 74, 86, 108, 130, 151, 177, 203, 241, 258, 292, 322, 364, 394, 442, 482, 509, 565, 611, 661, 715, 751, 805, 868, 908, 982, 1030, 1112, 1168, 1228, 1283, 1351, 1423, 1499, 1579, 1663},
    // HIGH
    {7, 14, 24, 34, 44, 58, 64, 84, 98, 119, 137, 155, 177, 194, 220, 250, 280, 310, 338, 382, 403, 439, 461, 511, 535, 593, 625, 658, 698, 742, 790, 842, 898, 958, 983, 1051, 1093, 1139, 1219, 1273}
};

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

const char *ts_led_qr_ecc_name(ts_led_qr_ecc_t ecc)
{
    static const char *names[] = {"L", "M", "Q", "H"};
    if (ecc >= 0 && ecc <= 3) {
        return names[ecc];
    }
    return "?";
}

esp_err_t ts_led_qr_ecc_parse(const char *str, ts_led_qr_ecc_t *ecc)
{
    if (!str || !ecc) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Convert to uppercase for comparison
    char upper[16] = {0};
    for (int i = 0; i < 15 && str[i]; i++) {
        upper[i] = toupper((unsigned char)str[i]);
    }
    
    if (strcmp(upper, "L") == 0 || strcmp(upper, "LOW") == 0) {
        *ecc = TS_LED_QR_ECC_LOW;
        return ESP_OK;
    }
    if (strcmp(upper, "M") == 0 || strcmp(upper, "MEDIUM") == 0) {
        *ecc = TS_LED_QR_ECC_MEDIUM;
        return ESP_OK;
    }
    if (strcmp(upper, "Q") == 0 || strcmp(upper, "QUARTILE") == 0) {
        *ecc = TS_LED_QR_ECC_QUARTILE;
        return ESP_OK;
    }
    if (strcmp(upper, "H") == 0 || strcmp(upper, "HIGH") == 0) {
        *ecc = TS_LED_QR_ECC_HIGH;
        return ESP_OK;
    }
    
    return ESP_ERR_INVALID_ARG;
}

int ts_led_qr_get_capacity(int version, ts_led_qr_ecc_t ecc, bool is_alphanumeric)
{
    if (version < 1 || version > 40 || ecc < 0 || ecc > 3) {
        return -1;
    }
    
    if (is_alphanumeric) {
        return ALPHANUMERIC_CAPACITY[ecc][version - 1];
    } else {
        return BYTE_CAPACITY[ecc][version - 1];
    }
}

// Check if string is alphanumeric (for QR code purposes)
static bool is_qr_alphanumeric(const char *text)
{
    static const char *ALPHANUMERIC_CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
    for (; *text != '\0'; text++) {
        if (strchr(ALPHANUMERIC_CHARSET, *text) == NULL) {
            return false;
        }
    }
    return true;
}

/*===========================================================================*/
/*                          QR Code Display                                   */
/*===========================================================================*/

esp_err_t ts_led_qrcode_show(ts_led_layer_t layer, 
                              const ts_led_qr_config_t *config,
                              ts_led_qr_result_t *result)
{
    if (!layer || !config || !config->text) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get device info to know matrix size
    // For now, assume 32x32 matrix
    const int matrix_size = 32;
    
    // Determine version range
    // QR v4 = 33x33, slightly larger than 32x32
    // We'll allow v1-v4, cropping v4 slightly
    int min_ver = config->version_min > 0 ? config->version_min : 1;
    int max_ver = config->version_max > 0 ? config->version_max : TS_LED_QR_VERSION_MAX_32x32;
    
    // Clamp to valid range for matrix
    if (min_ver < 1) min_ver = 1;
    if (max_ver > TS_LED_QR_VERSION_MAX_32x32) max_ver = TS_LED_QR_VERSION_MAX_32x32;
    if (min_ver > max_ver) min_ver = max_ver;
    
    ESP_LOGI(TAG, "Generating QR code: text='%s', ecc=%s, ver=%d-%d", 
             config->text, ts_led_qr_ecc_name(config->ecc), min_ver, max_ver);
    
    // Allocate buffers for QR code generation
    // QR v4 = 33x33 = 1089 modules, need ~137 bytes for qrcode buffer
    // Using buffer size for version 4
    uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(4)];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_FOR_VERSION(4)];
    
    // Convert ECC level
    enum qrcodegen_Ecc qr_ecc;
    switch (config->ecc) {
        case TS_LED_QR_ECC_LOW:      qr_ecc = qrcodegen_Ecc_LOW; break;
        case TS_LED_QR_ECC_MEDIUM:   qr_ecc = qrcodegen_Ecc_MEDIUM; break;
        case TS_LED_QR_ECC_QUARTILE: qr_ecc = qrcodegen_Ecc_QUARTILE; break;
        case TS_LED_QR_ECC_HIGH:     qr_ecc = qrcodegen_Ecc_HIGH; break;
        default: qr_ecc = qrcodegen_Ecc_MEDIUM; break;
    }
    
    // Generate QR code
    bool ok = qrcodegen_encodeText(config->text, tempBuffer, qrcode,
                                    qr_ecc, min_ver, max_ver,
                                    qrcodegen_Mask_AUTO, true);
    
    if (!ok) {
        ESP_LOGE(TAG, "Failed to generate QR code - text too long or invalid");
        return ESP_ERR_INVALID_SIZE;
    }
    
    int qr_size = qrcodegen_getSize(qrcode);
    int qr_version = (qr_size - 17) / 4;
    
    ESP_LOGI(TAG, "Generated QR v%d, size=%dx%d", qr_version, qr_size, qr_size);
    
    // Calculate offset for centering or cropping
    int offset_x = 0;
    int offset_y = 0;
    int crop = 0;
    
    if (qr_size <= matrix_size) {
        // QR fits in matrix, center it if requested
        if (config->center) {
            offset_x = (matrix_size - qr_size) / 2;
            offset_y = (matrix_size - qr_size) / 2;
        }
    } else {
        // QR is larger than matrix (v4 = 33x33)
        // Crop edges to fit - remove quiet zone partially
        crop = (qr_size - matrix_size + 1) / 2;  // e.g., (33-32+1)/2 = 1
        ESP_LOGD(TAG, "QR v%d (%dx%d) cropped %dpx edges to fit %dx%d matrix", 
                 qr_version, qr_size, qr_size, crop, matrix_size, matrix_size);
    }
    
    // Get background image info if provided
    ts_led_image_info_t bg_info = {0};
    if (config->bg_image) {
        ts_led_image_get_info(config->bg_image, &bg_info);
        ESP_LOGI(TAG, "Using background image: %dx%d", bg_info.width, bg_info.height);
    }
    
    // Successful QR generation takes over this layer only; overlays are independent.
    ts_led_animation_stop(layer);
    // Clear layer with background color
    ts_led_fill(layer, config->bg_color);
    
    // Draw QR code modules
    for (int y = 0; y < qr_size && (y - crop) < matrix_size; y++) {
        for (int x = 0; x < qr_size && (x - crop) < matrix_size; x++) {
            // Skip cropped edges
            if (x < crop || y < crop) continue;
            
            int mx = x - crop + offset_x;  // Matrix X position
            int my = y - crop + offset_y;  // Matrix Y position
            
            // Bounds check
            if (mx < 0 || mx >= matrix_size || my < 0 || my >= matrix_size) continue;
            
            // Get module color (true = dark = foreground)
            bool is_dark = qrcodegen_getModule(qrcode, x, y);
            ts_led_rgb_t color;
            
            if (is_dark) {
                // Foreground: use background image pixel if available
                if (config->bg_image) {
                    // Map matrix position to image position with scaling
                    uint16_t img_x = (uint16_t)(mx * bg_info.width / matrix_size);
                    uint16_t img_y = (uint16_t)(my * bg_info.height / matrix_size);
                    if (ts_led_image_get_pixel(config->bg_image, img_x, img_y, &color) == ESP_OK) {
                        // Calculate luminance: Y = 0.299*R + 0.587*G + 0.114*B
                        // Using integer approximation: (77*R + 150*G + 29*B) >> 8
                        uint8_t luminance = (77 * color.r + 150 * color.g + 29 * color.b) >> 8;
                        
                        // If too dark (low luminance), invert to ensure QR readability
                        if (luminance < 96) {
                            color.r = 255 - color.r;
                            color.g = 255 - color.g;
                            color.b = 255 - color.b;
                        }
                    } else {
                        color = config->fg_color;
                    }
                } else {
                    color = config->fg_color;
                }
            } else {
                color = config->bg_color;
            }
            
            ts_led_set_pixel_xy(layer, mx, my, color);
        }
    }
    
    // Fill result if requested
    if (result) {
        result->version = qr_version;
        result->size = qr_size;
        
        // Calculate remaining capacity
        bool is_alphanum = is_qr_alphanumeric(config->text);
        int max_capacity = ts_led_qr_get_capacity(qr_version, config->ecc, is_alphanum);
        int text_len = strlen(config->text);
        result->data_capacity = max_capacity - text_len;
    }
    
    ESP_LOGI(TAG, "QR code displayed successfully");
    return ESP_OK;
}

esp_err_t ts_led_qrcode_show_on_device(const char *device_name,
                                        const ts_led_qr_config_t *config,
                                        ts_led_qr_result_t *result)
{
    if (!device_name) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_led_device_t device = ts_led_device_get(device_name);
    if (!device) {
        ESP_LOGE(TAG, "Device '%s' not found", device_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Get layer 0
    ts_led_layer_t layer = ts_led_layer_get(device, 0);
    if (!layer) {
        ESP_LOGE(TAG, "Failed to get layer for device '%s'", device_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_err_t err = ts_led_qrcode_show(layer, config, result);
    
    // Refresh device to display changes
    if (err == ESP_OK) {
        ts_led_device_refresh(device);
    }
    
    return err;
}
