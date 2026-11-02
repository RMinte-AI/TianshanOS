/**
 * @file ts_api_gpio.c
 * @brief GPIO API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_log.h"
#include "driver/gpio.h"
#include <string.h>

#define TAG "api_gpio"

/*===========================================================================*/
/*                          Controllable Pins Definition                      */
/*===========================================================================*/

typedef struct {
    int pin;
    const char *name;
    const char *description;
    int default_level;
} gpio_pin_info_t;

static const gpio_pin_info_t s_controllable_pins[] = {
    {1,  "AGX_RESET",         "Inference Computer reset (HIGH=reset)", 0},
    {2,  "LPMU_RESET",        "Application Computer reset (HIGH=reset)", 0},
    {3,  "AGX_FORCE_SHUTDOWN", "Inference Computer force shutdown", 0},
    {8,  "USB_MUX_0",         "USB MUX select bit 0", 0},
    {17, "RTL8367_RST",       "Network switch reset", 0},
    {39, "ETH_RST",           "W5500 Ethernet reset (LOW=reset)", 1},
    {40, "AGX_RECOVERY",      "Inference Computer recovery mode", 0},
    {46, "LPMU_POWER",        "Application Computer power button", 0},
    {48, "USB_MUX_1",         "USB MUX select bit 1", 0},
};

#define NUM_CONTROLLABLE_PINS (sizeof(s_controllable_pins) / sizeof(s_controllable_pins[0]))

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static const gpio_pin_info_t *find_pin_by_number(int pin)
{
    for (int i = 0; i < NUM_CONTROLLABLE_PINS; i++) {
        if (s_controllable_pins[i].pin == pin) {
            return &s_controllable_pins[i];
        }
    }
    return NULL;
}

static const gpio_pin_info_t *find_pin_by_name(const char *name)
{
    for (int i = 0; i < NUM_CONTROLLABLE_PINS; i++) {
        if (strcasecmp(s_controllable_pins[i].name, name) == 0) {
            return &s_controllable_pins[i];
        }
    }
    return NULL;
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief gpio.list - List all controllable GPIO pins
 */
static esp_err_t api_gpio_list(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON *pins = cJSON_AddArrayToObject(data, "pins");
    
    for (int i = 0; i < NUM_CONTROLLABLE_PINS; i++) {
        const gpio_pin_info_t *info = &s_controllable_pins[i];
        int level = gpio_get_level(info->pin);
        
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "gpio", info->pin);
        cJSON_AddStringToObject(item, "name", info->name);
        cJSON_AddStringToObject(item, "description", info->description);
        cJSON_AddNumberToObject(item, "level", level);
        cJSON_AddNumberToObject(item, "default", info->default_level);
        cJSON_AddBoolToObject(item, "modified", level != info->default_level);
        cJSON_AddItemToArray(pins, item);
    }
    
    cJSON_AddNumberToObject(data, "count", NUM_CONTROLLABLE_PINS);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief gpio.info - Get specific GPIO pin info
 * 
 * @param params { "pin": 1 } or { "name": "AGX_RESET" }
 */
static esp_err_t api_gpio_info(const cJSON *params, ts_api_result_t *result)
{
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const gpio_pin_info_t *info = NULL;
    
    const cJSON *pin_param = cJSON_GetObjectItem(params, "pin");
    if (pin_param && cJSON_IsNumber(pin_param)) {
        info = find_pin_by_number(pin_param->valueint);
    }
    
    if (!info) {
        const cJSON *name_param = cJSON_GetObjectItem(params, "name");
        if (name_param && cJSON_IsString(name_param)) {
            info = find_pin_by_name(name_param->valuestring);
        }
    }
    
    if (!info) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "GPIO pin not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    int level = gpio_get_level(info->pin);
    
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddNumberToObject(data, "gpio", info->pin);
    cJSON_AddStringToObject(data, "name", info->name);
    cJSON_AddStringToObject(data, "description", info->description);
    cJSON_AddNumberToObject(data, "level", level);
    cJSON_AddNumberToObject(data, "default", info->default_level);
    cJSON_AddBoolToObject(data, "modified", level != info->default_level);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief gpio.set - Set GPIO level
 * 
 * @param params { "pin": 1, "level": 1 } or { "name": "AGX_RESET", "level": 1 }
 */
static esp_err_t api_gpio_set(const cJSON *params, ts_api_result_t *result)
{
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const gpio_pin_info_t *info = NULL;
    
    const cJSON *pin_param = cJSON_GetObjectItem(params, "pin");
    if (pin_param && cJSON_IsNumber(pin_param)) {
        info = find_pin_by_number(pin_param->valueint);
    }
    
    if (!info) {
        const cJSON *name_param = cJSON_GetObjectItem(params, "name");
        if (name_param && cJSON_IsString(name_param)) {
            info = find_pin_by_name(name_param->valuestring);
        }
    }
    
    if (!info) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "GPIO pin not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    const cJSON *level_param = cJSON_GetObjectItem(params, "level");
    if (level_param == NULL || !cJSON_IsNumber(level_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'level' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    int level = level_param->valueint ? 1 : 0;
    
    /* Configure as output and set level */
    gpio_set_level(info->pin, level);
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << info->pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "gpio", info->pin);
    cJSON_AddStringToObject(data, "name", info->name);
    cJSON_AddNumberToObject(data, "level", level);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief gpio.toggle - Toggle GPIO level
 * 
 * @param params { "pin": 1 } or { "name": "AGX_RESET" }
 */
static esp_err_t api_gpio_toggle(const cJSON *params, ts_api_result_t *result)
{
    if (params == NULL) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const gpio_pin_info_t *info = NULL;
    
    const cJSON *pin_param = cJSON_GetObjectItem(params, "pin");
    if (pin_param && cJSON_IsNumber(pin_param)) {
        info = find_pin_by_number(pin_param->valueint);
    }
    
    if (!info) {
        const cJSON *name_param = cJSON_GetObjectItem(params, "name");
        if (name_param && cJSON_IsString(name_param)) {
            info = find_pin_by_name(name_param->valuestring);
        }
    }
    
    if (!info) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "GPIO pin not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    int current_level = gpio_get_level(info->pin);
    int new_level = current_level ? 0 : 1;
    
    gpio_set_level(info->pin, new_level);
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << info->pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "gpio", info->pin);
    cJSON_AddStringToObject(data, "name", info->name);
    cJSON_AddNumberToObject(data, "level", new_level);
    cJSON_AddNumberToObject(data, "previous", current_level);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t s_gpio_endpoints[] = {
    {
        .name = "gpio.list",
        .description = "List all controllable GPIO pins",
        .category = TS_API_CAT_HAL,
        .handler = api_gpio_list,
        .requires_auth = false
    },
    {
        .name = "gpio.info",
        .description = "Get GPIO pin info",
        .category = TS_API_CAT_HAL,
        .handler = api_gpio_info,
        .requires_auth = false
    },
    {
        .name = "gpio.set",
        .description = "Set GPIO level",
        .category = TS_API_CAT_HAL,
        .handler = api_gpio_set,
        .requires_auth = true
    },
    {
        .name = "gpio.toggle",
        .description = "Toggle GPIO level",
        .category = TS_API_CAT_HAL,
        .handler = api_gpio_toggle,
        .requires_auth = true
    }
};

esp_err_t ts_api_gpio_register(void)
{
    TS_LOGI(TAG, "Registering GPIO APIs...");
    
    esp_err_t ret = ts_api_register_multiple(
        s_gpio_endpoints, 
        sizeof(s_gpio_endpoints) / sizeof(s_gpio_endpoints[0])
    );
    
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "GPIO APIs registered (%zu endpoints)", 
            sizeof(s_gpio_endpoints) / sizeof(s_gpio_endpoints[0]));
    }
    
    return ret;
}
