#include "ts_action_codec.h"
#include "ts_action_filter.h"
#include "ts_automation_config.h"
#include "ts_automation_runtime.h"
#include "ts_ssh_probe.h"
#include "ts_ssh_service.h"
#include <stdatomic.h>
/**
 * @file ts_action_manager.c
 * @brief TianShanOS Automation Engine - Action Manager Implementation
 *
 * Implements unified action execution for automation rules:
 * - SSH command execution (sync/async)
 * - LED control (board/touch/matrix)
 * - GPIO control (set level, pulse)
 * - Log, variable set, device control
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_action_manager.h"
#include "ts_rule_engine.h"
#include "ts_api.h"
#include "ts_variable.h"
#include "ts_event.h"
#include "ts_ssh_client.h"
#include "ts_ssh_commands_config.h"
#include "ts_ssh_hosts_config.h"
#include "ts_ssh_log_watch.h"
#include "ts_keystore.h"
#include "ts_led.h"
#include "ts_led_preset.h"
#include "ts_led_animation.h"
#include "ts_hal.h"
#include "ts_device_ctrl.h"
#include "esp_http_client.h"
#include "ts_core.h"
#include "ts_console.h"
#include "ts_storage.h"
#include "ts_config_module.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"  /* For xTaskCreateWithCaps */
#include "cJSON.h"
#include "esp_heap_caps.h"           /* For MALLOC_CAP_INTERNAL */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "ts_action_mgr";

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

/** Action executor task stack size
 * CRITICAL: Must use DRAM stack (not PSRAM) because this task performs
 * NVS/Flash operations. SPI Flash operations disable cache, and accessing
 * PSRAM with cache disabled causes a crash.
 * NOTE: SSH execution requires ~12KB stack (libssh2 + mbedTLS), so 16KB is safe.
 */
#define ACTION_TASK_STACK_SIZE      16384

/** Action executor task priority */
#define ACTION_TASK_PRIORITY        5
#define SERVICE_QUEUE_TIMEOUT_MS    30000

/*===========================================================================*/
/*                              Internal State                                */
/*===========================================================================*/

typedef struct {
    /* Action queue */
    QueueHandle_t action_queue;
    TaskHandle_t executor_task;
    atomic_bool running;
    atomic_bool accepting;
    bool direct_closed;
    unsigned direct_pending; /* caller + executor; at most one direct launch */
    unsigned pending; /* producers + queued + executing, protected by stats_mutex */

    /* Statistics */
    ts_action_stats_t stats;
    SemaphoreHandle_t stats_mutex;
    
    /* Initialization state */
    bool initialized;
} action_manager_ctx_t;

static action_manager_ctx_t *s_ctx = NULL;

typedef struct {
    atomic_uint refs;
    ts_ssh_command_config_t command;
    ts_action_ssh_host_t host;
    uint64_t host_instance;
    char owner_rule[TS_AUTO_NAME_MAX_LEN], config_version[33];
    char run_id[TS_SSH_RUN_ID_MAX];
    uint32_t control_id;
} action_binding_t;
typedef struct {
    atomic_uint refs;
    SemaphoreHandle_t semaphore;
    ts_action_result_t result;
} action_completion_t;
static void completion_release(action_completion_t *c) {
    if (c && atomic_fetch_sub(&c->refs, 1) == 1) {
        vSemaphoreDelete(c->semaphore);
        free(c);
    }
}
void ts_action_snapshot_owner(ts_auto_action_t *a, const char *rule, uint32_t instance) {
    a->runtime_epoch=ts_auto_config_execution_epoch();a->runtime_rule_instance=instance;
    action_binding_t *b = a->runtime_binding;
    if (b) snprintf(b->owner_rule,sizeof(b->owner_rule),"%s",rule);
}
static bool same_host(const ts_action_ssh_host_t *a,const ts_action_ssh_host_t *b)
{
    return !strcmp(a->id,b->id)&&!strcmp(a->host,b->host)&&a->port==b->port&&
        !strcmp(a->username,b->username)&&a->use_key_auth==b->use_key_auth&&
        !strcmp(a->password,b->password)&&!strcmp(a->key_path,b->key_path);
}
static const char *const cli_outputs[]={"exit_code","status","output"};
static uint64_t action_variable(const ts_auto_action_t *action,unsigned index)
{
    char name[80];
    if(action->type==TS_AUTO_ACT_SET_VAR)return ts_variable_instance(action->set_var.variable);
    snprintf(name,sizeof(name),"%.63s.%s",action->cli.var_name,cli_outputs[index]);
    return ts_variable_instance(name);
}
/* No remote I/O. The executor calls this under the publication/admission gate. */
bool ts_action_snapshot_current(const ts_auto_action_t *action)
{
    bool current=true;ts_ssh_binding_lock();
    if(action->runtime_epoch)current=action->runtime_epoch==ts_auto_config_execution_epoch();
    if(current&&action->runtime_rule_instance)current=ts_rule_instance_current(action->runtime_rule_instance);
    if(current&&action->runtime_template_instance)current=ts_auto_config_template_current(action->runtime_template_instance);
    unsigned variables=action->type==TS_AUTO_ACT_SET_VAR?1:action->type==TS_AUTO_ACT_CLI&&action->cli.var_name[0]?3:0;
    for(unsigned i=0;current&&i<variables;++i)current=action_variable(action,i)==action->runtime_variables[i];
    const action_binding_t *binding=action->runtime_binding;
    if(current&&binding){
        if(binding->command.id[0]){
            current=ts_auto_config_command_current(binding->command.id,binding->command.instance);
        }
        ts_action_ssh_host_t host;
        if(current)current=ts_auto_config_host_instance(binding->host.id)==binding->host_instance&&
            ts_action_get_ssh_host(binding->host.id,&host)==ESP_OK&&same_host(&host,&binding->host);
    }
    ts_ssh_binding_unlock();return current;
}
static bool action_result_begin(const ts_auto_action_t *origin)
{
    ts_variable_registration_begin();ts_ssh_binding_lock();
    if(!origin||ts_action_snapshot_current(origin))return true;
    ts_ssh_binding_unlock();ts_variable_registration_end();return false;
}
static void action_result_end(void)
{
    ts_ssh_binding_unlock();ts_variable_registration_end();
}
void ts_action_snapshot_retain(const ts_auto_action_t *a) {
    if (a && a->runtime_binding)
        atomic_fetch_add(&((action_binding_t *)a->runtime_binding)->refs, 1);
}
void ts_action_snapshot_release(ts_auto_action_t *a) {
    action_binding_t *b = a ? a->runtime_binding : NULL;
    if (b && atomic_fetch_sub(&b->refs, 1) == 1) {
        memset(b->host.password, 0, sizeof(b->host.password));
        free(b);
    }
    if (a)
        a->runtime_binding = NULL;
}
static esp_err_t snapshot_command(ts_auto_action_t *out, bool for_start, const char **reason) {
    action_binding_t *binding =
        heap_caps_calloc(1, sizeof(*binding), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!binding) {if(reason)*reason="out_of_memory";return ESP_ERR_NO_MEM;}
    atomic_init(&binding->refs, 1);
    esp_err_t ret = ts_ssh_commands_config_get(out->ssh_ref.cmd_id, &binding->command);
    const char *failed="command_unavailable";
    if (ret == ESP_OK) {ret = ts_action_get_ssh_host(binding->command.host_id, &binding->host);failed="command_host_unavailable";}
    if(ret==ESP_OK)binding->host_instance=ts_auto_config_host_instance(binding->host.id);
    if (ret == ESP_OK && for_start && !binding->command.enabled) {ret = ESP_ERR_INVALID_STATE;failed="command_disabled";}
    ts_auto_config_status_t status;ts_auto_config_status(&status);
    strcpy(binding->config_version,status.active_version);
    if (ret != ESP_OK) {
        if(reason)*reason=failed;
        free(binding);
        return ret;
    }
    out->runtime_binding = binding;
    out->runtime_epoch=ts_auto_config_execution_epoch();
    return ESP_OK;
}
esp_err_t ts_action_snapshot(const ts_auto_action_t *source, ts_auto_action_t *out) {
    *out = *source;
    out->runtime_binding = NULL;
    out->runtime_snapshot = true;
    out->runtime_epoch=out->runtime_template_instance=out->runtime_rule_instance=0;
    memset(out->runtime_variables,0,sizeof(out->runtime_variables));
    if (source->template_id[0]) {
        ts_action_template_t *tpl =
            heap_caps_malloc(sizeof(*tpl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tpl)
            return ESP_ERR_NO_MEM;
        esp_err_t ret = ts_action_template_get(source->template_id, tpl);
        if (ret == ESP_OK && !tpl->enabled)
            ret = ESP_ERR_INVALID_STATE;
        if (ret == ESP_OK) {
            *out = tpl->action;
            out->async |= tpl->async;
            out->delay_ms = source->delay_ms;
            out->repeat_mode = source->repeat_mode;
            out->repeat_count = source->repeat_count;
            out->repeat_interval_ms = source->repeat_interval_ms;
            out->condition = source->condition;
            out->template_id[0] = 0;
            out->runtime_binding = NULL;
            out->runtime_snapshot = true;
            out->runtime_template_instance=ts_auto_config_template_instance(source->template_id);
            out->runtime_epoch=ts_auto_config_execution_epoch();
        }
        free(tpl);
        if (ret != ESP_OK)
            return ret;
    }
    unsigned variables=out->type==TS_AUTO_ACT_SET_VAR?1:out->type==TS_AUTO_ACT_CLI&&out->cli.var_name[0]?3:0;
    if(variables)out->runtime_epoch=ts_auto_config_execution_epoch();
    for(unsigned i=0;i<variables;++i)out->runtime_variables[i]=action_variable(out,i);
    if(out->type==TS_AUTO_ACT_SSH_CMD){
        action_binding_t *binding=heap_caps_calloc(1,sizeof(*binding),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!binding)return ESP_ERR_NO_MEM;
        atomic_init(&binding->refs,1);
        esp_err_t ret=ts_action_get_ssh_host(out->ssh.host_ref,&binding->host);
        if(ret!=ESP_OK){free(binding);return ret;}
        out->runtime_binding=binding;out->runtime_epoch=ts_auto_config_execution_epoch();
        binding->host_instance=ts_auto_config_host_instance(binding->host.id);
    }
    if (out->type != TS_AUTO_ACT_SSH_CMD_REF)
        return ESP_OK;
    if (!source->template_id[0] && !source->runtime_snapshot)
        out->async = true;
    return snapshot_command(out, true, NULL);
}

/*===========================================================================*/
/*                          Forward Declarations                              */
/*===========================================================================*/

static void action_executor_task(void *arg);
static esp_err_t execute_action_internal(const ts_auto_action_t *action, 
                                          ts_action_result_t *result);

/*===========================================================================*/
/*                          Initialization                                    */
/*===========================================================================*/

esp_err_t ts_action_manager_init(void)
{
    if (s_ctx != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing action manager");
    
    /* Allocate context (PSRAM preferred) */
    s_ctx = TS_CALLOC_PSRAM(1, sizeof(action_manager_ctx_t));
    if (s_ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return ESP_ERR_NO_MEM;
    }
    
    /* Create mutexes */
    s_ctx->stats_mutex = xSemaphoreCreateMutex();
    if (!s_ctx->stats_mutex) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        goto cleanup;
    }
    
    /* Create action queue */
    s_ctx->action_queue = xQueueCreate(TS_ACTION_QUEUE_SIZE, 
                                        sizeof(ts_action_queue_entry_t));
    if (!s_ctx->action_queue) {
        ESP_LOGE(TAG, "Failed to create action queue");
        goto cleanup;
    }
    
    /* Start executor task
     * CRITICAL: Must use DRAM stack (not PSRAM) because this task performs
     * NVS/Flash operations. SPI Flash operations disable cache, and accessing
     * PSRAM (external SPI RAM) with cache disabled causes a crash.
     * Use xTaskCreateWithCaps to explicitly allocate stack in DRAM.
     */
    s_ctx->running = true;
    s_ctx->accepting = true;
    BaseType_t ret = xTaskCreateWithCaps(action_executor_task,
                                          "action_exec",
                                          ACTION_TASK_STACK_SIZE,
                                          NULL,
                                          ACTION_TASK_PRIORITY,
                                          &s_ctx->executor_task,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create executor task");
        goto cleanup;
    }
    
    s_ctx->initialized = true;
    ESP_LOGI(TAG, "Action manager initialized");
    return ESP_OK;
    
cleanup:
    if (s_ctx->action_queue) vQueueDelete(s_ctx->action_queue);
    if (s_ctx->stats_mutex) vSemaphoreDelete(s_ctx->stats_mutex);
    free(s_ctx);
    s_ctx = NULL;
    return ESP_ERR_NO_MEM;
}

esp_err_t ts_action_manager_deinit(void)
{
    if (s_ctx == NULL) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Deinitializing action manager");
    
    /* Admission stays closed until every producer/executor has released ownership. */
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    s_ctx->direct_closed = true;
    bool direct_idle = s_ctx->direct_pending == 0;
    xSemaphoreGive(s_ctx->stats_mutex);
    if (ts_action_manager_quiesce() != ESP_OK || !direct_idle) return ESP_ERR_TIMEOUT;
    s_ctx->running = false;
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    bool exiting = s_ctx->executor_task != NULL;
    xSemaphoreGive(s_ctx->stats_mutex);
    if (exiting) {
        /* Queue receive has a bounded timeout; no fake entry owns a pending credit. */
        /* Cooperative stop requested; cleanup requires a later call after exit. */
        return ESP_ERR_INVALID_STATE;
    }

    ts_action_cancel_all();
    /* Cleanup resources */
    if (s_ctx->action_queue) vQueueDelete(s_ctx->action_queue);
    if (s_ctx->stats_mutex) vSemaphoreDelete(s_ctx->stats_mutex);
    
    free(s_ctx);
    s_ctx = NULL;
    
    ESP_LOGI(TAG, "Action manager deinitialized");
    return ESP_OK;
}

bool ts_action_manager_is_initialized(void)
{
    return (s_ctx != NULL && s_ctx->initialized);
}

/*===========================================================================*/
/*                          SSH Host Management                               */
/*===========================================================================*/

esp_err_t ts_action_get_ssh_host(const char *host_id,ts_action_ssh_host_t *out)
{
    if(!host_id||!out)return ESP_ERR_INVALID_ARG;
    ts_ssh_host_config_t config;esp_err_t ret=ts_ssh_hosts_config_get(host_id,&config);
    if(ret!=ESP_OK)return ret;
    memset(out,0,sizeof(*out));
    strcpy(out->id,config.id);strcpy(out->host,config.host);out->port=config.port;
    strcpy(out->username,config.username);out->use_key_auth=config.auth_type==TS_SSH_HOST_AUTH_KEY;
    if(out->use_key_auth)strcpy(out->key_path,config.keyid);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Action Execution                                  */
/*===========================================================================*/

/**
 * Execute action synchronously via the executor task (DRAM stack).
 * 
 * This function queues the action and waits for completion. This ensures
 * the action executes in the executor task context, which has a DRAM stack
 * and can safely perform NVS/Flash operations.
 */
static bool action_admit(void) {
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    bool accept = s_ctx->running && s_ctx->accepting;
    if (accept) s_ctx->pending += 2; /* caller + queue/executor ownership */
    xSemaphoreGive(s_ctx->stats_mutex);
    return accept;
}
static void action_finished(void) {
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    --s_ctx->pending;
    xSemaphoreGive(s_ctx->stats_mutex);
}
static bool direct_service_admit(const char **reason) {
    if (!s_ctx) {*reason="service_controls_unavailable";return false;}
    xSemaphoreTake(s_ctx->stats_mutex,portMAX_DELAY);
    bool accept=s_ctx->running&&!s_ctx->direct_closed&&!s_ctx->direct_pending;
    if(accept)s_ctx->direct_pending=2;
    else *reason=s_ctx->direct_pending?"service_control_busy":"service_controls_unavailable";
    xSemaphoreGive(s_ctx->stats_mutex);return accept;
}
static void direct_service_finished(void) {
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    --s_ctx->direct_pending;
    xSemaphoreGive(s_ctx->stats_mutex);
}
static void entry_finished(const ts_action_queue_entry_t *entry, esp_err_t error) {
    if (entry->service_operation_id) {
        ts_ssh_service_complete_operation(entry->service_run_id, entry->service_operation_id, error);
        direct_service_finished();
    } else action_finished();
}
bool ts_action_manager_accepting(void) { return s_ctx && s_ctx->accepting; }
esp_err_t ts_action_manager_quiesce(void) {
    if (!s_ctx) return ESP_OK;
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    s_ctx->accepting = false;
    bool idle = s_ctx->pending == 0;
    xSemaphoreGive(s_ctx->stats_mutex);
    return idle ? ESP_OK : ESP_ERR_TIMEOUT;
}
esp_err_t ts_action_manager_resume(void) {
    if (!s_ctx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    bool idle = s_ctx->running && !s_ctx->pending;
    if (idle) s_ctx->accepting = true;
    xSemaphoreGive(s_ctx->stats_mutex);
    return idle ? ESP_OK : ESP_ERR_INVALID_STATE;
}
static esp_err_t prepare_action_entry(const ts_auto_action_t *action,
                                      ts_action_queue_entry_t *entry, bool delay_handled) {
    esp_err_t ret = ESP_OK;
    if (action->runtime_snapshot) {
        entry->action = *action;
        ts_action_snapshot_retain(&entry->action);
    } else {
        ts_ssh_binding_lock();
        ret = ts_action_snapshot(action, &entry->action);
        ts_ssh_binding_unlock();
    }
    if (ret == ESP_OK && delay_handled)
        entry->action.delay_ms = 0;
    return ret;
}

static esp_err_t action_manager_execute(const ts_auto_action_t *action,
                                        ts_action_result_t *result, bool delay_handled) {
    if (!s_ctx || !action || !s_ctx->running || !s_ctx->executor_task)
        return ESP_ERR_INVALID_STATE;
    if (!action_admit()) return ESP_ERR_INVALID_STATE;
    action_completion_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) { action_finished(); action_finished(); return ESP_ERR_NO_MEM; }
    c->semaphore = xSemaphoreCreateBinary();
    if (!c->semaphore) {
        free(c);
        action_finished(); action_finished();
        return ESP_ERR_NO_MEM;
    }
    atomic_init(&c->refs, 2);
    ts_action_queue_entry_t entry = {0};
    esp_err_t ret = prepare_action_entry(action, &entry, delay_handled);
    if (ret != ESP_OK) {
        completion_release(c);
        completion_release(c);
        action_finished(); action_finished();
        return ret;
    }
    entry.completion = c;
    entry.done_sem = c->semaphore;
    entry.result_ptr = &c->result;
    if (xQueueSend(s_ctx->action_queue, &entry, pdMS_TO_TICKS(100)) != pdTRUE) {
        ts_action_snapshot_release(&entry.action);
        completion_release(c);
        completion_release(c);
        action_finished(); action_finished();
        return ESP_ERR_NO_MEM;
    }
    uint32_t timeout =
        action->type == TS_AUTO_ACT_SSH_CMD || action->type == TS_AUTO_ACT_SSH_CMD_REF ? 60000
                                                                                       : 30000;
    if (xSemaphoreTake(c->semaphore, pdMS_TO_TICKS(timeout)) != pdTRUE) {
        if (result) {
            memset(result, 0, sizeof(*result));
            result->status = TS_ACTION_STATUS_TIMEOUT;
        }
        ret = ESP_ERR_TIMEOUT;
    } else {
        if (result)
            *result = c->result;
        ret = c->result.status == TS_ACTION_STATUS_SUCCESS ? ESP_OK : ESP_FAIL;
    }
    completion_release(c);
    action_finished(); /* caller releases only its own credit */
    return ret;
}

esp_err_t ts_action_manager_execute(const ts_auto_action_t *action, ts_action_result_t *result) {
    return action_manager_execute(action, result, false);
}

static esp_err_t action_queue(const ts_auto_action_t *action,
                              ts_action_callback_t callback,
                              void *user_data,
                              uint8_t priority, bool delay_handled)
{
    if (!s_ctx || !s_ctx->running || !action) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!action_admit()) return ESP_ERR_INVALID_STATE;
    ts_action_queue_entry_t entry = {0};
    esp_err_t ret = prepare_action_entry(action, &entry, delay_handled);
    if (ret != ESP_OK) { action_finished(); action_finished(); return ret; }
    entry.callback = callback;
    entry.user_data = user_data;
    entry.priority = priority;
    entry.enqueue_time = esp_timer_get_time() / 1000;

    if (xQueueSend(s_ctx->action_queue, &entry, pdMS_TO_TICKS(100)) != pdTRUE) {
        ts_action_snapshot_release(&entry.action);
        ESP_LOGW(TAG, "Action queue full");
        action_finished(); action_finished();
        return ESP_ERR_NO_MEM;
    }
    
    /* Update high water mark */
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    UBaseType_t waiting = uxQueueMessagesWaiting(s_ctx->action_queue);
    if (waiting > s_ctx->stats.queue_high_water) {
        s_ctx->stats.queue_high_water = waiting;
    }
    xSemaphoreGive(s_ctx->stats_mutex);
    action_finished();
    return ESP_OK;
}

esp_err_t ts_action_queue(const ts_auto_action_t *action, ts_action_callback_t callback,
                          void *user_data, uint8_t priority) {
    return action_queue(action, callback, user_data, priority, false);
}

esp_err_t ts_action_submit_prepared(const ts_auto_action_t *action) {
    if (!action || !action->runtime_snapshot)
        return ESP_ERR_INVALID_ARG;
    return action->async ? action_queue(action, NULL, NULL, 5, true)
                         : action_manager_execute(action, NULL, true);
}

esp_err_t ts_action_execute_sequence(const ts_auto_action_t *actions,
                                      int count,
                                      bool stop_on_error)
{
    if (!actions || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ESP_OK;
    ts_action_result_t result;
    
    for (int i = 0; i < count; i++) {
        ret = ts_action_manager_execute(&actions[i], &result);
        if (ret != ESP_OK || result.status != TS_ACTION_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "Action %d failed (code=%d)", i, ret);
            if (stop_on_error) {
                return ret != ESP_OK ? ret : ESP_FAIL;
            }
        }
    }
    
    return ESP_OK;
}

esp_err_t ts_action_cancel_all(void)
{
    if (!s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Clear queue */
    ts_action_queue_entry_t entry;
    while (xQueueReceive(s_ctx->action_queue, &entry, 0) == pdTRUE) {
        ts_action_snapshot_release(&entry.action);
        if (entry.result_ptr)
            entry.result_ptr->status = TS_ACTION_STATUS_CANCELLED;
        if (entry.done_sem)
            xSemaphoreGive(entry.done_sem);
        completion_release(entry.completion);
        entry_finished(&entry, ESP_ERR_INVALID_STATE);
    }
    ESP_LOGI(TAG, "Cancelled all pending actions");
    return ESP_OK;
}

/*===========================================================================*/
/*                       Individual Action Executors                          */
/*===========================================================================*/

static esp_err_t exec_ssh_bound(const ts_auto_action_ssh_t *ssh,const ts_action_ssh_host_t *snapshot,
                                  ts_action_result_t *result)
{
    if (!ssh || !result || !snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int64_t start_time = esp_timer_get_time();
    result->status = TS_ACTION_STATUS_RUNNING;
    
    const ts_action_ssh_host_t host=*snapshot;

    /* Expand variables in command
     * Use heap allocation to avoid stack overflow with large commands
     */
    char *expanded_cmd = heap_caps_malloc(TS_SSH_CMD_COMMAND_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!expanded_cmd) {
        expanded_cmd = malloc(TS_SSH_CMD_COMMAND_MAX);
    }
    if (!expanded_cmd) {
        result->status = TS_ACTION_STATUS_FAILED;
        snprintf(result->output, sizeof(result->output), "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    if (ts_action_expand_variables(ssh->command, expanded_cmd, TS_SSH_CMD_COMMAND_MAX) < 0) {
        free(expanded_cmd);
        result->status = TS_ACTION_STATUS_FAILED;
        snprintf(result->output, sizeof(result->output), "Expanded command exceeds capacity");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGD(TAG, "SSH host=%s, command bytes=%u", ssh->host_ref, (unsigned)strlen(expanded_cmd));

    /* Create SSH session */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    config.host = host.host;
    config.port = host.port;
    config.username = host.username;
    config.timeout_ms = ssh->timeout_ms > 0 ? ssh->timeout_ms : TS_ACTION_SSH_TIMEOUT_MS;
    
    /* Load SSH key from keystore if using key auth */
    char *key_data = NULL;
    size_t key_len = 0;
    esp_err_t ret;
    
    if (host.use_key_auth && host.key_path[0]) {
        /* host.key_path actually contains the keyid */
        const char *keyid = host.key_path;
        
        /* Try to load from keystore first */
        ret = ts_keystore_load_private_key(keyid, &key_data, &key_len);
        if (ret == ESP_OK && key_data && key_len > 0) {
            config.auth_method = TS_SSH_AUTH_PUBLICKEY;
            config.auth.key.private_key = (const uint8_t *)key_data;
            config.auth.key.private_key_len = key_len;
            config.auth.key.private_key_path = NULL;
            ESP_LOGI(TAG, "Loaded SSH key '%s' from keystore (%zu bytes)", keyid, key_len);
        } else {
            free(key_data);
            free(expanded_cmd);
            result->reason="ssh_key_unavailable";
            result->status=TS_ACTION_STATUS_FAILED;
            snprintf(result->output,sizeof(result->output),"SSH key unavailable: %s",keyid);
            return ret==ESP_OK?ESP_ERR_INVALID_STATE:ret;
        }
    } else {
        config.auth_method = TS_SSH_AUTH_PASSWORD;
        config.auth.password = host.password;
    }
    
    ts_ssh_session_t session = NULL;
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        snprintf(result->output, sizeof(result->output), 
                 "SSH session create failed: %s", esp_err_to_name(ret));
        result->status = TS_ACTION_STATUS_FAILED;
        free(expanded_cmd);
        if (key_data) free(key_data);
        return ret;
    }
    
    /* Connect */
    ret = ts_ssh_connect(session);
    if (ret != ESP_OK) {
        snprintf(result->output, sizeof(result->output), 
                 "SSH connect failed: %s", esp_err_to_name(ret));
        result->status = TS_ACTION_STATUS_FAILED;
        ts_ssh_session_destroy(session);
        free(expanded_cmd);
        if (key_data) free(key_data);
        return ret;
    }
    
    /* Execute command */
    ts_ssh_exec_result_t exec_result = {0};
    ret = ts_ssh_exec(session, expanded_cmd, &exec_result);
    
    if (ret == ESP_OK) {
        result->exit_code = exec_result.exit_code;
        if (exec_result.stdout_data && exec_result.stdout_len > 0) {
            size_t copy_len = exec_result.stdout_len < sizeof(result->output) - 1 
                            ? exec_result.stdout_len 
                            : sizeof(result->output) - 1;
            memcpy(result->output, exec_result.stdout_data, copy_len);
            result->output[copy_len] = '\0';
        } else if (exec_result.stderr_data && exec_result.stderr_len > 0) {
            size_t copy_len = exec_result.stderr_len < sizeof(result->output) - 1 
                            ? exec_result.stderr_len 
                            : sizeof(result->output) - 1;
            memcpy(result->output, exec_result.stderr_data, copy_len);
            result->output[copy_len] = '\0';
        }
        
        result->status = (exec_result.exit_code == 0) 
                       ? TS_ACTION_STATUS_SUCCESS 
                       : TS_ACTION_STATUS_FAILED;
        
    } else {
        snprintf(result->output, sizeof(result->output), 
                 "SSH exec failed: %s", esp_err_to_name(ret));
        result->status = (ret == ESP_ERR_TIMEOUT) 
                       ? TS_ACTION_STATUS_TIMEOUT 
                       : TS_ACTION_STATUS_FAILED;
    }

    ts_ssh_exec_result_free(&exec_result);
    /* Cleanup */
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    
    /* Free key data if loaded from keystore */
    if (key_data) {
        free(key_data);
    }
    
    result->duration_ms = (esp_timer_get_time() - start_time) / 1000;
    result->timestamp = esp_timer_get_time() / 1000;
    
    /* Update statistics */
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    s_ctx->stats.ssh_commands++;
    xSemaphoreGive(s_ctx->stats_mutex);
    
    ESP_LOGD(TAG, "SSH result: exit=%d, duration=%lu ms", 
             result->exit_code, result->duration_ms);
    
    /* Free heap allocated buffer */
    free(expanded_cmd);
    
    return ret;
}

/**
 * @brief 辅助函数：解析设备名称短名到完整名称
 */
static const char *action_resolve_led_device_name(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "touch") == 0) return "led_touch";
    if (strcmp(name, "board") == 0) return "led_board";
    if (strcmp(name, "matrix") == 0) return "led_matrix";
    return name;  // 返回原名（可能已经是完整名称）
}

esp_err_t ts_action_exec_ssh(const ts_auto_action_ssh_t *ssh,ts_action_result_t *result)
{
    if(!ssh)return ESP_ERR_INVALID_ARG;
    ts_auto_action_t action={.type=TS_AUTO_ACT_SSH_CMD,.ssh=*ssh};return ts_action_manager_execute(&action,result);
}

static esp_err_t require_action_resource(const char *path,const char *reason,ts_action_result_t *result)
{
    unsigned char first;
    if(ts_storage_read_file(path,&first,1)==1)return ESP_OK;
    result->reason=reason;
    snprintf(result->output,sizeof(result->output),"%s",path);
    ESP_LOGW(TAG,"%s: %s",reason,path);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ts_action_exec_led(const ts_auto_action_led_t *led,
                              ts_action_result_t *result)
{
    if (!led || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    
    result->reason=NULL;
    /* 创建本地副本以支持变量展开 */
    ts_auto_action_led_t led_expanded;
    memcpy(&led_expanded, led, sizeof(ts_auto_action_led_t));
    
    /* Expand into each actual destination; reject rather than truncate. */
    if (ts_action_expand_variables(led->text, led_expanded.text, sizeof(led_expanded.text)) < 0) {
        result->status = TS_ACTION_STATUS_FAILED;
        return ESP_ERR_INVALID_SIZE;
    }
    if (ts_action_expand_variables(led->image_path, led_expanded.image_path, sizeof(led_expanded.image_path)) < 0) {
        result->status = TS_ACTION_STATUS_FAILED;
        return ESP_ERR_INVALID_SIZE;
    }
    if (ts_action_expand_variables(led->qr_text, led_expanded.qr_text, sizeof(led_expanded.qr_text)) < 0) {
        result->status = TS_ACTION_STATUS_FAILED;
        return ESP_ERR_INVALID_SIZE;
    }
    if (ts_action_expand_variables(led->filter, led_expanded.filter, sizeof(led_expanded.filter)) < 0) {
        result->status = TS_ACTION_STATUS_FAILED;
        return ESP_ERR_INVALID_SIZE;
    }
    if (ts_action_expand_variables(led->effect, led_expanded.effect, sizeof(led_expanded.effect)) < 0) {
        result->status = TS_ACTION_STATUS_FAILED;
        return ESP_ERR_INVALID_SIZE;
    }

    /* 使用展开后的结构 */
    const ts_auto_action_led_t *led_final = &led_expanded;
    
    result->status = TS_ACTION_STATUS_RUNNING;
    int64_t start_time = esp_timer_get_time();
    esp_err_t ret = ESP_OK;
    
    /* 解析设备名称（支持短名和完整名） */
    const char *device_name = action_resolve_led_device_name(led_final->device);
    
    ESP_LOGI(TAG, "LED action: device=%s, ctrl_type=%d", device_name, led_final->ctrl_type);
    
    /* 获取设备句柄 */
    ts_led_device_t device = ts_led_device_get(device_name);
    if (!device) {
        ESP_LOGW(TAG, "LED device '%s' not found", device_name);
        ret = ESP_ERR_NOT_FOUND;
        goto done;
    }
    
    /* 获取默认 layer */
    ts_led_layer_t layer = ts_led_layer_get(device, 0);
    if (!layer) {
        ESP_LOGW(TAG, "LED layer not found for device '%s'", device_name);
        ret = ESP_ERR_NOT_FOUND;
        goto done;
    }
    
    /* 根据控制类型执行对应操作 */
    switch (led_final->ctrl_type) {
        case TS_LED_CTRL_OFF:
            /* 关闭 LED */
            ts_led_animation_stop(layer);
            ret = ts_led_fill(layer, TS_LED_RGB(0, 0, 0));
            snprintf(result->output, sizeof(result->output), "LED %s turned off", led->device);
            break;
            
        case TS_LED_CTRL_BRIGHTNESS:
            /* 仅调节亮度 */
            ret = ts_led_device_set_brightness(device, led_final->brightness);
            snprintf(result->output, sizeof(result->output), "LED %s brightness=%d", led->device, led_final->brightness);
            break;
            
        case TS_LED_CTRL_EFFECT:
            /* 启动效果动画 */
            if (led_final->effect[0]) {
                const ts_led_animation_def_t *anim = ts_led_animation_get_builtin(led_final->effect);
                if (anim) {
                    ESP_LOGI(TAG, "Starting effect '%s' on device '%s'", led_final->effect, device_name);
                    ret = ts_led_animation_start(layer, anim);
                    snprintf(result->output, sizeof(result->output), "LED %s effect=%.32s started", led->device, led_final->effect);
                } else {
                    ESP_LOGW(TAG, "Effect '%s' not found", led_final->effect);
                    ret = ESP_ERR_NOT_FOUND;
                    snprintf(result->output, sizeof(result->output), "Effect '%.32s' not found", led_final->effect);
                }
            } else {
                ret = ESP_ERR_INVALID_ARG;
                snprintf(result->output, sizeof(result->output), "No effect specified");
            }
            break;
            
        case TS_LED_CTRL_FILL:
        default:
            /* 纯色填充（默认行为，兼容旧版本） */
            /* 如果有效果名（旧版兼容），优先启动效果 */
            if (led_final->effect[0]) {
                const ts_led_animation_def_t *anim = ts_led_animation_get_builtin(led_final->effect);
                if (anim) {
                    ret = ts_led_animation_start(layer, anim);
                    snprintf(result->output, sizeof(result->output), "LED %s effect=%.32s started", led->device, led_final->effect);
                    break;
                }
            }
            
            ts_led_rgb_t color = TS_LED_RGB(led_final->r, led_final->g, led_final->b);
            if (led_final->index == 0xFF) {
                ret = ts_led_fill(layer, color);
            } else {
                ret = ts_led_device_set_pixel(device, (uint16_t)led_final->index, color);
            }
            snprintf(result->output, sizeof(result->output), "LED %s filled with color", led->device);
            break;
            
        case TS_LED_CTRL_TEXT:
            /* 显示文本（仅 Matrix）- 通过 CLI 执行 */
            if (strcmp(device_name, "led_matrix") != 0) {
                ret = ESP_ERR_NOT_SUPPORTED;
                snprintf(result->output, sizeof(result->output), "Text display only supported on matrix");
                break;
            }
            if (led_final->text[0]) {
                const char *font=led_final->font[0]?led_final->font:"pixel9x9";
                char font_path[sizeof(led_final->font)+sizeof("/sdcard/fonts/.fnt")];
                if(font[0]=='/')snprintf(font_path,sizeof(font_path),"%.*s",(int)sizeof(led_final->font)-1,font);
                else snprintf(font_path,sizeof(font_path),"/sdcard/fonts/%.*s.fnt",(int)sizeof(led_final->font)-1,font);
                ret=require_action_resource(font_path,"action_font_unavailable",result);
                if(ret!=ESP_OK)break;
                /* 构建 CLI 命令 */
                char cmd[512];
                int len = snprintf(cmd, sizeof(cmd), "led --draw-text --device matrix --text \"%s\"", led_final->text);
                if (led_final->font[0]) {
                    len += snprintf(cmd + len, sizeof(cmd) - len, " --font %s", led_final->font);
                } else {
                    len += snprintf(cmd + len, sizeof(cmd) - len, " --font pixel9x9");
                }
                if (led_final->r || led_final->g || led_final->b) {
                    len += snprintf(cmd + len, sizeof(cmd) - len, " --color #%02X%02X%02X", led_final->r, led_final->g, led_final->b);
                }
                if (led_final->scroll[0] && strcmp(led_final->scroll, "none") != 0) {
                    len += snprintf(cmd + len, sizeof(cmd) - len, " --scroll %s", led_final->scroll);
                    if (led_final->loop) {
                        len += snprintf(cmd + len, sizeof(cmd) - len, " --loop");
                    }
                }
                if (led_final->speed > 0) {
                    len += snprintf(cmd + len, sizeof(cmd) - len, " --speed %d", led_final->speed);
                }

                ESP_LOGD(TAG, "Executing LED CLI (%u bytes)", (unsigned)strlen(cmd));
                ret = ts_console_exec(cmd, NULL);
                snprintf(result->output, sizeof(result->output), "LED text: %.200s%s", led_final->text, strlen(led_final->text) > 200 ? "..." : "");
            } else {
                ret = ESP_ERR_INVALID_ARG;
                snprintf(result->output, sizeof(result->output), "No text specified");
            }
            break;
            
        case TS_LED_CTRL_IMAGE:
            /* 显示图像（仅 Matrix）- 通过 CLI 执行 */
            if (strcmp(device_name, "led_matrix") != 0) {
                ret = ESP_ERR_NOT_SUPPORTED;
                snprintf(result->output, sizeof(result->output), "Image display only supported on matrix");
                break;
            }
            if (led_final->image_path[0]) {
                ret=require_action_resource(led_final->image_path,"action_image_unavailable",result);
                if(ret!=ESP_OK)break;
                char cmd[512];
                snprintf(cmd, sizeof(cmd), "led --image --device matrix --file %.256s%s", 
                         led_final->image_path, led_final->center ? " --center content" : "");

                ESP_LOGD(TAG, "Executing LED CLI (%u bytes)", (unsigned)strlen(cmd));
                ret = ts_console_exec(cmd, NULL);
                snprintf(result->output, sizeof(result->output), "LED image: %.200s%s", led_final->image_path, strlen(led_final->image_path) > 200 ? "..." : "");
            } else {
                ret = ESP_ERR_INVALID_ARG;
                snprintf(result->output, sizeof(result->output), "No image path specified");
            }
            break;
            
        case TS_LED_CTRL_QRCODE:
            /* 显示 QR 码（仅 Matrix）- 通过 CLI 执行 */
            if (strcmp(device_name, "led_matrix") != 0) {
                ret = ESP_ERR_NOT_SUPPORTED;
                snprintf(result->output, sizeof(result->output), "QR code only supported on matrix");
                break;
            }
            if (led_final->qr_text[0]) {
                char cmd[512];
                int len = snprintf(cmd, sizeof(cmd), "led --qrcode --device matrix --text \"%s\"", led_final->qr_text);
                if (led_final->qr_ecc) {
                    len += snprintf(cmd + len, sizeof(cmd) - len, " --ecc %c", led_final->qr_ecc);
                }
                if (led_final->r || led_final->g || led_final->b) {
                    len += snprintf(cmd + len, sizeof(cmd) - len, " --color #%02X%02X%02X", led_final->r, led_final->g, led_final->b);
                }

                ESP_LOGD(TAG, "Executing LED CLI (%u bytes)", (unsigned)strlen(cmd));
                ret = ts_console_exec(cmd, NULL);
                snprintf(result->output, sizeof(result->output), "LED QR: %.200s%s", 
                         led_final->qr_text, strlen(led_final->qr_text) > 200 ? "..." : "");
            } else {
                ret = ESP_ERR_INVALID_ARG;
                snprintf(result->output, sizeof(result->output), "No QR text specified");
            }
            break;
            
        case TS_LED_CTRL_FILTER:
            /* 应用后处理滤镜（仅 Matrix）- 通过 CLI 执行 */
            if (strcmp(device_name, "led_matrix") != 0) {
                ret = ESP_ERR_NOT_SUPPORTED;
                snprintf(result->output, sizeof(result->output), "Filter only supported on matrix");
                break;
            }
            if (led_final->filter[0]) {
                if (led_final->filter_params.present) {
                    cJSON *params = cJSON_CreateObject();
                    if (!params || !cJSON_AddStringToObject(params, "device", device_name) ||
                        !cJSON_AddStringToObject(params, "filter", led_final->filter) ||
                        !ts_action_filter_api_params(&led_final->filter_params, params)) {
                        cJSON_Delete(params);
                        ret = ESP_ERR_NO_MEM;
                        break;
                    }
                    ts_api_result_t response = {0};
                    ret = ts_api_call("led.filter.start", params, &response);
                    if (ret == ESP_OK && response.code != TS_API_OK) ret = ESP_FAIL;
                    ts_api_result_free(&response);
                    cJSON_Delete(params);
                    snprintf(result->output, sizeof(result->output), "LED filter: %s", led_final->filter);
                    break;
                }
                char cmd[128];
                if (strcmp(led_final->filter, "none") == 0 || strcmp(led_final->filter, "stop") == 0) {
                    snprintf(cmd, sizeof(cmd), "led --stop-filter --device matrix");
                } else {
                    snprintf(cmd, sizeof(cmd), "led --filter --device matrix --filter-name %.*s",
                             (int)sizeof(led_final->filter) - 1, led_final->filter);
                }

                ESP_LOGD(TAG, "Executing LED CLI (%u bytes)", (unsigned)strlen(cmd));
                ret = ts_console_exec(cmd, NULL);
                snprintf(result->output, sizeof(result->output), "LED filter: %s", led_final->filter);
            } else {
                ret = ESP_ERR_INVALID_ARG;
                snprintf(result->output, sizeof(result->output), "No filter specified");
            }
            break;
            
        case TS_LED_CTRL_FILTER_STOP:
            /* 停止滤镜（仅 Matrix）*/
            if (strcmp(device_name, "led_matrix") != 0) {
                ret = ESP_ERR_NOT_SUPPORTED;
                snprintf(result->output, sizeof(result->output), "Filter stop only supported on matrix");
                break;
            }
            {
                const char *cmd = "led --stop-filter --device matrix";
                ESP_LOGD(TAG, "Executing LED CLI (%u bytes)", (unsigned)strlen(cmd));
                ret = ts_console_exec(cmd, NULL);
                snprintf(result->output, sizeof(result->output), "LED filter stopped");
            }
            break;
            
        case TS_LED_CTRL_TEXT_STOP:
            /* 停止文本覆盖层（仅 Matrix）*/
            if (strcmp(device_name, "led_matrix") != 0) {
                ret = ESP_ERR_NOT_SUPPORTED;
                snprintf(result->output, sizeof(result->output), "Text stop only supported on matrix");
                break;
            }
            {
                const char *cmd = "led --stop-text --device matrix";
                ESP_LOGD(TAG, "Executing LED CLI (%u bytes)", (unsigned)strlen(cmd));
                ret = ts_console_exec(cmd, NULL);
                snprintf(result->output, sizeof(result->output), "LED text stopped");
            }
            break;
    }

done:
    result->duration_ms = (esp_timer_get_time() - start_time) / 1000;
    result->timestamp = esp_timer_get_time() / 1000;
    
    if (ret == ESP_OK) {
        result->status = TS_ACTION_STATUS_SUCCESS;
    } else {
        result->status = TS_ACTION_STATUS_FAILED;
        if (result->output[0] == '\0') {
            snprintf(result->output, sizeof(result->output), 
                     "LED failed: %s", esp_err_to_name(ret));
        }
    }
    
    /* Update statistics */
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    s_ctx->stats.led_actions++;
    xSemaphoreGive(s_ctx->stats_mutex);
    
    return ret;
}

esp_err_t ts_action_exec_gpio(const ts_auto_action_gpio_t *gpio_action,
                               ts_action_result_t *result)
{
    if (!gpio_action || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    
    result->status = TS_ACTION_STATUS_RUNNING;
    int64_t start_time = esp_timer_get_time();
    
    uint8_t pin = gpio_action->pin;
    bool level = gpio_action->level;
    uint32_t pulse_ms = gpio_action->pulse_ms;
    
    ESP_LOGD(TAG, "GPIO action: pin=%d, level=%d, pulse_ms=%lu", 
             pin, level, pulse_ms);
    
    /* Configure GPIO as output if not already */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        snprintf(result->output, sizeof(result->output), 
                 "GPIO config failed: %s", esp_err_to_name(ret));
        result->status = TS_ACTION_STATUS_FAILED;
        return ret;
    }
    
    /* Set level */
    ret = gpio_set_level(pin, level ? 1 : 0);
    if (ret != ESP_OK) {
        snprintf(result->output, sizeof(result->output), 
                 "GPIO set failed: %s", esp_err_to_name(ret));
        result->status = TS_ACTION_STATUS_FAILED;
        return ret;
    }
    
    /* If pulse mode, wait and toggle back */
    if (pulse_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(pulse_ms));
        ret = gpio_set_level(pin, level ? 0 : 1);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GPIO pulse restore failed: %s", esp_err_to_name(ret));
        }
        snprintf(result->output, sizeof(result->output), 
                 "GPIO %d pulse %lu ms", pin, pulse_ms);
    } else {
        snprintf(result->output, sizeof(result->output), 
                 "GPIO %d set to %d", pin, level);
    }
    
    result->duration_ms = (esp_timer_get_time() - start_time) / 1000;
    result->timestamp = esp_timer_get_time() / 1000;
    result->status = ret==ESP_OK?TS_ACTION_STATUS_SUCCESS:TS_ACTION_STATUS_FAILED;
    
    /* Update statistics */
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    s_ctx->stats.gpio_actions++;
    xSemaphoreGive(s_ctx->stats_mutex);
    
    return ret;
}

esp_err_t ts_action_exec_log(const ts_auto_action_log_t *log_action,
                              ts_action_result_t *result)
{
    if (!log_action || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    
    result->status = TS_ACTION_STATUS_RUNNING;
    
    /* Expand variables in message */
    char expanded_msg[256];
    if (ts_action_expand_variables(log_action->message, expanded_msg, sizeof(expanded_msg)) < 0) {
        result->status = TS_ACTION_STATUS_FAILED;
        return ESP_ERR_INVALID_SIZE;
    }
    
    /* Log at appropriate level */
    switch (log_action->level) {
        case ESP_LOG_ERROR:
            ESP_LOGE("AUTOMATION", "%s", expanded_msg);
            break;
        case ESP_LOG_WARN:
            ESP_LOGW("AUTOMATION", "%s", expanded_msg);
            break;
        case ESP_LOG_INFO:
            ESP_LOGI("AUTOMATION", "%s", expanded_msg);
            break;
        case ESP_LOG_DEBUG:
            ESP_LOGD("AUTOMATION", "%s", expanded_msg);
            break;
        default:
            ESP_LOGI("AUTOMATION", "%s", expanded_msg);
            break;
    }
    
    result->status = TS_ACTION_STATUS_SUCCESS;
    strncpy(result->output, expanded_msg, sizeof(result->output) - 1);
    result->timestamp = esp_timer_get_time() / 1000;
    
    return ESP_OK;
}

esp_err_t ts_action_exec_set_var(const ts_auto_action_set_var_t *set_var,
                                  ts_action_result_t *result)
{
    if (!set_var || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    
    result->status = TS_ACTION_STATUS_RUNNING;
    
    esp_err_t ret = ts_variable_set(set_var->variable, &set_var->value);
    
    if (ret == ESP_OK) {
        result->status = TS_ACTION_STATUS_SUCCESS;
        snprintf(result->output, sizeof(result->output), 
                 "Variable '%s' set", set_var->variable);
    } else {
        result->status = TS_ACTION_STATUS_FAILED;
        snprintf(result->output, sizeof(result->output), 
                 "Set variable failed: %s", esp_err_to_name(ret));
    }
    
    result->timestamp = esp_timer_get_time() / 1000;
    return ret;
}

static esp_err_t device_operation(const ts_auto_action_device_t *device)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Device action: device=%s, action=%s",
             device->device, device->action);

    // 解析设备 ID
    ts_device_id_t dev_id = TS_DEVICE_MAX;
    if (strcasecmp(device->device, "agx") == 0 ||
        strcasecmp(device->device, "AGX") == 0) {
        dev_id = TS_DEVICE_AGX;
    } else if (strcasecmp(device->device, "lpmu") == 0 ||
               strcasecmp(device->device, "LPMU") == 0) {
        dev_id = TS_DEVICE_LPMU;
    } else {
        ESP_LOGW(TAG, "Unknown device: %s", device->device);
        return ESP_ERR_NOT_FOUND;
    }

    // 执行动作
    const char *act = device->action;

    if (strcasecmp(act, "power_on") == 0 || strcasecmp(act, "on") == 0) {
        return ts_device_power_on(dev_id);
    } else if (strcasecmp(act, "power_off") == 0 || strcasecmp(act, "off") == 0) {
        return ts_device_power_off(dev_id);
    } else if (strcasecmp(act, "force_off") == 0) {
        return ts_device_force_off(dev_id);
    } else if (strcasecmp(act, "reset") == 0 || strcasecmp(act, "reboot") == 0) {
        return ts_device_reset(dev_id);
    } else if (strcasecmp(act, "recovery") == 0) {
        return ts_device_enter_recovery(dev_id);
    } else {
        ESP_LOGW(TAG, "Unknown device action: %s", act);
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t ts_action_exec_device(const ts_auto_action_device_t *device,ts_action_result_t *result)
{
    if(!device||!result)return ESP_ERR_INVALID_ARG;
    int64_t start=esp_timer_get_time();esp_err_t ret=device_operation(device);
    result->status=ret==ESP_OK?TS_ACTION_STATUS_SUCCESS:TS_ACTION_STATUS_FAILED;
    result->timestamp=esp_timer_get_time()/1000;result->duration_ms=(esp_timer_get_time()-start)/1000;
    snprintf(result->output,sizeof(result->output),"%s.%s: %s",device->device,device->action,esp_err_to_name(ret));
    return ret;
}

static esp_err_t execute_webhook(const ts_auto_action_t *action)
{
    if (!action || action->type != TS_AUTO_ACT_WEBHOOK) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Webhook action: url=%s, method=%s",
             action->webhook.url, action->webhook.method);

    esp_http_client_config_t config = {
        .url = action->webhook.url,
        .timeout_ms = 5000,  // 默认 5 秒超时
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return ESP_ERR_NO_MEM;
    }

    // 设置方法
    if (strcasecmp(action->webhook.method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (strcasecmp(action->webhook.method, "PUT") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    // 设置 Content-Type (对于 POST/PUT)
    if (strcasecmp(action->webhook.method, "POST") == 0 ||
        strcasecmp(action->webhook.method, "PUT") == 0) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }

    // 设置 body（使用 body_template）
    if (action->webhook.body_template[0] != '\0') {
        esp_http_client_set_post_field(client, action->webhook.body_template,
                                       strlen(action->webhook.body_template));
    }

    esp_err_t ret = esp_http_client_perform(client);
    if (ret == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Webhook response: %d", status);

        if (status < 200 || status >= 300) {
            ret = ESP_FAIL;  // 非 2xx 视为失败
        }
    } else {
        ESP_LOGE(TAG, "Webhook request failed: %s", esp_err_to_name(ret));
    }

    esp_http_client_cleanup(client);
    return ret;
}

static esp_err_t exec_ssh_ref_bound(const ts_auto_action_ssh_ref_t *, const action_binding_t *,
                                    const char *,uint32_t, ts_action_result_t *,const ts_auto_action_t *);
static esp_err_t prepare_run(const ts_auto_action_t *action,const action_binding_t *binding,
    char run_id[TS_SSH_RUN_ID_MAX],uint32_t *operation,const char **reason)
{
    ts_ssh_host_config_t host={.port=binding->host.port,
        .auth_type=binding->host.use_key_auth?TS_SSH_HOST_AUTH_KEY:TS_SSH_HOST_AUTH_PASSWORD};
    strcpy(host.id,binding->host.id);strcpy(host.host,binding->host.host);strcpy(host.username,binding->host.username);
    snprintf(host.keyid,sizeof(host.keyid),"%.31s",binding->host.key_path);
    ts_ssh_run_origin_t origin={.epoch=action->runtime_epoch,.rule_instance=action->runtime_rule_instance,
        .template_instance=action->runtime_template_instance,
        .command_instance=binding->command.instance,.host_instance=binding->host_instance};
    strcpy(origin.rule,binding->owner_rule);strcpy(origin.config_version,binding->config_version);
    if(binding->command.var_name[0]){
        char key[64];snprintf(key,sizeof(key),"%s.status",binding->command.var_name);origin.variables[0]=ts_variable_instance(key);
        snprintf(key,sizeof(key),"%s.ready_time",binding->command.var_name);origin.variables[1]=ts_variable_instance(key);
    }
    return ts_ssh_service_prepare(&binding->command,&host,&origin,run_id,operation,reason);
}
/* Starts address a saved command; verify/stop address an independent run. */
esp_err_t ts_action_service_control(const char *id,ts_service_operation_t kind,
    char run_id[TS_SSH_RUN_ID_MAX],uint32_t *operation_id,const char **reason) {
    if(!reason)return ESP_ERR_INVALID_ARG;
    *reason="invalid_request";
    if(!id||!id[0]||strlen(id)>=TS_SSH_CMD_ID_MAX||!operation_id||!run_id||
        kind<TS_SERVICE_START||kind>TS_SERVICE_STOP)return ESP_ERR_INVALID_ARG;
    *operation_id=0;run_id[0]=0;*reason=NULL;
    if(!direct_service_admit(reason))return ESP_ERR_INVALID_STATE;
    ts_action_queue_entry_t *entry=heap_caps_calloc(1,sizeof(*entry),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!entry){*reason="out_of_memory";direct_service_finished();direct_service_finished();return ESP_ERR_NO_MEM;}
    entry->service_kind=kind;entry->enqueue_time=esp_timer_get_time()/1000;
    entry->action.type=TS_AUTO_ACT_SSH_CMD_REF;
    ts_ssh_binding_lock();esp_err_t ret;
    if(kind==TS_SERVICE_START){
        strcpy(entry->action.ssh_ref.cmd_id,id);ret=snapshot_command(&entry->action,true,reason);
        action_binding_t *binding=entry->action.runtime_binding;
        if(ret==ESP_OK&&!binding->command.nohup){ret=ESP_ERR_INVALID_ARG;*reason="background_command_required";}
        if(ret==ESP_OK){
            ret=prepare_run(&entry->action,binding,entry->service_run_id,&entry->service_operation_id,reason);
            if(ret==ESP_OK){strcpy(binding->run_id,entry->service_run_id);binding->control_id=entry->service_operation_id;}
        }
    }else{
        strcpy(entry->service_run_id,id);
        ret=ts_ssh_service_reserve_operation(id,kind,&entry->service_operation_id,reason);
    }
    ts_ssh_binding_unlock();
    if(ret==ESP_OK&&xQueueSend(s_ctx->action_queue,entry,0)!=pdTRUE){ret=ESP_ERR_NO_MEM;*reason="service_queue_full";}
    if(ret==ESP_OK){strcpy(run_id,entry->service_run_id);*operation_id=entry->service_operation_id;}
    else{
        if(entry->service_operation_id)ts_ssh_service_complete_operation(entry->service_run_id,entry->service_operation_id,ret);
        ts_action_snapshot_release(&entry->action);direct_service_finished();
    }
    free(entry);direct_service_finished();return ret;
}
esp_err_t ts_action_exec_ssh_ref(const ts_auto_action_ssh_ref_t *ref, ts_action_result_t *result) {
    if(!ref)return ESP_ERR_INVALID_ARG;
    ts_auto_action_t action={.type=TS_AUTO_ACT_SSH_CMD_REF,.ssh_ref=*ref};return ts_action_manager_execute(&action,result);
}
static esp_err_t exec_ssh_ref_bound(const ts_auto_action_ssh_ref_t *ssh_ref,
                                    const action_binding_t *binding,const char *run_id, uint32_t launch_id, ts_action_result_t *result,const ts_auto_action_t *origin) {
    if (!ssh_ref || !result || !binding) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int64_t start_time = esp_timer_get_time();
    result->status = TS_ACTION_STATUS_RUNNING;
    
    const ts_ssh_command_config_t cmd_config=binding->command;
    esp_err_t ret=ESP_OK;
    if (!cmd_config.enabled) {
        snprintf(result->output, sizeof(result->output), 
                 "SSH command '%s' is disabled", ssh_ref->cmd_id);
        result->status = TS_ACTION_STATUS_FAILED;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "SSH ref [%s]: host=%s, command_bytes=%u", ssh_ref->cmd_id, cmd_config.host_id,
             (unsigned)strlen(cmd_config.command));
    ESP_LOGI(TAG, "SSH ref config: var_name='%s', nohup=%d, service_mode=%d, ready_pattern='%s'",
             cmd_config.var_name, cmd_config.nohup, cmd_config.service_mode, cmd_config.ready_pattern);
    
    const ts_action_ssh_host_t host=binding->host;

    /* Expand variables in command
     * Use heap allocation to avoid stack overflow with large commands (up to 1024 bytes)
     */
    char *expanded_cmd = heap_caps_malloc(TS_SSH_CMD_COMMAND_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!expanded_cmd) {
        expanded_cmd = malloc(TS_SSH_CMD_COMMAND_MAX);
    }
    if (!expanded_cmd) {
        result->status = TS_ACTION_STATUS_FAILED;
        snprintf(result->output, sizeof(result->output), "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    /* Direct commands retain their literal text; only automation expands variables. */
    int expanded_length = binding && binding->control_id
        ? snprintf(expanded_cmd, TS_SSH_CMD_COMMAND_MAX, "%s", cmd_config.command)
        : ts_action_expand_variables(cmd_config.command, expanded_cmd, TS_SSH_CMD_COMMAND_MAX);
    if (expanded_length < 0 || expanded_length >= TS_SSH_CMD_COMMAND_MAX) {
        free(expanded_cmd);
        result->status = TS_ACTION_STATUS_FAILED;
        snprintf(result->output, sizeof(result->output), "Expanded command exceeds capacity");
        return ESP_ERR_INVALID_SIZE;
    }
    
    /* Handle nohup mode: wrap command for background execution */
    char *nohup_cmd =
        heap_caps_malloc(TS_SSH_CMD_COMMAND_MAX * 5 + 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nohup_cmd) {
        nohup_cmd = malloc(TS_SSH_CMD_COMMAND_MAX * 5 + 1024);
    }
    if (!nohup_cmd) {
        free(expanded_cmd);
        result->status = TS_ACTION_STATUS_FAILED;
        snprintf(result->output, sizeof(result->output), "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    if (cmd_config.nohup && !ts_ssh_service_launch_command(run_id,expanded_cmd,nohup_cmd,TS_SSH_CMD_COMMAND_MAX*5+1024)) {
        free(expanded_cmd);free(nohup_cmd);result->status=TS_ACTION_STATUS_FAILED;return ESP_ERR_INVALID_SIZE;
    }

    /* Create SSH session */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    config.host = host.host;
    config.port = host.port;
    config.username = host.username;
    config.timeout_ms = cmd_config.timeout_sec > 0 ? cmd_config.timeout_sec * 1000 : TS_ACTION_SSH_TIMEOUT_MS;
    
    /* Load SSH key from keystore if using key auth */
    char *key_data = NULL;
    size_t key_len = 0;
    if (host.use_key_auth && host.key_path[0]) {
        /* host.key_path actually contains the keyid */
        const char *keyid = host.key_path;
        
        /* Try to load from keystore first */
        ret = ts_keystore_load_private_key(keyid, &key_data, &key_len);
        if (ret == ESP_OK && key_data && key_len > 0) {
            config.auth_method = TS_SSH_AUTH_PUBLICKEY;
            config.auth.key.private_key = (const uint8_t *)key_data;
            config.auth.key.private_key_len = key_len;
            config.auth.key.private_key_path = NULL;
            ESP_LOGI(TAG, "Loaded SSH key '%s' from keystore (%zu bytes)", keyid, key_len);
        } else {
            if(cmd_config.nohup)ts_ssh_service_set_operation_reason(run_id,launch_id,"ssh_key_unavailable");
            free(key_data);free(expanded_cmd);free(nohup_cmd);
            result->reason="ssh_key_unavailable";result->status=TS_ACTION_STATUS_FAILED;
            snprintf(result->output,sizeof(result->output),"SSH key unavailable: %s",keyid);
            return ret==ESP_OK?ESP_ERR_INVALID_STATE:ret;
        }
    } else {
        config.auth_method = TS_SSH_AUTH_PASSWORD;
        config.auth.password = host.password;
    }
    
    ts_ssh_session_t session = NULL;
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        if(cmd_config.nohup)ts_ssh_service_set_operation_reason(run_id,launch_id,ret==ESP_ERR_NO_MEM?"out_of_memory":"ssh_connect_failed");
        snprintf(result->output, sizeof(result->output), 
                 "SSH session create failed: %s", esp_err_to_name(ret));
        result->status = TS_ACTION_STATUS_FAILED;
        free(expanded_cmd);
        free(nohup_cmd);
        if (key_data) free(key_data);
        return ret;
    }
    
    /* Connect */
    ret = ts_ssh_connect(session);
    if (ret != ESP_OK) {
        const char *stage;
        if(cmd_config.nohup)ts_ssh_service_set_operation_reason(run_id,launch_id,ts_ssh_connect_reason(session,&stage));
        snprintf(result->output, sizeof(result->output), 
                 "SSH connect failed: %s", esp_err_to_name(ret));
        result->status = TS_ACTION_STATUS_FAILED;
        ts_ssh_session_destroy(session);
        free(expanded_cmd);
        free(nohup_cmd);
        if (key_data) free(key_data);
        return ret;
    }

    uint32_t run_generation = 0;
    if (cmd_config.nohup) {
        ret = ts_ssh_service_begin_reserved(run_id, session, launch_id, &run_generation);
        if (ret != ESP_OK) {
            result->status = TS_ACTION_STATUS_FAILED;
            snprintf(result->output, sizeof(result->output),
                     "Service busy or remote state unknown; verify before retry");
            ts_ssh_session_destroy(session);
            free(expanded_cmd);
            free(nohup_cmd);
            free(key_data);
            return ret;
        }
    }
    /* Execute command */
    ts_ssh_exec_result_t exec_result = {0};
    const char *cmd_to_exec = cmd_config.nohup ? nohup_cmd : expanded_cmd;
    ESP_LOGD(TAG, "SSH execution: command=%s, bytes=%u", ssh_ref->cmd_id,
             (unsigned)strlen(cmd_to_exec));
    ret = ts_ssh_exec(session, cmd_to_exec, &exec_result);
    if (run_generation) {
        esp_err_t finish=ts_ssh_service_finish(run_id,run_generation,
            ret==ESP_OK && exec_result.exit_code==0?exec_result.stdout_data:NULL,session);
        if(ret==ESP_OK)ret=finish;
    }

    if (ret == ESP_OK) {
        result->exit_code = exec_result.exit_code;
        if (exec_result.stdout_data && exec_result.stdout_len > 0) {
            size_t copy_len = exec_result.stdout_len < sizeof(result->output) - 1 
                            ? exec_result.stdout_len 
                            : sizeof(result->output) - 1;
            memcpy(result->output, exec_result.stdout_data, copy_len);
            result->output[copy_len] = '\0';
        } else if (exec_result.stderr_data && exec_result.stderr_len > 0) {
            size_t copy_len = exec_result.stderr_len < sizeof(result->output) - 1 
                            ? exec_result.stderr_len 
                            : sizeof(result->output) - 1;
            memcpy(result->output, exec_result.stderr_data, copy_len);
            result->output[copy_len] = '\0';
        }
        
        result->status = (exec_result.exit_code == 0) 
                       ? TS_ACTION_STATUS_SUCCESS 
                       : TS_ACTION_STATUS_FAILED;
        
        /* Update variables if var_name is configured */
        if (cmd_config.var_name[0] && !run_generation && action_result_begin(origin)) {
            char var_full[64];
            
            /* Set exit_code variable */
            snprintf(var_full, sizeof(var_full), "%s.exit_code", cmd_config.var_name);
            ts_variable_set_int(var_full, exec_result.exit_code);
            
            /* Set status variable */
            snprintf(var_full, sizeof(var_full), "%s.status", cmd_config.var_name);
            ts_variable_set_string(var_full, 
                exec_result.exit_code == 0 ? "success" : "failed");
            
            /* Set timestamp variable */
            snprintf(var_full, sizeof(var_full), "%s.timestamp", cmd_config.var_name);
            ts_variable_set_int(var_full, (int32_t)(esp_timer_get_time() / 1000000));
            
            /* Debug: set execution info */
            snprintf(var_full, sizeof(var_full), "%s.exec_info", cmd_config.var_name);
            char debug_info[256];
            snprintf(debug_info, sizeof(debug_info), "nohup=%d,svcmode=%d,pattern=%.64s", 
                     cmd_config.nohup, cmd_config.service_mode, cmd_config.ready_pattern);
            ts_variable_set_string(var_full, debug_info);
            action_result_end();
        }

        /* Handle service mode: start log watching for nohup commands */
        if (exec_result.exit_code == 0 && cmd_config.nohup && cmd_config.service_mode &&
            cmd_config.ready_pattern[0]) {

            ts_ssh_log_watch_config_t watch_config = {
                .timeout_sec = cmd_config.ready_timeout_sec > 0 ? cmd_config.ready_timeout_sec : 60,
                .check_interval_ms = cmd_config.ready_check_interval_ms > 0 ? cmd_config.ready_check_interval_ms : 3000
            };
            watch_config.run_generation = run_generation;
            strcpy(watch_config.run_id, run_id);
            strncpy(watch_config.ready_pattern, cmd_config.ready_pattern, sizeof(watch_config.ready_pattern) - 1);
            strncpy(watch_config.fail_pattern, cmd_config.service_fail_pattern, sizeof(watch_config.fail_pattern) - 1);
            strncpy(watch_config.var_name, cmd_config.var_name, sizeof(watch_config.var_name) - 1);
            
            esp_err_t watch_ret = ts_ssh_log_watch_start(&watch_config, NULL);
            if (watch_ret != ESP_OK)
                ts_ssh_service_observe(run_id, run_generation, "unknown");
            if (watch_ret == ESP_OK) {
                ESP_LOGI(TAG, "🔍 Service mode: watching log for '%s' (fail='%s', timeout: %us)", 
                         cmd_config.ready_pattern, cmd_config.service_fail_pattern, watch_config.timeout_sec);
            } else {
                ESP_LOGW(TAG, "Failed to start log watch: %s", esp_err_to_name(watch_ret));
            }
        }

        /* Free result strings */
        ts_ssh_exec_result_free(&exec_result);
    } else {
        snprintf(result->output, sizeof(result->output), 
                 "SSH exec failed: %s", esp_err_to_name(ret));
        result->status = (ret == ESP_ERR_TIMEOUT) 
                       ? TS_ACTION_STATUS_TIMEOUT 
                       : TS_ACTION_STATUS_FAILED;
    }

    ts_ssh_exec_result_free(&exec_result);
    /* Cleanup */
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    
    /* Free key data if loaded from keystore */
    if (key_data) {
        free(key_data);
    }
    
    result->duration_ms = (esp_timer_get_time() - start_time) / 1000;
    result->timestamp = esp_timer_get_time() / 1000;
    
    /* Update command execution time */
    if(action_result_begin(origin)){ts_ssh_commands_config_update_exec_time(&cmd_config);action_result_end();}
    
    /* Update statistics */
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    s_ctx->stats.ssh_commands++;
    xSemaphoreGive(s_ctx->stats_mutex);
    
    ESP_LOGD(TAG, "SSH ref result: cmd=%s, exit=%d, duration=%lu ms", 
             ssh_ref->cmd_id, result->exit_code, result->duration_ms);
    
    /* Free heap allocated buffers */
    free(expanded_cmd);
    free(nohup_cmd);
    
    return ret;
}

/**
 * @brief Execute CLI command action
 */
static esp_err_t exec_cli_bound(const ts_auto_action_cli_t *cli,
                              ts_action_result_t *result,const ts_auto_action_t *origin)
{
    if (!cli || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(result, 0, sizeof(*result));
    result->status = TS_ACTION_STATUS_RUNNING;
    
    int64_t start_time = esp_timer_get_time();
    
    if (!cli->command[0]) {
        snprintf(result->output, sizeof(result->output), "Empty CLI command");
        result->status = TS_ACTION_STATUS_FAILED;
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Executing CLI command (%u bytes)", (unsigned)strlen(cli->command));

    /* Execute CLI command using ts_console_exec */
    ts_cmd_result_t cmd_result = {0};
    esp_err_t ret = ts_console_exec(cli->command, &cmd_result);
    
    result->duration_ms = (esp_timer_get_time() - start_time) / 1000;
    result->timestamp = esp_timer_get_time() / 1000;
    result->exit_code = cmd_result.code;
    
    if (ret == ESP_OK) {
        result->status = (cmd_result.code == 0) 
                       ? TS_ACTION_STATUS_SUCCESS 
                       : TS_ACTION_STATUS_FAILED;
        
        /* Copy message to output */
        if (cmd_result.message) {
            strncpy(result->output, cmd_result.message, sizeof(result->output) - 1);
            free(cmd_result.message);
        } else {
            snprintf(result->output, sizeof(result->output), 
                     "CLI command completed (code=%d)", cmd_result.code);
        }
        
        /* Store to variable if configured */
        if (cli->var_name[0] && action_result_begin(origin)) {
            char var_full[80];  /* var_name (max 64) + suffix (max 11) + null */
            
            /* Set exit_code variable */
            snprintf(var_full, sizeof(var_full), "%.63s.exit_code", cli->var_name);
            ts_variable_set_int(var_full, cmd_result.code);
            
            /* Set status variable */
            snprintf(var_full, sizeof(var_full), "%.63s.status", cli->var_name);
            ts_variable_set_string(var_full, 
                cmd_result.code == 0 ? "success" : "failed");
            
            /* Set output variable */
            snprintf(var_full, sizeof(var_full), "%.63s.output", cli->var_name);
            ts_variable_set_string(var_full, result->output);
            action_result_end();
        }
    } else {
        snprintf(result->output, sizeof(result->output), 
                 "CLI exec failed: %s", esp_err_to_name(ret));
        result->status = TS_ACTION_STATUS_FAILED;
    }
    
    /* Free any allocated data */
    if (cmd_result.data) {
        free(cmd_result.data);
    }

    ESP_LOGD(TAG, "CLI result: exit=%d, duration=%lu ms", result->exit_code, result->duration_ms);

    return ret;
}

esp_err_t ts_action_exec_cli(const ts_auto_action_cli_t *cli,ts_action_result_t *result)
{
    if(!cli)return ESP_ERR_INVALID_ARG;
    ts_auto_action_t action={.type=TS_AUTO_ACT_CLI,.cli=*cli};return ts_action_manager_execute(&action,result);
}

/*===========================================================================*/
/*                          Internal Execute                                  */
/*===========================================================================*/

/* Reserve an automation launch when its action actually starts, not while the
 * rule merely holds a configuration snapshot or waits for a later action. */
static esp_err_t execute_bound_ref(const ts_auto_action_t *action, ts_action_result_t *result) {
    const action_binding_t *binding=action->runtime_binding;
    if(!binding)return ESP_ERR_INVALID_STATE;
    uint32_t launch_id=binding->control_id;char run_id[TS_SSH_RUN_ID_MAX];strcpy(run_id,binding->run_id);
    bool own_reservation=binding->command.nohup&&!launch_id;
    if(own_reservation){
        esp_err_t ret=prepare_run(action,binding,run_id,&launch_id,NULL);
        if(ret!=ESP_OK){result->status=TS_ACTION_STATUS_FAILED;return ret;}
    }
    esp_err_t ret=launch_id?ts_ssh_service_operation_begin(run_id,launch_id):ESP_OK;
    if(ret==ESP_OK)ret=exec_ssh_ref_bound(&action->ssh_ref,binding,run_id,launch_id,result,action);
    else result->status=TS_ACTION_STATUS_FAILED;
    if(own_reservation)ts_ssh_service_complete_operation(run_id,launch_id,ret);
    return ret;
}

static esp_err_t execute_action_internal(const ts_auto_action_t *action, 
                                          ts_action_result_t *result)
{
    switch (action->type) {
        case TS_AUTO_ACT_SSH_CMD:
            return exec_ssh_bound(&action->ssh,action->runtime_binding?&((action_binding_t*)action->runtime_binding)->host:NULL,result);
            
        case TS_AUTO_ACT_SSH_CMD_REF:
            return execute_bound_ref(action, result);

        case TS_AUTO_ACT_CLI:
            return exec_cli_bound(&action->cli,result,action);
            
        case TS_AUTO_ACT_LED:
            return ts_action_exec_led(&action->led, result);
            
        case TS_AUTO_ACT_GPIO:
            return ts_action_exec_gpio(&action->gpio, result);
            
        case TS_AUTO_ACT_LOG:
            return ts_action_exec_log(&action->log, result);
            
        case TS_AUTO_ACT_SET_VAR: {
            if(!action_result_begin(action)){result->status=TS_ACTION_STATUS_CANCELLED;return ESP_ERR_INVALID_STATE;}
            esp_err_t ret=ts_action_exec_set_var(&action->set_var,result);action_result_end();return ret;
        }
            
        case TS_AUTO_ACT_DEVICE_CTRL:
            return ts_action_exec_device(&action->device, result);
            
        case TS_AUTO_ACT_WEBHOOK: {
            esp_err_t ret=execute_webhook(action);
            result->status=ret==ESP_OK?TS_ACTION_STATUS_SUCCESS:TS_ACTION_STATUS_FAILED;
            return ret;
        }
            
        default:
            result->status = TS_ACTION_STATUS_FAILED;
            snprintf(result->output, sizeof(result->output), 
                     "Unknown action type: %d", action->type);
            return ESP_ERR_INVALID_ARG;
    }
}

/*===========================================================================*/
/*                          Executor Task                                     */
/*===========================================================================*/

static esp_err_t execute_service_control(const ts_action_queue_entry_t *entry, ts_action_result_t *result) {
    if (esp_timer_get_time() / 1000 - entry->enqueue_time >= SERVICE_QUEUE_TIMEOUT_MS) {
        result->status = TS_ACTION_STATUS_TIMEOUT;
        return ESP_ERR_TIMEOUT; /* Still queued: completion reports expired, never executes. */
    }
    if (entry->service_kind == TS_SERVICE_START) return execute_action_internal(&entry->action, result);
    esp_err_t ret = ts_ssh_service_operation_begin(entry->service_run_id, entry->service_operation_id);
    if (ret == ESP_OK) {
        ts_ssh_service_status_t state;
        ret = ts_ssh_service_execute_control(entry->service_run_id, entry->service_operation_id,
                                             entry->service_kind, &state);
    }
    result->status = ret == ESP_OK ? TS_ACTION_STATUS_SUCCESS :
        ret == ESP_ERR_TIMEOUT ? TS_ACTION_STATUS_TIMEOUT : TS_ACTION_STATUS_FAILED;
    return ret;
}

static esp_err_t execute_entry(const ts_action_queue_entry_t *entry,ts_action_result_t *result)
{
    /* Handle delay */
    uint32_t delay = entry->action.delay_ms;
    while (delay && s_ctx->accepting && ts_action_snapshot_current(&entry->action)) {
        uint32_t slice = delay > 100 ? 100 : delay;
        vTaskDelay(pdMS_TO_TICKS(slice));
        delay -= slice;
    }
    /* Stop discards work not yet started; active I/O owns its cleanup. */
    esp_err_t execution_error = ESP_ERR_INVALID_STATE;
    ts_ssh_binding_lock();
    bool current=entry->service_operation_id && entry->service_kind!=TS_SERVICE_START ? true :
        ts_action_snapshot_current(&entry->action);
    bool admitted=current && (entry->service_operation_id || s_ctx->accepting);
    ts_ssh_binding_unlock();
    if (admitted && entry->service_operation_id)
        execution_error = execute_service_control(entry, result);
    else if (admitted)
        execution_error = execute_action_internal(&entry->action, result);
    else
        result->status = TS_ACTION_STATUS_CANCELLED;
    if (execution_error == ESP_OK && result->status != TS_ACTION_STATUS_SUCCESS)
        execution_error = result->status == TS_ACTION_STATUS_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    return execution_error;
}

static void action_executor_task(void *arg)
{
    ESP_LOGI(TAG, "Action executor task started (DRAM stack)");
    
    ts_action_queue_entry_t entry;
    
    while (s_ctx->running) {
        if (xQueueReceive(s_ctx->action_queue, &entry, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (!s_ctx->running) {
                ts_action_snapshot_release(&entry.action);
                if (entry.result_ptr)
                    entry.result_ptr->status = TS_ACTION_STATUS_CANCELLED;
                if (entry.done_sem)
                    xSemaphoreGive(entry.done_sem);
                completion_release(entry.completion);
                entry_finished(&entry, ESP_ERR_INVALID_STATE);
                break;
            }

            ts_action_result_t local_result = {0};
            ts_action_result_t *result = entry.result_ptr ? entry.result_ptr : &local_result;
            
            esp_err_t execution_error=execute_entry(&entry,result);

            /* Callback (async mode) */
            if (entry.callback) {
                entry.callback(&entry.action, result, entry.user_data);
            }
            
            /* Signal completion (sync mode) */

            /* Update stats */
            xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
            s_ctx->stats.total_executed++;
            if (result->status == TS_ACTION_STATUS_SUCCESS) {
                s_ctx->stats.total_success++;
            } else if (result->status == TS_ACTION_STATUS_TIMEOUT) {
                s_ctx->stats.total_timeout++;
            } else {
                s_ctx->stats.total_failed++;
            }
            xSemaphoreGive(s_ctx->stats_mutex);
            ts_action_snapshot_release(&entry.action);
            if (entry.done_sem)
                xSemaphoreGive(entry.done_sem);
            completion_release(entry.completion);
            entry_finished(&entry, execution_error);
        }
    }
    
    ESP_LOGI(TAG, "Action executor task exiting");
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    s_ctx->executor_task = NULL;
    xSemaphoreGive(s_ctx->stats_mutex);
    vTaskDelete(NULL);
}

/*===========================================================================*/
/*                          Status & Statistics                               */
/*===========================================================================*/

esp_err_t ts_action_get_queue_status(int *pending_out, int *running_out)
{
    if (!s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (pending_out) {
        *pending_out = uxQueueMessagesWaiting(s_ctx->action_queue);
    }
    if (running_out) {
        *running_out = 0; /* TODO: track running count */
    }
    
    return ESP_OK;
}

esp_err_t ts_action_get_stats(ts_action_stats_t *stats_out)
{
    if (!s_ctx || !stats_out) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    memcpy(stats_out, &s_ctx->stats, sizeof(ts_action_stats_t));
    xSemaphoreGive(s_ctx->stats_mutex);
    
    return ESP_OK;
}

void ts_action_reset_stats(void)
{
    if (!s_ctx) return;
    
    xSemaphoreTake(s_ctx->stats_mutex, portMAX_DELAY);
    memset(&s_ctx->stats, 0, sizeof(ts_action_stats_t));
    xSemaphoreGive(s_ctx->stats_mutex);
}

/*===========================================================================*/
/*                          Utility Functions                                 */
/*===========================================================================*/

int ts_action_expand_variables(const char *input, char *output, size_t output_size)
{
    if (!input || !output || !output_size) return -1;
    size_t used = 0;
    output[0] = 0;
    for (const char *p = input; *p;) {
        const char *text = p;
        size_t length = 1, consumed = 1;
        ts_auto_value_t value;
        char formatted[64];
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            if (end) {
                consumed = length = (size_t)(end + 1 - p);
                size_t name_length = (size_t)(end - p - 2);
                char name[64];
                if (name_length < sizeof(name)) {
                    memcpy(name, p + 2, name_length);
                    name[name_length] = 0;
                    if (ts_variable_get(name, &value) == ESP_OK) {
                        int written = 0;
                        switch (value.type) {
                            case TS_AUTO_VAL_STRING:
                                text = value.str_val;
                                length = strnlen(value.str_val, sizeof(value.str_val));
                                break;
                            case TS_AUTO_VAL_BOOL:
                                text = value.bool_val ? "true" : "false";
                                length = strlen(text);
                                break;
                            case TS_AUTO_VAL_INT:
                                written = snprintf(formatted, sizeof(formatted), "%ld", (long)value.int_val);
                                text = formatted;
                                length = written < 0 ? sizeof(formatted) : (size_t)written;
                                break;
                            case TS_AUTO_VAL_FLOAT:
                                written = snprintf(formatted, sizeof(formatted), "%.2f", value.float_val);
                                text = formatted;
                                length = written < 0 ? sizeof(formatted) : (size_t)written;
                                break;
                            default:
                                text = "";
                                length = 0;
                        }
                        if (text == formatted && length >= sizeof(formatted)) goto overflow;
                    }
                }
            }
        }
        if (length > output_size - 1 - used) goto overflow;
        memcpy(output + used, text, length);
        used += length;
        p += consumed;
    }
    output[used] = 0;
    return (int)used;
overflow:
    output[0] = 0; /* No caller may execute or publish a partial expansion. */
    return -1;
}

esp_err_t ts_action_parse_color(const char *color_str, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!color_str || !r || !g || !b) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Skip whitespace */
    while (isspace((unsigned char)*color_str)) color_str++;
    
    /* Hex format: #RRGGBB */
    if (color_str[0] == '#') {
        unsigned int rgb;
        if (sscanf(color_str + 1, "%6x", &rgb) == 1) {
            *r = (rgb >> 16) & 0xFF;
            *g = (rgb >> 8) & 0xFF;
            *b = rgb & 0xFF;
            return ESP_OK;
        }
    }
    
    /* RGB format: rgb(r,g,b) */
    if (strncasecmp(color_str, "rgb(", 4) == 0) {
        int rv, gv, bv;
        if (sscanf(color_str + 4, "%d,%d,%d", &rv, &gv, &bv) == 3) {
            *r = (uint8_t)rv;
            *g = (uint8_t)gv;
            *b = (uint8_t)bv;
            return ESP_OK;
        }
    }
    
    /* Named colors */
    static const struct {
        const char *name;
        uint8_t r, g, b;
    } colors[] = {
        {"red", 255, 0, 0},
        {"green", 0, 255, 0},
        {"blue", 0, 0, 255},
        {"white", 255, 255, 255},
        {"black", 0, 0, 0},
        {"yellow", 255, 255, 0},
        {"cyan", 0, 255, 255},
        {"magenta", 255, 0, 255},
        {"orange", 255, 165, 0},
        {"purple", 128, 0, 128},
        {"pink", 255, 192, 203},
        {NULL, 0, 0, 0}
    };
    
    for (int i = 0; colors[i].name; i++) {
        if (strcasecmp(color_str, colors[i].name) == 0) {
            *r = colors[i].r;
            *g = colors[i].g;
            *b = colors[i].b;
            return ESP_OK;
        }
    }
    
    return ESP_ERR_INVALID_ARG;
}

const char *ts_action_type_name(ts_auto_action_type_t type)
{
    switch (type) {
        case TS_AUTO_ACT_LED:         return "LED";
        case TS_AUTO_ACT_SSH_CMD:     return "SSH";
        case TS_AUTO_ACT_SSH_CMD_REF: return "SSH-Ref";
        case TS_AUTO_ACT_CLI:         return "CLI";
        case TS_AUTO_ACT_GPIO:        return "GPIO";
        case TS_AUTO_ACT_WEBHOOK:     return "Webhook";
        case TS_AUTO_ACT_LOG:         return "Log";
        case TS_AUTO_ACT_SET_VAR:     return "SetVar";
        case TS_AUTO_ACT_DEVICE_CTRL: return "Device";
        default:                       return "Unknown";
    }
}

const char *ts_action_status_name(ts_action_status_t status)
{
    switch (status) {
        case TS_ACTION_STATUS_PENDING:   return "Pending";
        case TS_ACTION_STATUS_RUNNING:   return "Running";
        case TS_ACTION_STATUS_SUCCESS:   return "Success";
        case TS_ACTION_STATUS_FAILED:    return "Failed";
        case TS_ACTION_STATUS_TIMEOUT:   return "Timeout";
        case TS_ACTION_STATUS_CANCELLED: return "Cancelled";
        case TS_ACTION_STATUS_QUEUED:    return "Queued";
        default:                         return "Unknown";
    }
}
