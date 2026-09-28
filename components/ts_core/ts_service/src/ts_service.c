/**
 * @file ts_service.c
 * @brief TianShanOS Service Management Implementation
 *
 * 服务管理系统实现
 *
 * @author TianShanOS Team
 * @version 0.1.0
 */

#include <string.h>
#include <stdlib.h>
#include "ts_service.h"
#include "ts_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

/* PSRAM-first allocation for reduced DRAM fragmentation */
#define TS_SVC_CALLOC(n, size) ({ void *p = heap_caps_calloc((n), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); p ? p : calloc((n), (size)); })
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ts_service";

/* ============================================================================
 * 私有类型定义
 * ========================================================================== */

/**
 * @brief 服务实例结构
 */
typedef struct ts_service_handle {
    ts_service_def_t def;                           /**< 服务定义副本 */
    ts_service_state_t state;                       /**< 当前状态 */
    uint32_t start_time_ms;                         /**< 启动时间戳 */
    uint32_t start_duration_ms;                     /**< 启动耗时 */
    uint32_t last_health_check_ms;                  /**< 上次健康检查时间 */
    bool healthy;                                   /**< 健康状态 */
    void *api;                                      /**< 服务 API */
    SemaphoreHandle_t state_sem;                    /**< 状态变化信号量 */
    struct ts_service_handle *next;                 /**< 链表下一个 */
} ts_service_instance_t;

/**
 * @brief 服务管理器上下文
 */
typedef struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    ts_service_instance_t *services;                /**< 服务链表 */
    size_t service_count;                           /**< 服务数量 */
    ts_service_stats_t stats;                       /**< 统计信息 */
    ts_service_phase_t current_phase;               /**< 当前启动阶段 */
    bool startup_complete;                          /**< 启动是否完成 */
} ts_service_context_t;

/* ============================================================================
 * 私有变量
 * ========================================================================== */

static ts_service_context_t s_svc_ctx = {0};

/* ============================================================================
 * 私有函数声明
 * ========================================================================== */

static ts_service_instance_t *find_service_by_name(const char *name);
static esp_err_t start_service_internal(ts_service_instance_t *service);
static esp_err_t stop_service_internal(ts_service_instance_t *service);
static void set_service_state(ts_service_instance_t *service, ts_service_state_t state);
static bool check_dependencies(ts_service_instance_t *service);
static void notify_state_change(ts_service_instance_t *service, 
                                 ts_service_state_t old_state,
                                 ts_service_state_t new_state);
// TODO: 实现按依赖关系排序服务的功能
// static int compare_services(const void *a, const void *b);

/* ============================================================================
 * 阶段和状态名称
 * ========================================================================== */

static const char *s_phase_names[] = {
    "PLATFORM",
    "CORE",
    "HAL",
    "DRIVER",
    "NETWORK",
    "SECURITY",
    "SERVICE",
    "UI"
};

static const char *s_state_names[] = {
    "UNREGISTERED",
    "REGISTERED",
    "STARTING",
    "RUNNING",
    "STOPPING",
    "STOPPED",
    "ERROR"
};

/* ============================================================================
 * 初始化和反初始化
 * ========================================================================== */

esp_err_t ts_service_init(void)
{
    if (s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing TianShanOS Service Management...");

    // 创建互斥锁
    s_svc_ctx.mutex = xSemaphoreCreateMutex();
    if (s_svc_ctx.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_svc_ctx.services = NULL;
    s_svc_ctx.service_count = 0;
    s_svc_ctx.current_phase = TS_SERVICE_PHASE_PLATFORM;
    s_svc_ctx.startup_complete = false;

    memset(&s_svc_ctx.stats, 0, sizeof(ts_service_stats_t));

    s_svc_ctx.initialized = true;
    ESP_LOGI(TAG, "Service management initialized");

    return ESP_OK;
}

esp_err_t ts_service_deinit(void)
{
    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing service management...");

    // 停止所有服务
    esp_err_t ret = ts_service_stop_all();
    if (ret != ESP_OK) return ret;

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

    // 释放所有服务
    ts_service_instance_t *service = s_svc_ctx.services;
    while (service != NULL) {
        ts_service_instance_t *next = service->next;
        if (service->state_sem != NULL) {
            vSemaphoreDelete(service->state_sem);
        }
        free(service);
        service = next;
    }
    s_svc_ctx.services = NULL;
    s_svc_ctx.service_count = 0;

    xSemaphoreGive(s_svc_ctx.mutex);

    vSemaphoreDelete(s_svc_ctx.mutex);
    s_svc_ctx.mutex = NULL;

    s_svc_ctx.initialized = false;
    ESP_LOGI(TAG, "Service management deinitialized");

    return ESP_OK;
}

bool ts_service_is_initialized(void)
{
    return s_svc_ctx.initialized;
}

/* ============================================================================
 * 服务注册
 * ========================================================================== */

esp_err_t ts_service_register(const ts_service_def_t *def, ts_service_handle_t *handle)
{
    if (def == NULL || def->name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_svc_ctx.service_count >= TS_SERVICE_MAX_COUNT) {
        ESP_LOGE(TAG, "Maximum services reached");
        return ESP_ERR_NO_MEM;
    }

    // 检查是否已存在
    if (find_service_by_name(def->name) != NULL) {
        ESP_LOGE(TAG, "Service '%s' already registered", def->name);
        return ESP_ERR_INVALID_STATE;
    }

    // 创建服务实例 (PSRAM 优先，减少 DRAM 碎片)
    ts_service_instance_t *service = TS_SVC_CALLOC(1, sizeof(ts_service_instance_t));
    if (service == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // 复制服务定义
    memcpy(&service->def, def, sizeof(ts_service_def_t));

    service->state = TS_SERVICE_STATE_REGISTERED;
    service->healthy = true;
    service->api = NULL;

    // 创建状态变化信号量
    service->state_sem = xSemaphoreCreateBinary();
    if (service->state_sem == NULL) {
        free(service);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

    // 添加到链表
    service->next = s_svc_ctx.services;
    s_svc_ctx.services = service;
    s_svc_ctx.service_count++;
    s_svc_ctx.stats.total_services = s_svc_ctx.service_count;

    xSemaphoreGive(s_svc_ctx.mutex);

    if (handle != NULL) {
        *handle = service;
    }

    ESP_LOGI(TAG, "Registered service: %s (phase=%s)", 
             def->name, ts_service_phase_to_string(def->phase));

    return ESP_OK;
}

esp_err_t ts_service_unregister(ts_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ts_service_instance_t *target = (ts_service_instance_t *)handle;

    // 必须先停止服务
    if (target->state == TS_SERVICE_STATE_RUNNING) {
        esp_err_t ret = ts_service_stop(handle);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

    ts_service_instance_t *prev = NULL;
    ts_service_instance_t *service = s_svc_ctx.services;

    while (service != NULL) {
        if (service == target) {
            if (prev == NULL) {
                s_svc_ctx.services = service->next;
            } else {
                prev->next = service->next;
            }
            s_svc_ctx.service_count--;
            s_svc_ctx.stats.total_services = s_svc_ctx.service_count;

            xSemaphoreGive(s_svc_ctx.mutex);

            ESP_LOGI(TAG, "Unregistered service: %s", service->def.name);

            if (service->state_sem != NULL) {
                vSemaphoreDelete(service->state_sem);
            }
            free(service);
            return ESP_OK;
        }
        prev = service;
        service = service->next;
    }

    xSemaphoreGive(s_svc_ctx.mutex);
    return ESP_ERR_NOT_FOUND;
}

ts_service_handle_t ts_service_find(const char *name)
{
    if (name == NULL || !s_svc_ctx.initialized) {
        return NULL;
    }

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);
    ts_service_instance_t *service = find_service_by_name(name);
    xSemaphoreGive(s_svc_ctx.mutex);

    return service;
}

bool ts_service_exists(const char *name)
{
    return ts_service_find(name) != NULL;
}

/* ============================================================================
 * 服务生命周期
 * ========================================================================== */

esp_err_t ts_service_start_all(void)
{
    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting all services...");

    uint32_t total_start_time = (uint32_t)(esp_timer_get_time() / 1000);

    // 按阶段启动
    for (int phase = 0; phase < TS_SERVICE_PHASE_MAX; phase++) {
        s_svc_ctx.current_phase = (ts_service_phase_t)phase;
        
        ESP_LOGI(TAG, "=== Phase %d: %s ===", phase, ts_service_phase_to_string(phase));
        
        uint32_t phase_start = (uint32_t)(esp_timer_get_time() / 1000);
        
        esp_err_t ret = ts_service_start_phase((ts_service_phase_t)phase);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start phase %s", ts_service_phase_to_string(phase));
            return ret;
        }

        uint32_t phase_duration = (uint32_t)(esp_timer_get_time() / 1000) - phase_start;
        s_svc_ctx.stats.phase_times_ms[phase] = phase_duration;

        // 发送阶段完成事件
        int32_t phase_id = phase;
        ts_event_post(TS_EVENT_BASE_SERVICE, TS_EVENT_SERVICE_PHASE_COMPLETE,
                      &phase_id, sizeof(phase_id), 100);
    }

    s_svc_ctx.stats.startup_time_ms = (uint32_t)(esp_timer_get_time() / 1000) - total_start_time;
    s_svc_ctx.startup_complete = true;

    ESP_LOGI(TAG, "All services started in %lu ms", 
             (unsigned long)s_svc_ctx.stats.startup_time_ms);

    // 发送所有服务启动完成事件
    ts_event_post(TS_EVENT_BASE_SERVICE, TS_EVENT_SERVICE_ALL_STARTED, NULL, 0, 100);

    return ESP_OK;
}

esp_err_t ts_service_stop_all(void)
{
    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping all services...");

    // 按相反顺序停止（从 UI 到 PLATFORM）
    for (int phase = TS_SERVICE_PHASE_MAX - 1; phase >= 0; phase--) {
        ESP_LOGI(TAG, "Stopping phase: %s", ts_service_phase_to_string(phase));

        xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

        ts_service_instance_t *service = s_svc_ctx.services;
        while (service != NULL) {
            if (service->def.phase == phase && 
                service->state == TS_SERVICE_STATE_RUNNING) {
                xSemaphoreGive(s_svc_ctx.mutex);
                esp_err_t ret = ts_service_stop(service);
                if (ret != ESP_OK) return ret; /* keep dependencies and registry for retry */
                xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);
            }
            service = service->next;
        }

        xSemaphoreGive(s_svc_ctx.mutex);
    }

    s_svc_ctx.startup_complete = false;
    ESP_LOGI(TAG, "All services stopped");

    return ESP_OK;
}

esp_err_t ts_service_start_phase(ts_service_phase_t phase)
{
    if (phase >= TS_SERVICE_PHASE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 收集该阶段的所有服务
    ts_service_instance_t *phase_services[TS_SERVICE_MAX_COUNT];
    size_t phase_count = 0;

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

    ts_service_instance_t *service = s_svc_ctx.services;
    while (service != NULL && phase_count < TS_SERVICE_MAX_COUNT) {
        if (service->def.phase == phase && 
            service->state == TS_SERVICE_STATE_REGISTERED) {
            phase_services[phase_count++] = service;
        }
        service = service->next;
    }

    xSemaphoreGive(s_svc_ctx.mutex);

    if (phase_count == 0) {
        ESP_LOGD(TAG, "No services in phase %s", ts_service_phase_to_string(phase));
        return ESP_OK;
    }

    // TODO: 根据依赖关系排序服务
    // 目前按注册顺序启动

    ESP_LOGI(TAG, "Starting %zu services in phase %s", 
             phase_count, ts_service_phase_to_string(phase));

    // 启动服务
    for (size_t i = 0; i < phase_count; i++) {
        esp_err_t ret = start_service_internal(phase_services[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start service '%s': %s",
                     phase_services[i]->def.name, esp_err_to_name(ret));
            // 继续启动其他服务
        }
    }

    return ESP_OK;
}

esp_err_t ts_service_start(ts_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;
    return start_service_internal(service);
}

esp_err_t ts_service_stop(ts_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;
    return stop_service_internal(service);
}

esp_err_t ts_service_restart(ts_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;

    if (!(service->def.capabilities & TS_SERVICE_CAP_RESTARTABLE)) {
        ESP_LOGW(TAG, "Service '%s' is not restartable", service->def.name);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Restarting service: %s", service->def.name);

    esp_err_t ret = ts_service_stop(handle);
    if (ret != ESP_OK && (ret != ESP_ERR_INVALID_STATE ||
                         service->state == TS_SERVICE_STATE_RUNNING)) {
        return ret;
    }

    return ts_service_start(handle);
}

/* ============================================================================
 * 服务状态查询
 * ========================================================================== */

ts_service_state_t ts_service_get_state(ts_service_handle_t handle)
{
    if (handle == NULL) {
        return TS_SERVICE_STATE_UNREGISTERED;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;
    return service->state;
}

esp_err_t ts_service_get_info(ts_service_handle_t handle, ts_service_info_t *info)
{
    if (handle == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;

    strncpy(info->name, service->def.name, TS_SERVICE_NAME_MAX_LEN - 1);
    info->name[TS_SERVICE_NAME_MAX_LEN - 1] = '\0';
    info->phase = service->def.phase;
    info->state = service->state;
    info->capabilities = service->def.capabilities;
    info->start_time_ms = service->start_time_ms;
    info->start_duration_ms = service->start_duration_ms;
    info->last_health_check_ms = service->last_health_check_ms;
    info->healthy = service->healthy;

    return ESP_OK;
}

bool ts_service_is_running(ts_service_handle_t handle)
{
    if (handle == NULL) {
        return false;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;
    return service->state == TS_SERVICE_STATE_RUNNING;
}

bool ts_service_is_healthy(ts_service_handle_t handle)
{
    if (handle == NULL) {
        return false;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;

    // 如果有健康检查回调，执行它
    if (service->def.health_check != NULL && 
        service->state == TS_SERVICE_STATE_RUNNING) {
        service->healthy = service->def.health_check(handle, service->def.user_data);
        service->last_health_check_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }

    return service->healthy;
}

esp_err_t ts_service_wait_state(ts_service_handle_t handle,
                                 ts_service_state_t state,
                                 uint32_t timeout_ms)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;

    // 已经是目标状态
    if (service->state == state) {
        return ESP_OK;
    }

    // 等待状态变化
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);

    while (service->state != state) {
        if (xSemaphoreTake(service->state_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (service->state == state) {
                return ESP_OK;
            }
        }

        uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000) - start;
        if (timeout_ms != portMAX_DELAY && elapsed >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
    }

    return ESP_OK;
}

esp_err_t ts_service_wait_all_started(uint32_t timeout_ms)
{
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);

    while (!s_svc_ctx.startup_complete) {
        vTaskDelay(pdMS_TO_TICKS(100));

        uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000) - start;
        if (timeout_ms != portMAX_DELAY && elapsed >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
    }

    return ESP_OK;
}

/* ============================================================================
 * 服务 API 访问
 * ========================================================================== */

void *ts_service_get_api(ts_service_handle_t handle)
{
    if (handle == NULL) {
        return NULL;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;
    return service->api;
}

esp_err_t ts_service_set_api(ts_service_handle_t handle, void *api)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;
    service->api = api;

    return ESP_OK;
}

/* ============================================================================
 * 服务枚举
 * ========================================================================== */

size_t ts_service_enumerate(ts_service_enum_fn callback, void *user_data)
{
    if (callback == NULL || !s_svc_ctx.initialized) {
        return 0;
    }

    size_t count = 0;
    ts_service_info_t info;

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

    ts_service_instance_t *service = s_svc_ctx.services;
    while (service != NULL) {
        ts_service_get_info(service, &info);
        
        xSemaphoreGive(s_svc_ctx.mutex);
        
        if (!callback(service, &info, user_data)) {
            return count;
        }
        count++;

        xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);
        service = service->next;
    }

    xSemaphoreGive(s_svc_ctx.mutex);

    return count;
}

size_t ts_service_enumerate_phase(ts_service_phase_t phase,
                                   ts_service_enum_fn callback,
                                   void *user_data)
{
    if (callback == NULL || phase >= TS_SERVICE_PHASE_MAX || !s_svc_ctx.initialized) {
        return 0;
    }

    size_t count = 0;
    ts_service_info_t info;

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

    ts_service_instance_t *service = s_svc_ctx.services;
    while (service != NULL) {
        if (service->def.phase == phase) {
            ts_service_get_info(service, &info);
            
            xSemaphoreGive(s_svc_ctx.mutex);
            
            if (!callback(service, &info, user_data)) {
                return count;
            }
            count++;

            xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);
        }
        service = service->next;
    }

    xSemaphoreGive(s_svc_ctx.mutex);

    return count;
}

/* ============================================================================
 * 统计和调试
 * ========================================================================== */

esp_err_t ts_service_get_stats(ts_service_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 更新统计
    s_svc_ctx.stats.running_services = 0;
    s_svc_ctx.stats.stopped_services = 0;
    s_svc_ctx.stats.error_services = 0;

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

    ts_service_instance_t *service = s_svc_ctx.services;
    while (service != NULL) {
        switch (service->state) {
            case TS_SERVICE_STATE_RUNNING:
                s_svc_ctx.stats.running_services++;
                break;
            case TS_SERVICE_STATE_STOPPED:
            case TS_SERVICE_STATE_REGISTERED:
                s_svc_ctx.stats.stopped_services++;
                break;
            case TS_SERVICE_STATE_ERROR:
                s_svc_ctx.stats.error_services++;
                break;
            default:
                break;
        }
        service = service->next;
    }

    xSemaphoreGive(s_svc_ctx.mutex);

    memcpy(stats, &s_svc_ctx.stats, sizeof(ts_service_stats_t));
    return ESP_OK;
}

void ts_service_dump(void)
{
    ts_service_stats_t stats;
    if (ts_service_get_stats(&stats) != ESP_OK) {
        memset(&stats, 0, sizeof(stats));
    }

    ESP_LOGI(TAG, "=== Service Status ===");
    ESP_LOGI(TAG, "Total: %lu, Running: %lu, Stopped: %lu, Error: %lu",
             (unsigned long)stats.total_services,
             (unsigned long)stats.running_services,
             (unsigned long)stats.stopped_services,
             (unsigned long)stats.error_services);
    ESP_LOGI(TAG, "Startup time: %lu ms", (unsigned long)stats.startup_time_ms);

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

    ts_service_instance_t *service = s_svc_ctx.services;
    while (service != NULL) {
        ESP_LOGI(TAG, "  [%s] %s - %s (%lu ms)",
                 ts_service_phase_to_string(service->def.phase),
                 service->def.name,
                 ts_service_state_to_string(service->state),
                 (unsigned long)service->start_duration_ms);
        service = service->next;
    }

    xSemaphoreGive(s_svc_ctx.mutex);

    ESP_LOGI(TAG, "======================");
}

const char *ts_service_phase_to_string(ts_service_phase_t phase)
{
    if (phase >= TS_SERVICE_PHASE_MAX) {
        return "UNKNOWN";
    }
    return s_phase_names[phase];
}

const char *ts_service_state_to_string(ts_service_state_t state)
{
    if (state >= TS_SERVICE_STATE_MAX) {
        return "UNKNOWN";
    }
    return s_state_names[state];
}

/* ============================================================================
 * 私有函数实现
 * ========================================================================== */

static ts_service_instance_t *find_service_by_name(const char *name)
{
    ts_service_instance_t *service = s_svc_ctx.services;
    while (service != NULL) {
        if (strcmp(service->def.name, name) == 0) {
            return service;
        }
        service = service->next;
    }
    return NULL;
}

static esp_err_t start_service_internal(ts_service_instance_t *service)
{
    if (service == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 已经在运行
    if (service->state == TS_SERVICE_STATE_RUNNING) {
        return ESP_OK;
    }

    // 检查依赖
    if (!check_dependencies(service)) {
        ESP_LOGE(TAG, "Dependencies not met for service '%s'", service->def.name);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting service: %s", service->def.name);

    ts_service_state_t old_state = service->state;
    set_service_state(service, TS_SERVICE_STATE_STARTING);

    uint32_t start_time = (uint32_t)(esp_timer_get_time() / 1000);
    service->start_time_ms = start_time;

    esp_err_t ret = ESP_OK;

    // 调用初始化回调
    if (service->def.init != NULL) {
        ret = service->def.init(service, service->def.user_data);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Service '%s' init failed: %s", 
                     service->def.name, esp_err_to_name(ret));
            set_service_state(service, TS_SERVICE_STATE_ERROR);
            return ret;
        }
    }

    // 调用启动回调
    if (service->def.start != NULL) {
        ret = service->def.start(service, service->def.user_data);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Service '%s' start failed: %s", 
                     service->def.name, esp_err_to_name(ret));
            set_service_state(service, TS_SERVICE_STATE_ERROR);
            return ret;
        }
    }

    service->start_duration_ms = (uint32_t)(esp_timer_get_time() / 1000) - start_time;
    set_service_state(service, TS_SERVICE_STATE_RUNNING);

    ESP_LOGI(TAG, "Service '%s' started in %lu ms", 
             service->def.name, (unsigned long)service->start_duration_ms);

    // 发送服务启动事件
    ts_service_event_data_t event_data = {
        .service_name = service->def.name,
        .old_state = old_state,
        .new_state = TS_SERVICE_STATE_RUNNING,
        .error_code = ESP_OK,
    };
    ts_event_post(TS_EVENT_BASE_SERVICE, TS_EVENT_SERVICE_STARTED,
                  &event_data, sizeof(event_data), 100);

    return ESP_OK;
}

static esp_err_t stop_service_internal(ts_service_instance_t *service)
{
    if (service == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 已经停止
    if (service->state == TS_SERVICE_STATE_STOPPED ||
        service->state == TS_SERVICE_STATE_REGISTERED) {
        return ESP_OK;
    }

    // 不能停止非运行状态的服务
    if (service->state != TS_SERVICE_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping service: %s", service->def.name);

    ts_service_state_t old_state = service->state;
    set_service_state(service, TS_SERVICE_STATE_STOPPING);

    esp_err_t ret = ESP_OK;

    // 调用停止回调
    if (service->def.stop != NULL) {
        ret = service->def.stop(service, service->def.user_data);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Service '%s' stop returned error: %s", 
                     service->def.name, esp_err_to_name(ret));
            /* Failed stop retains the service and its resources for a later retry. */
            set_service_state(service, old_state);
            return ret;
        }
    }

    set_service_state(service, TS_SERVICE_STATE_STOPPED);

    ESP_LOGI(TAG, "Service '%s' stopped", service->def.name);

    // 发送服务停止事件
    ts_service_event_data_t event_data = {
        .service_name = service->def.name,
        .old_state = old_state,
        .new_state = TS_SERVICE_STATE_STOPPED,
        .error_code = ret,
    };
    ts_event_post(TS_EVENT_BASE_SERVICE, TS_EVENT_SERVICE_STOPPED,
                  &event_data, sizeof(event_data), 100);

    return ESP_OK;
}

static void set_service_state(ts_service_instance_t *service, ts_service_state_t state)
{
    ts_service_state_t old_state = service->state;
    service->state = state;

    // 通知等待者
    xSemaphoreGive(service->state_sem);

    // 发送状态变化事件
    if (old_state != state) {
        notify_state_change(service, old_state, state);
    }
}

static bool check_dependencies(ts_service_instance_t *service)
{
    for (int i = 0; i < TS_SERVICE_DEPS_MAX; i++) {
        const char *dep_name = service->def.dependencies[i];
        if (dep_name == NULL) {
            break;
        }

        ts_service_instance_t *dep = find_service_by_name(dep_name);
        if (dep == NULL) {
            ESP_LOGW(TAG, "Dependency '%s' not found for service '%s'",
                     dep_name, service->def.name);
            return false;
        }

        if (dep->state != TS_SERVICE_STATE_RUNNING) {
            ESP_LOGW(TAG, "Dependency '%s' not running for service '%s'",
                     dep_name, service->def.name);
            return false;
        }
    }

    return true;
}

static void notify_state_change(ts_service_instance_t *service,
                                 ts_service_state_t old_state,
                                 ts_service_state_t new_state)
{
    ts_service_event_data_t event_data = {
        .service_name = service->def.name,
        .old_state = old_state,
        .new_state = new_state,
        .error_code = ESP_OK,
    };

    ts_event_post(TS_EVENT_BASE_SERVICE, TS_EVENT_SERVICE_STATE_CHANGED,
                  &event_data, sizeof(event_data), 0);
}
