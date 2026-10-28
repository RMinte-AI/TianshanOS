/**
 * @file ts_variable.c
 * @brief TianShanOS 自动化引擎 - 变量存储实现
 *
 * 提供层级化的变量存储系统：
 * - 支持 bool/int/float/string 类型
 * - 变量变化通过事件总线通知
 * - 用户定义与初始值由统一配置保存，观测值只在运行时更新
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_variable.h"
#include "ts_automation_runtime.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "ts_variable";

/*===========================================================================*/
/*                              配置常量                                      */
/*===========================================================================*/

#ifndef CONFIG_TS_AUTOMATION_MAX_VARIABLES
#define CONFIG_TS_AUTOMATION_MAX_VARIABLES  128
#endif

/*===========================================================================*/
/*                              内部状态                                      */
/*===========================================================================*/

typedef struct {
    ts_auto_variable_t *variables;       // 变量数组
    int count;                           // 当前变量数量
    int capacity;                        // 最大容量
    SemaphoreHandle_t mutex;             // 访问互斥锁
    bool initialized;                    // 初始化标志
} ts_variable_ctx_t;

static ts_variable_ctx_t s_var_ctx = {
    .variables = NULL,
    .count = 0,
    .capacity = 0,
    .mutex = NULL,
    .initialized = false,
};

static StaticSemaphore_t registration_storage;
static SemaphoreHandle_t registration;
static uint64_t next_definition_instance;

struct ts_variable_config_view {
    ts_auto_variable_t *variables;
    int count;
};

void ts_variable_registration_begin(void) { xSemaphoreTakeRecursive(registration, portMAX_DELAY); }
void ts_variable_registration_end(void) { xSemaphoreGiveRecursive(registration); }

/*===========================================================================*/
/*                              辅助函数                                      */
/*===========================================================================*/

/**
 * 查找变量索引
 * @return 找到返回索引，否则返回 -1
 */
static int find_variable_index(const char *name)
{
    for (int i = 0; i < s_var_ctx.count; i++) {
        if (strcmp(s_var_ctx.variables[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * 比较两个值是否相等
 */
static bool value_equal(const ts_auto_value_t *a, const ts_auto_value_t *b)
{
    if (a->type != b->type) {
        return false;
    }

    switch (a->type) {
        case TS_AUTO_VAL_NULL:
            return true;
        case TS_AUTO_VAL_BOOL:
            return a->bool_val == b->bool_val;
        case TS_AUTO_VAL_INT:
            return a->int_val == b->int_val;
        case TS_AUTO_VAL_FLOAT:
            // 浮点比较使用小容差
            return (a->float_val - b->float_val) < 0.0001 && 
                   (a->float_val - b->float_val) > -0.0001;
        case TS_AUTO_VAL_STRING:
            return strcmp(a->str_val, b->str_val) == 0;
        default:
            return false;
    }
}

/**
 * 发送变量变化事件
 */
static void notify_change(const char *name, 
                          const ts_auto_value_t *old_val,
                          const ts_auto_value_t *new_val)
{
    (void)old_val;  // 暂未使用，预留接口
    (void)new_val;  // 暂未使用，预留接口

    ESP_LOGD(TAG, "Variable '%s' changed", name);
}

/*===========================================================================*/
/*                              初始化                                        */
/*===========================================================================*/

esp_err_t ts_variable_init(void)
{
    if (s_var_ctx.initialized) {
        ESP_LOGD(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Initializing variable storage (max %d)", 
             CONFIG_TS_AUTOMATION_MAX_VARIABLES);

    // 分配变量数组（使用 PSRAM）
    s_var_ctx.capacity = CONFIG_TS_AUTOMATION_MAX_VARIABLES;
    size_t alloc_size = s_var_ctx.capacity * sizeof(ts_auto_variable_t);
    
    s_var_ctx.variables = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
    if (!s_var_ctx.variables) {
        // Fallback 到 DRAM
        s_var_ctx.variables = malloc(alloc_size);
        if (!s_var_ctx.variables) {
            ESP_LOGE(TAG, "Failed to allocate variable storage");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGD(TAG, "Using DRAM for variable storage");
    }

    memset(s_var_ctx.variables, 0, alloc_size);

    // 创建互斥锁
    s_var_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_var_ctx.mutex) {
        free(s_var_ctx.variables);
        s_var_ctx.variables = NULL;
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_var_ctx.count = 0;
    if (!registration) registration = xSemaphoreCreateRecursiveMutexStatic(&registration_storage);
    s_var_ctx.initialized = true;

    ESP_LOGD(TAG, "Variable storage initialized");
    return ESP_OK;
}

esp_err_t ts_variable_deinit(void)
{
    if (!s_var_ctx.initialized) {
        return ESP_OK;
    }

    /* 清理所有变量和回调 */
    if (s_var_ctx.mutex) {
        xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);
    }

    if (s_var_ctx.variables) {
        free(s_var_ctx.variables);
        s_var_ctx.variables = NULL;
    }

    s_var_ctx.count = 0;
    s_var_ctx.initialized = false;

    if (s_var_ctx.mutex) {
        xSemaphoreGive(s_var_ctx.mutex);
        vSemaphoreDelete(s_var_ctx.mutex);
        s_var_ctx.mutex = NULL;
    }

    ESP_LOGD(TAG, "Variable storage deinitialized");
    return ESP_OK;
}

bool ts_variable_is_initialized(void)
{
    return s_var_ctx.initialized;
}

/*===========================================================================*/
/*                              变量注册                                      */
/*===========================================================================*/

static bool same_definition(const ts_auto_variable_t *a,const ts_auto_variable_t *b);

static esp_err_t register_variable(const ts_auto_variable_t *var)
{
    if (!var || !var->name[0]) {
        ESP_LOGE(TAG, "ts_variable_register: invalid arg (var=%p, name=%s)", 
                 var, var ? var->name : "NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_var_ctx.initialized) {
        ESP_LOGE(TAG, "ts_variable_register: not initialized!");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);
    int64_t now_ms = esp_timer_get_time() / 1000;

    // 检查是否已存在
    int idx = find_variable_index(var->name);
    if (idx >= 0) {
        if (s_var_ctx.variables[idx].definition_instance) {
            xSemaphoreGive(s_var_ctx.mutex);
            return ESP_ERR_INVALID_STATE;
        }
        // 更新现有变量
        int64_t last_change_ms = s_var_ctx.variables[idx].last_change_ms;
        int64_t last_update_ms = s_var_ctx.variables[idx].last_update_ms;
        if (!value_equal(&s_var_ctx.variables[idx].value, &var->value)) {
            last_change_ms = now_ms;
            last_update_ms = 0;
        }
        uint64_t identity=same_definition(&s_var_ctx.variables[idx],var)?s_var_ctx.variables[idx].runtime_instance:++next_definition_instance;
        memcpy(&s_var_ctx.variables[idx], var, sizeof(ts_auto_variable_t));
        s_var_ctx.variables[idx].runtime_instance=identity;
        s_var_ctx.variables[idx].last_change_ms = last_change_ms;
        s_var_ctx.variables[idx].last_update_ms = last_update_ms;
        xSemaphoreGive(s_var_ctx.mutex);
        ESP_LOGD(TAG, "Updated variable: %s (type=%d)", var->name, var->value.type);
        return ESP_OK;
    }

    // 检查容量
    if (s_var_ctx.count >= s_var_ctx.capacity) {
        xSemaphoreGive(s_var_ctx.mutex);
        ESP_LOGE(TAG, "Variable storage full (count=%d, capacity=%d)", 
                 s_var_ctx.count, s_var_ctx.capacity);
        return ESP_ERR_NO_MEM;
    }

    // 添加新变量
    memcpy(&s_var_ctx.variables[s_var_ctx.count], var, sizeof(ts_auto_variable_t));
    s_var_ctx.variables[s_var_ctx.count].runtime_instance=++next_definition_instance;
    s_var_ctx.variables[s_var_ctx.count].last_change_ms = now_ms;
    s_var_ctx.variables[s_var_ctx.count].last_update_ms = 0;
    s_var_ctx.count++;

    xSemaphoreGive(s_var_ctx.mutex);

    ESP_LOGD(TAG, "Registered variable: %s (type=%d, total: %d)", 
             var->name, var->value.type, s_var_ctx.count);
    return ESP_OK;
}

static esp_err_t upsert_variable(const ts_auto_variable_t *var)
{
    if (!var || !var->name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_var_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ts_auto_value_t old_value = {0};
    bool changed = false;
    bool existed = false;
    int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);

    int idx = find_variable_index(var->name);
    if (idx >= 0) {
        if (s_var_ctx.variables[idx].definition_instance &&
            (!var->source_id[0] || strcmp(var->source_id, s_var_ctx.variables[idx].source_id))) {
            xSemaphoreGive(s_var_ctx.mutex);
            return ESP_ERR_INVALID_STATE;
        }
        existed = true;
        old_value = s_var_ctx.variables[idx].value;
        changed = !value_equal(&old_value, &var->value);

        int64_t last_change_ms = changed
                                 ? now_ms
                                 : s_var_ctx.variables[idx].last_change_ms;
        uint64_t identity=s_var_ctx.variables[idx].runtime_instance;
        if (s_var_ctx.variables[idx].definition_instance)
            s_var_ctx.variables[idx].value = var->value;
        else memcpy(&s_var_ctx.variables[idx], var, sizeof(ts_auto_variable_t));
        s_var_ctx.variables[idx].runtime_instance=identity;
        s_var_ctx.variables[idx].last_change_ms = last_change_ms;
        s_var_ctx.variables[idx].last_update_ms = now_ms;
    } else {
        if (s_var_ctx.count >= s_var_ctx.capacity) {
            xSemaphoreGive(s_var_ctx.mutex);
            return ESP_ERR_NO_MEM;
        }

        memcpy(&s_var_ctx.variables[s_var_ctx.count], var, sizeof(ts_auto_variable_t));
        s_var_ctx.variables[s_var_ctx.count].runtime_instance=++next_definition_instance;
        s_var_ctx.variables[s_var_ctx.count].last_change_ms = now_ms;
        s_var_ctx.variables[s_var_ctx.count].last_update_ms = now_ms;
        s_var_ctx.count++;
    }

    xSemaphoreGive(s_var_ctx.mutex);

    if (existed && changed) {
        notify_change(var->name, &old_value, &var->value);
    }

    return ESP_OK;
}

static esp_err_t unregister_variable(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_var_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);

    int idx = find_variable_index(name);
    if (idx < 0) {
        xSemaphoreGive(s_var_ctx.mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_var_ctx.variables[idx].definition_instance) {
        xSemaphoreGive(s_var_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    // 移动后续元素
    if (idx < s_var_ctx.count - 1) {
        memmove(&s_var_ctx.variables[idx], 
                &s_var_ctx.variables[idx + 1],
                (s_var_ctx.count - idx - 1) * sizeof(ts_auto_variable_t));
    }
    s_var_ctx.count--;

    xSemaphoreGive(s_var_ctx.mutex);

    ESP_LOGD(TAG, "Unregistered variable: %s", name);
    return ESP_OK;
}

static int unregister_source(const char *source_id)
{
    if (!source_id || !s_var_ctx.initialized) {
        return 0;
    }

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);

    int removed = 0;
    // 从后向前遍历，避免删除时索引混乱
    for (int i = s_var_ctx.count - 1; i >= 0; i--) {
        if (!s_var_ctx.variables[i].definition_instance &&
            strcmp(s_var_ctx.variables[i].source_id, source_id) == 0) {
            // 移动后续元素
            if (i < s_var_ctx.count - 1) {
                memmove(&s_var_ctx.variables[i],
                        &s_var_ctx.variables[i + 1],
                        (s_var_ctx.count - i - 1) * sizeof(ts_auto_variable_t));
            }
            s_var_ctx.count--;
            removed++;
        }
    }

    xSemaphoreGive(s_var_ctx.mutex);

    if (removed > 0) {
        ESP_LOGD(TAG, "Removed %d variables for source: %s", removed, source_id);
    }
    return removed;
}

bool ts_variable_exists(const char *name)
{
    if (!name || !s_var_ctx.initialized) {
        return false;
    }

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);
    bool exists = (find_variable_index(name) >= 0);
    xSemaphoreGive(s_var_ctx.mutex);

    return exists;
}

/*===========================================================================*/
/*                              值访问                                        */
/*===========================================================================*/

esp_err_t ts_variable_get(const char *name, ts_auto_value_t *value)
{
    if (!name || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_var_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);

    int idx = find_variable_index(name);
    if (idx < 0) {
        xSemaphoreGive(s_var_ctx.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *value = s_var_ctx.variables[idx].value;

    xSemaphoreGive(s_var_ctx.mutex);
    return ESP_OK;
}

esp_err_t ts_variable_get_bool(const char *name, bool *value)
{
    ts_auto_value_t val;
    esp_err_t ret = ts_variable_get(name, &val);
    if (ret != ESP_OK) {
        return ret;
    }

    switch (val.type) {
        case TS_AUTO_VAL_BOOL:
            *value = val.bool_val;
            break;
        case TS_AUTO_VAL_INT:
            *value = (val.int_val != 0);
            break;
        case TS_AUTO_VAL_FLOAT:
            *value = (val.float_val != 0.0);
            break;
        default:
            return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t ts_variable_get_int(const char *name, int32_t *value)
{
    ts_auto_value_t val;
    esp_err_t ret = ts_variable_get(name, &val);
    if (ret != ESP_OK) {
        return ret;
    }

    switch (val.type) {
        case TS_AUTO_VAL_INT:
            *value = val.int_val;
            break;
        case TS_AUTO_VAL_FLOAT:
            *value = (int32_t)val.float_val;
            break;
        case TS_AUTO_VAL_BOOL:
            *value = val.bool_val ? 1 : 0;
            break;
        default:
            return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t ts_variable_get_float(const char *name, double *value)
{
    ts_auto_value_t val;
    esp_err_t ret = ts_variable_get(name, &val);
    if (ret != ESP_OK) {
        return ret;
    }

    switch (val.type) {
        case TS_AUTO_VAL_FLOAT:
            *value = val.float_val;
            break;
        case TS_AUTO_VAL_INT:
            *value = (double)val.int_val;
            break;
        default:
            return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t ts_variable_get_string(const char *name, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ts_auto_value_t val;
    esp_err_t ret = ts_variable_get(name, &val);
    if (ret != ESP_OK) {
        return ret;
    }

    switch (val.type) {
        case TS_AUTO_VAL_STRING:
            strncpy(buffer, val.str_val, buffer_size - 1);
            buffer[buffer_size - 1] = '\0';
            break;
        case TS_AUTO_VAL_INT:
            snprintf(buffer, buffer_size, "%"PRId32, val.int_val);
            break;
        case TS_AUTO_VAL_FLOAT:
            snprintf(buffer, buffer_size, "%.2f", val.float_val);
            break;
        case TS_AUTO_VAL_BOOL:
            strncpy(buffer, val.bool_val ? "true" : "false", buffer_size - 1);
            break;
        default:
            buffer[0] = '\0';
            break;
    }

    return ESP_OK;
}

static esp_err_t variable_get_info_impl(const char *name, ts_variable_info_t *info,
                                        TickType_t ticks_to_wait)
{
    if (!name || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_var_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_var_ctx.mutex, ticks_to_wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int idx = find_variable_index(name);
    if (idx < 0) {
        xSemaphoreGive(s_var_ctx.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memset(info, 0, sizeof(ts_variable_info_t));
    strncpy(info->name, s_var_ctx.variables[idx].name, sizeof(info->name) - 1);
    strncpy(info->source_id, s_var_ctx.variables[idx].source_id, sizeof(info->source_id) - 1);
    info->value = s_var_ctx.variables[idx].value;
    info->flags = s_var_ctx.variables[idx].flags;
    info->last_change_ms = s_var_ctx.variables[idx].last_change_ms;
    info->last_update_ms = s_var_ctx.variables[idx].last_update_ms;

    xSemaphoreGive(s_var_ctx.mutex);
    return ESP_OK;
}

esp_err_t ts_variable_get_info(const char *name, ts_variable_info_t *info)
{
    return variable_get_info_impl(name, info, portMAX_DELAY);
}

esp_err_t ts_variable_get_info_try(const char *name, ts_variable_info_t *info)
{
    return variable_get_info_impl(name, info, 0);
}

/*===========================================================================*/
/*                              值修改                                        */
/*===========================================================================*/

/**
 * @brief 内部设置函数，可选择绕过只读检查
 */
static esp_err_t variable_set_impl(const char *name, const ts_auto_value_t *value, bool check_readonly)
{
    if (!name || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_var_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);

    int idx = find_variable_index(name);
    if (idx < 0) {
        xSemaphoreGive(s_var_ctx.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    ts_auto_variable_t *var = &s_var_ctx.variables[idx];

    // 检查只读标志（仅当 check_readonly 为 true 时）
    if (check_readonly && (var->flags & TS_AUTO_VAR_READONLY)) {
        xSemaphoreGive(s_var_ctx.mutex);
        ESP_LOGD(TAG, "Variable '%s' is read-only", name);
        return ESP_ERR_NOT_ALLOWED;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    var->last_update_ms = now_ms;

    // 检查值是否变化
    if (!value_equal(&var->value, value)) {
        ts_auto_value_t old_value = var->value;
        var->value = *value;
        var->last_change_ms = now_ms;

        xSemaphoreGive(s_var_ctx.mutex);

        // 发送变化通知
        notify_change(name, &old_value, value);
    } else {
        xSemaphoreGive(s_var_ctx.mutex);
    }

    return ESP_OK;
}

esp_err_t ts_variable_set(const char *name, const ts_auto_value_t *value)
{
    return variable_set_impl(name, value, true);  // 检查只读
}

esp_err_t ts_variable_set_internal(const char *name, const ts_auto_value_t *value)
{
    return variable_set_impl(name, value, false);  // 不检查只读
}

esp_err_t ts_variable_set_bool(const char *name, bool value)
{
    ts_auto_value_t val = {
        .type = TS_AUTO_VAL_BOOL,
        .bool_val = value,
    };
    return ts_variable_set(name, &val);
}

esp_err_t ts_variable_set_int(const char *name, int32_t value)
{
    ts_auto_value_t val = {
        .type = TS_AUTO_VAL_INT,
        .int_val = value,
    };
    return ts_variable_set(name, &val);
}

esp_err_t ts_variable_set_float(const char *name, double value)
{
    ts_auto_value_t val = {
        .type = TS_AUTO_VAL_FLOAT,
        .float_val = value,
    };
    return ts_variable_set(name, &val);
}

esp_err_t ts_variable_set_string(const char *name, const char *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    ts_auto_value_t val = {
        .type = TS_AUTO_VAL_STRING,
    };
    strncpy(val.str_val, value, sizeof(val.str_val) - 1);
    val.str_val[sizeof(val.str_val) - 1] = '\0';

    return ts_variable_set(name, &val);
}

/*===========================================================================*/
/*                              枚举与计数                                    */
/*===========================================================================*/

int ts_variable_enumerate(const char *prefix, ts_variable_enum_cb_t callback, void *user_data)
{
    if (!callback || !s_var_ctx.initialized) {
        return 0;
    }

    int count = 0;
    size_t prefix_len = prefix ? strlen(prefix) : 0;

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);

    for (int i = 0; i < s_var_ctx.count; i++) {
        // 前缀过滤
        if (prefix_len > 0) {
            if (strncmp(s_var_ctx.variables[i].name, prefix, prefix_len) != 0) {
                continue;
            }
        }

        count++;
        
        // 回调
        if (!callback(&s_var_ctx.variables[i], user_data)) {
            break;  // 回调返回 false，停止枚举
        }
    }

    xSemaphoreGive(s_var_ctx.mutex);
    return count;
}

int ts_variable_count(void)
{
    if (!s_var_ctx.initialized) {
        return 0;
    }

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);
    int count = s_var_ctx.count;
    xSemaphoreGive(s_var_ctx.mutex);

    return count;
}

esp_err_t ts_variable_iterate(ts_variable_iterate_ctx_t *ctx, ts_auto_variable_t *var)
{
    if (!ctx || !var) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_var_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);

    if (ctx->index >= s_var_ctx.count) {
        xSemaphoreGive(s_var_ctx.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    // 复制变量到输出
    memcpy(var, &s_var_ctx.variables[ctx->index], sizeof(ts_auto_variable_t));

    // 字符串需要深拷贝吗？这里只是浅拷贝，调用者需要注意
    // 对于安全性，可以在持有锁期间使用

    ctx->index++;
    xSemaphoreGive(s_var_ctx.mutex);

    return ESP_OK;
}


/* Configuration publication owns definitions; ordinary producers own runtime
 * observations. Registration is serialized with the prepared replacement so a
 * save cannot consume capacity that a source discovers during file I/O. */
esp_err_t ts_variable_register(const ts_auto_variable_t *value)
{
    if (!registration || !s_var_ctx.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(registration, portMAX_DELAY);
    esp_err_t ret = value && value->definition_instance ? ESP_ERR_INVALID_ARG : register_variable(value);
    xSemaphoreGiveRecursive(registration);
    return ret;
}
esp_err_t ts_variable_upsert(const ts_auto_variable_t *value)
{
    if (!registration || !s_var_ctx.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(registration, portMAX_DELAY);
    esp_err_t ret = value && value->definition_instance ? ESP_ERR_INVALID_ARG : upsert_variable(value);
    xSemaphoreGiveRecursive(registration);
    return ret;
}
esp_err_t ts_variable_unregister(const char *name)
{
    if (!registration || !s_var_ctx.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(registration, portMAX_DELAY);
    esp_err_t ret = unregister_variable(name);
    xSemaphoreGiveRecursive(registration);
    return ret;
}
int ts_variable_unregister_by_source(const char *source)
{
    if (!registration || !s_var_ctx.initialized) return 0;
    xSemaphoreTakeRecursive(registration, portMAX_DELAY);
    int count = unregister_source(source);
    xSemaphoreGiveRecursive(registration);
    return count;
}

static bool same_definition(const ts_auto_variable_t *a, const ts_auto_variable_t *b)
{
    bool same_initial = a->default_value.type == TS_AUTO_VAL_FLOAT ?
        b->default_value.type == TS_AUTO_VAL_FLOAT && a->default_value.float_val == b->default_value.float_val :
        value_equal(&a->default_value, &b->default_value);
    return same_initial && a->flags == b->flags && !strcmp(a->source_id, b->source_id) &&
           !strcmp(a->expression, b->expression);
}

static esp_err_t stage_variable(ts_variable_config_view_t *view, const ts_auto_variable_t *value, bool shared, bool keep_runtime)
{
    for (int i = 0; i < view->count; ++i) {
        ts_auto_variable_t *existing = &view->variables[i];
        if (strcmp(existing->name, value->name)) continue;
        return shared && existing->definition_instance && same_definition(existing, value) ?
            ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (view->count == s_var_ctx.capacity) return ESP_ERR_INVALID_SIZE;
    ts_auto_variable_t *next = &view->variables[view->count++];
    *next = *value;
    next->value = next->default_value;
    next->definition_instance = ++next_definition_instance;
    next->runtime_instance=next->definition_instance;
    next->last_update_ms = 0;
    next->last_change_ms = esp_timer_get_time() / 1000;
    int old = find_variable_index(value->name);
    if (keep_runtime && old >= 0 && s_var_ctx.variables[old].definition_instance &&
        same_definition(value, &s_var_ctx.variables[old])) {
        *next = s_var_ctx.variables[old];
    }
    return ESP_OK;
}

static const ts_ssh_host_config_t *definition_host(const ts_auto_document_t *document,const char *id)
{
    for(int i=0;document&&i<document->host_count;++i)if(!strcmp(document->hosts[i].id,id))return &document->hosts[i];
    return NULL;
}
static bool command_group_same(const ts_auto_document_t *next,const ts_auto_document_t *previous,const char *prefix)
{
    if(!previous)return false;
    int before=0,after=0;
    for(int i=0;i<previous->command_count;++i)before+=!strcmp(previous->commands[i].var_name,prefix);
    for(int i=0;i<next->command_count;++i){
        const ts_ssh_command_config_t *command=&next->commands[i];
        if(strcmp(command->var_name,prefix))continue;
        ++after;bool matched=false;
        for(int j=0;j<previous->command_count;++j){
            const ts_ssh_command_config_t *old=&previous->commands[j];
            if(memcmp(command,old,offsetof(ts_ssh_command_config_t,last_exec_time)))continue;
            const ts_ssh_host_config_t *a=definition_host(next,command->host_id),*b=definition_host(previous,old->host_id);
            matched=a&&b&&!memcmp(a,b,sizeof(*a));break;
        }
        if(!matched)return false;
    }
    return before==after;
}

esp_err_t ts_variable_config_prepare(const ts_auto_document_t *document, const ts_source_config_view_t *sources, const ts_auto_document_t *previous,
                                     ts_variable_config_view_t **out)
{
    if (!document || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!s_var_ctx.initialized) return ESP_ERR_INVALID_STATE;
    ts_variable_config_view_t *view = heap_caps_calloc(1, sizeof(*view), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view) return ESP_ERR_NO_MEM;
    view->variables = heap_caps_calloc(s_var_ctx.capacity, sizeof(*view->variables), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view->variables) { free(view); return ESP_ERR_NO_MEM; }
    xSemaphoreTakeRecursive(registration, portMAX_DELAY);
    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);
    for (int i = 0; i < s_var_ctx.count; ++i) {
        if (!s_var_ctx.variables[i].definition_instance &&
            ts_source_config_keeps_variables(sources, s_var_ctx.variables[i].source_id))
            view->variables[view->count++] = s_var_ctx.variables[i];
    }
    esp_err_t ret = ESP_OK;
    for (int i = 0; ret == ESP_OK && i < document->variable_count; ++i)
        ret = stage_variable(view, &document->variables[i], false, previous!=NULL);
    const struct { const char *suffix; ts_auto_value_type_t type; } fields[] = {
        {"status", TS_AUTO_VAL_STRING}, {"exit_code", TS_AUTO_VAL_INT}, {"extracted", TS_AUTO_VAL_STRING},
        {"expect_matched", TS_AUTO_VAL_BOOL}, {"fail_matched", TS_AUTO_VAL_BOOL},
        {"host", TS_AUTO_VAL_STRING}, {"timestamp", TS_AUTO_VAL_INT}
    };
    for (int i = 0; ret == ESP_OK && i < document->command_count; ++i) {
        const char *prefix = document->commands[i].var_name;
        if (!prefix[0]) continue;
        bool shared=false;
        for(int n=0;n<i;++n)if(!strcmp(document->commands[n].var_name,prefix)){shared=true;break;}
        if(shared)continue;
        bool keep_values=command_group_same(document,previous,prefix);
        ts_auto_variable_t value = {0};
        strcpy(value.source_id, prefix);
        for (unsigned j = 0; ret == ESP_OK && j < sizeof(fields) / sizeof(fields[0]); ++j) {
            snprintf(value.name, sizeof(value.name), "%s.%s", prefix, fields[j].suffix);
            value.default_value.type = fields[j].type;
            bool user_defined = false;
            for (int n = 0; n < document->variable_count; ++n)
                user_defined |= !strcmp(document->variables[n].name, value.name);
            ret = user_defined ? ESP_ERR_INVALID_ARG : stage_variable(view, &value, true, keep_values);
        }
    }
    xSemaphoreGive(s_var_ctx.mutex);
    if (ret != ESP_OK) { ts_variable_config_discard(view); return ret; }
    *out = view;
    return ESP_OK;
}

void ts_variable_config_discard(ts_variable_config_view_t *view)
{
    if (!view) return;
    free(view->variables); free(view);
    xSemaphoreGiveRecursive(registration);
}

void ts_variable_config_publish(ts_variable_config_view_t *view)
{
    xSemaphoreTake(s_var_ctx.mutex, portMAX_DELAY);
    for (int i = 0; i < view->count; ++i) {
        ts_auto_variable_t *next = &view->variables[i];
        int old = find_variable_index(next->name);
        if (old < 0 || next->definition_instance != s_var_ctx.variables[old].definition_instance) continue;
        next->value = s_var_ctx.variables[old].value;
        next->last_change_ms = s_var_ctx.variables[old].last_change_ms;
        next->last_update_ms = s_var_ctx.variables[old].last_update_ms;
    }
    ts_auto_variable_t *previous = s_var_ctx.variables;
    s_var_ctx.variables = view->variables;
    s_var_ctx.count = view->count;
    xSemaphoreGive(s_var_ctx.mutex);
    free(previous); free(view);
    xSemaphoreGiveRecursive(registration);
}

void ts_variable_config_clear(const ts_auto_document_t *previous)
{
    if(!s_var_ctx.initialized)return;
    ts_variable_registration_begin();xSemaphoreTake(s_var_ctx.mutex,portMAX_DELAY);
    int retained=0;
    for(int i=0;i<s_var_ctx.count;++i){
        ts_auto_variable_t *value=&s_var_ctx.variables[i];
        bool removed=value->definition_instance!=0;
        for(int n=0;!removed&&previous&&n<previous->source_count;++n)
            removed=!strcmp(value->source_id,previous->sources[n].id);
        if(!removed)s_var_ctx.variables[retained++]=*value;
    }
    memset(s_var_ctx.variables+retained,0,(s_var_ctx.count-retained)*sizeof(*s_var_ctx.variables));
    s_var_ctx.count=retained;xSemaphoreGive(s_var_ctx.mutex);ts_variable_registration_end();
}

uint64_t ts_variable_instance(const char *name)
{
    if(!name||!s_var_ctx.initialized)return 0;
    xSemaphoreTake(s_var_ctx.mutex,portMAX_DELAY);
    int index=find_variable_index(name);uint64_t instance=index<0?0:s_var_ctx.variables[index].runtime_instance;
    xSemaphoreGive(s_var_ctx.mutex);return instance;
}
