/**
 * @file ts_core_init.c
 * @brief TianShanOS Core Initialization Implementation
 *
 * 核心初始化实现
 *
 * @author TianShanOS Team
 * @version 0.1.0
 */

#include "ts_core.h"
#include "ts_config_nvs.h"
#include "ts_config_file.h"
#include "ts_config_schemas.h"
#include "ts_mempool.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "ts_core";

static bool s_core_initialized = false;
static bool s_core_started = false;
static bool s_core_cleanup_pending;
static bool s_core_stopping;

static esp_err_t core_release_components(void)
{
    esp_err_t ret;
    if (ts_service_is_initialized() && (ret = ts_service_deinit()) != ESP_OK) return ret;
    if (ts_event_is_initialized() && (ret = ts_event_deinit()) != ESP_OK) return ret;
    if (ts_log_is_initialized() && (ret = ts_log_deinit()) != ESP_OK) return ret;
    if (ts_config_is_initialized() && (ret = ts_config_deinit()) != ESP_OK) return ret;
    return ESP_OK;
}
static esp_err_t core_init_rollback(esp_err_t original)
{
    s_core_cleanup_pending=true;
    esp_err_t ret=core_release_components();
    if(ret!=ESP_OK)return ret;
    s_core_cleanup_pending=false;
    return original;
}

/* ============================================================================
 * cJSON PSRAM 内存钩子 - 减少 DRAM 碎片
 * ========================================================================== */

static void *cjson_psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);  /* 回退到 DRAM */
}

static void cjson_psram_free(void *ptr)
{
    free(ptr);  /* heap_caps 分配的内存可以用 free() 释放 */
}

/* ============================================================================
 * 版本信息
 * ========================================================================== */

const char *ts_get_version(void)
{
    return TIANSHAN_OS_VERSION_FULL;
}

const char *ts_get_build_time(void)
{
    static char build_time[32];
    snprintf(build_time, sizeof(build_time), "%s %s", __DATE__, __TIME__);
    return build_time;
}

/* ============================================================================
 * 核心初始化
 * ========================================================================== */

esp_err_t ts_core_init(void)
{
    esp_err_t ret;

    if (s_core_initialized || s_core_cleanup_pending) {
        ESP_LOGW(TAG, "Core already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "TianShanOS Core v%s initializing...", TIANSHAN_OS_VERSION_STRING);

    // 0a. 初始化内存池（必须最先，减少 DRAM 碎片）
    ret = ts_mempool_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Memory pool init failed: %s (continuing without pools)", esp_err_to_name(ret));
        // 不是致命错误，继续初始化
    } else {
        ESP_LOGI(TAG, "Memory pools initialized in PSRAM");
    }

    // 0b. 配置 cJSON 使用 PSRAM（必须在任何 JSON 操作之前）
    cJSON_Hooks hooks = {
        .malloc_fn = cjson_psram_malloc,
        .free_fn = cjson_psram_free
    };
    cJSON_InitHooks(&hooks);
    ESP_LOGI(TAG, "cJSON PSRAM hooks installed");

    // 1. 初始化配置管理
    ESP_LOGI(TAG, "Initializing configuration system...");
    ret = ts_config_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize config: %s", esp_err_to_name(ret));
        return ret;
    }

    // 注册 NVS 后端
    ret = ts_config_nvs_register();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register NVS backend: %s", esp_err_to_name(ret));
        // 继续，NVS 不是必须的
    }

    // 注册文件后端
    ret = ts_config_file_register();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register file backend: %s", esp_err_to_name(ret));
        // 继续，文件后端不是必须的
    }

    // 2. 初始化日志系统
    ESP_LOGI(TAG, "Initializing logging system...");
    ret = ts_log_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize log: %s", esp_err_to_name(ret));
        return core_init_rollback(ret);
    }

    // 3. 初始化事件系统
    ESP_LOGI(TAG, "Initializing event system...");
    ret = ts_event_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize event: %s", esp_err_to_name(ret));
        return core_init_rollback(ret);
    }

    // 3.1 注册 config_file 事件监听器（依赖事件系统）
    ret = ts_config_file_register_events();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register config file events: %s", esp_err_to_name(ret));
        // 不是致命错误，继续
    }

    // 3.2 初始化配置模块系统（注册所有模块 Schema 并加载配置）
    ret = ts_config_schemas_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init config schemas: %s", esp_err_to_name(ret));
        // 不是致命错误，继续
    }

    // 4. 初始化服务管理
    ESP_LOGI(TAG, "Initializing service management...");
    ret = ts_service_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize service: %s", esp_err_to_name(ret));
        return core_init_rollback(ret);
    }

    s_core_initialized = true;
    ESP_LOGI(TAG, "TianShanOS Core initialized successfully");

    return ESP_OK;
}

esp_err_t ts_core_deinit(void)
{
    if (!s_core_initialized && !s_core_cleanup_pending) return ESP_ERR_INVALID_STATE;
    s_core_cleanup_pending=true;
    esp_err_t ret = ts_core_stop();
    if (ret != ESP_OK) return ret;
    /* Earlier stages may already have completed on a previous attempt. */
    ret=core_release_components();
    if(ret!=ESP_OK)return ret;
    s_core_initialized=false;
    s_core_cleanup_pending=false;
    return ESP_OK;
}

bool ts_core_is_initialized(void)
{
    return s_core_initialized;
}

esp_err_t ts_core_start(void)
{
    if(s_core_stopping || s_core_cleanup_pending) return ESP_ERR_INVALID_STATE;
    if (!s_core_initialized) {
        ESP_LOGE(TAG, "Core not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_core_started) {
        ESP_LOGW(TAG, "Core already started");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting TianShanOS...");

    /*
     * 配置文件加载说明：
     * 不在此处直接调用 ts_config_file_load_all()，因为 SD 卡尚未挂载。
     * ts_config_file 组件已注册存储事件监听器，当 SD 卡挂载后
     * 会自动触发配置加载（见 ts_config_file.c 中的 storage_event_handler）。
     */

    // 启动所有服务
    esp_err_t ret = ts_service_start_all();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start services: %s", esp_err_to_name(ret));
        return ret;
    }

    s_core_started = true;

    // 发送系统启动完成事件
    ts_event_post(TS_EVENT_BASE_SYSTEM, TS_EVENT_SYSTEM_STARTED, NULL, 0, 100);

    ESP_LOGI(TAG, "TianShanOS started");

    return ESP_OK;
}

esp_err_t ts_core_stop(void)
{
    if (!s_core_started) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping TianShanOS...");
    esp_err_t ret;
    /* A retry continues the same stop intent; do not replay shutdown handlers. */
    if(!s_core_stopping) {
        s_core_stopping=true;
        ret=ts_event_post_sync(TS_EVENT_BASE_SYSTEM,TS_EVENT_SYSTEM_SHUTDOWN,NULL,0);
        if(ret!=ESP_OK){s_core_stopping=false;return ret;}
    }

    // 停止所有服务
    ret = ts_service_stop_all();
    if (ret != ESP_OK) return ret;

    // 保存配置
    ret = ts_config_save();
    if (ret != ESP_OK) return ret;

    s_core_started = false;
    s_core_stopping=false;
    ESP_LOGI(TAG, "TianShanOS stopped");

    return ESP_OK;
}
