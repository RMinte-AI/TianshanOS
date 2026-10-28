#include "ts_automation_legacy.h"
#include "ts_action_codec.h"
#include "ts_config_partitions.h"
#include "ts_config_pack.h"
#include "ts_crypto.h"
#include "ts_rule_codec.h"
#include "ts_rule_pack.h"
#include "ts_ssh_config_codec.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef LEGACY_CONFIG_DIR
#define LEGACY_CONFIG_DIR "/sdcard/config"
#endif
#define RECORD_LIMIT 262144u
#define FNV_OFFSET 2166136261u

/* These are the deployed wire layouts, not the current runtime structures. */
typedef struct __attribute__((packed)) {
    char id[32], host_id[32], name[64], command[1024], desc[128], icon[64];
    char expect_pattern[128], fail_pattern[128], extract_pattern[128], var_name[32];
    uint16_t timeout_sec;
    uint8_t stop_on_match, nohup, enabled;
    uint32_t created_time, last_exec_time;
    uint8_t service_mode;
    char ready_pattern[128];
    uint16_t ready_timeout_sec, ready_check_interval_ms;
    char service_fail_pattern[128];
} legacy_command_t;
typedef struct __attribute__((packed)) {
    char id[32], host[64];
    uint16_t port;
    char username[32];
    uint8_t auth_type;
    char keyid[32];
    uint32_t created_time, last_used_time;
    uint8_t enabled;
} legacy_host_t;
typedef struct {
    uint32_t magic, version, length, generation, bank, count, aggregate, checksum;
} legacy_selector_t;
typedef struct {
    uint32_t magic, sequence, phase, source, bank, mirror, old_hash, new_hash;
    uint8_t had_old, deleting;
    char id[TS_AUTO_NAME_MAX_LEN];
} legacy_guard_t;
typedef struct { uint32_t count, hash; } legacy_bank_t;

static esp_err_t failed(ts_auto_config_error_t *error, ts_auto_config_kind_t kind,
                         const char *id, const char *reason, esp_err_t ret)
{
    if (error) {
        error->reason = reason;
        error->collection = ts_auto_config_collection(kind);
        snprintf(error->id, sizeof(error->id), "%s", id ? id : "");
    }
    return ret;
}

static uint32_t hash_bytes(uint32_t hash, const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    while (length--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

static const char *string(const cJSON *json, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static bool number(const cJSON *json, const char *key, uint32_t *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > UINT32_MAX ||
        value->valuedouble != (uint32_t)value->valuedouble) return false;
    *out = value->valuedouble;
    return true;
}

static esp_err_t old_open(const char *ns, nvs_handle_t *out)
{
    return nvs_open_from_partition(TS_CONFIG_KEYS_PARTITION, ns, NVS_READONLY, out);
}

static esp_err_t blob(nvs_handle_t handle, const char *key, void *out, size_t expected)
{
    size_t length = expected;
    esp_err_t ret = nvs_get_blob(handle, key, out, &length);
    return ret == ESP_OK && length != expected ? ESP_ERR_INVALID_SIZE : ret;
}

static esp_err_t nvs_text(nvs_handle_t handle, const char *key, bool as_blob, char **out)
{
    *out = NULL;
    size_t length = 0;
    esp_err_t ret = as_blob ? nvs_get_blob(handle, key, NULL, &length) :
                              nvs_get_str(handle, key, NULL, &length);
    if (ret != ESP_OK) return ret;
    if (length < 2 || length > RECORD_LIMIT) return ESP_ERR_INVALID_SIZE;
    char *text = heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) return ESP_ERR_NO_MEM;
    ret = as_blob ? nvs_get_blob(handle, key, text, &length) : nvs_get_str(handle, key, text, &length);
    if (ret == ESP_OK && (text[length - 1] || strlen(text) != length - 1)) ret = ESP_ERR_INVALID_ARG;
    if (ret == ESP_OK) *out = text;
    else free(text);
    return ret;
}

static esp_err_t file_bytes(const char *path, char **out, size_t *length)
{
    *out = NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long size = ok ? ftell(file) : -1;
    ok = size >= 0 && fseek(file, 0, SEEK_SET) == 0;
    char *text = ok ? heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    esp_err_t ret = !ok ? ESP_FAIL : !text ? ESP_ERR_NO_MEM : ESP_OK;
    if (ret == ESP_OK && (fread(text, 1, size, file) != (size_t)size || ferror(file))) ret = ESP_FAIL;
    if (fclose(file) && ret == ESP_OK) ret = ESP_FAIL;
    if (ret != ESP_OK) { free(text); return ret; }
    text[size] = 0;
    *out = text;
    *length = size;
    return ESP_OK;
}

static esp_err_t load_json(const char *path, bool encrypted, ts_auto_config_kind_t kind,
                            const ts_config_pack_acceptance_t *accepted, cJSON **out,
                            ts_auto_config_error_t *error)
{
    *out = NULL;
    char *text = NULL;
    size_t length = 0;
    esp_err_t ret = file_bytes(path, &text, &length);
    if (ret != ESP_OK) return ret;
    if (encrypted) {
        ts_config_pack_t *pack = NULL;
        ts_config_pack_acceptance_t proof = {0};
        /* Preserve each installed manager's original verification policy.
         * Guarded rule packages retain their explicit signer acceptance; the
         * other installed files use the same signature/recipient loader as
         * their former SD-priority managers. This is not a failed-check retry. */
        ts_config_pack_result_t got = kind == TS_AUTO_CONFIG_RULE
            ? ts_config_pack_load_verified_mem(text, length, accepted, &pack, &proof, NULL)
            : ts_config_pack_load_mem(text, length, &pack);
        if (got == TS_CONFIG_PACK_OK) {
            *out = ts_config_pack_parse_json(pack->content, pack->content_len);
            ts_config_pack_free(pack);
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(*out, "type");
            if (kind == TS_AUTO_CONFIG_RULE && cJSON_IsString(type) && !strcmp(type->valuestring, "automation_rule")) {
                cJSON *value = cJSON_GetObjectItemCaseSensitive(*out, "rule");
                ts_auto_rule_t rule = {0};
                ret = ts_rule_decode(value, &rule);
                cJSON *provenance = ret == ESP_OK ? ts_rule_pack_provenance(&rule, &proof) : NULL;
                ts_rule_dispose(&rule);
                if (ret == ESP_OK) {
                    cJSON_DeleteItemFromObjectCaseSensitive(value,"_package");
                    if (!provenance || !cJSON_AddItemToObject(value,"_package",provenance)) {
                        cJSON_Delete(provenance);ret=ESP_ERR_NO_MEM;
                    }
                }
            }
        } else {
            if (error) error->reason = ts_rule_pack_error(got);
            ret = ESP_ERR_INVALID_STATE;
        }
    } else *out = ts_config_pack_parse_json(text, length);
    free(text);
    return ret != ESP_OK ? ret : *out ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* A present package owns its basename. Verification failure never tries JSON. */
static esp_err_t priority_json(const char *base, ts_auto_config_kind_t kind, cJSON **out, ts_auto_config_error_t *error)
{
    char path[256];
    if (snprintf(path, sizeof(path), "%s.tscfg", base) >= sizeof(path)) return ESP_ERR_INVALID_SIZE;
    struct stat st;
    if (stat(path, &st) == 0) return load_json(path, true, kind, NULL, out, error);
    if (errno != ENOENT) return ESP_FAIL;
    if (snprintf(path, sizeof(path), "%s.json", base) >= sizeof(path)) return ESP_ERR_INVALID_SIZE;
    return load_json(path, false, kind, NULL, out, error);
}

static const cJSON *unwrap(const cJSON *json, ts_auto_config_kind_t kind)
{
    const char *type = string(json, "type");
    if (kind == TS_AUTO_CONFIG_RULE && cJSON_HasObjectItem(json, "rule"))
        return cJSON_GetObjectItemCaseSensitive(json, "rule");
    if (kind == TS_AUTO_CONFIG_ACTION && type && !strcmp(type, "action_template"))
        return cJSON_GetObjectItemCaseSensitive(json, "template");
    if (kind == TS_AUTO_CONFIG_COMMAND && type && !strcmp(type, "ssh_command"))
        return cJSON_GetObjectItemCaseSensitive(json, "command");
    return json;
}

static esp_err_t append(cJSON *array, const cJSON *value, bool replace)
{
    const char *id = string(value, "id");
    if (!cJSON_IsObject(value) || !id || !ts_rule_id_valid(id)) return ESP_ERR_INVALID_ARG;
    int found = -1;
    for (int i = 0; i < cJSON_GetArraySize(array); ++i) {
        const char *old = string(cJSON_GetArrayItem(array, i), "id");
        if (old && !strcmp(old, id)) { found = i; break; }
    }
    if (found >= 0 && !replace) return ESP_ERR_INVALID_STATE;
    cJSON *copy = cJSON_Duplicate(value, true);
    if (!copy) return ESP_ERR_NO_MEM;
    bool ok = found >= 0 ? cJSON_ReplaceItemInArray(array, found, copy) : cJSON_AddItemToArray(array, copy);
    if (!ok) cJSON_Delete(copy);
    return ok ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool suffix(const char *name, const char *extension)
{
    size_t n = strlen(name), e = strlen(extension);
    return n > e && !strcmp(name + n - e, extension);
}

static esp_err_t append_kind(cJSON *array, const cJSON *json, ts_auto_config_kind_t kind, bool replace)
{
    if (kind != TS_AUTO_CONFIG_SOURCE) return append(array, unwrap(json, kind), replace);
    ts_auto_source_t *source = heap_caps_calloc(1, sizeof(*source), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *raw = source ? cJSON_PrintUnformatted(json) : NULL;
    esp_err_t ret = raw ? ts_source_config_decode(raw, source) : ESP_ERR_NO_MEM;
    free(raw);
    raw = ret == ESP_OK ? ts_source_config_encode(source) : NULL;
    cJSON *normalized = raw ? cJSON_Parse(raw) : NULL;
    if (ret == ESP_OK) ret = normalized ? append(array, normalized, replace) : ESP_ERR_NO_MEM;
    cJSON_Delete(normalized);free(raw);free(source);
    return ret;
}

/* Zero files is distinguished from an explicit empty selected rule set. */
static esp_err_t directory(cJSON *array, ts_auto_config_kind_t kind, const char *name,
                            bool replace, const char *exclude, ts_auto_config_error_t *error)
{
    char path[256];
    snprintf(path, sizeof(path), LEGACY_CONFIG_DIR "/%s", name);
    DIR *dir = opendir(path);
    if (!dir) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    esp_err_t ret = ESP_OK;
    bool seen = false;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        bool encrypted = suffix(entry->d_name, ".tscfg"), plain = suffix(entry->d_name, ".json");
        if (!encrypted && !plain) continue;
        size_t id_length = strlen(entry->d_name) - (encrypted ? 6 : 5);
        char id[TS_AUTO_NAME_MAX_LEN];
        if (id_length >= sizeof(id)) { ret = ESP_ERR_INVALID_SIZE; break; }
        memcpy(id, entry->d_name, id_length); id[id_length] = 0;
        if (id[0] == '.') continue; /* Old transaction artifacts are never definitions. */
        if (!ts_rule_id_valid(id)) { ret = ESP_ERR_INVALID_ARG; break; }
        seen = true;
        if (exclude && !strcmp(exclude, id)) continue;
        if (plain) {
            snprintf(path, sizeof(path), LEGACY_CONFIG_DIR "/%s/%s.tscfg", name, id);
            struct stat st;
            if (stat(path, &st) == 0) continue;
            if (errno != ENOENT) { ret = ESP_FAIL; break; }
        }
        snprintf(path, sizeof(path), LEGACY_CONFIG_DIR "/%s/%s%s", name, id, encrypted ? ".tscfg" : ".json");
        cJSON *json = NULL;
        ret = load_json(path, encrypted, kind, NULL, &json, error);
        if (ret == ESP_OK && encrypted && kind == TS_AUTO_CONFIG_RULE) {
            const char *type = string(json, "type");
            if (!type || strcmp(type, "automation_rule")) ret = ESP_ERR_INVALID_ARG;
        }
        if (ret == ESP_OK) ret = append_kind(array, json, kind, replace);
        cJSON_Delete(json);
        if (ret != ESP_OK) {
            failed(error, kind, id, error && error->reason ? error->reason : "legacy_configuration_invalid", ret);
            break;
        }
    }
    if (closedir(dir) && ret == ESP_OK) ret = ESP_FAIL;
    return ret == ESP_OK && !seen ? ESP_ERR_NOT_FOUND : ret;
}

static esp_err_t aggregate(cJSON *array, ts_auto_config_kind_t kind, const char *name,
                            const char *key, ts_auto_config_error_t *error)
{
    char base[160];
    snprintf(base, sizeof(base), LEGACY_CONFIG_DIR "/%s", name);
    cJSON *json = NULL;
    esp_err_t ret = priority_json(base, kind, &json, error);
    if (ret == ESP_OK) {
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(json, key), *item;
        if (!cJSON_IsArray(items)) ret = ESP_ERR_INVALID_ARG;
        else cJSON_ArrayForEach(item, items) {
            ret = append_kind(array, item, kind, false);
            if (ret != ESP_OK) break;
        }
    }
    cJSON_Delete(json);
    return ret;
}

static esp_err_t string_list(cJSON *array, ts_auto_config_kind_t kind,
                              const char *ns, const char *prefix)
{
    nvs_handle_t handle;
    esp_err_t ret = old_open(ns, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;
    uint8_t count = 0;
    ret = nvs_get_u8(handle, "count", &count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
    for (unsigned i = 0; ret == ESP_OK && i < count; ++i) {
        char key[16], *text = NULL;
        snprintf(key, sizeof(key), "%s%u", prefix, i);
        ret = nvs_text(handle, key, false, &text);
        if (ret == ESP_OK) {
            cJSON *json = ts_config_pack_parse_json(text, strlen(text));
            ret = append_kind(array, json, kind, false);
            cJSON_Delete(json);
        }
        free(text);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t command_list(cJSON *array)
{
    nvs_handle_t handle;
    esp_err_t ret = old_open("ts_ssh_cmd", &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;
    legacy_command_t *old = heap_caps_calloc(1, sizeof(*old), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ts_ssh_command_config_t *next = heap_caps_calloc(1, sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!old || !next) ret = ESP_ERR_NO_MEM;
    for (unsigned i = 0; ret == ESP_OK && i < TS_SSH_COMMANDS_MAX; ++i) {
        char key[16]; snprintf(key, sizeof(key), "c_%u", i);
        size_t length = sizeof(*old);
        memset(old, 0, sizeof(*old));
        ret = nvs_get_blob(handle, key, old, &length);
        if (ret == ESP_ERR_NVS_NOT_FOUND) { ret = ESP_OK; continue; }
        if (ret != ESP_OK) break;
        if (length != sizeof(*old) && length != offsetof(legacy_command_t, service_mode) &&
            length != offsetof(legacy_command_t, service_fail_pattern)) { ret = ESP_ERR_INVALID_SIZE; break; }
#define COPY_STRING(field) \
        if (!memchr(old->field, 0, sizeof(old->field))) { ret = ESP_ERR_INVALID_ARG; break; } \
        memcpy(next->field, old->field, sizeof(next->field));
        COPY_STRING(id) COPY_STRING(host_id) COPY_STRING(name) COPY_STRING(command)
        COPY_STRING(desc) COPY_STRING(icon) COPY_STRING(expect_pattern) COPY_STRING(fail_pattern)
        COPY_STRING(extract_pattern) COPY_STRING(var_name) COPY_STRING(ready_pattern) COPY_STRING(service_fail_pattern)
#undef COPY_STRING
        next->timeout_sec = old->timeout_sec; next->stop_on_match = old->stop_on_match;
        next->nohup = old->nohup; next->enabled = old->enabled; next->service_mode = old->service_mode;
        next->ready_timeout_sec = old->ready_timeout_sec; next->ready_check_interval_ms = old->ready_check_interval_ms;
        next->created_time = old->created_time;
        cJSON *json = ts_ssh_command_encode(next);
        ret = json ? append(array, json, false) : ESP_ERR_NO_MEM;
        cJSON_Delete(json);
    }
    free(old); free(next); nvs_close(handle);
    return ret;
}

static esp_err_t host_list(cJSON *array)
{
    nvs_handle_t handle;
    esp_err_t ret = old_open("ts_ssh_cfg", &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;
    for (unsigned i = 0; ret == ESP_OK && i < TS_SSH_HOSTS_MAX; ++i) {
        legacy_host_t old;
        char key[16]; snprintf(key, sizeof(key), "h_%u", i);
        ret = blob(handle, key, &old, sizeof(old));
        if (ret == ESP_ERR_NVS_NOT_FOUND) { ret = ESP_OK; continue; }
        if (ret != ESP_OK) break;
        ts_ssh_host_config_t next = {0};
#define COPY_STRING(field) \
        if (!memchr(old.field, 0, sizeof(old.field))) { ret = ESP_ERR_INVALID_ARG; break; } \
        memcpy(next.field, old.field, sizeof(next.field));
        COPY_STRING(id) COPY_STRING(host) COPY_STRING(username) COPY_STRING(keyid)
#undef COPY_STRING
        if (old.auth_type > TS_SSH_HOST_AUTH_PASSWORD) { ret = ESP_ERR_INVALID_ARG; break; }
        next.port = old.port; next.auth_type = old.auth_type;
        next.created_time = old.created_time; next.enabled = old.enabled;
        cJSON *json = ts_ssh_host_encode(&next);
        ret = json ? append(array, json, false) : ESP_ERR_NO_MEM;
        cJSON_Delete(json);
    }
    nvs_close(handle);
    return ret;
}

static bool digest_matches(const char *text, size_t length, const char *expected)
{
    unsigned char hash[32]; char hex[65];
    return expected && strlen(expected) == 64 &&
        ts_crypto_hash(TS_HASH_SHA256, text, length, hash, sizeof(hash)) == ESP_OK &&
        ts_crypto_hex_encode(hash, sizeof(hash), hex, sizeof(hex)) == ESP_OK && !strcmp(hex, expected);
}

static bool acceptance(const cJSON *record, ts_config_pack_acceptance_t *out)
{
    const cJSON *json = cJSON_GetObjectItemCaseSensitive(record, "acceptance");
    const cJSON *when = cJSON_GetObjectItemCaseSensitive(json, "accepted_at");
    if (!number(json, "policy", &out->policy) || !out->policy || !cJSON_IsNumber(when) ||
        when->valuedouble <= 0 || when->valuedouble > 9007199254740991.0 ||
        when->valuedouble != (int64_t)when->valuedouble) return false;
    out->accepted_at = when->valuedouble;
    const char *keys[] = {"package", "signer", "root", "recipient"};
    char *values[] = {out->package_sha256, out->signer_sha256, out->root_sha256, out->recipient};
    for (unsigned i = 0; i < 4; ++i) {
        const char *value = string(json, keys[i]);
        if (!value || strlen(value) != 64) return false;
        memcpy(values[i], value, 65);
    }
    return true;
}

static esp_err_t saved_rule(cJSON *rules, const cJSON *record, bool sd, ts_auto_config_error_t *error)
{
    uint32_t source, revision, expected_length;
    const char *id = string(record, "id"), *digest = string(record, "digest");
    if (!id || !ts_rule_id_valid(id) || !number(record, "source", &source) || source > 2 ||
        !number(record, "revision", &revision) || !revision ||
        !number(record, "length", &expected_length) || !expected_length || expected_length > RECORD_LIMIT)
        return ESP_ERR_INVALID_ARG;
    cJSON *json = NULL;
    esp_err_t ret = ESP_OK;
    ts_config_pack_acceptance_t accepted = {0};
    if (source == 2) {
        if (!sd) return failed(error, TS_AUTO_CONFIG_RULE, id, "source_unavailable", ESP_ERR_INVALID_STATE);
        if (!acceptance(record, &accepted) || !digest || strcmp(accepted.package_sha256, digest))
            return ESP_ERR_INVALID_ARG;
        const char *path = string(record, "path");
        char *bytes = NULL; size_t length = 0;
        ret = path ? file_bytes(path, &bytes, &length) : ESP_ERR_INVALID_ARG;
        if (ret == ESP_OK && (length != expected_length || !digest_matches(bytes, length, digest))) ret = ESP_FAIL;
        if (ret == ESP_OK) {
            ts_config_pack_t *pack = NULL;
            ts_config_pack_result_t got = ts_config_pack_load_verified_mem(bytes, length, &accepted, &pack, NULL, NULL);
            if (got == TS_CONFIG_PACK_OK) {
                json = ts_config_pack_parse_json(pack->content, pack->content_len);
                ts_config_pack_free(pack);
                const char *type = string(json, "type");
                if (!type || strcmp(type, "automation_rule")) ret = ESP_ERR_INVALID_ARG;
            } else ret = failed(error, TS_AUTO_CONFIG_RULE, id, ts_rule_pack_error(got), ESP_ERR_INVALID_STATE);
        }
        free(bytes);
    } else {
        const cJSON *inline_json = cJSON_GetObjectItemCaseSensitive(record, "json");
        char *bytes = cJSON_PrintUnformatted(inline_json);
        if (!bytes) return ESP_ERR_NO_MEM;
        bool valid = strlen(bytes) == expected_length && digest_matches(bytes, expected_length, digest);
        free(bytes);
        if (!valid) return ESP_FAIL;
        json = cJSON_Duplicate(inline_json, true);
        if (!json) return ESP_ERR_NO_MEM;
        /* The committed inline definition owns its content. A leftover directory
         * file from a previous generation is not a second source. */
    }
    const cJSON *value = unwrap(json, TS_AUTO_CONFIG_RULE);
    const char *actual_id = string(value, "id");
    if (ret == ESP_OK && (!actual_id || strcmp(actual_id, id))) ret = ESP_ERR_INVALID_ARG;
    if (ret == ESP_OK) {
        cJSON *copy = cJSON_Duplicate(value, true), *rev = cJSON_CreateNumber(revision);
        if (!copy || !rev) { cJSON_Delete(copy); cJSON_Delete(rev); ret = ESP_ERR_NO_MEM; }
        else {
            bool ok = cJSON_HasObjectItem(copy, "revision") ?
                cJSON_ReplaceItemInObjectCaseSensitive(copy, "revision", rev) : cJSON_AddItemToObject(copy, "revision", rev);
            if (!ok) { cJSON_Delete(rev); ret = ESP_ERR_NO_MEM; }
            if (ret == ESP_OK && source == 2) {
                ts_auto_rule_t rule = {0};
                ret = ts_rule_decode(copy,&rule);
                cJSON *provenance = ret == ESP_OK ? ts_rule_pack_provenance(&rule,&accepted) : NULL;
                ts_rule_dispose(&rule);
                if (ret == ESP_OK) {
                    cJSON_DeleteItemFromObjectCaseSensitive(copy,"_package");
                    if (!provenance || !cJSON_AddItemToObject(copy,"_package",provenance)) {
                        cJSON_Delete(provenance);ret=ESP_ERR_NO_MEM;
                    }
                }
            }
            if (ret == ESP_OK) ret = append(rules, copy, false);
            cJSON_Delete(copy);
        }
    }
    cJSON_Delete(json);
    return ret;
}

static esp_err_t selected_rules(nvs_handle_t handle, cJSON *rules, bool sd, ts_auto_config_error_t *error)
{
    legacy_selector_t select, meta;
    esp_err_t ret = blob(handle, "set_select", &select, sizeof(select));
    if (ret != ESP_OK) return ret;
    if (select.magic != 0x52534c33u || select.version != 3 || select.length != sizeof(select) ||
        !select.generation || select.bank > 1 || select.count > ts_auto_config_limit(TS_AUTO_CONFIG_RULE) ||
        select.checksum != hash_bytes(FNV_OFFSET, &select, offsetof(legacy_selector_t, checksum)))
        return ESP_ERR_INVALID_STATE;
    char key[16]; snprintf(key, sizeof(key), "set_meta%u", (unsigned)select.bank);
    ret = blob(handle, key, &meta, sizeof(meta));
    if (ret != ESP_OK || memcmp(&meta, &select, sizeof(meta))) return ESP_ERR_INVALID_STATE;
    uint32_t aggregate_hash = FNV_OFFSET;
    for (unsigned i = 0; i < select.count; ++i) {
        snprintf(key, sizeof(key), "set_%c%u", select.bank ? 'b' : 'a', i);
        char *text = NULL;
        ret = nvs_text(handle, key, true, &text);
        if (ret == ESP_OK) {
            aggregate_hash = hash_bytes(aggregate_hash, text, strlen(text) + 1);
            cJSON *json = ts_config_pack_parse_json(text, strlen(text));
            ret = saved_rule(rules, json, sd, error);
            cJSON_Delete(json);
        }
        free(text);
        if (ret != ESP_OK) return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_STATE : ret;
    }
    return aggregate_hash == select.aggregate ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t bank_rules(nvs_handle_t handle, const legacy_guard_t *guard, cJSON *rules)
{
    legacy_bank_t meta;
    esp_err_t ret = blob(handle, guard->bank ? "meta_b" : "meta_a", &meta, sizeof(meta));
    if (ret != ESP_OK) return ret;
    if (meta.count > ts_auto_config_limit(TS_AUTO_CONFIG_RULE)) return ESP_ERR_INVALID_SIZE;
    uint32_t hash = FNV_OFFSET;
    for (unsigned i = 0; i < meta.count; ++i) {
        char key[16], *text = NULL;
        snprintf(key, sizeof(key), "%c%u", guard->bank ? 'b' : 'a', i);
        ret = nvs_text(handle, key, true, &text);
        if (ret == ESP_OK) {
            hash = hash_bytes(hash, text, strlen(text) + 1);
            cJSON *json = ts_config_pack_parse_json(text, strlen(text));
            ret = append(rules, unwrap(json, TS_AUTO_CONFIG_RULE), false);
            cJSON_Delete(json);
        }
        free(text);
        if (ret != ESP_OK) return ret;
    }
    return hash == meta.hash ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t guarded_rules(nvs_handle_t handle, cJSON *rules, bool sd, ts_auto_config_error_t *error)
{
    legacy_guard_t guard;
    esp_err_t ret = blob(handle, "guard", &guard, sizeof(guard));
    if (ret != ESP_OK) return ret;
    if (guard.magic != 0x52554c32u || guard.bank > 1 || guard.source > 1 || guard.phase > 2 ||
        (guard.phase && (!memchr(guard.id, 0, sizeof(guard.id)) || !ts_rule_id_valid(guard.id))))
        return ESP_ERR_INVALID_STATE;
    if (guard.source == 0 || (!sd && guard.phase == 0 && guard.mirror)) {
        ret = bank_rules(handle, &guard, rules);
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_INVALID_STATE : ret;
    }
    if (!sd) return failed(error, TS_AUTO_CONFIG_RULE, NULL, "source_unavailable", ESP_ERR_INVALID_STATE);
    const char *exclude = guard.phase ? guard.id : NULL;
    if (guard.phase && !(guard.phase == 1 && !guard.had_old) && !(guard.phase == 2 && guard.deleting)) {
        char path[256], *text = NULL; size_t length = 0;
        snprintf(path, sizeof(path), LEGACY_CONFIG_DIR "/rules/%s.json", guard.id);
        ret = file_bytes(path, &text, &length);
        uint32_t expected = guard.phase == 1 ? guard.old_hash : guard.new_hash;
        bool match = ret == ESP_OK && hash_bytes(FNV_OFFSET, text, length) == expected;
        free(text); text = NULL;
        if (!match && guard.phase == 1 && (ret == ESP_OK || ret == ESP_ERR_NOT_FOUND)) {
            snprintf(path, sizeof(path), LEGACY_CONFIG_DIR "/rules/.%s.previous", guard.id);
            ret = file_bytes(path, &text, &length);
            match = ret == ESP_OK && hash_bytes(FNV_OFFSET, text, length) == expected;
        } else if (match) ret = file_bytes(path, &text, &length);
        if (ret != ESP_OK || !match) { free(text); return ESP_ERR_INVALID_STATE; }
        cJSON *json = ts_config_pack_parse_json(text, length);
        const cJSON *value = unwrap(json, TS_AUTO_CONFIG_RULE);
        const char *id = string(value, "id");
        ret = id && !strcmp(id, guard.id) ? append(rules, value, false) : ESP_ERR_INVALID_ARG;
        free(text); cJSON_Delete(json);
        if (ret != ESP_OK) return ret;
    } else if (guard.phase == 2 && guard.deleting) {
        char path[256]; struct stat st;
        snprintf(path, sizeof(path), LEGACY_CONFIG_DIR "/rules/%s.json", guard.id);
        if (stat(path, &st) == 0 || errno != ENOENT) return ESP_ERR_INVALID_STATE;
    }
    ret = directory(rules, TS_AUTO_CONFIG_RULE, "rules", false, exclude, error);
    return ret == ESP_ERR_NOT_FOUND ? ESP_OK : ret;
}

static esp_err_t rules(cJSON *array, bool sd, ts_auto_config_error_t *error)
{
    nvs_handle_t handle;
    esp_err_t ret = old_open("rule_stage", &handle);
    if (ret == ESP_OK) {
        ret = selected_rules(handle, array, sd, error);
        if (ret == ESP_ERR_NVS_NOT_FOUND) ret = guarded_rules(handle, array, sd, error);
        nvs_close(handle);
        if (ret != ESP_ERR_NVS_NOT_FOUND) return ret;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) return ret;
    if (sd) {
        ret = directory(array, TS_AUTO_CONFIG_RULE, "rules", false, NULL, error);
        if (ret == ESP_ERR_NOT_FOUND) ret = aggregate(array, TS_AUTO_CONFIG_RULE, "rules", "rules", error);
        if (ret != ESP_ERR_NOT_FOUND) return ret;
    }
    return string_list(array, TS_AUTO_CONFIG_RULE, "auto_rules", "rule_");
}

esp_err_t ts_auto_legacy_read(bool sd_available, cJSON **out, ts_auto_config_error_t *error)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (error) memset(error, 0, sizeof(*error));
    ts_auto_document_t empty = {.enabled=true, .migration_version=1};
    cJSON *json = ts_auto_document_encode(&empty);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t ret = ESP_OK;
    ts_auto_config_kind_t kind;
    for (kind = TS_AUTO_CONFIG_RULE; ret == ESP_OK && kind < TS_AUTO_CONFIG_VARIABLE; ++kind) {
        cJSON *array = cJSON_GetObjectItemCaseSensitive(json, ts_auto_config_collection(kind));
        if (kind == TS_AUTO_CONFIG_RULE) ret = rules(array, sd_available, error);
        else if (kind == TS_AUTO_CONFIG_HOST) {
            ret = host_list(array);
            if (ret == ESP_OK && sd_available) {
                ret = directory(array, kind, "ssh_hosts", true, NULL, error);
                if (ret == ESP_ERR_NOT_FOUND) ret = ESP_OK;
            }
        } else {
            const char *name = kind == TS_AUTO_CONFIG_ACTION ? "actions" :
                               kind == TS_AUTO_CONFIG_COMMAND ? "ssh_commands" : "sources";
            ret = sd_available ? directory(array, kind, name, false, NULL, error) : ESP_ERR_NOT_FOUND;
            if (ret == ESP_ERR_NOT_FOUND && sd_available && kind != TS_AUTO_CONFIG_COMMAND)
                ret = aggregate(array, kind, name, kind == TS_AUTO_CONFIG_ACTION ? "templates" : name, error);
            if (ret == ESP_ERR_NOT_FOUND) {
                ret = kind == TS_AUTO_CONFIG_COMMAND ? command_list(array) :
                    string_list(array, kind, kind == TS_AUTO_CONFIG_ACTION ? "action_tpl" : "ts_auto_src",
                                kind == TS_AUTO_CONFIG_ACTION ? "tpl_" : "src_");
            }
        }
        if (ret != ESP_OK && (!error || !error->reason))
            failed(error, kind, NULL, "legacy_configuration_invalid", ret);
    }
    /* User-variable persistence was never implemented in the old release.
     * Runtime discoveries and execution results are deliberately not imported. */
    if (ret == ESP_OK) *out = json;
    else cJSON_Delete(json);
    return ret;
}
