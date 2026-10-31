/**
 * @file ts_device_ctrl.c
 * @brief Device Power Control (AGX/LPMU) Implementation
 * 
 * AGX 使用电平控制（保持 HIGH/LOW）
 * LPMU 使用脉冲控制（按按钮方式切换）
 */

#include "ts_device_ctrl.h"
#include "ts_hal_gpio.h"
#include "ts_log.h"
#include "ts_event.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "cJSON.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "lwip/inet_chksum.h"

#define TAG "ts_device"

// LPMU 默认 IP 地址
#define LPMU_DEFAULT_IP "10.10.99.99"

/*===========================================================================*/
/*                          AGX Instance                                      */
/*===========================================================================*/

typedef struct {
    ts_gpio_handle_t power_en;
    ts_gpio_handle_t reset;
    ts_gpio_handle_t force_recovery;
    ts_gpio_handle_t sys_rst;
    ts_gpio_handle_t power_good;
    ts_gpio_handle_t carrier_pwr_on;
    ts_gpio_handle_t shutdown_req;
    ts_gpio_handle_t sleep_wake;
} agx_gpio_handles_t;

typedef struct {
    bool configured;
    ts_agx_pins_t pins;
    agx_gpio_handles_t gpio;
    ts_device_state_t state;
    uint32_t power_on_time;
    uint32_t boot_count;
    int32_t last_error;
} agx_instance_t;

/*===========================================================================*/
/*                          LPMU Instance                                     */
/*===========================================================================*/

typedef struct {
    ts_gpio_handle_t power_btn;
    ts_gpio_handle_t reset;
} lpmu_gpio_handles_t;

typedef struct {
    bool configured;
    ts_lpmu_pins_t pins;
    lpmu_gpio_handles_t gpio;
    ts_device_state_t state;
    uint32_t power_on_time;
    uint32_t boot_count;
    int32_t last_error;
} lpmu_instance_t;

/*===========================================================================*/
/*                          Static Instances                                  */
/*===========================================================================*/

static agx_instance_t s_agx = {0};
static lpmu_instance_t s_lpmu = {0};
static bool s_initialized = false;

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief Create output GPIO with specified initial level
 * @param gpio_num GPIO number
 * @param name GPIO name for debugging
 * @param initial_level Initial output level (0=LOW, 1=HIGH)
 * @return GPIO handle or NULL on failure
 */
static ts_gpio_handle_t create_output_gpio_with_level(int gpio_num, const char *name, int initial_level)
{
    if (gpio_num < 0) return NULL;
    
    ts_gpio_handle_t handle = ts_gpio_create_raw(gpio_num, name);
    if (!handle) return NULL;
    
    ts_gpio_config_t cfg = {
        .direction = TS_GPIO_DIR_OUTPUT,
        .pull_mode = TS_GPIO_PULL_NONE,
        .intr_type = TS_GPIO_INTR_DISABLE,
        .drive = TS_GPIO_DRIVE_2,
        .invert = false,
        .initial_level = initial_level
    };
    ts_gpio_configure(handle, &cfg);
    return handle;
}

/**
 * @brief Create output GPIO with default LOW level
 * @note Use create_output_gpio_with_level() for pins requiring HIGH initial state
 */
static ts_gpio_handle_t create_output_gpio(int gpio_num, const char *name)
{
    return create_output_gpio_with_level(gpio_num, name, 0);
}

static ts_gpio_handle_t create_input_gpio(int gpio_num, const char *name, bool with_pullup)
{
    if (gpio_num < 0) return NULL;
    
    ts_gpio_handle_t handle = ts_gpio_create_raw(gpio_num, name);
    if (!handle) return NULL;
    
    ts_gpio_config_t cfg = {
        .direction = TS_GPIO_DIR_INPUT,
        .pull_mode = with_pullup ? TS_GPIO_PULL_UP : TS_GPIO_PULL_NONE,
        .intr_type = TS_GPIO_INTR_DISABLE,
        .drive = TS_GPIO_DRIVE_2,
        .invert = false,
        .initial_level = -1
    };
    ts_gpio_configure(handle, &cfg);
    return handle;
}

static void shutdown_req_callback(ts_gpio_handle_t handle, void *arg)
{
    (void)handle;
    (void)arg;
    // AGX 发送了 shutdown 请求，可以发布事件
    // ts_event_post(TS_EVENT_BASE_DEVICE, TS_EVENT_DEVICE_SHUTDOWN_REQ, ...);
}

/*===========================================================================*/
/*                          AGX Control Functions                             */
/*===========================================================================*/

/**
 * AGX 电源控制说明（基于硬件设计）：
 * 
 * GPIO 1 (AGX_RESET / gpio_reset) - 主电源控制引脚：
 *   - 持续 LOW  = 通电（开机状态）
 *   - 持续 HIGH = 断电（关机状态）
 *   - LOW → HIGH → LOW 脉冲 = 重启
 * 
 * GPIO 3 (AGX_FORCE_SHUTDOWN / gpio_power_en) - 强制关机引脚：
 *   - HIGH = 强制关机，除非物理断电否则无法恢复开机
 *   - **禁止在正常操作中使用！**
 * 
 * 开机流程：GPIO 1 设置为 LOW（持续）
 * 关机流程：GPIO 1 设置为 HIGH（持续）
 * 重启流程：GPIO 1 脉冲 LOW → HIGH → LOW
 * 
 * 注意：
 * 1. 避免操作 GPIO 后立即读取状态，可能导致状态变化（robOS 经验）
 * 2. GPIO 3 (FORCE_SHUTDOWN) 不应在正常电源控制中使用
 */

static esp_err_t agx_power_on(void)
{
    TS_LOGI(TAG, "AGX powering on (reset=GPIO%d)...", s_agx.pins.gpio_reset);
    
    if (s_agx.state == TS_DEVICE_STATE_ON) {
        TS_LOGI(TAG, "AGX already ON");
        return ESP_OK;
    }
    
    s_agx.state = TS_DEVICE_STATE_BOOTING;
    
    int pin_reset = s_agx.pins.gpio_reset;  // GPIO 1 = 电源控制引脚
    
    if (pin_reset < 0) {
        TS_LOGE(TAG, "Invalid RESET pin: %d", pin_reset);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 设置 RESET = LOW（通电/开机）
    // 注意：不要调用 gpio_reset_pin，会导致电平毛刺
    gpio_set_level(pin_reset, 0);  // LOW = 通电（持续）
    TS_LOGI(TAG, "RESET set to LOW (GPIO%d), AGX powering on", pin_reset);
    
    // Wait for power stabilization
    vTaskDelay(pdMS_TO_TICKS(TS_AGX_POWER_ON_DELAY_MS));
    
    s_agx.power_on_time = esp_timer_get_time() / 1000;
    s_agx.boot_count++;
    s_agx.state = TS_DEVICE_STATE_ON;
    
    TS_LOGI(TAG, "AGX powered on (boot #%lu)", s_agx.boot_count);
    
    // 发送设备状态变更事件 (WebSocket)
    cJSON *status = cJSON_CreateObject();
    if (status) {
        cJSON_AddStringToObject(status, "device", "agx");
        cJSON_AddBoolToObject(status, "power", true);
        cJSON_AddStringToObject(status, "state", "on");
        cJSON_AddNumberToObject(status, "boot_count", s_agx.boot_count);
        char *json_str = cJSON_PrintUnformatted(status);
        if (json_str) {
            ts_event_post(TS_EVENT_BASE_DEVICE_MON, TS_EVENT_DEVICE_STATUS_CHANGED, 
                         json_str, strlen(json_str) + 1, 0);
            cJSON_free(json_str);
        }
        cJSON_Delete(status);
    }
    
    return ESP_OK;
}

static esp_err_t agx_power_off(void)
{
    TS_LOGI(TAG, "AGX powering off (reset=GPIO%d)...", s_agx.pins.gpio_reset);
    
    int pin_reset = s_agx.pins.gpio_reset;  // GPIO 1 = 电源控制引脚
    
    if (pin_reset < 0) {
        TS_LOGE(TAG, "Invalid RESET pin: %d", pin_reset);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 设置 RESET = HIGH（断电/关机）
    // 注意：不要调用 gpio_reset_pin，会导致电平毛刺
    gpio_set_level(pin_reset, 1);  // HIGH = 断电（持续）
    TS_LOGI(TAG, "RESET set to HIGH (GPIO%d), AGX powered off", pin_reset);
    
    s_agx.state = TS_DEVICE_STATE_OFF;
    TS_LOGI(TAG, "AGX powered off");
    
    // 发送设备状态变更事件 (WebSocket)
    cJSON *status = cJSON_CreateObject();
    if (status) {
        cJSON_AddStringToObject(status, "device", "agx");
        cJSON_AddBoolToObject(status, "power", false);
        cJSON_AddStringToObject(status, "state", "off");
        char *json_str = cJSON_PrintUnformatted(status);
        if (json_str) {
            ts_event_post(TS_EVENT_BASE_DEVICE_MON, TS_EVENT_DEVICE_STATUS_CHANGED, 
                         json_str, strlen(json_str) + 1, 0);
            cJSON_free(json_str);
        }
        cJSON_Delete(status);
    }
    
    return ESP_OK;
}

static esp_err_t agx_reset(void)
{
    TS_LOGI(TAG, "AGX resetting (reset=GPIO%d)...", s_agx.pins.gpio_reset);
    
    int pin_reset = s_agx.pins.gpio_reset;  // GPIO 1 = 电源控制引脚
    
    if (pin_reset < 0) {
        TS_LOGE(TAG, "Invalid RESET pin");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 重启脉冲：LOW → HIGH → LOW（断电后重新通电）
    // 注意：不要调用 gpio_reset_pin()，避免电平毛刺（robOS 经验）
    TS_LOGI(TAG, "Sending RESET pulse (GPIO%d: LOW->HIGH->LOW, HIGH for %dms)...", 
            pin_reset, TS_AGX_RESET_PULSE_MS);
    gpio_set_level(pin_reset, 0);  // LOW = 确保通电状态
    vTaskDelay(pdMS_TO_TICKS(50)); // 短暂延时
    gpio_set_level(pin_reset, 1);  // HIGH = 断电
    vTaskDelay(pdMS_TO_TICKS(TS_AGX_RESET_PULSE_MS));
    gpio_set_level(pin_reset, 0);  // LOW = 重新通电
    TS_LOGI(TAG, "RESET pulse complete, GPIO%d back to LOW (powered on)", pin_reset);
    
    s_agx.boot_count++;
    s_agx.state = TS_DEVICE_STATE_BOOTING;
    
    TS_LOGI(TAG, "AGX reset complete (boot #%lu)", s_agx.boot_count);
    
    // 发送设备状态变更事件 (WebSocket)
    cJSON *status = cJSON_CreateObject();
    if (status) {
        cJSON_AddStringToObject(status, "device", "agx");
        cJSON_AddBoolToObject(status, "power", true);
        cJSON_AddStringToObject(status, "state", "resetting");
        cJSON_AddNumberToObject(status, "boot_count", s_agx.boot_count);
        char *json_str = cJSON_PrintUnformatted(status);
        if (json_str) {
            ts_event_post(TS_EVENT_BASE_DEVICE_MON, TS_EVENT_DEVICE_STATUS_CHANGED, 
                         json_str, strlen(json_str) + 1, 0);
            cJSON_free(json_str);
        }
        cJSON_Delete(status);
    }
    
    return ESP_OK;
}

static esp_err_t agx_force_off(void)
{
    TS_LOGW(TAG, "AGX force power off...");
    
    // Pulse force_off signal (like holding power button)
    if (s_agx.gpio.force_recovery) {  // Use force_recovery pin as force_off
        ts_gpio_set_level(s_agx.gpio.force_recovery, 1);
        vTaskDelay(pdMS_TO_TICKS(TS_AGX_POWER_PULSE_MS));
        ts_gpio_set_level(s_agx.gpio.force_recovery, 0);
    }
    
    return agx_power_off();
}

static esp_err_t agx_enter_recovery(void)
{
    TS_LOGI(TAG, "AGX entering recovery mode...");
    
    // robOS recovery sequence:
    // 1. Assert force recovery HIGH, hold 1000ms
    if (s_agx.gpio.force_recovery) {
        ts_gpio_set_level(s_agx.gpio.force_recovery, 1);
        vTaskDelay(pdMS_TO_TICKS(TS_AGX_RECOVERY_DELAY_MS));
    }
    
    // 2. Execute reset pulse
    if (s_agx.gpio.reset) {
        ts_gpio_set_level(s_agx.gpio.reset, 1);
        vTaskDelay(pdMS_TO_TICKS(TS_AGX_RESET_PULSE_MS));
        ts_gpio_set_level(s_agx.gpio.reset, 0);
        vTaskDelay(pdMS_TO_TICKS(TS_AGX_RECOVERY_DELAY_MS));
    }
    
    // 3. Release force recovery
    if (s_agx.gpio.force_recovery) {
        ts_gpio_set_level(s_agx.gpio.force_recovery, 0);
    }
    
    s_agx.state = TS_DEVICE_STATE_RECOVERY;
    s_agx.boot_count++;
    
    TS_LOGI(TAG, "AGX in recovery mode");
    return ESP_OK;
}

/*===========================================================================*/
/*                          LPMU Control Functions                            */
/*===========================================================================*/

static esp_err_t lpmu_power_toggle(void)
{
    TS_LOGI(TAG, "LPMU power toggle (pulse)");
    
    // LPMU uses pulse to toggle power (like pressing a button)
    if (s_lpmu.gpio.power_btn) {
        ts_gpio_set_level(s_lpmu.gpio.power_btn, 1);
        vTaskDelay(pdMS_TO_TICKS(TS_LPMU_POWER_PULSE_MS));
        ts_gpio_set_level(s_lpmu.gpio.power_btn, 0);
    }
    
    return ESP_OK;
}

static esp_err_t lpmu_power_on(void)
{
    if (s_lpmu.state == TS_DEVICE_STATE_ON) {
        return ESP_OK;
    }
    
    TS_LOGI(TAG, "LPMU powering on...");
    s_lpmu.state = TS_DEVICE_STATE_BOOTING;
    
    lpmu_power_toggle();
    
    s_lpmu.power_on_time = esp_timer_get_time() / 1000;
    s_lpmu.boot_count++;
    s_lpmu.state = TS_DEVICE_STATE_ON;
    
    TS_LOGI(TAG, "LPMU powered on (boot #%lu)", s_lpmu.boot_count);
    
    // 发送设备状态变更事件 (WebSocket)
    cJSON *status = cJSON_CreateObject();
    if (status) {
        cJSON_AddStringToObject(status, "device", "lpmu");
        cJSON_AddBoolToObject(status, "power", true);
        cJSON_AddStringToObject(status, "state", "on");
        cJSON_AddNumberToObject(status, "boot_count", s_lpmu.boot_count);
        char *json_str = cJSON_PrintUnformatted(status);
        if (json_str) {
            ts_event_post(TS_EVENT_BASE_DEVICE_MON, TS_EVENT_DEVICE_STATUS_CHANGED, 
                         json_str, strlen(json_str) + 1, 0);
            cJSON_free(json_str);
        }
        cJSON_Delete(status);
    }
    
    return ESP_OK;
}

static esp_err_t lpmu_power_off(void)
{
    if (s_lpmu.state == TS_DEVICE_STATE_OFF) {
        return ESP_OK;
    }
    
    TS_LOGI(TAG, "LPMU powering off...");
    
    lpmu_power_toggle();
    
    s_lpmu.state = TS_DEVICE_STATE_OFF;
    TS_LOGI(TAG, "LPMU powered off");
    
    // 发送设备状态变更事件 (WebSocket)
    cJSON *status = cJSON_CreateObject();
    if (status) {
        cJSON_AddStringToObject(status, "device", "lpmu");
        cJSON_AddBoolToObject(status, "power", false);
        cJSON_AddStringToObject(status, "state", "off");
        char *json_str = cJSON_PrintUnformatted(status);
        if (json_str) {
            ts_event_post(TS_EVENT_BASE_DEVICE_MON, TS_EVENT_DEVICE_STATUS_CHANGED, 
                         json_str, strlen(json_str) + 1, 0);
            cJSON_free(json_str);
        }
        cJSON_Delete(status);
    }
    
    return ESP_OK;
}

static esp_err_t lpmu_reset(void)
{
    TS_LOGI(TAG, "LPMU resetting...");
    
    if (s_lpmu.gpio.reset) {
        // Pulse reset HIGH
        ts_gpio_set_level(s_lpmu.gpio.reset, 1);
        vTaskDelay(pdMS_TO_TICKS(TS_LPMU_RESET_PULSE_MS));
        ts_gpio_set_level(s_lpmu.gpio.reset, 0);
    }
    
    s_lpmu.boot_count++;
    s_lpmu.state = TS_DEVICE_STATE_BOOTING;
    
    TS_LOGI(TAG, "LPMU reset complete (boot #%lu)", s_lpmu.boot_count);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Public API                                        */
/*===========================================================================*/

esp_err_t ts_device_ctrl_init(void)
{
    if (s_initialized) return ESP_OK;
    
    memset(&s_agx, 0, sizeof(s_agx));
    memset(&s_lpmu, 0, sizeof(s_lpmu));
    
    // Initialize AGX pins to -1
    s_agx.pins = (ts_agx_pins_t){
        .gpio_power_en = -1,
        .gpio_reset = -1,
        .gpio_force_recovery = -1,
        .gpio_sys_rst = -1,
        .gpio_power_good = -1,
        .gpio_carrier_pwr_on = -1,
        .gpio_shutdown_req = -1,
        .gpio_sleep_wake = -1
    };
    
    // Initialize LPMU pins to -1
    s_lpmu.pins = (ts_lpmu_pins_t){
        .gpio_power_btn = -1,
        .gpio_reset = -1
    };
    
    s_initialized = true;
    TS_LOGI(TAG, "Device control initialized");
    return ESP_OK;
}

esp_err_t ts_device_ctrl_deinit(void)
{
    // Cleanup AGX GPIO
    if (s_agx.gpio.power_en) ts_gpio_destroy(s_agx.gpio.power_en);
    if (s_agx.gpio.reset) ts_gpio_destroy(s_agx.gpio.reset);
    if (s_agx.gpio.force_recovery) ts_gpio_destroy(s_agx.gpio.force_recovery);
    if (s_agx.gpio.sys_rst) ts_gpio_destroy(s_agx.gpio.sys_rst);
    if (s_agx.gpio.power_good) ts_gpio_destroy(s_agx.gpio.power_good);
    if (s_agx.gpio.carrier_pwr_on) ts_gpio_destroy(s_agx.gpio.carrier_pwr_on);
    if (s_agx.gpio.shutdown_req) ts_gpio_destroy(s_agx.gpio.shutdown_req);
    if (s_agx.gpio.sleep_wake) ts_gpio_destroy(s_agx.gpio.sleep_wake);
    
    // Cleanup LPMU GPIO
    if (s_lpmu.gpio.power_btn) ts_gpio_destroy(s_lpmu.gpio.power_btn);
    if (s_lpmu.gpio.reset) ts_gpio_destroy(s_lpmu.gpio.reset);
    
    memset(&s_agx, 0, sizeof(s_agx));
    memset(&s_lpmu, 0, sizeof(s_lpmu));
    
    s_initialized = false;
    return ESP_OK;
}

esp_err_t ts_device_configure_agx(const ts_agx_pins_t *pins)
{
    if (!pins) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    s_agx.pins = *pins;
    
    /*
     * AGX GPIO 初始状态：
     * 
     * AGX 是上电自启动设计！ESP32 不应该阻止它启动。
     * 
     * - FORCE_SHUTDOWN (gpio_power_en): LOW=允许开机, HIGH=强制关机
     *   → 初始化为 LOW，让 AGX 可以正常上电启动
     * 
     * - RESET: LOW=正常运行（必须保持）, HIGH=断电
     *   → 初始化为 LOW，启用电源控制
     * 
     * - FORCE_RECOVERY: HIGH=恢复模式, LOW=正常模式
     *   → 初始化为 LOW
     */
    
    // FORCE_SHUTDOWN: 初始化为 LOW（允许 AGX 上电自启动）
    s_agx.gpio.power_en = create_output_gpio_with_level(pins->gpio_power_en, "agx_pwr", 0);
    
    // RESET: 初始化为 LOW（正常运行，电源控制启用）
    s_agx.gpio.reset = create_output_gpio_with_level(pins->gpio_reset, "agx_rst", 0);
    
    // FORCE_RECOVERY: 初始化为 LOW（正常模式）
    s_agx.gpio.force_recovery = create_output_gpio_with_level(pins->gpio_force_recovery, "agx_rcv", 0);
    
    // 其他输出引脚（如果配置）
    s_agx.gpio.carrier_pwr_on = create_output_gpio(pins->gpio_carrier_pwr_on, "agx_carrier");
    s_agx.gpio.sleep_wake = create_output_gpio(pins->gpio_sleep_wake, "agx_sw");
    
    // Configure input pins
    s_agx.gpio.power_good = create_input_gpio(pins->gpio_power_good, "agx_pg", true);
    s_agx.gpio.sys_rst = create_input_gpio(pins->gpio_sys_rst, "agx_rst_in", true);
    
    // Shutdown request with interrupt
    if (pins->gpio_shutdown_req >= 0) {
        s_agx.gpio.shutdown_req = ts_gpio_create_raw(pins->gpio_shutdown_req, "agx_shutdown");
        if (s_agx.gpio.shutdown_req) {
            ts_gpio_config_t cfg = {
                .direction = TS_GPIO_DIR_INPUT,
                .pull_mode = TS_GPIO_PULL_UP,
                .intr_type = TS_GPIO_INTR_NEGEDGE,
                .drive = TS_GPIO_DRIVE_2,
                .invert = false,
                .initial_level = -1
            };
            ts_gpio_configure(s_agx.gpio.shutdown_req, &cfg);
            ts_gpio_set_isr_callback(s_agx.gpio.shutdown_req, shutdown_req_callback, NULL);
            ts_gpio_intr_enable(s_agx.gpio.shutdown_req);
        }
    }
    
    s_agx.configured = true;
    s_agx.state = TS_DEVICE_STATE_ON;  // 假设 AGX 已经在运行（上电自启动）
    
    TS_LOGI(TAG, "AGX configured: FORCE_SHUTDOWN=GPIO%d(LOW=run), RESET=GPIO%d(LOW=normal)",
            pins->gpio_power_en, pins->gpio_reset);
    return ESP_OK;
}

esp_err_t ts_device_configure_lpmu(const ts_lpmu_pins_t *pins)
{
    if (!pins) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    s_lpmu.pins = *pins;
    
    /*
     * LPMU GPIO 初始状态：
     * - LPMU_POWER_BTN: 脉冲控制（HIGH 300ms 切换电源）→ 初始化为 LOW
     * - LPMU_RESET: 脉冲控制（HIGH 300ms 复位）→ 初始化为 LOW
     */
    s_lpmu.gpio.power_btn = create_output_gpio_with_level(pins->gpio_power_btn, "lpmu_pwr", 0);
    s_lpmu.gpio.reset = create_output_gpio_with_level(pins->gpio_reset, "lpmu_rst", 0);
    
    s_lpmu.configured = true;
    s_lpmu.state = TS_DEVICE_STATE_OFF;
    
    TS_LOGI(TAG, "LPMU configured (power=%d[pulse HIGH], reset=%d[pulse HIGH])",
            pins->gpio_power_btn, pins->gpio_reset);
    return ESP_OK;
}

esp_err_t ts_device_power_on(ts_device_id_t device)
{
    switch (device) {
        case TS_DEVICE_AGX:
            if (!s_agx.configured) return ESP_ERR_INVALID_STATE;
            return agx_power_on();
        case TS_DEVICE_LPMU:
            if (!s_lpmu.configured) return ESP_ERR_INVALID_STATE;
            return lpmu_power_on();
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t ts_device_power_off(ts_device_id_t device)
{
    switch (device) {
        case TS_DEVICE_AGX:
            if (!s_agx.configured) return ESP_ERR_INVALID_STATE;
            return agx_power_off();
        case TS_DEVICE_LPMU:
            if (!s_lpmu.configured) return ESP_ERR_INVALID_STATE;
            return lpmu_power_off();
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t ts_device_force_off(ts_device_id_t device)
{
    switch (device) {
        case TS_DEVICE_AGX:
            if (!s_agx.configured) return ESP_ERR_INVALID_STATE;
            return agx_force_off();
        case TS_DEVICE_LPMU:
            // LPMU 没有 force_off，直接 power_off
            if (!s_lpmu.configured) return ESP_ERR_INVALID_STATE;
            return lpmu_power_off();
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t ts_device_power_toggle(ts_device_id_t device)
{
    switch (device) {
        case TS_DEVICE_AGX:
            // AGX 没有 toggle，返回不支持
            TS_LOGW(TAG, "AGX does not support power toggle");
            return ESP_ERR_NOT_SUPPORTED;
        case TS_DEVICE_LPMU:
            if (!s_lpmu.configured) return ESP_ERR_INVALID_STATE;
            TS_LOGI(TAG, "LPMU power toggle (direct pulse)");
            return lpmu_power_toggle();
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t ts_device_reset(ts_device_id_t device)
{
    switch (device) {
        case TS_DEVICE_AGX:
            if (!s_agx.configured) return ESP_ERR_INVALID_STATE;
            return agx_reset();
        case TS_DEVICE_LPMU:
            if (!s_lpmu.configured) return ESP_ERR_INVALID_STATE;
            return lpmu_reset();
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t ts_device_enter_recovery(ts_device_id_t device)
{
    if (device != TS_DEVICE_AGX) {
        TS_LOGW(TAG, "Recovery mode only supported for AGX");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    if (!s_agx.configured) return ESP_ERR_INVALID_STATE;
    return agx_enter_recovery();
}

esp_err_t ts_device_get_status(ts_device_id_t device, ts_device_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    
    switch (device) {
        case TS_DEVICE_AGX:
            if (!s_agx.configured) return ESP_ERR_INVALID_STATE;
            status->state = s_agx.state;
            status->boot_count = s_agx.boot_count;
            status->last_error = s_agx.last_error;
            if (s_agx.gpio.power_good) {
                status->power_good = ts_gpio_get_level(s_agx.gpio.power_good) == 1;
            } else {
                status->power_good = s_agx.state == TS_DEVICE_STATE_ON;
            }
            if (s_agx.state == TS_DEVICE_STATE_ON && s_agx.power_on_time > 0) {
                status->uptime_ms = (esp_timer_get_time() / 1000) - s_agx.power_on_time;
            } else {
                status->uptime_ms = 0;
            }
            break;
            
        case TS_DEVICE_LPMU:
            if (!s_lpmu.configured) return ESP_ERR_INVALID_STATE;
            status->state = s_lpmu.state;
            status->boot_count = s_lpmu.boot_count;
            status->last_error = s_lpmu.last_error;
            status->power_good = s_lpmu.state == TS_DEVICE_STATE_ON;
            if (s_lpmu.state == TS_DEVICE_STATE_ON && s_lpmu.power_on_time > 0) {
                status->uptime_ms = (esp_timer_get_time() / 1000) - s_lpmu.power_on_time;
            } else {
                status->uptime_ms = 0;
            }
            break;
            
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

bool ts_device_is_powered(ts_device_id_t device)
{
    switch (device) {
        case TS_DEVICE_AGX:
            return s_agx.state == TS_DEVICE_STATE_ON || s_agx.state == TS_DEVICE_STATE_RECOVERY;
        case TS_DEVICE_LPMU:
            return s_lpmu.state == TS_DEVICE_STATE_ON;
        default:
            return false;
    }
}

bool ts_device_is_configured(ts_device_id_t device)
{
    switch (device) {
        case TS_DEVICE_AGX:
            return s_agx.configured;
        case TS_DEVICE_LPMU:
            return s_lpmu.configured;
        default:
            return false;
    }
}

esp_err_t ts_device_request_shutdown(ts_device_id_t device)
{
    if (device != TS_DEVICE_AGX) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    if (!s_agx.configured) return ESP_ERR_INVALID_STATE;
    
    TS_LOGI(TAG, "Requesting AGX shutdown...");
    
    if (s_agx.gpio.sleep_wake) {
        // Pulse sleep/wake to signal shutdown request
        ts_gpio_set_level(s_agx.gpio.sleep_wake, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        ts_gpio_set_level(s_agx.gpio.sleep_wake, 0);
    }
    
    return ESP_OK;
}

esp_err_t ts_device_handle_shutdown_request(ts_device_id_t device)
{
    TS_LOGI(TAG, "Handling shutdown request from device %d", device);
    return ts_device_power_off(device);
}

const char *ts_device_state_to_str(ts_device_state_t state)
{
    switch (state) {
        case TS_DEVICE_STATE_OFF:      return "off";
        case TS_DEVICE_STATE_STANDBY:  return "standby";
        case TS_DEVICE_STATE_ON:       return "on";
        case TS_DEVICE_STATE_BOOTING:  return "booting";
        case TS_DEVICE_STATE_RECOVERY: return "recovery";
        case TS_DEVICE_STATE_ERROR:    return "error";
        default:                       return "unknown";
    }
}

/*===========================================================================*/
/*                          LPMU Network Detection                            */
/*===========================================================================*/

/**
 * @brief Ping an IP address using ICMP
 * @param ip IP address string
 * @param timeout_ms Timeout in milliseconds
 * @return true if reachable, false otherwise
 */
static bool ping_host(const char *ip, int timeout_ms)
{
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        TS_LOGW(TAG, "Failed to create ICMP socket");
        return false;
    }
    
    // 设置接收超时
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &addr.sin_addr);
    
    // 构建 ICMP echo request
    struct {
        uint8_t type;
        uint8_t code;
        uint16_t checksum;
        uint16_t id;
        uint16_t seq;
        uint8_t data[32];
    } icmp_pkt;
    
    memset(&icmp_pkt, 0, sizeof(icmp_pkt));
    icmp_pkt.type = 8;  // ICMP_ECHO
    icmp_pkt.code = 0;
    icmp_pkt.id = htons(0x1234);
    icmp_pkt.seq = htons(1);
    memset(icmp_pkt.data, 0xAB, sizeof(icmp_pkt.data));
    
    // 计算校验和
    icmp_pkt.checksum = 0;
    icmp_pkt.checksum = inet_chksum(&icmp_pkt, sizeof(icmp_pkt));
    
    // 发送
    int sent = sendto(sock, &icmp_pkt, sizeof(icmp_pkt), 0, 
                      (struct sockaddr *)&addr, sizeof(addr));
    
    bool reachable = false;
    if (sent > 0) {
        char recv_buf[64];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        
        int recv_len = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                (struct sockaddr *)&from, &from_len);
        
        if (recv_len >= 28) {
            uint8_t icmp_type = recv_buf[20];
            if (icmp_type == 0) {  // ICMP_ECHOREPLY
                reachable = true;
            }
        }
    }
    
    close(sock);
    return reachable;
}

// LPMU 启动检测任务句柄
static TaskHandle_t s_lpmu_detect_task = NULL;

/**
 * @brief LPMU 启动状态检测任务
 * 在网络就绪后检测 LPMU 是否已经在运行
 */
static void lpmu_startup_detect_task(void *arg)
{
    TS_LOGI(TAG, "LPMU startup detection: waiting for network...");
    
    // 等待一段时间让网络稳定（网络服务启动后）
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    TS_LOGI(TAG, "LPMU startup detection: checking if LPMU is online...");
    
    // 尝试 ping LPMU 3 次
    bool is_online = false;
    for (int i = 0; i < 3; i++) {
        if (ping_host(LPMU_DEFAULT_IP, 1000)) {
            is_online = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    if (is_online) {
        // LPMU 已经在运行
        TS_LOGI(TAG, "LPMU detected online at %s, setting state to ON", LPMU_DEFAULT_IP);
        s_lpmu.state = TS_DEVICE_STATE_ON;
        s_lpmu.power_on_time = esp_timer_get_time() / 1000;
    } else {
        // LPMU 不在线，尝试开机
        TS_LOGI(TAG, "LPMU not detected, attempting power on...");
        s_lpmu.state = TS_DEVICE_STATE_BOOTING;
        
        // 发送开机脉冲
        if (s_lpmu.gpio.power_btn) {
            ts_gpio_set_level(s_lpmu.gpio.power_btn, 1);
            vTaskDelay(pdMS_TO_TICKS(TS_LPMU_POWER_PULSE_MS));
            ts_gpio_set_level(s_lpmu.gpio.power_btn, 0);
        }
        
        // 等待 LPMU 启动（最多 80 秒）
        TS_LOGI(TAG, "LPMU power pulse sent, waiting for boot (max 80s)...");
        bool booted = false;
        for (int i = 0; i < 16; i++) {  // 16 * 5s = 80s
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (ping_host(LPMU_DEFAULT_IP, 1000)) {
                booted = true;
                break;
            }
            TS_LOGI(TAG, "LPMU boot wait: %d/80 seconds...", (i + 1) * 5);
        }
        
        if (booted) {
            TS_LOGI(TAG, "LPMU boot successful, state set to ON");
            s_lpmu.state = TS_DEVICE_STATE_ON;
            s_lpmu.power_on_time = esp_timer_get_time() / 1000;
            s_lpmu.boot_count++;
        } else {
            TS_LOGW(TAG, "LPMU boot timeout, state remains OFF");
            s_lpmu.state = TS_DEVICE_STATE_OFF;
        }
    }
    
    // 发送状态事件
    cJSON *status = cJSON_CreateObject();
    if (status) {
        cJSON_AddStringToObject(status, "device", "lpmu");
        cJSON_AddBoolToObject(status, "power", s_lpmu.state == TS_DEVICE_STATE_ON);
        cJSON_AddStringToObject(status, "state", ts_device_state_to_str(s_lpmu.state));
        char *json_str = cJSON_PrintUnformatted(status);
        if (json_str) {
            ts_event_post(TS_EVENT_BASE_DEVICE_MON, TS_EVENT_DEVICE_STATUS_CHANGED, 
                         json_str, strlen(json_str) + 1, 0);
            cJSON_free(json_str);
        }
        cJSON_Delete(status);
    }
    
    s_lpmu_detect_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t ts_device_lpmu_start_detection(void)
{
    if (!s_lpmu.configured) {
        TS_LOGW(TAG, "LPMU not configured, skip detection");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_lpmu_detect_task != NULL) {
        TS_LOGW(TAG, "LPMU detection already running");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 使用 PSRAM 栈以减少 DRAM 压力（纯网络 ping 操作，不涉及 NVS/Flash）*/
    BaseType_t ret = xTaskCreateWithCaps(lpmu_startup_detect_task, "lpmu_detect", 4096, NULL, 5, &s_lpmu_detect_task,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        TS_LOGE(TAG, "Failed to create LPMU detection task");
        return ESP_FAIL;
    }
    
    TS_LOGI(TAG, "LPMU startup detection task started");
    return ESP_OK;
}
