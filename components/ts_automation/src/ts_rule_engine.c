#include "ts_rule_codec.h"
#include "ts_rule_store.h"
#include "ts_ssh_commands_config.h"
#include "ts_ssh_service.h"
/**
 * @file ts_rule_engine.c
 * @brief TianShanOS 自动化引擎 - 规则引擎实现
 *
 * 规则引擎负责：
 * - 条件评估（比较操作符、逻辑组合）
 * - 动作执行（LED、SSH、GPIO、Webhook 等）
 * - 冷却时间管理
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_rule_engine.h"
#include "ts_variable.h"
#include "ts_config_module.h"
#include "ts_config_pack.h"
#include "ts_storage.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// 动作执行依赖
#include "ts_led.h"
#include "ts_hal_gpio.h"
#include "ts_device_ctrl.h"
#include "ts_ssh_client.h"
#include "ts_action_manager.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static const char *TAG = "ts_rule_engine";

/*===========================================================================*/
/*                              配置常量                                      */
/*===========================================================================*/

#ifndef CONFIG_TS_AUTOMATION_MAX_RULES
#define CONFIG_TS_AUTOMATION_MAX_RULES  32
#endif

/** NVS namespace for rules */
#define NVS_NAMESPACE_RULES         "auto_rules"

/** NVS key for rule count */
#define NVS_KEY_RULE_COUNT          "count"

/** NVS key prefix for rules */
#define NVS_KEY_RULE_PREFIX         "rule_"

/** SD 卡独立文件目录 */
#define RULES_SDCARD_DIR            "/sdcard/config/rules"

/*===========================================================================*/
/*                              内部状态                                      */
/*===========================================================================*/

typedef struct {
    ts_auto_rule_t *rules;               // 规则数组
    int count;                           // 当前规则数量
    int capacity;                        // 最大容量
    SemaphoreHandle_t mutex;             // 访问互斥锁
    bool initialized;
    bool loaded, recovery_error;
    int source;
    uint32_t next_instance;
    unsigned retired;
    SemaphoreHandle_t transaction;
    struct {
        bool committing, executing, readonly;
    } meta[CONFIG_TS_AUTOMATION_MAX_RULES];
    ts_rule_engine_stats_t stats;        // 统计信息
} ts_rule_engine_ctx_t;

static ts_rule_engine_ctx_t s_rule_ctx = {
    .rules = NULL,
    .count = 0,
    .capacity = 0,
    .mutex = NULL,
    .initialized = false,
};

typedef struct {
    unsigned refs;
    bool retired;
    ts_auto_condition_t *conditions;
    ts_auto_action_t *actions;
} rule_payload_t;
static void payload_free(rule_payload_t *p) {
    if (p) {
        free(p->conditions);
        free(p->actions);
        free(p);
    }
}
static rule_payload_t *payload_adopt(ts_auto_rule_t *r) {
    rule_payload_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        p->conditions = r->conditions.conditions;
        p->actions = r->actions;
        r->lease = p;
    }
    return p;
}
static int find_rule_index(const char *id);
void ts_rule_resolve_presentation(ts_auto_rule_t *r) {
    bool service = false, unresolved = false;
    for (unsigned i = 0; i < r->action_count; ++i) {
        ts_auto_action_t *a = &r->actions[i];
        ts_action_template_t *tpl = NULL;
        if (a->template_id[0]) {
            tpl = heap_caps_calloc(1, sizeof(*tpl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!tpl || ts_action_template_get(a->template_id, tpl) != ESP_OK) {
                free(tpl);
                unresolved = true;
                continue;
            }
            a = &tpl->action;
        }
        if (a->type == TS_AUTO_ACT_SSH_CMD_REF) {
            ts_ssh_command_config_t *cmd =
                heap_caps_calloc(1, sizeof(*cmd), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!cmd || ts_ssh_commands_config_get(a->ssh_ref.cmd_id, cmd) != ESP_OK)
                unresolved = true;
            else
                service |= cmd->service_mode;
            free(cmd);
        }
        free(tpl);
    }
    if (!(r->presentation_fields & 1))
        r->show_on_dashboard = (r->enabled && r->manual_trigger) || service;
    if (!(r->presentation_fields & 2))
        r->allow_manual_trigger = r->manual_trigger || service;
    r->reference_unresolved = unresolved;
    if (!unresolved)
        r->presentation_fields = 3;
    if (r->lease && s_rule_ctx.initialized) {
        xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
        int index = find_rule_index(r->id);
        if (index >= 0 && s_rule_ctx.rules[index].revision == r->revision &&
            s_rule_ctx.rules[index].instance == r->instance) {
            s_rule_ctx.rules[index].show_on_dashboard = r->show_on_dashboard;
            s_rule_ctx.rules[index].allow_manual_trigger = r->allow_manual_trigger;
            s_rule_ctx.rules[index].reference_unresolved = unresolved;
            s_rule_ctx.rules[index].presentation_fields = r->presentation_fields;
        }
        xSemaphoreGive(s_rule_ctx.mutex);
    }
}

/*===========================================================================*/
/*                      执行历史环形缓冲区                                    */
/*===========================================================================*/

/**
 * @brief 执行历史环形缓冲区上下文
 * 
 * 使用固定大小数组避免动态内存分配
 * 内存占用：16 × 96 = 1536 字节
 */
typedef struct {
    ts_rule_exec_record_t records[TS_RULE_EXEC_HISTORY_SIZE];
    int head;                            // 下一个写入位置
    int count;                           // 当前记录数量
} ts_rule_exec_history_t;

static ts_rule_exec_history_t s_exec_history = {
    .head = 0,
    .count = 0,
};

/**
 * @brief 记录一条规则执行结果
 * 
 * @note 调用者需持有 mutex
 */
static void record_execution(const char *rule_id, 
                             ts_rule_exec_status_t status,
                             ts_rule_trigger_source_t source,
                             const char *message,
                             uint8_t action_count,
                             uint8_t failed_count)
{
    ts_rule_exec_record_t *rec = &s_exec_history.records[s_exec_history.head];
    
    // 填充记录
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->rule_id, rule_id, sizeof(rec->rule_id) - 1);
    rec->timestamp_ms = esp_timer_get_time() / 1000;
    rec->status = status;
    rec->source = source;
    if (message) {
        strncpy(rec->message, message, sizeof(rec->message) - 1);
    }
    rec->action_count = action_count;
    rec->failed_count = failed_count;
    
    // 移动 head
    s_exec_history.head = (s_exec_history.head + 1) % TS_RULE_EXEC_HISTORY_SIZE;
    if (s_exec_history.count < TS_RULE_EXEC_HISTORY_SIZE) {
        s_exec_history.count++;
    }
}

/*===========================================================================*/
/*                          静态函数声明                                      */
/*===========================================================================*/

static int find_rule_index(const char *id);
static int compare_values(const ts_auto_value_t *a, const ts_auto_value_t *b);
static esp_err_t execute_led_action(const ts_auto_action_t *action);
static esp_err_t execute_gpio_action(const ts_auto_action_t *action);
static esp_err_t execute_device_action(const ts_auto_action_t *action);
static esp_err_t execute_ssh_action(const ts_auto_action_t *action);
static esp_err_t execute_ssh_ref_action(const ts_auto_action_t *action);
static esp_err_t execute_cli_action(const ts_auto_action_t *action);
static esp_err_t execute_webhook_action(const ts_auto_action_t *action);
static esp_err_t execute_actions_with_stats(const ts_auto_action_t *actions, int count,
                                             int *success_count, int *fail_count);

/*===========================================================================*/
/*                              辅助函数                                      */
/*===========================================================================*/

/**
 * 查找规则索引
 */
static int find_rule_index(const char *id)
{
    for (int i = 0; i < s_rule_ctx.count; i++) {
        if (strcmp(s_rule_ctx.rules[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * 比较两个值
 * @return <0: a<b, 0: a==b, >0: a>b
 */
static int compare_values(const ts_auto_value_t *a, const ts_auto_value_t *b)
{
    // 类型不同时尝试转换比较
    if (a->type != b->type) {
        // 简化处理：都转为 float 比较
        double va = 0, vb = 0;
        
        switch (a->type) {
            case TS_AUTO_VAL_INT: va = a->int_val; break;
            case TS_AUTO_VAL_FLOAT: va = a->float_val; break;
            case TS_AUTO_VAL_BOOL: va = a->bool_val ? 1 : 0; break;
            default: return 0;
        }
        
        switch (b->type) {
            case TS_AUTO_VAL_INT: vb = b->int_val; break;
            case TS_AUTO_VAL_FLOAT: vb = b->float_val; break;
            case TS_AUTO_VAL_BOOL: vb = b->bool_val ? 1 : 0; break;
            default: return 0;
        }
        
        if (fabs(va - vb) < 0.0001) return 0;
        return (va < vb) ? -1 : 1;
    }

    // 同类型比较
    switch (a->type) {
        case TS_AUTO_VAL_BOOL:
            return (a->bool_val == b->bool_val) ? 0 : 
                   (a->bool_val ? 1 : -1);
        
        case TS_AUTO_VAL_INT:
            if (a->int_val == b->int_val) return 0;
            return (a->int_val < b->int_val) ? -1 : 1;
        
        case TS_AUTO_VAL_FLOAT:
            if (fabs(a->float_val - b->float_val) < 0.0001) return 0;
            return (a->float_val < b->float_val) ? -1 : 1;
        
        case TS_AUTO_VAL_STRING:
            return strcmp(a->str_val, b->str_val);
        
        default:
            return 0;
    }
}

/*===========================================================================*/
/*                              初始化                                        */
/*===========================================================================*/

esp_err_t ts_rule_engine_init(void)
{
    if (s_rule_ctx.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing rule engine (max %d rules)",
             CONFIG_TS_AUTOMATION_MAX_RULES);

    // 分配规则数组
    s_rule_ctx.capacity = CONFIG_TS_AUTOMATION_MAX_RULES;
    size_t alloc_size = s_rule_ctx.capacity * sizeof(ts_auto_rule_t);

    s_rule_ctx.rules = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
    if (!s_rule_ctx.rules) {
        s_rule_ctx.rules = malloc(alloc_size);
        if (!s_rule_ctx.rules) {
            ESP_LOGE(TAG, "Failed to allocate rule storage");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "Using DRAM for rule storage");
    }

    memset(s_rule_ctx.rules, 0, alloc_size);

    // 创建互斥锁
    if (!s_rule_ctx.mutex)
        s_rule_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_rule_ctx.transaction)
        s_rule_ctx.transaction = xSemaphoreCreateRecursiveMutex();
    if (!s_rule_ctx.mutex || !s_rule_ctx.transaction) {
        free(s_rule_ctx.rules);
        s_rule_ctx.rules = NULL;
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_rule_ctx.stats, 0, sizeof(s_rule_ctx.stats));
    s_rule_ctx.count = 0;
    s_rule_ctx.initialized = true;

    // 延迟加载规则（等待 SD 卡挂载，避免栈溢出）
    extern void ts_rule_deferred_load_task(void *arg);
    BaseType_t task_ret = xTaskCreateWithCaps(
        ts_rule_deferred_load_task,
        "rule_load",
        8192,               // 8KB 栈用于 SD 卡 I/O
        NULL,
        tskIDLE_PRIORITY + 1,
        NULL,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create deferred load task, loading synchronously");
        ts_rules_load();
    }

    ESP_LOGI(TAG, "Rule engine initialized (loading deferred)");
    return ESP_OK;
}

/**
 * @brief 延迟加载任务 - 等待 SD 卡挂载后加载规则
 */
void ts_rule_deferred_load_task(void *arg)
{
    (void)arg;
    
    // 等待 3 秒，确保 SD 卡和 NVS 都已就绪
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    if (!s_rule_ctx.initialized) {
        ESP_LOGW(TAG, "Rule engine not initialized, skip deferred load");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Deferred rule loading started");
    ts_rules_load();
    ESP_LOGI(TAG, "Deferred rule loading complete: %d rules", s_rule_ctx.count);
    
    vTaskDelete(NULL);
}

esp_err_t ts_rule_engine_deinit(void)
{
    if (!s_rule_ctx.initialized)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_rule_ctx.transaction, portMAX_DELAY);
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    bool busy = s_rule_ctx.retired != 0;
    for (int i = 0; i < s_rule_ctx.count; ++i)
        busy |= ((rule_payload_t *)s_rule_ctx.rules[i].lease)->refs != 0 ||
                s_rule_ctx.meta[i].executing;
    if (busy) {
        xSemaphoreGive(s_rule_ctx.mutex);
        xSemaphoreGiveRecursive(s_rule_ctx.transaction);
        return ESP_ERR_INVALID_STATE;
    }
    s_rule_ctx.initialized = false;
    xSemaphoreGive(s_rule_ctx.mutex);
    for (int i = 0; i < s_rule_ctx.count; ++i)
        payload_free(s_rule_ctx.rules[i].lease);
    free(s_rule_ctx.rules);
    s_rule_ctx.rules = NULL;
    s_rule_ctx.count = 0;
    xSemaphoreGiveRecursive(s_rule_ctx.transaction);
    /* Retain small mutexes for late rejected callers. */
    return ESP_OK;
}

/*===========================================================================*/
/*                              规则管理                                      */
/*===========================================================================*/

/* Readers pin only the immutable arrays; scalar metadata is copied in the short lock. */
esp_err_t ts_rule_acquire(const char *id, ts_auto_rule_t *out) {
    if (!id || !out || !s_rule_ctx.initialized)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    int i = find_rule_index(id);
    if (i < 0) {
        xSemaphoreGive(s_rule_ctx.mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_rule_ctx.rules[i];
    ++((rule_payload_t *)out->lease)->refs;
    xSemaphoreGive(s_rule_ctx.mutex);
    return ESP_OK;
}
void ts_rule_release(ts_auto_rule_t *r) {
    if (!r || !r->lease)
        return;
    rule_payload_t *p = r->lease;
    bool destroy = false;
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    if (--p->refs == 0 && p->retired) {
        --s_rule_ctx.retired;
        destroy = true;
    }
    xSemaphoreGive(s_rule_ctx.mutex);
    if (destroy)
        payload_free(p);
    memset(r, 0, sizeof(*r));
}
static bool same_config(const ts_auto_rule_t *a, const ts_auto_rule_t *b) {
    return !strcmp(a->id, b->id) && !strcmp(a->name, b->name) && !strcmp(a->icon, b->icon) &&
           a->enabled == b->enabled && a->manual_trigger == b->manual_trigger &&
           a->show_on_dashboard == b->show_on_dashboard &&
           a->allow_manual_trigger == b->allow_manual_trigger &&
           a->presentation_fields == b->presentation_fields && a->cooldown_ms == b->cooldown_ms &&
           a->conditions.logic == b->conditions.logic &&
           a->conditions.count == b->conditions.count && a->action_count == b->action_count &&
           (!a->conditions.count || !memcmp(a->conditions.conditions, b->conditions.conditions,
                                            a->conditions.count * sizeof(ts_auto_condition_t))) &&
           (!a->action_count ||
            !memcmp(a->actions, b->actions, a->action_count * sizeof(ts_auto_action_t)));
}
static bool protected_bindings(const ts_auto_rule_t *old, const ts_auto_rule_t *next) {
    bool actions_changed = !next || old->action_count != next->action_count ||
        (old->action_count && memcmp(old->actions, next->actions, old->action_count * sizeof(*old->actions)));
    if (actions_changed && ts_ssh_service_rule_protected(old->id)) return true;
    for (unsigned i = 0; i < old->action_count; i++) {
        const ts_auto_action_t *a = &old->actions[i];
        bool retained = false;
        if (next)
            for (unsigned n = 0; n < next->action_count; n++) {
                const ts_auto_action_t *b = &next->actions[n];
                if (a->template_id[0])
                    retained |= !strcmp(a->template_id, b->template_id);
                else if (a->type == TS_AUTO_ACT_SSH_CMD_REF)
                    retained |= b->type == a->type && !strcmp(a->ssh_ref.cmd_id, b->ssh_ref.cmd_id);
            }
        if (retained)
            continue;
        ts_action_template_t *tpl = NULL;
        if (a->template_id[0]) {
            tpl = heap_caps_malloc(sizeof(*tpl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!tpl)
                return true;
            esp_err_t lookup = ts_action_template_get(a->template_id, tpl);
            if (lookup != ESP_OK) {
                free(tpl);
                /* Missing configuration is not proof that a remote service ended.
                 * Without the old binding, conservatively consult independent runs. */
                if (lookup != ESP_ERR_NOT_FOUND || ts_ssh_service_any_in_use()) return true;
                continue;
            }
            a = &tpl->action;
        }
        bool busy = a->type == TS_AUTO_ACT_SSH_CMD_REF &&
                    ts_ssh_service_command_protected(a->ssh_ref.cmd_id);
        free(tpl);
        if (busy)
            return true;
    }
    return false;
}
esp_err_t ts_rule_commit(const ts_auto_rule_t *input, const char *id, uint32_t expected,
                         ts_rule_commit_result_t *result) {
    ts_rule_commit_result_t local = {.error_code = "invalid_argument"};
    if (!result)
        result = &local;
    *result = local;
    if (!ts_rule_id_valid(id) || !s_rule_ctx.initialized)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTakeRecursive(s_rule_ctx.transaction, portMAX_DELAY);
    ts_auto_rule_t candidate = {0};
    if (input) {
        cJSON *j = ts_rule_encode(input);
        if (!j) {
            xSemaphoreGiveRecursive(s_rule_ctx.transaction);
            return ESP_ERR_NO_MEM;
        }
        esp_err_t ret = ts_rule_decode(j, &candidate);
        cJSON_Delete(j);
        if (ret != ESP_OK) {
            xSemaphoreGiveRecursive(s_rule_ctx.transaction);
            return ret;
        }
        ts_rule_resolve_presentation(&candidate);
        if (strcmp(candidate.id, id)) {
            ts_rule_dispose(&candidate);
            xSemaphoreGiveRecursive(s_rule_ctx.transaction);
            return ESP_ERR_INVALID_ARG;
        }
        if (!payload_adopt(&candidate)) {
            ts_rule_dispose(&candidate);
            xSemaphoreGiveRecursive(s_rule_ctx.transaction);
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    int i = find_rule_index(id);
    esp_err_t ret = ESP_OK;
    if (!s_rule_ctx.loaded || s_rule_ctx.recovery_error) {
        ret = ESP_ERR_INVALID_STATE;
        result->error_code = "recovery_required";
    } else if ((i < 0 && expected != 0) || (i >= 0 && expected != s_rule_ctx.rules[i].revision)) {
        ret = ESP_ERR_INVALID_STATE;
        result->error_code = "revision_conflict";
    } else if (i < 0 && !input) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (i < 0 && s_rule_ctx.count == s_rule_ctx.capacity) {
        ret = ESP_ERR_NO_MEM;
        result->error_code = "capacity";
    } else if (i >= 0 && s_rule_ctx.meta[i].readonly) {
        ret = ESP_ERR_NOT_SUPPORTED;
        result->error_code = "source_read_only";
    } else if (i >= 0 && s_rule_ctx.retired &&
               ((rule_payload_t *)s_rule_ctx.rules[i].lease)->refs) {
        ret = ESP_ERR_INVALID_STATE;
        result->error_code = "busy_retired_config";
    } else if (i >= 0 && !input && s_rule_ctx.meta[i].executing) {
        ret = ESP_ERR_INVALID_STATE;
        result->error_code = "execution_busy";
    } else if (i >= 0 && input && same_config(&candidate, &s_rule_ctx.rules[i])) {
        result->applied = result->durable = 1;
        result->mirror_synced = -1;
        result->revision = s_rule_ctx.rules[i].revision;
        result->error_code = "no_change";
        xSemaphoreGive(s_rule_ctx.mutex);
        goto done;
    }
    if (ret != ESP_OK) {
        xSemaphoreGive(s_rule_ctx.mutex);
        goto done;
    }
    if (i >= 0)
        s_rule_ctx.meta[i].committing = true;
    if (i >= 0 && s_rule_ctx.rules[i].revision == UINT32_MAX) {
        s_rule_ctx.meta[i].committing = false;
        result->error_code = "revision_exhausted";
        ret = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_rule_ctx.mutex);
        goto done;
    }
    candidate.revision = i >= 0 ? s_rule_ctx.rules[i].revision + 1 : 1;
    xSemaphoreGive(s_rule_ctx.mutex);
    ts_ssh_binding_lock();
    if (i >= 0 && protected_bindings(&s_rule_ctx.rules[i], input ? &candidate : NULL)) {
        ts_ssh_binding_unlock();
        xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
        s_rule_ctx.meta[i].committing = false;
        xSemaphoreGive(s_rule_ctx.mutex);
        result->error_code = "service_busy";
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }
    /* The edit gate keeps payloads alive; copy scalar metadata under the rule
     * lock so cold presentation resolution cannot race the durable snapshot. */
    ts_auto_rule_t *persisted = s_rule_ctx.count
                                    ? heap_caps_malloc(s_rule_ctx.count * sizeof(*persisted),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                    : NULL;
    if (s_rule_ctx.count && !persisted) {
        ts_ssh_binding_unlock();
        xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
        if (i >= 0)
            s_rule_ctx.meta[i].committing = false;
        xSemaphoreGive(s_rule_ctx.mutex);
        result->error_code = "no_memory";
        ret = ESP_ERR_NO_MEM;
        goto done;
    }
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    if (persisted)
        memcpy(persisted, s_rule_ctx.rules, s_rule_ctx.count * sizeof(*persisted));
    xSemaphoreGive(s_rule_ctx.mutex);
    ts_rule_commit_t stored;
    ret = ts_rule_store_commit(persisted, s_rule_ctx.count, input ? &candidate : NULL, id,
                               s_rule_ctx.source, &stored);
    free(persisted);
    result->applied = stored.applied;
    result->durable = stored.durable;
    result->mirror_synced = stored.mirror_synced;
    result->error_code = stored.error_code;
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    if (stored.applied < 0)
        s_rule_ctx.recovery_error = true;
    rule_payload_t *old = NULL;
    if (ret == ESP_OK) {
        if (i >= 0) {
            old = s_rule_ctx.rules[i].lease;
            candidate.last_trigger_ms = s_rule_ctx.rules[i].last_trigger_ms;
            candidate.trigger_count = s_rule_ctx.rules[i].trigger_count;
            candidate.instance = s_rule_ctx.rules[i].instance;
        } else {
            i = s_rule_ctx.count++;
            memset(&s_rule_ctx.meta[i], 0, sizeof(s_rule_ctx.meta[i]));
            candidate.instance = ++s_rule_ctx.next_instance;
        }
        if (input) {
            s_rule_ctx.rules[i] = candidate;
            candidate.lease = NULL;
            result->revision = s_rule_ctx.rules[i].revision;
        } else {
            memmove(s_rule_ctx.rules + i, s_rule_ctx.rules + i + 1,
                    (s_rule_ctx.count - i - 1) * sizeof(ts_auto_rule_t));
            memmove(s_rule_ctx.meta + i, s_rule_ctx.meta + i + 1,
                    (s_rule_ctx.count - i - 1) * sizeof(s_rule_ctx.meta[0]));
            --s_rule_ctx.count;
            i = -1;
        }
        if (old && old->refs) {
            old->retired = true;
            ++s_rule_ctx.retired;
            old = NULL;
        }
    }
    if (i >= 0)
        s_rule_ctx.meta[i].committing = false;
    xSemaphoreGive(s_rule_ctx.mutex);
    payload_free(old);
    ts_ssh_binding_unlock();
done:
    xSemaphoreGiveRecursive(s_rule_ctx.transaction);
    payload_free(candidate.lease);
    return ret;
}
esp_err_t ts_rule_register(const ts_auto_rule_t *r) {
    return r ? ts_rule_commit(r, r->id, r->revision, NULL) : ESP_ERR_INVALID_ARG;
}
esp_err_t ts_rule_unregister(const char *id) {
    ts_auto_rule_t r;
    esp_err_t e = ts_rule_acquire(id, &r);
    if (e != ESP_OK)
        return e;
    uint32_t v = r.revision;
    ts_rule_release(&r);
    return ts_rule_commit(NULL, id, v, NULL);
}
static esp_err_t set_enabled(const char *id, bool enabled) {
    ts_auto_rule_t r;
    esp_err_t e = ts_rule_acquire(id, &r);
    if (e != ESP_OK)
        return e;
    r.enabled = enabled;
    e = ts_rule_commit(&r, id, r.revision, NULL);
    ts_rule_release(&r);
    return e;
}
esp_err_t ts_rule_enable(const char *id) { return set_enabled(id, true); }
esp_err_t ts_rule_disable(const char *id) { return set_enabled(id, false); }

int ts_rule_count(void)
{
    if (!s_rule_ctx.initialized) {
        return 0;
    }

    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    int count = s_rule_ctx.count;
    xSemaphoreGive(s_rule_ctx.mutex);

    return count;
}

/*===========================================================================*/
/*                              条件评估                                      */
/*===========================================================================*/

bool ts_rule_eval_condition(const ts_auto_condition_t *condition)
{
    if (!condition) {
        return false;
    }

    // 获取变量当前值
    ts_auto_value_t var_value;
    esp_err_t ret = ts_variable_get(condition->variable, &var_value);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Variable '%s' not found", condition->variable);
        return false;
    }

    /* Missing/non-numeric values must not compare equal after a failed coercion. */
    bool lhs_numeric = var_value.type >= TS_AUTO_VAL_BOOL && var_value.type <= TS_AUTO_VAL_FLOAT;
    bool rhs_numeric =
        condition->value.type >= TS_AUTO_VAL_BOOL && condition->value.type <= TS_AUTO_VAL_FLOAT;
    if (var_value.type == TS_AUTO_VAL_NULL || condition->value.type == TS_AUTO_VAL_NULL ||
        (var_value.type != condition->value.type && !(lhs_numeric && rhs_numeric)))
        return false;
    // 执行比较
    int cmp = compare_values(&var_value, &condition->value);

    switch (condition->op) {
        case TS_AUTO_OP_EQ:
            return (cmp == 0);
        
        case TS_AUTO_OP_NE:
            return (cmp != 0);
        
        case TS_AUTO_OP_LT:
            return (cmp < 0);
        
        case TS_AUTO_OP_LE:
            return (cmp <= 0);
        
        case TS_AUTO_OP_GT:
            return (cmp > 0);
        
        case TS_AUTO_OP_GE:
            return (cmp >= 0);
        
        case TS_AUTO_OP_CONTAINS:
            // 仅字符串支持
            if (var_value.type == TS_AUTO_VAL_STRING &&
                condition->value.type == TS_AUTO_VAL_STRING) {
                return (strstr(var_value.str_val, condition->value.str_val) != NULL);
            }
            return false;
        
        case TS_AUTO_OP_CHANGED:
            // TODO: 需要保存上一次的值来比较
            return false;
        
        case TS_AUTO_OP_CHANGED_TO:
            // TODO: 需要保存上一次的值来比较
            return false;
        
        default:
            return false;
    }
}

bool ts_rule_eval_condition_group(const ts_auto_condition_group_t *group)
{
    if (!group || !group->conditions || group->count == 0) {
        return false;  // 空条件组视为不满足（仅手动触发的规则）
    }

    if (group->logic == TS_AUTO_LOGIC_AND) {
        // AND: 所有条件都必须满足
        for (int i = 0; i < group->count; i++) {
            if (!ts_rule_eval_condition(&group->conditions[i])) {
                return false;
            }
        }
        return true;
    } else {
        // OR: 任一条件满足即可
        for (int i = 0; i < group->count; i++) {
            if (ts_rule_eval_condition(&group->conditions[i])) {
                return true;
            }
        }
        return false;
    }
}

/*===========================================================================*/
/*                              规则评估                                      */
/*===========================================================================*/

static esp_err_t execute_rule(const char *id, bool manual, bool *triggered) {
    if (!ts_action_manager_accepting()) return ESP_ERR_INVALID_STATE;
    *triggered = false;
    ts_auto_rule_t r;
    esp_err_t ret = ts_rule_acquire(id, &r);
    if (ret != ESP_OK)
        return ret;
    if (manual && (r.reference_unresolved || r.presentation_fields != 3))
        ts_rule_resolve_presentation(&r);
    if (!r.enabled || (!manual && r.manual_trigger) || (manual && !r.allow_manual_trigger)) {
        ts_rule_release(&r);
        return manual ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    if (!manual && !ts_rule_eval_condition_group(&r.conditions)) {
        ts_rule_release(&r);
        return ESP_OK;
    }
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    int preliminary = find_rule_index(id);
    bool unavailable = preliminary < 0 || !s_rule_ctx.loaded || s_rule_ctx.recovery_error ||
                       s_rule_ctx.meta[preliminary].committing ||
                       s_rule_ctx.meta[preliminary].executing ||
                       (!manual && r.cooldown_ms && r.last_trigger_ms &&
                        esp_timer_get_time() / 1000 - r.last_trigger_ms < r.cooldown_ms);
    xSemaphoreGive(s_rule_ctx.mutex);
    if (unavailable) {
        ts_rule_release(&r);
        return manual ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    for (unsigned n = 0; n < r.action_count; n++) {
        if (r.actions[n].type == TS_AUTO_ACT_SSH_CMD_REF &&
            !ts_ssh_service_start_admissible(r.actions[n].ssh_ref.cmd_id)) {
            ts_rule_release(&r);
            return manual ? ESP_ERR_INVALID_STATE : ESP_OK;
        }
    }
    /* Freeze all referenced actions/SSH bindings once at admission. No allocation
     * on unmatched evaluations; queue entries retain their own binding lease. */
    ts_auto_action_t *frozen =
        heap_caps_calloc(r.action_count, sizeof(*frozen), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frozen) {
        ts_rule_release(&r);
        return ESP_ERR_NO_MEM;
    }
    ts_ssh_binding_lock();
    unsigned prepared = 0;
    for (; prepared < r.action_count; ++prepared) {
        ret = ts_action_snapshot(&r.actions[prepared], &frozen[prepared]);
        if (ret != ESP_OK)
            break;
    }
    if (ret != ESP_OK) {
        ts_ssh_binding_unlock();
        for (unsigned n = 0; n < prepared; n++)
            ts_action_snapshot_release(&frozen[n]);
        free(frozen);
        ts_rule_release(&r);
        return ret;
    }
    int64_t now = esp_timer_get_time() / 1000;
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    int i = find_rule_index(id);
    bool valid = i >= 0 && !s_rule_ctx.recovery_error && s_rule_ctx.loaded &&
                 !s_rule_ctx.meta[i].committing && !s_rule_ctx.meta[i].executing &&
                 s_rule_ctx.rules[i].revision == r.revision &&
                 s_rule_ctx.rules[i].instance == r.instance;
    if (valid && !manual && r.cooldown_ms && s_rule_ctx.rules[i].last_trigger_ms &&
        now - s_rule_ctx.rules[i].last_trigger_ms < r.cooldown_ms)
        valid = false;
    if (valid)
        s_rule_ctx.meta[i].executing = true;
    xSemaphoreGive(s_rule_ctx.mutex);
    if (valid)
        for (unsigned n = 0; n < r.action_count; ++n) ts_action_snapshot_owner(&frozen[n], r.id);
    ts_ssh_binding_unlock();
    if (!valid) {
        for (unsigned n = 0; n < r.action_count; n++)
            ts_action_snapshot_release(&frozen[n]);
        free(frozen);
        ts_rule_release(&r);
        return manual ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    int success = 0, failed = 0;
    ret = execute_actions_with_stats(frozen, r.action_count, &success, &failed);
    for (unsigned n = 0; n < r.action_count; n++)
        ts_action_snapshot_release(&frozen[n]);
    free(frozen);
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    i = find_rule_index(id);
    if (i >= 0 && s_rule_ctx.rules[i].instance == r.instance) {
        s_rule_ctx.meta[i].executing = false;
        s_rule_ctx.rules[i].last_trigger_ms = now;
        ++s_rule_ctx.rules[i].trigger_count;
        ++s_rule_ctx.stats.total_triggers;
        record_execution(id, failed ? TS_RULE_EXEC_FAILED : TS_RULE_EXEC_SUCCESS,
                         manual ? TS_RULE_TRIGGER_MANUAL : TS_RULE_TRIGGER_CONDITION,
                         failed ? "action failed" : "actions accepted", r.action_count, failed);
    }
    xSemaphoreGive(s_rule_ctx.mutex);
    *triggered = true;
    ts_rule_release(&r);
    return ret;
}
esp_err_t ts_rule_evaluate(const char *id, bool *triggered) {
    if (!triggered)
        return ESP_ERR_INVALID_ARG;
    return execute_rule(id, false, triggered);
}

int ts_rule_evaluate_all(void)
{
    if (!s_rule_ctx.initialized) {
        return 0;
    }

    int triggered = 0;

    s_rule_ctx.stats.total_evaluations++;
    s_rule_ctx.stats.last_evaluation_ms = esp_timer_get_time() / 1000;

    // 获取规则 ID 列表（避免在评估过程中持有锁）
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    int count = s_rule_ctx.count;
    xSemaphoreGive(s_rule_ctx.mutex);

    for (int i = 0; i < count; i++) {
        xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
        
        if (i >= s_rule_ctx.count) {
            xSemaphoreGive(s_rule_ctx.mutex);
            break;  // 规则可能被删除
        }
        
        char id[TS_AUTO_NAME_MAX_LEN];
        strncpy(id, s_rule_ctx.rules[i].id, sizeof(id) - 1);
        id[sizeof(id) - 1] = '\0';
        
        xSemaphoreGive(s_rule_ctx.mutex);

        bool was_triggered = false;
        ts_rule_evaluate(id, &was_triggered);
        if (was_triggered) {
            triggered++;
        }
    }

    return triggered;
}

esp_err_t ts_rule_trigger(const char *id) {
    bool triggered;
    return execute_rule(id, true, &triggered);
}

/*===========================================================================*/
/*                          Action 执行器实现                                 */
/*===========================================================================*/

/**
 * @brief 解析 LED 设备名称（支持简短别名）
 */
static const char *resolve_led_device_name(const char *name)
{
    if (!name) return NULL;
    
    /* 支持简短别名 */
    if (strcmp(name, "touch") == 0) return "led_touch";
    if (strcmp(name, "board") == 0) return "led_board";
    if (strcmp(name, "matrix") == 0) return "led_matrix";
    
    /* 也支持完整名 */
    return name;
}

/**
 * @brief 执行 LED 动作
 * @note Reserved for automation LED action feature
 */
__attribute__((unused))
static esp_err_t execute_led_action(const ts_auto_action_t *action)
{
    if (!action || action->type != TS_AUTO_ACT_LED) {
        return ESP_ERR_INVALID_ARG;
    }

    // 解析设备名称
    const char *device_name = resolve_led_device_name(action->led.device);

    ESP_LOGI(TAG, "LED action: device=%s, index=%d, color=#%02X%02X%02X, effect=%s",
             device_name, action->led.index,
             action->led.r, action->led.g, action->led.b,
             action->led.effect[0] ? action->led.effect : "(none)");

    // 获取设备句柄
    ts_led_device_t device = ts_led_device_get(device_name);
    if (!device) {
        ESP_LOGW(TAG, "LED device '%s' not found", device_name);
        return ESP_ERR_NOT_FOUND;
    }

    // 获取默认 layer
    ts_led_layer_t layer = ts_led_layer_get(device, 0);
    if (!layer) {
        ESP_LOGW(TAG, "LED layer not found for device '%s'", device_name);
        return ESP_ERR_NOT_FOUND;
    }

    // 如果有效果名，启动效果动画
    if (action->led.effect[0]) {
        const ts_led_animation_def_t *anim = ts_led_animation_get_builtin(action->led.effect);
        if (anim) {
            ESP_LOGI(TAG, "Starting effect '%s' on device '%s'", action->led.effect, device_name);
            return ts_led_animation_start(layer, anim);
        } else {
            ESP_LOGW(TAG, "Effect '%s' not found", action->led.effect);
            // 继续尝试设置颜色
        }
    }

    ts_led_rgb_t color = TS_LED_RGB(action->led.r, action->led.g, action->led.b);

    // index = 0xFF 表示填充整个设备（根据 ts_automation_types.h）
    if (action->led.index == 0xFF) {
        return ts_led_fill(layer, color);
    }

    // 设置单个像素
    return ts_led_device_set_pixel(device, (uint16_t)action->led.index, color);
}

/**
 * @brief 执行 GPIO 动作
 */
static esp_err_t execute_gpio_action(const ts_auto_action_t *action)
{
    if (!action || action->type != TS_AUTO_ACT_GPIO) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "GPIO action: pin=%d, level=%d, pulse=%dms",
             action->gpio.pin, action->gpio.level, action->gpio.pulse_ms);

    // 使用 raw GPIO 方式（直接操作物理引脚）
    ts_gpio_handle_t handle = ts_gpio_create_raw(action->gpio.pin, "automation");
    if (!handle) {
        ESP_LOGE(TAG, "Failed to create GPIO handle for pin %d", action->gpio.pin);
        return ESP_ERR_NO_MEM;
    }

    // 配置为输出
    ts_gpio_config_t cfg = TS_GPIO_CONFIG_DEFAULT();
    cfg.direction = TS_GPIO_DIR_OUTPUT;
    esp_err_t ret = ts_gpio_configure(handle, &cfg);
    if (ret != ESP_OK) {
        ts_gpio_destroy(handle);
        return ret;
    }

    // 设置电平
    ret = ts_gpio_set_level(handle, action->gpio.level);

    // 如果是脉冲模式
    if (ret == ESP_OK && action->gpio.pulse_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(action->gpio.pulse_ms));
        ret = ts_gpio_set_level(handle, !action->gpio.level);
    }

    ts_gpio_destroy(handle);
    return ret;
}

/**
 * @brief 执行 SSH 命令引用动作
 * 
 * 通过 cmd_id 查找已注册的 SSH 命令并通过队列异步执行
 * 避免在 HTTP 任务中直接执行 SSH 操作导致栈溢出
 */
static esp_err_t execute_ssh_ref_action(const ts_auto_action_t *action)
{
    if (!action || action->type != TS_AUTO_ACT_SSH_CMD_REF) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *cmd_id = action->ssh_ref.cmd_id;
    ESP_LOGI(TAG, "SSH command ref action: cmd_id=%s (queued)", cmd_id);

    if (!cmd_id || cmd_id[0] == '\0') {
        ESP_LOGE(TAG, "Empty SSH command ID");
        return ESP_ERR_INVALID_ARG;
    }

    // 通过 action_manager 队列异步执行，避免栈溢出
    esp_err_t ret = ts_action_queue(action, NULL, NULL, 5);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to queue SSH action: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief 执行 CLI 命令动作
 * 
 * 通过队列异步执行 TianShanOS CLI 命令
 */
static esp_err_t execute_cli_action(const ts_auto_action_t *action)
{
    if (!action || action->type != TS_AUTO_ACT_CLI) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *command = action->cli.command;
    ESP_LOGD(TAG, "CLI action queued (%u bytes)", (unsigned)strlen(command));

    if (!command || command[0] == '\0') {
        ESP_LOGE(TAG, "Empty CLI command");
        return ESP_ERR_INVALID_ARG;
    }

    // 通过 action_manager 队列异步执行
    esp_err_t ret = ts_action_queue(action, NULL, NULL, 5);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to queue CLI action: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief 执行设备控制动作
 */
static esp_err_t execute_device_action(const ts_auto_action_t *action)
{
    if (!action || action->type != TS_AUTO_ACT_DEVICE_CTRL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Device action: device=%s, action=%s",
             action->device.device, action->device.action);

    // 解析设备 ID
    ts_device_id_t dev_id = TS_DEVICE_MAX;
    if (strcasecmp(action->device.device, "agx") == 0 ||
        strcasecmp(action->device.device, "AGX") == 0) {
        dev_id = TS_DEVICE_AGX;
    } else if (strcasecmp(action->device.device, "lpmu") == 0 ||
               strcasecmp(action->device.device, "LPMU") == 0) {
        dev_id = TS_DEVICE_LPMU;
    } else {
        ESP_LOGW(TAG, "Unknown device: %s", action->device.device);
        return ESP_ERR_NOT_FOUND;
    }

    // 执行动作
    const char *act = action->device.action;
    
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

/**
 * @brief 执行 SSH 命令动作
 */
static esp_err_t execute_ssh_action(const ts_auto_action_t *action)
{
    if (!action || action->type != TS_AUTO_ACT_SSH_CMD) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "SSH action: host=%s, cmd=%s",
             action->ssh.host_ref, action->ssh.command);

    // 从变量系统获取主机配置
    // host_ref 格式: "hosts.<name>" 或直接 IP
    char host[64] = {0};
    uint16_t port = 22;
    char username[32] = {0};
    char password[64] = {0};

    // 尝试从变量获取主机配置
    char var_name[96];  // 足够容纳 "hosts.<name>.password"
    
    // 获取 host
    snprintf(var_name, sizeof(var_name), "hosts.%s.ip", action->ssh.host_ref);
    ts_auto_value_t val;
    if (ts_variable_get(var_name, &val) == ESP_OK && val.type == TS_AUTO_VAL_STRING) {
        strncpy(host, val.str_val, sizeof(host) - 1);
    } else {
        // 直接使用 host_ref 作为 IP
        strncpy(host, action->ssh.host_ref, sizeof(host) - 1);
    }

    // 获取 port
    snprintf(var_name, sizeof(var_name), "hosts.%s.port", action->ssh.host_ref);
    if (ts_variable_get(var_name, &val) == ESP_OK && val.type == TS_AUTO_VAL_INT) {
        port = (uint16_t)val.int_val;
    }

    // 获取 username
    snprintf(var_name, sizeof(var_name), "hosts.%s.username", action->ssh.host_ref);
    if (ts_variable_get(var_name, &val) == ESP_OK && val.type == TS_AUTO_VAL_STRING) {
        strncpy(username, val.str_val, sizeof(username) - 1);
    } else {
        strncpy(username, "root", sizeof(username) - 1);  // 默认
    }

    // 获取 password（可选）
    snprintf(var_name, sizeof(var_name), "hosts.%s.password", action->ssh.host_ref);
    if (ts_variable_get(var_name, &val) == ESP_OK && val.type == TS_AUTO_VAL_STRING) {
        strncpy(password, val.str_val, sizeof(password) - 1);
    }

    // 配置 SSH
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    config.host = host;
    config.port = port;
    config.username = username;
    config.auth_method = TS_SSH_AUTH_PASSWORD;
    config.auth.password = password;
    config.timeout_ms = action->ssh.timeout_ms > 0 ? action->ssh.timeout_ms : 10000;

    // 执行命令
    ts_ssh_exec_result_t result = {0};
    esp_err_t ret = ts_ssh_exec_simple(&config, action->ssh.command, &result);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SSH command exit code: %d", result.exit_code);
        if (result.stdout_data && result.stdout_len > 0) {
            ESP_LOGD(TAG, "SSH stdout bytes: %u", (unsigned)result.stdout_len);
        }
        if (result.stderr_data && result.stderr_len > 0) {
            ESP_LOGW(TAG, "SSH stderr bytes: %u", (unsigned)result.stderr_len);
        }

        // 存储 exit_code 到变量（使用 host_ref 作为变量前缀）
        char result_var[TS_AUTO_NAME_MAX_LEN + 16];
        snprintf(result_var, sizeof(result_var), "ssh.%s.exit_code", action->ssh.host_ref);
        ts_auto_value_t res_val = {
            .type = TS_AUTO_VAL_INT,
            .int_val = result.exit_code
        };
        ts_variable_set(result_var, &res_val);
        
        // 非零 exit_code 视为执行失败
        if (result.exit_code != 0) {
            ESP_LOGW(TAG, "SSH command failed with exit code %d", result.exit_code);
            ret = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "SSH command failed: %s", esp_err_to_name(ret));
    }

    ts_ssh_exec_result_free(&result);
    return ret;
}

/**
 * @brief 执行 Webhook 动作
 */
static esp_err_t execute_webhook_action(const ts_auto_action_t *action)
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

/*===========================================================================*/
/*                              动作执行                                      */
/*===========================================================================*/

esp_err_t ts_action_execute(const ts_auto_action_t *action)
{
    if (!action) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    ts_action_result_t result = {0};

    if (action->runtime_snapshot) {
        ts_auto_action_t queued = *action;
        queued.delay_ms = 0; /* Rule sequencer already applied this delay. */
        return action->async ? ts_action_queue(&queued, NULL, NULL, 5)
                             : ts_action_manager_execute(&queued, &result);
    }
    /* 如果有 template_id，使用模板执行（模板包含完整的动作数据） */
    if (action->template_id[0] != '\0') {
        ESP_LOGD(TAG, "Executing action via template: %s", action->template_id);
        ret = ts_action_template_execute(action->template_id, &result);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Template action failed: %s (code=%d)", action->template_id, ret);
        }
        s_rule_ctx.stats.total_actions++;
        if (ret != ESP_OK) {
            s_rule_ctx.stats.failed_actions++;
        }
        return ret;
    }

    /* 内联动作（无模板引用） */
    ESP_LOGD(TAG, "Executing inline action type: %d", action->type);

    switch (action->type) {
        case TS_AUTO_ACT_LED:
            // 使用 ts_action_manager 的完整实现（支持所有 ctrl_type）
            ret = ts_action_exec_led(&action->led, &result);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "LED action failed: %s", result.output);
            }
            break;

        case TS_AUTO_ACT_SSH_CMD:
            ret = execute_ssh_action(action);
            break;

        case TS_AUTO_ACT_GPIO:
            ret = execute_gpio_action(action);
            break;

        case TS_AUTO_ACT_WEBHOOK:
            ret = execute_webhook_action(action);
            break;

        case TS_AUTO_ACT_LOG:
            ESP_LOG_LEVEL((esp_log_level_t)action->log.level, TAG, 
                          "Rule log: %s", action->log.message);
            break;

        case TS_AUTO_ACT_SET_VAR:
            ret = ts_variable_set(action->set_var.variable, &action->set_var.value);
            break;

        case TS_AUTO_ACT_DEVICE_CTRL:
            ret = execute_device_action(action);
            break;

        case TS_AUTO_ACT_SSH_CMD_REF:
            ret = execute_ssh_ref_action(action);
            break;

        case TS_AUTO_ACT_CLI:
            ret = execute_cli_action(action);
            break;

        default:
            ESP_LOGW(TAG, "Unknown action type: %d", action->type);
            ret = ESP_ERR_NOT_SUPPORTED;
    }

    s_rule_ctx.stats.total_actions++;
    if (ret != ESP_OK) {
        s_rule_ctx.stats.failed_actions++;
    }

    return ret;
}

/**
 * @brief Check if action's condition is met
 */
static bool check_action_condition(const ts_auto_action_t *action)
{
    if (!action->condition.has_condition) {
        return true;  // 没有条件，直接执行
    }
    
    // 构造条件结构并评估
    ts_auto_condition_t cond = {
        .op = action->condition.op,
        .value = action->condition.value,
    };
    strncpy(cond.variable, action->condition.variable, sizeof(cond.variable) - 1);
    
    bool result = ts_rule_eval_condition(&cond);
    
    ESP_LOGD(TAG, "Action condition check: %s %d %s -> %s",
             action->condition.variable, action->condition.op,
             action->condition.value.type == TS_AUTO_VAL_STRING ? 
                 action->condition.value.str_val : "(numeric)",
             result ? "PASS" : "SKIP");
    
    return result;
}

/**
 * @brief Execute a single action with repeat support
 */
/* Delays/repeat waits cooperate with stop without abandoning rule ownership. */
static bool rule_wait(uint32_t ms) {
    while (ms && ts_action_manager_accepting()) {
        uint32_t slice = ms > 100 ? 100 : ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
    return ts_action_manager_accepting();
}
static esp_err_t execute_action_with_repeat(const ts_auto_action_t *action,
                                             ts_action_result_cb_t callback,
                                             void *user_data)
{
    esp_err_t ret = ESP_OK;
    if (!ts_action_manager_accepting()) return ESP_ERR_INVALID_STATE;
    
    // 先检查动作级别的条件
    if (!check_action_condition(action)) {
        ESP_LOGI(TAG, "Action skipped: condition not met");
        return ESP_OK;  // 条件不满足，跳过执行
    }
    
    switch (action->repeat_mode) {
        case TS_AUTO_REPEAT_ONCE:
        default:
            // 单次执行
            ret = ts_action_execute(action);
            if (callback) {
                callback(action, ret, user_data);
            }
            break;
            
        case TS_AUTO_REPEAT_COUNT: {
            // 指定次数重复
            uint8_t count = action->repeat_count > 0 ? action->repeat_count : 1;
            uint16_t interval = action->repeat_interval_ms > 0 ? action->repeat_interval_ms : 1000;
            
            ESP_LOGI(TAG, "Repeat action %d times, interval=%dms", count, interval);
            
            for (uint8_t i = 0; i < count; i++) {
                if (!ts_action_manager_accepting()) return ESP_ERR_INVALID_STATE;
                // 每次重复前检查条件
                if (!check_action_condition(action)) {
                    ESP_LOGI(TAG, "Repeat stopped: condition no longer met");
                    break;
                }
                
                ret = ts_action_execute(action);
                if (callback) {
                    callback(action, ret, user_data);
                }
                
                // 最后一次不需要等待
                if (i < count - 1 && interval > 0) {
                    if (!rule_wait(interval)) return ESP_ERR_INVALID_STATE;
                }
            }
            break;
        }
        
        case TS_AUTO_REPEAT_WHILE_TRUE: {
            // 条件持续时重复
            uint16_t interval = action->repeat_interval_ms > 0 ? action->repeat_interval_ms : 1000;
            uint8_t max_iterations = 100;  // 安全限制，防止无限循环
            uint8_t iterations = 0;
            
            ESP_LOGI(TAG, "Repeat while condition true, interval=%dms, max=%d", interval, max_iterations);
            
            while (ts_action_manager_accepting() && check_action_condition(action) && iterations < max_iterations) {
                ret = ts_action_execute(action);
                if (callback) {
                    callback(action, ret, user_data);
                }
                iterations++;
                
                if (!rule_wait(interval)) return ESP_ERR_INVALID_STATE;
            }
            
            if (iterations >= max_iterations) {
                ESP_LOGW(TAG, "Repeat stopped: max iterations reached (%d)", max_iterations);
            }
            break;
        }
    }
    
    return ret;
}

/**
 * @brief 执行动作数组并统计成功/失败数量
 * 
 * @param actions 动作数组
 * @param count 动作数量
 * @param success_count 成功动作计数（输出）
 * @param fail_count 失败动作计数（输出）
 * @return 整体执行结果
 */
static esp_err_t execute_actions_with_stats(const ts_auto_action_t *actions, int count,
                                             int *success_count, int *fail_count)
{
    if (!actions || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int success = 0, fail = 0;
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Executing %d actions sequentially", count);

    for (int i = 0; i < count; i++) {
        if (!ts_action_manager_accepting()) { fail += count - i; ret = ESP_ERR_INVALID_STATE; break; }
        ESP_LOGI(TAG, "Action [%d/%d]: type=%d, template=%s, delay=%dms", 
                 i + 1, count, actions[i].type, 
                 actions[i].template_id[0] ? actions[i].template_id : "(inline)",
                 actions[i].delay_ms);
        
        // 动作前延迟
        if (actions[i].delay_ms > 0) {
            ESP_LOGI(TAG, "  Waiting %dms before action", actions[i].delay_ms);
            if (!rule_wait(actions[i].delay_ms)) { fail += count - i; ret = ESP_ERR_INVALID_STATE; break; }
        }

        esp_err_t action_ret = execute_action_with_repeat(&actions[i], NULL, NULL);
        if (action_ret == ESP_OK) {
            success++;
        } else {
            fail++;
            ret = action_ret;  // 记录最后一个错误
        }
        
        // LED Matrix 动作后自动添加延迟，确保渲染完成
        // 这对于连续的 LED 操作（如 image + filter）很重要
        if (actions[i].type == TS_AUTO_ACT_LED) {
            const ts_auto_action_led_t *led = &actions[i].led;
            ESP_LOGI(TAG, "  LED action: device=%s, ctrl_type=%d", led->device, led->ctrl_type);
            
            // Matrix 设备的渲染操作需要等待
            if (strcmp(led->device, "matrix") == 0 || 
                strcmp(led->device, "led_matrix") == 0) {
                int delay_after = 0;
                switch (led->ctrl_type) {
                    case TS_LED_CTRL_IMAGE:
                    case TS_LED_CTRL_TEXT:
                    case TS_LED_CTRL_QRCODE:
                    case TS_LED_CTRL_EFFECT:
                        delay_after = 100;
                        break;
                    case TS_LED_CTRL_FILTER:
                        delay_after = 50;
                        break;
                    default:
                        delay_after = 20;
                        break;
                }
                if (delay_after > 0) {
                    ESP_LOGI(TAG, "  Auto delay %dms after LED Matrix action", delay_after);
                    rule_wait(delay_after);
                }
            }
        }
    }
    
    if (success_count) *success_count = success;
    if (fail_count) *fail_count = fail;
    
    ESP_LOGI(TAG, "All %d actions executed (success=%d, fail=%d)", count, success, fail);
    
    return (fail == 0) ? ESP_OK : ret;
}

esp_err_t ts_action_execute_array(const ts_auto_action_t *actions, int count,
                                   ts_action_result_cb_t callback, void *user_data)
{
    // 兼容旧 API，忽略统计
    return execute_actions_with_stats(actions, count, NULL, NULL);
}

/*===========================================================================*/
/*                              规则访问                                       */
/*===========================================================================*/

esp_err_t ts_rule_get_by_index(int index, ts_auto_rule_t *rule)
{
    if (!rule) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_rule_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);

    if (index < 0 || index >= s_rule_ctx.count) {
        xSemaphoreGive(s_rule_ctx.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(rule, &s_rule_ctx.rules[index], sizeof(ts_auto_rule_t));
    ++((rule_payload_t *)rule->lease)->refs;
    xSemaphoreGive(s_rule_ctx.mutex);

    return ESP_OK;
}

/*===========================================================================*/
/*                              统计                                          */
/*===========================================================================*/

esp_err_t ts_rule_engine_get_stats(ts_rule_engine_stats_t *stats)
{
    if (!stats || !s_rule_ctx.initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    *stats = s_rule_ctx.stats;
    xSemaphoreGive(s_rule_ctx.mutex);

    return ESP_OK;
}

esp_err_t ts_rule_engine_reset_stats(void)
{
    if (!s_rule_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    memset(&s_rule_ctx.stats, 0, sizeof(s_rule_ctx.stats));
    xSemaphoreGive(s_rule_ctx.mutex);

    return ESP_OK;
}

/*===========================================================================*/
/*                          执行历史查询                                      */
/*===========================================================================*/

const char *ts_rule_exec_status_str(ts_rule_exec_status_t status)
{
    switch (status) {
        case TS_RULE_EXEC_SUCCESS: return "SUCCESS";
        case TS_RULE_EXEC_PARTIAL: return "PARTIAL";
        case TS_RULE_EXEC_FAILED:  return "FAILED";
        case TS_RULE_EXEC_SKIPPED: return "SKIPPED";
        default:                   return "UNKNOWN";
    }
}

esp_err_t ts_rule_get_exec_history(ts_rule_exec_record_t *records, 
                                    int max_count, int *actual_count)
{
    if (!records || max_count <= 0 || !actual_count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_rule_ctx.initialized) {
        *actual_count = 0;
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    
    int count = (max_count < s_exec_history.count) ? max_count : s_exec_history.count;
    *actual_count = count;
    
    // 从最新到最旧返回（逆序）
    for (int i = 0; i < count; i++) {
        // head 指向下一个写入位置，所以最新的是 head-1
        int idx = (s_exec_history.head - 1 - i + TS_RULE_EXEC_HISTORY_SIZE) 
                  % TS_RULE_EXEC_HISTORY_SIZE;
        records[i] = s_exec_history.records[idx];
    }
    
    xSemaphoreGive(s_rule_ctx.mutex);
    return ESP_OK;
}

esp_err_t ts_rule_get_exec_history_by_id(const char *rule_id,
                                          ts_rule_exec_record_t *records,
                                          int max_count, int *actual_count)
{
    if (!rule_id || !records || max_count <= 0 || !actual_count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_rule_ctx.initialized) {
        *actual_count = 0;
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    
    int out_count = 0;
    
    // 从最新到最旧遍历
    for (int i = 0; i < s_exec_history.count && out_count < max_count; i++) {
        int idx = (s_exec_history.head - 1 - i + TS_RULE_EXEC_HISTORY_SIZE) 
                  % TS_RULE_EXEC_HISTORY_SIZE;
        
        if (strcmp(s_exec_history.records[idx].rule_id, rule_id) == 0) {
            records[out_count++] = s_exec_history.records[idx];
        }
    }
    
    *actual_count = out_count;
    
    xSemaphoreGive(s_rule_ctx.mutex);
    return ESP_OK;
}

esp_err_t ts_rule_clear_exec_history(void)
{
    if (!s_rule_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    s_exec_history.head = 0;
    s_exec_history.count = 0;
    memset(s_exec_history.records, 0, sizeof(s_exec_history.records));
    xSemaphoreGive(s_rule_ctx.mutex);
    
    ESP_LOGI(TAG, "Execution history cleared");
    return ESP_OK;
}

/*===========================================================================*/
/*                              NVS 持久化                                    */
/*===========================================================================*/

/**
 * @brief 操作符转字符串
 */
static esp_err_t decode_text(const char *text, ts_auto_rule_t *r) {
    cJSON *j = cJSON_Parse(text);
    if (!j)
        return ESP_ERR_INVALID_ARG;
    esp_err_t e =
        ts_rule_decode(cJSON_HasObjectItem(j, "rule") ? cJSON_GetObjectItem(j, "rule") : j, r);
    cJSON_Delete(j);
    return e;
}
static esp_err_t load_one(const char *path, ts_auto_rule_t *r, bool *encrypted) {
    char *text = NULL;
    size_t len = 0;
    esp_err_t e = ts_config_pack_load_with_priority(path, &text, &len, encrypted);
    if (e == ESP_OK)
        e = decode_text(text, r);
    free(text);
    return e;
}
static esp_err_t load_directory(ts_auto_rule_t *rules, int *count, bool *readonly) {
    DIR *dir = opendir(RULES_SDCARD_DIR);
    if (!dir)
        return ESP_ERR_NOT_FOUND;
    esp_err_t e = ESP_OK;
    struct dirent *entry;
    *count = 0;
    while ((entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        bool enc = n > 6 && !strcmp(entry->d_name + n - 6, ".tscfg");
        bool json = n > 5 && !strcmp(entry->d_name + n - 5, ".json");
        if (!enc && !json)
            continue;
        char path[160], id[TS_AUTO_NAME_MAX_LEN];
        size_t idlen = n - (enc ? 6 : 5);
        if (idlen >= sizeof(id)) {
            e = ESP_ERR_INVALID_SIZE;
            break;
        }
        memcpy(id, entry->d_name, idlen);
        id[idlen] = 0;
        if (!ts_rule_id_valid(id)) {
            e = ESP_ERR_INVALID_ARG;
            break;
        }
        if (json) {
            snprintf(path, sizeof(path), RULES_SDCARD_DIR "/%s.tscfg", id);
            struct stat st;
            if (stat(path, &st) == 0)
                continue;
        }
        if (*count >= s_rule_ctx.capacity) {
            e = ESP_ERR_NO_MEM;
            break;
        }
        snprintf(path, sizeof(path), RULES_SDCARD_DIR "/%s.json", id);
        bool encrypted = false;
        e = load_one(path, &rules[*count], &encrypted);
        if (e != ESP_OK)
            break;
        readonly[*count] = encrypted || strcmp(id, rules[*count].id) != 0;
        ++*count;
    }
    closedir(dir);
    return e == ESP_OK && !*count ? ESP_ERR_NOT_FOUND : e;
}
static esp_err_t load_legacy_file(const char *path, ts_auto_rule_t *rules, int *count) {
    char *text = NULL;
    size_t len;
    bool encrypted;
    esp_err_t e = ts_config_pack_load_with_priority(path, &text, &len, &encrypted);
    if (e != ESP_OK)
        return e;
    cJSON *root = cJSON_Parse(text);
    free(text);
    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "rules");
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > s_rule_ctx.capacity) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    const cJSON *j;
    cJSON_ArrayForEach(j, array) {
        e = ts_rule_decode(j, &rules[*count]);
        if (e != ESP_OK)
            break;
        ++*count;
    }
    cJSON_Delete(root);
    return e;
}
static esp_err_t load_legacy_nvs(ts_auto_rule_t *rules, int *count) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE_RULES, NVS_READONLY, &h);
    *count = 0;
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (e != ESP_OK)
        return e;
    uint8_t n = 0;
    e = nvs_get_u8(h, NVS_KEY_RULE_COUNT, &n);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return ESP_OK;
    }
    if (e != ESP_OK || n > s_rule_ctx.capacity) {
        nvs_close(h);
        return ESP_FAIL;
    }
    for (unsigned i = 0; i < n; ++i) {
        char key[16];
        snprintf(key, sizeof(key), NVS_KEY_RULE_PREFIX "%u", i);
        size_t len = 0;
        e = nvs_get_str(h, key, NULL, &len);
        if (e != ESP_OK || !len || len > 4000) {
            e = ESP_FAIL;
            break;
        }
        char *text = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!text) {
            e = ESP_ERR_NO_MEM;
            break;
        }
        e = nvs_get_str(h, key, text, &len);
        if (e == ESP_OK)
            e = decode_text(text, &rules[*count]);
        free(text);
        if (e != ESP_OK)
            break;
        ++*count;
    }
    nvs_close(h);
    return e;
}
static esp_err_t load_rules(const char *file) {
    if (!s_rule_ctx.initialized)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_rule_ctx.transaction, portMAX_DELAY);
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    bool busy = s_rule_ctx.retired != 0;
    for (int i = 0; i < s_rule_ctx.count; ++i)
        busy |= ((rule_payload_t *)s_rule_ctx.rules[i].lease)->refs != 0 ||
                s_rule_ctx.meta[i].executing;
    if (busy) {
        xSemaphoreGive(s_rule_ctx.mutex);
        xSemaphoreGiveRecursive(s_rule_ctx.transaction);
        return ESP_ERR_INVALID_STATE;
    }
    s_rule_ctx.loaded = false;
    xSemaphoreGive(s_rule_ctx.mutex);
    ts_auto_rule_t *rules =
        heap_caps_calloc(s_rule_ctx.capacity, sizeof(*rules), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool readonly[CONFIG_TS_AUTOMATION_MAX_RULES] = {0};
    int count = 0, source = TS_RULE_SOURCE_NVS;
    esp_err_t e = rules ? ESP_OK : ESP_ERR_NO_MEM;
    bool staging = false, require_sd = false;
    if (e == ESP_OK)
        e = ts_rule_store_recover(ts_storage_sd_mounted(), &staging, &require_sd);
    if (e == ESP_OK) {
        if (file) {
            source = TS_RULE_SOURCE_READONLY;
            e = load_legacy_file(file, rules, &count);
        } else if (staging) {
            e = ts_rule_store_load_bank(rules, s_rule_ctx.capacity, &count);
            source = require_sd ? TS_RULE_SOURCE_READONLY : TS_RULE_SOURCE_NVS;
        } else {
            e = ESP_ERR_NOT_FOUND;
            if (ts_storage_sd_mounted()) {
                e = load_directory(rules, &count, readonly);
                if (e == ESP_OK || require_sd)
                    source = TS_RULE_SOURCE_SD;
                if (e == ESP_ERR_NOT_FOUND && !require_sd) {
                    e = load_legacy_file("/sdcard/config/rules.json", rules, &count);
                    if (e == ESP_OK)
                        source = TS_RULE_SOURCE_READONLY;
                }
            }
            if (e == ESP_ERR_NOT_FOUND && !require_sd)
                e = load_legacy_nvs(rules, &count);
            else if (e == ESP_ERR_NOT_FOUND &&
                     require_sd) { /* Committed empty SD directory remains authoritative. */
                DIR *d = opendir(RULES_SDCARD_DIR);
                if (d) {
                    closedir(d);
                    e = ESP_OK;
                }
            }
        }
    }
    if (e == ESP_OK)
        for (int i = 0; i < count; ++i) {
            for (int k = 0; k < i; ++k)
                if (!strcmp(rules[k].id, rules[i].id)) {
                    e = ESP_ERR_INVALID_ARG;
                    break;
                }
            if (e != ESP_OK)
                break;
            ts_rule_resolve_presentation(&rules[i]);
            if (!rules[i].revision)
                rules[i].revision = 1;
            if (!payload_adopt(&rules[i])) {
                e = ESP_ERR_NO_MEM;
                break;
            }
            rules[i].instance = ++s_rule_ctx.next_instance;
        }
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    if (e == ESP_OK) {
        ts_auto_rule_t *old = s_rule_ctx.rules;
        int oldcount = s_rule_ctx.count;
        s_rule_ctx.rules = rules;
        s_rule_ctx.count = count;
        s_rule_ctx.source = source;
        memset(s_rule_ctx.meta, 0, sizeof(s_rule_ctx.meta));
        for (int i = 0; i < count; ++i)
            s_rule_ctx.meta[i].readonly = readonly[i];
        s_rule_ctx.loaded = true;
        s_rule_ctx.recovery_error = false;
        xSemaphoreGive(s_rule_ctx.mutex);
        for (int i = 0; i < oldcount; ++i)
            payload_free(old[i].lease);
        free(old);
    } else {
        s_rule_ctx.recovery_error = true;
        xSemaphoreGive(s_rule_ctx.mutex);
        for (int i = 0; i < count; ++i) {
            if (rules[i].lease)
                payload_free(rules[i].lease);
            else
                ts_rule_dispose(&rules[i]);
        }
        free(rules);
    }
    xSemaphoreGiveRecursive(s_rule_ctx.transaction);
    return e;
}
esp_err_t ts_rules_load(void) { return load_rules(NULL); }
esp_err_t ts_rules_load_from_file(const char *path) {
    return path ? load_rules(path) : ESP_ERR_INVALID_ARG;
}
/* Mutations persist through ts_rule_commit; reads/loads never rewrite all rules. */
esp_err_t ts_rules_save(void) {
    return s_rule_ctx.loaded && !s_rule_ctx.recovery_error ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool ts_rule_edit_begin(void) {
    return s_rule_ctx.initialized && xSemaphoreTakeRecursive(s_rule_ctx.transaction, 0) == pdTRUE;
}
void ts_rule_edit_end(void) { xSemaphoreGiveRecursive(s_rule_ctx.transaction); }
void ts_rule_config_status(bool *loaded, bool *recovery) {
    if (!s_rule_ctx.initialized) {
        *loaded = false;
        *recovery = false;
        return;
    }
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    *loaded = s_rule_ctx.loaded;
    *recovery = s_rule_ctx.recovery_error;
    xSemaphoreGive(s_rule_ctx.mutex);
}
