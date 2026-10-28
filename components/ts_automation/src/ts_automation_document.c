#include "ts_automation_document.h"
#include "ts_action_codec.h"
#include "ts_rule_codec.h"
#include "ts_ssh_config_codec.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_TS_AUTOMATION_MAX_SOURCES
#define CONFIG_TS_AUTOMATION_MAX_SOURCES 16
#endif
#ifndef CONFIG_TS_AUTOMATION_MAX_VARIABLES
#define CONFIG_TS_AUTOMATION_MAX_VARIABLES 512
#endif

static const char *const collections[] = {
    "rules", "actions", "commands", "hosts", "sources", "variables"
};
static const int limits[] = {64, TS_ACTION_TEMPLATE_MAX, TS_SSH_COMMANDS_MAX,
                            TS_SSH_HOSTS_MAX, CONFIG_TS_AUTOMATION_MAX_SOURCES,
                            CONFIG_TS_AUTOMATION_MAX_VARIABLES};

const char *ts_auto_config_collection(ts_auto_config_kind_t kind)
{
    return (unsigned)kind < TS_AUTO_CONFIG_KIND_COUNT ? collections[kind] : NULL;
}

int ts_auto_config_limit(ts_auto_config_kind_t kind)
{
    return (unsigned)kind < TS_AUTO_CONFIG_KIND_COUNT ? limits[kind] : 0;
}

static esp_err_t error_at(ts_auto_config_error_t *error, const char *reason,
                          ts_auto_config_kind_t kind, const char *id, const char *dependency,
                          esp_err_t code)
{
    if (error) {
        error->reason = reason;
        error->collection = ts_auto_config_collection(kind);
        snprintf(error->id, sizeof(error->id), "%s", id ? id : "");
        snprintf(error->dependency, sizeof(error->dependency), "%s", dependency ? dependency : "");
    }
    return code;
}

static bool integer(const cJSON *value, uint32_t max, uint32_t *out)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > max ||
        trunc(value->valuedouble) != value->valuedouble) return false;
    *out = (uint32_t)value->valuedouble;
    return true;
}

static bool text(const cJSON *json, const char *name, char *out, size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!value) { out[0] = 0; return true; }
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= capacity) return false;
    strcpy(out, value->valuestring);
    return true;
}

static esp_err_t variable_decode(const cJSON *json, ts_auto_variable_t *value)
{
    uint32_t flags = 0;
    const cJSON *flag = cJSON_GetObjectItemCaseSensitive(json, "flags");
    if (!text(json, "name", value->name, sizeof(value->name)) ||
        !ts_rule_id_valid(value->name) ||
        !text(json, "source_id", value->source_id, sizeof(value->source_id)) ||
        !text(json, "expression", value->expression, sizeof(value->expression)) ||
        (flag && !integer(flag, TS_AUTO_VAR_PERSISTENT | TS_AUTO_VAR_READONLY |
                              TS_AUTO_VAR_COMPUTED, &flags)) ||
        !ts_auto_value_decode(json, &value->default_value)) return ESP_ERR_INVALID_ARG;
    value->flags = flags;
    value->value = value->default_value;
    return ESP_OK;
}

static cJSON *variable_encode(const ts_auto_variable_t *value)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    if (!cJSON_AddStringToObject(json, "name", value->name) ||
        !cJSON_AddStringToObject(json, "source_id", value->source_id) ||
        !cJSON_AddStringToObject(json, "expression", value->expression) ||
        !cJSON_AddNumberToObject(json, "flags", value->flags) ||
        !cJSON_AddNumberToObject(json, "value_type", value->default_value.type)) goto fail;
    cJSON *initial = ts_auto_value_encode(&value->default_value);
    if (!initial) goto fail;
    if (!cJSON_AddItemToObject(json, "value", initial)) { cJSON_Delete(initial); goto fail; }
    return json;
fail:
    cJSON_Delete(json);
    return NULL;
}

static int find_id(const void *items, int count, size_t stride, const char *id)
{
    /* Every definition begins with its stable ID (variable definitions: name). */
    for (int i = 0; i < count; ++i)
        if (!strcmp((const char *)items + i * stride, id)) return i;
    return -1;
}

static esp_err_t decode_item(ts_auto_config_kind_t kind, const cJSON *json, void *out)
{
    if (!cJSON_IsObject(json)) return ESP_ERR_INVALID_ARG;
    if (kind == TS_AUTO_CONFIG_RULE) return ts_rule_decode(json, out);
    if (kind == TS_AUTO_CONFIG_COMMAND) return ts_ssh_command_decode(json, out);
    if (kind == TS_AUTO_CONFIG_HOST) return ts_ssh_host_decode(json, out);
    if (kind == TS_AUTO_CONFIG_VARIABLE) return variable_decode(json, out);
    char *raw = cJSON_PrintUnformatted(json);
    if (!raw) return ESP_ERR_NO_MEM;
    esp_err_t ret = kind == TS_AUTO_CONFIG_ACTION ? ts_action_template_decode(raw, out)
                                                : ts_source_config_decode(raw, out);
    free(raw);
    return ret;
}

esp_err_t ts_auto_document_check_item(ts_auto_config_kind_t kind, const cJSON *json,
                                     ts_auto_config_error_t *error)
{
    if ((unsigned)kind >= TS_AUTO_CONFIG_KIND_COUNT || !cJSON_IsObject(json))
        return error_at(error, "configuration_invalid", kind, NULL, NULL, ESP_ERR_INVALID_ARG);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json,
        kind == TS_AUTO_CONFIG_VARIABLE ? "name" : "id");
    const char *name = cJSON_IsString(id) ? id->valuestring : "";
    size_t capacity = kind == TS_AUTO_CONFIG_COMMAND ? TS_SSH_CMD_ID_MAX :
                      kind == TS_AUTO_CONFIG_HOST ? TS_SSH_HOST_ID_MAX : TS_AUTO_NAME_MAX_LEN;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    uint32_t source_type;
    if (!ts_rule_id_valid(name) || strlen(name) >= capacity ||
        (kind == TS_AUTO_CONFIG_ACTION && !cJSON_IsString(type)) ||
        (kind == TS_AUTO_CONFIG_SOURCE && !integer(type, TS_AUTO_SRC_VARIABLE, &source_type)))
        return error_at(error, "configuration_invalid", kind, name, NULL, ESP_ERR_INVALID_ARG);
    const size_t sizes[] = {sizeof(ts_auto_rule_t), sizeof(ts_action_template_t),
        sizeof(ts_ssh_command_config_t), sizeof(ts_ssh_host_config_t),
        sizeof(ts_auto_source_t), sizeof(ts_auto_variable_t)};
    void *value = heap_caps_calloc(1, sizes[kind], MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!value) return error_at(error, "out_of_memory", kind, name, NULL, ESP_ERR_NO_MEM);
    esp_err_t ret = decode_item(kind, json, value);
    if (kind == TS_AUTO_CONFIG_RULE) ts_rule_dispose(value);
    free(value);
    if (ret != ESP_OK)
        error_at(error, ret == ESP_ERR_NO_MEM ? "out_of_memory" : "configuration_invalid",
                 kind, name, NULL, ret);
    return ret;
}

static void resolve_presentation(ts_auto_document_t *document)
{
    for (int i = 0; i < document->rule_count; ++i) {
        ts_auto_rule_t *rule = &document->rules[i];
        bool service = false, unresolved = false;
        for (int j = 0; j < rule->action_count; ++j) {
            const ts_auto_action_t *action = &rule->actions[j];
            if (action->template_id[0]) {
                int index = find_id(document->actions, document->action_count,
                                    sizeof(*document->actions), action->template_id);
                if (index < 0) { unresolved = true; continue; }
                action = &document->actions[index].action;
            }
            if (action->type == TS_AUTO_ACT_SSH_CMD_REF) {
                int index = find_id(document->commands, document->command_count,
                                    sizeof(*document->commands), action->ssh_ref.cmd_id);
                if (index < 0) unresolved = true;
                else service |= document->commands[index].service_mode;
            }
        }
        if (!(rule->presentation_fields & 1))
            rule->show_on_dashboard = (rule->enabled && rule->manual_trigger) || service;
        if (!(rule->presentation_fields & 2))
            rule->allow_manual_trigger = rule->manual_trigger || service;
        rule->reference_unresolved = unresolved;
    }
}

void ts_auto_document_free(ts_auto_document_t *document)
{
    if (!document) return;
    if (document->rules)
        for (int i = 0; i < document->rule_count; ++i) ts_rule_dispose(&document->rules[i]);
    if (document->rule_packages)
        for (int i = 0; i < document->rule_count; ++i) cJSON_Delete(document->rule_packages[i]);
    free(document->rule_packages);
    free(document->rules);
    free(document->actions);
    free(document->commands);
    free(document->hosts);
    free(document->sources);
    free(document->variables);
    free(document);
}

esp_err_t ts_auto_document_decode(const cJSON *json, ts_auto_document_t **out,
                                 ts_auto_config_error_t *error)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (error) memset(error, 0, sizeof(*error));
    uint32_t format;
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (!cJSON_IsObject(json) ||
        !integer(cJSON_GetObjectItemCaseSensitive(json, "format_version"), 1, &format) ||
        format != 1 || !cJSON_IsBool(enabled))
        return error_at(error, "configuration_invalid", TS_AUTO_CONFIG_KIND_COUNT,
                        NULL, NULL, ESP_ERR_INVALID_ARG);
    ts_auto_document_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) return error_at(error, "out_of_memory", TS_AUTO_CONFIG_KIND_COUNT,
                            NULL, NULL, ESP_ERR_NO_MEM);
    d->enabled = cJSON_IsTrue(enabled);
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    const cJSON *migration = cJSON_GetObjectItemCaseSensitive(json, "migration_version");
    if (!integer(cJSON_GetObjectItemCaseSensitive(json, "revision"), UINT32_MAX, &d->revision) ||
        !integer(cJSON_GetObjectItemCaseSensitive(json, "saved_at"), UINT32_MAX, &d->saved_at) ||
        (migration && !integer(migration, UINT32_MAX, &d->migration_version)) ||
        !text(json, "pause_reason", d->pause_reason, sizeof(d->pause_reason)) ||
        (d->enabled && d->pause_reason[0])) {
        error_at(error, "configuration_invalid", TS_AUTO_CONFIG_KIND_COUNT, NULL, NULL, ret);
        goto fail;
    }
    const size_t sizes[] = {sizeof(*d->rules), sizeof(*d->actions), sizeof(*d->commands),
                           sizeof(*d->hosts), sizeof(*d->sources), sizeof(*d->variables)};
    void *arrays[TS_AUTO_CONFIG_KIND_COUNT] = {0};
    int *counts[] = {&d->rule_count, &d->action_count, &d->command_count,
                    &d->host_count, &d->source_count, &d->variable_count};
    for (int kind = 0; kind < TS_AUTO_CONFIG_KIND_COUNT; ++kind) {
        const cJSON *array = cJSON_GetObjectItemCaseSensitive(json, collections[kind]);
        if (!cJSON_IsArray(array)) {
            ret = error_at(error, "configuration_invalid", kind, NULL, NULL, ESP_ERR_INVALID_ARG);
            goto fail;
        }
        int count = cJSON_GetArraySize(array);
        if (count > limits[kind]) {
            error_at(error, kind == TS_AUTO_CONFIG_RULE ? "too_many_rules" : "too_many_items",
                     kind, NULL, NULL, ESP_ERR_INVALID_SIZE);
            ret = ESP_ERR_INVALID_SIZE;
            goto fail;
        }
        *counts[kind] = count;
        if (count) {
            arrays[kind] = heap_caps_calloc(count, sizes[kind], MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!arrays[kind]) {
                ret = error_at(error, "out_of_memory", kind, NULL, NULL, ESP_ERR_NO_MEM);
                goto fail;
            }
            switch (kind) {
            case TS_AUTO_CONFIG_RULE:
                d->rules = arrays[kind];
                d->rule_packages = heap_caps_calloc(count, sizeof(*d->rule_packages), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!d->rule_packages) { ret = error_at(error, "out_of_memory", kind, NULL, NULL, ESP_ERR_NO_MEM);goto fail; }
                break;
            case TS_AUTO_CONFIG_ACTION: d->actions = arrays[kind]; break;
            case TS_AUTO_CONFIG_COMMAND: d->commands = arrays[kind]; break;
            case TS_AUTO_CONFIG_HOST: d->hosts = arrays[kind]; break;
            case TS_AUTO_CONFIG_SOURCE: d->sources = arrays[kind]; break;
            case TS_AUTO_CONFIG_VARIABLE: d->variables = arrays[kind]; break;
            }
        }
        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem(array, i);
            const cJSON *package = kind == TS_AUTO_CONFIG_RULE ? cJSON_GetObjectItemCaseSensitive(item, "_package") : NULL;
            if (cJSON_IsObject(package)) {
                d->rule_packages[i] = cJSON_Duplicate(package, true);
                if (!d->rule_packages[i]) { ret = error_at(error, "out_of_memory", kind, NULL, NULL, ESP_ERR_NO_MEM);goto fail; }
            }
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(item,
                kind == TS_AUTO_CONFIG_VARIABLE ? "name" : "id");
            const char *name = cJSON_IsString(id) ? id->valuestring : "";
            size_t id_capacity = kind == TS_AUTO_CONFIG_COMMAND ? TS_SSH_CMD_ID_MAX :
                                 kind == TS_AUTO_CONFIG_HOST ? TS_SSH_HOST_ID_MAX : TS_AUTO_NAME_MAX_LEN;
            if (!ts_rule_id_valid(name) || strlen(name) >= id_capacity) {
                ret = error_at(error, "configuration_invalid", kind, name, NULL, ESP_ERR_INVALID_ARG);
                goto fail;
            }
            if (find_id(arrays[kind], i, sizes[kind], name) >= 0) {
                ret = error_at(error, "duplicate_id", kind, name, NULL, ESP_ERR_INVALID_ARG);
                goto fail;
            }
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
            uint32_t source_type;
            if ((kind == TS_AUTO_CONFIG_ACTION && !cJSON_IsString(type)) ||
                (kind == TS_AUTO_CONFIG_SOURCE && !integer(type, TS_AUTO_SRC_VARIABLE, &source_type))) {
                ret = error_at(error, "configuration_invalid", kind, name, NULL, ESP_ERR_INVALID_ARG);
                goto fail;
            }
            ret = decode_item(kind, item, (char *)arrays[kind] + i * sizes[kind]);
            if (ret != ESP_OK) {
                error_at(error, ret == ESP_ERR_NO_MEM ? "out_of_memory" : "configuration_invalid",
                         kind, name, NULL, ret);
                goto fail;
            }
        }
    }
    resolve_presentation(d);
    *out = d;
    return ESP_OK;
fail:
    ts_auto_document_free(d);
    return ret;
}

static esp_err_t action_dependency(const ts_auto_document_t *d, const ts_auto_action_t *action,
                                   ts_auto_config_kind_t owner, const char *id,
                                   ts_auto_config_error_t *error)
{
    if (action->template_id[0]) {
        int index = find_id(d->actions, d->action_count, sizeof(*d->actions), action->template_id);
        if (index < 0 || !d->actions[index].enabled)
            return error_at(error, index < 0 ? "dependency_missing" : "dependency_disabled",
                            owner, id, action->template_id, ESP_ERR_INVALID_STATE);
        action = &d->actions[index].action;
    }
    if (action->type == TS_AUTO_ACT_SSH_CMD_REF) {
        int index = find_id(d->commands, d->command_count, sizeof(*d->commands), action->ssh_ref.cmd_id);
        if (index < 0 || !d->commands[index].enabled)
            return error_at(error, index < 0 ? "dependency_missing" : "dependency_disabled",
                            owner, id, action->ssh_ref.cmd_id, ESP_ERR_INVALID_STATE);
    } else if (action->type == TS_AUTO_ACT_SSH_CMD) {
        int index = find_id(d->hosts, d->host_count, sizeof(*d->hosts), action->ssh.host_ref);
        if (index < 0 || !d->hosts[index].enabled)
            return error_at(error, index < 0 ? "dependency_missing" : "dependency_disabled",
                            owner, id, action->ssh.host_ref, ESP_ERR_INVALID_STATE);
    } else if (action->type == TS_AUTO_ACT_TEMPLATE_REF)
        return error_at(error, "dependency_missing", owner, id, action->template_id, ESP_ERR_INVALID_STATE);
    return ESP_OK;
}

esp_err_t ts_auto_document_validate(const ts_auto_document_t *d, ts_auto_config_error_t *error)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    if (error) memset(error, 0, sizeof(*error));
    for (int i = 0; i < d->command_count; ++i) {
        const ts_ssh_command_config_t *command = &d->commands[i];
        if (!command->enabled) continue;
        int index = find_id(d->hosts, d->host_count, sizeof(*d->hosts), command->host_id);
        if (index < 0 || !d->hosts[index].enabled)
            return error_at(error, index < 0 ? "dependency_missing" : "dependency_disabled",
                            TS_AUTO_CONFIG_COMMAND, command->id, command->host_id, ESP_ERR_INVALID_STATE);
        if (!command->name[0] || !command->command[0])
            return error_at(error, "configuration_invalid", TS_AUTO_CONFIG_COMMAND,
                            command->id, NULL, ESP_ERR_INVALID_ARG);
    }
    for (int i = 0; i < d->host_count; ++i) {
        const ts_ssh_host_config_t *host = &d->hosts[i];
        if (host->enabled && (!host->host[0] || !host->username[0] || !host->port))
            return error_at(error, "configuration_invalid", TS_AUTO_CONFIG_HOST, host->id,
                            NULL, ESP_ERR_INVALID_ARG);
    }
    for (int i = 0; i < d->source_count; ++i) {
        const ts_auto_source_t *source = &d->sources[i];
        if (source->type < TS_AUTO_SRC_BUILTIN || source->type > TS_AUTO_SRC_VARIABLE ||
            source->mapping_count > TS_AUTO_MAX_MAPPINGS)
            return error_at(error, "configuration_invalid", TS_AUTO_CONFIG_SOURCE,
                            source->id, NULL, ESP_ERR_INVALID_ARG);
        if (source->enabled && source->type == TS_AUTO_SRC_VARIABLE &&
            source->variable.ssh_host_id[0]) {
            int index = find_id(d->hosts, d->host_count, sizeof(*d->hosts), source->variable.ssh_host_id);
            if (index < 0 || !d->hosts[index].enabled)
                return error_at(error, index < 0 ? "dependency_missing" : "dependency_disabled",
                                TS_AUTO_CONFIG_SOURCE, source->id, source->variable.ssh_host_id,
                                ESP_ERR_INVALID_STATE);
        }
    }
    for (int i = 0; i < d->action_count; ++i) {
        if (!d->actions[i].enabled) continue;
        esp_err_t ret = action_dependency(d, &d->actions[i].action, TS_AUTO_CONFIG_ACTION,
                                          d->actions[i].id, error);
        if (ret != ESP_OK) return ret;
    }
    for (int i = 0; i < d->rule_count; ++i) {
        const ts_auto_rule_t *rule = &d->rules[i];
        if (!rule->enabled) continue;
        if (!rule->action_count)
            return error_at(error, "rule_needs_action", TS_AUTO_CONFIG_RULE,
                            rule->id, NULL, ESP_ERR_INVALID_STATE);
        for (int j = 0; j < rule->action_count; ++j) {
            esp_err_t ret = action_dependency(d, &rule->actions[j], TS_AUTO_CONFIG_RULE, rule->id, error);
            if (ret != ESP_OK) return ret;
        }
    }
    return ESP_OK;
}

cJSON *ts_auto_document_encode(const ts_auto_document_t *d)
{
    if (!d) return NULL;
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    if (!cJSON_AddNumberToObject(json, "format_version", 1) ||
        !cJSON_AddNumberToObject(json, "revision", d->revision) ||
        !cJSON_AddNumberToObject(json, "saved_at", d->saved_at) ||
        !cJSON_AddNumberToObject(json, "migration_version", d->migration_version) ||
        !cJSON_AddBoolToObject(json, "enabled", d->enabled) ||
        !cJSON_AddStringToObject(json, "pause_reason", d->pause_reason)) goto fail;
    const int counts[] = {d->rule_count, d->action_count, d->command_count,
                          d->host_count, d->source_count, d->variable_count};
    for (int kind = 0; kind < TS_AUTO_CONFIG_KIND_COUNT; ++kind) {
        cJSON *array = cJSON_AddArrayToObject(json, collections[kind]);
        if (!array) goto fail;
        for (int i = 0; i < counts[kind]; ++i) {
            cJSON *item = NULL;
            char *raw = NULL;
            switch (kind) {
            case TS_AUTO_CONFIG_RULE:
                item = ts_rule_encode(&d->rules[i]);
                if (item && d->rule_packages && d->rule_packages[i]) {
                    cJSON *package = cJSON_Duplicate(d->rule_packages[i], true);
                    if (!package || !cJSON_AddItemToObject(item, "_package", package)) {
                        cJSON_Delete(package);cJSON_Delete(item);goto fail;
                    }
                }
                break;
            case TS_AUTO_CONFIG_ACTION: raw = ts_action_template_encode(&d->actions[i]); break;
            case TS_AUTO_CONFIG_COMMAND: item = ts_ssh_command_encode(&d->commands[i]); break;
            case TS_AUTO_CONFIG_HOST: item = ts_ssh_host_encode(&d->hosts[i]); break;
            case TS_AUTO_CONFIG_SOURCE: raw = ts_source_config_encode(&d->sources[i]); break;
            case TS_AUTO_CONFIG_VARIABLE: item = variable_encode(&d->variables[i]); break;
            }
            if (raw) { item = cJSON_Parse(raw); free(raw); }
            if (!item) goto fail;
            if (!cJSON_AddItemToArray(array, item)) { cJSON_Delete(item); goto fail; }
        }
    }
    return json;
fail:
    cJSON_Delete(json);
    return NULL;
}
