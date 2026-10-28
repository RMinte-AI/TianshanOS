/**
 * @file ts_automation.c
 * @brief TianShanOS 自动化引擎 - 核心实现
 *
 * 自动化引擎主入口，负责：
 * - 初始化各子模块（变量存储、数据源、规则引擎）
 * - 主循环任务（数据采集、规则评估）
 * - 配置管理（加载、保存、热重载）
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_automation.h"
#include "ts_automation_types.h"
#include "ts_variable.h"
#include "ts_rule_engine.h"
#include "ts_source_manager.h"
#include "ts_action_manager.h"
#include "ts_storage.h"
#include "ts_automation_config.h"
#include "ts_event.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "ts_automation";

/*===========================================================================*/
/*                              内部状态                                      */
/*===========================================================================*/

/**
 * 引擎内部状态结构
 */
typedef struct {
    _Atomic(ts_automation_state_t) state;
    atomic_bool stopping, deinitializing;
    TaskHandle_t task_handle;            // 主任务句柄
    _Atomic(SemaphoreHandle_t) mutex;
    
    ts_event_handler_handle_t storage_events;
    
    int64_t start_time_ms;               // 启动时间
    uint32_t actions_executed;           // 执行的动作计数
    uint32_t rule_triggers;              // 规则触发计数
} ts_automation_ctx_t;

static ts_automation_ctx_t s_ctx = {
    .state = TS_AUTO_STATE_UNINITIALIZED,
    .task_handle = NULL,
    .mutex = NULL,
};
static StaticSemaphore_t s_state_mutex_storage;

/*===========================================================================*/
/*                              前向声明                                      */
/*===========================================================================*/

static void automation_task(void *arg);
static void storage_changed(const ts_event_t *event,void *unused)
{
    (void)unused;
    if(event->id!=TS_EVT_STORAGE_SD_MOUNTED && event->id!=TS_EVT_STORAGE_SD_UNMOUNTED)return;
    ts_auto_config_error_t error={0};
    esp_err_t ret=ts_auto_config_storage_changed(&error);
    if(ret!=ESP_OK)ESP_LOGW(TAG,"Storage change: %s (%s)",error.reason?error.reason:"configuration_unavailable",esp_err_to_name(ret));
}

/*===========================================================================*/
/*                              版本信息                                      */
/*===========================================================================*/

static const char *s_version_str = NULL;

const char *ts_automation_get_version(void)
{
    if (!s_version_str) {
        static char ver[16];
        snprintf(ver, sizeof(ver), "%d.%d.%d",
                 TS_AUTOMATION_VERSION_MAJOR,
                 TS_AUTOMATION_VERSION_MINOR,
                 TS_AUTOMATION_VERSION_PATCH);
        s_version_str = ver;
    }
    return s_version_str;
}

/*===========================================================================*/
/*                              初始化/反初始化                               */
/*===========================================================================*/

esp_err_t ts_automation_init(const ts_automation_config_t *config)
{
    if (s_ctx.state != TS_AUTO_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing automation engine v%s", ts_automation_get_version());

    // 创建互斥锁
    /* This single lock outlives late API readers and service restarts. */
    if(!s_ctx.mutex)s_ctx.mutex=xSemaphoreCreateMutexStatic(&s_state_mutex_storage);

    // 初始化子模块
    esp_err_t ret;

    ret = ts_variable_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init variable storage: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = ts_source_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init source manager: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = ts_rule_engine_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init rule engine: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = ts_action_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init action manager: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // 注册 power policy 变量（如果 power policy 已初始化）
    extern esp_err_t ts_power_policy_register_variables(void);
    esp_err_t pp_ret = ts_power_policy_register_variables();
    if (pp_ret != ESP_OK && pp_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to register power policy variables: %s", esp_err_to_name(pp_ret));
    }

    // All SDK-owned definitions must exist before installing user definitions.
    ts_auto_config_error_t config_error={0};
    ret=ts_auto_config_startup(&config_error);
    if(ret!=ESP_OK)ESP_LOGW(TAG,"Configuration is unavailable; management remains available: %s (%s)",
        config_error.reason?config_error.reason:"configuration_unavailable",esp_err_to_name(ret));
    ret=ts_event_register(TS_EVENT_BASE_STORAGE,TS_EVENT_ANY_ID,storage_changed,NULL,&s_ctx.storage_events);
    if(ret!=ESP_OK)goto cleanup;
    s_ctx.state=TS_AUTO_STATE_INITIALIZED;
    s_ctx.start_time_ms=esp_timer_get_time()/1000;

    ESP_LOGI(TAG, "Automation engine initialized");

    // 自动启动
    if (!config || config->auto_start) {
        return ts_automation_start();
    }

    return ESP_OK;

cleanup:
    ts_auto_config_shutdown();
    ts_action_manager_deinit();
    ts_rule_engine_deinit();
    ts_source_manager_deinit();
    ts_variable_deinit();
    return ret;
}

esp_err_t ts_automation_deinit(void)
{
    if (s_ctx.state == TS_AUTO_STATE_UNINITIALIZED) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing automation engine");
    xSemaphoreTake(s_ctx.mutex,portMAX_DELAY);s_ctx.deinitializing=true;xSemaphoreGive(s_ctx.mutex);

    // 先停止
    esp_err_t ret = ts_automation_stop();
    if (ret != ESP_OK) return ret;
    if(s_ctx.storage_events){
        ret=ts_event_unregister_sync(s_ctx.storage_events,1000);
        if(ret!=ESP_OK && ret!=ESP_ERR_NOT_FOUND)return ret;
        s_ctx.storage_events=NULL;
    }
    ts_auto_config_shutdown();
    ret = ts_rule_engine_deinit();
    if (ret != ESP_OK) return ret;
    ret = ts_action_manager_deinit();
    if (ret != ESP_OK) return ret;
    ts_source_manager_deinit();
    ts_variable_deinit();

    xSemaphoreTake(s_ctx.mutex,portMAX_DELAY);
    s_ctx.state = TS_AUTO_STATE_UNINITIALIZED;s_ctx.deinitializing=false;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Automation engine deinitialized");
    return ESP_OK;
}

bool ts_automation_is_initialized(void)
{
    return !s_ctx.deinitializing && s_ctx.state != TS_AUTO_STATE_UNINITIALIZED;
}

/*===========================================================================*/
/*                              控制接口                                      */
/*===========================================================================*/

esp_err_t ts_automation_start(void) {
    if (!s_ctx.mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.stopping || s_ctx.deinitializing || s_ctx.state==TS_AUTO_STATE_UNINITIALIZED) {
        xSemaphoreGive(s_ctx.mutex); return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.state == TS_AUTO_STATE_RUNNING) { xSemaphoreGive(s_ctx.mutex); return ESP_OK; }
    if (s_ctx.task_handle || ts_action_manager_resume() != ESP_OK) {
        xSemaphoreGive(s_ctx.mutex); return ESP_ERR_INVALID_STATE;
    }
    ts_source_start_all();
    s_ctx.state = TS_AUTO_STATE_RUNNING;
    BaseType_t ret = xTaskCreatePinnedToCore(automation_task, "ts_auto",
        CONFIG_TS_AUTOMATION_TASK_STACK_SIZE, NULL, CONFIG_TS_AUTOMATION_TASK_PRIORITY,
        &s_ctx.task_handle, 1);
    if (ret != pdPASS) s_ctx.state = TS_AUTO_STATE_INITIALIZED;
    xSemaphoreGive(s_ctx.mutex);
    return ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t ts_automation_stop(void) {
    if (!s_ctx.mutex) return ESP_OK;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if(s_ctx.state==TS_AUTO_STATE_UNINITIALIZED){xSemaphoreGive(s_ctx.mutex);return ESP_OK;}
    s_ctx.stopping = true;
    xSemaphoreGive(s_ctx.mutex);
    /* No external vTaskDelete: evaluator returns through rule lease cleanup.
     * Async actions may outlive it; do not reopen admission until both are idle. */
    bool exited = false;
    esp_err_t idle = ESP_ERR_TIMEOUT;
    for (unsigned waited = 0; waited <= 200; waited += 10) {
        idle = ts_action_manager_quiesce();
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        exited = s_ctx.task_handle == NULL;
        xSemaphoreGive(s_ctx.mutex);
        if (exited && idle == ESP_OK) break;
        if (waited < 200) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!exited || idle != ESP_OK) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    ts_source_stop_all();
    s_ctx.state = TS_AUTO_STATE_INITIALIZED;
    s_ctx.stopping = false;
    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t ts_automation_pause(void)
{
    if(!s_ctx.mutex)return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex,portMAX_DELAY);
    if (s_ctx.stopping || s_ctx.deinitializing || s_ctx.state != TS_AUTO_STATE_RUNNING) {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.state = TS_AUTO_STATE_PAUSED;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Automation engine paused");
    return ESP_OK;
}

esp_err_t ts_automation_resume(void)
{
    if(!s_ctx.mutex)return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex,portMAX_DELAY);
    if (s_ctx.stopping || s_ctx.deinitializing || s_ctx.state != TS_AUTO_STATE_PAUSED) {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.state = TS_AUTO_STATE_RUNNING;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Automation engine resumed");
    return ESP_OK;
}

/*===========================================================================*/
/*                              状态查询                                      */
/*===========================================================================*/

esp_err_t ts_automation_get_status(ts_automation_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    if(!s_ctx.mutex)return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if(s_ctx.deinitializing || s_ctx.state==TS_AUTO_STATE_UNINITIALIZED){
        xSemaphoreGive(s_ctx.mutex);return ESP_ERR_INVALID_STATE;
    }
    
    status->state = s_ctx.state;
    ts_auto_config_status_t config;ts_auto_config_status(&config);
    status->config_path = config.source==TS_AUTO_CONFIG_SD?"/sdcard/config/automation/state.json":"/autocfg/state.json";
    status->actions_executed = s_ctx.actions_executed;
    status->rule_triggers = s_ctx.rule_triggers;
    
    if (s_ctx.state >= TS_AUTO_STATE_INITIALIZED) {
        status->uptime_ms = (uint32_t)((esp_timer_get_time() / 1000) - s_ctx.start_time_ms);
    }

    // 从子模块获取计数
    status->sources_count = ts_source_count();
    status->rules_count = ts_rule_count();
    status->variables_count = ts_variable_count();
    xSemaphoreGive(s_ctx.mutex);

    // TODO: 统计活跃源和规则

    return ESP_OK;
}

/*===========================================================================*/
/*                              主任务                                        */
/*===========================================================================*/

/**
 * 自动化引擎主任务
 * 
 * 职责：
 * 1. 轮询数据源获取新数据
 * 2. 更新变量值
 * 3. 评估规则并触发动作
 */
static void automation_task(void *arg)
{
    ESP_LOGI(TAG, "Automation task started");

    const TickType_t poll_interval = pdMS_TO_TICKS(100);  // 100ms 轮询间隔

    while (1) {
        // 检查状态
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        ts_automation_state_t state = s_ctx.state;
        bool stopping = s_ctx.stopping;
        xSemaphoreGive(s_ctx.mutex);

        if (stopping || (state != TS_AUTO_STATE_RUNNING && state != TS_AUTO_STATE_PAUSED)) {
            break;  // 退出任务
        }

        if (state == TS_AUTO_STATE_RUNNING) {
            // 1. 轮询数据源
            int polled = ts_source_poll_all();
            if (polled > 0) {
                ESP_LOGD(TAG, "Polled %d sources", polled);
            }

            // 2. 评估规则
            int triggered = ts_rule_evaluate_all();
            if (triggered > 0) {
                ESP_LOGD(TAG, "Triggered %d rules", triggered);
                
                xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                s_ctx.rule_triggers += triggered;
                xSemaphoreGive(s_ctx.mutex);
            }
        }

        vTaskDelay(poll_interval);
    }

    ESP_LOGI(TAG, "Automation task exiting");
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.task_handle = NULL;
    xSemaphoreGive(s_ctx.mutex);
    vTaskDelete(NULL);
}

/*===========================================================================*/
/*                              配置管理                                      */
/*===========================================================================*/

esp_err_t ts_automation_reload(void)
{
    if(s_ctx.state==TS_AUTO_STATE_UNINITIALIZED)return ESP_ERR_INVALID_STATE;
    ts_auto_config_error_t error={0};return ts_auto_config_reload(&error);
}
