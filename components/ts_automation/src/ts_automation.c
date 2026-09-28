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
    ts_automation_state_t state;         // 当前状态
    atomic_bool stopping;
    TaskHandle_t task_handle;            // 主任务句柄
    SemaphoreHandle_t mutex;             // 状态互斥锁
    
    char config_path[128];               // 配置文件路径
    bool config_modified;                // 配置是否被修改
    
    int64_t start_time_ms;               // 启动时间
    uint32_t actions_executed;           // 执行的动作计数
    uint32_t rule_triggers;              // 规则触发计数
} ts_automation_ctx_t;

static ts_automation_ctx_t s_ctx = {
    .state = TS_AUTO_STATE_UNINITIALIZED,
    .task_handle = NULL,
    .mutex = NULL,
    .config_modified = false,
};

/*===========================================================================*/
/*                              前向声明                                      */
/*===========================================================================*/

static void automation_task(void *arg);
static esp_err_t load_config(const char *path);
static esp_err_t apply_default_config(void);

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
    s_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_ctx.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // 确定配置路径
    const char *cfg_path = CONFIG_TS_AUTOMATION_CONFIG_PATH;
    if (config && config->config_path) {
        cfg_path = config->config_path;
    }
    strncpy(s_ctx.config_path, cfg_path, sizeof(s_ctx.config_path) - 1);

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

    // 加载引擎配置（sources/rules/actions 已由各自模块从 SD 卡加载）
    ret = load_config(s_ctx.config_path);
    if (ret == ESP_ERR_NOT_FOUND) {
        /* automation.json 不存在是正常情况，使用默认引擎设置 */
        ESP_LOGD(TAG, "No automation.json found, using default engine settings");
        ret = apply_default_config();
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load automation.json: %s, using defaults", esp_err_to_name(ret));
        ret = apply_default_config();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply config: %s", esp_err_to_name(ret));
        // 继续运行，使用空配置
    }

    s_ctx.state = TS_AUTO_STATE_INITIALIZED;
    s_ctx.start_time_ms = esp_timer_get_time() / 1000;

    // 预创建 SSH 命令变量（从 NVS 配置读取）
    extern esp_err_t ts_ssh_commands_precreate_variables(void);
    esp_err_t precreate_ret = ts_ssh_commands_precreate_variables();
    if (precreate_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to precreate SSH command variables: %s", esp_err_to_name(precreate_ret));
        // 不影响启动
    }

    // 注册 power policy 变量（如果 power policy 已初始化）
    extern esp_err_t ts_power_policy_register_variables(void);
    esp_err_t pp_ret = ts_power_policy_register_variables();
    if (pp_ret != ESP_OK && pp_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to register power policy variables: %s", esp_err_to_name(pp_ret));
    }

    ESP_LOGI(TAG, "Automation engine initialized");

    // 自动启动
    if (!config || config->auto_start) {
        return ts_automation_start();
    }

    return ESP_OK;

cleanup:
    if (s_ctx.mutex) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }
    return ret;
}

esp_err_t ts_automation_deinit(void)
{
    if (s_ctx.state == TS_AUTO_STATE_UNINITIALIZED) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing automation engine");

    // 先停止
    esp_err_t ret = ts_automation_stop();
    if (ret != ESP_OK) return ret;
    ret = ts_action_manager_deinit();
    if (ret != ESP_OK) return ret;
    ret = ts_rule_engine_deinit();
    if (ret != ESP_OK) return ret;
    ts_source_manager_deinit();
    ts_variable_deinit();

    // 清理互斥锁
    if (s_ctx.mutex) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }

    s_ctx.state = TS_AUTO_STATE_UNINITIALIZED;

    ESP_LOGI(TAG, "Automation engine deinitialized");
    return ESP_OK;
}

bool ts_automation_is_initialized(void)
{
    return s_ctx.state != TS_AUTO_STATE_UNINITIALIZED;
}

/*===========================================================================*/
/*                              控制接口                                      */
/*===========================================================================*/

esp_err_t ts_automation_start(void) {
    if (!s_ctx.mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.stopping) { xSemaphoreGive(s_ctx.mutex); return ESP_ERR_INVALID_STATE; }
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
    if (s_ctx.stopping || s_ctx.state != TS_AUTO_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.state = TS_AUTO_STATE_PAUSED;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Automation engine paused");
    return ESP_OK;
}

esp_err_t ts_automation_resume(void)
{
    if (s_ctx.stopping || s_ctx.state != TS_AUTO_STATE_PAUSED) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
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

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    
    status->state = s_ctx.state;
    status->config_path = s_ctx.config_path;
    status->config_modified = s_ctx.config_modified;
    status->actions_executed = s_ctx.actions_executed;
    status->rule_triggers = s_ctx.rule_triggers;
    
    if (s_ctx.state >= TS_AUTO_STATE_INITIALIZED) {
        status->uptime_ms = (uint32_t)((esp_timer_get_time() / 1000) - s_ctx.start_time_ms);
    }

    xSemaphoreGive(s_ctx.mutex);

    // 从子模块获取计数
    status->sources_count = ts_source_count();
    status->rules_count = ts_rule_count();
    status->variables_count = ts_variable_count();

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

/**
 * 加载配置文件
 * 
 * automation.json 是引擎级元配置，包含：
 * - version: 配置版本
 * - enabled: 引擎是否启用
 * - eval_interval_ms: 规则评估间隔
 * 
 * 注意：sources、rules、actions 由各自模块加载，这里只处理引擎级设置
 */
static esp_err_t load_config(const char *path)
{
    ESP_LOGI(TAG, "Loading config from: %s", path);
    
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGD(TAG, "Config file not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 4096) {
        fclose(f);
        ESP_LOGW(TAG, "Config file invalid size: %ld", size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    char *content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    size_t read = fread(content, 1, size, f);
    fclose(f);
    content[read] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    free(content);
    
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse config JSON");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 解析配置项 */
    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (version && cJSON_IsString(version)) {
        ESP_LOGI(TAG, "Config version: %s", version->valuestring);
    }
    
    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (enabled && !cJSON_IsTrue(enabled)) {
        ESP_LOGW(TAG, "Automation engine disabled by config");
        /* 可以在这里设置标志，但目前不阻止启动 */
    }
    
    cJSON *eval_interval = cJSON_GetObjectItem(root, "eval_interval_ms");
    if (eval_interval && cJSON_IsNumber(eval_interval)) {
        ESP_LOGI(TAG, "Rule eval interval: %d ms", eval_interval->valueint);
        /* TODO: 应用到规则引擎 */
    }
    
    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Config loaded from SD card");
    return ESP_OK;
}

/**
 * @brief 应用默认引擎配置
 * 
 * 注意：数据源、规则、动作模板已由各自模块按 SD卡>NVS 优先级加载
 * 此函数仅设置引擎级默认参数
 */
static esp_err_t apply_default_config(void)
{
    /* 引擎级默认设置已在结构体初始化时设定 */
    /* sources.json/rules.json/actions.json 已由子模块独立加载 */
    return ESP_OK;
}

esp_err_t ts_automation_reload(void)
{
    if (s_ctx.state == TS_AUTO_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Reloading configuration");

    // 暂停引擎
    bool was_running = (s_ctx.state == TS_AUTO_STATE_RUNNING);
    if (was_running) {
        ts_automation_pause();
    }

    // 重新加载配置
    esp_err_t ret = load_config(s_ctx.config_path);
    if (ret == ESP_ERR_NOT_FOUND) {
        ret = apply_default_config();
    }

    // 恢复运行
    if (was_running) {
        ts_automation_resume();
    }

    s_ctx.config_modified = false;

    return ret;
}

esp_err_t ts_automation_save(const char *path)
{
    const char *save_path = path ? path : s_ctx.config_path;

    ESP_LOGI(TAG, "Saving configuration to: %s", save_path);

    // TODO: 实现配置保存
    // 1. 导出所有源、规则、变量为 JSON
    // 2. 写入文件

    s_ctx.config_modified = false;

    return ESP_OK;
}

int ts_automation_get_config_json(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return -1;
    }

    // TODO: 实现完整的 JSON 导出
    // 暂时返回最小配置
    const char *json = "{\"version\":\"1.0\",\"sources\":[],\"rules\":[],\"variables\":[]}";
    int len = strlen(json);
    
    if (len >= (int)buffer_size) {
        return -1;
    }

    strcpy(buffer, json);
    return len;
}

esp_err_t ts_automation_apply_config_json(const char *json)
{
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Applying config from JSON");

    // TODO: 实现 JSON 配置解析和应用

    s_ctx.config_modified = true;

    return ESP_OK;
}
