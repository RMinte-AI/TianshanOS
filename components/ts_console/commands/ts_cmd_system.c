/**
 * @file ts_cmd_system.c
 * @brief System Console Commands (API Layer)
 * 
 * 实现 system 命令族（通过 ts_api 调用）：
 * - system --info        显示系统信息
 * - system --version     显示版本
 * - system --uptime      显示运行时间
 * - system --memory      显示内存使用
 * - system --tasks       显示任务列表
 * - system --reboot      重启系统
 * 
 * @note JSON 输出模式使用 ts_api_call() 统一接口
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 * @date 2026-01-20
 */

#include "ts_console.h"
#include "ts_api.h"
#include "ts_log.h"
#include "ts_config_module.h"
#include "argtable3/argtable3.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define TAG "cmd_system"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *info;
    struct arg_lit *version;
    struct arg_lit *uptime;
    struct arg_lit *memory;
    struct arg_lit *memory_detail;  // 详细内存分析
    struct arg_lit *cpu;
    struct arg_lit *tasks;
    struct arg_lit *reboot;
    struct arg_lit *save;
    struct arg_int *delay;
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_system_args;

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static void format_uptime(uint64_t uptime_us, char *buf, size_t len)
{
    uint64_t seconds = uptime_us / 1000000;
    uint32_t days = seconds / 86400;
    uint32_t hours = (seconds % 86400) / 3600;
    uint32_t mins = (seconds % 3600) / 60;
    uint32_t secs = seconds % 60;
    
    if (days > 0) {
        snprintf(buf, len, "%lud %02lu:%02lu:%02lu", 
            (unsigned long)days, (unsigned long)hours, 
            (unsigned long)mins, (unsigned long)secs);
    } else {
        snprintf(buf, len, "%02lu:%02lu:%02lu", 
            (unsigned long)hours, (unsigned long)mins, (unsigned long)secs);
    }
}

static void format_size(size_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024 * 1024) {
        snprintf(buf, len, "%.1f MB", (float)bytes / (1024 * 1024));
    } else if (bytes >= 1024) {
        snprintf(buf, len, "%.1f KB", (float)bytes / 1024);
    } else {
        snprintf(buf, len, "%zu B", bytes);
    }
}

/*===========================================================================*/
/*                          Command: system --info                            */
/*===========================================================================*/

static int do_system_info(bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("system.info", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_error("API call failed: %s\n", 
                result.message ? result.message : esp_err_to_name(ret));
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 非 JSON 模式直接获取数据（格式化输出） */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    
    const esp_app_desc_t *app = esp_app_get_description();
    
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    
    uint64_t uptime_us = esp_timer_get_time();
    char uptime_str[32];
    format_uptime(uptime_us, uptime_str, sizeof(uptime_str));
    
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    char free_heap_str[16], min_heap_str[16], flash_str[16];
    format_size(free_heap, free_heap_str, sizeof(free_heap_str));
    format_size(min_heap, min_heap_str, sizeof(min_heap_str));
    format_size(flash_size, flash_str, sizeof(flash_str));
    
    const char *chip_model;
    switch (chip.model) {
        case CHIP_ESP32:   chip_model = "ESP32"; break;
        case CHIP_ESP32S2: chip_model = "ESP32-S2"; break;
        case CHIP_ESP32S3: chip_model = "ESP32-S3"; break;
        case CHIP_ESP32C3: chip_model = "ESP32-C3"; break;
        case CHIP_ESP32C6: chip_model = "ESP32-C6"; break;
        case CHIP_ESP32H2: chip_model = "ESP32-H2"; break;
        default:           chip_model = "Unknown"; break;
    }
    
    /* 格式化输出 */
    ts_console_printf("\n");
    ts_console_printf("╔══════════════════════════════════════════════════╗\n");
    ts_console_printf("║           TianShanOS System Information          ║\n");
    ts_console_printf("╚══════════════════════════════════════════════════╝\n\n");
    
    ts_console_printf("Application:\n");
    ts_console_printf("  Name:      %s\n", app->project_name);
    ts_console_printf("  Version:   %s\n", app->version);
    ts_console_printf("  IDF Ver:   %s\n", app->idf_ver);
    ts_console_printf("  Compiled:  %s %s\n", app->date, app->time);
    ts_console_printf("\n");
    
    ts_console_printf("Hardware:\n");
    ts_console_printf("  Chip:      %s\n", chip_model);
    ts_console_printf("  Cores:     %d\n", chip.cores);
    ts_console_printf("  Revision:  %d\n", chip.revision);
    ts_console_printf("  Flash:     %s\n", flash_str);
    ts_console_printf("\n");
    
    ts_console_printf("Runtime:\n");
    ts_console_printf("  Uptime:    %s\n", uptime_str);
    ts_console_printf("  Free Heap: %s\n", free_heap_str);
    ts_console_printf("  Min Heap:  %s\n", min_heap_str);
    ts_console_printf("\n");
    
    return 0;
}

/*===========================================================================*/
/*                          Command: system --version                         */
/*===========================================================================*/

static int do_system_version(bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("system.info", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            cJSON *app = cJSON_GetObjectItem(result.data, "app");
            if (app) {
                char *json_str = cJSON_PrintUnformatted(app);
                if (json_str) {
                    ts_console_printf("%s\n", json_str);
                    free(json_str);
                }
            }
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    const esp_app_desc_t *app = esp_app_get_description();
    ts_console_printf("%s v%s (ESP-IDF %s)\n", 
        app->project_name, app->version, app->idf_ver);
    
    return 0;
}

/*===========================================================================*/
/*                          Command: system --uptime                          */
/*===========================================================================*/

static int do_system_uptime(bool json)
{
    uint64_t uptime_us = esp_timer_get_time();
    char uptime_str[32];
    format_uptime(uptime_us, uptime_str, sizeof(uptime_str));
    
    if (json) {
        ts_console_printf("{\"uptime_us\":%llu,\"uptime_str\":\"%s\"}\n",
            (unsigned long long)uptime_us, uptime_str);
    } else {
        ts_console_printf("Uptime: %s\n", uptime_str);
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: system --cpu                             */
/*===========================================================================*/

static int do_system_cpu(bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("system.cpu", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_error("API call failed: %s\n", 
                result.message ? result.message : esp_err_to_name(ret));
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    ts_api_result_t result;
    esp_err_t ret = ts_api_call("system.cpu", NULL, &result);
    if (ret != ESP_OK || result.code != TS_API_OK || !result.data) {
        ts_console_error("Failed to get CPU stats: %s\n", 
            result.message ? result.message : esp_err_to_name(ret));
        ts_api_result_free(&result);
        return 1;
    }
    
    cJSON *cores = cJSON_GetObjectItem(result.data, "cores");
    cJSON *total_usage = cJSON_GetObjectItem(result.data, "total_usage");
    
    ts_console_printf("CPU Usage:\n\n");
    
    if (cores && cJSON_IsArray(cores)) {
        int num_cores = cJSON_GetArraySize(cores);
        for (int i = 0; i < num_cores; i++) {
            cJSON *core = cJSON_GetArrayItem(cores, i);
            if (core) {
                cJSON *core_id = cJSON_GetObjectItem(core, "id");
                cJSON *usage = cJSON_GetObjectItem(core, "usage");
                if (core_id && usage) {
                    int usage_val = (int)cJSON_GetNumberValue(usage);
                    ts_console_printf("  Core %d: %3d%%  ", 
                        (int)cJSON_GetNumberValue(core_id), usage_val);
                    
                    // 简单的进度条（40个字符宽）
                    int bars = (usage_val * 40) / 100;
                    ts_console_printf("[");
                    for (int j = 0; j < 40; j++) {
                        if (j < bars) ts_console_printf("█");
                        else ts_console_printf("░");
                    }
                    ts_console_printf("]\n");
                }
            }
        }
    }
    
    if (total_usage) {
        int total = (int)cJSON_GetNumberValue(total_usage);
        ts_console_printf("\n  Total:  %3d%%\n", total);
    }
    
    ts_api_result_free(&result);
    return 0;
}

/*===========================================================================*/
/*                          Command: system --memory                          */
/*===========================================================================*/

static int do_system_memory(bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("system.memory", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_error("API call failed: %s\n", 
                result.message ? result.message : esp_err_to_name(ret));
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    
    // PSRAM 信息
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    
    // DMA 内存
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t total_dma = heap_caps_get_total_size(MALLOC_CAP_DMA);
    
    char buf1[16], buf2[16];
    
    ts_console_printf("Memory Usage:\n\n");
    ts_console_printf("%-10s  %12s  %12s  %8s\n", 
        "TYPE", "FREE", "TOTAL", "USED%");
    ts_console_printf("────────────────────────────────────────────────\n");
    
    if (total_heap > 0) {
        format_size(free_heap, buf1, sizeof(buf1));
        format_size(total_heap, buf2, sizeof(buf2));
        int used_pct = 100 - (free_heap * 100 / total_heap);
        ts_console_printf("%-10s  %12s  %12s  %7d%%\n", 
            "Heap", buf1, buf2, used_pct);
    }
    
    if (total_psram > 0) {
        format_size(free_psram, buf1, sizeof(buf1));
        format_size(total_psram, buf2, sizeof(buf2));
        int used_pct = 100 - (free_psram * 100 / total_psram);
        ts_console_printf("%-10s  %12s  %12s  %7d%%\n", 
            "PSRAM", buf1, buf2, used_pct);
    }
    
    if (total_dma > 0) {
        format_size(free_dma, buf1, sizeof(buf1));
        format_size(total_dma, buf2, sizeof(buf2));
        int used_pct = 100 - (free_dma * 100 / total_dma);
        ts_console_printf("%-10s  %12s  %12s  %7d%%\n", 
            "DMA", buf1, buf2, used_pct);
    }
    
    format_size(min_heap, buf1, sizeof(buf1));
    ts_console_printf("\nMinimum free heap ever: %s\n", buf1);
    
    return 0;
}

/*===========================================================================*/
/*                     Command: system --memory-detail                        */
/*===========================================================================*/

/**
 * @brief 详细内存分析（DRAM/PSRAM/DMA 堆信息）
 */
static int do_system_memory_detail(void)
{
    ts_console_printf("\n=== Detailed Heap Memory Analysis ===\n");
    
    // DRAM (Internal RAM)
    ts_console_printf("\n[DRAM - Internal RAM]\n");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    // PSRAM (External RAM)
    ts_console_printf("\n[PSRAM - External RAM]\n");
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    // DMA Capable Memory
    ts_console_printf("\n[DMA Capable Memory]\n");
    heap_caps_print_heap_info(MALLOC_CAP_DMA);
    
    // 32-bit aligned memory
    ts_console_printf("\n[32-bit Aligned Memory]\n");
    heap_caps_print_heap_info(MALLOC_CAP_32BIT);
    
    // 显示最大可分配块
    size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t dram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t dram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t dma_total = heap_caps_get_total_size(MALLOC_CAP_DMA);
    
    ts_console_printf("\n=== Allocation Summary ===\n");
    ts_console_printf("%-8s  %10s  %10s  %10s  %7s  %10s\n",
                      "Type", "Total", "Free", "Used", "Used%", "Largest");
    ts_console_printf("────────────────────────────────────────────────────────────────\n");
    
    if (dram_total > 0) {
        int used_pct = 100 - (dram_free * 100 / dram_total);
        ts_console_printf("%-8s  %10u  %10u  %10u  %6d%%  %10u\n",
                          "DRAM", dram_total, dram_free, 
                          dram_total - dram_free, used_pct, dram_largest);
    }
    
    if (psram_total > 0) {
        int used_pct = 100 - (psram_free * 100 / psram_total);
        ts_console_printf("%-8s  %10u  %10u  %10u  %6d%%  %10u\n",
                          "PSRAM", psram_total, psram_free,
                          psram_total - psram_free, used_pct, psram_largest);
    }
    
    if (dma_total > 0) {
        int used_pct = 100 - (dma_free * 100 / dma_total);
        ts_console_printf("%-8s  %10u  %10u  %10u  %6d%%  %10u\n",
                          "DMA", dma_total, dma_free,
                          dma_total - dma_free, used_pct, dma_largest);
    }
    
    // 内存碎片化分析
    ts_console_printf("\n=== Fragmentation Analysis ===\n");
    if (dram_total > 0 && dram_free > 0) {
        float dram_frag = 100.0f * (1.0f - (float)dram_largest / dram_free);
        ts_console_printf("DRAM  Fragmentation: %.1f%% (largest/free: %u/%u)\n",
                          dram_frag, dram_largest, dram_free);
    }
    if (psram_total > 0 && psram_free > 0) {
        float psram_frag = 100.0f * (1.0f - (float)psram_largest / psram_free);
        ts_console_printf("PSRAM Fragmentation: %.1f%% (largest/free: %u/%u)\n",
                          psram_frag, psram_largest, psram_free);
    }
    
    // 最小空闲堆记录
    size_t min_free = esp_get_minimum_free_heap_size();
    ts_console_printf("\n=== Historical Data ===\n");
    ts_console_printf("Minimum free heap ever: %u bytes\n", min_free);
    
    ts_console_printf("\n💡 Optimization Tips:\n");
    if (dram_total > 0) {
        int used_pct = 100 - (dram_free * 100 / dram_total);
        if (used_pct > 80) {
            ts_console_printf("  ⚠️  DRAM usage > 80%%, consider moving buffers to PSRAM\n");
        }
    }
    if (psram_total > 0) {
        int used_pct = 100 - (psram_free * 100 / psram_total);
        if (used_pct < 50) {
            ts_console_printf("  ℹ️  PSRAM usage < 50%%, plenty of space available\n");
        }
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: system --tasks                           */
/*===========================================================================*/

static int do_system_tasks(bool json)
{
    /* JSON 模式使用 API */
    if (json) {
        ts_api_result_t result;
        esp_err_t ret = ts_api_call("system.tasks", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_error("API call failed: %s\n", 
                result.message ? result.message : esp_err_to_name(ret));
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    uint32_t num_tasks = uxTaskGetNumberOfTasks();
    
    ts_console_printf("Tasks (%lu total):\n\n", (unsigned long)num_tasks);
    ts_console_printf("%-20s  %5s  %6s  %10s\n", 
        "NAME", "PRI", "STATE", "STACK");
    ts_console_printf("──────────────────────────────────────────────\n");
    
#if configUSE_TRACE_FACILITY
    TaskStatus_t *tasks = heap_caps_malloc(num_tasks * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tasks) tasks = malloc(num_tasks * sizeof(TaskStatus_t));
    if (!tasks) {
        ts_console_error("Out of memory\n");
        return 1;
    }
    
    uint32_t total_runtime;
    uint32_t count = uxTaskGetSystemState(tasks, num_tasks, &total_runtime);
    
    for (uint32_t i = 0; i < count; i++) {
        const char *state_str;
        switch (tasks[i].eCurrentState) {
            case eRunning:   state_str = "RUN"; break;
            case eReady:     state_str = "READY"; break;
            case eBlocked:   state_str = "BLOCK"; break;
            case eSuspended: state_str = "SUSP"; break;
            case eDeleted:   state_str = "DEL"; break;
            default:         state_str = "?"; break;
        }
        
        ts_console_printf("%-20s  %5lu  %6s  %10lu\n",
            tasks[i].pcTaskName,
            (unsigned long)tasks[i].uxCurrentPriority,
            state_str,
            (unsigned long)tasks[i].usStackHighWaterMark);
    }
    
    free(tasks);
#else
    ts_console_printf("Task tracing not enabled\n");
#endif
    
    return 0;
}

/*===========================================================================*/
/*                          Command: system --reboot                          */
/*===========================================================================*/

static int do_system_reboot(int delay_sec)
{
    if (delay_sec > 0) {
        ts_console_printf("System will reboot in %d seconds...\n", delay_sec);
    } else {
        ts_console_printf("Rebooting...\n");
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // 让消息打印出来
    
    /* 使用 API 执行重启 */
    cJSON *params = cJSON_CreateObject();
    if (params) {
        cJSON_AddNumberToObject(params, "delay_ms", delay_sec * 1000);
    }
    
    ts_api_result_t result;
    ts_api_call("system.reboot", params, &result);
    
    if (params) cJSON_Delete(params);
    ts_api_result_free(&result);
    
    // 不会执行到这里
    return 0;
}

/*===========================================================================*/
/*                          Command: system --save                            */
/*===========================================================================*/

static int do_system_save(void)
{
    ts_console_printf("Saving system configuration...\n");
    
    esp_err_t ret = ts_config_module_persist(TS_CONFIG_MODULE_SYSTEM);
    if (ret == ESP_OK) {
        ts_console_success("System configuration saved to NVS");
        ts_console_printf("\n");
        return 0;
    } else {
        ts_console_error("Failed to save configuration: %s\n", esp_err_to_name(ret));
        return 1;
    }
}

/*===========================================================================*/
/*                          Main Command Handler                              */
/*===========================================================================*/

static int cmd_system(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_system_args);
    
    // 显示帮助
    if (s_system_args.help->count > 0) {
        ts_console_printf("Usage: system [options]\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  -i, --info          Show system information\n");
        ts_console_printf("  -V, --version       Show version\n");
        ts_console_printf("  -u, --uptime        Show uptime\n");
        ts_console_printf("  -m, --memory        Show memory usage\n");
        ts_console_printf("      --memory-detail Show detailed memory analysis\n");
        ts_console_printf("  -c, --cpu           Show CPU core usage\n");
        ts_console_printf("  -t, --tasks         Show task list\n");
        ts_console_printf("  -r, --reboot        Reboot system\n");
        ts_console_printf("      --save          Save system config to NVS/SD\n");
        ts_console_printf("      --delay <sec>   Delay before reboot\n");
        ts_console_printf("  -j, --json          JSON output\n");
        ts_console_printf("  -h, --help          Show this help\n\n");
        ts_console_printf("Examples:\n");
        ts_console_printf("  system --info\n");
        ts_console_printf("  system --cpu\n");
        ts_console_printf("  system --memory --json\n");
        ts_console_printf("  system --reboot --delay 5\n");
        return 0;
    }
    
    if (nerrors != 0) {
        arg_print_errors(stderr, s_system_args.end, argv[0]);
        return 1;
    }
    
    bool json = s_system_args.json->count > 0;
    
    // 重启命令优先
    if (s_system_args.reboot->count > 0) {
        int delay = s_system_args.delay->count > 0 ? 
                    s_system_args.delay->ival[0] : 0;
        return do_system_reboot(delay);
    }
    
    if (s_system_args.save->count > 0) {
        return do_system_save();
    }
    
    if (s_system_args.version->count > 0) {
        return do_system_version(json);
    }
    
    if (s_system_args.uptime->count > 0) {
        return do_system_uptime(json);
    }
    
    if (s_system_args.memory->count > 0) {
        return do_system_memory(json);
    }
    
    if (s_system_args.memory_detail->count > 0) {
        return do_system_memory_detail();
    }
    
    if (s_system_args.cpu->count > 0) {
        return do_system_cpu(json);
    }
    
    if (s_system_args.tasks->count > 0) {
        return do_system_tasks(json);
    }
    
    // 默认显示系统信息
    return do_system_info(json);
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_system_register(void)
{
    s_system_args.info    = arg_lit0("i", "info", "Show system info");
    s_system_args.version = arg_lit0("V", "version", "Show version");
    s_system_args.uptime  = arg_lit0("u", "uptime", "Show uptime");
    s_system_args.memory  = arg_lit0("m", "memory", "Show memory usage");
    s_system_args.memory_detail = arg_lit0(NULL, "memory-detail", "Show detailed memory analysis");
    s_system_args.cpu     = arg_lit0("c", "cpu", "Show CPU core usage");
    s_system_args.tasks   = arg_lit0("t", "tasks", "Show tasks");
    s_system_args.reboot  = arg_lit0("r", "reboot", "Reboot system");
    s_system_args.save    = arg_lit0(NULL, "save", "Save system config");
    s_system_args.delay   = arg_int0(NULL, "delay", "<sec>", "Delay before reboot");
    s_system_args.json    = arg_lit0("j", "json", "JSON output");
    s_system_args.help    = arg_lit0("h", "help", "Show help");
    s_system_args.end     = arg_end(10);
    
    const ts_console_cmd_t cmd = {
        .command = "system",
        .help = "System information and control (via API)",
        .hint = NULL,
        .category = TS_CMD_CAT_SYSTEM,
        .func = cmd_system,
        .argtable = &s_system_args
    };
    
    esp_err_t ret = ts_console_register_cmd(&cmd);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "System commands registered");
    }
    
    return ret;
}
