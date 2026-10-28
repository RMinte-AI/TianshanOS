#include "ts_config_partitions.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "ts_cert.h"
#include "ts_config_pack.h"
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIGRATION_NAMESPACE "ts_cfg_state"
static const char *TAG = "config_migrate";

/* These definitions are migrated by the automation owner after PKI is ready. */
static bool system_entry(const nvs_entry_info_t *info)
{
    static const char *const automation[] = {
        "action_tpl", "ts_auto_src", "auto_rules", "rule_stage", "ts_ssh_cmd",
        "ts_ssh_cfg", "ts_rules", "ts_actions", "ts_sources", "ts_var"
    };
    if (!strcmp(info->namespace_name, "ts_keystore") ||
        (!strcmp(info->namespace_name, "ts_security") && !strncmp(info->key, "key_", 4)) ||
        (!strcmp(info->namespace_name, "ts_pki") && !strcmp(info->key, "privkey")))
        return false;
    for (size_t i = 0; i < sizeof(automation) / sizeof(automation[0]); ++i)
        if (!strcmp(info->namespace_name, automation[i])) return false;
    /* Only the nine system schemas carry over; synchronization metadata retires. */
    if (!strcmp(info->namespace_name, "ts_meta"))
        return !strncmp(info->key, "schema_v", 8) &&
               info->key[8] >= '0' && info->key[8] <= '8' && !info->key[9];
    return true;
}

static esp_err_t copy_entry(const nvs_entry_info_t *info)
{
    nvs_handle_t from, to;
    esp_err_t ret = nvs_open_from_partition(TS_CONFIG_KEYS_PARTITION,
        info->namespace_name, NVS_READONLY, &from);
    if (ret != ESP_OK) return ret;
    ret = nvs_open(info->namespace_name, NVS_READWRITE, &to);
    if (ret != ESP_OK) { nvs_close(from); return ret; }

#define COPY_INT(kind, suffix, ctype) case NVS_TYPE_##kind: { \
    ctype value; \
    ret = nvs_get_##suffix(from, info->key, &value); \
    if (ret == ESP_OK) ret = nvs_set_##suffix(to, info->key, value); \
    break; \
}
    switch (info->type) {
    COPY_INT(U8, u8, uint8_t)
    COPY_INT(I8, i8, int8_t)
    COPY_INT(U16, u16, uint16_t)
    COPY_INT(I16, i16, int16_t)
    COPY_INT(U32, u32, uint32_t)
    COPY_INT(I32, i32, int32_t)
    COPY_INT(U64, u64, uint64_t)
    COPY_INT(I64, i64, int64_t)
    case NVS_TYPE_STR:
    case NVS_TYPE_BLOB: {
        size_t size = 0;
        bool string = info->type == NVS_TYPE_STR;
        ret = string ? nvs_get_str(from, info->key, NULL, &size)
                     : nvs_get_blob(from, info->key, NULL, &size);
        void *data = ret == ESP_OK ?
            heap_caps_malloc(size ? size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (ret == ESP_OK && !data) ret = ESP_ERR_NO_MEM;
        if (ret == ESP_OK) {
            ret = string ? nvs_get_str(from, info->key, data, &size)
                         : nvs_get_blob(from, info->key, data, &size);
            if (ret == ESP_OK)
                ret = string ? nvs_set_str(to, info->key, data)
                             : nvs_set_blob(to, info->key, data, size);
        }
        free(data);
        break;
    }
    default: ret = ESP_ERR_NOT_SUPPORTED; break;
    }
#undef COPY_INT
    if (ret == ESP_OK) ret = nvs_commit(to);
    nvs_close(to);
    nvs_close(from);
    return ret;
}

static esp_err_t visit_system_entries(bool remove)
{
    nvs_iterator_t it = NULL;
    esp_err_t ret = nvs_entry_find(TS_CONFIG_KEYS_PARTITION, NULL, NVS_TYPE_ANY, &it);
    while (ret == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        /* Erasure does not append or compact entries. Advance before removing. */
        esp_err_t next = nvs_entry_next(&it);
        if (system_entry(&info)) {
            if (remove) {
                nvs_handle_t old;
                ret = nvs_open_from_partition(TS_CONFIG_KEYS_PARTITION,
                    info.namespace_name, NVS_READWRITE, &old);
                if (ret == ESP_OK) {
                    ret = nvs_erase_key(old, info.key);
                    if (ret == ESP_OK) ret = nvs_commit(old);
                    nvs_close(old);
                }
            } else ret = copy_entry(&info);
            if (ret != ESP_OK) break;
        }
        ret = next;
    }
    nvs_release_iterator(it);
    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
}

/* Encrypted legacy files keep precedence. A selected but unreadable pack must
 * not silently import a different plaintext configuration or finish migration. */
static esp_err_t read_system_file(const char *path, char **raw, size_t *length)
{
    char encrypted[80];
    snprintf(encrypted, sizeof(encrypted), "%.*s.tscfg", (int)strlen(path) - 5, path);
    struct stat st;
    if (stat(encrypted, &st) == 0) {
        esp_err_t ret = ts_cert_init();
        if (ret == ESP_OK) ret = ts_config_pack_init();
        if (ret != ESP_OK) return ret;
        ts_config_pack_t *pack = NULL;
        ts_config_pack_result_t packed = ts_config_pack_load(encrypted, &pack);
        if (packed != TS_CONFIG_PACK_OK || !pack || !pack->content) {
            ts_config_pack_free(pack);
            return ESP_FAIL;
        }
        *raw = pack->content;
        *length = pack->content_len;
        pack->content = NULL;
        ts_config_pack_free(pack);
        return ESP_OK;
    }
    if (errno != ENOENT) return ESP_FAIL;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ESP_FAIL; }
    char *bytes = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bytes) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t read = fread(bytes, 1, size, f);
    int closed = fclose(f);
    if (read != (size_t)size || closed != 0) { free(bytes); return ESP_FAIL; }
    bytes[size] = 0;
    *raw = bytes;
    *length = size;
    return ESP_OK;
}

/* Match the old module priority: pending NVS edits win; otherwise valid SD wins.
 * Direct NVS keys in mixed namespaces are retained separately from "config". */
static esp_err_t import_system_files(void)
{
    static const struct { const char *ns, *file, *key; } modules[] = {
        {"ts_net", "net", "config"}, {"ts_dhcp", "dhcp", "config"},
        {"ts_wifi", "wifi", "config"}, {"ts_nat", "nat", "config"},
        {"ts_led", "led", "config"}, {"ts_fan", "fan", "config"},
        {"ts_device", "device", "config"}, {"ts_system", "system", "config"},
        {"ts_temp", "temp", "config"}, {"ts_ui", "ui_widgets", "widgets"},
        {"ts_power", "power_policy", "config"}
    };
    uint8_t pending = 0;
    nvs_handle_t old;
    esp_err_t ret = nvs_open_from_partition(TS_CONFIG_KEYS_PARTITION,
        "ts_meta", NVS_READONLY, &old);
    if (ret == ESP_OK) {
        ret = nvs_get_u8(old, "pending_sync", &pending);
        nvs_close(old);
    }
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) return ret;
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        if (i < 9 && (pending & (1U << i))) continue;
        char path[80];
        snprintf(path, sizeof(path), "/sdcard/config/%s.json", modules[i].file);
        char *raw = NULL;
        size_t length = 0;
        ret = read_system_file(path, &raw, &length);
        if (ret == ESP_ERR_NOT_FOUND) continue;
        if (ret != ESP_OK) return ret;
        cJSON *json = cJSON_ParseWithLengthOpts(raw, length + 1, NULL, true);
        bool valid = cJSON_IsObject(json);
        cJSON_Delete(json);
        if (!valid) {
            ESP_LOGW(TAG, "Ignoring invalid legacy system file: %s", path);
            free(raw);
            continue;
        }
        nvs_handle_t to;
        ret = nvs_open(modules[i].ns, NVS_READWRITE, &to);
        if (ret == ESP_OK) {
            ret = nvs_set_blob(to, modules[i].key, raw, length);
            if (ret == ESP_OK) ret = nvs_commit(to);
            nvs_close(to);
        }
        free(raw);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

esp_err_t ts_config_migrate_system(bool sd_available)
{
    nvs_handle_t state;
    esp_err_t ret = nvs_open(MIGRATION_NAMESPACE, NVS_READWRITE, &state);
    if (ret != ESP_OK) return ret;
    uint8_t version = 0;
    ret = nvs_get_u8(state, "system_v", &version);
    if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
    if (ret == ESP_OK && version != 1) {
        ret = visit_system_entries(false);
        if (ret == ESP_OK && sd_available) ret = import_system_files();
        if (ret == ESP_OK) ret = nvs_set_u8(state, "system_v", 1);
        if (ret == ESP_OK) ret = nvs_commit(state);
    }
    nvs_close(state);
    if (ret != ESP_OK) return ret;
    ret = visit_system_entries(true);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "System migrated; old non-key cleanup incomplete: %s", esp_err_to_name(ret));
    return ESP_OK;
}
