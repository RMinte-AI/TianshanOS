#include "ts_automation_document.h"
#include <stdio.h>
#include <string.h>

static esp_err_t fail(ts_auto_config_error_t *error, const char *reason, esp_err_t code)
{
    if (error) error->reason = reason;
    return code;
}

static const char *string(const cJSON *object, const char *key, bool required, bool *known)
{
    if (!cJSON_IsObject(object)) { *known = false; return ""; }
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value && !required) return "";
    if (!cJSON_IsString(value)) { *known = false; return ""; }
    return value->valuestring;
}

static const cJSON *find(const cJSON *json, ts_auto_config_kind_t kind,
                         const char *id, bool *known)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(json, ts_auto_config_collection(kind));
    if (!cJSON_IsArray(array)) { *known = false; return NULL; }
    const cJSON *found = NULL, *item;
    cJSON_ArrayForEach(item, (cJSON_IsArray(array) ? array : NULL)) {
        const char *name = string(item, kind == TS_AUTO_CONFIG_VARIABLE ? "name" : "id", true, known);
        if (strcmp(name, id)) continue;
        if (found) *known = false;
        found = item;
    }
    return found;
}

static bool command_depends(const cJSON *command, ts_auto_config_kind_t kind,
                             const char *id, bool *known)
{
    return !strcmp(string(command, kind == TS_AUTO_CONFIG_HOST ? "host_id" : "id", true, known), id);
}

static bool command_ref(const cJSON *json, const char *command, ts_auto_config_kind_t kind,
                         const char *id, bool *known)
{
    if (kind == TS_AUTO_CONFIG_COMMAND) return !strcmp(command, id);
    if (kind != TS_AUTO_CONFIG_HOST) return false;
    const cJSON *definition = find(json, TS_AUTO_CONFIG_COMMAND, command, known);
    if (!definition) { *known = false; return false; }
    return command_depends(definition, kind, id, known);
}

static bool template_depends(const cJSON *json, const cJSON *action,
                             ts_auto_config_kind_t kind, const char *id, bool *known)
{
    if (kind == TS_AUTO_CONFIG_ACTION)
        return !strcmp(string(action, "id", true, known), id);
    const char *type = string(action, "type", true, known);
    if (!strcmp(type, "ssh_cmd_ref")) {
        const cJSON *ref = cJSON_GetObjectItemCaseSensitive(action, "ssh_ref");
        return command_ref(json, string(ref, "cmd_id", true, known), kind, id, known);
    }
    if (!strcmp(type, "ssh_cmd") && kind == TS_AUTO_CONFIG_HOST) {
        const cJSON *ssh = cJSON_GetObjectItemCaseSensitive(action, "ssh");
        return !strcmp(string(ssh, "host_ref", true, known), id);
    }
    const char *const types[] = {"led","cli","ssh_cmd","gpio","webhook","log","set_var","device_ctrl"};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        if (!strcmp(type, types[i])) return false;
    *known = false;
    return false;
}

static bool step_depends(const cJSON *json, const cJSON *step,
                         ts_auto_config_kind_t kind, const char *id, bool *known)
{
    const char *template_id = string(step, "template_id", false, known);
    if (template_id[0]) {
        if (kind == TS_AUTO_CONFIG_ACTION) return !strcmp(template_id, id);
        const cJSON *action = find(json, TS_AUTO_CONFIG_ACTION, template_id, known);
        if (!action) { *known = false; return false; }
        return template_depends(json, action, kind, id, known);
    }
    const char *type = string(step, "type", true, known);
    if (!strcmp(type, "ssh_cmd_ref"))
        return command_ref(json, string(step, "cmd_id", true, known), kind, id, known);
    if ((!strcmp(type, "ssh") || !strcmp(type, "ssh_cmd")) && kind == TS_AUTO_CONFIG_HOST)
        return !strcmp(string(step, "host_ref", true, known), id);
    const char *const types[] = {"led","cli","ssh","ssh_cmd","gpio","webhook","log","set_var","device_ctrl","device"};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        if (!strcmp(type, types[i])) return false;
    *known = false;
    return false;
}

static bool set(cJSON *json, const char *name, cJSON *value)
{
    if (!value) return false;
    bool ok = cJSON_HasObjectItem(json, name) ?
        cJSON_ReplaceItemInObjectCaseSensitive(json, name, value) :
        cJSON_AddItemToObject(json, name, value);
    if (!ok) cJSON_Delete(value);
    return ok;
}

static bool row(cJSON *array, const cJSON *object)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
    if (!cJSON_IsString(id)) return true;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(object, "name");
    cJSON *item = cJSON_CreateObject();
    if (!item) return false;
    if (!cJSON_AddStringToObject(item, "id", id->valuestring) ||
        !cJSON_AddStringToObject(item, "name", cJSON_IsString(name) ? name->valuestring : id->valuestring) ||
        !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static int targets(const cJSON *json, ts_auto_config_kind_t kind, const char *id, bool *known)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(json, ts_auto_config_collection(kind));
    if (!cJSON_IsArray(array)) { *known = false; return 0; }
    int count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, (cJSON_IsArray(array) ? array : NULL))
        count += !strcmp(string(item, kind == TS_AUTO_CONFIG_VARIABLE ? "name" : "id", true, known), id);
    if (count > 1) *known = false;
    return count;
}

esp_err_t ts_auto_document_impact(const cJSON *json, ts_auto_config_kind_t kind,
                                 const char *id, cJSON **impact, ts_auto_config_error_t *error)
{
    if (!impact || !cJSON_IsObject(json) || (unsigned)kind >= TS_AUTO_CONFIG_KIND_COUNT || !id || !id[0])
        return ESP_ERR_INVALID_ARG;
    *impact = NULL;
    if (error) {
        memset(error, 0, sizeof(*error));
        error->collection = ts_auto_config_collection(kind);
        snprintf(error->id, sizeof(error->id), "%s", id);
    }
    bool known = true;
    int count = targets(json, kind, id, &known);
    if (!count) return fail(error, "not_found", ESP_ERR_NOT_FOUND);
    cJSON *result = cJSON_CreateObject();
    if (!result) return fail(error, "out_of_memory", ESP_ERR_NO_MEM);
    cJSON *rules = cJSON_AddArrayToObject(result, "rules");
    cJSON *actions = cJSON_AddArrayToObject(result, "actions");
    cJSON *commands = cJSON_AddArrayToObject(result, "commands");
    cJSON *sources = cJSON_AddArrayToObject(result, "sources");
    if (!rules || !actions || !commands || !sources ||
        !cJSON_AddNumberToObject(result, "target_count", count)) goto oom;
    if (kind == TS_AUTO_CONFIG_ACTION || kind == TS_AUTO_CONFIG_COMMAND || kind == TS_AUTO_CONFIG_HOST) {
        const cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "rules"), *rule;
        if (!cJSON_IsArray(array)) known = false;
        cJSON_ArrayForEach(rule, (cJSON_IsArray(array) ? array : NULL)) {
            const cJSON *steps = cJSON_GetObjectItemCaseSensitive(rule, "actions"), *step;
            bool affected = false;
            if (!cJSON_IsArray(steps)) known = false;
            cJSON_ArrayForEach(step, (cJSON_IsArray(steps) ? steps : NULL))
                affected |= step_depends(json, step, kind, id, &known);
            if (affected && !row(rules, rule)) goto oom;
        }
        array = cJSON_GetObjectItemCaseSensitive(json, "actions");
        const cJSON *action;
        if (!cJSON_IsArray(array)) known = false;
        if (kind != TS_AUTO_CONFIG_ACTION)
            cJSON_ArrayForEach(action, (cJSON_IsArray(array) ? array : NULL))
                if (template_depends(json, action, kind, id, &known) && !row(actions, action)) goto oom;
    }
    if (kind == TS_AUTO_CONFIG_HOST) {
        const cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "commands"), *command;
        if (!cJSON_IsArray(array)) known = false;
        cJSON_ArrayForEach(command, (cJSON_IsArray(array) ? array : NULL))
            if (command_depends(command, kind, id, &known) && !row(commands, command)) goto oom;
        array = cJSON_GetObjectItemCaseSensitive(json, "sources");
        const cJSON *source;
        if (!cJSON_IsArray(array)) known = false;
        cJSON_ArrayForEach(source, (cJSON_IsArray(array) ? array : NULL))
            if (!strcmp(string(source, "ssh_host_id", false, &known), id) && !row(sources, source)) goto oom;
    }
    if (!cJSON_AddBoolToObject(result, "references_confirmed", known) ||
        !cJSON_AddBoolToObject(result, "requires_pause", !known)) goto oom;
    *impact = result;
    return ESP_OK;
oom:
    cJSON_Delete(result);
    return fail(error, "out_of_memory", ESP_ERR_NO_MEM);
}

static esp_err_t change_references(cJSON *json, ts_auto_config_kind_t kind,
                                   const char *id, const char *replacement)
{
    bool known = true;
    cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "rules"), *rule;
    /* Determine rule effects before changing the templates or commands. */
    cJSON_ArrayForEach(rule, (cJSON_IsArray(array) ? array : NULL)) {
        cJSON *steps = cJSON_GetObjectItemCaseSensitive(rule, "actions");
        if (!cJSON_IsArray(steps)) continue;
        for (int i = cJSON_GetArraySize(steps) - 1; i >= 0; --i) {
            cJSON *step = cJSON_GetArrayItem(steps, i);
            if (!step_depends(json, step, kind, id, &known)) continue;
            cJSON_DeleteItemFromObjectCaseSensitive(rule, "_package");
            if (replacement) {
                const char *template_id = string(step, "template_id", false, &known);
                if (kind == TS_AUTO_CONFIG_ACTION) {
                    if (!set(step, "template_id", cJSON_CreateString(replacement))) return ESP_ERR_NO_MEM;
                } else if (!template_id[0] && kind == TS_AUTO_CONFIG_COMMAND) {
                    if (!set(step, "cmd_id", cJSON_CreateString(replacement))) return ESP_ERR_NO_MEM;
                } else if (!template_id[0] && kind == TS_AUTO_CONFIG_HOST &&
                           strcmp(string(step, "type", true, &known), "ssh_cmd_ref")) {
                    if (!set(step, "host_ref", cJSON_CreateString(replacement))) return ESP_ERR_NO_MEM;
                }
            } else {
                cJSON_DeleteItemFromArray(steps, i);
                if (!set(rule, "enabled", cJSON_CreateFalse())) return ESP_ERR_NO_MEM;
            }
        }
    }
    array = cJSON_GetObjectItemCaseSensitive(json, "actions");
    cJSON *action;
    cJSON_ArrayForEach(action, (cJSON_IsArray(array) ? array : NULL)) {
        if (kind == TS_AUTO_CONFIG_ACTION || !template_depends(json, action, kind, id, &known)) continue;
        const char *type = string(action, "type", true, &known);
        bool command_reference = !strcmp(type, "ssh_cmd_ref");
        cJSON *parameters = cJSON_GetObjectItemCaseSensitive(action, command_reference ? "ssh_ref" : "ssh");
        if (replacement) {
            if (command_reference && kind == TS_AUTO_CONFIG_COMMAND) {
                if (!set(parameters, "cmd_id", cJSON_CreateString(replacement))) return ESP_ERR_NO_MEM;
            } else if (!command_reference && kind == TS_AUTO_CONFIG_HOST) {
                if (!set(parameters, "host_ref", cJSON_CreateString(replacement))) return ESP_ERR_NO_MEM;
            }
        } else {
            if (!set(action, "enabled", cJSON_CreateFalse()) ||
                !set(parameters, command_reference ? "cmd_id" : "host_ref", cJSON_CreateString("")))
                return ESP_ERR_NO_MEM;
        }
    }
    if (kind == TS_AUTO_CONFIG_HOST) {
        array = cJSON_GetObjectItemCaseSensitive(json, "commands");
        cJSON *command;
        cJSON_ArrayForEach(command, (cJSON_IsArray(array) ? array : NULL)) {
            if (!command_depends(command, kind, id, &known)) continue;
            if (!set(command, "host_id", cJSON_CreateString(replacement ? replacement : "")) ||
                (!replacement && !set(command, "enabled", cJSON_CreateFalse()))) return ESP_ERR_NO_MEM;
        }
        array = cJSON_GetObjectItemCaseSensitive(json, "sources");
        cJSON *source;
        cJSON_ArrayForEach(source, (cJSON_IsArray(array) ? array : NULL)) {
            if (strcmp(string(source, "ssh_host_id", false, &known), id)) continue;
            if (!set(source, "ssh_host_id", cJSON_CreateString(replacement ? replacement : "")) ||
                (!replacement && !set(source, "enabled", cJSON_CreateFalse()))) return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t ts_auto_document_remove(cJSON *json, ts_auto_config_kind_t kind, const char *id,
                                 ts_auto_delete_mode_t mode, const char *replacement,
                                 bool pause_confirmed, cJSON **impact, ts_auto_config_error_t *error)
{
    if (impact) *impact = NULL;
    if (!cJSON_IsObject(json) || !id || !id[0] || (unsigned)kind >= TS_AUTO_CONFIG_KIND_COUNT ||
        (unsigned)mode > TS_AUTO_DELETE_FORCE) return ESP_ERR_INVALID_ARG;
    bool known = true;
    if (!targets(json, kind, id, &known)) return fail(error, "not_found", ESP_ERR_NOT_FOUND);
    cJSON *plan = NULL;
    bool forced_pause = mode == TS_AUTO_DELETE_FORCE && pause_confirmed;
    if (!forced_pause) {
        esp_err_t ret = ts_auto_document_impact(json, kind, id, &plan, error);
        if (ret != ESP_OK)
            return mode == TS_AUTO_DELETE_FORCE ? fail(error, "pause_required", ESP_ERR_INVALID_STATE) : ret;
        const cJSON *confirmed = cJSON_GetObjectItemCaseSensitive(plan, "references_confirmed");
        bool references = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(plan, "rules")) ||
                          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(plan, "actions")) ||
                          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(plan, "commands")) ||
                          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(plan, "sources"));
        if (impact) *impact = plan;
        if (!cJSON_IsTrue(confirmed)) {
            if (!impact) cJSON_Delete(plan);
            return fail(error, mode == TS_AUTO_DELETE_FORCE ? "pause_required" : "reference_check_failed",
                        ESP_ERR_INVALID_STATE);
        }
        if (mode == TS_AUTO_DELETE_NORMAL && references) {
            if (!impact) cJSON_Delete(plan);
            return fail(error, "references_exist", ESP_ERR_INVALID_STATE);
        }
    }
    if (mode == TS_AUTO_DELETE_REPLACE) {
        const cJSON *next = replacement && strcmp(replacement, id) ? find(json, kind, replacement, &known) : NULL;
        if (!next) {
            if (!impact) cJSON_Delete(plan);
            return fail(error, "replacement_missing", ESP_ERR_NOT_FOUND);
        }
    }
    esp_err_t ret = ESP_OK;
    if (kind == TS_AUTO_CONFIG_ACTION || kind == TS_AUTO_CONFIG_COMMAND || kind == TS_AUTO_CONFIG_HOST)
        ret = change_references(json, kind, id, mode == TS_AUTO_DELETE_REPLACE ? replacement : NULL);
    if (ret == ESP_OK) {
        cJSON *array = cJSON_GetObjectItemCaseSensitive(json, ts_auto_config_collection(kind));
        for (int i = cJSON_GetArraySize(array) - 1; i >= 0; --i) {
            cJSON *item = cJSON_GetArrayItem(array, i);
            if (!strcmp(string(item, kind == TS_AUTO_CONFIG_VARIABLE ? "name" : "id", true, &known), id))
                cJSON_DeleteItemFromArray(array, i);
        }
        if (forced_pause &&
            (!set(json, "enabled", cJSON_CreateFalse()) ||
             !set(json, "pause_reason", cJSON_CreateString("references_unavailable"))))
            ret = ESP_ERR_NO_MEM;
    }
    if (!impact) cJSON_Delete(plan);
    return ret == ESP_OK ? ret : fail(error, "out_of_memory", ret);
}
