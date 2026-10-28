#include "ts_ssh_config_codec.h"
#include <string.h>
#include <math.h>

static const cJSON *field(const cJSON *json, const char *name, const char *alias)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    return !value && alias ? cJSON_GetObjectItemCaseSensitive(json, alias) : value;
}

static bool read_string(const cJSON *json, const char *name, const char *alias,
                        char *out, size_t capacity)
{
    const cJSON *value = field(json, name, alias);
    if (!value) return true;
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= capacity) return false;
    strcpy(out, value->valuestring);
    return true;
}

static bool read_unsigned(const cJSON *json, const char *name, const char *alias,
                          uint32_t *out, uint32_t max)
{
    const cJSON *value = field(json, name, alias);
    if (!value) return true;
    double number = value->valuedouble;
    if (!cJSON_IsNumber(value) || !isfinite(number) || number < 0 ||
        number > max || trunc(number) != number) return false;
    *out = number;
    return true;
}

static bool read_bool(const cJSON *json, const char *name, const char *alias, bool *out)
{
    const cJSON *value = field(json, name, alias);
    if (!value) return true;
    if (!cJSON_IsBool(value)) return false;
    *out = cJSON_IsTrue(value);
    return true;
}

/* Keep the existing public aliases and units. Reject incomplete field copies. */
#define STRING(member, alias) do { \
    if (!read_string(obj, #member, alias, cfg->member, sizeof(cfg->member))) return ESP_ERR_INVALID_ARG; \
} while (0)
#define NUMBER(member, alias, max) do { \
    uint32_t number = cfg->member; \
    if (!read_unsigned(obj, #member, alias, &number, max)) return ESP_ERR_INVALID_ARG; \
    cfg->member = number; \
} while (0)
#define BOOL(member, alias) do { \
    if (!read_bool(obj, #member, alias, &cfg->member)) return ESP_ERR_INVALID_ARG; \
} while (0)

cJSON *ts_ssh_command_encode(const ts_ssh_command_config_t *cfg)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    if (!cJSON_AddStringToObject(obj, "id", cfg->id)) goto failed;
    if (!cJSON_AddStringToObject(obj, "host_id", cfg->host_id)) goto failed;
    if (!cJSON_AddStringToObject(obj, "name", cfg->name)) goto failed;
    if (!cJSON_AddStringToObject(obj, "command", cfg->command)) goto failed;
    if (cfg->desc[0]) if (!cJSON_AddStringToObject(obj, "desc", cfg->desc)) goto failed;
    if (cfg->icon[0]) if (!cJSON_AddStringToObject(obj, "icon", cfg->icon)) goto failed;
    if (cfg->expect_pattern[0]) if (!cJSON_AddStringToObject(obj, "expect_pattern", cfg->expect_pattern)) goto failed;
    if (cfg->fail_pattern[0]) if (!cJSON_AddStringToObject(obj, "fail_pattern", cfg->fail_pattern)) goto failed;
    if (cfg->extract_pattern[0]) if (!cJSON_AddStringToObject(obj, "extract_pattern", cfg->extract_pattern)) goto failed;
    if (cfg->var_name[0]) if (!cJSON_AddStringToObject(obj, "var_name", cfg->var_name)) goto failed;
    if (!cJSON_AddNumberToObject(obj, "timeout_sec", cfg->timeout_sec)) goto failed;
    if (!cJSON_AddBoolToObject(obj, "stop_on_match", cfg->stop_on_match)) goto failed;
    if (!cJSON_AddBoolToObject(obj, "nohup", cfg->nohup)) goto failed;
    if (!cJSON_AddBoolToObject(obj, "enabled", cfg->enabled)) goto failed;
    /* 服务模式字段 */
    if (!cJSON_AddBoolToObject(obj, "service_mode", cfg->service_mode)) goto failed;
    if (cfg->ready_pattern[0]) if (!cJSON_AddStringToObject(obj, "ready_pattern", cfg->ready_pattern)) goto failed;
    if (cfg->service_fail_pattern[0]) if (!cJSON_AddStringToObject(obj, "service_fail_pattern", cfg->service_fail_pattern)) goto failed;
    if (cfg->ready_timeout_sec > 0) if (!cJSON_AddNumberToObject(obj, "ready_timeout_sec", cfg->ready_timeout_sec)) goto failed;
    if (cfg->ready_check_interval_ms > 0) if (!cJSON_AddNumberToObject(obj, "ready_check_interval_ms", cfg->ready_check_interval_ms)) goto failed;
    if (!cJSON_AddNumberToObject(obj, "created_time", cfg->created_time)) goto failed;

    return obj;
failed:
    cJSON_Delete(obj);
    return NULL;
}

esp_err_t ts_ssh_command_decode(const cJSON *obj, ts_ssh_command_config_t *cfg)
{
    if (!cJSON_IsObject(obj) || !cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    cfg->timeout_sec = 30;
    STRING(id, NULL);
    STRING(host_id, NULL);
    STRING(name, NULL);
    STRING(command, NULL);
    STRING(desc, NULL);
    STRING(icon, NULL);
    STRING(expect_pattern, "expectPattern");
    STRING(fail_pattern, "failPattern");
    STRING(extract_pattern, "extractPattern");
    STRING(var_name, "varName");
    STRING(ready_pattern, "readyPattern");
    STRING(service_fail_pattern, "serviceFailPattern");
    NUMBER(timeout_sec, "timeout", UINT16_MAX);
    NUMBER(ready_timeout_sec, "readyTimeout", UINT16_MAX);
    NUMBER(ready_check_interval_ms, "readyInterval", UINT16_MAX);
    NUMBER(created_time, NULL, UINT32_MAX);
    BOOL(stop_on_match, "stopOnMatch");
    BOOL(nohup, NULL);
    BOOL(enabled, NULL);
    BOOL(service_mode, "serviceMode");
    return ESP_OK;
}

cJSON *ts_ssh_host_encode(const ts_ssh_host_config_t *cfg)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    if (!cJSON_AddStringToObject(obj, "id", cfg->id)) goto failed;
    if (!cJSON_AddStringToObject(obj, "host", cfg->host)) goto failed;
    if (!cJSON_AddNumberToObject(obj, "port", cfg->port)) goto failed;
    if (!cJSON_AddStringToObject(obj, "username", cfg->username)) goto failed;
    if (!cJSON_AddStringToObject(obj, "auth_type", cfg->auth_type == TS_SSH_HOST_AUTH_KEY ? "key" : "password")) goto failed;
    if (cfg->keyid[0]) if (!cJSON_AddStringToObject(obj, "keyid", cfg->keyid)) goto failed;
    if (!cJSON_AddBoolToObject(obj, "enabled", cfg->enabled)) goto failed;
    if (!cJSON_AddNumberToObject(obj, "created_time", cfg->created_time)) goto failed;

    return obj;
failed:
    cJSON_Delete(obj);
    return NULL;
}

esp_err_t ts_ssh_host_decode(const cJSON *obj, ts_ssh_host_config_t *cfg)
{
    if (!cJSON_IsObject(obj) || !cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    cfg->port = 22;
    STRING(id, NULL);
    STRING(host, NULL);
    STRING(username, NULL);
    STRING(keyid, NULL);
    NUMBER(port, NULL, UINT16_MAX);
    NUMBER(created_time, NULL, UINT32_MAX);
    BOOL(enabled, NULL);
    const cJSON *auth = cJSON_GetObjectItemCaseSensitive(obj, "auth_type");
    if (auth) {
        if (!cJSON_IsString(auth) ||
            (strcmp(auth->valuestring, "key") && strcmp(auth->valuestring, "password")))
            return ESP_ERR_INVALID_ARG;
        cfg->auth_type = !strcmp(auth->valuestring, "key") ? TS_SSH_HOST_AUTH_KEY : TS_SSH_HOST_AUTH_PASSWORD;
    }
    return ESP_OK;
}

#undef STRING
#undef NUMBER
#undef BOOL
