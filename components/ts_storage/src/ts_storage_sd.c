/**
 * @file ts_storage_sd.c
 * @brief SD Card Storage Implementation
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#include "ts_storage.h"
#include "ts_log.h"
#include "ts_event.h"
#include "esp_vfs_fat.h"
#include "esp_vfs.h"       /* for esp_vfs_dump_registered_paths() */
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "diskio_sdmmc.h"  /* for ff_diskio_get_pdrv_card() */
#include "diskio_impl.h"   /* for ff_diskio_get_drive() */
#include "ffconf.h"        /* for FF_VOLUMES */
#include <string.h>
#include <strings.h>
#include <stdio.h>         /* for stdout */

#define TAG "storage_sd"

/**
 * @brief 安全喂看门狗 - 仅当任务已注册时才调用
 * 
 * 在 ESP-IDF v5.x 中，如果任务未注册到 TWDT，调用 esp_task_wdt_reset() 会报错。
 * 使用 taskYIELD() 作为替代方案，让出 CPU 使 IDLE 任务有机会喂狗。
 */
static inline void sd_yield_to_wdt(void)
{
    taskYIELD();
}

/*===========================================================================*/
/*                          External Functions                                */
/*===========================================================================*/

extern void ts_storage_set_sd_mounted(bool mounted, const char *mount_point);

/*===========================================================================*/
/*                          Private Data                                      */
/*===========================================================================*/

static char s_mount_point[32] = "";
static sdmmc_card_t *s_card = NULL;
static StaticSemaphore_t s_gate_storage;
static SemaphoreHandle_t s_gate;
static uint64_t s_generation;

void ts_storage_sd_lock(void)
{
    /* Storage initialization owns the first call, before services start. */
    if (!s_gate) s_gate=xSemaphoreCreateRecursiveMutexStatic(&s_gate_storage);
    xSemaphoreTakeRecursive(s_gate,portMAX_DELAY);
}
void ts_storage_sd_unlock(void) { xSemaphoreGiveRecursive(s_gate); }
uint64_t ts_storage_sd_generation(void)
{
    ts_storage_sd_lock();uint64_t value=s_generation;ts_storage_sd_unlock();return value;
}

/*===========================================================================*/
/*                          SD Card Operations                                */
/*===========================================================================*/

static esp_err_t mount_sd_locked(const ts_sd_config_t *config)
{
    if (s_card != NULL) {
        TS_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }
    
    /* 确保之前的状态已清理干净 */
    ts_storage_set_sd_mounted(false, NULL);
    
    /* 
     * 关键诊断：检查 FATFS 内部的 s_fat_ctxs[] 是否仍然有 /sdcard 条目
     * 如果 esp_vfs_fat_info() 成功，说明 s_fat_ctxs[] 中仍然有该路径的条目
     * 这与 VFS 的 s_vfs[] 是独立的资源！
     */
    {
        uint64_t total = 0, free_bytes = 0;
        esp_err_t info_ret = esp_vfs_fat_info("/sdcard", &total, &free_bytes);
        if (info_ret == ESP_OK) {
            TS_LOGE(TAG, "CRITICAL: /sdcard still exists in FATFS s_fat_ctxs[] (total=%llu)!", 
                    (unsigned long long)total);
            TS_LOGE(TAG, "This indicates s_fat_ctxs[] was not properly cleaned during previous unmount");
        } else {
            TS_LOGI(TAG, "Good: /sdcard not in FATFS s_fat_ctxs[] (info returned %s)", 
                    esp_err_to_name(info_ret));
        }
    }
    
    /* 打印挂载前的堆内存状态，帮助诊断内存问题 */
    TS_LOGD(TAG, "Pre-mount heap: free=%lu, min_free=%lu",
            (unsigned long)esp_get_free_heap_size(),
            (unsigned long)esp_get_minimum_free_heap_size());
    
    ts_sd_config_t cfg;
    
    if (config) {
        cfg = *config;
    } else {
        /* Use defaults from Kconfig */
#ifdef CONFIG_TS_STORAGE_SD_MOUNT_POINT
        cfg.mount_point = CONFIG_TS_STORAGE_SD_MOUNT_POINT;
#else
        cfg.mount_point = "/sdcard";
#endif

#ifdef CONFIG_TS_STORAGE_SD_MAX_FREQ_KHZ
        cfg.max_freq_khz = CONFIG_TS_STORAGE_SD_MAX_FREQ_KHZ;
#else
        cfg.max_freq_khz = 20000;
#endif

#if defined(CONFIG_TS_STORAGE_SD_MODE_SPI)
        cfg.mode = TS_SD_MODE_SPI;
#elif defined(CONFIG_TS_STORAGE_SD_MODE_SDIO_1BIT)
        cfg.mode = TS_SD_MODE_SDIO_1BIT;
#else
        cfg.mode = TS_SD_MODE_SDIO_4BIT;
#endif
        cfg.format_if_mount_failed = false;
        
        /* SDIO GPIO configuration from Kconfig */
#ifdef CONFIG_TS_STORAGE_SD_CMD_GPIO
        cfg.pin_cmd = CONFIG_TS_STORAGE_SD_CMD_GPIO;
#else
        cfg.pin_cmd = -1;
#endif
#ifdef CONFIG_TS_STORAGE_SD_CLK_GPIO
        cfg.pin_clk = CONFIG_TS_STORAGE_SD_CLK_GPIO;
#else
        cfg.pin_clk = -1;
#endif
#ifdef CONFIG_TS_STORAGE_SD_D0_GPIO
        cfg.pin_d0 = CONFIG_TS_STORAGE_SD_D0_GPIO;
#else
        cfg.pin_d0 = -1;
#endif
#ifdef CONFIG_TS_STORAGE_SD_D1_GPIO
        cfg.pin_d1 = CONFIG_TS_STORAGE_SD_D1_GPIO;
#else
        cfg.pin_d1 = -1;
#endif
#ifdef CONFIG_TS_STORAGE_SD_D2_GPIO
        cfg.pin_d2 = CONFIG_TS_STORAGE_SD_D2_GPIO;
#else
        cfg.pin_d2 = -1;
#endif
#ifdef CONFIG_TS_STORAGE_SD_D3_GPIO
        cfg.pin_d3 = CONFIG_TS_STORAGE_SD_D3_GPIO;
#else
        cfg.pin_d3 = -1;
#endif
    }
    
    TS_LOGI(TAG, "Mounting SD card at %s (mode: %d, freq: %d kHz)", 
            cfg.mount_point, cfg.mode, cfg.max_freq_khz);
    TS_LOGI(TAG, "SD GPIO: CMD=%d, CLK=%d, D0=%d, D1=%d, D2=%d, D3=%d",
            cfg.pin_cmd, cfg.pin_clk, cfg.pin_d0, cfg.pin_d1, cfg.pin_d2, cfg.pin_d3);
    
    // 让出 CPU，防止长时间操作阻塞其他任务
    sd_yield_to_wdt();
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = cfg.format_if_mount_failed,
        .max_files = 10,
        .allocation_unit_size = 8 * 1024,
        .disk_status_check_enable = true
    };
    
    esp_err_t ret;
    
    if (cfg.mode == TS_SD_MODE_SPI) {
        /* SPI mode */
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.max_freq_khz = cfg.max_freq_khz;
        
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = cfg.pin_mosi,
            .miso_io_num = cfg.pin_miso,
            .sclk_io_num = cfg.pin_clk,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        
        ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
            return ret;
        }
        
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = cfg.pin_cs;
        slot_config.host_id = host.slot;
        
        ret = esp_vfs_fat_sdspi_mount(cfg.mount_point, &host, &slot_config, 
                                       &mount_config, &s_card);
    } else {
        /* SDIO mode */
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = cfg.max_freq_khz;
        
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        
        if (cfg.mode == TS_SD_MODE_SDIO_1BIT) {
            slot_config.width = 1;
        } else {
            slot_config.width = 4;
        }
        
        /* Set GPIO pins if specified */
        if (cfg.pin_cmd >= 0) slot_config.cmd = cfg.pin_cmd;
        if (cfg.pin_clk >= 0) slot_config.clk = cfg.pin_clk;
        if (cfg.pin_d0 >= 0) slot_config.d0 = cfg.pin_d0;
        if (cfg.pin_d1 >= 0) slot_config.d1 = cfg.pin_d1;
        if (cfg.pin_d2 >= 0) slot_config.d2 = cfg.pin_d2;
        if (cfg.pin_d3 >= 0) slot_config.d3 = cfg.pin_d3;
        
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        
        // 让出 CPU
        sd_yield_to_wdt();
        
        ret = esp_vfs_fat_sdmmc_mount(cfg.mount_point, &host, &slot_config, 
                                       &mount_config, &s_card);
        
        // 挂载后让出 CPU
        sd_yield_to_wdt();
    }
    
    if (ret != ESP_OK) {
        // 让出 CPU，确保错误处理期间不阻塞
        sd_yield_to_wdt();
        
        // 根据错误类型提供更友好的错误信息
        switch (ret) {
            case ESP_ERR_TIMEOUT:
                TS_LOGW(TAG, "No SD card detected - slot may be empty");
                break;
            case ESP_ERR_INVALID_STATE:
                TS_LOGW(TAG, "SD card slot already in use or hardware conflict");
                break;
            case ESP_ERR_NOT_FOUND:
                TS_LOGW(TAG, "SD card not responding or unsupported");
                break;
            case ESP_ERR_NOT_SUPPORTED:
                TS_LOGW(TAG, "SD card format not supported - may need formatting");
                break;
            case ESP_ERR_NO_MEM:
                TS_LOGE(TAG, "Memory allocation failed during mount");
                TS_LOGE(TAG, "This may be caused by:");
                TS_LOGE(TAG, "  - Insufficient heap memory");
                TS_LOGE(TAG, "  - VFS/FATFS resources not properly released from previous unmount");
                TS_LOGE(TAG, "  - Maximum number of FATFS volumes reached (FF_VOLUMES=%d)", FF_VOLUMES);
                /* 打印堆内存信息帮助诊断 */
                TS_LOGE(TAG, "Free heap: %lu bytes, min free: %lu bytes",
                        (unsigned long)esp_get_free_heap_size(),
                        (unsigned long)esp_get_minimum_free_heap_size());
                /* 尝试获取下一个可用的 pdrv */
                {
                    BYTE next_pdrv = 0xFF;
                    esp_err_t pdrv_err = ff_diskio_get_drive(&next_pdrv);
                    TS_LOGE(TAG, "ff_diskio_get_drive() returned %s, pdrv=%d",
                            esp_err_to_name(pdrv_err), (int)next_pdrv);
                }
                /* 转储当前所有注册的 VFS 路径，帮助诊断 VFS 泄漏 */
                TS_LOGE(TAG, "=== Registered VFS paths (check for /sdcard leak) ===");
                esp_vfs_dump_registered_paths(stdout);
                break;
            case ESP_FAIL:
                TS_LOGE(TAG, "Failed to mount filesystem");
                break;
            default:
                TS_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
                break;
        }
        s_card = NULL;
        // 给 SDMMC 驱动时间完成清理
        vTaskDelay(pdMS_TO_TICKS(100));
        return ret;
    }
    
    /* Store mount info */
    strncpy(s_mount_point, cfg.mount_point, sizeof(s_mount_point) - 1);
    
    ts_storage_set_sd_mounted(true, s_mount_point);
    ++s_generation;
    
    /* Log card info */
    TS_LOGI(TAG, "SD card mounted:");
    TS_LOGI(TAG, "  Name: %s", s_card->cid.name);
    TS_LOGI(TAG, "  Type: %s", (s_card->ocr & (1 << 30)) ? "SDHC/SDXC" : "SDSC");
    TS_LOGI(TAG, "  Speed: %s", (s_card->csd.tr_speed > 25000000) ? "high speed" : "default");
    TS_LOGI(TAG, "  Size: %lluMB", ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    
    /* 发布 SD 卡挂载事件，通知其他组件（如 ts_config_file）*/
    ts_event_post(TS_EVENT_BASE_STORAGE, TS_EVT_STORAGE_SD_MOUNTED, 
                  s_mount_point, strlen(s_mount_point) + 1, 0);
    
    return ESP_OK;
}

static esp_err_t unmount_sd_locked(void)
{
    if (s_card == NULL) {
        return ESP_OK;
    }
    
    TS_LOGI(TAG, "Unmounting SD card from %s...", s_mount_point);
    
    /* 诊断：打印 unmount 前的状态 */
    BYTE pdrv_before = ff_diskio_get_pdrv_card(s_card);
    TS_LOGI(TAG, "Before unmount: card=%p, pdrv=%d, mount_point=%s", 
            s_card, (int)pdrv_before, s_mount_point);
    TS_LOGI(TAG, "=== VFS paths BEFORE unmount ===");
    esp_vfs_dump_registered_paths(stdout);
    
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    
    /* 
     * 关键诊断：打印 esp_vfs_fat_sdcard_unmount 的返回值
     * 根据 ESP-IDF 源码，这个函数内部调用：
     * 1. unmount_card_core() -> esp_vfs_fat_unregister_path()
     * 如果 esp_vfs_fat_unregister_path() 失败，VFS 上下文 (s_fat_ctxs[]) 不会被释放
     */
    TS_LOGI(TAG, "esp_vfs_fat_sdcard_unmount returned: %s (0x%x)", 
            esp_err_to_name(ret), ret);
    
    /* 无论成功失败都打印 unmount 后的 VFS 状态 */
    TS_LOGI(TAG, "=== VFS paths AFTER unmount ===");
    esp_vfs_dump_registered_paths(stdout);
    
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
        /* 尝试获取更多诊断信息 */
        BYTE pdrv_after = ff_diskio_get_pdrv_card(s_card);
        TS_LOGE(TAG, "After failed unmount: pdrv=%d (0xff means not found)", (int)pdrv_after);
        return ret;
    }
    
    /* 
     * 重要：esp_vfs_fat_sdcard_unmount() 内部会：
     * 1. f_mount(NULL) - 卸载 FAT 文件系统
     * 2. ff_diskio_unregister() - 取消 diskio 注册
     * 3. call_host_deinit() - 调用 sdmmc_host_deinit() 释放 SDMMC host
     * 4. free(card) - 释放 card 结构体内存
     * 5. esp_vfs_fat_unregister_path() - 取消 VFS 路径注册（释放 s_fat_ctxs[] 槽位）
     * 
     * 因此卸载后 s_card 指针已无效，必须设为 NULL
     */
    
    /* 关键诊断：检查 FATFS 内部的 s_fat_ctxs[] 是否被正确清理 */
    {
        uint64_t total = 0, free_bytes = 0;
        esp_err_t info_ret = esp_vfs_fat_info("/sdcard", &total, &free_bytes);
        if (info_ret == ESP_OK) {
            TS_LOGE(TAG, "WARNING: /sdcard STILL exists in FATFS s_fat_ctxs[] after unmount!");
            TS_LOGE(TAG, "This is a resource leak - s_fat_ctxs[] slot not properly released");
        } else {
            TS_LOGI(TAG, "Good: /sdcard removed from FATFS s_fat_ctxs[] (info returned %s)", 
                    esp_err_to_name(info_ret));
        }
    }
    
    /* 发布 SD 卡卸载事件（在清理 s_card 之前，确保事件处理器能获取信息）*/
    ts_event_post(TS_EVENT_BASE_STORAGE, TS_EVT_STORAGE_SD_UNMOUNTED, 
                  s_mount_point, strlen(s_mount_point) + 1, 0);
    
    /* 清理状态 - card 内存已由 esp_vfs_fat_sdcard_unmount 释放 */
    s_card = NULL;
    ++s_generation;
    ts_storage_set_sd_mounted(false, NULL);
    
    /* 给驱动时间完成清理，确保下次挂载不会冲突 */
    vTaskDelay(pdMS_TO_TICKS(100));
    
    TS_LOGI(TAG, "SD card unmounted successfully");
    
    return ESP_OK;
}

bool ts_storage_sd_mounted(void)
{
    ts_storage_sd_lock();bool mounted=s_card!=NULL;ts_storage_sd_unlock();return mounted;
}

static esp_err_t sd_stats_locked(ts_storage_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    FATFS *fs;
    DWORD free_clusters;
    
    FRESULT res = f_getfree(s_mount_point, &free_clusters, &fs);
    if (res != FR_OK) {
        return ESP_FAIL;
    }
    
    uint64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
    uint64_t free_sectors = free_clusters * fs->csize;
    uint32_t sector_size = s_card->csd.sector_size;
    
    stats->total_bytes = total_sectors * sector_size;
    stats->free_bytes = free_sectors * sector_size;
    stats->used_bytes = stats->total_bytes - stats->free_bytes;
    
    return ESP_OK;
}

esp_err_t ts_storage_format_sd(void)
{
    ts_storage_sd_lock();
    esp_err_t ret=s_card ? esp_vfs_fat_sdcard_format(s_mount_point,s_card) : ESP_ERR_INVALID_STATE;
    /* Even a failed format may have changed card contents. Old editors cannot
     * keep using the previous configuration's mount identity. */
    if(s_card)++s_generation;
    ts_storage_sd_unlock();return ret;
}

static esp_err_t sd_info_locked(uint64_t *capacity, size_t *sector_size)
{
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (capacity) {
        *capacity = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    }
    
    if (sector_size) {
        *sector_size = s_card->csd.sector_size;
    }
    
    return ESP_OK;
}

esp_err_t ts_storage_mount_sd(const ts_sd_config_t *config)
{
    ts_storage_sd_lock();esp_err_t ret=mount_sd_locked(config);ts_storage_sd_unlock();return ret;
}
esp_err_t ts_storage_unmount_sd(void)
{
    ts_storage_sd_lock();esp_err_t ret=unmount_sd_locked();ts_storage_sd_unlock();return ret;
}
esp_err_t ts_storage_sd_stats(ts_storage_stats_t *stats)
{
    ts_storage_sd_lock();esp_err_t ret=sd_stats_locked(stats);ts_storage_sd_unlock();return ret;
}
esp_err_t ts_storage_sd_info(uint64_t *capacity,size_t *sector_size)
{
    ts_storage_sd_lock();esp_err_t ret=sd_info_locked(capacity,sector_size);ts_storage_sd_unlock();return ret;
}
esp_err_t ts_storage_sd_refresh(void)
{
    ts_storage_sd_lock();
    esp_err_t ret=s_card ? sdmmc_get_status(s_card) : ESP_ERR_NOT_FOUND;
    if(ret!=ESP_OK){
        if(s_card)ret=unmount_sd_locked();
        else ret=ESP_OK;
        if(ret==ESP_OK)ret=mount_sd_locked(NULL);
    }
    ts_storage_sd_unlock();return ret;
}

/* Compare the resolved directory name, including FAT aliases/case rules. */
bool ts_storage_sd_name_is(const char *path, const char *name)
{
    ts_storage_sd_lock();
    bool matches = false;
    size_t mount_length = strlen(s_mount_point);
    if (s_card && mount_length && !strncmp(path, s_mount_point, mount_length) && path[mount_length] == '/') {
        BYTE drive = ff_diskio_get_pdrv_card(s_card);
        FILINFO info;
        char fat_path[TS_STORAGE_MAX_PATH];
        if (drive != 0xff) {
            snprintf(fat_path, sizeof(fat_path), "%u:%s", drive, path + mount_length);
            matches = f_stat(fat_path, &info) == FR_OK && !strcasecmp(info.fname, name);
        }
    }
    ts_storage_sd_unlock();
    return matches;
}
