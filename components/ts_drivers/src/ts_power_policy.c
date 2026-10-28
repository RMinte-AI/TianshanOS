/**
 * @file ts_power_policy.c
 * @brief Power Policy Engine Implementation
 * 
 * 移植自 robOS voltage_protection 组件
 * 
 * 状态机流程：
 * 1. NORMAL: 正常运行，持续监控电压
 * 2. LOW_VOLTAGE: 电压低于阈值，开始倒计时
 *    - 电压恢复 → 回到 NORMAL
 *    - 倒计时归零 → 进入 SHUTDOWN
 * 3. SHUTDOWN: 执行设备关机（AGX reset, LPMU toggle）
 * 4. PROTECTED: 等待电压恢复
 *    - 电压恢复 → 进入 RECOVERY
 * 5. RECOVERY: 电压稳定等待
 *    - 稳定后 → esp_restart() 重启系统
 */

#include "ts_power_policy.h"
#include "ts_power_monitor.h"
#include "ts_device_ctrl.h"
#include "ts_fan.h"
#include "ts_variable.h"
#include "ts_event.h"
#include "nvs.h"
#include "ts_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define TAG "ts_power_policy"

/* System NVS owner; "config" is one-time JSON staged by migration. */
#define CONFIG_NAMESPACE "ts_power"

/* LPMU IP for ping check */
#define LPMU_IP "10.10.99.99"

/*===========================================================================*/
/*                          Internal State                                    */
/*===========================================================================*/

typedef struct {
    bool initialized;
    bool running;
    bool enabled_on_boot;   /* 保存的启用状态，用于重启后恢复 */
    ts_power_policy_config_t config;
    ts_power_policy_state_t state;
    
    /* Voltage tracking */
    float current_voltage;
    float last_voltage;
    
    /* Timers */
    uint32_t countdown_remaining_sec;
    uint32_t recovery_timer_sec;
    uint32_t shutdown_timer_sec;
    
    /* Device status */
    bool agx_powered;
    bool lpmu_powered;
    bool agx_connected;
    bool fans_stopped;
    
    /* Statistics */
    uint32_t protection_count;
    uint64_t start_time_us;
    
    /* Task and sync */
    TaskHandle_t monitor_task_handle;
    SemaphoreHandle_t state_mutex;
    
    /* Test mode */
    bool test_mode;
    float test_voltage;
    
    /* Debug mode */
    bool debug_mode;
    uint32_t debug_remaining_sec;
    
    /* Callback */
    ts_power_policy_callback_t callback;
    void *callback_user_data;
    
} power_policy_state_t;

static power_policy_state_t s_pp = {0};

static esp_err_t load_config_values(const char *key)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (ret != ESP_OK) return ret;
    size_t length = 0;
    ret = nvs_get_blob(handle, key, NULL, &length);
    char *buf = ret == ESP_OK ? malloc(length + 1) : NULL;
    if (ret == ESP_OK && !buf) ret = ESP_ERR_NO_MEM;
    if (ret == ESP_OK) ret = nvs_get_blob(handle, key, buf, &length);
    nvs_close(handle);
    if (ret != ESP_OK) { free(buf); return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret; }
    buf[length] = 0;

    /* 解析 JSON */
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        TS_LOGE(TAG, "Failed to parse power policy JSON");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 提取配置值 */
    cJSON *item;
    bool loaded_any = false;
    
    item = cJSON_GetObjectItem(root, "low_voltage_threshold");
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        s_pp.config.low_voltage_threshold = (float)item->valuedouble;
        TS_LOGI(TAG, "NVS: low_voltage_threshold = %.2fV", s_pp.config.low_voltage_threshold);
        loaded_any = true;
    }
    
    item = cJSON_GetObjectItem(root, "recovery_voltage_threshold");
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        s_pp.config.recovery_voltage_threshold = (float)item->valuedouble;
        TS_LOGI(TAG, "NVS: recovery_voltage_threshold = %.2fV", s_pp.config.recovery_voltage_threshold);
        loaded_any = true;
    }
    
    item = cJSON_GetObjectItem(root, "shutdown_delay_sec");
    if (cJSON_IsNumber(item) && item->valueint > 0) {
        s_pp.config.shutdown_delay_sec = (uint32_t)item->valueint;
        TS_LOGI(TAG, "NVS: shutdown_delay_sec = %lu", (unsigned long)s_pp.config.shutdown_delay_sec);
        loaded_any = true;
    }
    
    item = cJSON_GetObjectItem(root, "recovery_hold_sec");
    if (cJSON_IsNumber(item) && item->valueint > 0) {
        s_pp.config.recovery_hold_sec = (uint32_t)item->valueint;
        TS_LOGI(TAG, "NVS: recovery_hold_sec = %lu", (unsigned long)s_pp.config.recovery_hold_sec);
        loaded_any = true;
    }
    
    item = cJSON_GetObjectItem(root, "fan_stop_delay_sec");
    if (cJSON_IsNumber(item) && item->valueint > 0) {
        s_pp.config.fan_stop_delay_sec = (uint32_t)item->valueint;
        TS_LOGI(TAG, "NVS: fan_stop_delay_sec = %lu", (unsigned long)s_pp.config.fan_stop_delay_sec);
        loaded_any = true;
    }
    
    /* enabled 字段单独处理，用于控制服务启动时是否自动启动保护 */
    item = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(item)) {
        s_pp.enabled_on_boot = cJSON_IsTrue(item);
        TS_LOGI(TAG, "NVS: enabled_on_boot = %s", s_pp.enabled_on_boot ? "true" : "false");
        loaded_any = true;
    }
    
    cJSON_Delete(root);
    
    if (loaded_any) {
        TS_LOGI(TAG, "Loaded power policy config from system NVS");
        return ESP_OK;
    }
    
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t save_config_values(bool enabled)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    bool complete = cJSON_AddNumberToObject(json, "low_voltage_threshold", s_pp.config.low_voltage_threshold) &&
        cJSON_AddNumberToObject(json, "recovery_voltage_threshold", s_pp.config.recovery_voltage_threshold) &&
        cJSON_AddNumberToObject(json, "shutdown_delay_sec", s_pp.config.shutdown_delay_sec) &&
        cJSON_AddNumberToObject(json, "recovery_hold_sec", s_pp.config.recovery_hold_sec) &&
        cJSON_AddNumberToObject(json, "fan_stop_delay_sec", s_pp.config.fan_stop_delay_sec) &&
        cJSON_AddBoolToObject(json, "enabled", enabled);
    char *bytes = complete ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!bytes) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(handle, "policy", bytes, strlen(bytes));
        if (ret == ESP_OK) {
            ret = nvs_erase_key(handle, "config");
            if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
        }
        if (ret == ESP_OK) ret = nvs_commit(handle);
        nvs_close(handle);
    }
    free(bytes);
    return ret;
}

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief Ping 检测主机是否在线
 */
static bool ping_host(const char *ip, int timeout_ms)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7);  // Echo port
    
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        return false;
    }
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }
    
    // 设置超时
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    // 非阻塞连接检测
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    
    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    bool reachable = false;
    
    if (ret == 0) {
        reachable = true;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        
        if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0) {
            int so_error;
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
            reachable = (so_error == 0);
        }
    }
    
    close(sock);
    return reachable;
}

/**
 * @brief 停止所有风扇
 */
static void stop_all_fans(void)
{
    TS_LOGI(TAG, "Stopping all fans...");
    
    if (!ts_fan_is_initialized()) {
        TS_LOGW(TAG, "Fan subsystem not initialized, skip fan stop");
        return;
    }
    
    for (int i = 0; i < TS_FAN_MAX; i++) {
        esp_err_t ret = ts_fan_set_mode(i, TS_FAN_MODE_OFF);
        if (ret == ESP_OK) {
            TS_LOGI(TAG, "Fan %d stopped", i);
        }
    }
}

/*===========================================================================*/
/*                          Forward Declarations                              */
/*===========================================================================*/

static void power_policy_task(void *pvParameters);
static void check_voltage(float voltage);
static void check_device_status(void);
static void execute_shutdown(void);
static void execute_recovery(void);
static void update_led_status(void);
static void trigger_event(ts_power_policy_event_t event);

/*===========================================================================*/
/*                          Public API                                        */
/*===========================================================================*/

esp_err_t ts_power_policy_get_default_config(ts_power_policy_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(config, 0, sizeof(ts_power_policy_config_t));
    config->low_voltage_threshold = TS_POWER_POLICY_LOW_VOLTAGE_DEFAULT;
    config->recovery_voltage_threshold = TS_POWER_POLICY_RECOVERY_VOLTAGE_DEFAULT;
    config->shutdown_delay_sec = TS_POWER_POLICY_SHUTDOWN_DELAY_DEFAULT;
    config->recovery_hold_sec = TS_POWER_POLICY_RECOVERY_HOLD_DEFAULT;
    config->fan_stop_delay_sec = TS_POWER_POLICY_FAN_STOP_DELAY_DEFAULT;
    config->auto_recovery_enabled = true;
    config->enable_led_feedback = true;
    config->enable_device_shutdown = true;
    config->enable_fan_control = true;
    config->lpmu_ping_before_shutdown = true;
    
    return ESP_OK;
}

esp_err_t ts_power_policy_get_config(ts_power_policy_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!s_pp.initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *config = s_pp.config;
    xSemaphoreGive(s_pp.state_mutex);
    return ESP_OK;
}

esp_err_t ts_power_policy_init(const ts_power_policy_config_t *config)
{
    if (s_pp.initialized) {
        TS_LOGW(TAG, "Power policy already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    TS_LOGI(TAG, "Initializing power policy v%s", TS_POWER_POLICY_VERSION);
    
    /* 使用提供的配置或默认配置 */
    if (config != NULL) {
        memcpy(&s_pp.config, config, sizeof(ts_power_policy_config_t));
    } else {
        ts_power_policy_get_default_config(&s_pp.config);
    }
    
    esp_err_t loaded = load_config_values("policy");
    if (loaded == ESP_ERR_NOT_FOUND) {
        loaded = load_config_values("config");
        if (loaded == ESP_OK) {
            esp_err_t ret = save_config_values(s_pp.enabled_on_boot);
            if (ret != ESP_OK) return ret;
        }
    }
    if (loaded != ESP_OK && loaded != ESP_ERR_NOT_FOUND) return loaded;
    bool loaded_from_nvs = loaded == ESP_OK;
    if (!loaded_from_nvs) s_pp.enabled_on_boot = true;

    /* 创建互斥锁 */
    s_pp.state_mutex = xSemaphoreCreateMutex();
    if (s_pp.state_mutex == NULL) {
        TS_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }
    
    /* 初始化状态 */
    s_pp.state = TS_POWER_POLICY_STATE_NORMAL;
    s_pp.start_time_us = esp_timer_get_time();
    s_pp.test_mode = false;
    s_pp.test_voltage = 0.0f;
    
    s_pp.initialized = true;
    
    TS_LOGI(TAG, "Power policy initialized (Low: %.1fV, Recovery: %.1fV, Delay: %lus) [source: %s]",
            s_pp.config.low_voltage_threshold,
            s_pp.config.recovery_voltage_threshold,
            (unsigned long)s_pp.config.shutdown_delay_sec,
            loaded_from_nvs ? "system NVS" : "defaults");
    
    return ESP_OK;
}

esp_err_t ts_power_policy_deinit(void)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    TS_LOGI(TAG, "Deinitializing power policy");
    
    /* 停止监控 */
    ts_power_policy_stop();
    
    /* 清理互斥锁 */
    if (s_pp.state_mutex) {
        vSemaphoreDelete(s_pp.state_mutex);
        s_pp.state_mutex = NULL;
    }
    
    memset(&s_pp, 0, sizeof(power_policy_state_t));
    
    return ESP_OK;
}

esp_err_t ts_power_policy_start(void)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_pp.running) {
        TS_LOGW(TAG, "Power policy already running");
        return ESP_OK;
    }
    
    TS_LOGI(TAG, "Starting power policy monitoring");
    
    /* 尝试注册自动化变量（如果 ts_variable 已初始化）
     * 通常此时 ts_variable 尚未初始化，变量注册会在 automation 服务启动后
     * 通过 ts_power_policy_register_variables() 完成 */
    ts_power_policy_register_variables();
    
    /* 先设置 running 标志，避免竞态条件 */
    s_pp.running = true;
    
    /* 创建监控任务 */
    BaseType_t ret = xTaskCreate(
        power_policy_task,
        "power_policy",
        3072,
        NULL,
        5,
        &s_pp.monitor_task_handle
    );
    
    if (ret != pdPASS) {
        TS_LOGE(TAG, "Failed to create power policy task");
        s_pp.running = false;
        return ESP_FAIL;
    }
    
    TS_LOGI(TAG, "Power policy monitoring started");
    return ESP_OK;
}

esp_err_t ts_power_policy_stop(void)
{
    if (!s_pp.initialized || !s_pp.running) {
        return ESP_OK;
    }
    
    TS_LOGI(TAG, "Stopping power policy monitoring");
    
    s_pp.running = false;
    
    /* 删除监控任务 */
    if (s_pp.monitor_task_handle) {
        vTaskDelete(s_pp.monitor_task_handle);
        s_pp.monitor_task_handle = NULL;
    }
    
    TS_LOGI(TAG, "Power policy monitoring stopped");
    return ESP_OK;
}

esp_err_t ts_power_policy_get_status(ts_power_policy_status_t *status)
{
    if (!s_pp.initialized || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        status->initialized = s_pp.initialized;
        status->running = s_pp.running;
        status->state = s_pp.state;
        status->current_voltage = s_pp.current_voltage;
        status->countdown_remaining_sec = s_pp.countdown_remaining_sec;
        status->recovery_timer_sec = s_pp.recovery_timer_sec;
        status->protection_count = s_pp.protection_count;
        status->uptime_ms = (esp_timer_get_time() - s_pp.start_time_us) / 1000;
        
        status->device_status.agx_powered = s_pp.agx_powered;
        status->device_status.lpmu_powered = s_pp.lpmu_powered;
        status->device_status.agx_connected = s_pp.agx_connected;
        
        xSemaphoreGive(s_pp.state_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

bool ts_power_policy_is_initialized(void)
{
    return s_pp.initialized;
}

bool ts_power_policy_is_running(void)
{
    return s_pp.running;
}

bool ts_power_policy_should_auto_start(void)
{
    /* 如果未初始化，默认应该启动 */
    if (!s_pp.initialized) {
        return true;
    }
    return s_pp.enabled_on_boot;
}

ts_power_policy_state_t ts_power_policy_get_state(void)
{
    return s_pp.state;
}

const char *ts_power_policy_get_state_name(ts_power_policy_state_t state)
{
    switch (state) {
        case TS_POWER_POLICY_STATE_NORMAL:      return "正常运行";
        case TS_POWER_POLICY_STATE_LOW_VOLTAGE: return "低电压保护";
        case TS_POWER_POLICY_STATE_SHUTDOWN:    return "关机中";
        case TS_POWER_POLICY_STATE_PROTECTED:   return "保护状态";
        case TS_POWER_POLICY_STATE_RECOVERY:    return "电压恢复中";
        default:                                return "未知";
    }
}

esp_err_t ts_power_policy_trigger_test(void)
{
    if (!s_pp.initialized) {
        TS_LOGE(TAG, "Cannot trigger test: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!s_pp.running) {
        TS_LOGE(TAG, "Cannot trigger test: not running");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_pp.test_mode = true;
        s_pp.test_voltage = s_pp.config.low_voltage_threshold - 0.5f;
        
        TS_LOGW(TAG, "Test mode activated: simulating %.2fV (threshold: %.2fV)",
                s_pp.test_voltage, s_pp.config.low_voltage_threshold);
        
        xSemaphoreGive(s_pp.state_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_policy_reset(void)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        TS_LOGI(TAG, "Protection reset requested - will restart ESP32");
        
        /* 重置状态 */
        s_pp.test_mode = false;
        s_pp.state = TS_POWER_POLICY_STATE_NORMAL;
        s_pp.countdown_remaining_sec = 0;
        s_pp.recovery_timer_sec = 0;
        
        xSemaphoreGive(s_pp.state_mutex);
        
        TS_LOGI(TAG, "Restarting system...");
        vTaskDelay(pdMS_TO_TICKS(100));  /* 允许日志刷新 */
        esp_restart();
        
        return ESP_OK;  /* 不会到达 */
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_policy_set_thresholds(float low_threshold, float recovery_threshold)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (low_threshold >= recovery_threshold) {
        TS_LOGE(TAG, "Low threshold must be less than recovery threshold");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_pp.config.low_voltage_threshold = low_threshold;
        s_pp.config.recovery_voltage_threshold = recovery_threshold;
        
        TS_LOGI(TAG, "Thresholds updated: Low=%.1fV, Recovery=%.1fV",
                low_threshold, recovery_threshold);
        
        xSemaphoreGive(s_pp.state_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_policy_get_thresholds(float *low_threshold, float *recovery_threshold)
{
    if (!s_pp.initialized || low_threshold == NULL || recovery_threshold == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *low_threshold = s_pp.config.low_voltage_threshold;
    *recovery_threshold = s_pp.config.recovery_voltage_threshold;
    
    return ESP_OK;
}

esp_err_t ts_power_policy_register_callback(ts_power_policy_callback_t callback, void *user_data)
{
    s_pp.callback = callback;
    s_pp.callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t ts_power_policy_set_debug_mode(bool enable, uint32_t duration_sec)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_pp.debug_mode = enable;
        s_pp.debug_remaining_sec = duration_sec;
        
        if (enable) {
            TS_LOGI(TAG, "Debug mode enabled for %lu seconds", (unsigned long)duration_sec);
        } else {
            TS_LOGI(TAG, "Debug mode disabled");
        }
        
        xSemaphoreGive(s_pp.state_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

bool ts_power_policy_is_debug_mode(void)
{
    return s_pp.debug_mode;
}

/*===========================================================================*/
/*                          Internal Functions                                */
/*===========================================================================*/

static void trigger_event(ts_power_policy_event_t event)
{
    /* 注意：此函数在持有 state_mutex 时调用，不能再获取锁 */
    /* 直接读取状态（调用者已持有锁） */
    ts_power_policy_status_t status = {
        .initialized = s_pp.initialized,
        .running = s_pp.running,
        .state = s_pp.state,
        .current_voltage = s_pp.current_voltage,
        .countdown_remaining_sec = s_pp.countdown_remaining_sec,
        .recovery_timer_sec = s_pp.recovery_timer_sec,
        .protection_count = s_pp.protection_count,
        .uptime_ms = (esp_timer_get_time() - s_pp.start_time_us) / 1000,
        .device_status = {
            .agx_powered = s_pp.agx_powered,
            .lpmu_powered = s_pp.lpmu_powered,
            .agx_connected = s_pp.agx_connected,
        }
    };
    
    /* 调用用户回调 */
    if (s_pp.callback) {
        s_pp.callback(event, &status, s_pp.callback_user_data);
    }
    
    /* 发布到事件总线 */
    ts_event_post(TS_EVENT_BASE_POWER, (int32_t)event, &status, sizeof(status), 0);
}

static void power_policy_task(void *pvParameters)
{
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t check_period = pdMS_TO_TICKS(1000);  /* 每秒检查一次 */
    uint32_t voltage_read_fail_count = 0;
    
    TS_LOGI(TAG, "Power policy task started");
    
    while (s_pp.running) {
        /* 读取电压 */
        float voltage_to_check;
        bool voltage_valid = false;
        
        /* 检查测试模式 */
        bool test_mode_active = false;
        float test_voltage_value = 0.0f;
        
        if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            test_mode_active = s_pp.test_mode;
            test_voltage_value = s_pp.test_voltage;
            xSemaphoreGive(s_pp.state_mutex);
        }
        
        if (test_mode_active) {
            voltage_to_check = test_voltage_value;
            voltage_valid = true;
            TS_LOGD(TAG, "Test mode: using simulated voltage %.2fV", voltage_to_check);
        } else {
            /* 从 power_monitor 读取电压 */
            ts_power_voltage_data_t voltage_data;
            esp_err_t ret = ts_power_monitor_read_voltage_now(&voltage_data);
            
            if (ret == ESP_OK) {
                voltage_to_check = voltage_data.supply_voltage;
                
                /* 忽略无效读数（0V 或 < 5V）*/
                if (voltage_to_check > TS_POWER_POLICY_MIN_VALID_VOLTAGE) {
                    voltage_valid = true;
                    
                    if (voltage_read_fail_count > 0) {
                        TS_LOGI(TAG, "Voltage reading recovered: %.2fV", voltage_to_check);
                        voltage_read_fail_count = 0;
                    }
                } else {
                    voltage_read_fail_count++;
                    if (voltage_read_fail_count == 1 || voltage_read_fail_count % 10 == 0) {
                        TS_LOGW(TAG, "Invalid voltage reading: %.2fV (count: %lu)",
                                voltage_to_check, (unsigned long)voltage_read_fail_count);
                    }
                }
            } else {
                voltage_read_fail_count++;
                if (voltage_read_fail_count == 1 || voltage_read_fail_count % 10 == 0) {
                    TS_LOGW(TAG, "Failed to read voltage (count: %lu): %s",
                            (unsigned long)voltage_read_fail_count, esp_err_to_name(ret));
                }
            }
        }
        
        /* 电压有效时进行状态检查 */
        if (voltage_valid) {
            check_voltage(voltage_to_check);
        }
        
        /* 检查设备状态 */
        check_device_status();
        
        /* 状态机处理 */
        if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (s_pp.state) {
                case TS_POWER_POLICY_STATE_LOW_VOLTAGE:
                    if (s_pp.countdown_remaining_sec > 0) {
                        s_pp.countdown_remaining_sec--;
                        
                        /* 每 10 秒或最后 5 秒打印日志 */
                        if (s_pp.countdown_remaining_sec % 10 == 0 ||
                            s_pp.countdown_remaining_sec <= 5) {
                            TS_LOGW(TAG, "Low voltage countdown: %lus remaining",
                                    (unsigned long)s_pp.countdown_remaining_sec);
                            trigger_event(TS_POWER_POLICY_EVENT_COUNTDOWN_TICK);
                        }
                        
                        if (s_pp.countdown_remaining_sec == 0) {
                            TS_LOGW(TAG, "Countdown complete, initiating shutdown");
                            s_pp.state = TS_POWER_POLICY_STATE_SHUTDOWN;
                            trigger_event(TS_POWER_POLICY_EVENT_SHUTDOWN_START);
                        }
                    }
                    break;
                    
                case TS_POWER_POLICY_STATE_RECOVERY:
                    if (s_pp.recovery_timer_sec > 0) {
                        s_pp.recovery_timer_sec--;
                        TS_LOGI(TAG, "Recovery timer: %lus remaining",
                                (unsigned long)s_pp.recovery_timer_sec);
                        
                        if (s_pp.recovery_timer_sec == 0) {
                            TS_LOGI(TAG, "Recovery confirmed, restarting system");
                            trigger_event(TS_POWER_POLICY_EVENT_RECOVERY_COMPLETE);
                            xSemaphoreGive(s_pp.state_mutex);
                            execute_recovery();
                            continue;  /* execute_recovery 会重启，不会返回 */
                        }
                    }
                    break;
                    
                case TS_POWER_POLICY_STATE_SHUTDOWN:
                    xSemaphoreGive(s_pp.state_mutex);
                    execute_shutdown();
                    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        s_pp.state = TS_POWER_POLICY_STATE_PROTECTED;
                        trigger_event(TS_POWER_POLICY_EVENT_PROTECTED);
                        TS_LOGW(TAG, "Entered protected state");
                    }
                    break;
                    
                case TS_POWER_POLICY_STATE_PROTECTED:
                    /* 在保护状态下处理风扇关闭定时器 */
                    if (s_pp.config.enable_fan_control && !s_pp.fans_stopped && s_pp.shutdown_timer_sec > 0) {
                        s_pp.shutdown_timer_sec--;
                        
                        if (s_pp.shutdown_timer_sec % 10 == 0 ||
                            s_pp.shutdown_timer_sec <= 5) {
                            TS_LOGI(TAG, "Fan shutdown countdown: %lus remaining",
                                    (unsigned long)s_pp.shutdown_timer_sec);
                        }
                        
                        if (s_pp.shutdown_timer_sec == 0) {
                            TS_LOGW(TAG, "Fan timer expired, stopping all fans now");
                            stop_all_fans();
                            s_pp.fans_stopped = true;
                        }
                    }
                    break;
                    
                default:
                    break;
            }
            
            /* 调试模式：每秒输出状态 */
            if (s_pp.debug_mode) {
                /* 发送 WebSocket 事件（Web 终端） */
                trigger_event(TS_POWER_POLICY_EVENT_DEBUG_TICK);
                
                /* 打印到串口日志 */
                TS_LOGI(TAG, "[DEBUG] %s | V: %.2fV | 倒计时: %lus | 恢复: %lus | 剩余: %lus",
                        ts_power_policy_get_state_name(s_pp.state),
                        s_pp.current_voltage,
                        (unsigned long)s_pp.countdown_remaining_sec,
                        (unsigned long)s_pp.recovery_timer_sec,
                        (unsigned long)s_pp.debug_remaining_sec);
                
                /* 递减调试计时器 */
                if (s_pp.debug_remaining_sec > 0) {
                    s_pp.debug_remaining_sec--;
                    if (s_pp.debug_remaining_sec == 0) {
                        s_pp.debug_mode = false;
                        TS_LOGI(TAG, "Debug mode auto-disabled");
                    }
                }
            }
            
            xSemaphoreGive(s_pp.state_mutex);
        }
        
        /* 更新 LED 状态 */
        update_led_status();
        
        /* 更新自动化系统变量（供 WebUI 数据监控使用） */
        ts_power_policy_update_variables();
        
        /* 等待下一个周期 */
        vTaskDelayUntil(&last_wake_time, check_period);
    }
    
    TS_LOGI(TAG, "Power policy task ended");
    vTaskDelete(NULL);
}

static void check_voltage(float voltage)
{
    if (!s_pp.initialized) {
        return;
    }
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    s_pp.current_voltage = voltage;
    ts_power_policy_state_t old_state = s_pp.state;
    
    switch (s_pp.state) {
        case TS_POWER_POLICY_STATE_NORMAL:
            if (voltage < s_pp.config.low_voltage_threshold) {
                TS_LOGW(TAG, "[STATE] NORMAL -> LOW_VOLTAGE: %.2fV < %.2fV",
                        voltage, s_pp.config.low_voltage_threshold);
                s_pp.state = TS_POWER_POLICY_STATE_LOW_VOLTAGE;
                s_pp.countdown_remaining_sec = s_pp.config.shutdown_delay_sec;
                s_pp.protection_count++;
                trigger_event(TS_POWER_POLICY_EVENT_LOW_VOLTAGE);
            }
            break;
            
        case TS_POWER_POLICY_STATE_LOW_VOLTAGE:
            /* 倒计时期间电压恢复 → 取消关机 */
            if (voltage >= s_pp.config.recovery_voltage_threshold) {
                TS_LOGI(TAG, "[STATE] LOW_VOLTAGE -> NORMAL: %.2fV >= %.2fV (countdown canceled)",
                        voltage, s_pp.config.recovery_voltage_threshold);
                s_pp.state = TS_POWER_POLICY_STATE_NORMAL;
                s_pp.countdown_remaining_sec = 0;
            }
            break;
            
        case TS_POWER_POLICY_STATE_PROTECTED:
            /* 保护状态下电压恢复 → 开始恢复流程 */
            if (voltage >= s_pp.config.recovery_voltage_threshold) {
                TS_LOGI(TAG, "[STATE] PROTECTED -> RECOVERY: %.2fV >= %.2fV",
                        voltage, s_pp.config.recovery_voltage_threshold);
                s_pp.state = TS_POWER_POLICY_STATE_RECOVERY;
                s_pp.recovery_timer_sec = s_pp.config.recovery_hold_sec;
                trigger_event(TS_POWER_POLICY_EVENT_RECOVERY_START);
            }
            break;
            
        case TS_POWER_POLICY_STATE_RECOVERY:
            /* 恢复期间电压再次下降 → 回到保护状态或低电压状态 */
            if (voltage < s_pp.config.recovery_voltage_threshold) {
                s_pp.recovery_timer_sec = 0;
                
                if (voltage < s_pp.config.low_voltage_threshold) {
                    /* 电压再次过低，重新进入低电压倒计时（但设备已关机） */
                    TS_LOGW(TAG, "[STATE] RECOVERY -> LOW_VOLTAGE: %.2fV < %.2fV",
                            voltage, s_pp.config.low_voltage_threshold);
                    s_pp.state = TS_POWER_POLICY_STATE_LOW_VOLTAGE;
                    s_pp.countdown_remaining_sec = s_pp.config.shutdown_delay_sec;
                } else {
                    /* 电压不够恢复阈值，回到保护状态继续等待 */
                    TS_LOGI(TAG, "[STATE] RECOVERY -> PROTECTED: %.2fV < %.2fV (voltage dropped)",
                            voltage, s_pp.config.recovery_voltage_threshold);
                    s_pp.state = TS_POWER_POLICY_STATE_PROTECTED;
                }
            }
            break;
            
        default:
            break;
    }
    
    /* 状态变化时触发事件 */
    if (old_state != s_pp.state) {
        trigger_event(TS_POWER_POLICY_EVENT_STATE_CHANGED);
    }
    
    xSemaphoreGive(s_pp.state_mutex);
}

static void check_device_status(void)
{
    /* 从 device_ctrl 获取 AGX/LPMU 状态 */
    ts_device_status_t agx_status, lpmu_status;
    
    if (ts_device_get_status(TS_DEVICE_AGX, &agx_status) == ESP_OK) {
        s_pp.agx_powered = (agx_status.state == TS_DEVICE_STATE_ON);
    }
    
    if (ts_device_get_status(TS_DEVICE_LPMU, &lpmu_status) == ESP_OK) {
        s_pp.lpmu_powered = (lpmu_status.state == TS_DEVICE_STATE_ON);
    }
    
    /* TODO: 检查 AGX WebSocket 连接状态（需要 agx_monitor 组件）*/
}

static void execute_shutdown(void)
{
    TS_LOGW(TAG, "Executing protective shutdown");
    
    if (!s_pp.config.enable_device_shutdown) {
        TS_LOGI(TAG, "Device shutdown disabled in config, skipping");
        return;
    }
    
    /* AGX: 通过 device_ctrl 执行关机（拉高 reset 引脚保持断电状态）*/
    if (s_pp.agx_powered) {
        TS_LOGI(TAG, "Powering off AGX via device_ctrl");
        esp_err_t ret = ts_device_power_off(TS_DEVICE_AGX);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to power off AGX: %s", esp_err_to_name(ret));
        }
    }
    
    /* LPMU: 先检查是否在线（ping），如果在线才执行关机 */
    if (s_pp.lpmu_powered) {
        if (s_pp.config.lpmu_ping_before_shutdown) {
            TS_LOGI(TAG, "Pinging LPMU at %s before shutdown...", LPMU_IP);
            bool lpmu_reachable = ping_host(LPMU_IP, 2000);
            
            if (lpmu_reachable) {
                TS_LOGI(TAG, "LPMU is reachable, sending power toggle");
                ts_device_power_off(TS_DEVICE_LPMU);
            } else {
                TS_LOGW(TAG, "LPMU unreachable, may already be off, skipping toggle");
            }
        } else {
            TS_LOGI(TAG, "Sending LPMU power toggle (ping check disabled)");
            ts_device_power_off(TS_DEVICE_LPMU);
        }
    }
    
    /* 启动风扇关闭定时器 */
    if (s_pp.config.enable_fan_control) {
        s_pp.shutdown_timer_sec = s_pp.config.fan_stop_delay_sec;
        s_pp.fans_stopped = false;
        TS_LOGI(TAG, "Fan stop timer started: will stop fans in %lu seconds", 
                (unsigned long)s_pp.config.fan_stop_delay_sec);
    }
}

static void execute_recovery(void)
{
    TS_LOGI(TAG, "Voltage recovered - restarting ESP32 to restore system");
    
    /* 等待日志刷新 */
    vTaskDelay(pdMS_TO_TICKS(100));
    
    /* 重启 ESP32 */
    esp_restart();
}

static void update_led_status(void)
{
    if (!s_pp.config.enable_led_feedback) {
        return;
    }
    
    static ts_power_policy_state_t last_state = TS_POWER_POLICY_STATE_NORMAL;
    ts_power_policy_state_t current_state;
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        current_state = s_pp.state;
        xSemaphoreGive(s_pp.state_mutex);
    } else {
        return;
    }
    
    if (current_state != last_state) {
        TS_LOGI(TAG, "LED state change: %s -> %s",
                ts_power_policy_get_state_name(last_state),
                ts_power_policy_get_state_name(current_state));
        
        switch (current_state) {
            case TS_POWER_POLICY_STATE_LOW_VOLTAGE:
            case TS_POWER_POLICY_STATE_SHUTDOWN:
            case TS_POWER_POLICY_STATE_PROTECTED:
                /* TODO: 启动 Touch LED 橙色呼吸动画 */
                TS_LOGI(TAG, "Should start orange breathe animation");
                break;
                
            case TS_POWER_POLICY_STATE_NORMAL:
            case TS_POWER_POLICY_STATE_RECOVERY:
                /* TODO: 恢复 Touch LED 正常状态 */
                TS_LOGI(TAG, "Should restore normal LED state");
                break;
                
            default:
                break;
        }
        
        last_state = current_state;
    }
}

/*===========================================================================*/
/*                       Additional API Functions                             */
/*===========================================================================*/

esp_err_t ts_power_policy_set_shutdown_delay(uint32_t seconds)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (seconds < 10 || seconds > 600) {
        TS_LOGE(TAG, "Invalid shutdown delay: %lu (must be 10-600 seconds)",
                (unsigned long)seconds);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_pp.config.shutdown_delay_sec = seconds;
        TS_LOGI(TAG, "Shutdown delay set to %lu seconds", (unsigned long)seconds);
        xSemaphoreGive(s_pp.state_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_policy_set_recovery_hold(uint32_t seconds)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (seconds < 1 || seconds > 300) {
        TS_LOGE(TAG, "Invalid recovery hold time: %lu (must be 1-300 seconds)",
                (unsigned long)seconds);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_pp.config.recovery_hold_sec = seconds;
        TS_LOGI(TAG, "Recovery hold time set to %lu seconds", (unsigned long)seconds);
        xSemaphoreGive(s_pp.state_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_policy_set_fan_stop_delay(uint32_t seconds)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (seconds < 10 || seconds > 600) {
        TS_LOGE(TAG, "Invalid fan stop delay: %lu (must be 10-600 seconds)",
                (unsigned long)seconds);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_pp.config.fan_stop_delay_sec = seconds;
        TS_LOGI(TAG, "Fan stop delay set to %lu seconds", (unsigned long)seconds);
        xSemaphoreGive(s_pp.state_mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_power_policy_save_config(void)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = save_config_values(s_pp.running);
    if (ret == ESP_OK) TS_LOGI(TAG, "Power policy config saved to system NVS (enabled=%s)", s_pp.running ? "true" : "false");
    return ret;
}

/* 标记变量是否已注册，避免重复注册 */
static bool s_variables_registered = false;

esp_err_t ts_power_policy_register_variables(void)
{
    /* 如果已注册或 ts_variable 未初始化，则跳过 */
    if (s_variables_registered) {
        return ESP_OK;
    }
    
    if (!ts_variable_is_initialized()) {
        TS_LOGD(TAG, "ts_variable not initialized, cannot register variables yet");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 变量定义 */
    static const struct {
        const char *name;
        ts_auto_value_type_t type;
    } var_defs[] = {
        {"power_policy.state", TS_AUTO_VAL_STRING},
        {"power_policy.voltage", TS_AUTO_VAL_FLOAT},
        {"power_policy.countdown", TS_AUTO_VAL_INT},
        {"power_policy.recovery_timer", TS_AUTO_VAL_INT},
        {"power_policy.protection_count", TS_AUTO_VAL_INT},
        {"power_policy.is_normal", TS_AUTO_VAL_INT},
        {"power_policy.is_protected", TS_AUTO_VAL_INT},
        {"power_policy.is_low_voltage", TS_AUTO_VAL_INT},
    };
    
    /* 使用堆分配避免栈溢出（ts_auto_variable_t ~350字节）*/
    ts_auto_variable_t *var = heap_caps_malloc(sizeof(ts_auto_variable_t), MALLOC_CAP_SPIRAM);
    if (var == NULL) {
        var = malloc(sizeof(ts_auto_variable_t));
    }
    if (var == NULL) {
        TS_LOGW(TAG, "Failed to allocate memory for variable registration");
        return ESP_ERR_NO_MEM;
    }
    
    int registered = 0;
    for (size_t i = 0; i < sizeof(var_defs) / sizeof(var_defs[0]); i++) {
        memset(var, 0, sizeof(ts_auto_variable_t));
        strncpy(var->name, var_defs[i].name, sizeof(var->name) - 1);
        strncpy(var->source_id, "power_policy", sizeof(var->source_id) - 1);
        var->value.type = var_defs[i].type;
        var->flags = TS_AUTO_VAR_READONLY;
        
        if (ts_variable_register(var) == ESP_OK) {
            registered++;
        }
    }
    
    free(var);
    s_variables_registered = true;
    
    TS_LOGI(TAG, "Registered %d automation variables for power policy", registered);
    return ESP_OK;
}

esp_err_t ts_power_policy_update_variables(void)
{
    if (!s_pp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ts_power_policy_state_t state;
    float voltage;
    uint32_t countdown, recovery_timer;
    uint32_t protection_count;
    
    /* 获取当前状态 */
    if (xSemaphoreTake(s_pp.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        state = s_pp.state;
        voltage = s_pp.current_voltage;
        countdown = s_pp.countdown_remaining_sec;
        recovery_timer = s_pp.recovery_timer_sec;
        protection_count = s_pp.protection_count;
        xSemaphoreGive(s_pp.state_mutex);
    } else {
        return ESP_ERR_TIMEOUT;
    }
    
    /* 更新自动化变量（使用 internal 版本绕过 READONLY 检查）*/
    ts_auto_value_t val;
    
    /* state (string) */
    memset(&val, 0, sizeof(val));
    val.type = TS_AUTO_VAL_STRING;
    strncpy(val.str_val, ts_power_policy_get_state_name(state), sizeof(val.str_val) - 1);
    ts_variable_set_internal("power_policy.state", &val);
    
    /* voltage (float) */
    val.type = TS_AUTO_VAL_FLOAT;
    val.float_val = voltage;
    ts_variable_set_internal("power_policy.voltage", &val);
    
    /* countdown (int) */
    val.type = TS_AUTO_VAL_INT;
    val.int_val = (int)countdown;
    ts_variable_set_internal("power_policy.countdown", &val);
    
    /* recovery_timer (int) */
    val.int_val = (int)recovery_timer;
    ts_variable_set_internal("power_policy.recovery_timer", &val);
    
    /* protection_count (int) */
    val.int_val = (int)protection_count;
    ts_variable_set_internal("power_policy.protection_count", &val);
    
    /* is_normal (int as bool) */
    val.int_val = (state == TS_POWER_POLICY_STATE_NORMAL) ? 1 : 0;
    ts_variable_set_internal("power_policy.is_normal", &val);
    
    /* is_protected (int as bool) */
    val.int_val = (state == TS_POWER_POLICY_STATE_PROTECTED || 
                   state == TS_POWER_POLICY_STATE_SHUTDOWN) ? 1 : 0;
    ts_variable_set_internal("power_policy.is_protected", &val);
    
    /* is_low_voltage (int as bool) */
    val.int_val = (state == TS_POWER_POLICY_STATE_LOW_VOLTAGE) ? 1 : 0;
    ts_variable_set_internal("power_policy.is_low_voltage", &val);
    
    return ESP_OK;
}
