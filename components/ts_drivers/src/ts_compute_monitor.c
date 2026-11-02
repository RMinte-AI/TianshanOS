/**
 * @file ts_compute_monitor.c
 * @brief Compute Device Monitor Implementation for TianShanOS
 * 
 * 算力设备监控服务实现 - WebSocket 客户端
 * 
 * 架构设计：
 * 1. 独立任务运行 WebSocket 客户端
 * 2. Socket.IO 协议握手和消息解析
 * 3. 温度数据自动推送到 ts_temp_source
 * 4. 通过 ts_event 发布数据更新事件
 * 
 * Socket.IO 协议：
 * - HTTP 升级握手获取 sid
 * - WebSocket 连接 + probe/upgrade 消息
 * - 事件消息格式：42["event_name", {...data...}]
 * 
 * 注意：此实现使用 ESP-IDF 的 esp_websocket_client
 */

#include "ts_compute_monitor.h"
#include "ts_core.h"  /* TS_CALLOC_PSRAM */
#include "ts_temp_source.h"
#include "ts_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

static const char *TAG = "ts_compute_monitor";

/*===========================================================================*/
/*                              Configuration                                 */
/*===========================================================================*/

/** NVS 存储键 */
#define NVS_NAMESPACE           "ts_compute"
#define NVS_KEY_SERVER_IP       "server_ip"
#define NVS_KEY_SERVER_PORT     "server_port"

#define SOCKETIO_PROBE_MESSAGE      "2probe"
#define SOCKETIO_UPGRADE_MESSAGE    "5"
#define SOCKETIO_PING_MESSAGE       "2"
#define SOCKETIO_PONG_MESSAGE       "3"
#define SOCKETIO_MESSAGE_PREFIX     "42"

#define HTTP_BUFFER_SIZE            1024
#define SID_MAX_LEN                 64
#define MAX_RECONNECT_DELAY_MS      30000

/*===========================================================================*/
/*                              Internal State                                */
/*===========================================================================*/

/** 运行时上下文 */
typedef struct {
    /* 配置 */
    ts_compute_config_t config;
    
    /* 状态 */
    bool initialized;
    atomic_bool running;
    atomic_bool should_stop;
    ts_compute_status_t status;
    
    /* WebSocket */
    esp_websocket_client_handle_t ws_client;
    char session_id[SID_MAX_LEN];
    bool ws_connected;
    bool upgrade_complete;
    
    /* 数据 */
    ts_compute_data_t latest_data;
    SemaphoreHandle_t data_mutex;
    
    /* 任务 */
    TaskHandle_t task_handle;
    SemaphoreHandle_t exit_done;
    
    /* 统计 */
    uint32_t total_reconnects;
    uint32_t messages_received;
    uint32_t parse_errors;
    uint64_t last_message_time;
    uint64_t connected_since;
    
    /* 回调 */
    ts_compute_event_callback_t callback;
    void *callback_user_data;
    
    /* 错误 */
    char last_error[TS_COMPUTE_MAX_ERROR_MSG_LEN];
    
} compute_monitor_ctx_t;

static compute_monitor_ctx_t *s_ctx = NULL;
/* One controller owns this monitor's context until its complete handoff. */
static atomic_flag s_control_busy = ATOMIC_FLAG_INIT;
static esp_err_t finish_control(esp_err_t result)
{
    atomic_flag_clear(&s_control_busy);
    return result;
}
static esp_err_t start_monitor(void);
static esp_err_t stop_monitor(void);

/*===========================================================================*/
/*                              Forward Declarations                          */
/*===========================================================================*/

static void compute_monitor_task(void *arg);
static esp_err_t socketio_handshake(void);
static void websocket_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data);
static esp_err_t parse_tegrastats_data(const char *json_str, ts_compute_data_t *data);
static void update_temp_source(const ts_compute_data_t *data);
static void publish_status_event(ts_compute_status_t status);
static void publish_data_event(const ts_compute_data_t *data);
static void set_error(const char *error);
static void set_status(ts_compute_status_t status);

/*===========================================================================*/
/*                              Public API                                    */
/*===========================================================================*/

esp_err_t ts_compute_monitor_get_default_config(ts_compute_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(config, 0, sizeof(ts_compute_config_t));
    strncpy(config->server_ip, TS_COMPUTE_DEFAULT_SERVER_IP, sizeof(config->server_ip) - 1);
    config->server_port = TS_COMPUTE_DEFAULT_SERVER_PORT;
    config->reconnect_interval_ms = TS_COMPUTE_DEFAULT_RECONNECT_MS;
    config->startup_delay_ms = TS_COMPUTE_DEFAULT_STARTUP_DELAY_MS;
    config->heartbeat_timeout_ms = TS_COMPUTE_DEFAULT_HEARTBEAT_TIMEOUT_MS;
    config->auto_start = true;
    config->update_temp_source = true;
    config->task_stack_size = TS_COMPUTE_DEFAULT_TASK_STACK;
    config->task_priority = TS_COMPUTE_DEFAULT_TASK_PRIORITY;
    
    return ESP_OK;
}

esp_err_t ts_compute_monitor_get_config(ts_compute_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_ctx != NULL && s_ctx->initialized) {
        /* 返回当前运行配置 */
        memcpy(config, &s_ctx->config, sizeof(ts_compute_config_t));
    } else {
        /* 尝试从 NVS 加载，否则返回默认配置 */
        if (ts_compute_monitor_load_config(config) != ESP_OK) {
            ts_compute_monitor_get_default_config(config);
        }
    }
    
    return ESP_OK;
}

esp_err_t ts_compute_monitor_load_config(ts_compute_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 先加载默认值 */
    ts_compute_monitor_get_default_config(config);
    
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "No NVS config found, using defaults");
        return ESP_ERR_NOT_FOUND;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 读取 server_ip */
    size_t len = sizeof(config->server_ip);
    ret = nvs_get_str(handle, NVS_KEY_SERVER_IP, config->server_ip, &len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Loaded server IP from NVS: %s", config->server_ip);
    }
    
    /* 读取 server_port */
    uint16_t port = 0;
    ret = nvs_get_u16(handle, NVS_KEY_SERVER_PORT, &port);
    if (ret == ESP_OK && port > 0) {
        config->server_port = port;
        ESP_LOGI(TAG, "Loaded server port from NVS: %d", port);
    }
    
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t ts_compute_monitor_save_config(const ts_compute_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 保存 server_ip */
    ret = nvs_set_str(handle, NVS_KEY_SERVER_IP, config->server_ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save server IP: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    /* 保存 server_port */
    ret = nvs_set_u16(handle, NVS_KEY_SERVER_PORT, config->server_port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save server port: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }
    
    ret = nvs_commit(handle);
    nvs_close(handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Saved config to NVS: %s:%d", config->server_ip, config->server_port);
    }
    
    return ret;
}

esp_err_t ts_compute_monitor_set_server(const char *server_ip, uint16_t server_port)
{
    if (atomic_flag_test_and_set(&s_control_busy)) return ESP_ERR_NOT_FINISHED;

    if (server_ip == NULL || strlen(server_ip) == 0) {
        return finish_control(ESP_ERR_INVALID_ARG);
    }
    
    ts_compute_config_t config;
    ts_compute_monitor_get_config(&config);
    
    /* 更新配置 */
    strncpy(config.server_ip, server_ip, sizeof(config.server_ip) - 1);
    config.server_ip[sizeof(config.server_ip) - 1] = '\0';
    
    if (server_port > 0) {
        config.server_port = server_port;
    }
    
    /* 保存到 NVS */
    esp_err_t ret = ts_compute_monitor_save_config(&config);
    if (ret != ESP_OK) {
        return finish_control(ret);
    }
    
    /* 如果已初始化，更新运行时配置 */
    if (s_ctx != NULL && s_ctx->initialized) {
        bool was_running = s_ctx->running;
        
        /* 如果正在运行，先停止 */
        if (was_running) {
            ESP_LOGI(TAG, "Stopping monitor to apply new config...");
            ret = stop_monitor();
            if (ret != ESP_OK) return finish_control(ret);
        }
        
        /* 更新配置 */
        strncpy(s_ctx->config.server_ip, server_ip, sizeof(s_ctx->config.server_ip) - 1);
        if (server_port > 0) {
            s_ctx->config.server_port = server_port;
        }
        
        ESP_LOGI(TAG, "Updated server config: %s:%d", 
                 s_ctx->config.server_ip, s_ctx->config.server_port);
        
        /* 如果之前在运行，重新启动 */
        if (was_running) {
            ESP_LOGI(TAG, "Restarting monitor with new config...");
            ret = start_monitor();
        }
    }
    
    return finish_control(ret);
}

esp_err_t ts_compute_monitor_init(const ts_compute_config_t *config)
{
    if (atomic_flag_test_and_set(&s_control_busy)) return ESP_ERR_NOT_FINISHED;

    if (s_ctx != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return finish_control(ESP_ERR_INVALID_STATE);
    }
    
    ESP_LOGI(TAG, "Initializing compute monitor v%s", TS_COMPUTE_MONITOR_VERSION);
    
    /* 分配上下文（优先使用 PSRAM）*/
    s_ctx = TS_CALLOC_PSRAM(1, sizeof(compute_monitor_ctx_t));
    if (s_ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return finish_control(ESP_ERR_NO_MEM);
    }
    
    atomic_init(&s_ctx->running, false);
    atomic_init(&s_ctx->should_stop, false);

    /* 加载配置：优先使用传入配置，否则尝试从 NVS 加载，最后使用默认值 */
    if (config != NULL) {
        memcpy(&s_ctx->config, config, sizeof(ts_compute_config_t));
    } else {
        /* 先尝试从 NVS 加载 */
        if (ts_compute_monitor_load_config(&s_ctx->config) != ESP_OK) {
            ts_compute_monitor_get_default_config(&s_ctx->config);
        }
    }
    
    /* 创建互斥锁 */
    s_ctx->data_mutex = xSemaphoreCreateMutex();
    if (s_ctx->data_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(s_ctx);
        s_ctx = NULL;
        return finish_control(ESP_ERR_NO_MEM);
    }
    
    s_ctx->exit_done = xSemaphoreCreateBinary();
    if (s_ctx->exit_done == NULL) {
        vSemaphoreDelete(s_ctx->data_mutex);
        free(s_ctx);
        s_ctx = NULL;
        return finish_control(ESP_ERR_NO_MEM);
    }

    /* 注册为温度源 provider */
    if (s_ctx->config.update_temp_source) {
        ts_temp_provider_register(TS_TEMP_SOURCE_AGX_AUTO, "agx_cpu");
    }
    
    s_ctx->initialized = true;
    s_ctx->status = TS_COMPUTE_STATUS_INITIALIZED;
    
    ESP_LOGI(TAG, "Initialized, server: %s:%d", 
             s_ctx->config.server_ip, s_ctx->config.server_port);
    
    return finish_control(ESP_OK);
}

esp_err_t ts_compute_monitor_deinit(void)
{
    if (atomic_flag_test_and_set(&s_control_busy)) return ESP_ERR_NOT_FINISHED;

    if (s_ctx == NULL) {
        return finish_control(ESP_ERR_INVALID_STATE);
    }
    
    /* 停止监控 */
    if (s_ctx->task_handle != NULL) {
        esp_err_t ret = stop_monitor();
        if (ret != ESP_OK) return finish_control(ret);
    }
    
    /* 注销温度源 provider */
    ts_temp_provider_unregister(TS_TEMP_SOURCE_AGX_AUTO);
    
    /* 清理资源 */
    if (s_ctx->data_mutex != NULL) {
        vSemaphoreDelete(s_ctx->data_mutex);
    }
    
    vSemaphoreDelete(s_ctx->exit_done);
    free(s_ctx);
    s_ctx = NULL;
    
    ESP_LOGI(TAG, "Deinitialized");
    return finish_control(ESP_OK);
}

static esp_err_t start_monitor(void)
{
    if (s_ctx == NULL || !s_ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_ctx->running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting compute monitor...");
    
    xSemaphoreTake(s_ctx->data_mutex, portMAX_DELAY);
    xSemaphoreTake(s_ctx->exit_done, 0);
    s_ctx->should_stop = false;
    s_ctx->running = true;
    
    /* 创建监控任务 */
    TaskHandle_t task = NULL;
    BaseType_t ret = xTaskCreate(
        compute_monitor_task,
        "compute_monitor",
        s_ctx->config.task_stack_size,
        s_ctx,
        s_ctx->config.task_priority,
        &task
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        s_ctx->running = false;
        xSemaphoreGive(s_ctx->data_mutex);
        return ESP_FAIL;
    }
    
    s_ctx->task_handle = task;
    xSemaphoreGive(s_ctx->data_mutex);
    return ESP_OK;
}

esp_err_t ts_compute_monitor_start(void)
{
    if (atomic_flag_test_and_set(&s_control_busy)) return ESP_ERR_NOT_FINISHED;
    return finish_control(start_monitor());
}

static esp_err_t stop_monitor(void)
{
    if (s_ctx == NULL || !s_ctx->running) return ESP_ERR_INVALID_STATE;
    if (s_ctx->task_handle == xTaskGetCurrentTaskHandle()) return ESP_ERR_NOT_ALLOWED;

    ESP_LOGI(TAG, "Stopping compute monitor...");
    xSemaphoreTake(s_ctx->data_mutex, portMAX_DELAY);
    s_ctx->should_stop = true;
    xTaskNotifyGive(s_ctx->task_handle);
    xSemaphoreGive(s_ctx->data_mutex);
    while (xSemaphoreTake(s_ctx->exit_done, portMAX_DELAY) != pdTRUE) {}
    /* The worker's last context access is its mutex release after signalling. */
    xSemaphoreTake(s_ctx->data_mutex, portMAX_DELAY);
    s_ctx->task_handle = NULL;
    s_ctx->running = false;
    xSemaphoreGive(s_ctx->data_mutex);
    set_status(TS_COMPUTE_STATUS_INITIALIZED);
    ESP_LOGI(TAG, "Stopped");
    return ESP_OK;
}

esp_err_t ts_compute_monitor_stop(void)
{
    if (atomic_flag_test_and_set(&s_control_busy)) return ESP_ERR_NOT_FINISHED;
    return finish_control(stop_monitor());
}

bool ts_compute_monitor_is_initialized(void)
{
    return (s_ctx != NULL && s_ctx->initialized);
}

bool ts_compute_monitor_is_running(void)
{
    return (s_ctx != NULL && s_ctx->running);
}

esp_err_t ts_compute_monitor_get_data(ts_compute_data_t *data)
{
    if (s_ctx == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_ctx->data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    memcpy(data, &s_ctx->latest_data, sizeof(ts_compute_data_t));
    xSemaphoreGive(s_ctx->data_mutex);
    
    return ESP_OK;
}

bool ts_compute_monitor_is_data_valid(void)
{
    if (s_ctx == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(s_ctx->data_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    
    bool valid = s_ctx->latest_data.is_valid;
    
    /* 检查超时 */
    if (valid) {
        uint64_t now = esp_timer_get_time();
        uint64_t age = now - s_ctx->latest_data.update_time_us;
        if (age > (uint64_t)s_ctx->config.heartbeat_timeout_ms * 1000) {
            valid = false;
        }
    }
    
    xSemaphoreGive(s_ctx->data_mutex);
    return valid;
}

esp_err_t ts_compute_monitor_get_status(ts_compute_status_info_t *status)
{
    if (s_ctx == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(status, 0, sizeof(ts_compute_status_info_t));
    
    status->initialized = s_ctx->initialized;
    status->running = s_ctx->running;
    status->connection_status = s_ctx->status;
    status->total_reconnects = s_ctx->total_reconnects;
    status->messages_received = s_ctx->messages_received;
    status->parse_errors = s_ctx->parse_errors;
    status->last_message_time_us = s_ctx->last_message_time;
    
    if (s_ctx->status == TS_COMPUTE_STATUS_CONNECTED && s_ctx->connected_since > 0) {
        status->connected_time_ms = (esp_timer_get_time() - s_ctx->connected_since) / 1000;
    }
    
    /* 计算连接可靠性 */
    if (s_ctx->messages_received > 0) {
        uint32_t total = s_ctx->messages_received + s_ctx->parse_errors;
        status->connection_reliability = 
            ((float)s_ctx->messages_received / total) * 100.0f;
    }
    
    strncpy(status->last_error, s_ctx->last_error, sizeof(status->last_error) - 1);
    
    return ESP_OK;
}

ts_compute_status_t ts_compute_monitor_get_connection_status(void)
{
    if (s_ctx == NULL) {
        return TS_COMPUTE_STATUS_UNINITIALIZED;
    }
    return s_ctx->status;
}

esp_err_t ts_compute_monitor_register_callback(ts_compute_event_callback_t callback,
                                           void *user_data)
{
    if (s_ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_ctx->callback = callback;
    s_ctx->callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t ts_compute_monitor_unregister_callback(void)
{
    if (s_ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_ctx->callback = NULL;
    s_ctx->callback_user_data = NULL;
    return ESP_OK;
}

const char *ts_compute_status_to_str(ts_compute_status_t status)
{
    switch (status) {
        case TS_COMPUTE_STATUS_UNINITIALIZED: return "UNINITIALIZED";
        case TS_COMPUTE_STATUS_INITIALIZED:   return "INITIALIZED";
        case TS_COMPUTE_STATUS_CONNECTING:    return "CONNECTING";
        case TS_COMPUTE_STATUS_CONNECTED:     return "CONNECTED";
        case TS_COMPUTE_STATUS_DISCONNECTED:  return "DISCONNECTED";
        case TS_COMPUTE_STATUS_RECONNECTING:  return "RECONNECTING";
        case TS_COMPUTE_STATUS_ERROR:         return "ERROR";
        default:                          return "UNKNOWN";
    }
}

/*===========================================================================*/
/*                              Internal Functions                            */
/*===========================================================================*/

/**
 * @brief 设置状态并发布事件
 */
static void set_status(ts_compute_status_t status)
{
    if (s_ctx == NULL) return;
    
    ts_compute_status_t old_status = s_ctx->status;
    s_ctx->status = status;
    
    if (old_status != status) {
        ESP_LOGI(TAG, "Status: %s -> %s", 
                 ts_compute_status_to_str(old_status),
                 ts_compute_status_to_str(status));
        publish_status_event(status);
    }
}

/**
 * @brief 设置错误信息
 */
static void set_error(const char *error)
{
    if (s_ctx == NULL || error == NULL) return;
    
    strncpy(s_ctx->last_error, error, sizeof(s_ctx->last_error) - 1);
    ESP_LOGE(TAG, "Error: %s", error);
}

/**
 * @brief 发布状态事件
 */
static void publish_status_event(ts_compute_status_t status)
{
    ts_event_id_t event_id;
    
    switch (status) {
        case TS_COMPUTE_STATUS_CONNECTED:
            event_id = TS_EVT_AGX_CONNECTED;
            break;
        case TS_COMPUTE_STATUS_DISCONNECTED:
        case TS_COMPUTE_STATUS_RECONNECTING:
            event_id = TS_EVT_AGX_DISCONNECTED;
            break;
        case TS_COMPUTE_STATUS_ERROR:
            event_id = TS_EVT_AGX_ERROR;
            break;
        default:
            return;
    }
    
    ts_event_post(TS_EVENT_BASE_DEVICE_MON, event_id, 
                  &status, sizeof(status), 0);
    
    /* 回调通知 */
    if (s_ctx->callback != NULL) {
        s_ctx->callback(status, &s_ctx->latest_data, s_ctx->callback_user_data);
    }
}

/**
 * @brief 发布数据更新事件
 */
static void publish_data_event(const ts_compute_data_t *data)
{
    if (data == NULL) return;
    
    ts_event_post(TS_EVENT_BASE_DEVICE_MON, TS_EVT_AGX_DATA_UPDATED,
                  data, sizeof(ts_compute_data_t), 0);
    
    /* 回调通知 */
    if (s_ctx->callback != NULL) {
        s_ctx->callback(s_ctx->status, data, s_ctx->callback_user_data);
    }
}

/**
 * @brief 更新温度源
 */
static void update_temp_source(const ts_compute_data_t *data)
{
    if (data == NULL || !s_ctx->config.update_temp_source) return;
    
    /* 将 CPU 温度更新到温度源管理 */
    int16_t temp_deci = (int16_t)(data->temperature.cpu * 10);  /* 转换为 0.1°C 单位 */
    ts_temp_provider_update(TS_TEMP_SOURCE_AGX_AUTO, temp_deci);
    
    ESP_LOGD(TAG, "Updated temp source: CPU=%.1f°C", data->temperature.cpu);
}

/**
 * @brief Socket.IO HTTP 握手
 * 
 * 获取 session id 和配置
 */
static esp_err_t socketio_handshake(void)
{
    char url[256];
    snprintf(url, sizeof(url), 
             "http://%s:%d/socket.io/?EIO=4&transport=polling",
             s_ctx->config.server_ip, s_ctx->config.server_port);
    
    ESP_LOGI(TAG, "Socket.IO handshake: %s", url);
    
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 5000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        set_error("HTTP client init failed");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        set_error("HTTP open failed");
        esp_http_client_cleanup(client);
        return err;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        set_error("HTTP fetch headers failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    
    char buffer[HTTP_BUFFER_SIZE];
    int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    if (read_len <= 0) {
        set_error("HTTP read failed");
        return ESP_FAIL;
    }
    
    buffer[read_len] = '\0';
    ESP_LOGD(TAG, "Handshake response: %s", buffer);
    
    /* 解析响应：格式为 "0{\"sid\":\"xxxxx\",...}" */
    char *json_start = strchr(buffer, '{');
    if (json_start == NULL) {
        set_error("Invalid handshake response");
        return ESP_FAIL;
    }
    
    cJSON *root = cJSON_Parse(json_start);
    if (root == NULL) {
        set_error("JSON parse failed");
        return ESP_FAIL;
    }
    
    cJSON *sid = cJSON_GetObjectItem(root, "sid");
    if (sid == NULL || !cJSON_IsString(sid)) {
        set_error("SID not found");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    strncpy(s_ctx->session_id, sid->valuestring, sizeof(s_ctx->session_id) - 1);
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Got session ID: %s", s_ctx->session_id);
    return ESP_OK;
}

/**
 * @brief WebSocket 事件处理器
 */
static void websocket_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected, sending probe");
            s_ctx->ws_connected = true;
            s_ctx->last_message_time = esp_timer_get_time();  /* 初始化心跳基准 */
            
            /* 发送 Socket.IO probe */
            esp_websocket_client_send_text(s_ctx->ws_client, 
                                           SOCKETIO_PROBE_MESSAGE, 
                                           strlen(SOCKETIO_PROBE_MESSAGE),
                                           portMAX_DELAY);
            ESP_LOGI(TAG, "Probe sent: %s", SOCKETIO_PROBE_MESSAGE);
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            s_ctx->ws_connected = false;
            s_ctx->upgrade_complete = false;
            set_status(TS_COMPUTE_STATUS_DISCONNECTED);
            break;
            
        case WEBSOCKET_EVENT_DATA:
            if (data->data_len > 0 && data->data_ptr != NULL) {
                char *msg = strndup(data->data_ptr, data->data_len);
                if (msg != NULL) {
                    ESP_LOGD(TAG, "WS recv: %s", msg);
                    
                    /* 处理 Socket.IO 消息 */
                    if (strcmp(msg, "3probe") == 0) {
                        /* Probe 响应，发送 upgrade */
                        ESP_LOGI(TAG, "Got probe response, sending upgrade");
                        esp_websocket_client_send_text(s_ctx->ws_client,
                                                       SOCKETIO_UPGRADE_MESSAGE,
                                                       strlen(SOCKETIO_UPGRADE_MESSAGE),
                                                       portMAX_DELAY);
                        s_ctx->upgrade_complete = true;
                        s_ctx->connected_since = esp_timer_get_time();
                        s_ctx->last_message_time = esp_timer_get_time();  /* 初始化心跳时间 */
                        set_status(TS_COMPUTE_STATUS_CONNECTED);
                        ESP_LOGI(TAG, "Socket.IO upgrade complete");
                    }
                    else if (strcmp(msg, SOCKETIO_PING_MESSAGE) == 0) {
                        /* Ping 消息，回复 Pong */
                        ESP_LOGD(TAG, "Socket.IO ping received, sending pong");
                        esp_websocket_client_send_text(s_ctx->ws_client,
                                                       SOCKETIO_PONG_MESSAGE,
                                                       strlen(SOCKETIO_PONG_MESSAGE),
                                                       portMAX_DELAY);
                        s_ctx->last_message_time = esp_timer_get_time();  /* 更新心跳时间 */
                    }
                    else if (strncmp(msg, SOCKETIO_MESSAGE_PREFIX, 2) == 0) {
                        /* 事件消息 42["event", data] */
                        char *json_start = strchr(msg, '[');
                        if (json_start != NULL) {
                            cJSON *array = cJSON_Parse(json_start);
                            if (array != NULL && cJSON_IsArray(array)) {
                                cJSON *event_name = cJSON_GetArrayItem(array, 0);
                                cJSON *event_data_json = cJSON_GetArrayItem(array, 1);
                                
                                if (event_name != NULL && 
                                    cJSON_IsString(event_name) &&
                                    strcmp(event_name->valuestring, "tegrastats_update") == 0 &&
                                    event_data_json != NULL) {
                                    
                                    ts_compute_data_t agx_data;
                                    memset(&agx_data, 0, sizeof(agx_data));
                                    
                                    if (parse_tegrastats_data(
                                            cJSON_Print(event_data_json), 
                                            &agx_data) == ESP_OK) {
                                        
                                        /* 更新数据 */
                                        agx_data.is_valid = true;
                                        agx_data.update_time_us = esp_timer_get_time();
                                        
                                        if (xSemaphoreTake(s_ctx->data_mutex, 
                                                          pdMS_TO_TICKS(100)) == pdTRUE) {
                                            memcpy(&s_ctx->latest_data, &agx_data, 
                                                   sizeof(ts_compute_data_t));
                                            xSemaphoreGive(s_ctx->data_mutex);
                                        }
                                        
                                        s_ctx->messages_received++;
                                        s_ctx->last_message_time = esp_timer_get_time();
                                        
                                        /* 更新温度源 */
                                        update_temp_source(&agx_data);
                                        
                                        /* 发布事件 */
                                        publish_data_event(&agx_data);
                                        
                                        ESP_LOGD(TAG, "AGX data: CPU=%.1f°C, RAM=%lu/%luMB",
                                                 agx_data.temperature.cpu,
                                                 agx_data.memory.ram.used_mb,
                                                 agx_data.memory.ram.total_mb);
                                    } else {
                                        s_ctx->parse_errors++;
                                    }
                                }
                                cJSON_Delete(array);
                            }
                        }
                    }
                    free(msg);
                }
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            set_error("WebSocket error");
            set_status(TS_COMPUTE_STATUS_ERROR);
            break;
            
        default:
            break;
    }
}

/**
 * @brief 解析 tegrastats 数据
 */
static esp_err_t parse_tegrastats_data(const char *json_str, ts_compute_data_t *data)
{
    if (json_str == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse tegrastats JSON");
        return ESP_FAIL;
    }
    
    /* 时间戳 */
    cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
    if (timestamp != NULL && cJSON_IsString(timestamp)) {
        strncpy(data->timestamp, timestamp->valuestring, 
                sizeof(data->timestamp) - 1);
    }
    
    /* CPU 数据 */
    cJSON *cpu = cJSON_GetObjectItem(root, "cpu");
    if (cpu != NULL && cJSON_IsArray(cpu)) {
        data->cpu.core_count = cJSON_GetArraySize(cpu);
        if (data->cpu.core_count > TS_COMPUTE_MAX_CPU_CORES) {
            data->cpu.core_count = TS_COMPUTE_MAX_CPU_CORES;
        }
        
        for (int i = 0; i < data->cpu.core_count; i++) {
            cJSON *core = cJSON_GetArrayItem(cpu, i);
            if (core != NULL && cJSON_IsObject(core)) {
                cJSON *usage = cJSON_GetObjectItem(core, "usage");
                cJSON *freq = cJSON_GetObjectItem(core, "freq");
                
                data->cpu.cores[i].id = i;
                if (usage != NULL) {
                    data->cpu.cores[i].usage = (uint8_t)cJSON_GetNumberValue(usage);
                }
                if (freq != NULL) {
                    data->cpu.cores[i].freq_mhz = (uint16_t)cJSON_GetNumberValue(freq);
                }
            }
        }
    }
    
    /* 内存数据 */
    cJSON *ram = cJSON_GetObjectItem(root, "ram");
    if (ram != NULL && cJSON_IsObject(ram)) {
        cJSON *used = cJSON_GetObjectItem(ram, "used");
        cJSON *total = cJSON_GetObjectItem(ram, "total");
        if (used != NULL) data->memory.ram.used_mb = (uint32_t)cJSON_GetNumberValue(used);
        if (total != NULL) data->memory.ram.total_mb = (uint32_t)cJSON_GetNumberValue(total);
    }
    
    cJSON *swap = cJSON_GetObjectItem(root, "swap");
    if (swap != NULL && cJSON_IsObject(swap)) {
        cJSON *used = cJSON_GetObjectItem(swap, "used");
        cJSON *total = cJSON_GetObjectItem(swap, "total");
        if (used != NULL) data->memory.swap.used_mb = (uint32_t)cJSON_GetNumberValue(used);
        if (total != NULL) data->memory.swap.total_mb = (uint32_t)cJSON_GetNumberValue(total);
    }
    
    /* 温度数据 */
    cJSON *temp = cJSON_GetObjectItem(root, "temperature");
    if (temp != NULL && cJSON_IsObject(temp)) {
        cJSON *cpu_temp = cJSON_GetObjectItem(temp, "cpu");
        cJSON *soc0 = cJSON_GetObjectItem(temp, "soc0");
        cJSON *soc1 = cJSON_GetObjectItem(temp, "soc1");
        cJSON *soc2 = cJSON_GetObjectItem(temp, "soc2");
        cJSON *tj = cJSON_GetObjectItem(temp, "tj");
        
        if (cpu_temp != NULL) data->temperature.cpu = (float)cJSON_GetNumberValue(cpu_temp);
        if (soc0 != NULL) data->temperature.soc0 = (float)cJSON_GetNumberValue(soc0);
        if (soc1 != NULL) data->temperature.soc1 = (float)cJSON_GetNumberValue(soc1);
        if (soc2 != NULL) data->temperature.soc2 = (float)cJSON_GetNumberValue(soc2);
        if (tj != NULL) data->temperature.tj = (float)cJSON_GetNumberValue(tj);
    }
    
    /* 功耗数据 */
    cJSON *power = cJSON_GetObjectItem(root, "power");
    if (power != NULL && cJSON_IsObject(power)) {
        cJSON *gpu_soc = cJSON_GetObjectItem(power, "GPU_SOC");
        cJSON *cpu_cv = cJSON_GetObjectItem(power, "CPU_CV");
        cJSON *sys_5v = cJSON_GetObjectItem(power, "SYS_5V");
        
        if (gpu_soc != NULL && cJSON_IsObject(gpu_soc)) {
            cJSON *cur = cJSON_GetObjectItem(gpu_soc, "current");
            cJSON *avg = cJSON_GetObjectItem(gpu_soc, "average");
            if (cur != NULL) data->power.gpu_soc.current_mw = (uint32_t)cJSON_GetNumberValue(cur);
            if (avg != NULL) data->power.gpu_soc.average_mw = (uint32_t)cJSON_GetNumberValue(avg);
        }
        
        if (cpu_cv != NULL && cJSON_IsObject(cpu_cv)) {
            cJSON *cur = cJSON_GetObjectItem(cpu_cv, "current");
            cJSON *avg = cJSON_GetObjectItem(cpu_cv, "average");
            if (cur != NULL) data->power.cpu_cv.current_mw = (uint32_t)cJSON_GetNumberValue(cur);
            if (avg != NULL) data->power.cpu_cv.average_mw = (uint32_t)cJSON_GetNumberValue(avg);
        }
        
        if (sys_5v != NULL && cJSON_IsObject(sys_5v)) {
            cJSON *cur = cJSON_GetObjectItem(sys_5v, "current");
            cJSON *avg = cJSON_GetObjectItem(sys_5v, "average");
            if (cur != NULL) data->power.sys_5v.current_mw = (uint32_t)cJSON_GetNumberValue(cur);
            if (avg != NULL) data->power.sys_5v.average_mw = (uint32_t)cJSON_GetNumberValue(avg);
        }
    }
    
    /* GPU 数据 */
    cJSON *gpu = cJSON_GetObjectItem(root, "gpu");
    if (gpu != NULL && cJSON_IsObject(gpu)) {
        cJSON *gr3d_freq = cJSON_GetObjectItem(gpu, "gr3d_freq");
        if (gr3d_freq != NULL) {
            data->gpu.gr3d_freq_pct = (uint8_t)cJSON_GetNumberValue(gr3d_freq);
        }
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 算力监控主任务
 */
static void compute_monitor_task(void *arg)
{
    compute_monitor_ctx_t *ctx = arg;
    xSemaphoreTake(ctx->data_mutex, portMAX_DELAY);
    xSemaphoreGive(ctx->data_mutex);
    ESP_LOGI(TAG, "Monitor task started");
    
    uint32_t reconnect_delay = ctx->config.reconnect_interval_ms;
    
    while (!ctx->should_stop) {
        /* Socket.IO 握手 */
        set_status(TS_COMPUTE_STATUS_CONNECTING);
        
        esp_err_t handshake = socketio_handshake();
        if (ctx->should_stop) break;
        if (handshake != ESP_OK) {
            ESP_LOGW(TAG, "Handshake failed, retry in %lu ms", reconnect_delay);
            set_status(TS_COMPUTE_STATUS_RECONNECTING);
            ctx->total_reconnects++;
            
            /* 指数退避 */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(reconnect_delay));
            reconnect_delay = (reconnect_delay * 2 > MAX_RECONNECT_DELAY_MS) 
                            ? MAX_RECONNECT_DELAY_MS : reconnect_delay * 2;
            continue;
        }
        
        /* 重置重连延迟 */
        reconnect_delay = ctx->config.reconnect_interval_ms;
        
        /* 建立 WebSocket 连接 */
        char ws_url[256];
        snprintf(ws_url, sizeof(ws_url),
                 "ws://%s:%d/socket.io/?EIO=4&transport=websocket&sid=%s",
                 ctx->config.server_ip, ctx->config.server_port,
                 ctx->session_id);
        
        ESP_LOGI(TAG, "Connecting WebSocket: %s", ws_url);
        
        esp_websocket_client_config_t ws_cfg = {
            .uri = ws_url,
            .buffer_size = 4096,
            .reconnect_timeout_ms = 10000,
            .network_timeout_ms = 10000,
            .ping_interval_sec = 0,  /* 禁用 WebSocket ping，使用 Socket.IO ping */
        };
        
        ctx->ws_client = esp_websocket_client_init(&ws_cfg);
        if (ctx->ws_client == NULL) {
            set_error("WebSocket init failed");
            set_status(TS_COMPUTE_STATUS_RECONNECTING);
            ctx->total_reconnects++;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(reconnect_delay));
            continue;
        }
        
        esp_websocket_register_events(ctx->ws_client,
                                      WEBSOCKET_EVENT_ANY,
                                      websocket_event_handler, 
                                      NULL);
        
        if (esp_websocket_client_start(ctx->ws_client) != ESP_OK) {
            set_error("WebSocket start failed");
            esp_websocket_client_destroy(ctx->ws_client);
            ctx->ws_client = NULL;
            set_status(TS_COMPUTE_STATUS_RECONNECTING);
            ctx->total_reconnects++;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(reconnect_delay));
            continue;
        }
        
        /* 等待连接建立（最多 5 秒） */
        int wait_count = 50;
        while (!ctx->ws_connected && wait_count > 0 && !ctx->should_stop) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count--;
        }
        
        if (!ctx->ws_connected && !ctx->should_stop) {
            ESP_LOGW(TAG, "WebSocket connection timeout");
            esp_websocket_client_stop(ctx->ws_client);
            esp_websocket_client_destroy(ctx->ws_client);
            ctx->ws_client = NULL;
            set_status(TS_COMPUTE_STATUS_RECONNECTING);
            ctx->total_reconnects++;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(reconnect_delay));
            continue;
        }
        
        /* 主循环：使用我们自己维护的 ws_connected 标志，而非 esp_websocket_client_is_connected() */
        while (!ctx->should_stop && ctx->ws_connected) {
            vTaskDelay(pdMS_TO_TICKS(100));
            
            if (ctx->should_stop) break;
            /* 检查心跳超时 */
            if (ctx->upgrade_complete) {
                uint64_t now = esp_timer_get_time();
                uint64_t age = now - ctx->last_message_time;
                if (ctx->last_message_time > 0 &&
                    age > (uint64_t)ctx->config.heartbeat_timeout_ms * 1000) {
                    ESP_LOGW(TAG, "Heartbeat timeout (%llu ms), reconnecting...", age / 1000);
                    break;
                }
            }
        }
        
        /* 清理 WebSocket */
        esp_websocket_client_stop(ctx->ws_client);
        esp_websocket_client_destroy(ctx->ws_client);
        ctx->ws_client = NULL;
        ctx->ws_connected = false;
        ctx->upgrade_complete = false;
        
        if (!ctx->should_stop) {
            set_status(TS_COMPUTE_STATUS_RECONNECTING);
            ctx->total_reconnects++;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(reconnect_delay));
        }
    }
    
    ESP_LOGI(TAG, "Monitor task exiting");
    xSemaphoreTake(ctx->data_mutex, portMAX_DELAY);
    xSemaphoreGive(ctx->exit_done);
    xSemaphoreGive(ctx->data_mutex); /* Last context access. */
    vTaskDelete(NULL);
}
