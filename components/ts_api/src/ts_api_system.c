/**
 * @file ts_api_system.c
 * @brief System API Handlers
 * 
 * 任务列表等大缓冲区优先分配到 PSRAM
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_api.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM */
#include "ts_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* Linker symbols for static memory sections */
extern uint32_t _data_start, _data_end;
extern uint32_t _bss_start, _bss_end;
extern uint32_t _rodata_start, _rodata_end;
extern uint32_t _iram_text_start, _iram_text_end;
extern uint32_t _rtc_data_start, _rtc_data_end;
extern uint32_t _rtc_bss_start, _rtc_bss_end;

#define TAG "api_system"

/*===========================================================================*/
/*                          Delayed Reboot Task                               */
/*===========================================================================*/

static void reboot_task(void *arg)
{
    int delay_ms = (int)(intptr_t)arg;
    
    /* Wait for response to be sent */
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    
    TS_LOGI(TAG, "Rebooting system...");
    esp_restart();
    
    /* Should not reach here */
    vTaskDelete(NULL);
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief system.info - Get system information
 */
static esp_err_t api_system_info(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* App info */
    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON *app = cJSON_AddObjectToObject(data, "app");
    cJSON_AddStringToObject(app, "name", app_desc->project_name);
    cJSON_AddStringToObject(app, "version", app_desc->version);
    cJSON_AddStringToObject(app, "idf_version", app_desc->idf_ver);
    cJSON_AddStringToObject(app, "compile_time", app_desc->time);
    cJSON_AddStringToObject(app, "compile_date", app_desc->date);
    
    /* Chip info */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    cJSON *chip = cJSON_AddObjectToObject(data, "chip");
    
    const char *model_name = "unknown";
    switch (chip_info.model) {
        case CHIP_ESP32:   model_name = "ESP32"; break;
        case CHIP_ESP32S2: model_name = "ESP32-S2"; break;
        case CHIP_ESP32S3: model_name = "ESP32-S3"; break;
        case CHIP_ESP32C3: model_name = "ESP32-C3"; break;
        case CHIP_ESP32C2: model_name = "ESP32-C2"; break;
        case CHIP_ESP32C6: model_name = "ESP32-C6"; break;
        case CHIP_ESP32H2: model_name = "ESP32-H2"; break;
        default: break;
    }
    cJSON_AddStringToObject(chip, "model", model_name);
    cJSON_AddNumberToObject(chip, "cores", chip_info.cores);
    cJSON_AddNumberToObject(chip, "revision", chip_info.revision);
    
    cJSON *features = cJSON_AddArrayToObject(chip, "features");
    if (chip_info.features & CHIP_FEATURE_WIFI_BGN) 
        cJSON_AddItemToArray(features, cJSON_CreateString("WiFi"));
    if (chip_info.features & CHIP_FEATURE_BT) 
        cJSON_AddItemToArray(features, cJSON_CreateString("BT"));
    if (chip_info.features & CHIP_FEATURE_BLE) 
        cJSON_AddItemToArray(features, cJSON_CreateString("BLE"));
    if (chip_info.features & CHIP_FEATURE_EMB_FLASH) 
        cJSON_AddItemToArray(features, cJSON_CreateString("Embedded Flash"));
    if (chip_info.features & CHIP_FEATURE_EMB_PSRAM) 
        cJSON_AddItemToArray(features, cJSON_CreateString("Embedded PSRAM"));
    
    /* Flash */
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        cJSON_AddNumberToObject(data, "flash_size", flash_size);
    }
    
    /* Uptime */
    int64_t uptime_us = esp_timer_get_time();
    cJSON_AddNumberToObject(data, "uptime_ms", uptime_us / 1000);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief system.memory - Get memory information
 */
static esp_err_t api_system_memory(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* Heap info */
    cJSON_AddNumberToObject(data, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(data, "min_free_heap", esp_get_minimum_free_heap_size());
    
    /* Internal memory */
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_INTERNAL);
    
    cJSON *internal = cJSON_AddObjectToObject(data, "internal");
    cJSON_AddNumberToObject(internal, "total", 
        heap_info.total_free_bytes + heap_info.total_allocated_bytes);
    cJSON_AddNumberToObject(internal, "free", heap_info.total_free_bytes);
    cJSON_AddNumberToObject(internal, "allocated", heap_info.total_allocated_bytes);
    cJSON_AddNumberToObject(internal, "largest_block", heap_info.largest_free_block);
    
    /* PSRAM */
    heap_caps_get_info(&heap_info, MALLOC_CAP_SPIRAM);
    if (heap_info.total_free_bytes > 0 || heap_info.total_allocated_bytes > 0) {
        cJSON *psram = cJSON_AddObjectToObject(data, "psram");
        cJSON_AddNumberToObject(psram, "total", 
            heap_info.total_free_bytes + heap_info.total_allocated_bytes);
        cJSON_AddNumberToObject(psram, "free", heap_info.total_free_bytes);
        cJSON_AddNumberToObject(psram, "allocated", heap_info.total_allocated_bytes);
        cJSON_AddNumberToObject(psram, "largest_block", heap_info.largest_free_block);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief system.tasks - Get task list
 */
static esp_err_t api_system_tasks(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
#if configUSE_TRACE_FACILITY
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_array = TS_MALLOC_PSRAM(task_count * sizeof(TaskStatus_t));
    
    if (task_array == NULL) {
        cJSON_Delete(data);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    uint32_t total_runtime;
    task_count = uxTaskGetSystemState(task_array, task_count, &total_runtime);
    
    cJSON *tasks = cJSON_AddArrayToObject(data, "tasks");
    
    for (int i = 0; i < task_count; i++) {
        cJSON *task = cJSON_CreateObject();
        cJSON_AddStringToObject(task, "name", task_array[i].pcTaskName);
        cJSON_AddNumberToObject(task, "priority", task_array[i].uxCurrentPriority);
        cJSON_AddNumberToObject(task, "stack_hwm", task_array[i].usStackHighWaterMark);
#if configTASKLIST_INCLUDE_COREID
        cJSON_AddNumberToObject(task, "core", task_array[i].xCoreID);
#else
        cJSON_AddNumberToObject(task, "core", -1);
#endif
        
        const char *state = "unknown";
        switch (task_array[i].eCurrentState) {
            case eRunning:   state = "running"; break;
            case eReady:     state = "ready"; break;
            case eBlocked:   state = "blocked"; break;
            case eSuspended: state = "suspended"; break;
            case eDeleted:   state = "deleted"; break;
            default: break;
        }
        cJSON_AddStringToObject(task, "state", state);
        
        cJSON_AddItemToArray(tasks, task);
    }
    
    free(task_array);
    cJSON_AddNumberToObject(data, "count", task_count);
#else
    cJSON_AddStringToObject(data, "error", "Task trace not enabled");
#endif
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief system.cpu - Get CPU core statistics
 */
/**
 * CPU 使用率采用增量计算（delta）：
 * 记录上一次采样的 IDLE 任务运行时间和总运行时间，
 * 通过两次采样的差值计算出当前（最近一个周期）的 CPU 使用率，
 * 而非自启动以来的累计平均值。
 */
static uint32_t s_prev_total_runtime = 0;
static uint32_t s_prev_idle_runtime[2] = {0, 0};  /* ESP32-S3 最多 2 核 */
static bool s_cpu_prev_valid = false;

static esp_err_t api_system_cpu(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* Get chip info for core count */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    uint8_t num_cores = chip_info.cores;
    
#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_array = TS_MALLOC_PSRAM(task_count * sizeof(TaskStatus_t));
    
    if (task_array == NULL) {
        cJSON_Delete(data);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    uint32_t total_runtime;
    UBaseType_t actual_count = uxTaskGetSystemState(task_array, task_count, &total_runtime);
    
    /* 只需要每个核心的 IDLE 任务运行时间 */
    uint32_t idle_runtime[2] = {0, 0};
    
    for (UBaseType_t i = 0; i < actual_count; i++) {
        if (strncmp(task_array[i].pcTaskName, "IDLE", 4) == 0) {
            BaseType_t core_id = task_array[i].xCoreID;
            if (core_id >= 0 && core_id < num_cores) {
                idle_runtime[core_id] = task_array[i].ulRunTimeCounter;
            }
        }
    }
    
    cJSON *cores = cJSON_AddArrayToObject(data, "cores");
    uint32_t total_usage_sum = 0;
    
    /* 增量计算：使用两次采样的差值 */
    uint32_t delta_total = total_runtime - s_prev_total_runtime;
    
    for (int i = 0; i < num_cores; i++) {
        cJSON *core = cJSON_CreateObject();
        cJSON_AddNumberToObject(core, "id", i);
        
        uint32_t cpu_percent = 0;
        
        if (s_cpu_prev_valid && delta_total > 0) {
            /* 每个核心可用时间 = 总时间增量（timer 对所有核心公用） */
            uint32_t delta_idle = idle_runtime[i] - s_prev_idle_runtime[i];
            
            /* 使用率 = 100% - 空闲率 */
            if (delta_idle < delta_total) {
                cpu_percent = ((delta_total - delta_idle) * 100) / delta_total;
            }
            /* 限幅保护（避免计数器回绕时出错） */
            if (cpu_percent > 100) cpu_percent = 100;
        }
        
        cJSON_AddNumberToObject(core, "usage", cpu_percent);
        cJSON_AddItemToArray(cores, core);
        total_usage_sum += cpu_percent;
    }
    
    /* 保存本次采样数据，供下次增量计算使用 */
    s_prev_total_runtime = total_runtime;
    for (int i = 0; i < num_cores && i < 2; i++) {
        s_prev_idle_runtime[i] = idle_runtime[i];
    }
    s_cpu_prev_valid = true;
    
    /* Overall CPU usage (average across cores) */
    cJSON_AddNumberToObject(data, "total_usage", num_cores > 0 ? total_usage_sum / num_cores : 0);
    cJSON_AddNumberToObject(data, "total_runtime", total_runtime);
    cJSON_AddNumberToObject(data, "task_count", actual_count);
    
    free(task_array);
#else
    /* Fallback when runtime stats not available */
    cJSON *cores = cJSON_AddArrayToObject(data, "cores");
    for (int i = 0; i < num_cores; i++) {
        cJSON *core = cJSON_CreateObject();
        cJSON_AddNumberToObject(core, "id", i);
        cJSON_AddNumberToObject(core, "usage", 0);
        cJSON_AddStringToObject(core, "error", "Runtime stats not enabled");
        cJSON_AddItemToArray(cores, core);
    }
    cJSON_AddNumberToObject(data, "total_usage", 0);
    cJSON_AddStringToObject(data, "error", "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS not enabled");
#endif
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief system.reboot - Reboot the system
 */
static esp_err_t api_system_reboot(const cJSON *params, ts_api_result_t *result)
{
    int delay_ms = 500;  /* Default 500ms to allow response to be sent */
    
    if (params) {
        cJSON *delay = cJSON_GetObjectItem(params, "delay");
        if (delay && cJSON_IsNumber(delay)) {
            delay_ms = delay->valueint;
            if (delay_ms < 100) delay_ms = 100;  /* Minimum 100ms to send response */
            if (delay_ms > 10000) delay_ms = 10000;
        }
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "status", "rebooting");
    cJSON_AddNumberToObject(data, "delay_ms", delay_ms);
    
    ts_api_result_ok(result, data);
    
    /* Schedule reboot in a separate task to allow HTTP response to be sent first */
    xTaskCreate(reboot_task, "reboot", 2048, (void *)(intptr_t)delay_ms, 1, NULL);
    
    return ESP_OK;
}

/**
 * @brief system.log.level - Get/set log level
 */
static esp_err_t api_system_log_level(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    
    if (params) {
        cJSON *level = cJSON_GetObjectItem(params, "level");
        cJSON *tag = cJSON_GetObjectItem(params, "tag");
        
        if (level && cJSON_IsNumber(level)) {
            int lvl = level->valueint;
            if (lvl >= TS_LOG_NONE && lvl <= TS_LOG_VERBOSE) {
                if (tag && cJSON_IsString(tag)) {
                    ts_log_set_tag_level(tag->valuestring, lvl);
                    cJSON_AddStringToObject(data, "tag", tag->valuestring);
                } else {
                    ts_log_set_level(lvl);
                }
                cJSON_AddNumberToObject(data, "level", lvl);
                cJSON_AddStringToObject(data, "status", "set");
            } else {
                ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid log level");
                cJSON_Delete(data);
                return ESP_ERR_INVALID_ARG;
            }
        }
    } else {
        cJSON_AddNumberToObject(data, "level", ts_log_get_level());
        cJSON_AddStringToObject(data, "status", "get");
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief Helper to add heap region info to JSON
 * @note Reserved for detailed memory analysis feature
 */
__attribute__((unused))
static void add_heap_regions(cJSON *regions_array, uint32_t caps)
{
    /* Use heap_caps_walk to get detailed region info */
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    
    /* Since we can't easily enumerate all regions, we provide aggregate info */
    cJSON *region = cJSON_CreateObject();
    cJSON_AddNumberToObject(region, "total_free", info.total_free_bytes);
    cJSON_AddNumberToObject(region, "total_allocated", info.total_allocated_bytes);
    cJSON_AddNumberToObject(region, "largest_free_block", info.largest_free_block);
    cJSON_AddNumberToObject(region, "minimum_free", info.minimum_free_bytes);
    cJSON_AddNumberToObject(region, "alloc_blocks", info.allocated_blocks);
    cJSON_AddNumberToObject(region, "free_blocks", info.free_blocks);
    cJSON_AddNumberToObject(region, "total_blocks", info.total_blocks);
    cJSON_AddItemToArray(regions_array, region);
}

/**
 * @brief system.memory_detail - Get detailed memory analysis
 * 
 * Returns comprehensive heap information including:
 * - DRAM (Internal RAM) statistics
 * - PSRAM (External RAM) statistics  
 * - DMA capable memory statistics
 * - IRAM (Instruction RAM) statistics
 * - Static memory sections (.data, .bss, .rodata)
 * - RTC memory usage
 * - Fragmentation analysis
 * - Task stack information with allocation size
 * - NVS usage statistics
 * - Optimization recommendations
 */
static esp_err_t api_system_memory_detail(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* === Static Memory Sections === */
    cJSON *static_mem = cJSON_AddObjectToObject(data, "static");
    
    /* .data section (initialized data) */
    size_t data_size = (size_t)(&_data_end - &_data_start) * sizeof(uint32_t);
    cJSON_AddNumberToObject(static_mem, "data_size", data_size);
    
    /* .bss section (uninitialized data) */
    size_t bss_size = (size_t)(&_bss_end - &_bss_start) * sizeof(uint32_t);
    cJSON_AddNumberToObject(static_mem, "bss_size", bss_size);
    
    /* .rodata section (read-only data, in flash) */
    size_t rodata_size = (size_t)(&_rodata_end - &_rodata_start) * sizeof(uint32_t);
    cJSON_AddNumberToObject(static_mem, "rodata_size", rodata_size);
    
    /* Total static DRAM usage */
    cJSON_AddNumberToObject(static_mem, "total_dram_static", data_size + bss_size);
    
    /* === IRAM (Instruction RAM) === */
    cJSON *iram = cJSON_AddObjectToObject(data, "iram");
    size_t iram_text_size = (size_t)(&_iram_text_end - &_iram_text_start) * sizeof(uint32_t);
    cJSON_AddNumberToObject(iram, "text_size", iram_text_size);
    
    /* IRAM heap (8-bit capable internal) */
    size_t iram_free = heap_caps_get_free_size(MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
    size_t iram_total = heap_caps_get_total_size(MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
    cJSON_AddNumberToObject(iram, "heap_total", iram_total);
    cJSON_AddNumberToObject(iram, "heap_free", iram_free);
    
    /* === RTC Memory === */
    cJSON *rtc = cJSON_AddObjectToObject(data, "rtc");
    size_t rtc_data_size = (size_t)(&_rtc_data_end - &_rtc_data_start) * sizeof(uint32_t);
    size_t rtc_bss_size = (size_t)(&_rtc_bss_end - &_rtc_bss_start) * sizeof(uint32_t);
    cJSON_AddNumberToObject(rtc, "data_size", rtc_data_size);
    cJSON_AddNumberToObject(rtc, "bss_size", rtc_bss_size);
    cJSON_AddNumberToObject(rtc, "total_used", rtc_data_size + rtc_bss_size);
    cJSON_AddNumberToObject(rtc, "total_available", 8192);  /* RTC slow memory is 8KB */
    
    /* === DRAM (Internal RAM) === */
    size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t dram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t dram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t dram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    
    multi_heap_info_t dram_info;
    heap_caps_get_info(&dram_info, MALLOC_CAP_INTERNAL);
    
    cJSON *dram = cJSON_AddObjectToObject(data, "dram");
    cJSON_AddNumberToObject(dram, "total", dram_total);
    cJSON_AddNumberToObject(dram, "free", dram_free);
    cJSON_AddNumberToObject(dram, "used", dram_total - dram_free);
    cJSON_AddNumberToObject(dram, "used_percent", dram_total > 0 ? (100 * (dram_total - dram_free) / dram_total) : 0);
    cJSON_AddNumberToObject(dram, "largest_block", dram_largest);
    cJSON_AddNumberToObject(dram, "min_free_ever", dram_min_free);
    cJSON_AddNumberToObject(dram, "alloc_blocks", dram_info.allocated_blocks);
    cJSON_AddNumberToObject(dram, "free_blocks", dram_info.free_blocks);
    
    /* Fragmentation: (1 - largest_free / total_free) * 100 */
    if (dram_free > 0) {
        float frag = 100.0f * (1.0f - (float)dram_largest / (float)dram_free);
        cJSON_AddNumberToObject(dram, "fragmentation", (int)(frag * 10) / 10.0);
    } else {
        cJSON_AddNumberToObject(dram, "fragmentation", 0);
    }
    
    /* === PSRAM (External RAM) === */
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t psram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    
    if (psram_total > 0) {
        multi_heap_info_t psram_info;
        heap_caps_get_info(&psram_info, MALLOC_CAP_SPIRAM);
        
        cJSON *psram = cJSON_AddObjectToObject(data, "psram");
        cJSON_AddNumberToObject(psram, "total", psram_total);
        cJSON_AddNumberToObject(psram, "free", psram_free);
        cJSON_AddNumberToObject(psram, "used", psram_total - psram_free);
        cJSON_AddNumberToObject(psram, "used_percent", 100 * (psram_total - psram_free) / psram_total);
        cJSON_AddNumberToObject(psram, "largest_block", psram_largest);
        cJSON_AddNumberToObject(psram, "min_free_ever", psram_min_free);
        cJSON_AddNumberToObject(psram, "alloc_blocks", psram_info.allocated_blocks);
        cJSON_AddNumberToObject(psram, "free_blocks", psram_info.free_blocks);
        
        if (psram_free > 0) {
            float frag = 100.0f * (1.0f - (float)psram_largest / (float)psram_free);
            cJSON_AddNumberToObject(psram, "fragmentation", (int)(frag * 10) / 10.0);
        } else {
            cJSON_AddNumberToObject(psram, "fragmentation", 0);
        }
    }
    
    /* === DMA Capable Memory === */
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dma_total = heap_caps_get_total_size(MALLOC_CAP_DMA);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    
    if (dma_total > 0) {
        cJSON *dma = cJSON_AddObjectToObject(data, "dma");
        cJSON_AddNumberToObject(dma, "total", dma_total);
        cJSON_AddNumberToObject(dma, "free", dma_free);
        cJSON_AddNumberToObject(dma, "used", dma_total - dma_free);
        cJSON_AddNumberToObject(dma, "used_percent", 100 * (dma_total - dma_free) / dma_total);
        cJSON_AddNumberToObject(dma, "largest_block", dma_largest);
    }
    
    /* === Historical Data === */
    cJSON *history = cJSON_AddObjectToObject(data, "history");
    cJSON_AddNumberToObject(history, "min_free_heap_ever", esp_get_minimum_free_heap_size());
    
    /* === Task Memory Usage (Top consumers) === */
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count > 0) {
        TaskStatus_t *task_array = TS_MALLOC_PSRAM(task_count * sizeof(TaskStatus_t));
        if (task_array) {
            uint32_t total_runtime;
            UBaseType_t actual_count = uxTaskGetSystemState(task_array, task_count, &total_runtime);
            
            /* Sort by stack high water mark (lower = more stack used) */
            /* We'll calculate stack usage = stack size - high water mark */
            /* Since we don't have stack size, we use high water mark directly (lower is worse) */
            
            /* Simple bubble sort for top tasks - sort by stack usage (ascending high water = more used) */
            for (UBaseType_t i = 0; i + 1 < actual_count; i++) {
                for (UBaseType_t j = 0; j < actual_count - i - 1; j++) {
                    if (task_array[j].usStackHighWaterMark > task_array[j + 1].usStackHighWaterMark) {
                        TaskStatus_t temp = task_array[j];
                        task_array[j] = task_array[j + 1];
                        task_array[j + 1] = temp;
                    }
                }
            }
            
            cJSON *tasks = cJSON_AddArrayToObject(data, "tasks");
            
            /* Calculate total stack memory used by all tasks */
            uint32_t total_stack_allocated = 0;
            
            /* Report ALL tasks sorted by stack high water mark (lowest first = most used) */
            for (UBaseType_t i = 0; i < actual_count; i++) {
                cJSON *task = cJSON_CreateObject();
                cJSON_AddStringToObject(task, "name", task_array[i].pcTaskName);
                
                /* Stack high water mark in bytes */
                uint32_t hwm_bytes = task_array[i].usStackHighWaterMark * sizeof(StackType_t);
                cJSON_AddNumberToObject(task, "stack_hwm", hwm_bytes);
                
                /* Try to get stack allocation size from task handle */
                TaskHandle_t task_handle = task_array[i].xHandle;
                if (task_handle != NULL) {
                    /* Get stack size using FreeRTOS API if available */
                    /* Note: pxTaskGetStackStart returns the start, we need to calculate size */
                    StackType_t *stack_start = (StackType_t *)task_array[i].pxStackBase;
                    if (stack_start != NULL) {
                        /* Estimate stack size based on known task configurations */
                        uint32_t stack_size = 0;
                        const char *name = task_array[i].pcTaskName;
                        
                        /* Known task stack sizes from sdkconfig and code */
                        if (strcmp(name, "main") == 0) stack_size = CONFIG_ESP_MAIN_TASK_STACK_SIZE;
                        else if (strcmp(name, "esp_timer") == 0) stack_size = CONFIG_ESP_TIMER_TASK_STACK_SIZE;
                        else if (strncmp(name, "ipc", 3) == 0) stack_size = CONFIG_ESP_IPC_TASK_STACK_SIZE;
                        else if (strncmp(name, "IDLE", 4) == 0) stack_size = CONFIG_FREERTOS_IDLE_TASK_STACKSIZE;
                        else if (strcmp(name, "Tmr Svc") == 0) stack_size = CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH;
                        else if (strcmp(name, "wifi") == 0) stack_size = 4096;  /* Default WiFi task */
                        else if (strcmp(name, "httpd") == 0 || strncmp(name, "http", 4) == 0) stack_size = 4096;
                        else if (strcmp(name, "console") == 0) stack_size = 4096;
                        else stack_size = 2048;  /* Default assumption */
                        
                        /* hwm_bytes is the REMAINING stack (high water mark = minimum free ever)
                         * If hwm > stack_size, our estimate is wrong - use hwm as minimum size */
                        if (hwm_bytes > stack_size) {
                            stack_size = hwm_bytes + 512;  /* Adjust estimate with some used margin */
                        }
                        uint32_t stack_used = stack_size - hwm_bytes;
                        uint32_t usage_pct = stack_size > 0 ? (100 * stack_used / stack_size) : 0;
                        
                        cJSON_AddNumberToObject(task, "stack_alloc", stack_size);
                        cJSON_AddNumberToObject(task, "stack_used", stack_used);
                        cJSON_AddNumberToObject(task, "stack_usage_pct", usage_pct);
                        total_stack_allocated += stack_size;
                    }
                }
                
                cJSON_AddNumberToObject(task, "priority", task_array[i].uxCurrentPriority);
                cJSON_AddNumberToObject(task, "core", task_array[i].xCoreID);
                
                /* State string */
                const char *state_str = "Unknown";
                switch (task_array[i].eCurrentState) {
                    case eRunning:   state_str = "Running"; break;
                    case eReady:     state_str = "Ready"; break;
                    case eBlocked:   state_str = "Blocked"; break;
                    case eSuspended: state_str = "Suspended"; break;
                    case eDeleted:   state_str = "Deleted"; break;
                    default: break;
                }
                cJSON_AddStringToObject(task, "state", state_str);
                
#if configGENERATE_RUN_TIME_STATS
                /* CPU usage percentage */
                if (total_runtime > 0) {
                    uint32_t cpu_percent = (task_array[i].ulRunTimeCounter * 100) / total_runtime;
                    cJSON_AddNumberToObject(task, "cpu_percent", cpu_percent);
                }
#endif
                cJSON_AddItemToArray(tasks, task);
            }
            
            cJSON_AddNumberToObject(data, "task_count", actual_count);
            cJSON_AddNumberToObject(data, "total_stack_allocated", total_stack_allocated);
            free(task_array);
        }
    }
    
    /* === NVS Usage Statistics === */
    nvs_stats_t nvs_stats;
    if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
        cJSON *nvs = cJSON_AddObjectToObject(data, "nvs");
        cJSON_AddNumberToObject(nvs, "used_entries", nvs_stats.used_entries);
        cJSON_AddNumberToObject(nvs, "free_entries", nvs_stats.free_entries);
        cJSON_AddNumberToObject(nvs, "total_entries", nvs_stats.total_entries);
        cJSON_AddNumberToObject(nvs, "namespace_count", nvs_stats.namespace_count);
        cJSON_AddNumberToObject(nvs, "used_percent", nvs_stats.total_entries > 0 ?
            100 * nvs_stats.used_entries / nvs_stats.total_entries : 0);
    }
    
    /* === Memory Capability Summary === */
    cJSON *caps = cJSON_AddObjectToObject(data, "caps");
    
    /* 8-bit accessible memory */
    size_t d8_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t d8_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    cJSON_AddNumberToObject(caps, "d8_free", d8_free);
    cJSON_AddNumberToObject(caps, "d8_total", d8_total);
    
    /* 32-bit accessible memory */
    size_t d32_free = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    size_t d32_total = heap_caps_get_total_size(MALLOC_CAP_32BIT);
    cJSON_AddNumberToObject(caps, "d32_free", d32_free);
    cJSON_AddNumberToObject(caps, "d32_total", d32_total);
    
    /* Default caps (what malloc uses) */
    size_t default_free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t default_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    cJSON_AddNumberToObject(caps, "default_free", default_free);
    cJSON_AddNumberToObject(caps, "default_total", default_total);
    
    /* === Optimization Tips === */
    cJSON *tips = cJSON_AddArrayToObject(data, "tips");
    
    if (dram_total > 0) {
        int dram_used_pct = 100 * (dram_total - dram_free) / dram_total;
        if (dram_used_pct > 85) {
            cJSON_AddItemToArray(tips, cJSON_CreateString("critical:DRAM 使用率超过 85%，系统可能不稳定"));
        } else if (dram_used_pct > 80) {
            cJSON_AddItemToArray(tips, cJSON_CreateString("warning:DRAM 使用率超过 80%，建议将缓冲区迁移到 PSRAM"));
        }
        
        if (dram_free > 0) {
            float frag = 100.0f * (1.0f - (float)dram_largest / (float)dram_free);
            if (frag > 60) {
                cJSON_AddItemToArray(tips, cJSON_CreateString("warning:dram_fragmented"));
            }
        }
    }
    
    if (psram_total > 0) {
        int psram_used_pct = 100 * (psram_total - psram_free) / psram_total;
        if (psram_used_pct < 50) {
            cJSON_AddItemToArray(tips, cJSON_CreateString("info:psram_sufficient"));
        }
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief system.apis - 列出所有已注册的 API（诊断用）
 */
static esp_err_t api_system_apis(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    // 获取所有 API 列表
    cJSON *apis = ts_api_list(TS_API_CAT_MAX);  // TS_API_CAT_MAX = 所有类别
    if (apis) {
        cJSON_AddItemToObject(data, "apis", apis);
    } else {
        cJSON_AddArrayToObject(data, "apis");
    }
    
    // 添加一些统计信息
    int total = 0;
    if (apis) {
        total = cJSON_GetArraySize(apis);
    }
    cJSON_AddNumberToObject(data, "total", total);
    
    // 检查 monitor.* API 是否存在
    bool has_monitor_status = false;
    bool has_monitor_data = false;
    if (apis) {
        cJSON *item;
        cJSON_ArrayForEach(item, apis) {
            cJSON *name = cJSON_GetObjectItem(item, "name");
            if (name && cJSON_IsString(name)) {
                if (strcmp(name->valuestring, "monitor.status") == 0) has_monitor_status = true;
                if (strcmp(name->valuestring, "monitor.data") == 0) has_monitor_data = true;
            }
        }
    }
    cJSON_AddBoolToObject(data, "has_monitor_status", has_monitor_status);
    cJSON_AddBoolToObject(data, "has_monitor_data", has_monitor_data);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                      Register System APIs                                  */
/*===========================================================================*/

esp_err_t ts_api_system_register(void)
{
    static const ts_api_endpoint_t system_apis[] = {
        {
            .name = "system.info",
            .description = "Get system information",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_system_info,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "system.memory",
            .description = "Get memory information",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_system_memory,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "system.memory_detail",
            .description = "Get detailed memory analysis",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_system_memory_detail,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "system.cpu",
            .description = "Get CPU core statistics",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_system_cpu,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "system.tasks",
            .description = "Get task list",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_system_tasks,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "system.reboot",
            .description = "Reboot the system",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_system_reboot,
            .requires_auth = false,  /* TODO: Enable auth in production */
            .permission = NULL
        },
        {
            .name = "system.log.level",
            .description = "Get/set log level",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_system_log_level,
            .requires_auth = true,
            .permission = "system.config"
        },
        {
            .name = "system.apis",
            .description = "List all registered APIs",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_system_apis,
            .requires_auth = false,
            .permission = NULL
        }
    };
    
    return ts_api_register_multiple(system_apis, 
                                     sizeof(system_apis) / sizeof(system_apis[0]));
}
