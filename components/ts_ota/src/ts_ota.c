/**
 * @file ts_ota.c
 * @brief TianShanOS OTA Core Implementation
 */

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_timer.h"
#include "ts_ota.h"
#include "ts_event.h"

static const char *TAG = "ts_ota";

// ============================================================================
//                           Static Variables
// ============================================================================

static bool s_initialized = false;
static SemaphoreHandle_t s_ota_mutex = NULL;
static ts_ota_state_t s_state = TS_OTA_STATE_IDLE;
static ts_ota_error_t s_error = TS_OTA_ERR_NONE;
static ts_ota_progress_cb_t s_progress_cb = NULL;
static void *s_user_data = NULL;

// Progress tracking
static size_t s_total_size = 0;
static size_t s_received_size = 0;
static char s_status_msg[64] = "空闲";

// Upload state
static esp_ota_handle_t s_upload_handle = 0;
static const esp_partition_t *s_upload_partition = NULL;
static bool s_upload_in_progress = false;

// Rollback timer
static esp_timer_handle_t s_rollback_timer = NULL;
static uint32_t s_rollback_timeout_sec = 0;
static int64_t s_rollback_start_time = 0;

// ============================================================================
//                           Forward Declarations
// ============================================================================

static void ota_set_state(ts_ota_state_t state, const char *msg);
static void ota_set_error(ts_ota_error_t error, const char *msg);
static void ota_notify_progress(void);
static void rollback_timer_callback(void *arg);

// ============================================================================
//                           Core Implementation
// ============================================================================

esp_err_t ts_ota_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // Create mutex
    s_ota_mutex = xSemaphoreCreateMutex();
    if (!s_ota_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Check if we're running from an OTA partition
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Running from partition: %s (addr=0x%lx, size=%lu KB)",
                 running->label, running->address, running->size / 1024);
    }

    // Check for pending verification (new firmware just booted)
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "Firmware pending verification - starting rollback timer");
            
            // Start rollback timer
            s_rollback_timeout_sec = CONFIG_TS_OTA_ROLLBACK_TIMEOUT;
            s_rollback_start_time = esp_timer_get_time();
            
            esp_timer_create_args_t timer_args = {
                .callback = rollback_timer_callback,
                .arg = NULL,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "ota_rollback"
            };
            
            if (esp_timer_create(&timer_args, &s_rollback_timer) == ESP_OK) {
                esp_timer_start_once(s_rollback_timer, 
                                     (uint64_t)s_rollback_timeout_sec * 1000000);
            }
            
            // Post event
            ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_ROLLBACK_PENDING,
                          &s_rollback_timeout_sec, sizeof(s_rollback_timeout_sec), 0);
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OTA subsystem initialized");
    
    return ESP_OK;
}

esp_err_t ts_ota_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_rollback_timer) {
        esp_timer_stop(s_rollback_timer);
        esp_timer_delete(s_rollback_timer);
        s_rollback_timer = NULL;
    }

    if (s_ota_mutex) {
        vSemaphoreDelete(s_ota_mutex);
        s_ota_mutex = NULL;
    }

    s_initialized = false;
    return ESP_OK;
}

esp_err_t ts_ota_get_status(ts_ota_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    
    status->state = s_state;
    
    // Get running partition info
    ts_ota_get_running_partition_info(&status->running);
    
    // Get next partition info
    ts_ota_get_next_partition_info(&status->next);
    
    // Check pending verification
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        status->pending_verify = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    
    // Calculate remaining rollback timeout
    if (s_rollback_timer && status->pending_verify) {
        int64_t elapsed_us = esp_timer_get_time() - s_rollback_start_time;
        int64_t remaining_us = (int64_t)s_rollback_timeout_sec * 1000000 - elapsed_us;
        status->rollback_timeout = (remaining_us > 0) ? (remaining_us / 1000000) : 0;
    }
    
    xSemaphoreGive(s_ota_mutex);

    return ESP_OK;
}

esp_err_t ts_ota_get_progress(ts_ota_progress_t *progress)
{
    if (!progress) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    
    progress->state = s_state;
    progress->error = s_error;
    progress->total_size = s_total_size;
    progress->received_size = s_received_size;
    
    if (s_total_size > 0) {
        progress->progress_percent = (uint8_t)((s_received_size * 100) / s_total_size);
    } else {
        progress->progress_percent = 0;
    }
    
    progress->status_msg = s_status_msg;
    
    xSemaphoreGive(s_ota_mutex);

    return ESP_OK;
}

esp_err_t ts_ota_abort(void)
{
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);

    /* SD has no cancellation handshake; a committed boot choice cannot be
     * undone by clearing this status either. Keep the real worker/result. */
    if (ts_ota_sdcard_is_running() || s_state == TS_OTA_STATE_PENDING_REBOOT) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    if (s_state == TS_OTA_STATE_IDLE) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_OK;
    }

    // Abort HTTPS OTA if running
    if (ts_ota_https_is_running()) {
        ts_ota_abort_https();
    }

    // Abort upload if in progress
    if (s_upload_in_progress && s_upload_handle) {
        esp_ota_abort(s_upload_handle);
        s_upload_handle = 0;
        s_upload_in_progress = false;
    }

    ota_set_state(TS_OTA_STATE_IDLE, "已中止");
    s_error = TS_OTA_ERR_ABORTED;
    
    xSemaphoreGive(s_ota_mutex);
    
    // Post event
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_ABORTED, NULL, 0, 0);
    
    ESP_LOGI(TAG, "OTA update aborted");
    return ESP_OK;
}

// ============================================================================
//                           Rollback Implementation
// ============================================================================

static void rollback_timer_callback(void *arg)
{
    ESP_LOGW(TAG, "Rollback timer expired - initiating rollback");
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_ROLLBACK_EXECUTED, NULL, 0, 0);
    ts_ota_rollback();
}

esp_err_t ts_ota_mark_valid(void)
{
    esp_err_t ret;

    // Stop rollback timer
    if (s_rollback_timer) {
        esp_timer_stop(s_rollback_timer);
        esp_timer_delete(s_rollback_timer);
        s_rollback_timer = NULL;
        s_rollback_timeout_sec = 0;
    }

    // Mark current partition as valid
    ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark firmware valid: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Firmware marked as valid - rollback cancelled");
    
    // Post event
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_VALIDATED, NULL, 0, 0);

    return ESP_OK;
}

bool ts_ota_is_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        return (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    
    return false;
}

bool ts_ota_can_rollback(void)
{
    // Check if there's a valid app in the other slot that we can rollback to
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return false;
    }
    
    // Find the other OTA partition
    const esp_partition_t *other = NULL;
    if (strcmp(running->label, "ota_0") == 0) {
        other = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    } else if (strcmp(running->label, "ota_1") == 0) {
        other = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    }
    
    if (!other) {
        return false;
    }
    
    // Check if the other partition has a valid app
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(other, &desc) != ESP_OK) {
        return false;  // No valid app header
    }
    
    // Check the OTA state of the other partition
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(other, &ota_state) == ESP_OK) {
        // Can only rollback to VALID or NEW apps, not INVALID or ABORTED
        if (ota_state == ESP_OTA_IMG_INVALID || ota_state == ESP_OTA_IMG_ABORTED) {
            return false;
        }
    }
    
    return true;
}

esp_err_t ts_ota_rollback(void)
{
    esp_err_t ret;

    ESP_LOGW(TAG, "Initiating rollback to previous firmware");

    ret = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // This should not be reached as the system reboots
    return ESP_OK;
}

uint32_t ts_ota_get_rollback_timeout(void)
{
    if (!s_rollback_timer || !ts_ota_is_pending_verify()) {
        return 0;
    }
    
    int64_t elapsed_us = esp_timer_get_time() - s_rollback_start_time;
    int64_t remaining_us = (int64_t)s_rollback_timeout_sec * 1000000 - elapsed_us;
    
    return (remaining_us > 0) ? (remaining_us / 1000000) : 0;
}

// ============================================================================
//                           Upload Implementation
// ============================================================================

esp_err_t ts_ota_upload_begin(size_t total_size)
{
    esp_err_t ret;

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);

    if (s_state != TS_OTA_STATE_IDLE) {
        xSemaphoreGive(s_ota_mutex);
        ESP_LOGE(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Get next update partition
    s_upload_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_upload_partition) {
        xSemaphoreGive(s_ota_mutex);
        ESP_LOGE(TAG, "No OTA partition available");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "OTA upload starting, target partition: %s (size: %lu KB)",
             s_upload_partition->label, s_upload_partition->size / 1024);

    // Begin OTA
    ret = esp_ota_begin(s_upload_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_upload_handle);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_ota_mutex);
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_upload_in_progress = true;
    s_total_size = total_size;
    s_received_size = 0;
    s_error = TS_OTA_ERR_NONE;
    
    ota_set_state(TS_OTA_STATE_WRITING, "正在写入固件...");
    
    xSemaphoreGive(s_ota_mutex);

    // Post event
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_STARTED, NULL, 0, 0);

    return ESP_OK;
}

esp_err_t ts_ota_upload_write(const void *data, size_t len)
{
    esp_err_t ret;

    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);

    if (!s_upload_in_progress || !s_upload_handle) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ret = esp_ota_write(s_upload_handle, data, len);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_ota_mutex);
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
        ota_set_error(TS_OTA_ERR_WRITE_FAILED, "写入失败");
        return ret;
    }

    s_received_size += len;
    
    // Update progress message
    if (s_total_size > 0) {
        uint8_t percent = (uint8_t)((s_received_size * 100) / s_total_size);
        snprintf(s_status_msg, sizeof(s_status_msg), 
                 "正在写入... %u%%", percent);
    } else {
        snprintf(s_status_msg, sizeof(s_status_msg), 
                 "正在写入... %zu KB", s_received_size / 1024);
    }

    xSemaphoreGive(s_ota_mutex);

    ota_notify_progress();

    return ESP_OK;
}

esp_err_t ts_ota_upload_end(bool auto_reboot)
{
    esp_err_t ret;

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);

    if (!s_upload_in_progress || !s_upload_handle) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // End OTA
    ret = esp_ota_end(s_upload_handle);
    if (ret != ESP_OK) {
        s_upload_handle = 0;
        s_upload_in_progress = false;
        xSemaphoreGive(s_ota_mutex);
        
        if (ret == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Firmware validation failed");
            ota_set_error(TS_OTA_ERR_VERIFY_FAILED, "固件校验失败");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
            ota_set_error(TS_OTA_ERR_INTERNAL, "完成失败");
        }
        return ret;
    }

    // Set boot partition
    ret = esp_ota_set_boot_partition(s_upload_partition);
    if (ret != ESP_OK) {
        s_upload_handle = 0;
        s_upload_in_progress = false;
        xSemaphoreGive(s_ota_mutex);
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        ota_set_error(TS_OTA_ERR_INTERNAL, "设置启动分区失败");
        return ret;
    }

    s_upload_handle = 0;
    s_upload_in_progress = false;
    
    ESP_LOGI(TAG, "OTA update completed successfully, %zu bytes written", s_received_size);
    
    if (auto_reboot) {
        ota_set_state(TS_OTA_STATE_PENDING_REBOOT, "升级完成，正在重启...");
    } else {
        ota_set_state(TS_OTA_STATE_PENDING_REBOOT, "升级完成，等待重启");
    }

    xSemaphoreGive(s_ota_mutex);

    // Post event
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_COMPLETED, NULL, 0, 0);

    if (auto_reboot) {
        ESP_LOGI(TAG, "Rebooting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    return ESP_OK;
}

esp_err_t ts_ota_upload_abort(void)
{
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);

    if (s_upload_in_progress && s_upload_handle) {
        esp_ota_abort(s_upload_handle);
        s_upload_handle = 0;
        s_upload_in_progress = false;
        ESP_LOGI(TAG, "OTA upload aborted");
    }

    ota_set_state(TS_OTA_STATE_IDLE, "已取消");
    
    xSemaphoreGive(s_ota_mutex);

    return ESP_OK;
}

// ============================================================================
//                           Helper Functions
// ============================================================================

static void ota_set_state(ts_ota_state_t state, const char *msg)
{
    s_state = state;
    if (msg) {
        strncpy(s_status_msg, msg, sizeof(s_status_msg) - 1);
        s_status_msg[sizeof(s_status_msg) - 1] = '\0';
    }
}

static void ota_set_error(ts_ota_error_t error, const char *msg)
{
    s_state = TS_OTA_STATE_ERROR;
    s_error = error;
    if (msg) {
        strncpy(s_status_msg, msg, sizeof(s_status_msg) - 1);
        s_status_msg[sizeof(s_status_msg) - 1] = '\0';
    }
    
    // Post event
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_FAILED, &error, sizeof(error), 0);
}

static void ota_notify_progress(void)
{
    if (s_progress_cb) {
        ts_ota_progress_t progress;
        ts_ota_get_progress(&progress);
        s_progress_cb(&progress, s_user_data);
    }
    
    // Post event
    ts_ota_progress_t progress;
    ts_ota_get_progress(&progress);
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_PROGRESS, 
                  &progress, sizeof(progress), 0);
}

// ============================================================================
//                           Internal API (for OTA sub-modules)
// ============================================================================

void ts_ota_update_progress(ts_ota_state_t state, size_t received, size_t total, const char *msg)
{
    if (!s_ota_mutex) return;
    
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    
    s_state = state;
    s_received_size = received;
    s_total_size = total;
    
    if (msg) {
        strncpy(s_status_msg, msg, sizeof(s_status_msg) - 1);
        s_status_msg[sizeof(s_status_msg) - 1] = '\0';
    }
    
    xSemaphoreGive(s_ota_mutex);
    
    // Notify via callback and event
    ota_notify_progress();
}

void ts_ota_set_error(ts_ota_error_t error, const char *msg)
{
    if (!s_ota_mutex) return;
    
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    
    s_state = TS_OTA_STATE_ERROR;
    s_error = error;
    
    if (msg) {
        strncpy(s_status_msg, msg, sizeof(s_status_msg) - 1);
        s_status_msg[sizeof(s_status_msg) - 1] = '\0';
    }
    
    xSemaphoreGive(s_ota_mutex);
    
    // Post event
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_FAILED, &error, sizeof(error), 0);
}

void ts_ota_set_completed(const char *msg)
{
    if (!s_ota_mutex) return;
    
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    
    s_state = TS_OTA_STATE_PENDING_REBOOT;
    s_error = TS_OTA_ERR_NONE;
    
    if (msg) {
        strncpy(s_status_msg, msg, sizeof(s_status_msg) - 1);
        s_status_msg[sizeof(s_status_msg) - 1] = '\0';
    }
    
    xSemaphoreGive(s_ota_mutex);
    
    // Post event
    ts_event_post(TS_EVENT_BASE_OTA, TS_EVENT_OTA_COMPLETED, NULL, 0, 0);
}
