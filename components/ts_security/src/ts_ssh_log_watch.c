#include "ts_ssh_log_watch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ts_keystore.h"
#include "ts_ssh_client.h"
#include "ts_ssh_hosts_config.h"
#include "ts_ssh_probe.h"
#include "ts_ssh_service.h"
#include "ts_variable.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_TS_SSH_LOG_WATCH_MAX
#define CONFIG_TS_SSH_LOG_WATCH_MAX 32
#endif
#define LOG_WATCH_TASK_STACK_SIZE (8 * 1024)
#define LOG_WATCH_TASK_PRIORITY 5

typedef struct {
    ts_ssh_log_watch_config_t *config;
    TaskHandle_t task;
    uint32_t generation;
    atomic_bool cancel;
    int64_t deadline;
} watch_slot_t;
static watch_slot_t slots[CONFIG_TS_SSH_LOG_WATCH_MAX];
static SemaphoreHandle_t mutex;
static const char *TAG = "ssh_log_watch";

/* Initialized by the security service before API/rule execution starts. */
esp_err_t ts_ssh_log_watch_init(void) {
    if (!mutex)
        mutex = xSemaphoreCreateMutex();
    return mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool cancelled(void *context) {
    watch_slot_t *slot = context;
    return atomic_load(&slot->cancel) || esp_timer_get_time() >= slot->deadline;
}

static void publish(watch_slot_t *slot, const char *status) {
    const ts_ssh_log_watch_config_t *cfg = slot->config;
    /* Generation/state is checked atomically with publication by service registry.
     * Cancellation alone never declares the remote process stopped. */
    if (!atomic_load(&slot->cancel))
        ts_ssh_service_observe(cfg->command_id, cfg->run_generation, status, cfg->var_name);
}

static esp_err_t check_log(watch_slot_t *slot, const char *command, int *match) {
    ts_ssh_host_config_t host;
    ts_ssh_session_t session = NULL;
    ts_ssh_exec_result_t result = {0};
    char *key = NULL;
    size_t key_len = 0;
    esp_err_t ret = ts_ssh_hosts_config_get(slot->config->host_id, &host);
    if (ret != ESP_OK)
        goto done;
    if (!host.keyid[0]) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }
    ret = ts_keystore_load_private_key(host.keyid, &key, &key_len);
    if (ret != ESP_OK)
        goto done;
    ts_ssh_config_t cfg = TS_SSH_DEFAULT_CONFIG();
    cfg.host = host.host;
    cfg.port = host.port;
    cfg.username = host.username;
    cfg.auth_method = TS_SSH_AUTH_PUBLICKEY;
    cfg.auth.key.private_key = (const uint8_t *)key;
    cfg.auth.key.private_key_len = key_len;
    cfg.timeout_ms = 5000;
    cfg.cancelled = cancelled;
    cfg.cancel_context = slot;
    cfg.max_output_bytes = 128;
    ret = ts_ssh_session_create(&cfg, &session);
    if (ret == ESP_OK)
        ret = ts_ssh_connect(session);
    if (ret == ESP_OK)
        ret = ts_ssh_service_verify_instance(slot->config->command_id, slot->config->run_generation, session);
    if (ret == ESP_OK)
        ret = ts_ssh_exec(session, command, &result);
    if (ret == ESP_OK)
        ret = ts_ssh_service_verify_instance(slot->config->command_id, slot->config->run_generation, session);
    if (ret == ESP_OK) {
        if (result.exit_code != 0)
            ret = ESP_FAIL;
        else if (ts_ssh_probe_token(result.stdout_data, "FAIL"))
            *match = -1;
        else if (ts_ssh_probe_token(result.stdout_data, "READY"))
            *match = 1;
        else if (ts_ssh_probe_token(result.stdout_data, "WAITING") ||
                 ts_ssh_probe_token(result.stdout_data, "NOTFOUND"))
            *match = 0;
        else
            ret = ESP_ERR_INVALID_RESPONSE;
    }
done:
    ts_ssh_exec_result_free(&result);
    if (session)
        ts_ssh_session_destroy(session);
    free(key);
    return ret;
}

static void worker(void *context) {
    watch_slot_t *slot = context;
    /* Creator registers the task handle and initial state before releasing gate. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    char *command = NULL;
    if (slot->config->recovery_only) {
        while (!cancelled(slot)) {
            if (!ts_ssh_service_recover(slot->config->command_id, slot->config->run_generation)) break;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(slot->config->check_interval_ms));
        }
        goto done;
    }
    command = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!command || !ts_ssh_log_probe_command(slot->config->log_file, slot->config->ready_pattern,
                                              slot->config->fail_pattern, command, 3072)) {
        publish(slot, "unknown");
        goto done;
    }
    while (!cancelled(slot)) {
        int match = 0;
        esp_err_t ret = check_log(slot, command, &match);
        if (cancelled(slot))
            break;
        if (ret == ESP_OK && match) {
            publish(slot, match > 0 ? "ready" : "failed");
            goto done;
        }
        if (ret != ESP_OK) {
            publish(slot, "unknown");
            if (ret == TS_SSH_ERR_HOST_UNKNOWN || ret == TS_SSH_ERR_HOST_CHANGED ||
                ret == ESP_ERR_INVALID_STATE)
                goto done;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(slot->config->check_interval_ms));
    }
    if (!atomic_load(&slot->cancel))
        publish(slot, "timeout");
done:
    free(command);
    xSemaphoreTake(mutex, portMAX_DELAY);
    ts_ssh_log_watch_config_t *cfg = slot->config;
    slot->config = NULL;
    slot->task = NULL;
    xSemaphoreGive(mutex);
    if (cfg->recovery_only)
        ts_ssh_service_recovery_done(cfg->command_id, cfg->run_generation);
    else
        ts_ssh_service_recovery_kick(cfg->command_id);
    free(cfg);
    vTaskDelete(NULL); /* Only the resource-owning task deletes itself. */
}

esp_err_t ts_ssh_log_watch_start(const ts_ssh_log_watch_config_t *cfg,
                                 ts_ssh_log_watch_handle_t *out) {
    if (out)
        *out = 0;
    if (!mutex)
        return ESP_ERR_INVALID_STATE;
    if (!cfg || !cfg->host_id[0] || (!cfg->recovery_only && (!cfg->log_file[0] || !cfg->ready_pattern[0])) ||
        !cfg->var_name[0] || !cfg->command_id[0] || !cfg->run_generation || !cfg->timeout_sec ||
        !cfg->check_interval_ms)
        return ESP_ERR_INVALID_ARG;
    if (!ts_ssh_service_observation_valid(cfg->command_id, cfg->run_generation))
        return ESP_ERR_INVALID_STATE;
    ts_ssh_log_watch_config_t *copy =
        heap_caps_malloc(sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy)
        return ESP_ERR_NO_MEM;
    *copy = *cfg;
    xSemaphoreTake(mutex, portMAX_DELAY);
    int index = -1;
    for (int i = 0; i < CONFIG_TS_SSH_LOG_WATCH_MAX; i++) {
        if (slots[i].config && (!strcmp(slots[i].config->var_name, cfg->var_name) ||
                                !strcmp(slots[i].config->command_id, cfg->command_id))) {
            xSemaphoreGive(mutex);
            free(copy);
            return ESP_ERR_INVALID_STATE;
        }
        if (!slots[i].config && slots[i].generation < 0xffffff && index < 0)
            index = i;
    }
    if (index < 0) {
        xSemaphoreGive(mutex);
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    watch_slot_t *slot = &slots[index];
    slot->config = copy;
    ++slot->generation;
    atomic_store(&slot->cancel, false);
    slot->deadline = esp_timer_get_time() + (int64_t)cfg->timeout_sec * 1000000;
    BaseType_t ret = xTaskCreatePinnedToCore(worker, "ssh_log_watch", LOG_WATCH_TASK_STACK_SIZE,
                                             slot, LOG_WATCH_TASK_PRIORITY, &slot->task, 1);
    if (ret != pdPASS) {
        slot->config = NULL;
        slot->task = NULL;
        xSemaphoreGive(mutex);
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    if (out)
        *out = (slot->generation << 8) | (index + 1);
    /* Publication before gate; worker never uses a partly registered slot. */
    if (!cfg->recovery_only) publish(slot, "checking");
    xTaskNotifyGive(slot->task);
    xSemaphoreGive(mutex);
    return ESP_OK;
}

static watch_slot_t *find_handle(ts_ssh_log_watch_handle_t handle) {
    unsigned index = (handle & 255);
    if (!index || index > CONFIG_TS_SSH_LOG_WATCH_MAX)
        return NULL;
    watch_slot_t *slot = &slots[index - 1];
    return slot->config && slot->generation == (handle >> 8) ? slot : NULL;
}
esp_err_t ts_ssh_log_watch_stop(ts_ssh_log_watch_handle_t handle) {
    if (!mutex)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(mutex, portMAX_DELAY);
    watch_slot_t *slot = find_handle(handle);
    if (slot) {
        atomic_store(&slot->cancel, true);
        ts_ssh_service_cancel_observation(slot->config->command_id, slot->config->run_generation);
        xTaskNotifyGive(slot->task);
    }
    xSemaphoreGive(mutex);
    return slot ? ESP_OK : ESP_ERR_NOT_FOUND;
}
bool ts_ssh_log_watch_pending(ts_ssh_log_watch_handle_t handle) {
    if (!mutex)
        return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool found = find_handle(handle) != NULL;
    xSemaphoreGive(mutex);
    return found;
}
bool ts_ssh_log_watch_is_running(const char *name) {
    if (!mutex || !name)
        return false;
    bool found = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < CONFIG_TS_SSH_LOG_WATCH_MAX; i++)
        if (slots[i].config && !strcmp(slots[i].config->var_name, name))
            found = true;
    xSemaphoreGive(mutex);
    return found;
}
esp_err_t ts_ssh_log_watch_cancel_name(const char *name) {
    if (!mutex || !name)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < CONFIG_TS_SSH_LOG_WATCH_MAX; i++)
        if (slots[i].config && !strcmp(slots[i].config->var_name, name)) {
            atomic_store(&slots[i].cancel, true);
            ts_ssh_service_cancel_observation(slots[i].config->command_id,
                                              slots[i].config->run_generation);
            xTaskNotifyGive(slots[i].task);
        }
    xSemaphoreGive(mutex);
    return ESP_OK;
}
void ts_ssh_log_watch_stop_all(void) {
    if (!mutex)
        return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < CONFIG_TS_SSH_LOG_WATCH_MAX; i++)
        if (slots[i].config) {
            atomic_store(&slots[i].cancel, true);
            ts_ssh_service_cancel_observation(slots[i].config->command_id,
                                              slots[i].config->run_generation);
            xTaskNotifyGive(slots[i].task);
        }
    xSemaphoreGive(mutex);
}
int ts_ssh_log_watch_get_active_count(void) {
    if (!mutex)
        return 0;
    int count = 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < CONFIG_TS_SSH_LOG_WATCH_MAX; i++)
        count += slots[i].config != NULL;
    xSemaphoreGive(mutex);
    return count;
}
void ts_ssh_log_watch_list(void) {
    ESP_LOGI(TAG, "Active watchers (including cancelling): %d",
             ts_ssh_log_watch_get_active_count());
}
