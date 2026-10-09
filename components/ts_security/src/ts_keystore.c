/**
 * @file ts_keystore.c
 * @brief TianShanOS Secure Key Storage Implementation
 * 
 * 使用 ESP32 NVS 加密分区存储 SSH 私钥。
 * 支持 HMAC 方案（推荐）和 Flash 加密方案。
 * 
 * NVS 命名空间：ts_keystore
 * 键值格式：
 *   - {id}_priv: 私钥 PEM 数据
 *   - {id}_pub:  公钥 OpenSSH 格式
 *   - {id}_meta: 元数据 (JSON)
 *   - _index:    已存储的所有 key ID 列表
 */

#include "ts_keystore.h"
#include "ts_crypto.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM */
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>

static const char *TAG = "ts_keystore";

/* NVS 命名空间 */
#define KEYSTORE_NAMESPACE      "ts_keystore"
#define KEYSTORE_INDEX_KEY      "_index"

/* 最大索引字符串长度 */
#define MAX_INDEX_LEN           (TS_KEYSTORE_MAX_KEYS * (TS_KEYSTORE_ID_MAX_LEN + 1))

/* 模块状态 */
static struct {
    bool initialized;
    nvs_handle_t nvs_handle;
    SemaphoreHandle_t write_lock;
    StaticSemaphore_t write_lock_storage;
} s_keystore = {
    .initialized = false,
    .nvs_handle = 0,
};

/* ========== 内部辅助函数 ========== */

/**
 * @brief 安全释放私钥内存（清零后释放）
 * 
 * 安全策略：私钥在释放前必须清零，防止内存残留被攻击者利用
 * 注意：使用 memset_s 风格的实现，确保不会被编译器优化掉
 */
static void secure_free_key(char *key, size_t len)
{
    if (key == NULL) {
        return;  /* 空指针，安全返回 */
    }
    
    if (len > 0 && len < 0x100000) {  /* 合理性检查：<1MB */
        /* 使用 volatile 防止编译器优化掉清零操作 */
        volatile unsigned char *p = (volatile unsigned char *)key;
        for (size_t i = 0; i < len; i++) {
            p[i] = 0;
        }
    }
    
    free(key);
}

/**
 * @brief 构造 NVS 键名
 */
static void make_nvs_key(char *buf, size_t buf_len, const char *id, const char *suffix)
{
    snprintf(buf, buf_len, "%s%s", id, suffix);
}

/**
 * @brief 解析索引字符串为 ID 列表
 */
static int parse_index(const char *index_str, char ids[][TS_KEYSTORE_ID_MAX_LEN], size_t max_ids)
{
    if (!index_str || !ids || max_ids == 0) {
        return 0;
    }
    
    int count = 0;
    char *copy = TS_STRDUP_PSRAM(index_str);
    if (!copy) return 0;
    char *saveptr = NULL;
    char *token = strtok_r(copy, ",", &saveptr);
    while (token && count < max_ids) {
        strncpy(ids[count], token, TS_KEYSTORE_ID_MAX_LEN - 1);
        ids[count][TS_KEYSTORE_ID_MAX_LEN - 1] = '\0';
        count++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(copy);
    return count;
}

/**
 * @brief 添加 ID 到索引
 */
/* Writers hold write_lock across the entire read/modify/write operation. */
static esp_err_t read_index(char *index, size_t size, bool *present)
{
    index[0] = '\0';
    if (present) *present = false;
    esp_err_t ret = nvs_get_str(s_keystore.nvs_handle, KEYSTORE_INDEX_KEY, index, &size);
    if (present && ret == ESP_OK) *present = true;
    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
}

static bool index_contains(const char *index, const char *id)
{
    size_t id_len = strlen(id);
    for (const char *p = index; *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == id_len && memcmp(p, id, len) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static esp_err_t append_index(char *index, size_t size, const char *id)
{
    if (index_contains(index, id)) return ESP_OK;
    size_t used = strlen(index), count = used ? 1 : 0;
    for (const char *p = index; *p; ++p) if (*p == ',') ++count;
    if (count >= TS_KEYSTORE_MAX_KEYS) return ESP_ERR_INVALID_SIZE;
    size_t needed = used + (used ? 1 : 0) + strlen(id) + 1;
    if (needed > size) return ESP_ERR_INVALID_SIZE;
    if (used) index[used++] = ',';
    strcpy(index + used, id);
    return ESP_OK;
}

static esp_err_t add_to_index(const char *id)
{
    char index[MAX_INDEX_LEN];
    esp_err_t ret = read_index(index, sizeof(index), NULL);
    if (ret != ESP_OK) return ret;
    ret = append_index(index, sizeof(index), id);
    return ret == ESP_OK ? nvs_set_str(s_keystore.nvs_handle, KEYSTORE_INDEX_KEY, index) : ret;
}

/**
 * @brief 从索引中移除 ID
 */
static esp_err_t remove_from_index(const char *id)
{
    char index_str[MAX_INDEX_LEN] = {0};
    size_t len = sizeof(index_str);
    
    esp_err_t ret = nvs_get_str(s_keystore.nvs_handle, KEYSTORE_INDEX_KEY, index_str, &len);
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 重建索引，排除指定 ID */
    char ids[TS_KEYSTORE_MAX_KEYS][TS_KEYSTORE_ID_MAX_LEN];
    int count = parse_index(index_str, ids, TS_KEYSTORE_MAX_KEYS);
    
    char new_index[MAX_INDEX_LEN] = {0};
    for (int i = 0; i < count; i++) {
        if (strcmp(ids[i], id) != 0) {
            if (strlen(new_index) > 0) {
                strncat(new_index, ",", sizeof(new_index) - strlen(new_index) - 1);
            }
            strncat(new_index, ids[i], sizeof(new_index) - strlen(new_index) - 1);
        }
    }
    
    return nvs_set_str(s_keystore.nvs_handle, KEYSTORE_INDEX_KEY, new_index);
}

/**
 * @brief 存储元数据
 */
static esp_err_t store_metadata(const char *id, const ts_keystore_key_info_t *info)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    
    if (!cJSON_AddStringToObject(json, "type", ts_keystore_type_to_string(info->type)) ||
        !cJSON_AddStringToObject(json, "comment", info->comment) ||
        !cJSON_AddStringToObject(json, "alias", info->alias) ||
        !cJSON_AddNumberToObject(json, "created_at", info->created_at) ||
        !cJSON_AddNumberToObject(json, "last_used", info->last_used) ||
        !cJSON_AddBoolToObject(json, "has_pubkey", info->has_public_key) ||
        !cJSON_AddBoolToObject(json, "exportable", info->exportable) ||
        !cJSON_AddBoolToObject(json, "hidden", info->hidden)) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    
    if (!str) return ESP_ERR_NO_MEM;
    
    char key[TS_KEYSTORE_ID_MAX_LEN + 8];
    make_nvs_key(key, sizeof(key), id, "_meta");
    
    esp_err_t ret = nvs_set_str(s_keystore.nvs_handle, key, str);
    free(str);
    
    return ret;
}

/**
 * @brief 加载元数据
 */
static esp_err_t load_metadata(const char *id, ts_keystore_key_info_t *info)
{
    char key[TS_KEYSTORE_ID_MAX_LEN + 8];
    make_nvs_key(key, sizeof(key), id, "_meta");
    
    /* 获取长度 */
    size_t len = 0;
    esp_err_t ret = nvs_get_str(s_keystore.nvs_handle, key, NULL, &len);
    if (ret != ESP_OK) return ret;
    
    char *str = TS_MALLOC_PSRAM(len);
    if (!str) return ESP_ERR_NO_MEM;
    
    ret = nvs_get_str(s_keystore.nvs_handle, key, str, &len);
    if (ret != ESP_OK) {
        free(str);
        return ret;
    }
    
    /* 解析 JSON */
    cJSON *json = cJSON_Parse(str);
    free(str);
    
    if (!json) return ESP_ERR_INVALID_RESPONSE;
    
    memset(info, 0, sizeof(*info));
    strncpy(info->id, id, sizeof(info->id) - 1);
    
    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "type"))) {
        info->type = ts_keystore_type_from_string(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(json, "comment"))) {
        strncpy(info->comment, item->valuestring, sizeof(info->comment) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "alias"))) {
        strncpy(info->alias, item->valuestring, sizeof(info->alias) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "created_at"))) {
        info->created_at = (uint32_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItem(json, "last_used"))) {
        info->last_used = (uint32_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItem(json, "has_pubkey"))) {
        info->has_public_key = cJSON_IsTrue(item);
    }
    if ((item = cJSON_GetObjectItem(json, "exportable"))) {
        info->exportable = cJSON_IsTrue(item);
    } else {
        info->exportable = false;  /* 旧密钥默认不可导出 */
    }
    if ((item = cJSON_GetObjectItem(json, "hidden"))) {
        info->hidden = cJSON_IsTrue(item);
    } else {
        info->hidden = false;  /* 旧密钥默认不隐藏 */
    }
    
    cJSON_Delete(json);
    return ESP_OK;
}

/* ========== 公开 API ========== */

/* 内部函数：带选项的密钥存储 */
static esp_err_t store_key_locked(const char *id,
                                          const ts_keystore_keypair_t *keypair,
                                          ts_keystore_key_type_t type,
                                          const ts_keystore_gen_opts_t *opts,
                                          ts_keystore_generate_result_t *detail)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!id || strlen(id) == 0 || strlen(id) >= TS_KEYSTORE_ID_MAX_LEN) {
        ESP_LOGE(TAG, "Invalid key ID");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!keypair || !keypair->private_key || keypair->private_key_len == 0) {
        ESP_LOGE(TAG, "Invalid keypair");
        return ESP_ERR_INVALID_ARG;
    }
    
    bool exportable = opts ? opts->exportable : false;
    const char *comment = opts ? opts->comment : NULL;
    const char *alias = opts ? opts->alias : NULL;
    bool hidden = opts ? opts->hidden : false;
    
    ESP_LOGI(TAG, "Storing key '%s' (type=%s, privkey_len=%zu, exportable=%d, hidden=%d)", 
             id, ts_keystore_type_to_string(type), keypair->private_key_len, exportable, hidden);
    
    char nvs_key[TS_KEYSTORE_ID_MAX_LEN + 8];
    esp_err_t ret;
    
    if (detail) detail->failed_stage = "private_write";
    /* 存储私钥 */
    make_nvs_key(nvs_key, sizeof(nvs_key), id, "_priv");
    ret = nvs_set_blob(s_keystore.nvs_handle, nvs_key, 
                       keypair->private_key, keypair->private_key_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store private key: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 存储公钥（如果有） */
    bool has_pubkey = false;
    if (keypair->public_key && keypair->public_key_len > 0) {
        if (detail) detail->failed_stage = "public_write";
        make_nvs_key(nvs_key, sizeof(nvs_key), id, "_pub");
        ret = nvs_set_blob(s_keystore.nvs_handle, nvs_key,
                           keypair->public_key, keypair->public_key_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to store public key: %s", esp_err_to_name(ret));
            if (detail) return ret; /* Generated keys require their public key. */
        } else {
            has_pubkey = true;
        }
    }
    
    /* 存储元数据 */
    ts_keystore_key_info_t info = {
        .type = type,
        .created_at = (uint32_t)time(NULL),
        .last_used = 0,
        .has_public_key = has_pubkey,
        .exportable = exportable,
        .hidden = hidden,
    };
    strncpy(info.id, id, sizeof(info.id) - 1);
    if (comment) {
        strncpy(info.comment, comment, sizeof(info.comment) - 1);
    }
    if (alias) {
        strncpy(info.alias, alias, sizeof(info.alias) - 1);
    }
    
    if (detail) detail->failed_stage = "metadata_write";
    ret = store_metadata(id, &info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to store metadata: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 添加到索引 */
    if (detail) detail->failed_stage = "index_write";
    ret = add_to_index(id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update index: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 提交更改 */
    if (detail) detail->failed_stage = "commit";
    ret = nvs_commit(s_keystore.nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (detail) { detail->stored = true; detail->failed_stage = NULL; }
    ESP_LOGI(TAG, "Key '%s' stored successfully", id);
    return ESP_OK;
}

static esp_err_t ts_keystore_store_key_ex(const char *id,
                                         const ts_keystore_keypair_t *keypair,
                                         ts_keystore_key_type_t type,
                                         const ts_keystore_gen_opts_t *opts)
{
    if (!s_keystore.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_keystore.write_lock, portMAX_DELAY);
    esp_err_t ret = store_key_locked(id, keypair, type, opts, NULL);
    xSemaphoreGive(s_keystore.write_lock);
    return ret;
}

/* New generation cannot replace an indexed key or an unlisted fragment. */
static esp_err_t check_new_key(const char *id, char *index, bool *index_present,
                               ts_keystore_generate_result_t *detail)
{
    detail->failed_stage = "initialization";
    if (!s_keystore.initialized) return ESP_ERR_INVALID_STATE;
    detail->failed_stage = "index_read";
    esp_err_t ret = read_index(index, MAX_INDEX_LEN, index_present);
    if (ret != ESP_OK) return ret;
    detail->failed_stage = "id_occupied";
    if (index_contains(index, id)) return ESP_ERR_INVALID_STATE;
    const char *suffixes[] = {"_priv", "_pub", "_meta"};
    char key[TS_KEYSTORE_ID_MAX_LEN + 8];
    for (size_t i = 0; i < 3; ++i) {
        make_nvs_key(key, sizeof(key), id, suffixes[i]);
        size_t len = 0;
        ret = i == 2 ? nvs_get_str(s_keystore.nvs_handle, key, NULL, &len)
                     : nvs_get_blob(s_keystore.nvs_handle, key, NULL, &len);
        if (ret == ESP_OK) return ESP_ERR_INVALID_STATE;
        if (ret != ESP_ERR_NVS_NOT_FOUND) { detail->failed_stage = "record_read"; return ret; }
    }
    detail->failed_stage = "index_capacity";
    char updated[MAX_INDEX_LEN];
    strcpy(updated, index);
    return append_index(updated, sizeof(updated), id);
}

static esp_err_t cleanup_new_key(const char *id, const char *old_index,
                                 bool index_attempted, bool index_present)
{
    esp_err_t ret;
    if (index_attempted) {
        char current[MAX_INDEX_LEN];
        ret = read_index(current, sizeof(current), NULL);
        if (ret != ESP_OK) return ret;
        if (index_contains(current, id)) {
            ret = index_present ? nvs_set_str(s_keystore.nvs_handle, KEYSTORE_INDEX_KEY, old_index)
                                : nvs_erase_key(s_keystore.nvs_handle, KEYSTORE_INDEX_KEY);
            if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) return ret; /* Keep materials if index removal failed. */
        }
    }
    const char *suffixes[] = {"_meta", "_pub", "_priv"};
    esp_err_t first_error = ESP_OK;
    char key[TS_KEYSTORE_ID_MAX_LEN + 8];
    for (size_t i = 0; i < 3; ++i) {
        make_nvs_key(key, sizeof(key), id, suffixes[i]);
        ret = nvs_erase_key(s_keystore.nvs_handle, key);
        if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND && first_error == ESP_OK) first_error = ret;
    }
    ret = nvs_commit(s_keystore.nvs_handle);
    return first_error != ESP_OK ? first_error : ret;
}

esp_err_t ts_keystore_init(void)
{
    /* Static lock remains valid for writers waiting while the service stops. */
    if (!s_keystore.write_lock) {
        s_keystore.write_lock = xSemaphoreCreateMutexStatic(&s_keystore.write_lock_storage);
        if (!s_keystore.write_lock) return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_keystore.write_lock, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    if (!s_keystore.initialized) {
        ret = nvs_open(KEYSTORE_NAMESPACE, NVS_READWRITE, &s_keystore.nvs_handle);
        if (ret == ESP_OK) s_keystore.initialized = true;
    }
    xSemaphoreGive(s_keystore.write_lock);
    return ret;
}

esp_err_t ts_keystore_deinit(void)
{
    if (!s_keystore.write_lock) return ESP_OK;
    xSemaphoreTake(s_keystore.write_lock, portMAX_DELAY);
    if (s_keystore.initialized) {
        nvs_close(s_keystore.nvs_handle);
        s_keystore.nvs_handle = 0;
        s_keystore.initialized = false;
    }
    xSemaphoreGive(s_keystore.write_lock);
    return ESP_OK;
}

bool ts_keystore_is_initialized(void)
{
    return s_keystore.initialized;
}

esp_err_t ts_keystore_store_key(const char *id, 
                                 const ts_keystore_keypair_t *keypair,
                                 ts_keystore_key_type_t type,
                                 const char *comment)
{
    ts_keystore_gen_opts_t opts = {
        .exportable = false,
        .comment = comment,
    };
    return ts_keystore_store_key_ex(id, keypair, type, &opts);
}

esp_err_t ts_keystore_load_private_key(const char *id,
                                        char **private_key,
                                        size_t *private_key_len)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!id || !private_key || !private_key_len) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char nvs_key[TS_KEYSTORE_ID_MAX_LEN + 8];
    make_nvs_key(nvs_key, sizeof(nvs_key), id, "_priv");
    
    /* 获取长度 */
    size_t len = 0;
    esp_err_t ret = nvs_get_blob(s_keystore.nvs_handle, nvs_key, NULL, &len);
    if (ret != ESP_OK) {
        return (ret == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : ret;
    }
    
    /* 分配内存并读取 */
    char *buf = TS_MALLOC_PSRAM(len + 1);  /* +1 for null terminator */
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    
    ret = nvs_get_blob(s_keystore.nvs_handle, nvs_key, buf, &len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }
    
    buf[len] = '\0';  /* Ensure null termination for PEM string */
    *private_key = buf;
    
    /* 
     * 返回字符串长度（不含 null 终止符）
     * libssh2_userauth_publickey_frommemory 期望的长度不包含 null
     * 它内部会自己添加 null 并传递 len+1 给 mbedTLS
     */
    *private_key_len = strlen(buf);
    
    /* 更新使用时间 */
    ts_keystore_touch_key(id);
    
    ESP_LOGI(TAG, "Loaded private key '%s' (%zu bytes)", id, *private_key_len);
    return ESP_OK;
}

esp_err_t ts_keystore_load_public_key(const char *id,
                                       char **public_key,
                                       size_t *public_key_len)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!id || !public_key || !public_key_len) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char nvs_key[TS_KEYSTORE_ID_MAX_LEN + 8];
    make_nvs_key(nvs_key, sizeof(nvs_key), id, "_pub");
    
    /* 获取长度 */
    size_t len = 0;
    esp_err_t ret = nvs_get_blob(s_keystore.nvs_handle, nvs_key, NULL, &len);
    if (ret != ESP_OK) {
        return (ret == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : ret;
    }
    
    /* 分配内存并读取 */
    char *buf = TS_MALLOC_PSRAM(len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    
    ret = nvs_get_blob(s_keystore.nvs_handle, nvs_key, buf, &len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }
    
    buf[len] = '\0';
    *public_key = buf;
    /* 返回字符串长度（不含 null 终止符） */
    *public_key_len = strlen(buf);
    
    ESP_LOGI(TAG, "Loaded public key '%s' (%zu bytes)", id, *public_key_len);
    return ESP_OK;
}

esp_err_t ts_keystore_delete_key(const char *id)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!id) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_keystore.write_lock, portMAX_DELAY);
    if (!s_keystore.initialized) {
        xSemaphoreGive(s_keystore.write_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Deleting key '%s'", id);
    
    char nvs_key[TS_KEYSTORE_ID_MAX_LEN + 8];
    
    /* 删除私钥 */
    make_nvs_key(nvs_key, sizeof(nvs_key), id, "_priv");
    nvs_erase_key(s_keystore.nvs_handle, nvs_key);
    
    /* 删除公钥 */
    make_nvs_key(nvs_key, sizeof(nvs_key), id, "_pub");
    nvs_erase_key(s_keystore.nvs_handle, nvs_key);
    
    /* 删除元数据 */
    make_nvs_key(nvs_key, sizeof(nvs_key), id, "_meta");
    nvs_erase_key(s_keystore.nvs_handle, nvs_key);
    
    /* 从索引中移除 */
    remove_from_index(id);
    
    nvs_commit(s_keystore.nvs_handle);
    
    xSemaphoreGive(s_keystore.write_lock);
    ESP_LOGI(TAG, "Key '%s' deleted", id);
    return ESP_OK;
}

bool ts_keystore_key_exists(const char *id)
{
    if (!s_keystore.initialized || !id) {
        return false;
    }
    
    char nvs_key[TS_KEYSTORE_ID_MAX_LEN + 8];
    make_nvs_key(nvs_key, sizeof(nvs_key), id, "_priv");
    
    size_t len = 0;
    return nvs_get_blob(s_keystore.nvs_handle, nvs_key, NULL, &len) == ESP_OK;
}

esp_err_t ts_keystore_get_key_info(const char *id, ts_keystore_key_info_t *info)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!id || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return load_metadata(id, info);
}

esp_err_t ts_keystore_list_keys(ts_keystore_key_info_t *keys, size_t *count)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!keys || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Listing keys, max_count=%zu", *count);
    
    /* 读取索引 */
    char index_str[MAX_INDEX_LEN] = {0};
    size_t len = sizeof(index_str);
    
    esp_err_t ret = nvs_get_str(s_keystore.nvs_handle, KEYSTORE_INDEX_KEY, index_str, &len);
    
    ESP_LOGI(TAG, "NVS read index: ret=%s, index='%s'", esp_err_to_name(ret), index_str);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *count = 0;
        return ESP_OK;
    } else if (ret != ESP_OK) {
        return ret;
    }
    
    /* 解析索引 */
    char ids[TS_KEYSTORE_MAX_KEYS][TS_KEYSTORE_ID_MAX_LEN];
    int num_keys = parse_index(index_str, ids, TS_KEYSTORE_MAX_KEYS);
    
    ESP_LOGI(TAG, "Parsed %d keys from index", num_keys);
    
    /* 加载每个密钥的元数据 */
    size_t valid_count = 0;
    for (int i = 0; i < num_keys && valid_count < TS_KEYSTORE_MAX_KEYS; i++) {
        ESP_LOGI(TAG, "Loading metadata for key[%d]: '%s'", i, ids[i]);
        if (load_metadata(ids[i], &keys[valid_count]) == ESP_OK) {
            valid_count++;
        } else {
            ESP_LOGW(TAG, "Failed to load metadata for key '%s'", ids[i]);
        }
    }
    
    ESP_LOGI(TAG, "Returning %zu valid keys", valid_count);
    *count = valid_count;
    return ESP_OK;
}

esp_err_t ts_keystore_list_keys_ex(ts_keystore_key_info_t *keys, size_t *count, bool show_hidden)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!keys || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Listing keys (show_hidden=%d), max_count=%zu", show_hidden, *count);
    
    /* 读取索引 */
    char index_str[MAX_INDEX_LEN] = {0};
    size_t len = sizeof(index_str);
    
    esp_err_t ret = nvs_get_str(s_keystore.nvs_handle, KEYSTORE_INDEX_KEY, index_str, &len);
    
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *count = 0;
        return ESP_OK;
    } else if (ret != ESP_OK) {
        return ret;
    }
    
    /* 解析索引 */
    char ids[TS_KEYSTORE_MAX_KEYS][TS_KEYSTORE_ID_MAX_LEN];
    int num_keys = parse_index(index_str, ids, TS_KEYSTORE_MAX_KEYS);
    
    ESP_LOGI(TAG, "Parsed %d keys from index", num_keys);
    
    /* 加载每个密钥的元数据，根据 show_hidden 过滤 */
    size_t valid_count = 0;
    for (int i = 0; i < num_keys && valid_count < TS_KEYSTORE_MAX_KEYS; i++) {
        ts_keystore_key_info_t temp_info;
        if (load_metadata(ids[i], &temp_info) == ESP_OK) {
            /* 如果不显示隐藏密钥，则跳过隐藏的密钥 */
            if (!show_hidden && temp_info.hidden) {
                ESP_LOGD(TAG, "Skipping hidden key '%s'", ids[i]);
                continue;
            }
            keys[valid_count] = temp_info;
            valid_count++;
        } else {
            ESP_LOGW(TAG, "Failed to load metadata for key '%s'", ids[i]);
        }
    }
    
    ESP_LOGI(TAG, "Returning %zu valid keys (filtered)", valid_count);
    *count = valid_count;
    return ESP_OK;
}

esp_err_t ts_keystore_touch_key(const char *id)
{
    if (!s_keystore.initialized || !id) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_keystore.write_lock, portMAX_DELAY);
    ts_keystore_key_info_t info;
    esp_err_t ret = s_keystore.initialized ? load_metadata(id, &info) : ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK) {
        info.last_used = (uint32_t)time(NULL);
        ret = store_metadata(id, &info);
    }
    xSemaphoreGive(s_keystore.write_lock);
    return ret;
}

esp_err_t ts_keystore_import_from_file(const char *id, 
                                        const char *path,
                                        const char *comment)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!id || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Importing key from %s as '%s'", path, id);
    
    /* 读取私钥文件 */
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open private key file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    
    fseek(f, 0, SEEK_END);
    long priv_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (priv_len <= 0 || priv_len > TS_KEYSTORE_PRIVKEY_MAX_LEN) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    
    char *priv_key = TS_MALLOC_PSRAM(priv_len + 1);
    if (!priv_key) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    size_t read_len = fread(priv_key, 1, priv_len, f);
    fclose(f);
    priv_key[read_len] = '\0';
    
    /* 尝试读取公钥文件 */
    char pub_path[256];
    snprintf(pub_path, sizeof(pub_path), "%s.pub", path);
    
    char *pub_key = NULL;
    size_t pub_len = 0;
    
    f = fopen(pub_path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        pub_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        if (pub_len > 0 && pub_len < TS_KEYSTORE_PUBKEY_MAX_LEN) {
            pub_key = TS_MALLOC_PSRAM(pub_len + 1);
            if (pub_key) {
                size_t read = fread(pub_key, 1, pub_len, f);
                pub_key[read] = '\0';
                pub_len = read;
            }
        }
        fclose(f);
    }
    
    /* 检测密钥类型 */
    ts_keystore_key_type_t type = TS_KEYSTORE_TYPE_UNKNOWN;
    if (strstr(priv_key, "BEGIN RSA PRIVATE KEY")) {
        /* 需要进一步检测位数 */
        if (priv_len > 2500) {
            type = TS_KEYSTORE_TYPE_RSA_4096;
        } else {
            type = TS_KEYSTORE_TYPE_RSA_2048;
        }
    } else if (strstr(priv_key, "BEGIN EC PRIVATE KEY")) {
        if (priv_len > 300) {
            type = TS_KEYSTORE_TYPE_ECDSA_P384;
        } else {
            type = TS_KEYSTORE_TYPE_ECDSA_P256;
        }
    }
    
    /* 存储密钥 */
    ts_keystore_keypair_t keypair = {
        .private_key = priv_key,
        .private_key_len = read_len,
        .public_key = pub_key,
        .public_key_len = pub_len,
    };
    
    esp_err_t ret = ts_keystore_store_key(id, &keypair, type, comment);
    
    /* 安全清零私钥内存 */
    secure_free_key(priv_key, read_len);
    if (pub_key) free(pub_key);
    
    return ret;
}

esp_err_t ts_keystore_export_public_key_to_file(const char *id, const char *path)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!id || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Exporting public key '%s' to %s", id, path);
    
    /*
     * 安全策略：只允许导出公钥，私钥永不离开安全存储
     * 这是 TianShanOS 安全模型的核心原则
     */
    
    /* 加载公钥 */
    char *pub_key = NULL;
    size_t pub_len = 0;
    
    esp_err_t ret = ts_keystore_load_public_key(id, &pub_key, &pub_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load public key '%s': %s", id, esp_err_to_name(ret));
        return ret;
    }
    
    if (!pub_key || pub_len == 0) {
        ESP_LOGE(TAG, "Key '%s' has no public key", id);
        return ESP_ERR_NOT_FOUND;
    }
    
    /* 写入公钥文件 */
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create file: %s", path);
        free(pub_key);
        return ESP_ERR_NOT_FOUND;
    }
    
    size_t written = fwrite(pub_key, 1, pub_len, f);
    fclose(f);
    free(pub_key);
    
    if (written != pub_len) {
        ESP_LOGE(TAG, "Write incomplete: %zu/%zu bytes", written, pub_len);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Public key '%s' exported to %s", id, path);
    return ESP_OK;
}

/* 
 * 兼容性包装器 - 已废弃，仅导出公钥
 * @deprecated 使用 ts_keystore_export_public_key_to_file() 替代
 */
esp_err_t ts_keystore_export_to_file(const char *id, const char *path)
{
    ESP_LOGW(TAG, "ts_keystore_export_to_file() is deprecated, use ts_keystore_export_public_key_to_file()");
    ESP_LOGW(TAG, "Security policy: Private keys NEVER leave secure storage unless exportable=true");
    return ts_keystore_export_public_key_to_file(id, path);
}

esp_err_t ts_keystore_export_private_key_to_file(const char *id, const char *path)
{
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!id || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 检查密钥是否存在并获取元数据 */
    ts_keystore_key_info_t info;
    esp_err_t ret = ts_keystore_get_key_info(id, &info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Key '%s' not found", id);
        return ESP_ERR_NOT_FOUND;
    }
    
    /* 安全检查：只有 exportable=true 的密钥才能导出 */
    if (!info.exportable) {
        ESP_LOGE(TAG, "Key '%s' is not exportable (security policy)", id);
        ESP_LOGW(TAG, "To export private keys, generate with --exportable flag");
        return ESP_ERR_NOT_ALLOWED;
    }
    
    ESP_LOGW(TAG, "Exporting private key '%s' to %s (exportable=true)", id, path);
    
    /* 加载私钥 */
    char *priv_key = NULL;
    size_t priv_len = 0;
    
    ret = ts_keystore_load_private_key(id, &priv_key, &priv_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load private key '%s': %s", id, esp_err_to_name(ret));
        return ret;
    }
    
    /* 写入私钥文件 */
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create file: %s", path);
        secure_free_key(priv_key, priv_len);
        return ESP_ERR_NOT_FOUND;
    }
    
    /* 设置文件权限为 600（仅所有者可读写）- 如果支持 */
#ifdef __unix__
    chmod(path, 0600);
#endif
    
    size_t written = fwrite(priv_key, 1, priv_len, f);
    fclose(f);
    
    /* 安全清零内存 */
    secure_free_key(priv_key, priv_len);
    
    if (written != priv_len) {
        ESP_LOGE(TAG, "Write incomplete: %zu/%zu bytes", written, priv_len);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Private key '%s' exported to %s", id, path);
    return ESP_OK;
}

esp_err_t ts_keystore_generate_key_with_result(const char *id,
                                       ts_keystore_key_type_t type,
                                       const ts_keystore_gen_opts_t *opts,
                                       ts_keystore_generate_result_t *detail)
{
    ts_keystore_generate_result_t local = {0};
    if (!detail) detail = &local;
    *detail = (ts_keystore_generate_result_t){ .failed_stage = "validation", .cleanup_complete = true };
    if (!s_keystore.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!id || !*id || strlen(id) > NVS_KEY_NAME_MAX_SIZE - 1 - 5 || strchr(id, ',') || type == TS_KEYSTORE_TYPE_UNKNOWN) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 使用默认选项 */
    ts_keystore_gen_opts_t default_opts = TS_KEYSTORE_GEN_OPTS_DEFAULT;
    if (!opts) {
        opts = &default_opts;
    }
    
    ESP_LOGI(TAG, "Generating %s key as '%s' (exportable=%d)", 
             ts_keystore_type_to_string(type), id, opts->exportable);
    
    /* 映射 keystore 类型到 crypto 类型 */
    ts_crypto_key_type_t crypto_type;
    switch (type) {
        case TS_KEYSTORE_TYPE_RSA_2048:
            crypto_type = TS_CRYPTO_KEY_RSA_2048;
            break;
        case TS_KEYSTORE_TYPE_RSA_4096:
            crypto_type = TS_CRYPTO_KEY_RSA_4096;
            break;
        case TS_KEYSTORE_TYPE_ECDSA_P256:
            crypto_type = TS_CRYPTO_KEY_EC_P256;
            break;
        case TS_KEYSTORE_TYPE_ECDSA_P384:
            crypto_type = TS_CRYPTO_KEY_EC_P384;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    char old_index[MAX_INDEX_LEN];
    bool index_present;
    xSemaphoreTake(s_keystore.write_lock, portMAX_DELAY);
    esp_err_t ret = check_new_key(id, old_index, &index_present, detail);
    xSemaphoreGive(s_keystore.write_lock);
    if (ret != ESP_OK) return ret;
    detail->failed_stage = "generate";
    /* 生成密钥对 */
    ts_keypair_t keypair = NULL;
    ret = ts_crypto_keypair_generate(crypto_type, &keypair);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate keypair: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 导出私钥 PEM */
    detail->failed_stage = "private_export";
    char *priv_key = TS_MALLOC_PSRAM(TS_KEYSTORE_PRIVKEY_MAX_LEN);
    if (!priv_key) {
        ts_crypto_keypair_free(keypair);
        return ESP_ERR_NO_MEM;
    }
    size_t priv_len = TS_KEYSTORE_PRIVKEY_MAX_LEN;
    ret = ts_crypto_keypair_export_private(keypair, priv_key, &priv_len);
    if (ret != ESP_OK || priv_len == 0) {
        ESP_LOGE(TAG, "Failed to export private key: %s", esp_err_to_name(ret));
        secure_free_key(priv_key, TS_KEYSTORE_PRIVKEY_MAX_LEN);
        ts_crypto_keypair_free(keypair);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }
    
    /* 导出公钥 OpenSSH 格式 */
    detail->failed_stage = "public_export";
    char *pub_key = TS_MALLOC_PSRAM(TS_KEYSTORE_PUBKEY_MAX_LEN);
    size_t pub_len = TS_KEYSTORE_PUBKEY_MAX_LEN;
    ret = pub_key ? ts_crypto_keypair_export_openssh(keypair, pub_key, &pub_len, opts->comment) : ESP_ERR_NO_MEM;
    if (ret != ESP_OK || pub_len == 0) {
        secure_free_key(priv_key, TS_KEYSTORE_PRIVKEY_MAX_LEN);
        free(pub_key);
        ts_crypto_keypair_free(keypair);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }

    ts_crypto_keypair_free(keypair);
    
    /* 存储密钥 */
    ts_keystore_keypair_t kp = {
        .private_key = priv_key,
        .private_key_len = priv_len,
        .public_key = pub_key,
        .public_key_len = pub_len,
    };
    
    xSemaphoreTake(s_keystore.write_lock, portMAX_DELAY);
    ret = check_new_key(id, old_index, &index_present, detail);
    if (ret == ESP_OK) {
        ret = store_key_locked(id, &kp, type, opts, detail);
        if (ret != ESP_OK) {
            bool index_attempted = strcmp(detail->failed_stage, "index_write") == 0 ||
                                   strcmp(detail->failed_stage, "commit") == 0;
            detail->cleanup_error = cleanup_new_key(id, old_index, index_attempted, index_present);
            detail->cleanup_complete = detail->cleanup_error == ESP_OK;
        }
    }
    xSemaphoreGive(s_keystore.write_lock);
    
    /* 安全清零私钥内存 */
    secure_free_key(priv_key, priv_len);
    if (pub_key) free(pub_key);
    
    return ret;
}

esp_err_t ts_keystore_generate_key_ex(const char *id,
                                      ts_keystore_key_type_t type,
                                      const ts_keystore_gen_opts_t *opts)
{
    return ts_keystore_generate_key_with_result(id, type, opts, NULL);
}

esp_err_t ts_keystore_generate_key(const char *id,
                                    ts_keystore_key_type_t type,
                                    const char *comment)
{
    ts_keystore_gen_opts_t opts = {
        .exportable = false,
        .comment = comment,
    };
    return ts_keystore_generate_key_ex(id, type, &opts);
}

const char *ts_keystore_type_to_string(ts_keystore_key_type_t type)
{
    switch (type) {
        case TS_KEYSTORE_TYPE_RSA_2048:   return "rsa2048";
        case TS_KEYSTORE_TYPE_RSA_4096:   return "rsa4096";
        case TS_KEYSTORE_TYPE_ECDSA_P256: return "ecdsa-p256";
        case TS_KEYSTORE_TYPE_ECDSA_P384: return "ecdsa-p384";
        default:                          return "unknown";
    }
}

ts_keystore_key_type_t ts_keystore_type_from_string(const char *str)
{
    if (!str) return TS_KEYSTORE_TYPE_UNKNOWN;
    
    if (strcmp(str, "rsa2048") == 0 || strcmp(str, "rsa") == 0) {
        return TS_KEYSTORE_TYPE_RSA_2048;
    }
    if (strcmp(str, "rsa4096") == 0) {
        return TS_KEYSTORE_TYPE_RSA_4096;
    }
    if (strcmp(str, "ecdsa-p256") == 0 || strcmp(str, "ecdsa") == 0 || strcmp(str, "ec256") == 0) {
        return TS_KEYSTORE_TYPE_ECDSA_P256;
    }
    if (strcmp(str, "ecdsa-p384") == 0 || strcmp(str, "ec384") == 0) {
        return TS_KEYSTORE_TYPE_ECDSA_P384;
    }
    
    return TS_KEYSTORE_TYPE_UNKNOWN;
}
