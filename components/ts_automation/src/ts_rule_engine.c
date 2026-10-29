#include "ts_automation_runtime.h"
#include "ts_automation_config.h"
#include "ts_rule_codec.h"
#include "ts_ssh_hosts_config.h"
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

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// 动作执行依赖
#include "ts_action_manager.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "ts_rule_engine";

/*===========================================================================*/
/*                              配置常量                                      */
/*===========================================================================*/

#ifndef CONFIG_TS_AUTOMATION_MAX_RULES
#define CONFIG_TS_AUTOMATION_MAX_RULES  64
#endif
/*===========================================================================*/
/*                              内部状态                                      */
/*===========================================================================*/

typedef struct {
    ts_auto_rule_t *rules;               // 规则数组
    int count;                           // 当前规则数量
    int capacity;                        // 最大容量
    SemaphoreHandle_t mutex;             // 访问互斥锁
    bool initialized;
    bool loaded;
    bool config_enabled;
    const char *load_error;
    uint32_t next_instance;
    unsigned retired;
    struct {
        bool executing;
        struct {
            int64_t first_ms, next_ms;
            unsigned action;
            bool awaiting_launch, counted;
            const char *state, *reason;
        } start;
    } meta[CONFIG_TS_AUTOMATION_MAX_RULES];
    ts_rule_engine_stats_t stats;        // 统计信息
} ts_rule_engine_ctx_t;

static ts_rule_engine_ctx_t s_rule_ctx = {
    .rules = NULL,
    .count = 0,
    .capacity = 0,
    .mutex = NULL,
    .initialized = false,
    .config_enabled = true,
};

enum { AUTO_START_RETRY_MS=30000, AUTO_START_WINDOW_MS=360000 };

typedef struct {
    unsigned refs;
    bool retired;
    ts_auto_condition_t *conditions;
    ts_auto_action_t *actions;
} rule_payload_t;
struct ts_rule_config_view {
    __typeof__(s_rule_ctx.meta) metadata;
    bool keep_runtime;
    ts_auto_rule_t *rules;
    int count;
};
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
static bool check_action_condition(const ts_auto_action_t *action);
static esp_err_t execute_actions_with_stats(const ts_auto_action_t *actions, int count,
                                             int *success_count, int *fail_count,
                                             unsigned *resume, ts_action_result_t *start_result);

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
    if (!s_rule_ctx.mutex) {
        free(s_rule_ctx.rules);
        s_rule_ctx.rules = NULL;
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_rule_ctx.stats, 0, sizeof(s_rule_ctx.stats));
    s_rule_ctx.count = 0;
    s_rule_ctx.loaded = false;
    s_rule_ctx.config_enabled = false;
    s_rule_ctx.load_error = "config_loading";
    s_rule_ctx.initialized = true;

    ESP_LOGI(TAG, "Rule engine initialized");
    return ESP_OK;
}

esp_err_t ts_rule_engine_deinit(void)
{
    if (!s_rule_ctx.initialized)
        return ESP_ERR_INVALID_STATE;
    ts_ssh_binding_lock();
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    bool busy = s_rule_ctx.retired != 0;
    for (int i = 0; i < s_rule_ctx.count; ++i)
        busy |= ((rule_payload_t *)s_rule_ctx.rules[i].lease)->refs != 0 ||
                s_rule_ctx.meta[i].executing;
    if (busy) {
        xSemaphoreGive(s_rule_ctx.mutex);
        ts_ssh_binding_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_rule_ctx.initialized = false;
    xSemaphoreGive(s_rule_ctx.mutex);
    for (int i = 0; i < s_rule_ctx.count; ++i)
        payload_free(s_rule_ctx.rules[i].lease);
    free(s_rule_ctx.rules);
    s_rule_ctx.rules = NULL;
    s_rule_ctx.count = 0;
    s_rule_ctx.loaded = false;
    ts_ssh_binding_unlock();
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
void ts_rule_config_discard(ts_rule_config_view_t *view)
{
    if (!view) return;
    for (int i = 0; i < view->count; ++i) {
        if (view->rules[i].lease) payload_free(view->rules[i].lease);
        else ts_rule_dispose(&view->rules[i]);
    }
    free(view->rules);
    free(view);
}

esp_err_t ts_rule_config_prepare(const ts_auto_rule_t *rules, int count, bool keep_runtime, ts_rule_config_view_t **out)
{
    if (!out || count < 0 || (count && !rules)) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!s_rule_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (count > s_rule_ctx.capacity) return ESP_ERR_INVALID_SIZE;
    ts_rule_config_view_t *view = heap_caps_calloc(1, sizeof(*view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view) return ESP_ERR_NO_MEM;
    view->rules = heap_caps_calloc(s_rule_ctx.capacity, sizeof(*view->rules),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view->rules) { free(view); return ESP_ERR_NO_MEM; }
    view->count = count;view->keep_runtime=keep_runtime;
    for (int i = 0; i < count; ++i) {
        ts_auto_rule_t *rule = &view->rules[i];
        *rule = rules[i];
        rule->lease = NULL;
        rule->conditions.conditions = NULL;
        rule->actions = NULL;
        rule->last_trigger_ms = rule->trigger_count = rule->instance = 0;
        size_t conditions = rule->conditions.count * sizeof(*rule->conditions.conditions);
        size_t actions = rule->action_count * sizeof(*rule->actions);
        if (conditions) {
            rule->conditions.conditions = heap_caps_malloc(conditions, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!rule->conditions.conditions) goto oom;
            memcpy(rule->conditions.conditions, rules[i].conditions.conditions, conditions);
        }
        if (actions) {
            rule->actions = heap_caps_malloc(actions, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!rule->actions) goto oom;
            memcpy(rule->actions, rules[i].actions, actions);
        }
        if (!payload_adopt(rule)) goto oom;
    }
    *out = view;
    return ESP_OK;
oom:
    ts_rule_config_discard(view);
    return ESP_ERR_NO_MEM;
}

/* The retry belongs to these definitions, not to the version of the whole SD document. */
static const void *binding_definition(const ts_auto_document_t *doc,ts_auto_config_kind_t kind,const char *id)
{
    if(!doc)return NULL;
    const char *base;size_t stride;int count;
    if(kind==TS_AUTO_CONFIG_ACTION){base=(const char *)doc->actions;stride=sizeof(*doc->actions);count=doc->action_count;}
    else if(kind==TS_AUTO_CONFIG_COMMAND){base=(const char *)doc->commands;stride=sizeof(*doc->commands);count=doc->command_count;}
    else {base=(const char *)doc->hosts;stride=sizeof(*doc->hosts);count=doc->host_count;}
    for(int i=0;i<count;++i)if(!strcmp(base+i*stride,id))return base+i*stride;
    return NULL;
}
static bool rule_dependencies_same(const ts_auto_rule_t *rule,const ts_auto_document_t *previous,const ts_auto_document_t *next)
{
    for(unsigned i=0;i<rule->action_count;++i){
        const ts_auto_action_t *action=&rule->actions[i];
        if(action->template_id[0]){
            const ts_action_template_t *a=binding_definition(previous,TS_AUTO_CONFIG_ACTION,action->template_id);
            const ts_action_template_t *b=binding_definition(next,TS_AUTO_CONFIG_ACTION,action->template_id);
            if(!a||!b||memcmp(a,b,offsetof(ts_action_template_t,last_used_at)))return false;
            action=&b->action;
        }
        const char *host=NULL;
        if(action->type==TS_AUTO_ACT_SSH_CMD_REF){
            const ts_ssh_command_config_t *a=binding_definition(previous,TS_AUTO_CONFIG_COMMAND,action->ssh_ref.cmd_id);
            const ts_ssh_command_config_t *b=binding_definition(next,TS_AUTO_CONFIG_COMMAND,action->ssh_ref.cmd_id);
            if(!a||!b||memcmp(a,b,offsetof(ts_ssh_command_config_t,last_exec_time)))return false;
            host=b->host_id;
        }else if(action->type==TS_AUTO_ACT_SSH_CMD)host=action->ssh.host_ref;
        if(host){
            const ts_ssh_host_config_t *a=binding_definition(previous,TS_AUTO_CONFIG_HOST,host);
            const ts_ssh_host_config_t *b=binding_definition(next,TS_AUTO_CONFIG_HOST,host);
            if(!a||!b||memcmp(a,b,sizeof(*a)))return false;
        }
    }
    return true;
}
void ts_rule_config_publish(ts_rule_config_view_t *view, bool enabled,
    const ts_auto_document_t *previous,const ts_auto_document_t *document)
{
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    __typeof__(s_rule_ctx.meta[0]) *metadata = view->metadata;
    for (int i = 0; i < view->count; ++i) {
        ts_auto_rule_t *next = &view->rules[i];
        int old = find_rule_index(next->id);
        if (view->keep_runtime && old >= 0) {
            next->last_trigger_ms = s_rule_ctx.rules[old].last_trigger_ms;
            next->trigger_count = s_rule_ctx.rules[old].trigger_count;
            if (same_config(&s_rule_ctx.rules[old], next)&&rule_dependencies_same(next,previous,document)) {
                next->instance = s_rule_ctx.rules[old].instance;
                metadata[i] = s_rule_ctx.meta[old];
            }
        }
        if (!next->instance) next->instance = ++s_rule_ctx.next_instance;
    }
    ts_auto_rule_t *old_rules = s_rule_ctx.rules;
    for (int i = 0; i < s_rule_ctx.count; ++i) {
        rule_payload_t *old = old_rules[i].lease;
        if (old->refs) {
            old->retired = true;
            ++s_rule_ctx.retired;
        } else payload_free(old);
    }
    s_rule_ctx.rules = view->rules;
    s_rule_ctx.count = view->count;
    memcpy(s_rule_ctx.meta, metadata, sizeof(s_rule_ctx.meta));
    s_rule_ctx.loaded = true;
    s_rule_ctx.config_enabled = enabled;
    s_rule_ctx.load_error = enabled ? NULL : "automation_paused";
    xSemaphoreGive(s_rule_ctx.mutex);
    free(old_rules);
    view->rules = NULL;
    view->count = 0;
    free(view);
}

void ts_rule_config_clear(void)
{
    if(!s_rule_ctx.initialized)return;
    xSemaphoreTake(s_rule_ctx.mutex,portMAX_DELAY);
    ts_auto_rule_t *previous=s_rule_ctx.rules;
    for(int i=0;i<s_rule_ctx.count;++i){
        rule_payload_t *payload=previous[i].lease;
        if(payload->refs){payload->retired=true;++s_rule_ctx.retired;}
        else payload_free(payload);
    }
    s_rule_ctx.rules=NULL;s_rule_ctx.count=0;s_rule_ctx.loaded=false;s_rule_ctx.config_enabled=false;
    memset(s_rule_ctx.meta,0,sizeof(s_rule_ctx.meta));
    s_rule_ctx.load_error="automation_paused";
    xSemaphoreGive(s_rule_ctx.mutex);free(previous);
}

bool ts_rule_instance_current(uint32_t instance)
{
    if(!instance||!s_rule_ctx.initialized)return false;
    xSemaphoreTake(s_rule_ctx.mutex,portMAX_DELAY);
    bool current=false;
    for(int i=0;s_rule_ctx.loaded&&s_rule_ctx.config_enabled&&i<s_rule_ctx.count;++i)
        current|=s_rule_ctx.rules[i].enabled&&s_rule_ctx.rules[i].instance==instance;
    xSemaphoreGive(s_rule_ctx.mutex);return current;
}

void ts_rule_config_pause(void)
{
    if (!s_rule_ctx.initialized) return;
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    s_rule_ctx.config_enabled = false;
    s_rule_ctx.load_error = "automation_paused";
    xSemaphoreGive(s_rule_ctx.mutex);
}

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

static esp_err_t refuse(ts_rule_reject_t *why, ts_rule_reject_t reason) {
    if (why) *why = reason;
    return ESP_ERR_INVALID_STATE;
}
/* Only unfinished automatic starts retain a retry state. Successful execution
 * returns to ordinary condition/cooldown evaluation. */
static void automatic_start_result(int i,esp_err_t error,const char *reason)
{
    __typeof__(s_rule_ctx.meta[i].start) *start=&s_rule_ctx.meta[i].start;
    start->reason=reason;
    if(error==ESP_OK || (reason&&!strcmp(reason,"service_already_running"))){memset(start,0,sizeof(*start));return;}
    bool temporary=reason && (!strcmp(reason,"ssh_dns_failed")||!strcmp(reason,"ssh_port_refused")||
        !strcmp(reason,"ssh_host_unreachable")||!strcmp(reason,"ssh_connect_timeout")||
        !strcmp(reason,"ssh_connect_failed")||!strcmp(reason,"service_control_busy")||
        !strcmp(reason,"service_process_unconfirmed")||!strcmp(reason,"service_outcome_unknown"));
    int64_t now=esp_timer_get_time()/1000;
    if(now-start->first_ms>=AUTO_START_WINDOW_MS){start->state="failed";start->reason="automatic_start_timeout";}
    else if(temporary){start->state="waiting";start->next_ms=now+AUTO_START_RETRY_MS;}
    else {start->state="failed";if(!reason)start->reason="action_failed";}
}
bool ts_rule_start_status(const char *id,ts_rule_start_status_t *out)
{
    memset(out,0,sizeof(*out));
    if(!s_rule_ctx.initialized)return false;
    xSemaphoreTake(s_rule_ctx.mutex,portMAX_DELAY);int i=find_rule_index(id);
    if(i>=0){
        __typeof__(s_rule_ctx.meta[i].start) *start=&s_rule_ctx.meta[i].start;
        out->state=start->state;out->reason=start->reason;
        if(start->state&&!strcmp(start->state,"waiting")){
            int64_t remaining=start->next_ms-esp_timer_get_time()/1000;
            out->retry_after_ms=remaining>0?(uint32_t)remaining:0;
        }
    }
    xSemaphoreGive(s_rule_ctx.mutex);return i>=0&&out->state;
}
static esp_err_t execute_rule(const char *id, bool manual, bool *triggered, ts_rule_reject_t *why, const char *active_version, const char **reason) {
    if (why) *why = TS_RULE_REJECT_NONE;
    if (reason) *reason = NULL;
    if (!ts_action_manager_accepting()) return refuse(why, TS_RULE_REJECT_NOT_ACCEPTING);
    *triggered = false;
    ts_auto_rule_t r;
    esp_err_t ret = ts_rule_acquire(id, &r);
    if (ret != ESP_OK)
        return ret;
    if (!r.enabled || (!manual && r.manual_trigger) || (manual && !r.allow_manual_trigger)) {
        ts_rule_reject_t reason = !r.enabled ? TS_RULE_REJECT_DISABLED : TS_RULE_REJECT_MANUAL_NOT_ALLOWED;
        ts_rule_release(&r);
        return manual ? refuse(why, reason) : ESP_OK;
    }
    if (!manual && !ts_rule_eval_condition_group(&r.conditions)) {
        xSemaphoreTake(s_rule_ctx.mutex,portMAX_DELAY);
        int index=find_rule_index(id);
        if(index>=0&&!s_rule_ctx.meta[index].executing)memset(&s_rule_ctx.meta[index].start,0,sizeof(s_rule_ctx.meta[index].start));
        xSemaphoreGive(s_rule_ctx.mutex);
        ts_rule_release(&r);
        return ESP_OK;
    }
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    int preliminary = find_rule_index(id);
    ts_rule_reject_t busy = !s_rule_ctx.config_enabled ? TS_RULE_REJECT_PAUSED
                          : preliminary < 0 ? TS_RULE_REJECT_CHANGED
                          : !s_rule_ctx.loaded ? TS_RULE_REJECT_NOT_LOADED
                          : s_rule_ctx.meta[preliminary].executing ? TS_RULE_REJECT_RUNNING : TS_RULE_REJECT_NONE;
    if(preliminary>=0 && !s_rule_ctx.meta[preliminary].executing){
        __typeof__(s_rule_ctx.meta[preliminary].start) *start=&s_rule_ctx.meta[preliminary].start;
        if(!manual && start->state){
            int64_t time=esp_timer_get_time()/1000;
            if(!strcmp(start->state,"waiting") && time-start->first_ms>=AUTO_START_WINDOW_MS){start->state="failed";start->reason="automatic_start_timeout";}
            if(strcmp(start->state,"waiting") || time<start->next_ms)busy=TS_RULE_REJECT_RUNNING;
        }
    }
    bool unavailable = busy != TS_RULE_REJECT_NONE ||
                       (!manual && !s_rule_ctx.meta[preliminary].start.state && r.cooldown_ms && r.last_trigger_ms &&
                        esp_timer_get_time() / 1000 - r.last_trigger_ms < r.cooldown_ms);
    xSemaphoreGive(s_rule_ctx.mutex);
    if (unavailable) {
        ts_rule_release(&r);
        return manual ? refuse(why, busy) : ESP_OK;
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
    if(active_version){
        ts_auto_config_status_t status;ts_auto_config_status(&status);
        if(strcmp(active_version,status.active_version)){
            ts_ssh_binding_unlock();free(frozen);ts_rule_release(&r);
            return refuse(why,TS_RULE_REJECT_CONFIG_VERSION);
        }
    }
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
    bool background=false;
    for(unsigned n=0;n<r.action_count;++n)background|=ts_action_is_background(&frozen[n]);
    bool automatic_start=!manual&&background;
    unsigned resume=0;bool awaiting_launch=false;
    ts_action_result_t start_result={0};
    int64_t now = esp_timer_get_time() / 1000;
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    int i = find_rule_index(id);
    bool valid = i >= 0 && s_rule_ctx.config_enabled && s_rule_ctx.loaded &&
                 !s_rule_ctx.meta[i].executing &&
                 s_rule_ctx.rules[i].revision == r.revision &&
                 s_rule_ctx.rules[i].instance == r.instance;
    ts_rule_reject_t late = valid ? TS_RULE_REJECT_NONE
                          : !s_rule_ctx.config_enabled ? TS_RULE_REJECT_PAUSED
                          : i < 0 ? TS_RULE_REJECT_CHANGED
                          : !s_rule_ctx.loaded ? TS_RULE_REJECT_NOT_LOADED
                          : s_rule_ctx.meta[i].executing ? TS_RULE_REJECT_RUNNING : TS_RULE_REJECT_CHANGED;
    if (valid && !manual && !automatic_start && r.cooldown_ms && s_rule_ctx.rules[i].last_trigger_ms &&
        now - s_rule_ctx.rules[i].last_trigger_ms < r.cooldown_ms)
        valid = false;
    if (valid) {
        s_rule_ctx.meta[i].executing = true;
        if(manual){
            memset(&s_rule_ctx.meta[i].start,0,sizeof(s_rule_ctx.meta[i].start));
            if(!r.manual_trigger&&background)s_rule_ctx.meta[i].start.state="manual";
        }
        if(automatic_start){
            __typeof__(s_rule_ctx.meta[i].start) *start=&s_rule_ctx.meta[i].start;
            if(!start->state)start->first_ms=now;
            start->state="starting";resume=start->action;awaiting_launch=start->awaiting_launch;
            for(unsigned n=0;n<r.action_count;++n)frozen[n].runtime_deadline_ms=start->first_ms+AUTO_START_WINDOW_MS;
        }
    }
    xSemaphoreGive(s_rule_ctx.mutex);
    if (valid)
        for (unsigned n = 0; n < r.action_count; ++n) ts_action_snapshot_owner(&frozen[n], r.id, r.instance);
    ts_ssh_binding_unlock();
    if (!valid) {
        for (unsigned n = 0; n < r.action_count; n++)
            ts_action_snapshot_release(&frozen[n]);
        free(frozen);
        ts_rule_release(&r);
        return manual ? refuse(why, late) : ESP_OK;
    }
    int success = 0, failed = 0;
    for(unsigned n=resume;n<r.action_count;++n){
        if(!check_action_condition(&frozen[n]))continue;
        ts_action_result_t check={0};
        ret=ts_action_manager_check_start(&frozen[n],&check,!manual);
        if(ret==ESP_OK)continue;
        /* If the previous launch lost its reply, a live process is its completion,
         * not a reason to discard the remaining actions in this rule. */
        if(automatic_start&&awaiting_launch&&n==resume&&check.reason&&!strcmp(check.reason,"service_already_running")){
            ++resume;awaiting_launch=false;ret=ESP_OK;continue;
        }
        if(reason)*reason=check.reason;
        xSemaphoreTake(s_rule_ctx.mutex,portMAX_DELAY);
        i=find_rule_index(id);
        if(i>=0&&s_rule_ctx.rules[i].instance==r.instance){
            s_rule_ctx.meta[i].executing=false;
            if(automatic_start)automatic_start_result(i,ret,check.reason);
        }
        xSemaphoreGive(s_rule_ctx.mutex);
        for(unsigned k=0;k<r.action_count;++k)ts_action_snapshot_release(&frozen[k]);
        free(frozen);ts_rule_release(&r);
        return manual?refuse(why,TS_RULE_REJECT_SERVICE_STATE):ESP_OK;
    }
    ret = execute_actions_with_stats(frozen, r.action_count, &success, &failed,
        automatic_start ? &resume : NULL, automatic_start ? &start_result : NULL);
    bool launch_pending=automatic_start&&ret!=ESP_OK&&resume<r.action_count&&ts_action_is_background(&frozen[resume]);
    for (unsigned n = 0; n < r.action_count; n++)
        ts_action_snapshot_release(&frozen[n]);
    free(frozen);
    xSemaphoreTake(s_rule_ctx.mutex, portMAX_DELAY);
    i = find_rule_index(id);
    if (i >= 0 && s_rule_ctx.rules[i].instance == r.instance) {
        s_rule_ctx.meta[i].executing = false;
        bool count_trigger=!automatic_start||!s_rule_ctx.meta[i].start.counted;
        if(automatic_start){
            s_rule_ctx.meta[i].start.action=resume;
            s_rule_ctx.meta[i].start.awaiting_launch=launch_pending;
            automatic_start_result(i,launch_pending?ret:ESP_OK,launch_pending?start_result.reason:NULL);
        }
        if(count_trigger){
            s_rule_ctx.rules[i].last_trigger_ms = now;
            ++s_rule_ctx.rules[i].trigger_count;
            ++s_rule_ctx.stats.total_triggers;
            if(automatic_start&&s_rule_ctx.meta[i].start.state)s_rule_ctx.meta[i].start.counted=true;
        }
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
    return execute_rule(id, false, triggered, NULL, NULL, NULL);
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

esp_err_t ts_rule_trigger_ex(const char *id, ts_rule_reject_t *why) {
    bool triggered;
    return execute_rule(id, true, &triggered, why, NULL, NULL);
}

esp_err_t ts_rule_trigger_versioned(const char *id,const char *active_version,ts_rule_reject_t *why,const char **reason) {
    bool triggered;
    return execute_rule(id,true,&triggered,why,active_version,reason);
}

esp_err_t ts_rule_trigger(const char *id) {
    return ts_rule_trigger_ex(id, NULL);
}

/*===========================================================================*/
/*                          Action 执行器实现                                 */
/*===========================================================================*/

esp_err_t ts_action_execute(const ts_auto_action_t *action)
{
    if(!action)return ESP_ERR_INVALID_ARG;
    if(action->runtime_snapshot)return ts_action_submit_prepared(action);
    ts_action_result_t result={0};
    esp_err_t ret=action->template_id[0]?ts_action_template_execute(action->template_id,&result):
        action->async?ts_action_queue(action,NULL,NULL,5):ts_action_manager_execute(action,&result);
    s_rule_ctx.stats.total_actions++;
    if(ret!=ESP_OK)s_rule_ctx.stats.failed_actions++;
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
static bool rule_wait(const ts_auto_action_t *action,uint32_t ms) {
    while (ms && ts_action_manager_accepting() && ts_action_snapshot_current(action)) {
        if(action->runtime_deadline_ms && esp_timer_get_time()/1000>=action->runtime_deadline_ms)return false;
        uint32_t slice = ms > 100 ? 100 : ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
    return ts_action_manager_accepting() && ts_action_snapshot_current(action);
}
static esp_err_t execute_action_with_repeat(const ts_auto_action_t *action,
                                             ts_action_result_cb_t callback,
                                             void *user_data, ts_action_result_t *start_result)
{
    esp_err_t ret = ESP_OK;
    if (!ts_action_manager_accepting() || !ts_action_snapshot_current(action)) return ESP_ERR_INVALID_STATE;

    // 先检查动作级别的条件
    if (!check_action_condition(action)) {
        ESP_LOGI(TAG, "Action skipped: condition not met");
        return ESP_OK;  // 条件不满足，跳过执行
    }

    switch (action->repeat_mode) {
        case TS_AUTO_REPEAT_ONCE:
        default:
            // 单次执行
            ret = start_result && ts_action_is_background(action)
                ? ts_action_submit_background(action,start_result) : ts_action_execute(action);
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
                if (!ts_action_manager_accepting() || !ts_action_snapshot_current(action)) return ESP_ERR_INVALID_STATE;
                // 每次重复前检查条件
                if (!check_action_condition(action)) {
                    ESP_LOGI(TAG, "Repeat stopped: condition no longer met");
                    break;
                }

                ret = start_result && ts_action_is_background(action)
                ? ts_action_submit_background(action,start_result) : ts_action_execute(action);
                if (callback) {
                    callback(action, ret, user_data);
                }

                if(ret!=ESP_OK && start_result && ts_action_is_background(action))break;
                // 最后一次不需要等待
                if (i < count - 1 && interval > 0) {
                    if (!rule_wait(action,interval)) return ESP_ERR_INVALID_STATE;
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

            while (ts_action_manager_accepting() && ts_action_snapshot_current(action) && check_action_condition(action) && iterations < max_iterations) {
                ret = start_result && ts_action_is_background(action)
                ? ts_action_submit_background(action,start_result) : ts_action_execute(action);
                if (callback) {
                    callback(action, ret, user_data);
                }
                iterations++;
                if(ret!=ESP_OK && start_result && ts_action_is_background(action))break;

                if (!rule_wait(action,interval)) return ESP_ERR_INVALID_STATE;
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
                                             int *success_count, int *fail_count,
                                             unsigned *resume, ts_action_result_t *start_result)
{
    if (!actions || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int success = 0, fail = 0;
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Executing %d actions sequentially", count);

    for (int i = resume ? *resume : 0; i < count; i++) {
        if (!ts_action_manager_accepting() || !ts_action_snapshot_current(&actions[i])) { fail += count - i; ret = ESP_ERR_INVALID_STATE; break; }
        ESP_LOGI(TAG, "Action [%d/%d]: type=%d, template=%s, delay=%dms",
                 i + 1, count, actions[i].type,
                 actions[i].template_id[0] ? actions[i].template_id : "(inline)",
                 actions[i].delay_ms);

        // 动作前延迟
        if (actions[i].delay_ms > 0) {
            ESP_LOGI(TAG, "  Waiting %dms before action", actions[i].delay_ms);
            if (!rule_wait(&actions[i],actions[i].delay_ms)) { fail += count - i; ret = ESP_ERR_INVALID_STATE; break; }
        }

        esp_err_t action_ret = execute_action_with_repeat(&actions[i], NULL, NULL, start_result);
        if(resume && start_result->reason && !strcmp(start_result->reason,"service_already_running")){
            action_ret=ESP_OK;start_result->reason=NULL;
        }
        if (action_ret == ESP_OK) {
            success++;
        } else {
            fail++;
            ret = action_ret;  // 记录最后一个错误
            if(resume&&ts_action_is_background(&actions[i])){*resume=i;break;}
        }
        if(resume)*resume=i+1;

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
                    rule_wait(&actions[i],delay_after);
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
    return execute_actions_with_stats(actions, count, NULL, NULL, NULL, NULL);
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
