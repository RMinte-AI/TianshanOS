#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_action_manager.h"
#include "ts_rule_engine.h"
#include "ts_ssh_hosts_config.h"

#define MAX_SSH_HOSTS 8
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOG_LEVEL(...) ((void)0)
#define TAG "submission-test"
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef unsigned UBaseType_t;
#include "action_submission_types.inc"

static action_manager_ctx_t context, *s_ctx = &context;
static struct { struct { unsigned total_actions, failed_actions; } stats; } s_rule_ctx;
static pthread_mutex_t stats_mutex, host_mutex, template_mutex;
static ts_ssh_command_config_t command;
static ts_action_template_t action_template;
static int lookup_error, fail_alloc_after = -1, fail_semaphore, fail_send;
static int sem_live, pins, unpins, direct_completions, callback_calls;
static int immediate_execution, timeout_wait, stop_on_send;
static int executed, legacy_webhook, legacy_ssh, legacy_gpio;
static unsigned delay_total, last_delay, last_wait;
static int64_t now_ms;
static bool condition_matches = true, change_condition_on_delay;
static char observed_command[1024], observed_host[128];
static ts_action_status_t execution_status = TS_ACTION_STATUS_SUCCESS;

static struct { void *pointer; size_t size; } allocations[128];
static unsigned live_allocations;
static void *allocate(size_t size, bool zero) {
    if (fail_alloc_after == 0) return NULL;
    if (fail_alloc_after > 0) --fail_alloc_after;
    void *p = zero ? calloc(1, size) : malloc(size);
    assert(p);
    for (unsigned i = 0; i < 128; ++i) {
        if (!allocations[i].pointer) {
            allocations[i].pointer = p;
            allocations[i].size = size;
            ++live_allocations;
            return p;
        }
    }
    abort();
}
void *heap_caps_malloc(size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return allocate(size, false);
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return allocate(count * size, true);
}
static void audited_free(void *p) {
    if (!p) return;
    for (unsigned i = 0; i < 128; ++i) {
        if (allocations[i].pointer == p) {
            allocations[i].pointer = NULL;
            --live_allocations;
            free(p);
            return;
        }
    }
    assert(!"unowned or repeated free");
}

typedef struct { bool signaled; } test_semaphore;
static SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    if (fail_semaphore) return NULL;
    test_semaphore *sem = calloc(1, sizeof(*sem));
    assert(sem); ++sem_live;
    return (SemaphoreHandle_t)sem;
}
static void vSemaphoreDelete(SemaphoreHandle_t sem) { assert(sem); --sem_live; free(sem); }
static void drain(void);
static int test_take(SemaphoreHandle_t sem, unsigned ticks) {
    if (sem == &stats_mutex || sem == &host_mutex || sem == &template_mutex) return 1;
    last_wait = ticks;
    if (timeout_wait) return 0;
    if (!((test_semaphore *)sem)->signaled) drain();
    return ((test_semaphore *)sem)->signaled;
}
static int test_give(SemaphoreHandle_t sem) {
    if (sem == &stats_mutex || sem == &host_mutex || sem == &template_mutex) return 1;
    ((test_semaphore *)sem)->signaled = true;
    return 1;
}
static void vTaskDelay(unsigned ticks) {
    delay_total += ticks;
    now_ms += ticks;
    if (change_condition_on_delay) condition_matches = false;
}
static void vTaskDelete(TaskHandle_t task) { assert(!task); }
static int64_t esp_timer_get_time(void) { return now_ms * 1000; }

static ts_action_queue_entry_t queue[16];
static unsigned queue_count;
static int xQueueSend(void *q, const void *item, unsigned ticks) {
    assert(ticks == 100);
    const ts_action_queue_entry_t *entry = item;
    assert(!entry->service_operation_id && !entry->service_kind);
    if (entry->completion) {
        assert(!entry->callback && !entry->user_data && !entry->priority && !entry->enqueue_time);
        assert(entry->done_sem && entry->result_ptr);
    } else assert(!entry->done_sem && !entry->result_ptr);
    if (fail_send || queue_count == 16) return 0;
    queue[queue_count++] = *entry;
    last_delay = entry->action.delay_ms;
    if (stop_on_send) s_ctx->accepting = false;
    if (immediate_execution) drain();
    return 1;
}
static int xQueueReceive(void *q, void *out, unsigned ticks) {
    if (!queue_count) {
        if (ticks) s_ctx->running = false;
        return 0;
    }
    *(ts_action_queue_entry_t *)out = queue[0];
    memmove(queue, queue + 1, --queue_count * sizeof(*queue));
    return 1;
}
static unsigned uxQueueMessagesWaiting(void *q) { return queue_count; }

void ts_ssh_binding_lock(void) {}
void ts_ssh_binding_unlock(void) {}
esp_err_t ts_ssh_commands_config_get(const char *id, ts_ssh_command_config_t *out) {
    if (lookup_error) return lookup_error;
    if (strcmp(id, command.id)) return ESP_ERR_NOT_FOUND;
    *out = command; return ESP_OK;
}
esp_err_t ts_ssh_hosts_config_get(const char *id, ts_ssh_host_config_t *out) {
    return ESP_ERR_NOT_FOUND; /* Exercise production host-list fallback. */
}
esp_err_t ts_action_template_get(const char *id, ts_action_template_t *out) {
    if (strcmp(id, action_template.id)) return ESP_ERR_NOT_FOUND;
    *out = action_template; return ESP_OK;
}
bool ts_ssh_service_start_admissible(const char *id) { return true; }
esp_err_t ts_ssh_service_pin(const ts_ssh_command_config_t *cmd, const char *host,
                             uint16_t port, uint32_t *registration) {
    assert(!strcmp(cmd->id, command.id)); assert(port == 22);
    ++pins; *registration = 42; return ESP_OK;
}
void ts_ssh_service_unpin(const char *id, uint32_t registration) {
    assert(registration == 42); assert(pins > 0); --pins; ++unpins;
}
bool ts_ssh_service_complete_operation(const char *id, uint32_t operation, esp_err_t error) {
    ++direct_completions; return true;
}
void ts_ssh_service_recovery_kick(const char *id) { assert(!"normal actions are not controls"); }
static esp_err_t execute_service_control(const ts_action_queue_entry_t *e, ts_action_result_t *r) {
    assert(!"normal actions must not execute service controls"); return ESP_FAIL;
}
static esp_err_t execute_action_internal(const ts_auto_action_t *action, ts_action_result_t *result) {
    ++executed;
    if (action->runtime_binding) {
        const action_binding_t *binding = action->runtime_binding;
        strcpy(observed_command, binding->command.command);
        strcpy(observed_host, binding->host.host);
        assert(atomic_load(&binding->refs) > 0);
    }
    result->status = execution_status;
    strcpy(result->output, "executor result");
    return execution_status == TS_ACTION_STATUS_SUCCESS ? ESP_OK : ESP_FAIL;
}
static esp_err_t execute_ssh_action(const ts_auto_action_t *a) { ++legacy_ssh; return ESP_OK; }
static esp_err_t execute_gpio_action(const ts_auto_action_t *a) { ++legacy_gpio; return ESP_OK; }
static esp_err_t execute_webhook_action(const ts_auto_action_t *a) { ++legacy_webhook; return ESP_OK; }
static esp_err_t execute_device_action(const ts_auto_action_t *a) { return ESP_OK; }
static esp_err_t execute_cli_action(const ts_auto_action_t *a) { return ESP_OK; }
esp_err_t ts_action_exec_led(const ts_auto_action_led_t *a, ts_action_result_t *r) { return ESP_OK; }
esp_err_t ts_variable_set(const char *id, const ts_auto_value_t *value) { return ESP_OK; }
bool ts_rule_eval_condition(const ts_auto_condition_t *condition) { return condition_matches; }

static void callback(const ts_auto_action_t *a, const ts_action_result_t *result, void *data) {
    assert(data == (void *)123 && result->status == execution_status);
    assert(a->runtime_binding); ++callback_calls;
}

#define xSemaphoreTake test_take
#define xSemaphoreGive test_give
#define free audited_free
#include "action_submission.inc"
#undef free

static void drain(void) {
    bool running = s_ctx->running;
    action_executor_task(NULL);
    s_ctx->running = running;
    s_ctx->executor_task = (void *)1;
}
static void fixture(void) {
    assert(!queue_count && !pins && !sem_live && !live_allocations);
    memset(s_ctx, 0, sizeof(*s_ctx));
    s_ctx->running = s_ctx->accepting = true;
    s_ctx->executor_task = s_ctx->action_queue = (void *)1;
    s_ctx->stats_mutex = &stats_mutex;
    s_ctx->ssh_hosts_mutex = &host_mutex;
    s_ctx->templates_mutex = &template_mutex;
    s_ctx->ssh_host_count = 1;
    strcpy(s_ctx->ssh_hosts[0].id, "host");
    strcpy(s_ctx->ssh_hosts[0].host, "192.0.2.1");
    strcpy(s_ctx->ssh_hosts[0].password, "secret");
    s_ctx->ssh_hosts[0].port = 22;
    command = (ts_ssh_command_config_t){.enabled = true, .nohup = true, .service_mode = true};
    strcpy(command.id, "model"); strcpy(command.host_id, "host");
    strcpy(command.command, "original command");
    action_template = (ts_action_template_t){.enabled = true};
    strcpy(action_template.id, "template");
    action_template.action.type = TS_AUTO_ACT_SSH_CMD_REF;
    action_template.action.delay_ms = 75;
    strcpy(action_template.action.ssh_ref.cmd_id, command.id);
    s_ctx->templates[0] = action_template; s_ctx->template_count = 1;
    fail_alloc_after = -1; lookup_error = fail_semaphore = fail_send = 0;
    timeout_wait = immediate_execution = stop_on_send = 0;
    executed = legacy_webhook = legacy_ssh = legacy_gpio = callback_calls = direct_completions = 0;
    delay_total = last_delay = last_wait = 0;
    condition_matches = true; change_condition_on_delay = false;
    execution_status = TS_ACTION_STATUS_SUCCESS;
}
static ts_auto_action_t prepared(bool async) {
    ts_auto_action_t source = {.type = TS_AUTO_ACT_SSH_CMD_REF, .delay_ms = 125};
    strcpy(source.template_id, action_template.id);
    ts_auto_action_t out;
    assert(ts_action_snapshot(&source, &out) == ESP_OK && out.runtime_snapshot && pins == 1);
    out.async = async; return out;
}
static void finish(ts_auto_action_t *action) {
    if (action) ts_action_snapshot_release(action);
    assert(!queue_count && !s_ctx->pending && !s_ctx->direct_pending);
    assert(!pins && !sem_live && !live_allocations && !direct_completions);
}

int main(void) {
    fixture(); ts_auto_action_t a = prepared(false);
    int success = 0, failed = 0;
    assert(execute_actions_with_stats(&a, 1, &success, &failed) == ESP_OK);
    assert(success == 1 && !failed && executed == 1 && delay_total == 125 && last_delay == 0);
    assert(a.delay_ms == 125 && atomic_load(&((action_binding_t *)a.runtime_binding)->refs) == 1);
    assert(last_wait == 60000); finish(&a);

    fixture(); a = prepared(false); ts_action_result_t result = {0};
    assert(ts_action_manager_execute(&a, &result) == ESP_OK);
    assert(delay_total == 125 && last_delay == 125 && result.status == TS_ACTION_STATUS_SUCCESS);
    finish(&a);
    fixture(); assert(ts_action_template_execute("template", &result) == ESP_OK);
    assert(last_delay == 75 && delay_total == 75 && executed == 1); finish(NULL);
    fixture(); action_template.async = true;
    assert(ts_action_template_execute("template", &result) == ESP_OK);
    assert(result.status == TS_ACTION_STATUS_QUEUED && queue_count == 1 && !executed);
    drain(); assert(delay_total == 75); finish(NULL);

    fixture(); a = prepared(true);
    assert(ts_action_execute(&a) == ESP_OK && queue_count == 1 && s_ctx->pending == 1);
    ts_action_snapshot_release(&a); assert(pins == 1);
    strcpy(command.command, "replacement command"); strcpy(s_ctx->ssh_hosts[0].host, "192.0.2.2");
    drain(); assert(!strcmp(observed_command, "original command") && !strcmp(observed_host, "192.0.2.1"));
    assert(!delay_total); finish(NULL);

    fixture(); a = prepared(true);
    assert(ts_action_queue(&a, callback, (void *)123, 3) == ESP_OK && queue[0].priority == 3);
    ts_action_snapshot_release(&a); drain(); assert(callback_calls == 1 && delay_total == 125); finish(NULL);

    for (int async = 0; async <= 1; ++async) {
        fixture(); a = prepared(async); immediate_execution = 1;
        assert(ts_action_execute(&a) == ESP_OK && executed == 1 && !s_ctx->pending);
        finish(&a);
    }
    fixture(); immediate_execution = 1;
    assert(ts_action_template_execute("template", &result) == ESP_OK); finish(NULL);

    fixture(); a = prepared(false); timeout_wait = 1;
    assert(ts_action_manager_execute(&a, &result) == ESP_ERR_TIMEOUT);
    assert(result.status == TS_ACTION_STATUS_TIMEOUT && sem_live == 1 && s_ctx->pending == 1);
    ts_action_snapshot_release(&a); timeout_wait = 0; drain();
    assert(result.status == TS_ACTION_STATUS_TIMEOUT && executed == 1); finish(NULL);
    fixture(); a = prepared(false); timeout_wait = 1;
    assert(ts_action_manager_execute(&a, &result) == ESP_ERR_TIMEOUT);
    ts_action_snapshot_release(&a);
    assert(ts_action_cancel_all() == ESP_OK && !executed);
    assert(result.status == TS_ACTION_STATUS_TIMEOUT); finish(NULL);

    fixture(); a = prepared(false); stop_on_send = 1;
    assert(ts_action_execute(&a) == ESP_FAIL && !executed);
    assert(ts_action_manager_resume() == ESP_OK); finish(&a);

    for (int async = 0; async <= 1; ++async) {
        fixture(); a = prepared(async); fail_send = 1;
        assert(ts_action_execute(&a) == ESP_ERR_NO_MEM && !queue_count && !s_ctx->pending);
        assert(atomic_load(&((action_binding_t *)a.runtime_binding)->refs) == 1); finish(&a);
    }
    fixture(); a = prepared(true);
    for (unsigned n = 0; n < 16; ++n) assert(ts_action_execute(&a) == ESP_OK);
    assert(ts_action_execute(&a) == ESP_ERR_NO_MEM && s_ctx->pending == 16);
    assert(ts_action_manager_quiesce() == ESP_ERR_TIMEOUT);
    assert(ts_action_execute(&a) == ESP_ERR_INVALID_STATE);
    assert(ts_action_cancel_all() == ESP_OK && !s_ctx->pending && !executed);
    assert(ts_action_manager_resume() == ESP_OK); finish(&a);

    fixture(); a = prepared(false); fail_alloc_after = 0;
    assert(ts_action_execute(&a) == ESP_ERR_NO_MEM && !s_ctx->pending); finish(&a);
    fixture(); a = prepared(false); fail_semaphore = 1;
    assert(ts_action_execute(&a) == ESP_ERR_NO_MEM && !s_ctx->pending); finish(&a);
    ts_auto_action_t raw = {.type = TS_AUTO_ACT_SSH_CMD_REF}; strcpy(raw.ssh_ref.cmd_id, "model");
    fixture(); fail_alloc_after = 1;
    assert(ts_action_manager_execute(&raw, &result) == ESP_ERR_NO_MEM); finish(NULL);
    fixture(); lookup_error = ESP_ERR_NOT_FOUND;
    assert(ts_action_manager_execute(&raw, &result) == ESP_ERR_NOT_FOUND); finish(NULL);
    fixture(); s_ctx->ssh_host_count = 0;
    assert(ts_action_queue(&raw, NULL, NULL, 0) == ESP_ERR_NOT_FOUND); finish(NULL);
    fixture(); ts_auto_action_t missing = {.type = TS_AUTO_ACT_TEMPLATE_REF}; strcpy(missing.template_id, "missing");
    assert(ts_action_manager_execute(&missing, &result) == ESP_ERR_NOT_FOUND); finish(NULL);

    fixture(); a = prepared(false); a.repeat_mode = TS_AUTO_REPEAT_COUNT; a.repeat_count = 3; a.repeat_interval_ms = 30;
    assert(execute_actions_with_stats(&a, 1, &success, &failed) == ESP_OK);
    assert(executed == 3 && delay_total == 185 && a.delay_ms == 125); finish(&a);
    fixture(); a = prepared(true); a.repeat_mode = TS_AUTO_REPEAT_COUNT; a.repeat_count = 3; a.repeat_interval_ms = 30;
    assert(execute_actions_with_stats(&a, 1, &success, &failed) == ESP_OK && queue_count == 3);
    ts_action_snapshot_release(&a); drain(); assert(executed == 3 && delay_total == 185); finish(NULL);
    fixture(); a = prepared(false); a.condition.has_condition = true; condition_matches = false;
    assert(execute_action_with_repeat(&a, NULL, NULL) == ESP_OK && !queue_count && !executed); finish(&a);
    fixture(); a = prepared(false); a.condition.has_condition = true;
    a.repeat_mode = TS_AUTO_REPEAT_WHILE_TRUE; a.repeat_interval_ms = 20;
    change_condition_on_delay = true;
    assert(execute_action_with_repeat(&a, NULL, NULL) == ESP_OK && executed == 1 && delay_total == 20);
    finish(&a);
    fixture(); a = prepared(false); execution_status = TS_ACTION_STATUS_FAILED;
    assert(ts_action_execute(&a) == ESP_FAIL); finish(&a);
    fixture(); raw = (ts_auto_action_t){.type = TS_AUTO_ACT_LOG, .runtime_snapshot = true, .delay_ms = 100};
    assert(ts_action_execute(&raw) == ESP_OK && executed == 1 && !delay_total); finish(NULL);

    fixture(); raw = (ts_auto_action_t){.type = TS_AUTO_ACT_WEBHOOK};
    assert(ts_action_execute(&raw) == ESP_OK && legacy_webhook == 1 && !queue_count);
    raw.type = TS_AUTO_ACT_SSH_CMD; assert(ts_action_execute(&raw) == ESP_OK && legacy_ssh == 1 && !queue_count);
    raw.type = TS_AUTO_ACT_GPIO; assert(ts_action_execute(&raw) == ESP_OK && legacy_gpio == 1 && !queue_count);
    assert(ts_action_submit_prepared(&raw) == ESP_ERR_INVALID_ARG); finish(NULL);
    fixture(); raw = (ts_auto_action_t){.type = TS_AUTO_ACT_SSH_CMD_REF}; strcpy(raw.ssh_ref.cmd_id, command.id);
    assert(ts_action_execute(&raw) == ESP_OK && queue_count == 1 && !executed);
    drain(); finish(NULL);

    for (unsigned n = 0; n < 50; ++n) {
        fixture(); a = prepared(n % 2); assert(ts_action_execute(&a) == ESP_OK);
        ts_action_snapshot_release(&a); drain(); finish(NULL);
    }
    puts("PASS production snapshot -> sequencer -> submission -> executor: delay once, immutable binding, sync/async, fast/late completion, failures, cancellation, repeat, legacy routing; 50 cycles balanced");
    return 0;
}
