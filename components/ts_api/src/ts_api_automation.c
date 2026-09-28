#include "ts_rule_codec.h"
#include "ts_ssh_service.h"
#include <math.h>
/**
 * @file ts_api_automation.c
 * @brief TianShanOS Automation Engine API
 *
 * Provides REST API endpoints for automation engine:
 * - automation.status - Get engine status
 * - automation.start/stop/pause/resume - Control engine
 * - automation.variables.list/get/set - Variable management
 * - automation.rules.list/enable/disable/trigger - Rule management
 * - automation.sources.list - Data source listing
 * - automation.sources.export/import - Source config pack export/import
 * - automation.rules.export/import - Rule config pack export/import
 * - automation.actions.export/import - Action template config pack export/import
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_automation.h"
#include "ts_variable.h"
#include "ts_rule_engine.h"
#include "ts_source_manager.h"
#include "ts_temp_source.h"
#include "ts_action_manager.h"
#include "ts_config_pack.h"
#include "ts_cert.h"
#include "ts_log.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static const char *TAG = "api_automation";

/* SD 卡配置目录 */
#define SOURCES_SDCARD_DIR  "/sdcard/config/sources"
#define RULES_SDCARD_DIR    "/sdcard/config/rules"
#define ACTIONS_SDCARD_DIR  "/sdcard/config/actions"

/*===========================================================================*/
/*                           Helper Functions                                 */
/*===========================================================================*/

static const char *state_to_string(ts_automation_state_t state)
{
    switch (state) {
        case TS_AUTO_STATE_UNINITIALIZED: return "uninitialized";
        case TS_AUTO_STATE_INITIALIZED: return "initialized";
        case TS_AUTO_STATE_RUNNING: return "running";
        case TS_AUTO_STATE_PAUSED: return "paused";
        case TS_AUTO_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

static cJSON *value_to_json(const ts_auto_value_t *val)
{
    switch (val->type) {
        case TS_AUTO_VAL_NULL:
            return cJSON_CreateNull();
        case TS_AUTO_VAL_BOOL:
            return cJSON_CreateBool(val->bool_val);
        case TS_AUTO_VAL_INT:
            return cJSON_CreateNumber(val->int_val);
        case TS_AUTO_VAL_FLOAT:
            return cJSON_CreateNumber(val->float_val);
        case TS_AUTO_VAL_STRING:
            return cJSON_CreateString(val->str_val);
        default:
            return cJSON_CreateNull();
    }
}

/*===========================================================================*/
/*                           Status API                                       */
/*===========================================================================*/

/**
 * @brief automation.status - Get automation engine status
 */
static esp_err_t api_automation_status(const cJSON *params, ts_api_result_t *result)
{
    ts_automation_status_t status = {0};
    esp_err_t ret = ts_automation_get_status(&status);

    if (ret != ESP_OK) {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to get automation status");
        return ESP_OK;
    }

    result->data = cJSON_CreateObject();
    cJSON_AddStringToObject(result->data, "state", state_to_string(status.state));
    cJSON_AddNumberToObject(result->data, "uptime_ms", status.uptime_ms);
    cJSON_AddNumberToObject(result->data, "sources_count", status.sources_count);
    cJSON_AddNumberToObject(result->data, "sources_active", status.sources_active);
    cJSON_AddNumberToObject(result->data, "rules_count", status.rules_count);
    cJSON_AddNumberToObject(result->data, "rules_active", status.rules_active);
    cJSON_AddNumberToObject(result->data, "variables_count", status.variables_count);
    cJSON_AddNumberToObject(result->data, "actions_executed", status.actions_executed);
    cJSON_AddNumberToObject(result->data, "rule_triggers", status.rule_triggers);
    cJSON_AddStringToObject(result->data, "config_path", status.config_path ? status.config_path : "");
    cJSON_AddBoolToObject(result->data, "config_modified", status.config_modified);
    cJSON_AddStringToObject(result->data, "version", ts_automation_get_version());

    result->code = TS_API_OK;
    return ESP_OK;
}

/*===========================================================================*/
/*                           Control API                                      */
/*===========================================================================*/

/**
 * @brief automation.start - Start the automation engine
 */
static esp_err_t api_automation_start(const cJSON *params, ts_api_result_t *result)
{
    esp_err_t ret = ts_automation_start();

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Automation engine started");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Engine is stopping, busy, or not initialized");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to start automation engine");
    }

    return ESP_OK;
}

/**
 * @brief automation.stop - Stop the automation engine
 */
static esp_err_t api_automation_stop(const cJSON *params, ts_api_result_t *result)
{
    esp_err_t ret = ts_automation_stop();

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Automation engine stopped");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup(ret == ESP_ERR_TIMEOUT
            ? "Automation is stopping; retry after active operations finish"
            : "Failed to stop automation engine");
    }

    return ESP_OK;
}

/**
 * @brief automation.pause - Pause the automation engine
 */
static esp_err_t api_automation_pause(const cJSON *params, ts_api_result_t *result)
{
    esp_err_t ret = ts_automation_pause();

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Automation engine paused");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to pause automation engine");
    }

    return ESP_OK;
}

/**
 * @brief automation.resume - Resume the automation engine
 */
static esp_err_t api_automation_resume(const cJSON *params, ts_api_result_t *result)
{
    esp_err_t ret = ts_automation_resume();

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Automation engine resumed");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to resume automation engine");
    }

    return ESP_OK;
}

/**
 * @brief automation.reload - Reload configuration
 */
static esp_err_t api_automation_reload(const cJSON *params, ts_api_result_t *result)
{
    esp_err_t ret = ts_automation_reload();

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Configuration reloaded");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to reload configuration");
    }

    return ESP_OK;
}

/*===========================================================================*/
/*                           Variables API                                    */
/*===========================================================================*/

/**
 * @brief automation.variables.list - List all variables
 */
static esp_err_t api_automation_variables_list(const cJSON *params, ts_api_result_t *result)
{
    result->data = cJSON_CreateObject();
    cJSON *vars_array = cJSON_AddArrayToObject(result->data, "variables");
    int64_t now_ms = esp_timer_get_time() / 1000;
    const char *prefix = NULL;
    const char *source_id = NULL;
    size_t prefix_len = 0;
    bool include_value = true;
    bool include_meta = true;

    if (params) {
        cJSON *prefix_param = cJSON_GetObjectItem(params, "prefix");
        if (prefix_param && cJSON_IsString(prefix_param) && prefix_param->valuestring) {
            prefix = prefix_param->valuestring;
            prefix_len = strlen(prefix);
        }

        cJSON *source_id_param = cJSON_GetObjectItem(params, "source_id");
        if (source_id_param && cJSON_IsString(source_id_param) && source_id_param->valuestring) {
            source_id = source_id_param->valuestring;
        }

        cJSON *include_value_param = cJSON_GetObjectItem(params, "include_value");
        if (include_value_param && cJSON_IsBool(include_value_param)) {
            include_value = cJSON_IsTrue(include_value_param);
        }

        cJSON *include_meta_param = cJSON_GetObjectItem(params, "include_meta");
        if (include_meta_param && cJSON_IsBool(include_meta_param)) {
            include_meta = cJSON_IsTrue(include_meta_param);
        }
    }

    // 遍历所有变量（使用内部迭代器）
    ts_variable_iterate_ctx_t ctx = {0};
    ts_auto_variable_t var;
    
    while (ts_variable_iterate(&ctx, &var) == ESP_OK) {
        if (prefix_len > 0 && strncmp(var.name, prefix, prefix_len) != 0) {
            continue;
        }
        if (source_id && strcmp(var.source_id, source_id) != 0) {
            continue;
        }

        cJSON *var_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(var_obj, "name", var.name);
        if (include_value) {
            cJSON_AddItemToObject(var_obj, "value", value_to_json(&var.value));
        }
        
        // 类型字符串
        const char *type_str = "null";
        switch (var.value.type) {
            case TS_AUTO_VAL_BOOL: type_str = "bool"; break;
            case TS_AUTO_VAL_INT: type_str = "int"; break;
            case TS_AUTO_VAL_FLOAT: type_str = "float"; break;
            case TS_AUTO_VAL_STRING: type_str = "string"; break;
            default: break;
        }
        cJSON_AddStringToObject(var_obj, "type", type_str);
        cJSON_AddBoolToObject(var_obj, "persistent", (var.flags & TS_AUTO_VAR_PERSISTENT) != 0);
        cJSON_AddBoolToObject(var_obj, "readonly", (var.flags & TS_AUTO_VAR_READONLY) != 0);
        if (include_meta) {
            cJSON_AddNumberToObject(var_obj, "last_change_ms", (double)var.last_change_ms);
            cJSON_AddNumberToObject(var_obj, "last_update_ms", (double)var.last_update_ms);
            if (var.last_update_ms > 0) {
                int64_t age_ms = now_ms - var.last_update_ms;
                bool stale = (age_ms < 0 || age_ms > TS_TEMP_DATA_TIMEOUT_MS);
                if (age_ms < 0) age_ms = 0;
                cJSON_AddNumberToObject(var_obj, "age_ms", (double)age_ms);
                cJSON_AddBoolToObject(var_obj, "stale", stale);
            } else {
                cJSON_AddNullToObject(var_obj, "age_ms");
                cJSON_AddBoolToObject(var_obj, "stale", true);
            }
        }
        
        if (var.source_id[0] != '\0') {
            cJSON_AddStringToObject(var_obj, "source_id", var.source_id);
        }
        
        cJSON_AddItemToArray(vars_array, var_obj);
    }

    cJSON_AddNumberToObject(result->data, "count", cJSON_GetArraySize(vars_array));
    result->code = TS_API_OK;
    return ESP_OK;
}

/**
 * @brief automation.variables.get - Get a variable value
 */
static esp_err_t api_automation_variables_get(const cJSON *params, ts_api_result_t *result)
{
    cJSON *name_param = cJSON_GetObjectItem(params, "name");
    if (!name_param || !cJSON_IsString(name_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'name' parameter");
        return ESP_OK;
    }

    ts_auto_value_t value = {0};
    esp_err_t ret = ts_variable_get(name_param->valuestring, &value);

    if (ret != ESP_OK) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Variable not found");
        return ESP_OK;
    }

    result->data = cJSON_CreateObject();
    cJSON_AddStringToObject(result->data, "name", name_param->valuestring);
    cJSON_AddItemToObject(result->data, "value", value_to_json(&value));

    result->code = TS_API_OK;
    return ESP_OK;
}

/**
 * @brief automation.variables.set - Set a variable value
 * 
 * Params:
 *   - name: Variable name (required)
 *   - value: Value to set (required)
 *   - create_only: If true, only create if not exists, don't overwrite (optional)
 */
static esp_err_t api_automation_variables_set(const cJSON *params, ts_api_result_t *result)
{
    cJSON *name_param = cJSON_GetObjectItem(params, "name");
    cJSON *value_param = cJSON_GetObjectItem(params, "value");
    cJSON *create_only_param = cJSON_GetObjectItem(params, "create_only");
    
    bool create_only = create_only_param && cJSON_IsTrue(create_only_param);

    if (!name_param || !cJSON_IsString(name_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'name' parameter");
        return ESP_OK;
    }

    if (!value_param) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'value' parameter");
        return ESP_OK;
    }

    // 转换 JSON 值到 ts_auto_value_t
    ts_auto_value_t value = {0};
    
    if (cJSON_IsBool(value_param)) {
        value.type = TS_AUTO_VAL_BOOL;
        value.bool_val = cJSON_IsTrue(value_param);
    } else if (cJSON_IsNumber(value_param)) {
        // 检查是否为整数
        double d = value_param->valuedouble;
        if (d == (int32_t)d) {
            value.type = TS_AUTO_VAL_INT;
            value.int_val = (int32_t)d;
        } else {
            value.type = TS_AUTO_VAL_FLOAT;
            value.float_val = d;
        }
    } else if (cJSON_IsString(value_param)) {
        value.type = TS_AUTO_VAL_STRING;
        strncpy(value.str_val, value_param->valuestring, sizeof(value.str_val) - 1);
    } else if (cJSON_IsNull(value_param)) {
        value.type = TS_AUTO_VAL_NULL;
    } else {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Unsupported value type");
        return ESP_OK;
    }

    // create_only 模式：只在变量不存在时创建
    if (create_only) {
        ts_auto_value_t existing = {0};
        esp_err_t check_ret = ts_variable_get(name_param->valuestring, &existing);
        if (check_ret == ESP_OK) {
            // 变量已存在，不覆盖，直接返回成功
            result->code = TS_API_OK;
            result->message = strdup("Variable already exists (not overwritten)");
            result->data = cJSON_CreateObject();
            cJSON_AddStringToObject(result->data, "name", name_param->valuestring);
            cJSON_AddItemToObject(result->data, "value", value_to_json(&existing));
            cJSON_AddBoolToObject(result->data, "created", false);
            return ESP_OK;
        }
        // 变量不存在，继续创建
    }

    esp_err_t ret = ts_variable_set(name_param->valuestring, &value);

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Variable set successfully");
        result->data = cJSON_CreateObject();
        cJSON_AddStringToObject(result->data, "name", name_param->valuestring);
        cJSON_AddItemToObject(result->data, "value", value_to_json(&value));
        if (create_only) {
            cJSON_AddBoolToObject(result->data, "created", false);  // 变量已存在被更新
        }
    } else if (ret == ESP_ERR_NOT_FOUND) {
        // 如果变量不存在，创建它
        ts_auto_variable_t new_var = {0};
        strncpy(new_var.name, name_param->valuestring, sizeof(new_var.name) - 1);
        new_var.value = value;
        new_var.default_value = value;
        
        // 从变量名自动提取 source_id（取第一个 '.' 之前的部分）
        // 例如: "ping_test.status" → source_id = "ping_test"
        const char *dot = strchr(name_param->valuestring, '.');
        if (dot) {
            size_t prefix_len = dot - name_param->valuestring;
            if (prefix_len > 0 && prefix_len < sizeof(new_var.source_id)) {
                strncpy(new_var.source_id, name_param->valuestring, prefix_len);
                new_var.source_id[prefix_len] = '\0';
            }
        }
        
        ret = ts_variable_register(&new_var);
        if (ret == ESP_OK) {
            ret = ts_variable_set(name_param->valuestring, &value);
        }
        
        if (ret == ESP_OK) {
            result->code = TS_API_OK;
            result->message = strdup("Variable created and set");
            result->data = cJSON_CreateObject();
            cJSON_AddStringToObject(result->data, "name", name_param->valuestring);
            cJSON_AddItemToObject(result->data, "value", value_to_json(&value));
            cJSON_AddBoolToObject(result->data, "created", true);
        } else {
            result->code = TS_API_ERR_INTERNAL;
            result->message = strdup("Failed to create variable");
        }
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to set variable");
    }

    return ESP_OK;
}

/*===========================================================================*/
/*                           Rules API                                        */
/*===========================================================================*/

/**
 * @brief automation.rules.list - List all rules
 */
static esp_err_t api_automation_rules_list(const cJSON *params, ts_api_result_t *result)
{
    result->data = cJSON_CreateObject();
    bool loaded, recovery;
    ts_rule_config_status(&loaded, &recovery);
    cJSON_AddBoolToObject(result->data, "loaded", loaded);
    cJSON_AddBoolToObject(result->data, "recovery_required", recovery);
    cJSON *rules_array = cJSON_AddArrayToObject(result->data, "rules");

    // 遍历所有规则
    int count = ts_rule_count();
    for (int i = 0; i < count; i++) {
        ts_auto_rule_t rule;
        if (ts_rule_get_by_index(i, &rule) != ESP_OK) continue;

        ts_rule_resolve_presentation(&rule);
        cJSON *rule_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(rule_obj, "id", rule.id);
        cJSON_AddStringToObject(rule_obj, "name", rule.name);
        cJSON_AddStringToObject(rule_obj, "icon", rule.icon[0] ? rule.icon : "⚡");
        cJSON_AddBoolToObject(rule_obj, "enabled", rule.enabled);
        cJSON_AddBoolToObject(rule_obj, "manual_trigger", rule.manual_trigger);
        cJSON_AddBoolToObject(rule_obj, "show_on_dashboard", rule.show_on_dashboard);
        cJSON_AddBoolToObject(rule_obj, "allow_manual_trigger", rule.allow_manual_trigger);
        cJSON_AddBoolToObject(rule_obj, "reference_unresolved", rule.reference_unresolved);
        cJSON_AddNumberToObject(rule_obj, "revision", rule.revision);
        cJSON_AddNumberToObject(rule_obj, "trigger_count", rule.trigger_count);
        cJSON_AddNumberToObject(rule_obj, "last_trigger_ms", (double)rule.last_trigger_ms);
        cJSON_AddNumberToObject(rule_obj, "cooldown_ms", rule.cooldown_ms);
        cJSON_AddNumberToObject(rule_obj, "conditions_count", rule.conditions.count);
        cJSON_AddNumberToObject(rule_obj, "actions_count", rule.action_count);

        cJSON_AddItemToArray(rules_array, rule_obj);
        ts_rule_release(&rule);
    }

    cJSON_AddNumberToObject(result->data, "count", count);
    result->code = TS_API_OK;
    return ESP_OK;
}

/**
 * @brief automation.rules.enable - Enable a rule
 */
static void rule_commit_reply(ts_api_result_t *result, esp_err_t ret,
                              const ts_rule_commit_result_t *commit) {
    result->code = ret == ESP_OK ? TS_API_OK : TS_API_ERR_INVALID_ARG;
    result->message = strdup(commit->error_code ? commit->error_code : "invalid_configuration");
    result->data = cJSON_CreateObject();
    if (!result->data)
        return;
    if (commit->applied < 0)
        cJSON_AddNullToObject(result->data, "applied");
    else
        cJSON_AddBoolToObject(result->data, "applied", commit->applied);
    if (commit->durable < 0)
        cJSON_AddNullToObject(result->data, "durable");
    else
        cJSON_AddBoolToObject(result->data, "durable", commit->durable);
    if (commit->mirror_synced < 0)
        cJSON_AddNullToObject(result->data, "mirror_synced");
    else
        cJSON_AddBoolToObject(result->data, "mirror_synced", commit->mirror_synced);
    cJSON_AddNumberToObject(result->data, "revision", commit->revision);
    cJSON_AddStringToObject(result->data, "error_code",
                            commit->error_code ? commit->error_code : "invalid_configuration");
}
static esp_err_t rule_mutate_locked(const cJSON *params, ts_api_result_t *result, int operation) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "id"),
                *revision = cJSON_GetObjectItemCaseSensitive(params, "expected_revision");
    if (!cJSON_IsString(id) || !ts_rule_id_valid(id->valuestring) || !cJSON_IsNumber(revision) ||
        !isfinite(revision->valuedouble) || revision->valuedouble < 0 ||
        revision->valuedouble > UINT32_MAX ||
        trunc(revision->valuedouble) != revision->valuedouble) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG,
                            "refresh_required: refresh the interface before saving");
        return ESP_OK;
    }
    uint32_t expected = revision->valuedouble;
    ts_rule_commit_result_t commit = {.error_code = "invalid_configuration"};
    if (operation == 3) {
        esp_err_t e = ts_rule_commit(NULL, id->valuestring, expected, &commit);
        rule_commit_reply(result, e, &commit);
        return ESP_OK;
    }
    ts_auto_rule_t old = {0};
    esp_err_t found = ts_rule_acquire(id->valuestring, &old);
    cJSON *merged = found == ESP_OK ? ts_rule_encode(&old) : cJSON_CreateObject();
    if (found == ESP_OK && old.revision != expected) {
        ts_rule_release(&old);
        cJSON_Delete(merged);
        commit.error_code = "revision_conflict";
        rule_commit_reply(result, ESP_ERR_INVALID_STATE, &commit);
        return ESP_OK;
    }
    ts_rule_release(&old);
    if (!merged) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "no_memory");
        return ESP_OK;
    }
    const cJSON *field;
    cJSON_ArrayForEach(field, params) {
        if (!field->string || !strcmp(field->string, "expected_revision"))
            continue;
        cJSON *copy = cJSON_Duplicate(field, true);
        if (!copy) {
            cJSON_Delete(merged);
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "no_memory");
            return ESP_OK;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(merged, field->string);
        if (!cJSON_AddItemToObject(merged, field->string, copy)) {
            cJSON_Delete(copy);
            cJSON_Delete(merged);
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "no_memory");
            return ESP_OK;
        }
    }
    if (operation == 1 || operation == 2) {
        cJSON_DeleteItemFromObject(merged, "enabled");
        if (!cJSON_AddBoolToObject(merged, "enabled", operation == 1)) {
            cJSON_Delete(merged);
            return ESP_ERR_NO_MEM;
        }
    }
    ts_auto_rule_t candidate = {0};
    esp_err_t e = ts_rule_decode(merged, &candidate);
    cJSON_Delete(merged);
    if (e == ESP_OK)
        e = ts_rule_commit(&candidate, id->valuestring, expected, &commit);
    else
        commit.error_code = e == ESP_ERR_NO_MEM ? "no_memory" : "invalid_configuration";
    ts_rule_dispose(&candidate);
    rule_commit_reply(result, e, &commit);
    return ESP_OK;
}

static esp_err_t rule_mutate(const cJSON *params, ts_api_result_t *result, int operation) {
    if (!ts_rule_edit_begin()) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "configuration_busy");
        return ESP_OK;
    }
    esp_err_t ret = rule_mutate_locked(params, result, operation);
    ts_rule_edit_end();
    return ret;
}
static esp_err_t api_automation_rules_enable(const cJSON *params, ts_api_result_t *result) {
    return rule_mutate(params, result, 1);
}

/**
 * @brief automation.rules.disable - Disable a rule
 */
static esp_err_t api_automation_rules_disable(const cJSON *params, ts_api_result_t *result) {
    return rule_mutate(params, result, 2);
}

/**
 * @brief automation.rules.trigger - Manually trigger a rule
 */
static esp_err_t service_request(const cJSON *params, ts_api_result_t *result, bool stop) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "command_id");
    if (!cJSON_IsString(id) || !id->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "command_id required");
        return ESP_ERR_INVALID_ARG;
    }
    ts_ssh_service_status_t state = {0};
    esp_err_t ret = stop ? ts_ssh_service_stop(id->valuestring, &state)
                         : (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "verify"))
                                ? ts_ssh_service_query(id->valuestring, &state)
                                : ts_ssh_service_cached(id->valuestring, &state));
    cJSON *data = cJSON_CreateObject();
    if (!data)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(data, "state", state.state[0] ? state.state : "unknown");
    cJSON_AddStringToObject(data, "last_known", state.last_known);
    cJSON_AddStringToObject(data, "source", state.source);
    cJSON_AddNumberToObject(data, "confirmed_ms", state.confirmed_ms);
    cJSON_AddNumberToObject(data, "generation", state.generation);
    cJSON_AddBoolToObject(data, "busy", state.busy);
    if (ret == ESP_OK)
        ts_api_result_ok(result, data);
    else {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "service_state_unconfirmed");
        result->data = data;
    }
    return ret;
}
static esp_err_t api_service_status(const cJSON *params, ts_api_result_t *result) {
    return service_request(params, result, false);
}
static esp_err_t api_service_stop(const cJSON *params, ts_api_result_t *result) {
    return service_request(params, result, true);
}

static esp_err_t api_automation_rules_trigger(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id_param = cJSON_GetObjectItem(params, "id");
    if (!id_param || !cJSON_IsString(id_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }

    ESP_LOGI("api_auto", "Calling ts_rule_trigger('%s')", id_param->valuestring);
    esp_err_t ret = ts_rule_trigger(id_param->valuestring);
    ESP_LOGI("api_auto", "ts_rule_trigger returned: %s", esp_err_to_name(ret));

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Rule triggered");
    } else if (ret == ESP_ERR_NOT_FOUND) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Rule not found");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to trigger rule");
    }

    return ESP_OK;
}

/**
 * @brief Convert operator to string
 */

/**
 * @brief automation.rules.get - Get rule details by ID
 */
static esp_err_t api_automation_rules_get(const cJSON *params, ts_api_result_t *result) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "id");
    ts_auto_rule_t rule;
    if (!cJSON_IsString(id) || ts_rule_acquire(id->valuestring, &rule) != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Rule not found");
        return ESP_OK;
    }
    ts_rule_resolve_presentation(&rule);
    result->data = ts_rule_encode(&rule);
    ts_rule_release(&rule);
    result->code = result->data ? TS_API_OK : TS_API_ERR_INTERNAL;
    return ESP_OK;
}

/**
 * @brief Parse operator from string
 */

/**
 * @brief Parse action type from string
 */

/**
 * @brief automation.rules.add - Add a new rule
 */
static esp_err_t api_automation_rules_add(const cJSON *params, ts_api_result_t *result) {
    return rule_mutate(params, result, 0);
}

/**
 * @brief automation.rules.delete - Delete a rule
 */
static esp_err_t api_automation_rules_delete(const cJSON *params, ts_api_result_t *result) {
    return rule_mutate(params, result, 3);
}

/*===========================================================================*/
/*                           Sources API                                      */
/*===========================================================================*/

/**
 * @brief automation.sources.list - List all data sources
 */
static esp_err_t api_automation_sources_list(const cJSON *params, ts_api_result_t *result)
{
    result->data = cJSON_CreateObject();
    cJSON *sources_array = cJSON_AddArrayToObject(result->data, "sources");

    // 遍历所有数据源 - 使用线程安全的副本获取
    int count = ts_source_count();
    ts_auto_source_t source_copy;
    
    for (int i = 0; i < count; i++) {
        // 获取源的线程安全副本
        if (ts_source_get_by_index_copy(i, &source_copy) != ESP_OK) {
            continue;
        }

        cJSON *src_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(src_obj, "id", source_copy.id);
        cJSON_AddStringToObject(src_obj, "label", source_copy.label);
        
        const char *type_str = "unknown";
        switch (source_copy.type) {
            case TS_AUTO_SRC_WEBSOCKET: type_str = "websocket"; break;
            case TS_AUTO_SRC_SOCKETIO: type_str = "socketio"; break;
            case TS_AUTO_SRC_REST: type_str = "rest"; break;
            case TS_AUTO_SRC_VARIABLE: type_str = "variable"; break;
            default: type_str = "unknown"; break;
        }
        cJSON_AddStringToObject(src_obj, "type", type_str);
        cJSON_AddBoolToObject(src_obj, "enabled", source_copy.enabled);
        cJSON_AddBoolToObject(src_obj, "connected", source_copy.connected);
        cJSON_AddBoolToObject(src_obj, "auto_discover", source_copy.auto_discover);
        
        // 根据源类型设置更新模式
        if (source_copy.type == TS_AUTO_SRC_SOCKETIO || source_copy.type == TS_AUTO_SRC_WEBSOCKET) {
            cJSON_AddStringToObject(src_obj, "update_mode", "realtime");
            cJSON_AddNumberToObject(src_obj, "poll_interval_ms", 0);
        } else {
            cJSON_AddStringToObject(src_obj, "update_mode", "polling");
            cJSON_AddNumberToObject(src_obj, "poll_interval_ms", source_copy.poll_interval_ms);
        }
        cJSON_AddItemToObject(src_obj, "last_value", value_to_json(&source_copy.last_value));
        cJSON_AddNumberToObject(src_obj, "last_update_ms", (double)source_copy.last_update_ms);
        
        // 添加 mappings 数组
        cJSON *mappings_arr = cJSON_AddArrayToObject(src_obj, "mappings");
        for (uint8_t m = 0; m < source_copy.mapping_count && m < TS_AUTO_MAX_MAPPINGS; m++) {
            cJSON *map_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(map_obj, "json_path", source_copy.mappings[m].json_path);
            cJSON_AddStringToObject(map_obj, "var_name", source_copy.mappings[m].var_name);
            if (source_copy.mappings[m].transform[0]) {
                cJSON_AddStringToObject(map_obj, "transform", source_copy.mappings[m].transform);
            }
            cJSON_AddItemToArray(mappings_arr, map_obj);
        }
        cJSON_AddNumberToObject(src_obj, "mapping_count", source_copy.mapping_count);

        cJSON_AddItemToArray(sources_array, src_obj);
    }

    cJSON_AddNumberToObject(result->data, "count", count);
    result->code = TS_API_OK;
    return ESP_OK;
}

/**
 * @brief Parse source type from string
 */
static ts_auto_source_type_t parse_source_type(const char *type_str)
{
    if (!type_str) return TS_AUTO_SRC_REST;
    if (strcmp(type_str, "websocket") == 0) return TS_AUTO_SRC_WEBSOCKET;
    if (strcmp(type_str, "socketio") == 0) return TS_AUTO_SRC_SOCKETIO;
    if (strcmp(type_str, "rest") == 0) return TS_AUTO_SRC_REST;
    if (strcmp(type_str, "variable") == 0) return TS_AUTO_SRC_VARIABLE;
    return TS_AUTO_SRC_REST;
}

/**
 * @brief automation.sources.add - Add a new data source
 */
static esp_err_t api_automation_sources_add(const cJSON *params, ts_api_result_t *result)
{
    // 必须参数
    cJSON *id_param = cJSON_GetObjectItem(params, "id");
    cJSON *label_param = cJSON_GetObjectItem(params, "label");
    cJSON *type_param = cJSON_GetObjectItem(params, "type");
    
    if (!id_param || !cJSON_IsString(id_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }
    
    if (!label_param || !cJSON_IsString(label_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'label' parameter");
        return ESP_OK;
    }
    
    if (!type_param || !cJSON_IsString(type_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'type' parameter");
        return ESP_OK;
    }

    // 使用堆分配避免栈溢出（ts_auto_source_t ~1KB+）
    ts_auto_source_t *source = heap_caps_calloc(1, sizeof(ts_auto_source_t), MALLOC_CAP_SPIRAM);
    if (!source) {
        source = calloc(1, sizeof(ts_auto_source_t));  // Fallback to DRAM
    }
    if (!source) {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Memory allocation failed");
        return ESP_OK;
    }

    strncpy(source->id, id_param->valuestring, sizeof(source->id) - 1);
    strncpy(source->label, label_param->valuestring, sizeof(source->label) - 1);
    source->type = parse_source_type(type_param->valuestring);
    
    // 可选参数
    cJSON *enabled = cJSON_GetObjectItem(params, "enabled");
    source->enabled = enabled ? cJSON_IsTrue(enabled) : true;
    
    cJSON *auto_discover = cJSON_GetObjectItem(params, "auto_discover");
    source->auto_discover = auto_discover ? cJSON_IsTrue(auto_discover) : true;  // 默认启用
    
    cJSON *interval = cJSON_GetObjectItem(params, "poll_interval_ms");
    source->poll_interval_ms = interval && cJSON_IsNumber(interval) ? (uint32_t)interval->valueint : 1000;

    // 根据类型解析配置
    switch (source->type) {
        case TS_AUTO_SRC_WEBSOCKET: {
            cJSON *uri = cJSON_GetObjectItem(params, "uri");
            if (!uri) uri = cJSON_GetObjectItem(params, "url");  // 兼容 url 参数
            if (uri && cJSON_IsString(uri)) {
                strncpy(source->websocket.uri, uri->valuestring, sizeof(source->websocket.uri) - 1);
            }
            cJSON *reconnect = cJSON_GetObjectItem(params, "reconnect_ms");
            source->websocket.reconnect_ms = reconnect && cJSON_IsNumber(reconnect) ? (uint16_t)reconnect->valueint : 5000;
            
            // 处理单独的 json_path 参数 - 自动转换为 mapping（与 Socket.IO 一致）
            cJSON *single_path = cJSON_GetObjectItem(params, "json_path");
            if (single_path && cJSON_IsString(single_path) && 
                strlen(single_path->valuestring) > 0 &&
                source->mapping_count < TS_AUTO_MAX_MAPPINGS) {
                // 同时保存到 websocket.path（向后兼容）
                strncpy(source->websocket.path, single_path->valuestring, sizeof(source->websocket.path) - 1);
                
                // 自动生成变量名: source_id.path
                char var_name[128];
                int len = snprintf(var_name, sizeof(var_name), "%s.%s", 
                                   source->id, single_path->valuestring);
                if (len >= TS_AUTO_NAME_MAX_LEN) {
                    var_name[TS_AUTO_NAME_MAX_LEN - 1] = '\0';
                }
                // 清理变量名中的特殊字符
                for (char *p = var_name; *p; p++) {
                    if (*p == '[' || *p == ']') *p = '_';
                }
                
                strncpy(source->mappings[source->mapping_count].json_path,
                        single_path->valuestring,
                        sizeof(source->mappings[0].json_path) - 1);
                strncpy(source->mappings[source->mapping_count].var_name,
                        var_name,
                        sizeof(source->mappings[0].var_name) - 1);
                source->mapping_count++;
                TS_LOGI(TAG, "WebSocket: Added mapping from json_path: %s -> %s",
                        single_path->valuestring, var_name);
            }
            
            // 解析 mappings 数组（与 Socket.IO 一致）
            cJSON *mappings = cJSON_GetObjectItem(params, "mappings");
            if (mappings && cJSON_IsArray(mappings)) {
                int count = cJSON_GetArraySize(mappings);
                if (count > TS_AUTO_MAX_MAPPINGS - source->mapping_count) {
                    count = TS_AUTO_MAX_MAPPINGS - source->mapping_count;
                }
                for (int i = 0; i < count; i++) {
                    cJSON *mapping = cJSON_GetArrayItem(mappings, i);
                    if (!mapping || !cJSON_IsObject(mapping)) continue;
                    
                    cJSON *json_path = cJSON_GetObjectItem(mapping, "json_path");
                    cJSON *var_name = cJSON_GetObjectItem(mapping, "var_name");
                    
                    if (json_path && cJSON_IsString(json_path) && 
                        var_name && cJSON_IsString(var_name)) {
                        strncpy(source->mappings[source->mapping_count].json_path, 
                                json_path->valuestring, 
                                sizeof(source->mappings[0].json_path) - 1);
                        strncpy(source->mappings[source->mapping_count].var_name, 
                                var_name->valuestring, 
                                sizeof(source->mappings[0].var_name) - 1);
                        
                        cJSON *transform = cJSON_GetObjectItem(mapping, "transform");
                        if (transform && cJSON_IsString(transform)) {
                            strncpy(source->mappings[source->mapping_count].transform, 
                                    transform->valuestring, 
                                    sizeof(source->mappings[0].transform) - 1);
                        }
                        
                        source->mapping_count++;
                        TS_LOGI(TAG, "WebSocket: Added mapping: %s -> %s", 
                                json_path->valuestring, var_name->valuestring);
                    }
                }
            }
            break;
        }
        case TS_AUTO_SRC_SOCKETIO: {
            // Socket.IO 是事件驱动的，不使用轮询间隔
            source->poll_interval_ms = 0;  // 0 = 事件驱动
            
            cJSON *url = cJSON_GetObjectItem(params, "url");
            if (!url) url = cJSON_GetObjectItem(params, "uri");  // 兼容 uri 参数
            if (url && cJSON_IsString(url)) {
                strncpy(source->socketio.url, url->valuestring, sizeof(source->socketio.url) - 1);
            }
            cJSON *event = cJSON_GetObjectItem(params, "event");
            if (event && cJSON_IsString(event)) {
                strncpy(source->socketio.event, event->valuestring, sizeof(source->socketio.event) - 1);
            }
            cJSON *reconnect = cJSON_GetObjectItem(params, "reconnect_ms");
            source->socketio.reconnect_ms = reconnect && cJSON_IsNumber(reconnect) ? (uint16_t)reconnect->valueint : 5000;
            
            // 处理单独的 json_path 参数 - 自动转换为 mapping
            // 这样用户选择单个节点时也能创建变量
            cJSON *single_path = cJSON_GetObjectItem(params, "json_path");
            if (single_path && cJSON_IsString(single_path) && 
                strlen(single_path->valuestring) > 0 &&
                source->mapping_count < TS_AUTO_MAX_MAPPINGS) {
                // 自动生成变量名: source_id.path (将 . 和 [] 转换为 _)
                // 使用更大的缓冲区，然后截断到目标大小
                char var_name[128];
                int len = snprintf(var_name, sizeof(var_name), "%s.%s", 
                                   source->id, single_path->valuestring);
                // 截断到最大长度
                if (len >= TS_AUTO_NAME_MAX_LEN) {
                    var_name[TS_AUTO_NAME_MAX_LEN - 1] = '\0';
                }
                // 清理变量名中的特殊字符
                for (char *p = var_name; *p; p++) {
                    if (*p == '[' || *p == ']') *p = '_';
                }
                
                strncpy(source->mappings[source->mapping_count].json_path,
                        single_path->valuestring,
                        sizeof(source->mappings[0].json_path) - 1);
                strncpy(source->mappings[source->mapping_count].var_name,
                        var_name,
                        sizeof(source->mappings[0].var_name) - 1);
                source->mapping_count++;
                TS_LOGI(TAG, "Added mapping from json_path: %s -> %s",
                        single_path->valuestring, var_name);
            }
            
            // 解析 mappings 数组 - 允许从事件数据中提取多个字段作为独立变量
            cJSON *mappings = cJSON_GetObjectItem(params, "mappings");
            if (mappings && cJSON_IsArray(mappings)) {
                int count = cJSON_GetArraySize(mappings);
                if (count > TS_AUTO_MAX_MAPPINGS - source->mapping_count) {
                    count = TS_AUTO_MAX_MAPPINGS - source->mapping_count;
                }
                for (int i = 0; i < count; i++) {
                    cJSON *mapping = cJSON_GetArrayItem(mappings, i);
                    if (!mapping || !cJSON_IsObject(mapping)) continue;
                    
                    cJSON *json_path = cJSON_GetObjectItem(mapping, "json_path");
                    cJSON *var_name = cJSON_GetObjectItem(mapping, "var_name");
                    
                    if (json_path && cJSON_IsString(json_path) && 
                        var_name && cJSON_IsString(var_name)) {
                        strncpy(source->mappings[source->mapping_count].json_path, 
                                json_path->valuestring, 
                                sizeof(source->mappings[0].json_path) - 1);
                        strncpy(source->mappings[source->mapping_count].var_name, 
                                var_name->valuestring, 
                                sizeof(source->mappings[0].var_name) - 1);
                        
                        // 可选的 transform 表达式
                        cJSON *transform = cJSON_GetObjectItem(mapping, "transform");
                        if (transform && cJSON_IsString(transform)) {
                            strncpy(source->mappings[source->mapping_count].transform, 
                                    transform->valuestring, 
                                    sizeof(source->mappings[0].transform) - 1);
                        }
                        
                        source->mapping_count++;
                        TS_LOGI(TAG, "Added mapping: %s -> %s", 
                                json_path->valuestring, var_name->valuestring);
                    }
                }
            }
            break;
        }
        case TS_AUTO_SRC_REST: {
            cJSON *url = cJSON_GetObjectItem(params, "url");
            if (url && cJSON_IsString(url)) {
                strncpy(source->rest.url, url->valuestring, sizeof(source->rest.url) - 1);
            }
            cJSON *method = cJSON_GetObjectItem(params, "method");
            if (method && cJSON_IsString(method)) {
                strncpy(source->rest.method, method->valuestring, sizeof(source->rest.method) - 1);
            } else {
                strcpy(source->rest.method, "GET");
            }
            cJSON *auth = cJSON_GetObjectItem(params, "auth_header");
            if (auth && cJSON_IsString(auth)) {
                strncpy(source->rest.auth_header, auth->valuestring, sizeof(source->rest.auth_header) - 1);
            }
            
            // 处理单独的 json_path 参数 - 自动转换为 mapping（与 Socket.IO 一致）
            cJSON *single_path = cJSON_GetObjectItem(params, "json_path");
            if (single_path && cJSON_IsString(single_path) && 
                strlen(single_path->valuestring) > 0 &&
                source->mapping_count < TS_AUTO_MAX_MAPPINGS) {
                // 同时保存到 rest.path（向后兼容）
                strncpy(source->rest.path, single_path->valuestring, sizeof(source->rest.path) - 1);
                
                // 自动生成变量名: source_id.path
                char var_name[128];
                int len = snprintf(var_name, sizeof(var_name), "%s.%s", 
                                   source->id, single_path->valuestring);
                if (len >= TS_AUTO_NAME_MAX_LEN) {
                    var_name[TS_AUTO_NAME_MAX_LEN - 1] = '\0';
                }
                // 清理变量名中的特殊字符
                for (char *p = var_name; *p; p++) {
                    if (*p == '[' || *p == ']') *p = '_';
                }
                
                strncpy(source->mappings[source->mapping_count].json_path,
                        single_path->valuestring,
                        sizeof(source->mappings[0].json_path) - 1);
                strncpy(source->mappings[source->mapping_count].var_name,
                        var_name,
                        sizeof(source->mappings[0].var_name) - 1);
                source->mapping_count++;
                TS_LOGI(TAG, "REST: Added mapping from json_path: %s -> %s",
                        single_path->valuestring, var_name);
            }
            
            // 解析 mappings 数组（与 Socket.IO 一致）
            cJSON *mappings = cJSON_GetObjectItem(params, "mappings");
            if (mappings && cJSON_IsArray(mappings)) {
                int count = cJSON_GetArraySize(mappings);
                if (count > TS_AUTO_MAX_MAPPINGS - source->mapping_count) {
                    count = TS_AUTO_MAX_MAPPINGS - source->mapping_count;
                }
                for (int i = 0; i < count; i++) {
                    cJSON *mapping = cJSON_GetArrayItem(mappings, i);
                    if (!mapping || !cJSON_IsObject(mapping)) continue;
                    
                    cJSON *json_path = cJSON_GetObjectItem(mapping, "json_path");
                    cJSON *var_name = cJSON_GetObjectItem(mapping, "var_name");
                    
                    if (json_path && cJSON_IsString(json_path) && 
                        var_name && cJSON_IsString(var_name)) {
                        strncpy(source->mappings[source->mapping_count].json_path, 
                                json_path->valuestring, 
                                sizeof(source->mappings[0].json_path) - 1);
                        strncpy(source->mappings[source->mapping_count].var_name, 
                                var_name->valuestring, 
                                sizeof(source->mappings[0].var_name) - 1);
                        
                        cJSON *transform = cJSON_GetObjectItem(mapping, "transform");
                        if (transform && cJSON_IsString(transform)) {
                            strncpy(source->mappings[source->mapping_count].transform, 
                                    transform->valuestring, 
                                    sizeof(source->mappings[0].transform) - 1);
                        }
                        
                        source->mapping_count++;
                        TS_LOGI(TAG, "REST: Added mapping: %s -> %s", 
                                json_path->valuestring, var_name->valuestring);
                    }
                }
            }
            break;
        }
        case TS_AUTO_SRC_VARIABLE: {
            // SSH 指令变量数据源配置
            cJSON *ssh_host = cJSON_GetObjectItem(params, "ssh_host_id");
            if (ssh_host && cJSON_IsString(ssh_host)) {
                strncpy(source->variable.ssh_host_id, ssh_host->valuestring, 
                        sizeof(source->variable.ssh_host_id) - 1);
            }
            
            cJSON *ssh_cmd = cJSON_GetObjectItem(params, "ssh_command");
            if (ssh_cmd && cJSON_IsString(ssh_cmd)) {
                strncpy(source->variable.ssh_command, ssh_cmd->valuestring, 
                        sizeof(source->variable.ssh_command) - 1);
            }
            
            cJSON *var_prefix = cJSON_GetObjectItem(params, "var_prefix");
            if (var_prefix && cJSON_IsString(var_prefix)) {
                strncpy(source->variable.var_prefix, var_prefix->valuestring, 
                        sizeof(source->variable.var_prefix) - 1);
            }
            
            // 高级选项
            cJSON *expect = cJSON_GetObjectItem(params, "ssh_expect_pattern");
            if (expect && cJSON_IsString(expect)) {
                strncpy(source->variable.expect_pattern, expect->valuestring, 
                        sizeof(source->variable.expect_pattern) - 1);
            }
            
            cJSON *fail = cJSON_GetObjectItem(params, "ssh_fail_pattern");
            if (fail && cJSON_IsString(fail)) {
                strncpy(source->variable.fail_pattern, fail->valuestring, 
                        sizeof(source->variable.fail_pattern) - 1);
            }
            
            cJSON *extract = cJSON_GetObjectItem(params, "ssh_extract_pattern");
            if (extract && cJSON_IsString(extract)) {
                strncpy(source->variable.extract_pattern, extract->valuestring, 
                        sizeof(source->variable.extract_pattern) - 1);
            }
            
            cJSON *timeout = cJSON_GetObjectItem(params, "ssh_timeout");
            source->variable.timeout_sec = timeout && cJSON_IsNumber(timeout) ? 
                                           (uint16_t)timeout->valueint : 30;
            
            // watch_all 模式
            cJSON *watch_all = cJSON_GetObjectItem(params, "var_watch_all");
            source->variable.watch_all = watch_all && cJSON_IsTrue(watch_all);
            
            TS_LOGI(TAG, "Variable source: host=%s, cmd=%s, prefix=%s", 
                    source->variable.ssh_host_id,
                    source->variable.ssh_command,
                    source->variable.var_prefix);
            
            // 预创建变量（值为空，等待指令执行后填充）
            if (source->variable.var_prefix[0] != '\0') {
                // 预创建标准变量，每个变量有不同的类型
                typedef struct {
                    const char *suffix;
                    ts_auto_value_type_t type;
                } var_def_t;
                
                const var_def_t var_defs[] = {
                    {"status",         TS_AUTO_VAL_STRING},   // "success"/"failed"/"timeout"
                    {"exit_code",      TS_AUTO_VAL_INT},      // 0, 1, 255, ...
                    {"extracted",      TS_AUTO_VAL_STRING},   // 提取的内容
                    {"expect_matched", TS_AUTO_VAL_BOOL},     // true/false
                    {"fail_matched",   TS_AUTO_VAL_BOOL},     // true/false
                    {"host",           TS_AUTO_VAL_STRING},   // 主机名/IP
                    {"timestamp",      TS_AUTO_VAL_INT},      // Unix 时间戳
                };
                
                int created_count = 0;
                for (size_t i = 0; i < sizeof(var_defs) / sizeof(var_defs[0]); i++) {
                    char var_name[TS_AUTO_NAME_MAX_LEN];
                    int written = snprintf(var_name, sizeof(var_name), "%s.%s", source->id, var_defs[i].suffix);
                    if (written < 0 || (size_t)written >= sizeof(var_name)) {
                        TS_LOGW(TAG, "Variable name too long, skipping: %s.%s", source->id, var_defs[i].suffix);
                        continue;
                    }
                    
                    // 只在变量不存在时创建，避免覆盖 SSH 执行产生的有效值
                    ts_auto_value_t existing_value = {0};
                    if (ts_variable_get(var_name, &existing_value) == ESP_OK) {
                        // 变量已存在，跳过
                        TS_LOGD(TAG, "Variable already exists, skipping: %s", var_name);
                        continue;
                    }
                    
                    ts_auto_variable_t var = {0};
                    strncpy(var.name, var_name, sizeof(var.name) - 1);
                    strncpy(var.source_id, source->id, sizeof(var.source_id) - 1);
                    var.value.type = var_defs[i].type;
                    // 根据类型设置默认值
                    switch (var_defs[i].type) {
                        case TS_AUTO_VAL_STRING:
                            var.value.str_val[0] = '\0';
                            break;
                        case TS_AUTO_VAL_INT:
                            var.value.int_val = 0;
                            break;
                        case TS_AUTO_VAL_BOOL:
                            var.value.bool_val = false;
                            break;
                        default:
                            break;
                    }
                    var.flags = 0;  // 可读写
                    
                    esp_err_t reg_ret = ts_variable_register(&var);
                    if (reg_ret == ESP_OK) {
                        TS_LOGD(TAG, "Pre-created variable: %s (type=%d)", var.name, var_defs[i].type);
                        created_count++;
                    } else {
                        TS_LOGW(TAG, "Failed to pre-create variable %s: %s", 
                                var.name, esp_err_to_name(reg_ret));
                    }
                }
                TS_LOGI(TAG, "Pre-created %d/%d variables for source '%s'", 
                        created_count, (int)(sizeof(var_defs) / sizeof(var_defs[0])), source->id);
            }
            break;
        }
        default:
            break;
    }

    esp_err_t ret = ts_source_register(source);
    
    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Source created successfully");
        result->data = cJSON_CreateObject();
        cJSON_AddStringToObject(result->data, "id", source->id);
    } else if (ret == ESP_ERR_NO_MEM) {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("No memory for new source");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Source with this ID already exists");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to create source");
    }
    
    free(source);
    return ESP_OK;
}

/**
 * @brief automation.sources.delete - Delete a data source
 */
static esp_err_t api_automation_sources_delete(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id_param = cJSON_GetObjectItem(params, "id");
    if (!id_param || !cJSON_IsString(id_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }

    const char *source_id = id_param->valuestring;
    esp_err_t ret = ts_source_unregister(source_id);
    
    if (ret == ESP_OK) {
        // 同时删除 SD 卡上的配置文件（.json 和 .tscfg）
        char filepath[128];
        snprintf(filepath, sizeof(filepath), "%s/%s.json", SOURCES_SDCARD_DIR, source_id);
        if (unlink(filepath) == 0) {
            TS_LOGI(TAG, "Deleted source config: %s", filepath);
        }
        snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", SOURCES_SDCARD_DIR, source_id);
        if (unlink(filepath) == 0) {
            TS_LOGI(TAG, "Deleted source config pack: %s", filepath);
        }
        
        result->code = TS_API_OK;
        result->message = strdup("Source deleted");
    } else if (ret == ESP_ERR_NOT_FOUND) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Source not found");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to delete source");
    }
    
    return ESP_OK;
}

/**
 * @brief automation.sources.add_mapping - Add a field mapping to an existing source
 * 
 * This allows users to select specific JSON fields from the data source
 * to be monitored as independent variables.
 * 
 * Parameters:
 *   - id: Source ID
 *   - json_path: JSONPath expression to extract value (e.g., "cpu.cores[0].usage")
 *   - var_name: Variable name to store the extracted value (e.g., "lpmu.cpu0_usage")
 *   - transform: (optional) Transform expression
 */
static esp_err_t api_automation_sources_add_mapping(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id_param = cJSON_GetObjectItem(params, "id");
    cJSON *json_path = cJSON_GetObjectItem(params, "json_path");
    cJSON *var_name = cJSON_GetObjectItem(params, "var_name");
    
    if (!id_param || !cJSON_IsString(id_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }
    if (!json_path || !cJSON_IsString(json_path)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'json_path' parameter");
        return ESP_OK;
    }
    if (!var_name || !cJSON_IsString(var_name)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'var_name' parameter");
        return ESP_OK;
    }
    
    // 获取数据源（需要可修改指针）
    ts_auto_source_t *source = ts_source_get_mutable(id_param->valuestring);
    if (!source) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Source not found");
        return ESP_OK;
    }
    
    // 检查是否已达到最大映射数
    if (source->mapping_count >= TS_AUTO_MAX_MAPPINGS) {
        result->code = TS_API_ERR_INTERNAL;
        char msg[64];
        snprintf(msg, sizeof(msg), "Maximum mappings (%d) reached", TS_AUTO_MAX_MAPPINGS);
        result->message = strdup(msg);
        return ESP_OK;
    }
    
    // 检查是否已存在相同的 json_path 或 var_name
    for (uint8_t i = 0; i < source->mapping_count; i++) {
        if (strcmp(source->mappings[i].json_path, json_path->valuestring) == 0) {
            result->code = TS_API_ERR_INVALID_ARG;
            result->message = strdup("json_path already exists in mappings");
            return ESP_OK;
        }
        if (strcmp(source->mappings[i].var_name, var_name->valuestring) == 0) {
            result->code = TS_API_ERR_INVALID_ARG;
            result->message = strdup("var_name already exists in mappings");
            return ESP_OK;
        }
    }
    
    // 添加新映射
    ts_auto_mapping_t *new_mapping = &source->mappings[source->mapping_count];
    strncpy(new_mapping->json_path, json_path->valuestring, sizeof(new_mapping->json_path) - 1);
    strncpy(new_mapping->var_name, var_name->valuestring, sizeof(new_mapping->var_name) - 1);
    
    cJSON *transform = cJSON_GetObjectItem(params, "transform");
    if (transform && cJSON_IsString(transform)) {
        strncpy(new_mapping->transform, transform->valuestring, sizeof(new_mapping->transform) - 1);
    }
    
    source->mapping_count++;
    
    result->code = TS_API_OK;
    result->message = strdup("Mapping added successfully");
    result->data = cJSON_CreateObject();
    cJSON_AddStringToObject(result->data, "source_id", id_param->valuestring);
    cJSON_AddStringToObject(result->data, "json_path", json_path->valuestring);
    cJSON_AddStringToObject(result->data, "var_name", var_name->valuestring);
    cJSON_AddNumberToObject(result->data, "mapping_index", source->mapping_count - 1);
    cJSON_AddNumberToObject(result->data, "mapping_count", source->mapping_count);
    
    TS_LOGI(TAG, "Added mapping to source '%s': %s -> %s", 
             id_param->valuestring, json_path->valuestring, var_name->valuestring);
    
    return ESP_OK;
}

/**
 * @brief automation.sources.remove_mapping - Remove a field mapping from a source
 * 
 * Parameters:
 *   - id: Source ID
 *   - var_name: Variable name of the mapping to remove
 *   OR
 *   - index: Index of the mapping to remove (0-based)
 */
static esp_err_t api_automation_sources_remove_mapping(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id_param = cJSON_GetObjectItem(params, "id");
    
    if (!id_param || !cJSON_IsString(id_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }
    
    // 获取数据源（需要可修改指针）
    ts_auto_source_t *source = ts_source_get_mutable(id_param->valuestring);
    if (!source) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Source not found");
        return ESP_OK;
    }
    
    int remove_idx = -1;
    
    // 通过 var_name 查找
    cJSON *var_name = cJSON_GetObjectItem(params, "var_name");
    if (var_name && cJSON_IsString(var_name)) {
        for (uint8_t i = 0; i < source->mapping_count; i++) {
            if (strcmp(source->mappings[i].var_name, var_name->valuestring) == 0) {
                remove_idx = i;
                break;
            }
        }
    }
    
    // 通过 index 查找
    if (remove_idx < 0) {
        cJSON *index = cJSON_GetObjectItem(params, "index");
        if (index && cJSON_IsNumber(index)) {
            int idx = index->valueint;
            if (idx >= 0 && idx < source->mapping_count) {
                remove_idx = idx;
            }
        }
    }
    
    if (remove_idx < 0) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Mapping not found (provide 'var_name' or 'index')");
        return ESP_OK;
    }
    
    // 保存被删除的映射信息用于返回
    char removed_path[128] = {0};
    char removed_var[32] = {0};
    strncpy(removed_path, source->mappings[remove_idx].json_path, sizeof(removed_path) - 1);
    strncpy(removed_var, source->mappings[remove_idx].var_name, sizeof(removed_var) - 1);
    
    // 移动后续映射
    for (int i = remove_idx; i < source->mapping_count - 1; i++) {
        source->mappings[i] = source->mappings[i + 1];
    }
    
    // 清空最后一个位置
    memset(&source->mappings[source->mapping_count - 1], 0, sizeof(ts_auto_mapping_t));
    source->mapping_count--;
    
    result->code = TS_API_OK;
    result->message = strdup("Mapping removed successfully");
    result->data = cJSON_CreateObject();
    cJSON_AddStringToObject(result->data, "source_id", id_param->valuestring);
    cJSON_AddStringToObject(result->data, "removed_json_path", removed_path);
    cJSON_AddStringToObject(result->data, "removed_var_name", removed_var);
    cJSON_AddNumberToObject(result->data, "mapping_count", source->mapping_count);
    
    TS_LOGI(TAG, "Removed mapping from source '%s': %s -> %s", 
             id_param->valuestring, removed_path, removed_var);
    
    return ESP_OK;
}

/**
 * @brief automation.sources.enable - Enable a data source
 */
static esp_err_t api_automation_sources_enable(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id_param = cJSON_GetObjectItem(params, "id");
    if (!id_param || !cJSON_IsString(id_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }

    esp_err_t ret = ts_source_enable(id_param->valuestring);
    
    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Source enabled");
    } else if (ret == ESP_ERR_NOT_FOUND) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Source not found");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to enable source");
    }
    
    return ESP_OK;
}

/**
 * @brief automation.sources.disable - Disable a data source
 */
static esp_err_t api_automation_sources_disable(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id_param = cJSON_GetObjectItem(params, "id");
    if (!id_param || !cJSON_IsString(id_param)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }

    esp_err_t ret = ts_source_disable(id_param->valuestring);
    
    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Source disabled");
    } else if (ret == ESP_ERR_NOT_FOUND) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Source not found");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to disable source");
    }
    
    return ESP_OK;
}

/*===========================================================================*/
/*                           Test Actions API                                 */
/*===========================================================================*/

/**
 * @brief automation.test.led - Test LED action
 */
static esp_err_t api_automation_test_led(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device = cJSON_GetObjectItem(params, "device");
    cJSON *index_j = cJSON_GetObjectItem(params, "index");
    cJSON *r = cJSON_GetObjectItem(params, "r");
    cJSON *g = cJSON_GetObjectItem(params, "g");
    cJSON *b = cJSON_GetObjectItem(params, "b");

    if (!device || !cJSON_IsString(device)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'device' parameter");
        return ESP_OK;
    }

    ts_auto_action_t action = {
        .type = TS_AUTO_ACT_LED,
        .led = {
            .index = index_j && cJSON_IsNumber(index_j) ? (uint8_t)index_j->valueint : 0xFF,
            .r = r && cJSON_IsNumber(r) ? (uint8_t)r->valueint : 0,
            .g = g && cJSON_IsNumber(g) ? (uint8_t)g->valueint : 0,
            .b = b && cJSON_IsNumber(b) ? (uint8_t)b->valueint : 0,
            .duration_ms = 0,
        }
    };
    strncpy(action.led.device, device->valuestring, sizeof(action.led.device) - 1);

    esp_err_t ret = ts_action_execute(&action);

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("LED action executed");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("LED action failed");
    }

    return ESP_OK;
}

/**
 * @brief automation.test.gpio - Test GPIO action
 */
static esp_err_t api_automation_test_gpio(const cJSON *params, ts_api_result_t *result)
{
    cJSON *pin = cJSON_GetObjectItem(params, "pin");
    cJSON *level = cJSON_GetObjectItem(params, "level");
    cJSON *pulse_ms = cJSON_GetObjectItem(params, "pulse_ms");

    if (!pin || !cJSON_IsNumber(pin)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'pin' parameter");
        return ESP_OK;
    }

    ts_auto_action_t action = {
        .type = TS_AUTO_ACT_GPIO,
        .gpio = {
            .pin = (uint8_t)pin->valueint,
            .level = level && cJSON_IsTrue(level),
            .pulse_ms = pulse_ms && cJSON_IsNumber(pulse_ms) ? (uint32_t)pulse_ms->valueint : 0,
        }
    };

    esp_err_t ret = ts_action_execute(&action);

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("GPIO action executed");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("GPIO action failed");
    }

    return ESP_OK;
}

/**
 * @brief automation.test.device - Test device control action
 */
static esp_err_t api_automation_test_device(const cJSON *params, ts_api_result_t *result)
{
    cJSON *device_name = cJSON_GetObjectItem(params, "device");
    cJSON *action_name = cJSON_GetObjectItem(params, "action");

    if (!device_name || !cJSON_IsString(device_name)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'device' parameter");
        return ESP_OK;
    }

    if (!action_name || !cJSON_IsString(action_name)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'action' parameter");
        return ESP_OK;
    }

    ts_auto_action_t action = {
        .type = TS_AUTO_ACT_DEVICE_CTRL,
    };
    strncpy(action.device.device, device_name->valuestring, sizeof(action.device.device) - 1);
    strncpy(action.device.action, action_name->valuestring, sizeof(action.device.action) - 1);

    esp_err_t ret = ts_action_execute(&action);

    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Device action executed");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Device action failed");
    }

    return ESP_OK;
}

/**
 * @brief automation.test.ssh - Test SSH command execution
 */
static esp_err_t api_automation_test_ssh(const cJSON *params, ts_api_result_t *result)
{
    cJSON *host_id = cJSON_GetObjectItem(params, "host_id");
    cJSON *command = cJSON_GetObjectItem(params, "command");
    cJSON *timeout_ms = cJSON_GetObjectItem(params, "timeout_ms");
    
    if (!host_id || !cJSON_IsString(host_id)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'host_id' parameter");
        return ESP_OK;
    }
    
    if (!command || !cJSON_IsString(command)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'command' parameter");
        return ESP_OK;
    }
    
    ts_auto_action_t action = {
        .type = TS_AUTO_ACT_SSH_CMD,
        .ssh = {
            .timeout_ms = (timeout_ms && cJSON_IsNumber(timeout_ms)) ? 
                          (uint32_t)timeout_ms->valueint : 30000,
            .async = false,
        }
    };
    strncpy(action.ssh.host_ref, host_id->valuestring, sizeof(action.ssh.host_ref) - 1);
    strncpy(action.ssh.command, command->valuestring, sizeof(action.ssh.command) - 1);
    
    ts_action_result_t exec_result = {0};
    esp_err_t ret = ts_action_manager_execute(&action, &exec_result);
    
    result->data = cJSON_CreateObject();
    
    if (ret == ESP_OK && exec_result.status == TS_ACTION_STATUS_SUCCESS) {
        result->code = TS_API_OK;
        result->message = strdup("SSH command executed");
        cJSON_AddStringToObject(result->data, "output", exec_result.output);
        cJSON_AddNumberToObject(result->data, "exit_code", exec_result.exit_code);
        cJSON_AddNumberToObject(result->data, "duration_ms", exec_result.duration_ms);
    } else {
        result->code = TS_API_ERR_INTERNAL;
        if (exec_result.status == TS_ACTION_STATUS_TIMEOUT) {
            result->message = strdup("SSH command timed out");
        } else if (exec_result.output[0]) {
            result->message = strdup(exec_result.output);
        } else {
            result->message = strdup("SSH command failed");
        }
        if (exec_result.output[0]) {
            cJSON_AddStringToObject(result->data, "output", exec_result.output);
        }
    }
    
    return ESP_OK;
}

/*===========================================================================*/
/*                        Action Template API                                 */
/*===========================================================================*/

/**
 * @brief Convert action type string to enum
 */
static ts_auto_action_type_t action_type_from_string(const char *str)
{
    if (!str) return TS_AUTO_ACT_LOG;
    
    if (strcasecmp(str, "cli") == 0) return TS_AUTO_ACT_CLI;
    if (strcasecmp(str, "led") == 0) return TS_AUTO_ACT_LED;
    if (strcasecmp(str, "ssh_cmd") == 0) return TS_AUTO_ACT_SSH_CMD;
    if (strcasecmp(str, "ssh_cmd_ref") == 0 || strcasecmp(str, "SSH_CMD_REF") == 0) return TS_AUTO_ACT_SSH_CMD_REF;
    if (strcasecmp(str, "gpio") == 0) return TS_AUTO_ACT_GPIO;
    if (strcasecmp(str, "webhook") == 0) return TS_AUTO_ACT_WEBHOOK;
    if (strcasecmp(str, "log") == 0) return TS_AUTO_ACT_LOG;
    if (strcasecmp(str, "set_var") == 0) return TS_AUTO_ACT_SET_VAR;
    if (strcasecmp(str, "device_ctrl") == 0 || strcasecmp(str, "DEVICE_CTRL") == 0) return TS_AUTO_ACT_DEVICE_CTRL;
    
    return TS_AUTO_ACT_LOG; /* Default */
}

/**
 * @brief Convert action type enum to string
 */
static const char *action_type_to_string(ts_auto_action_type_t type)
{
    switch (type) {
        case TS_AUTO_ACT_CLI: return "cli";
        case TS_AUTO_ACT_LED: return "led";
        case TS_AUTO_ACT_SSH_CMD: return "ssh_cmd";
        case TS_AUTO_ACT_SSH_CMD_REF: return "ssh_cmd_ref";
        case TS_AUTO_ACT_GPIO: return "gpio";
        case TS_AUTO_ACT_WEBHOOK: return "webhook";
        case TS_AUTO_ACT_LOG: return "log";
        case TS_AUTO_ACT_SET_VAR: return "set_var";
        case TS_AUTO_ACT_DEVICE_CTRL: return "device_ctrl";
        default: return "unknown";
    }
}

/**
 * @brief automation.actions.list - List all action templates
 */
static esp_err_t api_automation_actions_list(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    result->data = cJSON_CreateObject();
    cJSON *templates_arr = cJSON_AddArrayToObject(result->data, "templates");
    
    int count = ts_action_template_count();
    if (count > 0) {
        ts_action_template_t *templates = heap_caps_malloc(
            sizeof(ts_action_template_t) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        
        if (templates) {
            size_t actual_count = 0;
            esp_err_t ret = ts_action_template_list(templates, count, &actual_count);
            
            if (ret == ESP_OK) {
                for (size_t i = 0; i < actual_count; i++) {
                    cJSON *tpl_obj = cJSON_CreateObject();
                    cJSON_AddStringToObject(tpl_obj, "id", templates[i].id);
                    cJSON_AddStringToObject(tpl_obj, "name", templates[i].name);
                    cJSON_AddStringToObject(tpl_obj, "description", templates[i].description);
                    cJSON_AddStringToObject(tpl_obj, "type", 
                        action_type_to_string(templates[i].action.type));
                    cJSON_AddBoolToObject(tpl_obj, "enabled", templates[i].enabled);
                    cJSON_AddBoolToObject(tpl_obj, "async", templates[i].async);
                    cJSON_AddNumberToObject(tpl_obj, "use_count", templates[i].use_count);
                    cJSON_AddNumberToObject(tpl_obj, "created_at", templates[i].created_at);
                    cJSON_AddNumberToObject(tpl_obj, "last_used_at", templates[i].last_used_at);
                    cJSON_AddItemToArray(templates_arr, tpl_obj);
                }
            }
            
            free(templates);
        }
    }
    
    cJSON_AddNumberToObject(result->data, "count", count);
    result->code = TS_API_OK;
    return ESP_OK;
}

/**
 * @brief automation.actions.add - Add a new action template
 */
static esp_err_t api_automation_actions_add(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id = cJSON_GetObjectItem(params, "id");
    cJSON *name = cJSON_GetObjectItem(params, "name");
    cJSON *description = cJSON_GetObjectItem(params, "description");
    cJSON *type = cJSON_GetObjectItem(params, "type");
    cJSON *enabled = cJSON_GetObjectItem(params, "enabled");
    
    if (!id || !cJSON_IsString(id) || !id->valuestring[0]) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }
    
    if (!type || !cJSON_IsString(type)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'type' parameter");
        return ESP_OK;
    }
    
    /* Parse async mode at template level */
    cJSON *async_tpl = cJSON_GetObjectItem(params, "async");
    
    ts_action_template_t tpl = {
        .enabled = (enabled && cJSON_IsBool(enabled)) ? cJSON_IsTrue(enabled) : true,
        .async = (async_tpl && cJSON_IsBool(async_tpl)) ? cJSON_IsTrue(async_tpl) : false,
    };
    
    strncpy(tpl.id, id->valuestring, sizeof(tpl.id) - 1);
    if (name && cJSON_IsString(name)) {
        strncpy(tpl.name, name->valuestring, sizeof(tpl.name) - 1);
    } else {
        strncpy(tpl.name, id->valuestring, sizeof(tpl.name) - 1);
    }
    if (description && cJSON_IsString(description)) {
        strncpy(tpl.description, description->valuestring, sizeof(tpl.description) - 1);
    }
    
    tpl.action.type = action_type_from_string(type->valuestring);
    
    /* Parse delay_ms */
    cJSON *delay_ms = cJSON_GetObjectItem(params, "delay_ms");
    if (delay_ms && cJSON_IsNumber(delay_ms)) {
        tpl.action.delay_ms = (uint16_t)delay_ms->valueint;
    }
    
    /* Parse type-specific parameters */
    switch (tpl.action.type) {
        case TS_AUTO_ACT_LED: {
            cJSON *led = cJSON_GetObjectItem(params, "led");
            if (led) {
                cJSON *device = cJSON_GetObjectItem(led, "device");
                cJSON *index = cJSON_GetObjectItem(led, "index");
                cJSON *color = cJSON_GetObjectItem(led, "color");
                cJSON *effect = cJSON_GetObjectItem(led, "effect");
                cJSON *duration = cJSON_GetObjectItem(led, "duration_ms");
                cJSON *ctrl_type = cJSON_GetObjectItem(led, "ctrl_type");
                
                if (device && cJSON_IsString(device)) {
                    strncpy(tpl.action.led.device, device->valuestring, 
                            sizeof(tpl.action.led.device) - 1);
                }
                tpl.action.led.index = (index && cJSON_IsNumber(index)) ? 
                                       (uint8_t)index->valueint : 0xFF;
                if (color && cJSON_IsString(color)) {
                    ts_action_parse_color(color->valuestring, 
                                          &tpl.action.led.r, 
                                          &tpl.action.led.g, 
                                          &tpl.action.led.b);
                }
                if (effect && cJSON_IsString(effect)) {
                    strncpy(tpl.action.led.effect, effect->valuestring, 
                            sizeof(tpl.action.led.effect) - 1);
                }
                tpl.action.led.duration_ms = (duration && cJSON_IsNumber(duration)) ?
                                             (uint16_t)duration->valueint : 0;
                
                /* 解析控制类型 */
                if (ctrl_type && cJSON_IsString(ctrl_type)) {
                    const char *ct = ctrl_type->valuestring;
                    if (strcmp(ct, "fill") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_FILL;
                    else if (strcmp(ct, "effect") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_EFFECT;
                    else if (strcmp(ct, "brightness") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_BRIGHTNESS;
                    else if (strcmp(ct, "off") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_OFF;
                    else if (strcmp(ct, "text") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_TEXT;
                    else if (strcmp(ct, "image") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_IMAGE;
                    else if (strcmp(ct, "qrcode") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_QRCODE;
                    else if (strcmp(ct, "filter") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_FILTER;
                    else if (strcmp(ct, "filter_stop") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_FILTER_STOP;
                    else if (strcmp(ct, "text_stop") == 0) tpl.action.led.ctrl_type = TS_LED_CTRL_TEXT_STOP;
                }
                
                /* 解析 brightness 和 speed */
                cJSON *brightness = cJSON_GetObjectItem(led, "brightness");
                if (brightness && cJSON_IsNumber(brightness)) {
                    tpl.action.led.brightness = (uint8_t)brightness->valueint;
                }
                cJSON *speed = cJSON_GetObjectItem(led, "speed");
                if (speed && cJSON_IsNumber(speed)) {
                    tpl.action.led.speed = (uint8_t)speed->valueint;
                }
                
                /* 解析 Matrix 高级功能参数 */
                cJSON *text = cJSON_GetObjectItem(led, "text");
                if (text && cJSON_IsString(text)) {
                    strncpy(tpl.action.led.text, text->valuestring, 
                            sizeof(tpl.action.led.text) - 1);
                }
                cJSON *font = cJSON_GetObjectItem(led, "font");
                if (font && cJSON_IsString(font)) {
                    strncpy(tpl.action.led.font, font->valuestring, 
                            sizeof(tpl.action.led.font) - 1);
                }
                cJSON *image_path = cJSON_GetObjectItem(led, "image_path");
                if (image_path && cJSON_IsString(image_path)) {
                    strncpy(tpl.action.led.image_path, image_path->valuestring, 
                            sizeof(tpl.action.led.image_path) - 1);
                }
                cJSON *qr_text = cJSON_GetObjectItem(led, "qr_text");
                if (qr_text && cJSON_IsString(qr_text)) {
                    strncpy(tpl.action.led.qr_text, qr_text->valuestring, 
                            sizeof(tpl.action.led.qr_text) - 1);
                }
                cJSON *qr_ecc = cJSON_GetObjectItem(led, "qr_ecc");
                if (qr_ecc && cJSON_IsString(qr_ecc) && qr_ecc->valuestring[0]) {
                    tpl.action.led.qr_ecc = qr_ecc->valuestring[0]; /* L/M/Q/H */
                }
                cJSON *filter = cJSON_GetObjectItem(led, "filter");
                if (filter && cJSON_IsString(filter)) {
                    strncpy(tpl.action.led.filter, filter->valuestring, 
                            sizeof(tpl.action.led.filter) - 1);
                }
                cJSON *center = cJSON_GetObjectItem(led, "center");
                if (center && cJSON_IsBool(center)) {
                    tpl.action.led.center = cJSON_IsTrue(center);
                }
                cJSON *loop = cJSON_GetObjectItem(led, "loop");
                if (loop && cJSON_IsBool(loop)) {
                    tpl.action.led.loop = cJSON_IsTrue(loop);
                }
                cJSON *scroll = cJSON_GetObjectItem(led, "scroll");
                if (scroll && cJSON_IsString(scroll)) {
                    strncpy(tpl.action.led.scroll, scroll->valuestring, 
                            sizeof(tpl.action.led.scroll) - 1);
                }
                cJSON *align = cJSON_GetObjectItem(led, "align");
                if (align && cJSON_IsString(align)) {
                    strncpy(tpl.action.led.align, align->valuestring, 
                            sizeof(tpl.action.led.align) - 1);
                }
                cJSON *x = cJSON_GetObjectItem(led, "x");
                if (x && cJSON_IsNumber(x)) {
                    tpl.action.led.x = (int16_t)x->valueint;
                }
                cJSON *y = cJSON_GetObjectItem(led, "y");
                if (y && cJSON_IsNumber(y)) {
                    tpl.action.led.y = (int16_t)y->valueint;
                }
                
                /* 解析 r/g/b 分离颜色参数 */
                cJSON *r = cJSON_GetObjectItem(led, "r");
                cJSON *g = cJSON_GetObjectItem(led, "g");
                cJSON *b = cJSON_GetObjectItem(led, "b");
                if (r && cJSON_IsNumber(r)) tpl.action.led.r = (uint8_t)r->valueint;
                if (g && cJSON_IsNumber(g)) tpl.action.led.g = (uint8_t)g->valueint;
                if (b && cJSON_IsNumber(b)) tpl.action.led.b = (uint8_t)b->valueint;
            }
            break;
        }
        case TS_AUTO_ACT_SSH_CMD: {
            cJSON *ssh = cJSON_GetObjectItem(params, "ssh");
            if (ssh) {
                cJSON *host_ref = cJSON_GetObjectItem(ssh, "host_ref");
                cJSON *command = cJSON_GetObjectItem(ssh, "command");
                cJSON *timeout = cJSON_GetObjectItem(ssh, "timeout_ms");
                cJSON *async = cJSON_GetObjectItem(ssh, "async");
                
                if (host_ref && cJSON_IsString(host_ref)) {
                    strncpy(tpl.action.ssh.host_ref, host_ref->valuestring, 
                            sizeof(tpl.action.ssh.host_ref) - 1);
                }
                if (command && cJSON_IsString(command)) {
                    strncpy(tpl.action.ssh.command, command->valuestring, 
                            sizeof(tpl.action.ssh.command) - 1);
                }
                tpl.action.ssh.timeout_ms = (timeout && cJSON_IsNumber(timeout)) ?
                                            (uint32_t)timeout->valueint : 30000;
                tpl.action.ssh.async = (async && cJSON_IsBool(async)) ? 
                                       cJSON_IsTrue(async) : false;
            }
            break;
        }
        case TS_AUTO_ACT_GPIO: {
            cJSON *gpio = cJSON_GetObjectItem(params, "gpio");
            if (gpio) {
                cJSON *pin = cJSON_GetObjectItem(gpio, "pin");
                cJSON *level = cJSON_GetObjectItem(gpio, "level");
                cJSON *pulse = cJSON_GetObjectItem(gpio, "pulse_ms");
                
                tpl.action.gpio.pin = (pin && cJSON_IsNumber(pin)) ?
                                      (uint8_t)pin->valueint : 0;
                tpl.action.gpio.level = (level && cJSON_IsBool(level)) ?
                                        cJSON_IsTrue(level) : false;
                tpl.action.gpio.pulse_ms = (pulse && cJSON_IsNumber(pulse)) ?
                                           (uint32_t)pulse->valueint : 0;
            }
            break;
        }
        case TS_AUTO_ACT_DEVICE_CTRL: {
            cJSON *device = cJSON_GetObjectItem(params, "device");
            if (device) {
                cJSON *dev_name = cJSON_GetObjectItem(device, "device");
                cJSON *action = cJSON_GetObjectItem(device, "action");
                
                if (dev_name && cJSON_IsString(dev_name)) {
                    strncpy(tpl.action.device.device, dev_name->valuestring, 
                            sizeof(tpl.action.device.device) - 1);
                }
                if (action && cJSON_IsString(action)) {
                    strncpy(tpl.action.device.action, action->valuestring, 
                            sizeof(tpl.action.device.action) - 1);
                }
            }
            break;
        }
        case TS_AUTO_ACT_LOG: {
            cJSON *log = cJSON_GetObjectItem(params, "log");
            if (log) {
                cJSON *level = cJSON_GetObjectItem(log, "level");
                cJSON *message = cJSON_GetObjectItem(log, "message");
                
                tpl.action.log.level = (level && cJSON_IsNumber(level)) ?
                                       (uint8_t)level->valueint : 3;
                if (message && cJSON_IsString(message)) {
                    strncpy(tpl.action.log.message, message->valuestring, 
                            sizeof(tpl.action.log.message) - 1);
                }
            }
            break;
        }
        case TS_AUTO_ACT_SET_VAR: {
            cJSON *set_var = cJSON_GetObjectItem(params, "set_var");
            if (set_var) {
                cJSON *variable = cJSON_GetObjectItem(set_var, "variable");
                cJSON *value = cJSON_GetObjectItem(set_var, "value");
                
                if (variable && cJSON_IsString(variable)) {
                    strncpy(tpl.action.set_var.variable, variable->valuestring, 
                            sizeof(tpl.action.set_var.variable) - 1);
                }
                if (value && cJSON_IsString(value)) {
                    tpl.action.set_var.value.type = TS_AUTO_VAL_STRING;
                    strncpy(tpl.action.set_var.value.str_val, value->valuestring, 
                            sizeof(tpl.action.set_var.value.str_val) - 1);
                }
            }
            break;
        }
        case TS_AUTO_ACT_WEBHOOK: {
            cJSON *webhook = cJSON_GetObjectItem(params, "webhook");
            if (webhook) {
                cJSON *url = cJSON_GetObjectItem(webhook, "url");
                cJSON *method = cJSON_GetObjectItem(webhook, "method");
                cJSON *body = cJSON_GetObjectItem(webhook, "body_template");
                
                if (url && cJSON_IsString(url)) {
                    strncpy(tpl.action.webhook.url, url->valuestring, 
                            sizeof(tpl.action.webhook.url) - 1);
                }
                if (method && cJSON_IsString(method)) {
                    strncpy(tpl.action.webhook.method, method->valuestring, 
                            sizeof(tpl.action.webhook.method) - 1);
                } else {
                    strcpy(tpl.action.webhook.method, "POST");
                }
                if (body && cJSON_IsString(body)) {
                    strncpy(tpl.action.webhook.body_template, body->valuestring, 
                            sizeof(tpl.action.webhook.body_template) - 1);
                }
            }
            break;
        }
        case TS_AUTO_ACT_SSH_CMD_REF: {
            cJSON *ssh_ref = cJSON_GetObjectItem(params, "ssh_ref");
            if (ssh_ref) {
                cJSON *cmd_id = cJSON_GetObjectItem(ssh_ref, "cmd_id");
                if (cmd_id && cJSON_IsString(cmd_id)) {
                    strncpy(tpl.action.ssh_ref.cmd_id, cmd_id->valuestring, 
                            sizeof(tpl.action.ssh_ref.cmd_id) - 1);
                }
            }
            break;
        }
        case TS_AUTO_ACT_CLI: {
            cJSON *cli = cJSON_GetObjectItem(params, "cli");
            if (cli) {
                cJSON *command = cJSON_GetObjectItem(cli, "command");
                cJSON *var_name = cJSON_GetObjectItem(cli, "var_name");
                cJSON *timeout = cJSON_GetObjectItem(cli, "timeout_ms");
                if (command && cJSON_IsString(command)) {
                    strncpy(tpl.action.cli.command, command->valuestring, 
                            sizeof(tpl.action.cli.command) - 1);
                }
                if (var_name && cJSON_IsString(var_name)) {
                    strncpy(tpl.action.cli.var_name, var_name->valuestring, 
                            sizeof(tpl.action.cli.var_name) - 1);
                }
                tpl.action.cli.timeout_ms = timeout && cJSON_IsNumber(timeout) ? 
                                             timeout->valueint : 5000;
            }
            break;
        }
        default:
            break;
    }
    
    esp_err_t ret = ts_action_template_add(&tpl);
    
    if (ret == ESP_OK) {
        result->code = TS_API_OK;
        result->message = strdup("Action template created");
    } else if (ret == ESP_ERR_NO_MEM) {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Max templates reached");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Template ID already exists");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to create template");
    }
    
    return ESP_OK;
}

/**
 * @brief automation.actions.get - Get a single action template by ID
 */
static esp_err_t api_automation_actions_get(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id = cJSON_GetObjectItem(params, "id");
    
    if (!id || !cJSON_IsString(id)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }
    
    ts_action_template_t tpl;
    esp_err_t ret = ts_action_template_get(id->valuestring, &tpl);
    
    if (ret != ESP_OK) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Template not found");
        return ESP_OK;
    }
    
    result->data = cJSON_CreateObject();
    cJSON_AddStringToObject(result->data, "id", tpl.id);
    cJSON_AddStringToObject(result->data, "name", tpl.name);
    cJSON_AddStringToObject(result->data, "description", tpl.description);
    cJSON_AddStringToObject(result->data, "type", action_type_to_string(tpl.action.type));
    cJSON_AddBoolToObject(result->data, "enabled", tpl.enabled);
    cJSON_AddNumberToObject(result->data, "delay_ms", tpl.action.delay_ms);
    
    /* Add type-specific data */
    switch (tpl.action.type) {
        case TS_AUTO_ACT_CLI: {
            cJSON *cli = cJSON_AddObjectToObject(result->data, "cli");
            cJSON_AddStringToObject(cli, "command", tpl.action.cli.command);
            cJSON_AddStringToObject(cli, "var_name", tpl.action.cli.var_name);
            cJSON_AddNumberToObject(cli, "timeout_ms", tpl.action.cli.timeout_ms);
            break;
        }
        case TS_AUTO_ACT_SSH_CMD_REF: {
            cJSON *ssh_ref = cJSON_AddObjectToObject(result->data, "ssh_ref");
            cJSON_AddStringToObject(ssh_ref, "cmd_id", tpl.action.ssh_ref.cmd_id);
            break;
        }
        case TS_AUTO_ACT_LED: {
            cJSON *led = cJSON_AddObjectToObject(result->data, "led");
            cJSON_AddStringToObject(led, "device", tpl.action.led.device);
            /* 控制类型 */
            const char *ctrl_type_str = "fill";
            switch (tpl.action.led.ctrl_type) {
                case TS_LED_CTRL_EFFECT: ctrl_type_str = "effect"; break;
                case TS_LED_CTRL_BRIGHTNESS: ctrl_type_str = "brightness"; break;
                case TS_LED_CTRL_OFF: ctrl_type_str = "off"; break;
                case TS_LED_CTRL_TEXT: ctrl_type_str = "text"; break;
                case TS_LED_CTRL_IMAGE: ctrl_type_str = "image"; break;
                case TS_LED_CTRL_QRCODE: ctrl_type_str = "qrcode"; break;
                case TS_LED_CTRL_FILTER: ctrl_type_str = "filter"; break;
                case TS_LED_CTRL_FILTER_STOP: ctrl_type_str = "filter_stop"; break;
                case TS_LED_CTRL_TEXT_STOP: ctrl_type_str = "text_stop"; break;
                default: ctrl_type_str = "fill"; break;
            }
            cJSON_AddStringToObject(led, "ctrl_type", ctrl_type_str);
            cJSON_AddNumberToObject(led, "index", tpl.action.led.index);
            char color_str[8];
            snprintf(color_str, sizeof(color_str), "#%02X%02X%02X", 
                     tpl.action.led.r, tpl.action.led.g, tpl.action.led.b);
            cJSON_AddStringToObject(led, "color", color_str);
            cJSON_AddNumberToObject(led, "brightness", tpl.action.led.brightness);
            if (tpl.action.led.effect[0]) {
                cJSON_AddStringToObject(led, "effect", tpl.action.led.effect);
            }
            cJSON_AddNumberToObject(led, "speed", tpl.action.led.speed);
            cJSON_AddNumberToObject(led, "duration_ms", tpl.action.led.duration_ms);
            /* Matrix 高级功能 */
            if (tpl.action.led.text[0]) {
                cJSON_AddStringToObject(led, "text", tpl.action.led.text);
            }
            if (tpl.action.led.font[0]) {
                cJSON_AddStringToObject(led, "font", tpl.action.led.font);
            }
            if (tpl.action.led.image_path[0]) {
                cJSON_AddStringToObject(led, "image_path", tpl.action.led.image_path);
            }
            if (tpl.action.led.qr_text[0]) {
                cJSON_AddStringToObject(led, "qr_text", tpl.action.led.qr_text);
            }
            if (tpl.action.led.qr_ecc) {
                char ecc_str[2] = { tpl.action.led.qr_ecc, '\0' };
                cJSON_AddStringToObject(led, "qr_ecc", ecc_str);
            }
            if (tpl.action.led.filter[0]) {
                cJSON_AddStringToObject(led, "filter", tpl.action.led.filter);
            }
            cJSON_AddBoolToObject(led, "center", tpl.action.led.center);
            cJSON_AddBoolToObject(led, "loop", tpl.action.led.loop);
            if (tpl.action.led.scroll[0]) {
                cJSON_AddStringToObject(led, "scroll", tpl.action.led.scroll);
            }
            if (tpl.action.led.align[0]) {
                cJSON_AddStringToObject(led, "align", tpl.action.led.align);
            }
            cJSON_AddNumberToObject(led, "x", tpl.action.led.x);
            cJSON_AddNumberToObject(led, "y", tpl.action.led.y);
            break;
        }
        case TS_AUTO_ACT_LOG: {
            cJSON *log_obj = cJSON_AddObjectToObject(result->data, "log");
            cJSON_AddNumberToObject(log_obj, "level", tpl.action.log.level);
            cJSON_AddStringToObject(log_obj, "message", tpl.action.log.message);
            break;
        }
        case TS_AUTO_ACT_SET_VAR: {
            cJSON *set_var = cJSON_AddObjectToObject(result->data, "set_var");
            cJSON_AddStringToObject(set_var, "variable", tpl.action.set_var.variable);
            if (tpl.action.set_var.value.type == TS_AUTO_VAL_STRING) {
                cJSON_AddStringToObject(set_var, "value", tpl.action.set_var.value.str_val);
            }
            break;
        }
        case TS_AUTO_ACT_WEBHOOK: {
            cJSON *webhook = cJSON_AddObjectToObject(result->data, "webhook");
            cJSON_AddStringToObject(webhook, "url", tpl.action.webhook.url);
            cJSON_AddStringToObject(webhook, "method", tpl.action.webhook.method);
            cJSON_AddStringToObject(webhook, "body_template", tpl.action.webhook.body_template);
            break;
        }
        default:
            break;
    }
    
    result->code = TS_API_OK;
    return ESP_OK;
}

/**
 * @brief automation.actions.delete - Delete an action template
 */
static esp_err_t api_automation_actions_delete(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id = cJSON_GetObjectItem(params, "id");
    
    if (!id || !cJSON_IsString(id)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }
    
    const char *action_id = id->valuestring;
    esp_err_t ret = ts_action_template_remove(action_id);
    
    if (ret == ESP_OK) {
        // 同时删除 SD 卡上的配置文件（.json 和 .tscfg）
        char filepath[128];
        snprintf(filepath, sizeof(filepath), "%s/%s.json", ACTIONS_SDCARD_DIR, action_id);
        if (unlink(filepath) == 0) {
            TS_LOGI(TAG, "Deleted action config: %s", filepath);
        }
        snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", ACTIONS_SDCARD_DIR, action_id);
        if (unlink(filepath) == 0) {
            TS_LOGI(TAG, "Deleted action config pack: %s", filepath);
        }
        
        result->code = TS_API_OK;
        result->message = strdup("Action template deleted");
    } else if (ret == ESP_ERR_NOT_FOUND) {
        result->code = TS_API_ERR_NOT_FOUND;
        result->message = strdup("Template not found");
    } else {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to delete template");
    }
    
    return ESP_OK;
}

/**
 * @brief automation.actions.execute - Execute an action template
 */
static esp_err_t api_automation_actions_execute(const cJSON *params, ts_api_result_t *result)
{
    cJSON *id = cJSON_GetObjectItem(params, "id");
    
    if (!id || !cJSON_IsString(id)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'id' parameter");
        return ESP_OK;
    }
    
    TS_LOGI(TAG, "Executing action template: %s", id->valuestring);
    
    ts_action_result_t exec_result = {0};
    esp_err_t ret = ts_action_template_execute(id->valuestring, &exec_result);
    
    TS_LOGI(TAG, "Execute result: ret=%s, status=%d, output=%s", 
            esp_err_to_name(ret), exec_result.status, exec_result.output ? exec_result.output : "(null)");
    
    result->data = cJSON_CreateObject();
    
    /* Handle async (queued) execution */
    if (ret == ESP_OK && exec_result.status == TS_ACTION_STATUS_QUEUED) {
        result->code = TS_API_OK;
        result->message = strdup("Action queued for async execution");
        cJSON_AddBoolToObject(result->data, "async", true);
        cJSON_AddStringToObject(result->data, "status", "queued");
    } else if (ret == ESP_OK && exec_result.status == TS_ACTION_STATUS_SUCCESS) {
        result->code = TS_API_OK;
        result->message = strdup("Action executed successfully");
        cJSON_AddBoolToObject(result->data, "async", false);
        cJSON_AddNumberToObject(result->data, "duration_ms", exec_result.duration_ms);
        if (exec_result.output[0]) {
            cJSON_AddStringToObject(result->data, "output", exec_result.output);
        }
    } else {
        result->code = TS_API_ERR_INTERNAL;
        if (exec_result.output[0]) {
            result->message = strdup(exec_result.output);
            cJSON_AddStringToObject(result->data, "output", exec_result.output);
        } else {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg), "Action failed: %s (status=%d)", 
                     esp_err_to_name(ret), exec_result.status);
            result->message = strdup(err_msg);
        }
    }
    
    return ESP_OK;
}
/*===========================================================================*/
/*                        Action Statistics API                               */
/*===========================================================================*/

/**
 * @brief automation.action.stats - Get action execution statistics
 */
static esp_err_t api_automation_action_stats(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    ts_action_stats_t stats;
    esp_err_t ret = ts_action_get_stats(&stats);
    
    if (ret != ESP_OK) {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to get action stats");
        return ESP_OK;
    }
    
    result->data = cJSON_CreateObject();
    cJSON_AddNumberToObject(result->data, "total_executed", stats.total_executed);
    cJSON_AddNumberToObject(result->data, "success_count", stats.total_success);
    cJSON_AddNumberToObject(result->data, "failed_count", stats.total_failed);
    cJSON_AddNumberToObject(result->data, "timeout_count", stats.total_timeout);
    cJSON_AddNumberToObject(result->data, "queue_pending", stats.queue_high_water);  // 使用 high water mark
    cJSON_AddBoolToObject(result->data, "queue_running", false);  // TODO: 添加队列运行状态追踪
    
    result->code = TS_API_OK;
    return ESP_OK;
}

/**
 * @brief automation.action.stats.reset - Reset action statistics
 */
static esp_err_t api_automation_action_stats_reset(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    ts_action_reset_stats();
    
    result->code = TS_API_OK;
    result->message = strdup("Action statistics reset");
    
    return ESP_OK;
}

/*===========================================================================*/
/*                     Proxy APIs for External Connection Test               */
/*===========================================================================*/

#include "esp_http_client.h"
#include "esp_websocket_client.h"

/**
 * @brief automation.proxy.fetch - Fetch data from external REST API
 * 
 * Used by WebUI to test API connections before creating data sources.
 * Bypasses browser CORS restrictions by proxying through ESP32.
 * 
 * Special handling for local API calls (127.0.0.1 or self IP) - calls API directly.
 */
static esp_err_t api_automation_proxy_fetch(const cJSON *params, ts_api_result_t *result)
{
    cJSON *url_json = cJSON_GetObjectItem(params, "url");
    cJSON *method_json = cJSON_GetObjectItem(params, "method");
    cJSON *headers_json = cJSON_GetObjectItem(params, "headers");
    
    if (!url_json || !cJSON_IsString(url_json)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'url' parameter");
        return ESP_OK;
    }
    
    const char *url = url_json->valuestring;
    const char *method_str = (method_json && cJSON_IsString(method_json)) ? method_json->valuestring : "GET";
    cJSON *timeout_json = cJSON_GetObjectItem(params, "timeout");
    int timeout_ms = (timeout_json && cJSON_IsNumber(timeout_json)) ? timeout_json->valueint : 15000;
    
    TS_LOGI(TAG, "Proxy fetch: %s %s (timeout=%dms)", method_str, url, timeout_ms);
    
    // 检测是否是本地 API 调用 (127.0.0.1 或 localhost)
    // 格式: http://127.0.0.1/api/v1/xxx 或 http://localhost/api/v1/xxx
    bool is_local = (strstr(url, "://127.0.0.1") != NULL || 
                     strstr(url, "://localhost") != NULL);
    
    if (is_local) {
        // 提取 API 路径: /api/v1/system/memory -> system.memory
        const char *api_path = strstr(url, "/api/v1/");
        if (api_path) {
            api_path += 8;  // 跳过 "/api/v1/"
            
            // 将路径转换为 API 名称: system/memory -> system.memory
            char api_name[64] = {0};
            size_t i = 0, j = 0;
            while (api_path[i] && j < sizeof(api_name) - 1) {
                if (api_path[i] == '/') {
                    api_name[j++] = '.';
                } else if (api_path[i] == '?') {
                    break;  // 停止于查询参数
                } else {
                    api_name[j++] = api_path[i];
                }
                i++;
            }
            api_name[j] = '\0';
            
            TS_LOGI(TAG, "Local API call detected, calling directly: %s", api_name);
            
            // 直接调用本地 API
            ts_api_result_t local_result = {0};
            cJSON *api_params = cJSON_CreateObject();  // 空参数，实际应解析 query string
            
            esp_err_t ret = ts_api_call(api_name, api_params, &local_result);
            cJSON_Delete(api_params);
            
            if (ret == ESP_OK && local_result.code == TS_API_OK) {
                cJSON *data = cJSON_CreateObject();
                cJSON_AddNumberToObject(data, "status", 200);
                cJSON_AddNumberToObject(data, "content_length", 0);
                
                // 包装本地 API 响应
                cJSON *body = cJSON_CreateObject();
                cJSON_AddNumberToObject(body, "code", local_result.code);
                if (local_result.message) {
                    cJSON_AddStringToObject(body, "message", local_result.message);
                }
                if (local_result.data) {
                    cJSON_AddItemToObject(body, "data", cJSON_Duplicate(local_result.data, true));
                }
                cJSON_AddItemToObject(data, "body", body);
                
                result->code = TS_API_OK;
                result->message = strdup("Local API call successful");
                result->data = data;
            } else {
                result->code = local_result.code ? local_result.code : TS_API_ERR_INTERNAL;
                result->message = local_result.message ? strdup(local_result.message) : strdup("Local API call failed");
            }
            
            // 清理本地结果
            if (local_result.message) free(local_result.message);
            if (local_result.data) cJSON_Delete(local_result.data);
            
            return ESP_OK;
        }
    }
    
    // 外部 HTTP 请求
    esp_http_client_method_t method = HTTP_METHOD_GET;
    if (strcasecmp(method_str, "POST") == 0) method = HTTP_METHOD_POST;
    else if (strcasecmp(method_str, "PUT") == 0) method = HTTP_METHOD_PUT;
    
    // 配置 HTTP 客户端（支持 HTTP 和 HTTPS）
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .skip_cert_common_name_check = true,  // 跳过 CN 检查（测试用）
        .crt_bundle_attach = esp_crt_bundle_attach,  // 使用证书包验证 HTTPS
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to init HTTP client");
        return ESP_OK;
    }
    
    // 添加自定义头
    if (headers_json && cJSON_IsObject(headers_json)) {
        cJSON *header = NULL;
        cJSON_ArrayForEach(header, headers_json) {
            if (cJSON_IsString(header)) {
                esp_http_client_set_header(client, header->string, header->valuestring);
            }
        }
    }
    
    // 执行请求
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        result->code = TS_API_ERR_INTERNAL;
        char msg[128];
        snprintf(msg, sizeof(msg), "HTTP connection failed: %s", esp_err_to_name(err));
        result->message = strdup(msg);
        return ESP_OK;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    
    // 读取响应体（限制大小防止内存溢出）
    int max_len = 8192;
    char *body = heap_caps_malloc(max_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc(max_len + 1);
    }
    
    int read_len = 0;
    if (body) {
        read_len = esp_http_client_read(client, body, max_len);
        if (read_len >= 0) {
            body[read_len] = '\0';
        } else {
            body[0] = '\0';
        }
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    // 构建响应
    if (status_code >= 200 && status_code < 400) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "status", status_code);
        cJSON_AddNumberToObject(data, "content_length", content_length);
        
        // 尝试解析为 JSON，如果失败则作为字符串返回
        cJSON *body_json = cJSON_Parse(body);
        if (body_json) {
            cJSON_AddItemToObject(data, "body", body_json);
        } else {
            cJSON_AddStringToObject(data, "body", body ? body : "");
        }
        
        result->code = TS_API_OK;
        result->message = strdup("Request successful");
        result->data = data;
    } else {
        result->code = TS_API_ERR_INTERNAL;
        char msg[256];
        snprintf(msg, sizeof(msg), "HTTP %d: %s", status_code, body ? body : "(empty)");
        result->message = strdup(msg);
    }
    
    if (body) free(body);
    return ESP_OK;
}

/**
 * 递归收集 JSON 中的所有可选路径
 * @param json JSON 对象
 * @param prefix 当前路径前缀
 * @param paths_array 输出的路径数组
 * @param max_depth 最大递归深度（防止无限递归）
 */
static void collect_json_paths(cJSON *json, const char *prefix, cJSON *paths_array, int max_depth)
{
    if (!json || !paths_array || max_depth <= 0) return;
    
    char path_buf[256];
    
    if (cJSON_IsObject(json)) {
        cJSON *item;
        cJSON_ArrayForEach(item, json) {
            if (prefix && prefix[0]) {
                snprintf(path_buf, sizeof(path_buf), "%s.%s", prefix, item->string);
            } else {
                snprintf(path_buf, sizeof(path_buf), "%s", item->string);
            }
            
            // 添加当前路径和类型信息
            cJSON *path_info = cJSON_CreateObject();
            cJSON_AddStringToObject(path_info, "path", path_buf);
            
            if (cJSON_IsBool(item)) {
                cJSON_AddStringToObject(path_info, "type", "bool");
                cJSON_AddBoolToObject(path_info, "value", cJSON_IsTrue(item));
            } else if (cJSON_IsNumber(item)) {
                cJSON_AddStringToObject(path_info, "type", "number");
                cJSON_AddNumberToObject(path_info, "value", item->valuedouble);
            } else if (cJSON_IsString(item)) {
                cJSON_AddStringToObject(path_info, "type", "string");
                // 截断长字符串
                if (strlen(item->valuestring) > 50) {
                    char truncated[64];
                    snprintf(truncated, sizeof(truncated), "%.50s...", item->valuestring);
                    cJSON_AddStringToObject(path_info, "value", truncated);
                } else {
                    cJSON_AddStringToObject(path_info, "value", item->valuestring);
                }
            } else if (cJSON_IsArray(item)) {
                cJSON_AddStringToObject(path_info, "type", "array");
                cJSON_AddNumberToObject(path_info, "length", cJSON_GetArraySize(item));
            } else if (cJSON_IsObject(item)) {
                cJSON_AddStringToObject(path_info, "type", "object");
            } else {
                cJSON_AddStringToObject(path_info, "type", "null");
            }
            
            cJSON_AddItemToArray(paths_array, path_info);
            
            // 递归处理子对象和数组
            if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
                collect_json_paths(item, path_buf, paths_array, max_depth - 1);
            }
        }
    } else if (cJSON_IsArray(json)) {
        // 对数组只处理第一个元素作为示例
        int arr_size = cJSON_GetArraySize(json);
        if (arr_size > 0) {
            cJSON *first = cJSON_GetArrayItem(json, 0);
            snprintf(path_buf, sizeof(path_buf), "%s[0]", prefix);
            
            // 添加数组元素路径
            cJSON *path_info = cJSON_CreateObject();
            cJSON_AddStringToObject(path_info, "path", path_buf);
            
            if (cJSON_IsBool(first)) {
                cJSON_AddStringToObject(path_info, "type", "bool");
            } else if (cJSON_IsNumber(first)) {
                cJSON_AddStringToObject(path_info, "type", "number");
                cJSON_AddNumberToObject(path_info, "value", first->valuedouble);
            } else if (cJSON_IsString(first)) {
                cJSON_AddStringToObject(path_info, "type", "string");
            } else if (cJSON_IsObject(first)) {
                cJSON_AddStringToObject(path_info, "type", "object");
            } else if (cJSON_IsArray(first)) {
                cJSON_AddStringToObject(path_info, "type", "array");
            }
            cJSON_AddNumberToObject(path_info, "array_size", arr_size);
            cJSON_AddItemToArray(paths_array, path_info);
            
            // 递归处理数组元素
            if (cJSON_IsObject(first) || cJSON_IsArray(first)) {
                collect_json_paths(first, path_buf, paths_array, max_depth - 1);
            }
        }
    }
}

/**
 * 从 JSON 中按路径提取子对象（用于 WebSocket 测试）
 * @param json JSON 对象
 * @param path 路径字符串 (如 "data.cpu.usage", "items[0].name")
 * @return 提取的 cJSON 对象（不复制），失败返回 NULL
 */
static cJSON *extract_json_value(cJSON *json, const char *path)
{
    if (!json || !path || !path[0]) return json;
    
    // 跳过开头的 "$."
    const char *p = path;
    if (p[0] == '$' && p[1] == '.') p += 2;
    
    cJSON *current = json;
    char token[64];
    
    while (*p && current) {
        if (*p == '.' || *p == '/') { p++; continue; }
        
        // 处理数组索引 [n]
        if (*p == '[') {
            p++;
            int index = 0;
            while (*p >= '0' && *p <= '9') index = index * 10 + (*p++ - '0');
            if (*p == ']') p++;
            if (cJSON_IsArray(current)) {
                current = cJSON_GetArrayItem(current, index);
            } else {
                return NULL;
            }
            continue;
        }
        
        // 提取字段名
        size_t i = 0;
        while (*p && *p != '.' && *p != '[' && *p != '/' && i < sizeof(token) - 1) {
            token[i++] = *p++;
        }
        token[i] = '\0';
        if (i == 0) continue;
        
        // 检查纯数字（数组索引简写）
        bool is_numeric = true;
        for (size_t j = 0; j < i; j++) {
            if (token[j] < '0' || token[j] > '9') { is_numeric = false; break; }
        }
        
        if (is_numeric && cJSON_IsArray(current)) {
            current = cJSON_GetArrayItem(current, atoi(token));
        } else {
            current = cJSON_GetObjectItem(current, token);
        }
    }
    
    return current;
}

// WebSocket 测试状态
typedef struct {
    char *message;                  // 接收到的消息
    char *error;                    // 错误信息
    bool connected;                 // 已连接标志
    bool received;                  // 已收到数据标志
    SemaphoreHandle_t connect_sem;  // 连接完成信号量
    SemaphoreHandle_t data_sem;     // 数据接收信号量
} ws_test_ctx_t;

static void ws_test_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ws_test_ctx_t *ctx = (ws_test_ctx_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            TS_LOGI(TAG, "WS test: connected");
            ctx->connected = true;
            if (ctx->connect_sem) xSemaphoreGive(ctx->connect_sem);
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            TS_LOGI(TAG, "WS test: disconnected");
            ctx->connected = false;
            // 如果未收到数据就断开，释放信号量避免永久等待
            if (!ctx->received && ctx->data_sem) {
                xSemaphoreGive(ctx->data_sem);
            }
            break;
            
        case WEBSOCKET_EVENT_DATA:
            if (data->data_len > 0 && !ctx->received) {
                TS_LOGI(TAG, "WS test: received %d bytes", data->data_len);
                ctx->message = strndup(data->data_ptr, data->data_len);
                ctx->received = true;
                if (ctx->data_sem) xSemaphoreGive(ctx->data_sem);
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            TS_LOGE(TAG, "WS test: error");
            ctx->error = strdup("WebSocket error occurred");
            // 释放所有信号量
            if (ctx->connect_sem) xSemaphoreGive(ctx->connect_sem);
            if (ctx->data_sem) xSemaphoreGive(ctx->data_sem);
            break;
            
        default:
            break;
    }
}

/**
 * @brief automation.proxy.websocket_test - Test WebSocket connection
 *
 * Connects to WebSocket, optionally sends subscribe message, waits for data.
 * 
 * Parameters:
 *   - uri: WebSocket URI (ws://host:port/path or wss://...)
 *   - timeout_ms: Connection and data timeout (default 10000)
 *   - subscribe: Optional JSON message to send after connect (for subscription)
 *   - json_path: Optional JSON path to extract from received message
 * 
 * Returns:
 *   - connected: Whether connection was established
 *   - message: Raw message or extracted value
 *   - raw: Full raw message (if json_path is specified)
 */
static esp_err_t api_automation_proxy_ws_test(const cJSON *params, ts_api_result_t *result)
{
    cJSON *uri_json = cJSON_GetObjectItem(params, "uri");
    cJSON *timeout_json = cJSON_GetObjectItem(params, "timeout_ms");
    cJSON *subscribe_json = cJSON_GetObjectItem(params, "subscribe");
    cJSON *json_path_json = cJSON_GetObjectItem(params, "json_path");
    
    if (!uri_json || !cJSON_IsString(uri_json)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'uri' parameter");
        return ESP_OK;
    }
    
    const char *uri = uri_json->valuestring;
    int timeout_ms = (timeout_json && cJSON_IsNumber(timeout_json)) ? timeout_json->valueint : 10000;
    const char *subscribe_msg = (subscribe_json && cJSON_IsString(subscribe_json)) ? subscribe_json->valuestring : NULL;
    const char *json_path = (json_path_json && cJSON_IsString(json_path_json)) ? json_path_json->valuestring : NULL;
    
    TS_LOGI(TAG, "WS test: uri=%s timeout=%d", uri, timeout_ms);
    
    ws_test_ctx_t ctx = {
        .message = NULL,
        .error = NULL,
        .connected = false,
        .received = false,
        .connect_sem = xSemaphoreCreateBinary(),
        .data_sem = xSemaphoreCreateBinary(),
    };
    
    if (!ctx.connect_sem || !ctx.data_sem) {
        if (ctx.connect_sem) vSemaphoreDelete(ctx.connect_sem);
        if (ctx.data_sem) vSemaphoreDelete(ctx.data_sem);
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to create semaphores");
        return ESP_OK;
    }
    
    // 配置 WebSocket 客户端
    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .buffer_size = 4096,
        .network_timeout_ms = timeout_ms,
        .reconnect_timeout_ms = timeout_ms,  // 禁用自动重连（使用大超时）
    };
    
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        vSemaphoreDelete(ctx.connect_sem);
        vSemaphoreDelete(ctx.data_sem);
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to init WebSocket client");
        return ESP_OK;
    }
    
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_test_handler, &ctx);
    
    // 启动连接
    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(client);
        vSemaphoreDelete(ctx.connect_sem);
        vSemaphoreDelete(ctx.data_sem);
        result->code = TS_API_ERR_INTERNAL;
        char msg[128];
        snprintf(msg, sizeof(msg), "WebSocket start failed: %s", esp_err_to_name(err));
        result->message = strdup(msg);
        return ESP_OK;
    }
    
    // 等待连接建立
    bool connect_ok = (xSemaphoreTake(ctx.connect_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) && ctx.connected && !ctx.error;
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "connected", connect_ok);
    
    if (!connect_ok) {
        // 连接失败
        result->code = TS_API_ERR_INTERNAL;
        if (ctx.error) {
            result->message = ctx.error;
            ctx.error = NULL;
        } else {
            result->message = strdup("WebSocket connection timeout");
        }
        cJSON_AddStringToObject(data, "error", result->message);
        result->data = data;
        
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        vSemaphoreDelete(ctx.connect_sem);
        vSemaphoreDelete(ctx.data_sem);
        return ESP_OK;
    }
    
    // 连接成功，发送订阅消息（如果有）
    if (subscribe_msg) {
        TS_LOGI(TAG, "WS test: sending subscribe message");
        esp_websocket_client_send_text(client, subscribe_msg, strlen(subscribe_msg), pdMS_TO_TICKS(5000));
    }
    
    // 等待数据消息
    bool data_ok = (xSemaphoreTake(ctx.data_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) && ctx.message;
    
    if (data_ok && ctx.message) {
        TS_LOGI(TAG, "WS test: message received, len=%d", (int)strlen(ctx.message));
        
        // 尝试解析为 JSON
        cJSON *msg_json = cJSON_Parse(ctx.message);
        
        if (msg_json) {
            // 如果指定了 json_path，提取值
            if (json_path && strlen(json_path) > 0) {
                cJSON *extracted = extract_json_value(msg_json, json_path);
                if (extracted) {
                    cJSON_AddItemToObject(data, "value", cJSON_Duplicate(extracted, true));
                    cJSON_AddStringToObject(data, "path", json_path);
                } else {
                    cJSON_AddNullToObject(data, "value");
                    cJSON_AddStringToObject(data, "path_error", "Path not found in response");
                }
                // 同时返回完整消息
                cJSON_AddItemToObject(data, "message", msg_json);
            } else {
                cJSON_AddItemToObject(data, "message", msg_json);
            }
        } else {
            // 非 JSON 消息，返回原始文本
            cJSON_AddStringToObject(data, "message", ctx.message);
        }
        
        result->code = TS_API_OK;
        result->message = strdup("WebSocket test successful");
        
        free(ctx.message);
    } else {
        // 未收到数据
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("WebSocket connected but no data received");
        cJSON_AddStringToObject(data, "error", "No data received within timeout");
    }
    
    result->data = data;
    
    // 清理
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    vSemaphoreDelete(ctx.connect_sem);
    vSemaphoreDelete(ctx.data_sem);
    if (ctx.error) free(ctx.error);
    
    return ESP_OK;
}

/*===========================================================================*/
/*                        Socket.IO Client Support                            */
/*===========================================================================*/

// Socket.IO 测试上下文
typedef struct {
    char *sid;                      // Session ID
    char *message;                  // 接收到的事件数据
    char *event_name;               // 接收到的事件名
    char *error;                    // 错误信息
    bool connected;                 // WebSocket 已连接
    bool upgraded;                  // 已完成协议升级
    bool received;                  // 已收到事件数据
    SemaphoreHandle_t connect_sem;  // 连接完成信号量
    SemaphoreHandle_t upgrade_sem;  // 升级完成信号量
    SemaphoreHandle_t data_sem;     // 数据接收信号量
    const char *target_event;       // 目标事件名（可选过滤）
    // 消息分片缓冲区（大消息可能分多个帧发送）
    char *fragment_buf;             // 分片缓冲区
    size_t fragment_len;            // 当前分片长度
    size_t fragment_cap;            // 缓冲区容量
} sio_test_ctx_t;

/**
 * @brief 解析 Socket.IO 消息
 * 
 * Socket.IO Engine.IO 协议消息类型：
 * - 0: open (包含 sid)
 * - 2: ping
 * - 3: pong / probe response
 * - 4: message (Socket.IO 层)
 *   - 40: connect
 *   - 42: event ["event_name", data]
 *   - 43: ack
 */
/**
 * @brief 尝试解析完整的 42 事件消息
 */
static bool sio_try_parse_event(sio_test_ctx_t *ctx, const char *data, size_t len)
{
    // 查找 JSON 数组开始
    const char *json_start = NULL;
    for (size_t i = 0; i < len - 1; i++) {
        if (data[i] == '[') {
            json_start = &data[i];
            break;
        }
    }
    if (!json_start) return false;
    
    cJSON *arr = cJSON_Parse(json_start);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return false;  // JSON 不完整，继续等待分片
    }
    
    cJSON *event = cJSON_GetArrayItem(arr, 0);
    cJSON *payload = cJSON_GetArrayItem(arr, 1);
    
    if (event && cJSON_IsString(event)) {
        const char *event_str = event->valuestring;
        
        // 检查是否是目标事件
        bool match = (!ctx->target_event || strlen(ctx->target_event) == 0 ||
                      strcmp(ctx->target_event, event_str) == 0);
        
        if (match && !ctx->received) {
            TS_LOGI(TAG, "Socket.IO event matched: %s", event_str);
            
            if (ctx->event_name) free(ctx->event_name);
            ctx->event_name = strdup(event_str);
            
            if (payload) {
                char *payload_str = cJSON_PrintUnformatted(payload);
                if (payload_str) {
                    if (ctx->message) free(ctx->message);
                    ctx->message = payload_str;
                    TS_LOGI(TAG, "Socket.IO payload size: %zu bytes", strlen(payload_str));
                }
            }
            
            ctx->received = true;
            if (ctx->data_sem) xSemaphoreGive(ctx->data_sem);
        }
    }
    
    cJSON_Delete(arr);
    return true;  // 解析成功
}

static void sio_parse_message(sio_test_ctx_t *ctx, const char *data, int len)
{
    if (!data || len < 1) return;
    
    char type = data[0];
    
    // 检查是否是分片续传（不以协议类型数字开头）
    bool is_continuation = (type < '0' || type > '9') && ctx->fragment_len > 0;
    
    if (is_continuation) {
        // 追加到分片缓冲区
        size_t new_len = ctx->fragment_len + len;
        if (new_len + 1 > ctx->fragment_cap) {
            size_t new_cap = new_len + 1024;
            char *new_buf = heap_caps_realloc(ctx->fragment_buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!new_buf) {
                TS_LOGE(TAG, "Fragment buffer realloc failed");
                return;
            }
            ctx->fragment_buf = new_buf;
            ctx->fragment_cap = new_cap;
        }
        memcpy(ctx->fragment_buf + ctx->fragment_len, data, len);
        ctx->fragment_len = new_len;
        ctx->fragment_buf[new_len] = '\0';
        
        // 尝试解析完整消息
        if (ctx->fragment_buf[0] == '4' && ctx->fragment_buf[1] == '2') {
            if (sio_try_parse_event(ctx, ctx->fragment_buf, ctx->fragment_len)) {
                // 解析成功，清空缓冲区
                ctx->fragment_len = 0;
            }
        }
        return;
    }
    
    switch (type) {
        case '0': {
            // Engine.IO open - 解析 sid
            const char *json_start = strchr(data, '{');
            if (json_start) {
                cJSON *json = cJSON_Parse(json_start);
                if (json) {
                    cJSON *sid = cJSON_GetObjectItem(json, "sid");
                    if (sid && cJSON_IsString(sid)) {
                        if (ctx->sid) free(ctx->sid);
                        ctx->sid = strdup(sid->valuestring);
                        TS_LOGI(TAG, "Socket.IO sid: %s", ctx->sid);
                    }
                    cJSON_Delete(json);
                }
            }
            break;
        }
        
        case '3':
            // pong 或 probe response
            if (len >= 6 && strncmp(data, "3probe", 6) == 0) {
                TS_LOGI(TAG, "Socket.IO probe response received");
                ctx->upgraded = true;
                if (ctx->upgrade_sem) xSemaphoreGive(ctx->upgrade_sem);
            }
            break;
            
        case '4': {
            // Socket.IO message
            if (len < 2) break;
            char sio_type = data[1];
            
            if (sio_type == '0') {
                // Socket.IO connect acknowledgment
                TS_LOGI(TAG, "Socket.IO connected");
            }
            else if (sio_type == '2') {
                // Socket.IO event: 42["event_name", data]
                // 先尝试直接解析
                if (!sio_try_parse_event(ctx, data, len)) {
                    // 解析失败，可能是分片消息，存入缓冲区
                    if (ctx->fragment_cap < (size_t)len + 1) {
                        size_t new_cap = len + 4096;
                        char *new_buf = heap_caps_realloc(ctx->fragment_buf, new_cap, MALLOC_CAP_SPIRAM);
                        if (new_buf) {
                            ctx->fragment_buf = new_buf;
                            ctx->fragment_cap = new_cap;
                        }
                    }
                    if (ctx->fragment_buf && ctx->fragment_cap >= (size_t)len + 1) {
                        memcpy(ctx->fragment_buf, data, len);
                        ctx->fragment_len = len;
                        ctx->fragment_buf[len] = '\0';
                        TS_LOGI(TAG, "Socket.IO event fragmented, buffering %d bytes", len);
                    }
                }
            }
            break;
        }
        
        default:
            TS_LOGD(TAG, "Socket.IO unknown message type: %c", type);
            break;
    }
}

static void sio_ws_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    sio_test_ctx_t *ctx = (sio_test_ctx_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            TS_LOGI(TAG, "Socket.IO WS connected");
            ctx->connected = true;
            if (ctx->connect_sem) xSemaphoreGive(ctx->connect_sem);
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            TS_LOGI(TAG, "Socket.IO WS disconnected");
            ctx->connected = false;
            // 释放所有等待的信号量
            if (!ctx->upgraded && ctx->upgrade_sem) xSemaphoreGive(ctx->upgrade_sem);
            if (!ctx->received && ctx->data_sem) xSemaphoreGive(ctx->data_sem);
            break;
            
        case WEBSOCKET_EVENT_DATA:
            if (data->data_len > 0) {
                // 临时 null 终止以便解析
                char *buf = strndup(data->data_ptr, data->data_len);
                if (buf) {
                    // 打印接收到的所有消息（调试用）
                    TS_LOGI(TAG, "Socket.IO recv [%d]: %.100s%s", data->data_len, buf, 
                            data->data_len > 100 ? "..." : "");
                    sio_parse_message(ctx, buf, data->data_len);
                    free(buf);
                }
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            TS_LOGE(TAG, "Socket.IO WS error");
            ctx->error = strdup("Socket.IO WebSocket error");
            if (ctx->connect_sem) xSemaphoreGive(ctx->connect_sem);
            if (ctx->upgrade_sem) xSemaphoreGive(ctx->upgrade_sem);
            if (ctx->data_sem) xSemaphoreGive(ctx->data_sem);
            break;
            
        default:
            break;
    }
}

/**
 * @brief automation.proxy.socketio_test - Test Socket.IO connection
 *
 * Connects to a Socket.IO server (v4 protocol), waits for specified event.
 * 
 * Parameters:
 *   - url: Base URL (http://host:port)
 *   - event: Event name to listen for (default: any)
 *   - timeout_ms: Timeout in milliseconds (default: 15000)
 *   - json_path: Optional JSON path to extract from event data
 * 
 * Returns:
 *   - connected: Whether connection was established
 *   - event: Event name received
 *   - data: Event payload (or extracted value if json_path specified)
 * 
 * Example:
 *   {"url": "http://10.10.99.99:59090", "event": "lpmu_status_update", "json_path": "cpu.avg_usage"}
 */
static esp_err_t api_automation_proxy_socketio_test(const cJSON *params, ts_api_result_t *result)
{
    cJSON *url_json = cJSON_GetObjectItem(params, "url");
    cJSON *event_json = cJSON_GetObjectItem(params, "event");
    cJSON *timeout_json = cJSON_GetObjectItem(params, "timeout_ms");
    cJSON *json_path_json = cJSON_GetObjectItem(params, "json_path");
    
    if (!url_json || !cJSON_IsString(url_json)) {
        result->code = TS_API_ERR_INVALID_ARG;
        result->message = strdup("Missing 'url' parameter");
        return ESP_OK;
    }
    
    const char *base_url = url_json->valuestring;
    const char *target_event = (event_json && cJSON_IsString(event_json)) ? event_json->valuestring : NULL;
    int timeout_ms = (timeout_json && cJSON_IsNumber(timeout_json)) ? timeout_json->valueint : 15000;
    const char *json_path = (json_path_json && cJSON_IsString(json_path_json)) ? json_path_json->valuestring : NULL;
    
    TS_LOGI(TAG, "Socket.IO test: url=%s event=%s timeout=%d", 
            base_url, target_event ? target_event : "(any)", timeout_ms);
    
    // 初始化上下文
    sio_test_ctx_t ctx = {
        .sid = NULL,
        .message = NULL,
        .event_name = NULL,
        .error = NULL,
        .connected = false,
        .upgraded = false,
        .received = false,
        .connect_sem = xSemaphoreCreateBinary(),
        .upgrade_sem = xSemaphoreCreateBinary(),
        .data_sem = xSemaphoreCreateBinary(),
        .target_event = target_event,
    };
    
    if (!ctx.connect_sem || !ctx.upgrade_sem || !ctx.data_sem) {
        if (ctx.connect_sem) vSemaphoreDelete(ctx.connect_sem);
        if (ctx.upgrade_sem) vSemaphoreDelete(ctx.upgrade_sem);
        if (ctx.data_sem) vSemaphoreDelete(ctx.data_sem);
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to create semaphores");
        return ESP_OK;
    }
    
    cJSON *result_data = cJSON_CreateObject();
    
    // ========== Step 1: HTTP 轮询获取 session ==========
    char polling_url[256];
    snprintf(polling_url, sizeof(polling_url), "%s/socket.io/?EIO=4&transport=polling", base_url);
    
    esp_http_client_config_t http_config = {
        .url = polling_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    
    esp_http_client_handle_t http_client = esp_http_client_init(&http_config);
    if (!http_client) {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to init HTTP client");
        goto cleanup;
    }
    
    esp_err_t err = esp_http_client_open(http_client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(http_client);
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Socket.IO polling request failed");
        cJSON_AddBoolToObject(result_data, "connected", false);
        cJSON_AddStringToObject(result_data, "error", "HTTP polling failed");
        result->data = result_data;
        goto cleanup;
    }
    
    esp_http_client_fetch_headers(http_client);
    
    // 读取响应
    char *poll_response = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!poll_response) poll_response = malloc(2048);
    
    int read_len = 0;
    if (poll_response) {
        read_len = esp_http_client_read(http_client, poll_response, 2047);
        if (read_len > 0) {
            poll_response[read_len] = '\0';
            TS_LOGD(TAG, "Socket.IO polling response: %s", poll_response);
            
            // 解析响应获取 sid
            // 响应格式可能是: 0{"sid":"xxx",...} 或带长度前缀
            const char *json_start = strchr(poll_response, '{');
            if (json_start) {
                cJSON *json = cJSON_Parse(json_start);
                if (json) {
                    cJSON *sid = cJSON_GetObjectItem(json, "sid");
                    if (sid && cJSON_IsString(sid)) {
                        ctx.sid = strdup(sid->valuestring);
                        TS_LOGI(TAG, "Socket.IO session: %s", ctx.sid);
                    }
                    cJSON_Delete(json);
                }
            }
        }
        free(poll_response);
    }
    
    esp_http_client_cleanup(http_client);
    
    if (!ctx.sid) {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to get Socket.IO session");
        cJSON_AddBoolToObject(result_data, "connected", false);
        cJSON_AddStringToObject(result_data, "error", "No session ID received");
        result->data = result_data;
        goto cleanup;
    }
    
    // ========== Step 2: WebSocket 连接 ==========
    // 解析 host 和 port
    char ws_url[384];
    const char *host_start = strstr(base_url, "://");
    if (host_start) {
        host_start += 3;
    } else {
        host_start = base_url;
    }
    
    // 构造 WebSocket URL
    snprintf(ws_url, sizeof(ws_url), "ws://%s/socket.io/?EIO=4&transport=websocket&sid=%s",
             host_start, ctx.sid);
    
    TS_LOGI(TAG, "Socket.IO WS URL: %s", ws_url);
    
    esp_websocket_client_config_t ws_cfg = {
        .uri = ws_url,
        .buffer_size = 4096,
        .network_timeout_ms = timeout_ms,
        .reconnect_timeout_ms = timeout_ms * 2,  // 禁用自动重连
    };
    
    esp_websocket_client_handle_t ws_client = esp_websocket_client_init(&ws_cfg);
    if (!ws_client) {
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("Failed to init WebSocket client");
        cJSON_AddBoolToObject(result_data, "connected", false);
        result->data = result_data;
        goto cleanup;
    }
    
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, sio_ws_handler, &ctx);
    
    err = esp_websocket_client_start(ws_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(ws_client);
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("WebSocket start failed");
        cJSON_AddBoolToObject(result_data, "connected", false);
        result->data = result_data;
        goto cleanup;
    }
    
    // 等待 WebSocket 连接
    bool ws_connected = (xSemaphoreTake(ctx.connect_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) && ctx.connected;
    
    if (!ws_connected) {
        esp_websocket_client_stop(ws_client);
        esp_websocket_client_destroy(ws_client);
        result->code = TS_API_ERR_INTERNAL;
        result->message = strdup("WebSocket connection timeout");
        cJSON_AddBoolToObject(result_data, "connected", false);
        cJSON_AddStringToObject(result_data, "error", "WebSocket connection failed");
        result->data = result_data;
        goto cleanup;
    }
    
    // ========== Step 3: 发送升级探针 ==========
    TS_LOGI(TAG, "Socket.IO sending probe");
    esp_websocket_client_send_text(ws_client, "2probe", 6, pdMS_TO_TICKS(5000));
    
    // 等待 probe 响应
    bool probe_ok = (xSemaphoreTake(ctx.upgrade_sem, pdMS_TO_TICKS(5000)) == pdTRUE) && ctx.upgraded;
    
    if (!probe_ok) {
        TS_LOGW(TAG, "Socket.IO probe timeout, continuing anyway");
        // 某些服务器可能不需要 probe，继续尝试
    }
    
    // ========== Step 4: 发送升级确认 ==========
    TS_LOGI(TAG, "Socket.IO sending upgrade");
    esp_websocket_client_send_text(ws_client, "5", 1, pdMS_TO_TICKS(5000));
    
    // 短暂等待让服务器处理升级
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // ========== Step 5: 发送 Socket.IO CONNECT 到默认 namespace ==========
    // Socket.IO 协议要求发送 40 (Engine.IO message + Socket.IO CONNECT) 来连接 namespace
    TS_LOGI(TAG, "Socket.IO sending CONNECT to default namespace");
    esp_websocket_client_send_text(ws_client, "40", 2, pdMS_TO_TICKS(5000));
    
    // 短暂等待连接确认
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // ========== Step 6: 等待事件数据 ==========
    TS_LOGI(TAG, "Socket.IO waiting for event data...");
    bool data_ok = (xSemaphoreTake(ctx.data_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) && ctx.received;
    
    cJSON_AddBoolToObject(result_data, "connected", true);
    cJSON_AddStringToObject(result_data, "sid", ctx.sid);
    
    if (data_ok && ctx.message) {
        TS_LOGI(TAG, "Socket.IO received event: %s", ctx.event_name ? ctx.event_name : "(unknown)");
        
        if (ctx.event_name) {
            cJSON_AddStringToObject(result_data, "event", ctx.event_name);
        }
        
        // 解析数据
        cJSON *event_data = cJSON_Parse(ctx.message);
        if (event_data) {
            // 收集所有可用的 JSON 路径供 WebUI 展示
            cJSON *available_paths = cJSON_CreateArray();
            collect_json_paths(event_data, "", available_paths, 5);  // 最大深度 5
            cJSON_AddItemToObject(result_data, "available_paths", available_paths);
            
            // 如果指定了 json_path，提取值
            if (json_path && strlen(json_path) > 0) {
                cJSON *extracted = extract_json_value(event_data, json_path);
                if (extracted) {
                    cJSON_AddItemToObject(result_data, "value", cJSON_Duplicate(extracted, true));
                    cJSON_AddStringToObject(result_data, "path", json_path);
                } else {
                    cJSON_AddNullToObject(result_data, "value");
                    cJSON_AddStringToObject(result_data, "path_error", "Path not found");
                }
                // 同时返回完整数据
                cJSON_AddItemToObject(result_data, "data", event_data);
            } else {
                cJSON_AddItemToObject(result_data, "data", event_data);
            }
        } else {
            // 非 JSON 数据，返回原始字符串
            cJSON_AddStringToObject(result_data, "data", ctx.message);
        }
        
        result->code = TS_API_OK;
        result->message = strdup("Socket.IO test successful");
    } else {
        result->code = TS_API_ERR_TIMEOUT;
        result->message = strdup("Socket.IO event timeout");
        cJSON_AddStringToObject(result_data, "error", 
            target_event ? "Target event not received" : "No event received");
    }
    
    result->data = result_data;
    
    // 清理 WebSocket
    esp_websocket_client_stop(ws_client);
    esp_websocket_client_destroy(ws_client);
    
cleanup:
    if (ctx.sid) free(ctx.sid);
    if (ctx.message) free(ctx.message);
    if (ctx.event_name) free(ctx.event_name);
    if (ctx.error) free(ctx.error);
    if (ctx.fragment_buf) free(ctx.fragment_buf);
    if (ctx.connect_sem) vSemaphoreDelete(ctx.connect_sem);
    if (ctx.upgrade_sem) vSemaphoreDelete(ctx.upgrade_sem);
    if (ctx.data_sem) vSemaphoreDelete(ctx.data_sem);
    
    return ESP_OK;
}

/*===========================================================================*/
/*                    Source Export/Import APIs                               */
/*===========================================================================*/

/**
 * @brief Serialize source to JSON for export
 */
static cJSON *source_to_export_json(const ts_auto_source_t *source)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", source->id);
    cJSON_AddStringToObject(obj, "label", source->label);
    
    const char *type_str = "unknown";
    switch (source->type) {
        case TS_AUTO_SRC_WEBSOCKET: type_str = "websocket"; break;
        case TS_AUTO_SRC_SOCKETIO: type_str = "socketio"; break;
        case TS_AUTO_SRC_REST: type_str = "rest"; break;
        case TS_AUTO_SRC_VARIABLE: type_str = "variable"; break;
        default: type_str = "unknown"; break;
    }
    cJSON_AddStringToObject(obj, "type", type_str);
    cJSON_AddBoolToObject(obj, "enabled", source->enabled);
    cJSON_AddBoolToObject(obj, "auto_discover", source->auto_discover);
    cJSON_AddNumberToObject(obj, "poll_interval_ms", source->poll_interval_ms);
    
    // Type-specific config
    switch (source->type) {
        case TS_AUTO_SRC_WEBSOCKET:
            cJSON_AddStringToObject(obj, "uri", source->websocket.uri);
            cJSON_AddNumberToObject(obj, "reconnect_ms", source->websocket.reconnect_ms);
            break;
        case TS_AUTO_SRC_SOCKETIO:
            cJSON_AddStringToObject(obj, "url", source->socketio.url);
            cJSON_AddStringToObject(obj, "event", source->socketio.event);
            cJSON_AddNumberToObject(obj, "reconnect_ms", source->socketio.reconnect_ms);
            break;
        case TS_AUTO_SRC_REST:
            cJSON_AddStringToObject(obj, "url", source->rest.url);
            cJSON_AddStringToObject(obj, "method", source->rest.method);
            if (source->rest.auth_header[0]) {
                cJSON_AddStringToObject(obj, "auth_header", source->rest.auth_header);
            }
            break;
        case TS_AUTO_SRC_VARIABLE:
            if (source->variable.ssh_host_id[0]) {
                cJSON_AddStringToObject(obj, "ssh_host_id", source->variable.ssh_host_id);
            }
            if (source->variable.ssh_command[0]) {
                cJSON_AddStringToObject(obj, "ssh_command", source->variable.ssh_command);
            }
            if (source->variable.var_prefix[0]) {
                cJSON_AddStringToObject(obj, "var_prefix", source->variable.var_prefix);
            }
            break;
        default:
            break;
    }
    
    // Mappings
    if (source->mapping_count > 0) {
        cJSON *mappings = cJSON_AddArrayToObject(obj, "mappings");
        for (int i = 0; i < source->mapping_count && i < TS_AUTO_MAX_MAPPINGS; i++) {
            cJSON *map = cJSON_CreateObject();
            cJSON_AddStringToObject(map, "json_path", source->mappings[i].json_path);
            cJSON_AddStringToObject(map, "var_name", source->mappings[i].var_name);
            if (source->mappings[i].transform[0]) {
                cJSON_AddStringToObject(map, "transform", source->mappings[i].transform);
            }
            cJSON_AddItemToArray(mappings, map);
        }
    }
    
    return obj;
}

/**
 * @brief automation.sources.export - Export data source as config pack
 */
static esp_err_t api_automation_sources_export(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_OK;
    }
    
    const cJSON *id = cJSON_GetObjectItem(params, "id");
    if (!id || !cJSON_IsString(id) || !id->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'id' parameter");
        return ESP_OK;
    }
    
    if (!ts_config_pack_can_export()) {
        ts_api_result_error(result, TS_API_ERR_NO_PERMISSION, 
            "This device is not authorized to export config packs");
        return ESP_OK;
    }
    
    // Get source
    ts_auto_source_t source_copy;
    const ts_auto_source_t *source = ts_source_get(id->valuestring);
    if (!source) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Source not found");
        return ESP_OK;
    }
    // Copy to avoid holding lock
    memcpy(&source_copy, source, sizeof(ts_auto_source_t));

    // Build export JSON
    cJSON *export_json = cJSON_CreateObject();
    cJSON_AddStringToObject(export_json, "type", "automation_source");
    cJSON_AddItemToObject(export_json, "source", source_to_export_json(&source_copy));
    
    char *json_str = cJSON_PrintUnformatted(export_json);
    cJSON_Delete(export_json);
    if (!json_str) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to serialize config");
        return ESP_OK;
    }
    
    // Get recipient certificate
    const cJSON *recipient_cert = cJSON_GetObjectItem(params, "recipient_cert");
    char *cert_pem = NULL;
    size_t cert_len = 0;
    bool free_cert = false;
    
    if (recipient_cert && cJSON_IsString(recipient_cert) && recipient_cert->valuestring[0]) {
        cert_pem = recipient_cert->valuestring;
        cert_len = strlen(cert_pem);
    } else {
        cert_pem = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!cert_pem) {
            cJSON_free(json_str);
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Out of memory");
            return ESP_OK;
        }
        cert_len = 4096;
        esp_err_t ret = ts_cert_get_certificate(cert_pem, &cert_len);
        if (ret != ESP_OK) {
            free(cert_pem);
            cJSON_free(json_str);
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get device certificate");
            return ESP_OK;
        }
        free_cert = true;
    }
    
    // Create encrypted config pack
    ts_config_pack_export_opts_t opts = {
        .recipient_cert_pem = cert_pem,
        .recipient_cert_len = cert_len,
        .description = "Automation data source configuration"
    };
    
    char *tscfg_output = NULL;
    size_t tscfg_len = 0;
    ts_config_pack_result_t pack_result = ts_config_pack_create(
        source_copy.id, json_str, strlen(json_str), &opts, &tscfg_output, &tscfg_len);
    
    cJSON_free(json_str);
    if (free_cert) free(cert_pem);
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to create config pack");
        return ESP_OK;
    }
    
    // Return result
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "tscfg", tscfg_output);
    char filename[80];
    snprintf(filename, sizeof(filename), "source_%s.tscfg", source_copy.id);
    cJSON_AddStringToObject(data, "filename", filename);
    
    free(tscfg_output);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief automation.sources.import - Import data source from config pack
 */
static esp_err_t api_automation_sources_import(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_OK;
    }
    
    const cJSON *tscfg = cJSON_GetObjectItem(params, "tscfg");
    const cJSON *filename = cJSON_GetObjectItem(params, "filename");
    if (!tscfg || !cJSON_IsString(tscfg) || !tscfg->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'tscfg' parameter");
        return ESP_OK;
    }
    
    const cJSON *overwrite = cJSON_GetObjectItem(params, "overwrite");
    const cJSON *preview = cJSON_GetObjectItem(params, "preview");
    bool do_overwrite = overwrite && cJSON_IsTrue(overwrite);
    bool do_preview = preview && cJSON_IsTrue(preview);
    
    // Lightweight verification
    ts_config_pack_sig_info_t sig_info = {0};
    ts_config_pack_result_t pack_result = ts_config_pack_verify_mem(
        tscfg->valuestring, strlen(tscfg->valuestring), &sig_info);
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        const char *err_msg = "Failed to verify config pack";
        if (pack_result == TS_CONFIG_PACK_ERR_RECIPIENT) {
            err_msg = "This config pack is not for this device";
        } else if (pack_result == TS_CONFIG_PACK_ERR_SIGNATURE) {
            err_msg = "Invalid signature";
        }
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, err_msg);
        return ESP_OK;
    }
    
    // Extract config ID from filename
    char config_id[64] = {0};
    if (filename && cJSON_IsString(filename) && filename->valuestring[0]) {
        const char *name = filename->valuestring;
        // Strip "source_" prefix if present
        if (strncmp(name, "source_", 7) == 0) {
            name += 7;
        }
        const char *dot = strrchr(name, '.');
        if (dot && strcmp(dot, ".tscfg") == 0) {
            size_t len = dot - name;
            if (len > sizeof(config_id) - 1) len = sizeof(config_id) - 1;
            strncpy(config_id, name, len);
        } else {
            strncpy(config_id, name, sizeof(config_id) - 1);
        }
    } else {
        snprintf(config_id, sizeof(config_id), "src_%lld", (long long)time(NULL));
    }
    
    // Check if exists
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", SOURCES_SDCARD_DIR, config_id);
    struct stat st;
    bool exists = (stat(filepath, &st) == 0);
    if (!exists) {
        snprintf(filepath, sizeof(filepath), "%s/%s.json", SOURCES_SDCARD_DIR, config_id);
        exists = (stat(filepath, &st) == 0);
    }
    
    // Preview mode
    if (do_preview) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "valid", true);
        cJSON_AddStringToObject(data, "type", "automation_source");
        cJSON_AddStringToObject(data, "id", config_id);
        cJSON_AddBoolToObject(data, "exists", exists);
        cJSON_AddStringToObject(data, "signer", sig_info.signer_cn[0] ? sig_info.signer_cn : "unknown");
        cJSON_AddBoolToObject(data, "official", sig_info.is_official);
        cJSON_AddStringToObject(data, "note", "Content will be decrypted on system restart");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    if (exists && !do_overwrite) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "id", config_id);
        cJSON_AddBoolToObject(data, "exists", true);
        cJSON_AddStringToObject(data, "message", "Source config already exists, set overwrite=true to replace");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    // Ensure directory exists
    if (stat(SOURCES_SDCARD_DIR, &st) != 0) {
        mkdir(SOURCES_SDCARD_DIR, 0755);
    }
    
    // Save .tscfg file
    snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", SOURCES_SDCARD_DIR, config_id);
    FILE *f = fopen(filepath, "w");
    if (!f) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to create config file");
        return ESP_OK;
    }
    
    size_t written = fwrite(tscfg->valuestring, 1, strlen(tscfg->valuestring), f);
    fclose(f);
    
    if (written != strlen(tscfg->valuestring)) {
        unlink(filepath);
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to write config file");
        return ESP_OK;
    }
    
    // Delete old .json if overwriting
    if (do_overwrite) {
        char json_path[128];
        snprintf(json_path, sizeof(json_path), "%s/%s.json", SOURCES_SDCARD_DIR, config_id);
        unlink(json_path);
    }
    
    TS_LOGI(TAG, "Source config imported: %s (will be loaded on restart)", filepath);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "id", config_id);
    cJSON_AddStringToObject(data, "path", filepath);
    cJSON_AddBoolToObject(data, "imported", true);
    cJSON_AddBoolToObject(data, "overwritten", exists && do_overwrite);
    cJSON_AddStringToObject(data, "note", "Restart system to load the new config");
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                    Rule Export/Import APIs                                 */
/*===========================================================================*/

/**
 * @brief Serialize rule to JSON for export
 */

/**
 * @brief automation.rules.export - Export rule as config pack
 */
static esp_err_t api_automation_rules_export(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_OK;
    }
    
    const cJSON *id = cJSON_GetObjectItem(params, "id");
    if (!id || !cJSON_IsString(id) || !id->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'id' parameter");
        return ESP_OK;
    }
    
    if (!ts_config_pack_can_export()) {
        ts_api_result_error(result, TS_API_ERR_NO_PERMISSION, 
            "This device is not authorized to export config packs");
        return ESP_OK;
    }

    ts_auto_rule_t snapshot;
    if (ts_rule_acquire(id->valuestring, &snapshot) != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Rule not found");
        return ESP_OK;
    }

    char rule_id[TS_AUTO_NAME_MAX_LEN];
    strcpy(rule_id, snapshot.id);
    // Build export JSON
    cJSON *export_json = cJSON_CreateObject();
    cJSON_AddStringToObject(export_json, "type", "automation_rule");
    cJSON *encoded = ts_rule_encode(&snapshot);
    ts_rule_release(&snapshot);
    if (!encoded || !export_json || !cJSON_AddItemToObject(export_json, "rule", encoded)) {
        cJSON_Delete(encoded);
        cJSON_Delete(export_json);
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "no_memory");
        return ESP_OK;
    }

    char *json_str = cJSON_PrintUnformatted(export_json);
    cJSON_Delete(export_json);
    if (!json_str) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to serialize config");
        return ESP_OK;
    }
    
    // Get recipient certificate
    const cJSON *recipient_cert = cJSON_GetObjectItem(params, "recipient_cert");
    char *cert_pem = NULL;
    size_t cert_len = 0;
    bool free_cert = false;
    
    if (recipient_cert && cJSON_IsString(recipient_cert) && recipient_cert->valuestring[0]) {
        cert_pem = recipient_cert->valuestring;
        cert_len = strlen(cert_pem);
    } else {
        cert_pem = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!cert_pem) {
            cJSON_free(json_str);
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Out of memory");
            return ESP_OK;
        }
        cert_len = 4096;
        esp_err_t ret = ts_cert_get_certificate(cert_pem, &cert_len);
        if (ret != ESP_OK) {
            free(cert_pem);
            cJSON_free(json_str);
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get device certificate");
            return ESP_OK;
        }
        free_cert = true;
    }
    
    // Create encrypted config pack
    ts_config_pack_export_opts_t opts = {
        .recipient_cert_pem = cert_pem,
        .recipient_cert_len = cert_len,
        .description = "Automation rule configuration"
    };
    
    char *tscfg_output = NULL;
    size_t tscfg_len = 0;
    ts_config_pack_result_t pack_result = ts_config_pack_create(rule_id, json_str, strlen(json_str),
                                                                &opts, &tscfg_output, &tscfg_len);

    cJSON_free(json_str);
    if (free_cert) free(cert_pem);
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to create config pack");
        return ESP_OK;
    }
    
    // Return result
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "tscfg", tscfg_output);
    char filename[80];
    snprintf(filename, sizeof(filename), "rule_%s.tscfg", rule_id);
    cJSON_AddStringToObject(data, "filename", filename);
    
    free(tscfg_output);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief automation.rules.import - Import rule from config pack
 */
static esp_err_t api_automation_rules_import(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_OK;
    }
    
    const cJSON *tscfg = cJSON_GetObjectItem(params, "tscfg");
    const cJSON *filename = cJSON_GetObjectItem(params, "filename");
    if (!tscfg || !cJSON_IsString(tscfg) || !tscfg->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'tscfg' parameter");
        return ESP_OK;
    }
    
    const cJSON *overwrite = cJSON_GetObjectItem(params, "overwrite");
    const cJSON *preview = cJSON_GetObjectItem(params, "preview");
    bool do_overwrite = overwrite && cJSON_IsTrue(overwrite);
    bool do_preview = preview && cJSON_IsTrue(preview);
    
    // Lightweight verification
    ts_config_pack_sig_info_t sig_info = {0};
    ts_config_pack_result_t pack_result = ts_config_pack_verify_mem(
        tscfg->valuestring, strlen(tscfg->valuestring), &sig_info);
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        const char *err_msg = "Failed to verify config pack";
        if (pack_result == TS_CONFIG_PACK_ERR_RECIPIENT) {
            err_msg = "This config pack is not for this device";
        } else if (pack_result == TS_CONFIG_PACK_ERR_SIGNATURE) {
            err_msg = "Invalid signature";
        }
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, err_msg);
        return ESP_OK;
    }
    
    // Extract config ID from filename
    char config_id[64] = {0};
    if (filename && cJSON_IsString(filename) && filename->valuestring[0]) {
        const char *name = filename->valuestring;
        if (strncmp(name, "rule_", 5) == 0) {
            name += 5;
        }
        const char *dot = strrchr(name, '.');
        if (dot && strcmp(dot, ".tscfg") == 0) {
            size_t len = dot - name;
            if (len > sizeof(config_id) - 1) len = sizeof(config_id) - 1;
            strncpy(config_id, name, len);
        } else {
            strncpy(config_id, name, sizeof(config_id) - 1);
        }
    } else {
        snprintf(config_id, sizeof(config_id), "rule_%lld", (long long)time(NULL));
    }
    
    // Check if exists
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", RULES_SDCARD_DIR, config_id);
    struct stat st;
    bool exists = (stat(filepath, &st) == 0);
    if (!exists) {
        snprintf(filepath, sizeof(filepath), "%s/%s.json", RULES_SDCARD_DIR, config_id);
        exists = (stat(filepath, &st) == 0);
    }
    
    // Preview mode
    if (do_preview) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "valid", true);
        cJSON_AddStringToObject(data, "type", "automation_rule");
        cJSON_AddStringToObject(data, "id", config_id);
        cJSON_AddBoolToObject(data, "exists", exists);
        cJSON_AddStringToObject(data, "signer", sig_info.signer_cn[0] ? sig_info.signer_cn : "unknown");
        cJSON_AddBoolToObject(data, "official", sig_info.is_official);
        cJSON_AddStringToObject(data, "note", "Content will be decrypted on system restart");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    if (exists && !do_overwrite) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "id", config_id);
        cJSON_AddBoolToObject(data, "exists", true);
        cJSON_AddStringToObject(data, "message", "Rule config already exists, set overwrite=true to replace");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }

    ts_api_result_error(result, TS_API_ERR_INVALID_ARG,
                        "source_read_only: encrypted rule import requires a recoverable writer");
    return ESP_OK;
}

/*===========================================================================*/
/*                    Action Template Export/Import APIs                      */
/*===========================================================================*/

/**
 * @brief Serialize action template to JSON for export
 */
static cJSON *action_template_to_export_json(const ts_action_template_t *tpl)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", tpl->id);
    cJSON_AddStringToObject(obj, "name", tpl->name);
    cJSON_AddStringToObject(obj, "description", tpl->description);
    cJSON_AddBoolToObject(obj, "enabled", tpl->enabled);
    cJSON_AddBoolToObject(obj, "async", tpl->async);
    cJSON_AddStringToObject(obj, "type", action_type_to_string(tpl->action.type));
    cJSON_AddNumberToObject(obj, "delay_ms", tpl->action.delay_ms);
    
    // Type-specific config
    switch (tpl->action.type) {
        case TS_AUTO_ACT_CLI: {
            cJSON *cli = cJSON_AddObjectToObject(obj, "cli");
            cJSON_AddStringToObject(cli, "command", tpl->action.cli.command);
            cJSON_AddStringToObject(cli, "var_name", tpl->action.cli.var_name);
            cJSON_AddNumberToObject(cli, "timeout_ms", tpl->action.cli.timeout_ms);
            break;
        }
        case TS_AUTO_ACT_SSH_CMD_REF: {
            cJSON *ssh_ref = cJSON_AddObjectToObject(obj, "ssh_ref");
            cJSON_AddStringToObject(ssh_ref, "cmd_id", tpl->action.ssh_ref.cmd_id);
            break;
        }
        case TS_AUTO_ACT_LED: {
            cJSON *led = cJSON_AddObjectToObject(obj, "led");
            cJSON_AddStringToObject(led, "device", tpl->action.led.device);
            char color_str[8];
            snprintf(color_str, sizeof(color_str), "#%02X%02X%02X",
                     tpl->action.led.r, tpl->action.led.g, tpl->action.led.b);
            cJSON_AddStringToObject(led, "color", color_str);
            cJSON_AddNumberToObject(led, "brightness", tpl->action.led.brightness);
            if (tpl->action.led.effect[0]) {
                cJSON_AddStringToObject(led, "effect", tpl->action.led.effect);
            }
            cJSON_AddNumberToObject(led, "duration_ms", tpl->action.led.duration_ms);
            break;
        }
        case TS_AUTO_ACT_LOG: {
            cJSON *log_obj = cJSON_AddObjectToObject(obj, "log");
            cJSON_AddNumberToObject(log_obj, "level", tpl->action.log.level);
            cJSON_AddStringToObject(log_obj, "message", tpl->action.log.message);
            break;
        }
        case TS_AUTO_ACT_WEBHOOK: {
            cJSON *webhook = cJSON_AddObjectToObject(obj, "webhook");
            cJSON_AddStringToObject(webhook, "url", tpl->action.webhook.url);
            cJSON_AddStringToObject(webhook, "method", tpl->action.webhook.method);
            cJSON_AddStringToObject(webhook, "body_template", tpl->action.webhook.body_template);
            break;
        }
        default:
            break;
    }
    
    return obj;
}

/**
 * @brief automation.actions.export - Export action template as config pack
 */
static esp_err_t api_automation_actions_export(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_OK;
    }
    
    const cJSON *id = cJSON_GetObjectItem(params, "id");
    if (!id || !cJSON_IsString(id) || !id->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'id' parameter");
        return ESP_OK;
    }
    
    if (!ts_config_pack_can_export()) {
        ts_api_result_error(result, TS_API_ERR_NO_PERMISSION, 
            "This device is not authorized to export config packs");
        return ESP_OK;
    }
    
    ts_action_template_t tpl;
    esp_err_t ret = ts_action_template_get(id->valuestring, &tpl);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Action template not found");
        return ESP_OK;
    }
    
    // Build export JSON
    cJSON *export_json = cJSON_CreateObject();
    cJSON_AddStringToObject(export_json, "type", "action_template");
    cJSON_AddItemToObject(export_json, "template", action_template_to_export_json(&tpl));
    
    char *json_str = cJSON_PrintUnformatted(export_json);
    cJSON_Delete(export_json);
    if (!json_str) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to serialize config");
        return ESP_OK;
    }
    
    // Get recipient certificate
    const cJSON *recipient_cert = cJSON_GetObjectItem(params, "recipient_cert");
    char *cert_pem = NULL;
    size_t cert_len = 0;
    bool free_cert = false;
    
    if (recipient_cert && cJSON_IsString(recipient_cert) && recipient_cert->valuestring[0]) {
        cert_pem = recipient_cert->valuestring;
        cert_len = strlen(cert_pem);
    } else {
        cert_pem = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!cert_pem) {
            cJSON_free(json_str);
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Out of memory");
            return ESP_OK;
        }
        cert_len = 4096;
        ret = ts_cert_get_certificate(cert_pem, &cert_len);
        if (ret != ESP_OK) {
            free(cert_pem);
            cJSON_free(json_str);
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get device certificate");
            return ESP_OK;
        }
        free_cert = true;
    }
    
    // Create encrypted config pack
    ts_config_pack_export_opts_t opts = {
        .recipient_cert_pem = cert_pem,
        .recipient_cert_len = cert_len,
        .description = "Automation action template"
    };
    
    char *tscfg_output = NULL;
    size_t tscfg_len = 0;
    ts_config_pack_result_t pack_result = ts_config_pack_create(
        tpl.id, json_str, strlen(json_str), &opts, &tscfg_output, &tscfg_len);
    
    cJSON_free(json_str);
    if (free_cert) free(cert_pem);
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to create config pack");
        return ESP_OK;
    }
    
    // Return result
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "tscfg", tscfg_output);
    char filename[80];
    snprintf(filename, sizeof(filename), "action_%s.tscfg", tpl.id);
    cJSON_AddStringToObject(data, "filename", filename);
    
    free(tscfg_output);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief automation.actions.import - Import action template from config pack
 */
static esp_err_t api_automation_actions_import(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_OK;
    }
    
    const cJSON *tscfg = cJSON_GetObjectItem(params, "tscfg");
    const cJSON *filename = cJSON_GetObjectItem(params, "filename");
    if (!tscfg || !cJSON_IsString(tscfg) || !tscfg->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'tscfg' parameter");
        return ESP_OK;
    }
    
    const cJSON *overwrite = cJSON_GetObjectItem(params, "overwrite");
    const cJSON *preview = cJSON_GetObjectItem(params, "preview");
    bool do_overwrite = overwrite && cJSON_IsTrue(overwrite);
    bool do_preview = preview && cJSON_IsTrue(preview);
    
    // Lightweight verification
    ts_config_pack_sig_info_t sig_info = {0};
    ts_config_pack_result_t pack_result = ts_config_pack_verify_mem(
        tscfg->valuestring, strlen(tscfg->valuestring), &sig_info);
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        const char *err_msg = "Failed to verify config pack";
        if (pack_result == TS_CONFIG_PACK_ERR_RECIPIENT) {
            err_msg = "This config pack is not for this device";
        } else if (pack_result == TS_CONFIG_PACK_ERR_SIGNATURE) {
            err_msg = "Invalid signature";
        }
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, err_msg);
        return ESP_OK;
    }
    
    // Extract config ID from filename
    char config_id[64] = {0};
    if (filename && cJSON_IsString(filename) && filename->valuestring[0]) {
        const char *name = filename->valuestring;
        if (strncmp(name, "action_", 7) == 0) {
            name += 7;
        }
        const char *dot = strrchr(name, '.');
        if (dot && strcmp(dot, ".tscfg") == 0) {
            size_t len = dot - name;
            if (len > sizeof(config_id) - 1) len = sizeof(config_id) - 1;
            strncpy(config_id, name, len);
        } else {
            strncpy(config_id, name, sizeof(config_id) - 1);
        }
    } else {
        snprintf(config_id, sizeof(config_id), "action_%lld", (long long)time(NULL));
    }
    
    // Check if exists
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", ACTIONS_SDCARD_DIR, config_id);
    struct stat st;
    bool exists = (stat(filepath, &st) == 0);
    if (!exists) {
        snprintf(filepath, sizeof(filepath), "%s/%s.json", ACTIONS_SDCARD_DIR, config_id);
        exists = (stat(filepath, &st) == 0);
    }
    
    // Preview mode
    if (do_preview) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "valid", true);
        cJSON_AddStringToObject(data, "type", "action_template");
        cJSON_AddStringToObject(data, "id", config_id);
        cJSON_AddBoolToObject(data, "exists", exists);
        cJSON_AddStringToObject(data, "signer", sig_info.signer_cn[0] ? sig_info.signer_cn : "unknown");
        cJSON_AddBoolToObject(data, "official", sig_info.is_official);
        cJSON_AddStringToObject(data, "note", "Content will be decrypted on system restart");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    if (exists && !do_overwrite) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "id", config_id);
        cJSON_AddBoolToObject(data, "exists", true);
        cJSON_AddStringToObject(data, "message", "Action template already exists, set overwrite=true to replace");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    // Ensure directory exists
    if (stat(ACTIONS_SDCARD_DIR, &st) != 0) {
        mkdir(ACTIONS_SDCARD_DIR, 0755);
    }
    
    // Save .tscfg file
    snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", ACTIONS_SDCARD_DIR, config_id);
    FILE *f = fopen(filepath, "w");
    if (!f) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to create config file");
        return ESP_OK;
    }
    
    size_t written = fwrite(tscfg->valuestring, 1, strlen(tscfg->valuestring), f);
    fclose(f);
    
    if (written != strlen(tscfg->valuestring)) {
        unlink(filepath);
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to write config file");
        return ESP_OK;
    }
    
    // Delete old .json if overwriting
    if (do_overwrite) {
        char json_path[128];
        snprintf(json_path, sizeof(json_path), "%s/%s.json", ACTIONS_SDCARD_DIR, config_id);
        unlink(json_path);
    }
    
    TS_LOGI(TAG, "Action template imported: %s (will be loaded on restart)", filepath);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "id", config_id);
    cJSON_AddStringToObject(data, "path", filepath);
    cJSON_AddBoolToObject(data, "imported", true);
    cJSON_AddBoolToObject(data, "overwritten", exists && do_overwrite);
    cJSON_AddStringToObject(data, "note", "Restart system to load the new config");
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                           API Registration                                 */
/*===========================================================================*/

esp_err_t ts_api_automation_register(void)
{
    TS_LOGI(TAG, "Registering automation APIs");

    // Status
    ts_api_endpoint_t ep_status = {
        .name = "automation.status",
        .description = "Get automation engine status",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_status,
        .requires_auth = false,
    };
    ts_api_register(&ep_status);

    // Control APIs
    ts_api_endpoint_t ep_start = {
        .name = "automation.start",
        .description = "Start automation engine",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_start,
        .requires_auth = true,
    };
    ts_api_register(&ep_start);

    ts_api_endpoint_t ep_stop = {
        .name = "automation.stop",
        .description = "Stop automation engine",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_stop,
        .requires_auth = true,
    };
    ts_api_register(&ep_stop);

    ts_api_endpoint_t ep_pause = {
        .name = "automation.pause",
        .description = "Pause automation engine",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_pause,
        .requires_auth = true,
    };
    ts_api_register(&ep_pause);

    ts_api_endpoint_t ep_resume = {
        .name = "automation.resume",
        .description = "Resume automation engine",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_resume,
        .requires_auth = true,
    };
    ts_api_register(&ep_resume);

    ts_api_endpoint_t ep_reload = {
        .name = "automation.reload",
        .description = "Reload configuration",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_reload,
        .requires_auth = true,
    };
    ts_api_register(&ep_reload);

    // Variables APIs
    ts_api_endpoint_t ep_vars_list = {
        .name = "automation.variables.list",
        .description = "List all variables",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_variables_list,
        .requires_auth = false,
    };
    ts_api_register(&ep_vars_list);

    ts_api_endpoint_t ep_vars_get = {
        .name = "automation.variables.get",
        .description = "Get variable value",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_variables_get,
        .requires_auth = false,
    };
    ts_api_register(&ep_vars_get);

    ts_api_endpoint_t ep_vars_set = {
        .name = "automation.variables.set",
        .description = "Set variable value",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_variables_set,
        .requires_auth = true,
    };
    ts_api_register(&ep_vars_set);

    // Rules APIs
    ts_api_endpoint_t ep_rules_list = {
        .name = "automation.rules.list",
        .description = "List all rules",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_rules_list,
        .requires_auth = false,
    };
    ts_api_register(&ep_rules_list);

    ts_api_endpoint_t ep_rules_enable = {
        .name = "automation.rules.enable",
        .description = "Enable a rule",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_rules_enable,
        .requires_auth = true,
    };
    ts_api_register(&ep_rules_enable);

    ts_api_endpoint_t ep_rules_disable = {
        .name = "automation.rules.disable",
        .description = "Disable a rule",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_rules_disable,
        .requires_auth = true,
    };
    ts_api_register(&ep_rules_disable);

    ts_api_endpoint_t ep_rules_get = {
        .name = "automation.rules.get",
        .description = "Get rule details by ID",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_rules_get,
        .requires_auth = false,
    };
    ts_api_register(&ep_rules_get);

    ts_api_endpoint_t ep_service = {.name = "automation.services.status",
                                    .description = "Confirm managed remote process state",
                                    .category = TS_API_CAT_SYSTEM,
                                    .handler = api_service_status,
                                    .requires_auth = true};
    ts_api_register(&ep_service);
    ep_service.name = "automation.services.stop";
    ep_service.handler = api_service_stop;
    ep_service.description = "Stop verified managed remote process";
    ts_api_register(&ep_service);
    ts_api_endpoint_t ep_rules_trigger = {
        .name = "automation.rules.trigger",
        .description = "Manually trigger a rule",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_rules_trigger,
        .requires_auth = true,
    };
    ts_api_register(&ep_rules_trigger);

    ts_api_endpoint_t ep_rules_add = {
        .name = "automation.rules.add",
        .description = "Add a new rule",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_rules_add,
        .requires_auth = true,
    };
    ts_api_register(&ep_rules_add);
    ts_api_endpoint_t ep_rules_update = ep_rules_add;
    ep_rules_update.name = "automation.rules.update";
    ep_rules_update.description = "Update rule with expected revision";
    ts_api_register(&ep_rules_update);

    ts_api_endpoint_t ep_rules_delete = {
        .name = "automation.rules.delete",
        .description = "Delete a rule",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_rules_delete,
        .requires_auth = true,
    };
    ts_api_register(&ep_rules_delete);

    // Sources API
    ts_api_endpoint_t ep_sources_list = {
        .name = "automation.sources.list",
        .description = "List all data sources",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_sources_list,
        .requires_auth = false,
    };
    ts_api_register(&ep_sources_list);

    ts_api_endpoint_t ep_sources_add = {
        .name = "automation.sources.add",
        .description = "Add a new data source",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_sources_add,
        .requires_auth = true,
    };
    ts_api_register(&ep_sources_add);

    ts_api_endpoint_t ep_sources_delete = {
        .name = "automation.sources.delete",
        .description = "Delete a data source",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_sources_delete,
        .requires_auth = true,
    };
    ts_api_register(&ep_sources_delete);

    ts_api_endpoint_t ep_sources_add_mapping = {
        .name = "automation.sources.add_mapping",
        .description = "Add a field mapping to an existing data source",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_sources_add_mapping,
        .requires_auth = true,
    };
    ts_api_register(&ep_sources_add_mapping);

    ts_api_endpoint_t ep_sources_remove_mapping = {
        .name = "automation.sources.remove_mapping",
        .description = "Remove a field mapping from a data source",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_sources_remove_mapping,
        .requires_auth = true,
    };
    ts_api_register(&ep_sources_remove_mapping);

    ts_api_endpoint_t ep_sources_enable = {
        .name = "automation.sources.enable",
        .description = "Enable a data source",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_sources_enable,
        .requires_auth = true,
    };
    ts_api_register(&ep_sources_enable);

    ts_api_endpoint_t ep_sources_disable = {
        .name = "automation.sources.disable",
        .description = "Disable a data source",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_sources_disable,
        .requires_auth = true,
    };
    ts_api_register(&ep_sources_disable);

    // Test action APIs
    ts_api_endpoint_t ep_test_led = {
        .name = "automation.test.led",
        .description = "Test LED action",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_test_led,
        .requires_auth = true,
    };
    ts_api_register(&ep_test_led);

    ts_api_endpoint_t ep_test_gpio = {
        .name = "automation.test.gpio",
        .description = "Test GPIO action",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_test_gpio,
        .requires_auth = true,
    };
    ts_api_register(&ep_test_gpio);

    ts_api_endpoint_t ep_test_device = {
        .name = "automation.test.device",
        .description = "Test device control action",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_test_device,
        .requires_auth = true,
    };
    ts_api_register(&ep_test_device);

    ts_api_endpoint_t ep_test_ssh = {
        .name = "automation.test.ssh",
        .description = "Test SSH command execution",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_test_ssh,
        .requires_auth = true,
    };
    ts_api_register(&ep_test_ssh);

    // Action Template APIs
    ts_api_endpoint_t ep_actions_list = {
        .name = "automation.actions.list",
        .description = "List all action templates",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_actions_list,
        .requires_auth = false,
    };
    ts_api_register(&ep_actions_list);

    ts_api_endpoint_t ep_actions_get = {
        .name = "automation.actions.get",
        .description = "Get action template by ID",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_actions_get,
        .requires_auth = false,
    };
    ts_api_register(&ep_actions_get);

    ts_api_endpoint_t ep_actions_add = {
        .name = "automation.actions.add",
        .description = "Create a new action template",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_actions_add,
        .requires_auth = true,
    };
    ts_api_register(&ep_actions_add);

    ts_api_endpoint_t ep_actions_delete = {
        .name = "automation.actions.delete",
        .description = "Delete an action template",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_actions_delete,
        .requires_auth = true,
    };
    ts_api_register(&ep_actions_delete);

    ts_api_endpoint_t ep_actions_execute = {
        .name = "automation.actions.execute",
        .description = "Execute an action template",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_actions_execute,
        .requires_auth = true,
    };
    ts_api_register(&ep_actions_execute);

    // Action Statistics APIs
    ts_api_endpoint_t ep_action_stats = {
        .name = "automation.action.stats",
        .description = "Get action execution statistics",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_action_stats,
        .requires_auth = false,
    };
    ts_api_register(&ep_action_stats);

    ts_api_endpoint_t ep_action_stats_reset = {
        .name = "automation.action.stats.reset",
        .description = "Reset action statistics",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_action_stats_reset,
        .requires_auth = true,
    };
    ts_api_register(&ep_action_stats_reset);

    // Proxy APIs for external connection test
    ts_api_endpoint_t ep_proxy_fetch = {
        .name = "automation.proxy.fetch",
        .description = "Proxy fetch from external REST API",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_proxy_fetch,
        .requires_auth = false,
    };
    ts_api_register(&ep_proxy_fetch);

    ts_api_endpoint_t ep_proxy_ws = {
        .name = "automation.proxy.websocket_test",
        .description = "Test WebSocket connection",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_proxy_ws_test,
        .requires_auth = false,
    };
    ts_api_register(&ep_proxy_ws);

    ts_api_endpoint_t ep_proxy_sio = {
        .name = "automation.proxy.socketio_test",
        .description = "Test Socket.IO connection (v4 protocol)",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_proxy_socketio_test,
        .requires_auth = false,
    };
    ts_api_register(&ep_proxy_sio);

    // Export/Import APIs - Sources
    ts_api_endpoint_t ep_sources_export = {
        .name = "automation.sources.export",
        .description = "Export data source as config pack (.tscfg)",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_sources_export,
        .requires_auth = true,
    };
    ts_api_register(&ep_sources_export);

    ts_api_endpoint_t ep_sources_import = {
        .name = "automation.sources.import",
        .description = "Import data source from config pack",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_sources_import,
        .requires_auth = true,
    };
    ts_api_register(&ep_sources_import);

    // Export/Import APIs - Rules
    ts_api_endpoint_t ep_rules_export = {
        .name = "automation.rules.export",
        .description = "Export rule as config pack (.tscfg)",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_rules_export,
        .requires_auth = true,
    };
    ts_api_register(&ep_rules_export);

    ts_api_endpoint_t ep_rules_import = {
        .name = "automation.rules.import",
        .description = "Import rule from config pack",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_rules_import,
        .requires_auth = true,
    };
    ts_api_register(&ep_rules_import);

    // Export/Import APIs - Action Templates
    ts_api_endpoint_t ep_actions_export = {
        .name = "automation.actions.export",
        .description = "Export action template as config pack (.tscfg)",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_actions_export,
        .requires_auth = true,
    };
    ts_api_register(&ep_actions_export);

    ts_api_endpoint_t ep_actions_import = {
        .name = "automation.actions.import",
        .description = "Import action template from config pack",
        .category = TS_API_CAT_SYSTEM,
        .handler = api_automation_actions_import,
        .requires_auth = true,
    };
    ts_api_register(&ep_actions_import);

    TS_LOGI(TAG, "Automation APIs registered");
    return ESP_OK;
}
