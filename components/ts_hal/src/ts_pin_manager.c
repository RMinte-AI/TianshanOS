/**
 * @file ts_pin_manager.c
 * @brief TianShanOS Pin Manager Implementation
 * 
 * Runtime pin configuration and conflict management.
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_pin_manager.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM */
#include "ts_log.h"
#include "ts_config.h"
#include "ts_config_pack.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "ts_pin"

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

#define PIN_MANAGER_NVS_NAMESPACE   "ts_pins"
#define PIN_MANAGER_NVS_KEY         "pin_map"
#define MAX_OWNER_NAME_LEN          32
#define MAX_GPIO_NUM                SOC_GPIO_PIN_COUNT

/*===========================================================================*/
/*                          Function Name Table                               */
/*===========================================================================*/

typedef struct {
    ts_pin_function_t function;
    const char *name;
    const char *description;
} pin_function_info_t;

static const pin_function_info_t s_function_names[] = {
    /* LED System */
    { TS_PIN_FUNC_LED_TOUCH, "LED_TOUCH", "Touch LED data line" },
    { TS_PIN_FUNC_LED_BOARD, "LED_BOARD", "Board LED data line" },
    { TS_PIN_FUNC_LED_MATRIX, "LED_MATRIX", "Matrix LED data line" },
    { TS_PIN_FUNC_LED_STATUS, "LED_STATUS", "Status indicator LED" },
    
    /* Fan System */
    { TS_PIN_FUNC_FAN_PWM_0, "FAN_PWM_0", "Fan 0 PWM control" },
    { TS_PIN_FUNC_FAN_PWM_1, "FAN_PWM_1", "Fan 1 PWM control" },
    { TS_PIN_FUNC_FAN_PWM_2, "FAN_PWM_2", "Fan 2 PWM control" },
    { TS_PIN_FUNC_FAN_PWM_3, "FAN_PWM_3", "Fan 3 PWM control" },
    { TS_PIN_FUNC_FAN_TACH_0, "FAN_TACH_0", "Fan 0 tachometer" },
    { TS_PIN_FUNC_FAN_TACH_1, "FAN_TACH_1", "Fan 1 tachometer" },
    { TS_PIN_FUNC_FAN_TACH_2, "FAN_TACH_2", "Fan 2 tachometer" },
    { TS_PIN_FUNC_FAN_TACH_3, "FAN_TACH_3", "Fan 3 tachometer" },
    
    /* Ethernet */
    { TS_PIN_FUNC_ETH_MISO, "ETH_MISO", "Ethernet SPI MISO" },
    { TS_PIN_FUNC_ETH_MOSI, "ETH_MOSI", "Ethernet SPI MOSI" },
    { TS_PIN_FUNC_ETH_SCLK, "ETH_SCLK", "Ethernet SPI SCLK" },
    { TS_PIN_FUNC_ETH_CS, "ETH_CS", "Ethernet SPI CS" },
    { TS_PIN_FUNC_ETH_INT, "ETH_INT", "Ethernet interrupt" },
    { TS_PIN_FUNC_ETH_RST, "ETH_RST", "Ethernet reset" },
    
    /* USB MUX */
    { TS_PIN_FUNC_USB_MUX_0, "USB_MUX_0", "USB MUX select 0" },
    { TS_PIN_FUNC_USB_MUX_1, "USB_MUX_1", "USB MUX select 1" },
    { TS_PIN_FUNC_USB_MUX_2, "USB_MUX_2", "USB MUX select 2" },
    { TS_PIN_FUNC_USB_MUX_3, "USB_MUX_3", "USB MUX select 3" },
    
    /* Device Control */
    { TS_PIN_FUNC_AGX_POWER, "AGX_POWER", "AGX power control" },
    { TS_PIN_FUNC_AGX_RESET, "AGX_RESET", "AGX reset" },
    { TS_PIN_FUNC_AGX_FORCE_RECOVERY, "AGX_FORCE_RECOVERY", "AGX force recovery" },
    { TS_PIN_FUNC_LPMU_POWER, "LPMU_POWER", "LPMU power control" },
    { TS_PIN_FUNC_LPMU_RESET, "LPMU_RESET", "LPMU reset" },
    { TS_PIN_FUNC_RTL8367_RST, "RTL8367_RST", "RTL8367 switch reset" },
    
    /* Power Monitoring */
    { TS_PIN_FUNC_POWER_ADC, "POWER_ADC", "Power ADC input" },
    { TS_PIN_FUNC_POWER_UART_TX, "POWER_UART_TX", "Power module UART TX" },
    { TS_PIN_FUNC_POWER_UART_RX, "POWER_UART_RX", "Power module UART RX" },
    
    /* SD Card */
    { TS_PIN_FUNC_SD_CMD, "SD_CMD", "SD card CMD" },
    { TS_PIN_FUNC_SD_CLK, "SD_CLK", "SD card CLK" },
    { TS_PIN_FUNC_SD_D0, "SD_D0", "SD card D0" },
    { TS_PIN_FUNC_SD_D1, "SD_D1", "SD card D1" },
    { TS_PIN_FUNC_SD_D2, "SD_D2", "SD card D2" },
    { TS_PIN_FUNC_SD_D3, "SD_D3", "SD card D3" },
    { TS_PIN_FUNC_SD_DETECT, "SD_DETECT", "SD card detect" },
    
    /* I2C */
    { TS_PIN_FUNC_I2C0_SDA, "I2C0_SDA", "I2C bus 0 SDA" },
    { TS_PIN_FUNC_I2C0_SCL, "I2C0_SCL", "I2C bus 0 SCL" },
    { TS_PIN_FUNC_I2C1_SDA, "I2C1_SDA", "I2C bus 1 SDA" },
    { TS_PIN_FUNC_I2C1_SCL, "I2C1_SCL", "I2C bus 1 SCL" },
    
    /* UART */
    { TS_PIN_FUNC_UART1_TX, "UART1_TX", "UART1 TX" },
    { TS_PIN_FUNC_UART1_RX, "UART1_RX", "UART1 RX" },
    { TS_PIN_FUNC_UART2_TX, "UART2_TX", "UART2 TX" },
    { TS_PIN_FUNC_UART2_RX, "UART2_RX", "UART2 RX" },
    
    /* Debug */
    { TS_PIN_FUNC_DEBUG_0, "DEBUG_0", "Debug pin 0" },
    { TS_PIN_FUNC_DEBUG_1, "DEBUG_1", "Debug pin 1" },
};

#define FUNCTION_NAME_COUNT (sizeof(s_function_names) / sizeof(s_function_names[0]))

/*===========================================================================*/
/*                              Private Types                                 */
/*===========================================================================*/

typedef struct {
    int8_t gpio_num;                        /* Physical GPIO number (-1 if not mapped) */
    ts_pin_state_t state;                   /* Current state */
    char owner[MAX_OWNER_NAME_LEN];         /* Owner service name */
} pin_entry_t;

typedef struct {
    ts_pin_function_t function;             /* Function assigned to this GPIO */
    uint16_t capabilities;                  /* Pin capabilities */
} gpio_entry_t;

/*===========================================================================*/
/*                              Private Data                                  */
/*===========================================================================*/

static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;
static pin_entry_t s_pin_map[TS_PIN_FUNC_MAX];
static gpio_entry_t s_gpio_map[MAX_GPIO_NUM];

/*===========================================================================*/
/*                          Private Functions                                 */
/*===========================================================================*/

static const pin_function_info_t *get_function_info(ts_pin_function_t function)
{
    for (size_t i = 0; i < FUNCTION_NAME_COUNT; i++) {
        if (s_function_names[i].function == function) {
            return &s_function_names[i];
        }
    }
    return NULL;
}

static ts_pin_function_t get_function_by_name(const char *name)
{
    if (name == NULL) {
        return TS_PIN_FUNC_MAX;
    }
    
    for (size_t i = 0; i < FUNCTION_NAME_COUNT; i++) {
        if (strcasecmp(s_function_names[i].name, name) == 0) {
            return s_function_names[i].function;
        }
    }
    return TS_PIN_FUNC_MAX;
}

static uint16_t get_gpio_capabilities(int gpio_num)
{
    if (gpio_num < 0 || gpio_num >= MAX_GPIO_NUM) {
        return 0;
    }
    
    uint16_t caps = TS_PIN_CAP_GPIO_IN | TS_PIN_CAP_GPIO_OUT;
    
#if CONFIG_IDF_TARGET_ESP32S3
    /* ESP32S3 specific capabilities */
    if (gpio_num <= 21) {
        caps |= TS_PIN_CAP_ADC;
    }
    if (gpio_num >= 1 && gpio_num <= 14) {
        caps |= TS_PIN_CAP_TOUCH;
    }
    /* All GPIOs support PWM through LEDC */
    caps |= TS_PIN_CAP_PWM;
    
    /* Pull-up/down available on most pins */
    if (gpio_num != 0) {
        caps |= TS_PIN_CAP_PULLUP | TS_PIN_CAP_PULLDOWN;
    }
    
    /* Strapping pins */
    if (gpio_num == 0 || gpio_num == 3 || gpio_num == 45 || gpio_num == 46) {
        caps |= TS_PIN_CAP_STRAPPING;
    }
#endif
    
    return caps;
}

static void init_gpio_map(void)
{
    for (int i = 0; i < MAX_GPIO_NUM; i++) {
        s_gpio_map[i].function = TS_PIN_FUNC_MAX;
        s_gpio_map[i].capabilities = get_gpio_capabilities(i);
    }
}

static void init_pin_map(void)
{
    for (int i = 0; i < TS_PIN_FUNC_MAX; i++) {
        s_pin_map[i].gpio_num = -1;
        s_pin_map[i].state = TS_PIN_STATE_FREE;
        s_pin_map[i].owner[0] = '\0';
    }
}

/*===========================================================================*/
/*                         Public Functions                                   */
/*===========================================================================*/

esp_err_t ts_pin_manager_init(void)
{
    if (s_initialized) {
        TS_LOGW(TAG, "Pin manager already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    TS_LOGI(TAG, "Initializing pin manager");
    
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        TS_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    init_pin_map();
    init_gpio_map();
    
    s_initialized = true;
    TS_LOGI(TAG, "Pin manager initialized");
    
    return ESP_OK;
}

esp_err_t ts_pin_manager_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    TS_LOGI(TAG, "Deinitializing pin manager");
    
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    
    s_initialized = false;
    return ESP_OK;
}

esp_err_t ts_pin_manager_load_config(const char *path)
{
    if (!s_initialized || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    TS_LOGI(TAG, "Loading pin config from: %s", path);
    
    /* 使用 .tscfg 优先加载 */
    char *content = NULL;
    size_t content_len = 0;
    bool used_tscfg = false;
    
    esp_err_t ret = ts_config_pack_load_with_priority(
        path, &content, &content_len, &used_tscfg);
    
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Failed to open pin config file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    
    if (used_tscfg) {
        TS_LOGI(TAG, "Loaded encrypted pin config from .tscfg");
    }
    
    /* Parse JSON */
    cJSON *root = cJSON_Parse(content);
    free(content);
    
    if (root == NULL) {
        TS_LOGE(TAG, "Failed to parse pin config JSON");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Parse pins object */
    cJSON *pins = cJSON_GetObjectItem(root, "pins");
    if (pins == NULL || !cJSON_IsObject(pins)) {
        TS_LOGE(TAG, "Missing 'pins' object in config");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    int loaded_count = 0;
    cJSON *pin_item = NULL;
    cJSON_ArrayForEach(pin_item, pins) {
        const char *func_name = pin_item->string;
        ts_pin_function_t func = get_function_by_name(func_name);
        
        if (func == TS_PIN_FUNC_MAX) {
            TS_LOGW(TAG, "Unknown pin function: %s", func_name);
            continue;
        }
        
        cJSON *gpio_obj = cJSON_GetObjectItem(pin_item, "gpio");
        if (gpio_obj == NULL || !cJSON_IsNumber(gpio_obj)) {
            TS_LOGW(TAG, "Invalid gpio for function: %s", func_name);
            continue;
        }
        
        int gpio_num = gpio_obj->valueint;
        if (gpio_num < 0 || gpio_num >= MAX_GPIO_NUM) {
            TS_LOGW(TAG, "GPIO %d out of range for function: %s", gpio_num, func_name);
            continue;
        }
        
        /* Check for conflicts */
        if (s_gpio_map[gpio_num].function != TS_PIN_FUNC_MAX &&
            s_gpio_map[gpio_num].function != func) {
            TS_LOGW(TAG, "GPIO %d already assigned to another function", gpio_num);
            continue;
        }
        
        /* Apply mapping */
        s_pin_map[func].gpio_num = gpio_num;
        s_gpio_map[gpio_num].function = func;
        loaded_count++;
        
        TS_LOGD(TAG, "Mapped %s -> GPIO%d", func_name, gpio_num);
    }
    
    xSemaphoreGive(s_mutex);
    cJSON_Delete(root);
    
    TS_LOGI(TAG, "Loaded %d pin mappings from config", loaded_count);
    
    return ESP_OK;
}

esp_err_t ts_pin_manager_load_nvs(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PIN_MANAGER_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    
    size_t size = 0;
    ret = nvs_get_blob(nvs, PIN_MANAGER_NVS_KEY, NULL, &size);
    if (ret != ESP_OK || size == 0) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }
    
    /* Read and apply mappings */
    uint8_t *data = TS_MALLOC_PSRAM(size);
    if (data == NULL) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }
    
    ret = nvs_get_blob(nvs, PIN_MANAGER_NVS_KEY, data, &size);
    nvs_close(nvs);
    
    if (ret == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        
        /* Format: [func_id(1), gpio_num(1)] pairs */
        int loaded = 0;
        for (size_t i = 0; i + 1 < size; i += 2) {
            uint8_t func = data[i];
            int8_t gpio = (int8_t)data[i + 1];
            
            if (func < TS_PIN_FUNC_MAX && gpio >= -1 && gpio < MAX_GPIO_NUM) {
                s_pin_map[func].gpio_num = gpio;
                if (gpio >= 0) {
                    s_gpio_map[gpio].function = (ts_pin_function_t)func;
                }
                loaded++;
            }
        }
        
        xSemaphoreGive(s_mutex);
        TS_LOGI(TAG, "Loaded %d pin mappings from NVS", loaded);
    }
    
    free(data);
    return ret;
}

esp_err_t ts_pin_manager_save_nvs(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PIN_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* Build mapping data */
    uint8_t data[TS_PIN_FUNC_MAX * 2];
    size_t data_len = 0;
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    for (int i = 0; i < TS_PIN_FUNC_MAX; i++) {
        if (s_pin_map[i].gpio_num >= 0) {
            data[data_len++] = (uint8_t)i;
            data[data_len++] = (uint8_t)s_pin_map[i].gpio_num;
        }
    }
    
    xSemaphoreGive(s_mutex);
    
    ret = nvs_set_blob(nvs, PIN_MANAGER_NVS_KEY, data, data_len);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    
    nvs_close(nvs);
    
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Saved %zu pin mappings to NVS", data_len / 2);
    }
    
    return ret;
}

esp_err_t ts_pin_manager_load_defaults(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    TS_LOGI(TAG, "Loading default pin mappings for %s",
#if CONFIG_IDF_TARGET_ESP32S3
            "ESP32S3"
#elif CONFIG_IDF_TARGET_ESP32P4
            "ESP32P4"
#else
            "Unknown"
#endif
    );
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
#if CONFIG_IDF_TARGET_ESP32S3
    /* Default mappings for rm01_esp32s3 board */
    /* Only set if not already mapped */
    
    #define SET_DEFAULT_PIN(func, gpio) do { \
        if (s_pin_map[func].gpio_num < 0) { \
            s_pin_map[func].gpio_num = gpio; \
            s_gpio_map[gpio].function = func; \
        } \
    } while(0)
    
    /* LED System */
    SET_DEFAULT_PIN(TS_PIN_FUNC_LED_TOUCH, 45);
    SET_DEFAULT_PIN(TS_PIN_FUNC_LED_BOARD, 42);
    SET_DEFAULT_PIN(TS_PIN_FUNC_LED_MATRIX, 9);
    
    /* Fan System - 只有一个风扇 */
    SET_DEFAULT_PIN(TS_PIN_FUNC_FAN_PWM_0, 41);  // GPIO41: 风扇 PWM (25kHz)
    
    /* Ethernet W5500 */
    SET_DEFAULT_PIN(TS_PIN_FUNC_ETH_MISO, 13);
    SET_DEFAULT_PIN(TS_PIN_FUNC_ETH_MOSI, 11);
    SET_DEFAULT_PIN(TS_PIN_FUNC_ETH_SCLK, 12);
    SET_DEFAULT_PIN(TS_PIN_FUNC_ETH_CS, 10);
    SET_DEFAULT_PIN(TS_PIN_FUNC_ETH_INT, 38);
    SET_DEFAULT_PIN(TS_PIN_FUNC_ETH_RST, 39);
    
    /* USB MUX */
    SET_DEFAULT_PIN(TS_PIN_FUNC_USB_MUX_1, 8);
    SET_DEFAULT_PIN(TS_PIN_FUNC_USB_MUX_2, 48);
    
    /* Device Control (与 robOS 一致) */
    SET_DEFAULT_PIN(TS_PIN_FUNC_AGX_POWER, 3);          // GPIO3: 强制关机 (LOW=force off, HIGH=normal)
    SET_DEFAULT_PIN(TS_PIN_FUNC_AGX_RESET, 1);          // GPIO1: 复位 (HIGH=reset, LOW=normal)
    SET_DEFAULT_PIN(TS_PIN_FUNC_AGX_FORCE_RECOVERY, 40);// GPIO40: 恢复模式 (HIGH=recovery)
    SET_DEFAULT_PIN(TS_PIN_FUNC_LPMU_POWER, 46);        // GPIO46: 电源按钮 (pulse HIGH)
    SET_DEFAULT_PIN(TS_PIN_FUNC_LPMU_RESET, 2);         // GPIO2: 复位 (pulse HIGH)
    
    /* RTL8367 Switch */
    SET_DEFAULT_PIN(TS_PIN_FUNC_RTL8367_RST, 17);    // GPIO17: RTL8367 复位 (HIGH=reset, LOW=normal)

    /* Power Monitoring (与 PCB 图纸一致) */
    SET_DEFAULT_PIN(TS_PIN_FUNC_POWER_ADC, 18);      // GPIO18: ADC2_CH7, 分压比 11.4:1, 最高 72V
    SET_DEFAULT_PIN(TS_PIN_FUNC_POWER_UART_RX, 47);  // GPIO47: UART1_RX, 9600 8N1, [0xFF][V][I][CRC]

    /* SD Card (SDMMC 4-bit 模式，与 robOS storage_device.c 一致) */
    SET_DEFAULT_PIN(TS_PIN_FUNC_SD_D0, 4);      // GPIO4: D0
    SET_DEFAULT_PIN(TS_PIN_FUNC_SD_D1, 5);      // GPIO5: D1
    SET_DEFAULT_PIN(TS_PIN_FUNC_SD_D2, 6);      // GPIO6: D2
    SET_DEFAULT_PIN(TS_PIN_FUNC_SD_D3, 7);      // GPIO7: D3
    SET_DEFAULT_PIN(TS_PIN_FUNC_SD_CMD, 15);    // GPIO15: CMD
    SET_DEFAULT_PIN(TS_PIN_FUNC_SD_CLK, 16);    // GPIO16: CLK (40MHz)
    
    /* 注意: POWER_ADC (GPIO18) 和 POWER_UART (GPIO47) 在原理图中未确认，暂不配置 */
    
    #undef SET_DEFAULT_PIN
#endif
    
    xSemaphoreGive(s_mutex);
    
    return ESP_OK;
}

esp_err_t ts_pin_manager_set_mapping(ts_pin_function_t function, int gpio_num)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (function >= TS_PIN_FUNC_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (gpio_num < -1 || gpio_num >= MAX_GPIO_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    esp_err_t ret = ESP_OK;
    
    /* Check if GPIO is already assigned to another function */
    if (gpio_num >= 0 && s_gpio_map[gpio_num].function != TS_PIN_FUNC_MAX &&
        s_gpio_map[gpio_num].function != function) {
        TS_LOGE(TAG, "GPIO%d already assigned to function %d", 
                gpio_num, s_gpio_map[gpio_num].function);
        ret = ESP_ERR_INVALID_STATE;
    } else {
        /* Clear old mapping if exists */
        int old_gpio = s_pin_map[function].gpio_num;
        if (old_gpio >= 0) {
            s_gpio_map[old_gpio].function = TS_PIN_FUNC_MAX;
        }
        
        /* Set new mapping */
        s_pin_map[function].gpio_num = gpio_num;
        if (gpio_num >= 0) {
            s_gpio_map[gpio_num].function = function;
        }
        
        TS_LOGI(TAG, "Mapped function %d -> GPIO%d", function, gpio_num);
    }
    
    xSemaphoreGive(s_mutex);
    return ret;
}

int ts_pin_manager_get_gpio(ts_pin_function_t function)
{
    if (!s_initialized || function >= TS_PIN_FUNC_MAX) {
        return -1;
    }
    
    return s_pin_map[function].gpio_num;
}

ts_pin_function_t ts_pin_manager_get_function(int gpio_num)
{
    if (!s_initialized || gpio_num < 0 || gpio_num >= MAX_GPIO_NUM) {
        return TS_PIN_FUNC_MAX;
    }
    
    return s_gpio_map[gpio_num].function;
}

esp_err_t ts_pin_manager_acquire(ts_pin_function_t function, const char *owner)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (function >= TS_PIN_FUNC_MAX || owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    esp_err_t ret = ESP_OK;
    
    if (s_pin_map[function].gpio_num < 0) {
        TS_LOGE(TAG, "Function %d has no GPIO mapping", function);
        ret = ESP_ERR_NOT_FOUND;
    } else if (s_pin_map[function].state != TS_PIN_STATE_FREE) {
        TS_LOGE(TAG, "Pin already acquired by: %s", s_pin_map[function].owner);
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_pin_map[function].state = TS_PIN_STATE_ALLOCATED;
        strncpy(s_pin_map[function].owner, owner, MAX_OWNER_NAME_LEN - 1);
        s_pin_map[function].owner[MAX_OWNER_NAME_LEN - 1] = '\0';
        
        TS_LOGD(TAG, "Pin GPIO%d acquired by %s", 
                s_pin_map[function].gpio_num, owner);
    }
    
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t ts_pin_manager_release(ts_pin_function_t function)
{
    if (!s_initialized || function >= TS_PIN_FUNC_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    if (s_pin_map[function].state == TS_PIN_STATE_FREE) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    TS_LOGD(TAG, "Pin GPIO%d released by %s",
            s_pin_map[function].gpio_num, s_pin_map[function].owner);
    
    s_pin_map[function].state = TS_PIN_STATE_FREE;
    s_pin_map[function].owner[0] = '\0';
    
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool ts_pin_manager_is_available(int gpio_num)
{
    if (!s_initialized || gpio_num < 0 || gpio_num >= MAX_GPIO_NUM) {
        return false;
    }
    
    ts_pin_function_t func = s_gpio_map[gpio_num].function;
    if (func == TS_PIN_FUNC_MAX) {
        return true;  /* Not assigned to any function */
    }
    
    return s_pin_map[func].state == TS_PIN_STATE_FREE;
}

bool ts_pin_manager_is_mapped(ts_pin_function_t function)
{
    if (!s_initialized || function >= TS_PIN_FUNC_MAX) {
        return false;
    }
    
    return s_pin_map[function].gpio_num >= 0;
}

uint16_t ts_pin_manager_get_capabilities(int gpio_num)
{
    if (!s_initialized || gpio_num < 0 || gpio_num >= MAX_GPIO_NUM) {
        return 0;
    }
    
    return s_gpio_map[gpio_num].capabilities;
}

const char *ts_pin_manager_get_name(ts_pin_function_t function)
{
    const pin_function_info_t *info = get_function_info(function);
    return info ? info->name : "UNKNOWN";
}

esp_err_t ts_pin_manager_get_mappings(ts_pin_mapping_t *mappings, size_t *count)
{
    if (!s_initialized || mappings == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    size_t max_count = *count;
    size_t actual_count = 0;
    
    for (int i = 0; i < TS_PIN_FUNC_MAX && actual_count < max_count; i++) {
        if (s_pin_map[i].gpio_num >= 0) {
            const pin_function_info_t *info = get_function_info((ts_pin_function_t)i);
            
            mappings[actual_count].function = (ts_pin_function_t)i;
            mappings[actual_count].gpio_num = s_pin_map[i].gpio_num;
            mappings[actual_count].name = info ? info->name : "UNKNOWN";
            mappings[actual_count].description = info ? info->description : "";
            actual_count++;
        }
    }
    
    *count = actual_count;
    
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t ts_pin_manager_get_status(ts_pin_status_t *status, size_t *count)
{
    if (!s_initialized || status == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    size_t max_count = *count;
    size_t actual_count = 0;
    
    for (int i = 0; i < MAX_GPIO_NUM && actual_count < max_count; i++) {
        status[actual_count].gpio_num = i;
        status[actual_count].capabilities = s_gpio_map[i].capabilities;
        status[actual_count].function = s_gpio_map[i].function;
        
        if (s_gpio_map[i].function != TS_PIN_FUNC_MAX) {
            ts_pin_function_t func = s_gpio_map[i].function;
            status[actual_count].state = s_pin_map[func].state;
            status[actual_count].owner = s_pin_map[func].owner;
        } else {
            status[actual_count].state = TS_PIN_STATE_FREE;
            status[actual_count].owner = "";
        }
        
        actual_count++;
    }
    
    *count = actual_count;
    
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void ts_pin_manager_print_config(void)
{
    if (!s_initialized) {
        printf("Pin manager not initialized\n");
        return;
    }
    
    printf("\n===== Pin Configuration =====\n");
    printf("%-20s %-6s %-12s %-20s\n", "Function", "GPIO", "State", "Owner");
    printf("------------------------------------------------------------\n");
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    for (int i = 0; i < TS_PIN_FUNC_MAX; i++) {
        if (s_pin_map[i].gpio_num >= 0) {
            const char *state_str;
            switch (s_pin_map[i].state) {
                case TS_PIN_STATE_FREE: state_str = "free"; break;
                case TS_PIN_STATE_ALLOCATED: state_str = "allocated"; break;
                case TS_PIN_STATE_CONFIGURED: state_str = "configured"; break;
                case TS_PIN_STATE_RESERVED: state_str = "reserved"; break;
                default: state_str = "unknown"; break;
            }
            
            printf("%-20s %-6d %-12s %-20s\n",
                   ts_pin_manager_get_name((ts_pin_function_t)i),
                   s_pin_map[i].gpio_num,
                   state_str,
                   s_pin_map[i].owner[0] ? s_pin_map[i].owner : "-");
        }
    }
    
    xSemaphoreGive(s_mutex);
    printf("=============================\n\n");
}

void ts_pin_manager_print_usage(void)
{
    if (!s_initialized) {
        printf("Pin manager not initialized\n");
        return;
    }
    
    int free_count = 0, allocated_count = 0, configured_count = 0;
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    for (int i = 0; i < TS_PIN_FUNC_MAX; i++) {
        if (s_pin_map[i].gpio_num >= 0) {
            switch (s_pin_map[i].state) {
                case TS_PIN_STATE_FREE: free_count++; break;
                case TS_PIN_STATE_ALLOCATED: allocated_count++; break;
                case TS_PIN_STATE_CONFIGURED: configured_count++; break;
                default: break;
            }
        }
    }
    
    xSemaphoreGive(s_mutex);
    
    printf("\n===== Pin Usage Summary =====\n");
    printf("Free:       %d\n", free_count);
    printf("Allocated:  %d\n", allocated_count);
    printf("Configured: %d\n", configured_count);
    printf("Total Used: %d\n", free_count + allocated_count + configured_count);
    printf("============================\n\n");
}
