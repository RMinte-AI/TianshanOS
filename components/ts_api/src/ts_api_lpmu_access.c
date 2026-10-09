/**
 * @file ts_api_lpmu_access.c
 * @brief LPMU upper-network access API
 */

#include "ts_api.h"
#include "ts_core.h"
#include "ts_keystore.h"
#include "ts_known_hosts.h"
#include "ts_scp.h"
#include "ts_ssh_client.h"
#include "ts_ssh_hosts_config.h"
#include "ts_storage.h"
#include "ts_usb_mux.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "api_lpmu"

#define LPMU_HOST              "10.10.99.99"
#define LPMU_PORT              22
#define LPMU_REMOTE_TARBALL    "lpmu-agx-network-setup.tar.gz"
#define LPMU_STACK_SIZE        8192
#define LPMU_OUTPUT_TAIL_MAX   1024
#define LPMU_LOG_PATH          "/sdcard/logs/lpmu-access.log"
#define LPMU_LOG_CHUNK         1024

extern const uint8_t lpmu_archive_start[] asm("_binary_lpmu_agx_network_setup_tar_gz_start");
extern const uint8_t lpmu_archive_end[] asm("_binary_lpmu_agx_network_setup_tar_gz_end");

typedef enum {
    LPMU_STAGE_IDLE = 0,
    LPMU_STAGE_QUEUED,
    LPMU_STAGE_FINDING_HOST,
    LPMU_STAGE_LOADING_KEY,
    LPMU_STAGE_CONNECTING,
    LPMU_STAGE_VERIFYING_HOST,
    LPMU_STAGE_CHECKING_REMOTE,
    LPMU_STAGE_UPLOADING,
    LPMU_STAGE_EXTRACTING,
    LPMU_STAGE_CHMOD,
    LPMU_STAGE_RUNNING_SCRIPT,
    LPMU_STAGE_SUCCESS,
    LPMU_STAGE_FAILED,
} lpmu_stage_t;

typedef struct {
    uint32_t run_id;
    bool running;
    lpmu_stage_t stage;
    lpmu_stage_t result_stage; /* Published only after run resources are closed. */
    esp_err_t esp_error;
    int exit_code;
    uint32_t bytes_transferred;
    uint32_t bytes_total;
    uint32_t output_bytes;
    size_t output_len;
    size_t log_len;
    esp_err_t log_error;
    bool internet_confirmed;
    bool configuration_complete;
    char script_stdout_tail[64];
    size_t script_stdout_len;
    char last_error[160];
    char output_tail[LPMU_OUTPUT_TAIL_MAX];
    /* Fixed-size measurements; exported only when diagnostics=true. */
    uint64_t log_init_us;
    uint64_t log_write_us;
    uint64_t log_write_max_us;
    uint64_t log_read_us;
    uint64_t log_read_max_us;
    uint32_t log_write_count;
    uint32_t log_read_count;
    uint32_t stack_min_free_bytes;
} lpmu_status_t;

static SemaphoreHandle_t s_lpmu_mutex = NULL;
static TaskHandle_t s_lpmu_task = NULL;
static uint32_t s_next_run_id = 0;
static int s_run_log = -1;
static lpmu_status_t s_status = {
    .stage = LPMU_STAGE_IDLE,
    .exit_code = -1,
};

static size_t lpmu_archive_size(void)
{
    return (size_t)(lpmu_archive_end - lpmu_archive_start);
}

static void lpmu_secure_zero(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

static const char *lpmu_stage_str(lpmu_stage_t stage)
{
    switch (stage) {
        case LPMU_STAGE_IDLE: return "idle";
        case LPMU_STAGE_QUEUED: return "queued";
        case LPMU_STAGE_FINDING_HOST: return "finding_host";
        case LPMU_STAGE_LOADING_KEY: return "loading_key";
        case LPMU_STAGE_CONNECTING: return "connecting";
        case LPMU_STAGE_VERIFYING_HOST: return "verifying_host";
        case LPMU_STAGE_CHECKING_REMOTE: return "checking_remote";
        case LPMU_STAGE_UPLOADING: return "uploading";
        case LPMU_STAGE_EXTRACTING: return "extracting";
        case LPMU_STAGE_CHMOD: return "chmod";
        case LPMU_STAGE_RUNNING_SCRIPT: return "running_script";
        case LPMU_STAGE_SUCCESS: return "success";
        case LPMU_STAGE_FAILED: return "failed";
        default: return "unknown";
    }
}

static esp_err_t lpmu_ensure_mutex(void)
{
    if (s_lpmu_mutex) {
        return ESP_OK;
    }

    s_lpmu_mutex = xSemaphoreCreateMutex();
    return s_lpmu_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static void lpmu_lock(void)
{
    if (s_lpmu_mutex) {
        xSemaphoreTake(s_lpmu_mutex, portMAX_DELAY);
    }
}

static void lpmu_unlock(void)
{
    if (s_lpmu_mutex) {
        xSemaphoreGive(s_lpmu_mutex);
    }
}

static void lpmu_set_stage(lpmu_stage_t stage)
{
    lpmu_lock();
    s_status.stage = stage;
    lpmu_unlock();
}

static void lpmu_set_error_locked(esp_err_t err, int exit_code, const char *fmt, va_list args)
{
    s_status.result_stage = LPMU_STAGE_FAILED;
    s_status.esp_error = err;
    s_status.exit_code = exit_code;
    vsnprintf(s_status.last_error, sizeof(s_status.last_error), fmt, args);
}

static void lpmu_fail(esp_err_t err, int exit_code, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    lpmu_lock();
    lpmu_set_error_locked(err, exit_code, fmt, args);
    lpmu_unlock();
    va_end(args);
}

static void lpmu_success(int exit_code)
{
    lpmu_lock();
    s_status.result_stage = LPMU_STAGE_SUCCESS;
    s_status.esp_error = ESP_OK;
    s_status.exit_code = exit_code;
    s_status.last_error[0] = '\0';
    lpmu_unlock();
}

static void lpmu_finish(void)
{
    // running stays true while close can block. Readers and new starts remain
    // excluded; status queries need not wait for SD I/O.
    int close_result = 0;
    if (s_run_log >= 0) {
        close_result = close(s_run_log);
        s_run_log = -1;
    }
    uint32_t stack_free = s_lpmu_task ? uxTaskGetStackHighWaterMark(NULL) : UINT32_MAX;
    lpmu_lock();
    if (close_result != 0) s_status.log_error = ESP_FAIL;
    s_status.stage = s_status.result_stage;
    s_status.stack_min_free_bytes = stack_free;
    s_status.running = false;
    s_lpmu_task = NULL;
    lpmu_unlock();
}

static void lpmu_append_tail_locked(const char *data, size_t len)
{
    const size_t cap = sizeof(s_status.output_tail) - 1;

    if (!data || len == 0 || cap == 0) {
        return;
    }

    if (UINT32_MAX - s_status.output_bytes < len) {
        s_status.output_bytes = UINT32_MAX;
    } else {
        s_status.output_bytes += (uint32_t)len;
    }

    if (len >= cap) {
        memcpy(s_status.output_tail, data + len - cap, cap);
        s_status.output_len = cap;
        s_status.output_tail[cap] = '\0';
        return;
    }

    if (s_status.output_len + len > cap) {
        size_t drop = s_status.output_len + len - cap;
        memmove(s_status.output_tail, s_status.output_tail + drop, s_status.output_len - drop);
        s_status.output_len -= drop;
    }

    memcpy(s_status.output_tail + s_status.output_len, data, len);
    s_status.output_len += len;
    s_status.output_tail[s_status.output_len] = '\0';
}

static void lpmu_append_log(const char *data, size_t len)
{
    // Only the running task owns this descriptor; another start is excluded.
    lpmu_lock();
    bool available = s_status.log_error == ESP_OK;
    lpmu_unlock();
    if (s_run_log < 0 || !available) return;
    int64_t started = esp_timer_get_time();
    ssize_t written = write(s_run_log, data, len);
    uint64_t elapsed = esp_timer_get_time() - started;
    lpmu_lock();
    s_status.log_write_count++;
    s_status.log_write_us += elapsed;
    if (elapsed > s_status.log_write_max_us) s_status.log_write_max_us = elapsed;
    if (written > 0) s_status.log_len += (size_t)written;
    if (written < 0 || (size_t)written != len) s_status.log_error = ESP_FAIL;
    lpmu_unlock();
}

static void lpmu_record_command(const char *command)
{
    lpmu_append_log("$ ", 2);
    lpmu_append_log(command, strlen(command));
    lpmu_append_log("\n", 1);
}

static void lpmu_output_cb(const char *data, size_t len, bool is_stderr, void *user_data)
{
    (void)user_data;

    lpmu_lock();
    lpmu_append_tail_locked(data, len);
    if (s_status.stage == LPMU_STAGE_RUNNING_SCRIPT && !is_stderr) {
        // Retain stdout overlap so SSH chunk boundaries and interleaved stderr
        // cannot hide or manufacture the script's two confirmation messages.
        char scan[sizeof(s_status.script_stdout_tail) + 512 + 1];
        const char *cursor = data;
        size_t remaining = len;
        while (remaining) {
            size_t chunk = remaining < 512 ? remaining : 512;
            size_t total = s_status.script_stdout_len + chunk;
            memcpy(scan, s_status.script_stdout_tail, s_status.script_stdout_len);
            memcpy(scan + s_status.script_stdout_len, cursor, chunk);
            scan[total] = '\0';
            if (strstr(scan, "✓ 互联网连接正常！")) s_status.internet_confirmed = true;
            if (strstr(scan, "=== 配置完成 ===")) s_status.configuration_complete = true;
            size_t keep = total < sizeof(s_status.script_stdout_tail) ? total : sizeof(s_status.script_stdout_tail) - 1;
            memcpy(s_status.script_stdout_tail, scan + total - keep, keep);
            s_status.script_stdout_len = keep;
            cursor += chunk;
            remaining -= chunk;
        }
    }
    lpmu_unlock();
    lpmu_append_log(data, len);
}

static esp_err_t lpmu_run_command(ts_ssh_session_t session, const char *command, int *exit_code)
{
    int code = -1;
    lpmu_record_command(command);
    esp_err_t ret = ts_ssh_exec_stream(session, command, lpmu_output_cb, NULL, &code);
    if (exit_code) {
        *exit_code = code;
    }
    return ret;
}

static bool lpmu_verify_known_host(ts_ssh_session_t session, char *err, size_t err_size)
{
    ts_host_verify_result_t verify_result = TS_HOST_VERIFY_ERROR;
    ts_known_host_t host_info = {0};
    esp_err_t ret = ts_known_hosts_verify(session, &verify_result, &host_info);

    if (ret != ESP_OK) {
        snprintf(err, err_size, "known-host verification failed: %s", esp_err_to_name(ret));
        return false;
    }

    if (verify_result == TS_HOST_VERIFY_OK) {
        return true;
    }

    if (verify_result == TS_HOST_VERIFY_NOT_FOUND) {
        snprintf(err, err_size, "known-host missing for %s:%d", LPMU_HOST, LPMU_PORT);
    } else if (verify_result == TS_HOST_VERIFY_MISMATCH) {
        snprintf(err, err_size, "known-host mismatch for %s:%d", LPMU_HOST, LPMU_PORT);
    } else {
        snprintf(err, err_size, "known-host verification error for %s:%d", LPMU_HOST, LPMU_PORT);
    }
    return false;
}

static void lpmu_task(void *arg)
{
    char *sudo_input = (char *)arg;

    ts_ssh_session_t session = NULL;
    char *private_key = NULL;
    size_t private_key_len = 0;
    ts_ssh_host_config_t host_cfg = {0};
    int exit_code = -1;
    esp_err_t ret;
    char verify_error[128] = {0};

    int64_t log_started = esp_timer_get_time();
    esp_err_t log_error = ts_storage_mkdir_p("/sdcard/logs");
    if (log_error == ESP_OK) {
        s_run_log = open(LPMU_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (s_run_log < 0) log_error = ESP_FAIL;
    }
    uint64_t log_elapsed = esp_timer_get_time() - log_started;
    lpmu_lock();
    s_status.log_error = log_error;
    s_status.log_init_us = log_elapsed;
    lpmu_unlock();

    lpmu_set_stage(LPMU_STAGE_FINDING_HOST);
    ret = ts_ssh_hosts_config_find(LPMU_HOST, LPMU_PORT, NULL, &host_cfg);
    if (ret != ESP_OK) {
        lpmu_fail(ret, -1, "SSH host %s:%d is not configured", LPMU_HOST, LPMU_PORT);
        goto cleanup;
    }

    if (!host_cfg.enabled || host_cfg.auth_type != TS_SSH_HOST_AUTH_KEY || !host_cfg.keyid[0]) {
        lpmu_fail(ESP_ERR_INVALID_STATE, -1, "SSH host %s:%d must use enabled key auth", LPMU_HOST, LPMU_PORT);
        goto cleanup;
    }

    lpmu_set_stage(LPMU_STAGE_LOADING_KEY);
    ret = ts_keystore_load_private_key(host_cfg.keyid, &private_key, &private_key_len);
    if (ret != ESP_OK) {
        lpmu_fail(ret, -1, "failed to load SSH key '%s': %s", host_cfg.keyid, esp_err_to_name(ret));
        goto cleanup;
    }

    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    config.host = host_cfg.host;
    config.port = host_cfg.port ? host_cfg.port : LPMU_PORT;
    config.username = host_cfg.username;
    config.auth_method = TS_SSH_AUTH_PUBLICKEY;
    config.auth.key.private_key = (const uint8_t *)private_key;
    config.auth.key.private_key_len = private_key_len;
    config.auth.key.private_key_path = NULL;
    config.auth.key.passphrase = NULL;
    config.timeout_ms = 30000;
    config.verify_host_key = false;

    lpmu_set_stage(LPMU_STAGE_CONNECTING);
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        lpmu_fail(ret, -1, "failed to create SSH session: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = ts_ssh_connect(session);
    if (ret != ESP_OK) {
        lpmu_fail(ret, -1, "SSH connect failed: %s", ts_ssh_get_error(session));
        goto cleanup;
    }

    lpmu_set_stage(LPMU_STAGE_VERIFYING_HOST);
    if (!lpmu_verify_known_host(session, verify_error, sizeof(verify_error))) {
        lpmu_fail(ESP_ERR_INVALID_STATE, -1, "%s", verify_error);
        goto cleanup;
    }

    lpmu_set_stage(LPMU_STAGE_CHECKING_REMOTE);
    ret = lpmu_run_command(session,
                           "test -f \"$HOME/lpmu-agx-network-setup/lpmu/setup-smart-route.sh\"",
                           &exit_code);
    if (ret != ESP_OK) {
        lpmu_fail(ret, exit_code, "remote check failed: %s", ts_ssh_get_error(session));
        goto cleanup;
    }

    if (exit_code != 0) {
        lpmu_set_stage(LPMU_STAGE_UPLOADING);
        lpmu_lock();
        s_status.bytes_transferred = 0;
        s_status.bytes_total = (uint32_t)lpmu_archive_size();
        lpmu_unlock();

        ret = ts_scp_send_buffer(session, lpmu_archive_start, lpmu_archive_size(),
                                 LPMU_REMOTE_TARBALL, 0644);
        if (ret != ESP_OK) {
            lpmu_fail(ret, -1, "SCP upload failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }

        lpmu_lock();
        s_status.bytes_transferred = (uint32_t)lpmu_archive_size();
        lpmu_unlock();

        lpmu_set_stage(LPMU_STAGE_EXTRACTING);
        ret = lpmu_run_command(session,
                               "mkdir -p \"$HOME/lpmu-agx-network-setup\" && "
                               "tar -xzf \"$HOME/lpmu-agx-network-setup.tar.gz\" "
                               "-C \"$HOME/lpmu-agx-network-setup\" --strip-components=1",
                               &exit_code);
        if (ret != ESP_OK || exit_code != 0) {
            lpmu_fail(ret != ESP_OK ? ret : ESP_FAIL, exit_code,
                      "remote extract failed (exit=%d): %s", exit_code,
                      ret != ESP_OK ? ts_ssh_get_error(session) : "tar command failed");
            goto cleanup;
        }
    }

    lpmu_set_stage(LPMU_STAGE_CHMOD);
    ret = lpmu_run_command(session,
                           "chmod +x \"$HOME/lpmu-agx-network-setup/lpmu/\"*.sh",
                           &exit_code);
    if (ret != ESP_OK || exit_code != 0) {
        lpmu_fail(ret != ESP_OK ? ret : ESP_FAIL, exit_code,
                  "remote chmod failed (exit=%d): %s", exit_code,
                  ret != ESP_OK ? ts_ssh_get_error(session) : "chmod command failed");
        goto cleanup;
    }

    lpmu_set_stage(LPMU_STAGE_RUNNING_SCRIPT);
    const char *command = "cd \"$HOME/lpmu-agx-network-setup/lpmu\" && "
                          "sudo -S -k -p '' -- ./setup-smart-route.sh";
    lpmu_record_command(command);
    ret = ts_ssh_exec_stream_input(session, command,
                                  sudo_input, strlen(sudo_input),
                                  lpmu_output_cb, NULL, &exit_code);
    if (ret != ESP_OK || exit_code != 0) {
        lpmu_fail(ret != ESP_OK ? ret : ESP_FAIL, exit_code,
                  "setup-smart-route failed (exit=%d): %s", exit_code,
                  ret != ESP_OK ? ts_ssh_get_error(session) : "remote script failed");
        goto cleanup;
    }

    lpmu_lock();
    bool confirmed = s_status.internet_confirmed && s_status.configuration_complete;
    lpmu_unlock();
    if (!confirmed) {
        lpmu_fail(ESP_ERR_INVALID_STATE, exit_code,
                  "script exited with 0 but did not report Internet connectivity and configuration completion");
        goto cleanup;
    }
    lpmu_success(exit_code);

cleanup:
    lpmu_secure_zero(sudo_input, strlen(sudo_input));
    free(sudo_input);
    if (session) {
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
    }
    if (private_key) {
        lpmu_secure_zero(private_key, private_key_len);
        free(private_key);
    }

    lpmu_finish();

    vTaskDelete(NULL);
}

static esp_err_t api_lpmu_access_start(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(params, "sudo_password");
    if (!cJSON_IsString(password) || !password->valuestring[0] ||
        strpbrk(password->valuestring, "\r\n")) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "A single-line sudo_password is required");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = lpmu_ensure_mutex();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Failed to initialize LPMU access state");
        return ret;
    }

    uint32_t run_id;
    lpmu_lock();
    if (s_status.running || s_lpmu_task != NULL) {
        lpmu_unlock();
        ts_api_result_error(result, TS_API_ERR_BUSY, "LPMU access task is already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (!ts_usb_mux_is_configured()) {
        lpmu_unlock();
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "USB MUX 未配置，无法确认是否已切换到 LPMU");
        return ESP_ERR_INVALID_STATE;
    }

    if (ts_usb_mux_get_target() != TS_USB_MUX_LPMU) {
        lpmu_unlock();
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "请先将 USB 切换到 LPMU 后再接入上层网络");
        return ESP_ERR_INVALID_STATE;
    }

    size_t password_len = strlen(password->valuestring);
    char *sudo_input = malloc(password_len + 2);
    if (!sudo_input) {
        lpmu_unlock();
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Failed to allocate sudo input");
        return ESP_ERR_NO_MEM;
    }
    memcpy(sudo_input, password->valuestring, password_len);
    sudo_input[password_len] = '\n';
    sudo_input[password_len + 1] = '\0';

    memset(&s_status, 0, sizeof(s_status));
    s_status.log_error = ESP_ERR_INVALID_STATE; /* No log until the task opens it. */
    s_status.stack_min_free_bytes = UINT32_MAX;
    s_status.run_id = ++s_next_run_id;
    if (s_next_run_id == 0) {
        s_next_run_id = 1;
        s_status.run_id = 1;
    }
    s_status.running = true;
    s_status.stage = LPMU_STAGE_QUEUED;
    s_status.exit_code = -1;
    s_status.bytes_total = (uint32_t)lpmu_archive_size();
    run_id = s_status.run_id;
    lpmu_unlock();

    BaseType_t task_ret = xTaskCreate(lpmu_task, "lpmu_access", LPMU_STACK_SIZE,
                                      sudo_input, 5, &s_lpmu_task);
    if (task_ret != pdPASS) {
        lpmu_secure_zero(sudo_input, password_len + 1);
        free(sudo_input);
        lpmu_fail(ESP_ERR_NO_MEM, -1, "failed to create LPMU access task");
        lpmu_finish();
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Failed to create LPMU access task");
        return ESP_ERR_NO_MEM;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "run_id", run_id);
    cJSON_AddStringToObject(data, "stage", lpmu_stage_str(LPMU_STAGE_QUEUED));
    cJSON_AddBoolToObject(data, "running", true);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

static esp_err_t api_lpmu_access_status(const cJSON *params, ts_api_result_t *result)
{
    esp_err_t ret = lpmu_ensure_mutex();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Failed to initialize LPMU access state");
        return ret;
    }

    lpmu_lock();
    lpmu_status_t snap = s_status;
    lpmu_unlock();

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "run_id", snap.run_id);
    cJSON_AddBoolToObject(data, "running", snap.running);
    cJSON_AddStringToObject(data, "stage", lpmu_stage_str(snap.stage));
    cJSON_AddNumberToObject(data, "exit_code", snap.exit_code);
    cJSON_AddNumberToObject(data, "esp_error", snap.esp_error);
    cJSON_AddNumberToObject(data, "bytes_transferred", snap.bytes_transferred);
    cJSON_AddNumberToObject(data, "bytes_total", snap.bytes_total);
    cJSON_AddNumberToObject(data, "output_bytes", snap.output_bytes);
    cJSON_AddBoolToObject(data, "output_truncated", snap.output_bytes >= LPMU_OUTPUT_TAIL_MAX);
    cJSON_AddBoolToObject(data, "internet_confirmed", snap.internet_confirmed);
    cJSON_AddBoolToObject(data, "configuration_complete", snap.configuration_complete);
    cJSON_AddStringToObject(data, "last_error", snap.last_error);
    cJSON_AddStringToObject(data, "output_tail", snap.output_tail);
    cJSON_AddStringToObject(data, "host", LPMU_HOST);
    cJSON_AddNumberToObject(data, "port", LPMU_PORT);

    const cJSON *diagnostics = cJSON_GetObjectItemCaseSensitive(params, "diagnostics");
    if (cJSON_IsTrue(diagnostics) || (cJSON_IsNumber(diagnostics) && diagnostics->valuedouble == 1)) {
        cJSON *diag = cJSON_AddObjectToObject(data, "diagnostics");
        cJSON_AddNumberToObject(diag, "log_init_us", snap.log_init_us);
        cJSON_AddNumberToObject(diag, "log_write_us", snap.log_write_us);
        cJSON_AddNumberToObject(diag, "log_write_max_us", snap.log_write_max_us);
        cJSON_AddNumberToObject(diag, "log_read_us", snap.log_read_us);
        cJSON_AddNumberToObject(diag, "log_read_max_us", snap.log_read_max_us);
        cJSON_AddNumberToObject(diag, "log_write_count", snap.log_write_count);
        cJSON_AddNumberToObject(diag, "log_read_count", snap.log_read_count);
        cJSON_AddNumberToObject(diag, "stack_allocated_bytes", LPMU_STACK_SIZE);
        if (snap.run_id && snap.stack_min_free_bytes != UINT32_MAX)
            cJSON_AddNumberToObject(diag, "stack_min_free_bytes", snap.stack_min_free_bytes);
        else cJSON_AddNullToObject(diag, "stack_min_free_bytes");
    }

    ts_api_result_ok(result, data);
    return ESP_OK;
}

static esp_err_t api_lpmu_access_log(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(params, "run_id");
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(params, "offset");
    double position = offset ? offset->valuedouble : 0;
    if (!cJSON_IsNumber(run) || (offset && !cJSON_IsNumber(offset)) || !(position >= 0)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "run_id and a nonnegative offset are required");
        return ESP_ERR_INVALID_ARG;
    }
    lpmu_lock();
    if (run->valuedouble != s_status.run_id || s_status.run_id == 0) {
        lpmu_unlock();
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Execution log for this run is no longer available");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_status.running) {
        lpmu_unlock();
        ts_api_result_error(result, TS_API_ERR_BUSY, "Execution log is still being written");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_status.log_error != ESP_OK) {
        lpmu_unlock();
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Execution log could not be saved to SD card");
        return ESP_FAIL;
    }
    if (!(position <= s_status.log_len) || position != (size_t)position) {
        lpmu_unlock();
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Invalid execution log offset");
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t run_id = s_status.run_id;
    size_t log_len = s_status.log_len;
    lpmu_unlock();

    char *chunk = TS_MALLOC_PSRAM_ONLY(LPMU_LOG_CHUNK + 1);
    if (!chunk) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Failed to allocate execution log read buffer");
        return ESP_ERR_NO_MEM;
    }
    int64_t read_started = esp_timer_get_time();
    int file = open(LPMU_LOG_PATH, O_RDONLY);
    size_t expected = log_len - (size_t)position;
    if (expected > LPMU_LOG_CHUNK) expected = LPMU_LOG_CHUNK;
    ssize_t received = -1;
    bool read_failed = file < 0;
    if (file >= 0) {
        struct stat info;
        off_t seek = (off_t)(size_t)position;
        read_failed = fstat(file, &info) != 0 || info.st_size < 0 ||
                      (uint64_t)info.st_size != log_len || seek < 0 ||
                      lseek(file, seek, SEEK_SET) != seek;
        if (!read_failed) received = read(file, chunk, expected);
        if (close(file) != 0) read_failed = true;
    }
    if (received < 0 || (size_t)received != expected) read_failed = true;
    size_t count = received > 0 ? (size_t)received : 0;
    // Keep a UTF-8 character intact across JSON pages.
    if (!read_failed && count) {
        size_t start = count;
        while (start && ((unsigned char)chunk[start - 1] & 0xc0) == 0x80) start--;
        if (start) {
            unsigned char lead = (unsigned char)chunk[start - 1];
            size_t width = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
            if (count - (start - 1) < width) {
                if (position + count == log_len) read_failed = true;
                else count = start - 1;
            }
        } else read_failed = true;
    }
    if (!count && position < log_len) read_failed = true;

    uint64_t read_elapsed = esp_timer_get_time() - read_started;
    // The next task may have replaced the file while the mutex was released.
    lpmu_lock();
    bool stale = s_status.run_id != run_id;
    if (!stale) {
        s_status.log_read_count++;
        s_status.log_read_us += read_elapsed;
        if (read_elapsed > s_status.log_read_max_us) s_status.log_read_max_us = read_elapsed;
    }
    lpmu_unlock();
    if (stale || read_failed) {
        free(chunk);
        ts_api_result_error(result, stale ? TS_API_ERR_NOT_FOUND : TS_API_ERR_INTERNAL,
                            stale ? "Execution log for this run is no longer available" :
                                    "Failed to read execution log from SD card");
        return stale ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    chunk[count] = '\0';
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "run_id", run_id);
    cJSON_AddStringToObject(data, "output", chunk);
    cJSON_AddNumberToObject(data, "next_offset", position + count);
    cJSON_AddBoolToObject(data, "done", position + count >= log_len);
    free(chunk);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

static const ts_api_endpoint_t lpmu_access_endpoints[] = {
    {
        .name = "network.lpmu_access.start",
        .description = "Start LPMU upper-network access setup",
        .category = TS_API_CAT_NETWORK,
        .handler = api_lpmu_access_start,
        .requires_auth = true,
        .permission = "network.config",
    },
    {
        .name = "network.lpmu_access.status",
        .description = "Get LPMU upper-network access setup status",
        .category = TS_API_CAT_NETWORK,
        .handler = api_lpmu_access_status,
        .requires_auth = true,
        .permission = "network.view",
    },
    {
        .name = "network.lpmu_access.log",
        .description = "Get the execution log for an LPMU access run",
        .category = TS_API_CAT_NETWORK,
        .handler = api_lpmu_access_log,
        .requires_auth = true,
        .permission = "network.view",
    },
};

esp_err_t ts_api_lpmu_access_register(void)
{
    esp_err_t ret = lpmu_ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    return ts_api_register_multiple(lpmu_access_endpoints,
                                    sizeof(lpmu_access_endpoints) / sizeof(lpmu_access_endpoints[0]));
}
