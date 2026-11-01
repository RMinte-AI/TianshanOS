/**
 * @file ts_ota_sdcard.c
 * @brief TianShanOS OTA SD Card Implementation
 */

#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"

/* PSRAM-first allocation for OTA buffers */
#define OTA_MALLOC(size) ({ void *p = heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); p ? p : malloc(size); })
#define OTA_FLASH_SYNC_BUFFER_SIZE 4096
#include "esp_app_format.h"
#include "ts_ota.h"
#include "ts_event.h"

static const char *TAG = "ts_ota_sdcard";

// OTA task handle
static TaskHandle_t s_ota_task_handle = NULL;
static ts_ota_config_t s_ota_config;
static char *s_ota_url = NULL;
static bool s_ota_running = false;

// Forward declarations
static void sdcard_ota_task(void *arg);

/**
 * @brief Start SD Card OTA update
 */
esp_err_t ts_ota_start_sdcard(const ts_ota_config_t *config)
{
    if (!config || !config->url) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_running) {
        ESP_LOGE(TAG, "OTA already running");
        return ESP_ERR_INVALID_STATE;
    }

    // Check if file exists
    struct stat st;
    if (stat(config->url, &st) != 0) {
        ESP_LOGE(TAG, "Firmware file not found: %s", config->url);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Firmware file: %s, size: %ld bytes", config->url, st.st_size);

    size_t url_len = strlen(config->url) + 1;
    char *url_copy = OTA_MALLOC(url_len);
    if (!url_copy) {
        ESP_LOGE(TAG, "Failed to allocate URL buffer");
        return ESP_ERR_NO_MEM;
    }
    memcpy(url_copy, config->url, url_len);

    if (s_ota_url) {
        free(s_ota_url);
    }
    s_ota_url = url_copy;

    // Copy config and keep the file path valid for the OTA task lifetime.
    memcpy(&s_ota_config, config, sizeof(ts_ota_config_t));
    s_ota_config.url = s_ota_url;
    
    // Create OTA task
    s_ota_running = true;
    BaseType_t ret = xTaskCreate(
        sdcard_ota_task,
        "ota_sdcard",
        CONFIG_TS_OTA_TASK_STACK_SIZE,
        NULL,
        CONFIG_TS_OTA_TASK_PRIORITY,
        &s_ota_task_handle
    );

    if (ret != pdPASS) {
        s_ota_running = false;
        ESP_LOGE(TAG, "Failed to create OTA task");
        free(s_ota_url);
        s_ota_url = NULL;
        s_ota_config.url = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool ts_ota_sdcard_is_running(void)
{
    return s_ota_running;
}

/**
 * @brief Synchronously flash app firmware from a local file
 */
esp_err_t ts_ota_flash_file_sync(const char *filepath,
                                  bool allow_downgrade,
                                  ts_ota_progress_cb_t progress_cb,
                                  void *user_data)
{
    esp_err_t ret = ESP_OK;
    FILE *f = NULL;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    uint8_t *buffer = NULL;
    size_t file_size = 0;

    if (!filepath || strlen(filepath) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_running) {
        ESP_LOGE(TAG, "OTA already running");
        return ESP_ERR_INVALID_STATE;
    }

    struct stat st;
    if (stat(filepath, &st) != 0) {
        ESP_LOGE(TAG, "Firmware file not found: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0) {
        ESP_LOGE(TAG, "Invalid firmware file size: %ld", (long)st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }
    file_size = (size_t)st.st_size;

    s_ota_running = true;
    ts_ota_update_progress(TS_OTA_STATE_DOWNLOADING, 0, 0, "正在读取固件文件...");
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_STARTED, NULL, 0, 0);

    ESP_LOGI(TAG, "Synchronously flashing firmware from: %s", filepath);
    ESP_LOGI(TAG, "Firmware size: %zu bytes", file_size);

    f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open firmware file");
        ts_ota_set_error(TS_OTA_ERR_FILE_NOT_FOUND, "打开固件文件失败");
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    buffer = heap_caps_malloc(OTA_FLASH_SYNC_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate internal OTA buffer");
        ts_ota_set_error(TS_OTA_ERR_INTERNAL, "内部内存不足");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ts_ota_update_progress(TS_OTA_STATE_VERIFYING, 0, file_size, "正在验证固件...");

    size_t header_size = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    size_t read_len = fread(buffer, 1, header_size, f);
    if (read_len < header_size) {
        ESP_LOGE(TAG, "Failed to read firmware header");
        ts_ota_set_error(TS_OTA_ERR_VERIFY_FAILED, "读取固件头部失败");
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    esp_app_desc_t *app_desc = (esp_app_desc_t *)(buffer + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (app_desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "Invalid firmware magic word");
        ts_ota_set_error(TS_OTA_ERR_VERIFY_FAILED, "无效的固件格式");
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    ESP_LOGI(TAG, "New firmware: %s, version: %s", app_desc->project_name, app_desc->version);
    ESP_LOGI(TAG, "Compiled: %s %s", app_desc->date, app_desc->time);

#if CONFIG_TS_OTA_VERSION_CHECK
    if (!allow_downgrade) {
        const esp_app_desc_t *running_app = esp_app_get_description();
        int cmp = ts_ota_compare_versions(app_desc->version, running_app->version);
        if (cmp < 0) {
            ESP_LOGE(TAG, "Downgrade not allowed: %s -> %s",
                     running_app->version, app_desc->version);
            ts_ota_set_error(TS_OTA_ERR_VERSION_MISMATCH, "不允许固件降级");
            ret = ESP_ERR_INVALID_VERSION;
            goto cleanup;
        }
    }
#endif

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition available");
        ts_ota_set_error(TS_OTA_ERR_NO_PARTITION, "没有可用的 OTA 分区");
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Target partition: %s (addr=0x%lx, size=%lu KB)",
             update_partition->label, update_partition->address,
             update_partition->size / 1024);

    if (file_size > update_partition->size) {
        ESP_LOGE(TAG, "Firmware too large for partition");
        ts_ota_set_error(TS_OTA_ERR_PARTITION_FULL, "固件过大，超出分区容量");
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    ret = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        ts_ota_set_error(TS_OTA_ERR_WRITE_FAILED, "OTA 初始化失败");
        goto cleanup;
    }

    fseek(f, 0, SEEK_SET);
    ts_ota_update_progress(TS_OTA_STATE_WRITING, 0, file_size, "正在写入固件...");

    size_t written = 0;
    while (written < file_size) {
        size_t to_read = file_size - written;
        if (to_read > OTA_FLASH_SYNC_BUFFER_SIZE) {
            to_read = OTA_FLASH_SYNC_BUFFER_SIZE;
        }

        read_len = fread(buffer, 1, to_read, f);
        if (read_len != to_read) {
            ESP_LOGE(TAG, "Read error: %zu / %zu bytes", read_len, to_read);
            ts_ota_set_error(TS_OTA_ERR_DOWNLOAD_FAILED, "读取文件失败");
            ret = ESP_FAIL;
            goto cleanup;
        }

        ret = esp_ota_write(ota_handle, buffer, read_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
            ts_ota_set_error(TS_OTA_ERR_WRITE_FAILED, "写入闪存失败");
            goto cleanup;
        }

        written += read_len;
        int percent = (written * 100) / file_size;
        ESP_LOGI(TAG, "Written: %zu / %zu bytes (%d%%)", written, file_size, percent);

        ts_ota_update_progress(TS_OTA_STATE_WRITING, written, file_size, "正在写入固件...");
        if (progress_cb) {
            ts_ota_progress_t progress = {
                .state = TS_OTA_STATE_WRITING,
                .error = TS_OTA_ERR_NONE,
                .total_size = file_size,
                .received_size = written,
                .progress_percent = percent,
                .status_msg = "正在写入..."
            };
            progress_cb(&progress, user_data);
        }

        ts_ota_progress_t event_progress = {
            .state = TS_OTA_STATE_WRITING,
            .total_size = file_size,
            .received_size = written,
            .progress_percent = percent,
        };
        ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_PROGRESS,
                      &event_progress, sizeof(event_progress), 0);

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ts_ota_update_progress(TS_OTA_STATE_VERIFYING, written, file_size, "正在验证固件...");

    ret = esp_ota_end(ota_handle);
    ota_handle = 0;
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            ts_ota_set_error(TS_OTA_ERR_VERIFY_FAILED, "固件验证失败");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
            ts_ota_set_error(TS_OTA_ERR_WRITE_FAILED, "OTA 完成失败");
        }
        goto cleanup;
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        ts_ota_set_error(TS_OTA_ERR_WRITE_FAILED, "设置启动分区失败");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Synchronous firmware flash completed successfully!");
    ts_ota_set_completed("升级完成，等待重启");
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_COMPLETED, NULL, 0, 0);

    if (progress_cb) {
        ts_ota_progress_t progress = {
            .state = TS_OTA_STATE_PENDING_REBOOT,
            .error = TS_OTA_ERR_NONE,
            .total_size = file_size,
            .received_size = written,
            .progress_percent = 100,
            .status_msg = "升级完成，等待重启"
        };
        progress_cb(&progress, user_data);
    }

cleanup:
    if (f) {
        fclose(f);
    }
    if (buffer) {
        free(buffer);
    }
    if (ota_handle) {
        esp_ota_abort(ota_handle);
    }

    s_ota_running = false;
    return ret;
}

/**
 * @brief SD Card OTA task
 */
static void sdcard_ota_task(void *arg)
{
    esp_err_t ret;
    FILE *f = NULL;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    uint8_t *buffer = NULL;
    
    ESP_LOGI(TAG, "Starting SD Card OTA from: %s", s_ota_config.url);

    // Update global state - starting
    ts_ota_update_progress(TS_OTA_STATE_DOWNLOADING, 0, 0, "正在读取固件文件...");

    // Post start event
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_STARTED, NULL, 0, 0);

    // Open firmware file
    f = fopen(s_ota_config.url, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open firmware file");
        ts_ota_set_error(TS_OTA_ERR_FILE_NOT_FOUND, "打开固件文件失败");
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    ESP_LOGI(TAG, "Firmware size: %zu bytes", file_size);

    // Allocate buffer (PSRAM first)
    buffer = OTA_MALLOC(CONFIG_TS_OTA_BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        ts_ota_set_error(TS_OTA_ERR_INTERNAL, "内存分配失败");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    // Update state - verifying
    ts_ota_update_progress(TS_OTA_STATE_VERIFYING, 0, file_size, "正在验证固件...");

    // Read and verify app description
    size_t read_len = fread(buffer, 1, sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t), f);
    if (read_len < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
        ESP_LOGE(TAG, "Failed to read firmware header");
        ts_ota_set_error(TS_OTA_ERR_VERIFY_FAILED, "读取固件头部失败");
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    // Parse app description
    esp_app_desc_t *app_desc = (esp_app_desc_t *)(buffer + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    
    if (app_desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "Invalid firmware magic word");
        ts_ota_set_error(TS_OTA_ERR_VERIFY_FAILED, "无效的固件格式");
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    ESP_LOGI(TAG, "New firmware: %s, version: %s", app_desc->project_name, app_desc->version);
    ESP_LOGI(TAG, "Compiled: %s %s", app_desc->date, app_desc->time);

    // Version check
    #if CONFIG_TS_OTA_VERSION_CHECK
    if (!s_ota_config.allow_downgrade) {
        const esp_app_desc_t *running_app = esp_app_get_description();
        int cmp = ts_ota_compare_versions(app_desc->version, running_app->version);
        if (cmp < 0) {
            ESP_LOGE(TAG, "Downgrade not allowed: %s -> %s", 
                     running_app->version, app_desc->version);
            ts_ota_set_error(TS_OTA_ERR_VERSION_MISMATCH, "不允许固件降级");
            ret = ESP_ERR_INVALID_VERSION;
            goto cleanup;
        }
    }
    #endif

    // Seek back to start
    fseek(f, 0, SEEK_SET);

    // Get update partition
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition available");
        ts_ota_set_error(TS_OTA_ERR_NO_PARTITION, "没有可用的 OTA 分区");
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    ESP_LOGI(TAG, "Target partition: %s (addr=0x%lx, size=%lu KB)",
             update_partition->label, update_partition->address, 
             update_partition->size / 1024);

    // Check partition size
    if (file_size > update_partition->size) {
        ESP_LOGE(TAG, "Firmware too large for partition");
        ts_ota_set_error(TS_OTA_ERR_PARTITION_FULL, "固件过大，超出分区容量");
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    // Update state - writing
    ts_ota_update_progress(TS_OTA_STATE_WRITING, 0, file_size, "正在开始写入...");

    // Begin OTA
    ret = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        ts_ota_set_error(TS_OTA_ERR_WRITE_FAILED, "OTA 初始化失败");
        goto cleanup;
    }

    // Write firmware
    size_t written = 0;
    while (written < file_size) {
        read_len = fread(buffer, 1, CONFIG_TS_OTA_BUFFER_SIZE, f);
        if (read_len == 0) {
            if (feof(f)) {
                break;
            }
            ESP_LOGE(TAG, "Read error");
            ts_ota_set_error(TS_OTA_ERR_DOWNLOAD_FAILED, "读取文件失败");
            ret = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

        ret = esp_ota_write(ota_handle, buffer, read_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
            ts_ota_set_error(TS_OTA_ERR_WRITE_FAILED, "写入闪存失败");
            goto cleanup;
        }

        written += read_len;

        // Progress update
        int percent = (written * 100) / file_size;
        ESP_LOGI(TAG, "Written: %zu / %zu bytes (%d%%)", written, file_size, percent);

        // Update global state
        ts_ota_update_progress(TS_OTA_STATE_WRITING, written, file_size, "正在写入固件...");
        if (s_ota_config.progress_cb) {
            ts_ota_progress_t progress = {
                .state = TS_OTA_STATE_WRITING,
                .error = TS_OTA_ERR_NONE,
                .total_size = file_size,
                .received_size = written,
                .progress_percent = percent,
                .status_msg = "正在写入..."
            };
            s_ota_config.progress_cb(&progress, s_ota_config.user_data);
        }

        // Post progress event
        ts_ota_progress_t progress = {
            .state = TS_OTA_STATE_WRITING,
            .total_size = file_size,
            .received_size = written,
            .progress_percent = percent,
        };
        ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_PROGRESS, 
                      &progress, sizeof(progress), 0);

        vTaskDelay(pdMS_TO_TICKS(10)); // Yield to other tasks
    }

    // End OTA
    ts_ota_update_progress(TS_OTA_STATE_VERIFYING, written, file_size, "正在验证固件...");
    
    ret = esp_ota_end(ota_handle);
    ota_handle = 0;
    
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            ts_ota_set_error(TS_OTA_ERR_VERIFY_FAILED, "固件验证失败");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
            ts_ota_set_error(TS_OTA_ERR_WRITE_FAILED, "OTA 完成失败");
        }
        goto cleanup;
    }

    // Set boot partition
    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        ts_ota_set_error(TS_OTA_ERR_WRITE_FAILED, "设置启动分区失败");
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA update successful!");

    // Set global state - completed
    ts_ota_set_completed("升级完成，等待重启");

    // Post completion event
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_COMPLETED, NULL, 0, 0);

    // Notify callback
    if (s_ota_config.progress_cb) {
        ts_ota_progress_t progress = {
            .state = TS_OTA_STATE_PENDING_REBOOT,
            .error = TS_OTA_ERR_NONE,
            .total_size = file_size,
            .received_size = written,
            .progress_percent = 100,
            .status_msg = "升级完成，等待重启"
        };
        s_ota_config.progress_cb(&progress, s_ota_config.user_data);
    }

    // Auto reboot if requested
    if (s_ota_config.auto_reboot) {
        ESP_LOGI(TAG, "Rebooting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

cleanup:
    if (f) fclose(f);
    if (buffer) free(buffer);
    if (ota_handle) esp_ota_abort(ota_handle);

    // Error state already set by ts_ota_set_error calls above
    // Just notify callback if provided
    if (ret != ESP_OK && s_ota_config.progress_cb) {
        ts_ota_progress_t progress;
        ts_ota_get_progress(&progress);  // Get current error state
        s_ota_config.progress_cb(&progress, s_ota_config.user_data);
    }

    s_ota_running = false;
    s_ota_task_handle = NULL;
    if (s_ota_url) {
        free(s_ota_url);
        s_ota_url = NULL;
        s_ota_config.url = NULL;
    }
    vTaskDelete(NULL);
}

/**
 * @brief List firmware files on SD card
 */
esp_err_t ts_ota_list_sdcard_firmwares(const char *dir_path, 
                                        char firmwares[][64], 
                                        size_t *count,
                                        size_t max_count)
{
    if (!dir_path || !firmwares || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *count < max_count) {
        // Check for .bin extension
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".bin") == 0) {
            strncpy(firmwares[*count], entry->d_name, 63);
            firmwares[*count][63] = '\0';
            (*count)++;
        }
    }

    closedir(dir);
    return ESP_OK;
}
