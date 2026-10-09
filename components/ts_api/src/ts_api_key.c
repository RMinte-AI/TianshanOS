/**
 * @file ts_api_key.c
 * @brief Key Management API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_keystore.h"
#include "ts_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <string.h>

#define TAG "api_key"

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static const char *key_type_desc(ts_keystore_key_type_t type)
{
    switch (type) {
        case TS_KEYSTORE_TYPE_RSA_2048:  return "RSA 2048-bit";
        case TS_KEYSTORE_TYPE_RSA_4096:  return "RSA 4096-bit";
        case TS_KEYSTORE_TYPE_ECDSA_P256: return "ECDSA P-256";
        case TS_KEYSTORE_TYPE_ECDSA_P384: return "ECDSA P-384";
        default: return "Unknown";
    }
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief key.list - List all stored keys
 */
static esp_err_t api_key_list(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    TS_LOGI(TAG, "API: key.list called");
    
    /* 检查是否显示隐藏密钥（TODO: 从认证状态判断） */
    bool show_hidden = true;  // 暂时默认显示全部，后续接入认证系统
    
    /* 使用 PSRAM 分配避免栈溢出 */
    ts_keystore_key_info_t *keys = heap_caps_malloc(
        sizeof(ts_keystore_key_info_t) * TS_KEYSTORE_MAX_KEYS,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!keys) {
        keys = malloc(sizeof(ts_keystore_key_info_t) * TS_KEYSTORE_MAX_KEYS);
    }
    if (!keys) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    size_t count = TS_KEYSTORE_MAX_KEYS;
    
    esp_err_t ret = ts_keystore_list_keys_ex(keys, &count, show_hidden);
    
    TS_LOGI(TAG, "API: ts_keystore_list_keys_ex returned %s, count=%zu", esp_err_to_name(ret), count);
    
    // 如果模块未初始化，返回空列表而不是错误
    if (ret == ESP_ERR_INVALID_STATE) {
        free(keys);
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "count", 0);
        cJSON_AddNumberToObject(data, "max_keys", TS_KEYSTORE_MAX_KEYS);
        cJSON_AddArrayToObject(data, "keys");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    if (ret != ESP_OK) {
        free(keys);
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to list keys");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        free(keys);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddNumberToObject(data, "count", count);
    cJSON_AddNumberToObject(data, "max_keys", TS_KEYSTORE_MAX_KEYS);
    
    cJSON *keys_array = cJSON_AddArrayToObject(data, "keys");
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        
        /* 如果是隐藏密钥且未授权，则掩码 ID */
        if (keys[i].hidden && !show_hidden) {
            /* 掩码格式：🔒 <alias> 或 🔒 Hidden Key */
            const char *display_id = (strlen(keys[i].alias) > 0) ? keys[i].alias : "Hidden Key";
            char masked_id[TS_KEYSTORE_ALIAS_MAX_LEN + 8];
            snprintf(masked_id, sizeof(masked_id), "🔒 %s", display_id);
            cJSON_AddStringToObject(item, "id", masked_id);
        } else {
            cJSON_AddStringToObject(item, "id", keys[i].id);
        }
        
        cJSON_AddStringToObject(item, "type", ts_keystore_type_to_string(keys[i].type));
        cJSON_AddStringToObject(item, "type_desc", key_type_desc(keys[i].type));
        cJSON_AddStringToObject(item, "comment", keys[i].comment);
        cJSON_AddStringToObject(item, "alias", keys[i].alias);
        cJSON_AddNumberToObject(item, "created", keys[i].created_at);
        cJSON_AddNumberToObject(item, "last_used", keys[i].last_used);
        cJSON_AddBoolToObject(item, "has_pubkey", keys[i].has_public_key);
        cJSON_AddBoolToObject(item, "exportable", keys[i].exportable);
        cJSON_AddBoolToObject(item, "hidden", keys[i].hidden);
        cJSON_AddItemToArray(keys_array, item);
    }
    
    free(keys);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief key.info - Get key info
 * 
 * @param params { "id": "key_id" }
 */
static esp_err_t api_key_info(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *id = cJSON_GetObjectItem(params, "id");
    if (!id || !cJSON_IsString(id)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'id' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_keystore_key_info_t info;
    esp_err_t ret = ts_keystore_get_key_info(id->valuestring, &info);
    
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Key not found");
        return ESP_ERR_NOT_FOUND;
    } else if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get key info");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(data, "id", info.id);
    cJSON_AddStringToObject(data, "type", ts_keystore_type_to_string(info.type));
    cJSON_AddStringToObject(data, "type_desc", key_type_desc(info.type));
    cJSON_AddStringToObject(data, "comment", info.comment);
    cJSON_AddStringToObject(data, "alias", info.alias);
    cJSON_AddNumberToObject(data, "created", info.created_at);
    cJSON_AddNumberToObject(data, "last_used", info.last_used);
    cJSON_AddBoolToObject(data, "has_public_key", info.has_public_key);
    cJSON_AddBoolToObject(data, "exportable", info.exportable);
    cJSON_AddBoolToObject(data, "hidden", info.hidden);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief key.generate - Generate a new key
 * 
 * @param params { "id": "key_id", "type": "rsa2048", "comment": "...", "exportable": false }
 */
static esp_err_t api_key_generate(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *id = cJSON_GetObjectItem(params, "id");
    if (!id || !cJSON_IsString(id)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'id' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Parse key type */
    ts_keystore_key_type_t type = TS_KEYSTORE_TYPE_RSA_2048;  /* Default */
    const cJSON *type_json = cJSON_GetObjectItem(params, "type");
    if (type_json && cJSON_IsString(type_json)) {
        const char *type_str = type_json->valuestring;
        if (strcmp(type_str, "rsa4096") == 0) {
            type = TS_KEYSTORE_TYPE_RSA_4096;
        } else if (strcmp(type_str, "ec256") == 0 || strcmp(type_str, "ecdsa") == 0) {
            type = TS_KEYSTORE_TYPE_ECDSA_P256;
        } else if (strcmp(type_str, "ec384") == 0) {
            type = TS_KEYSTORE_TYPE_ECDSA_P384;
        }
    }
    
    /* Parse comment */
    const char *comment = "";
    const cJSON *comment_json = cJSON_GetObjectItem(params, "comment");
    if (comment_json && cJSON_IsString(comment_json)) {
        comment = comment_json->valuestring;
    }
    
    /* Parse exportable flag */
    bool exportable = false;
    const cJSON *exportable_json = cJSON_GetObjectItem(params, "exportable");
    if (exportable_json && cJSON_IsBool(exportable_json)) {
        exportable = cJSON_IsTrue(exportable_json);
    }
    
    /* Parse alias */
    const char *alias = NULL;
    const cJSON *alias_json = cJSON_GetObjectItem(params, "alias");
    if (alias_json && cJSON_IsString(alias_json)) {
        alias = alias_json->valuestring;
    }
    
    /* Parse hidden flag */
    bool hidden = false;
    const cJSON *hidden_json = cJSON_GetObjectItem(params, "hidden");
    if (hidden_json && cJSON_IsBool(hidden_json)) {
        hidden = cJSON_IsTrue(hidden_json);
    }
    
    /* Generate key using extended API with options */
    ts_keystore_gen_opts_t opts = {
        .exportable = exportable,
        .comment = comment,
        .alias = alias,
        .hidden = hidden,
    };
    
    char request_id[32] = "legacy";
    const cJSON *rid = cJSON_GetObjectItem(params, "request_id");
    if (cJSON_IsString(rid) && strlen(rid->valuestring) < sizeof(request_id)) {
        bool valid = rid->valuestring[0] != '\0';
        for (const char *p = rid->valuestring; *p; ++p) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-')) valid = false;
        }
        if (valid) strcpy(request_id, rid->valuestring);
    }
    ts_keystore_generate_result_t detail;
    esp_err_t ret = ts_keystore_generate_key_with_result(id->valuestring, type, &opts, &detail);
    TS_LOGI(TAG, "key.generate request=%s storage id=%s ret=%s stage=%s stored=%d cleanup=%s", request_id, id->valuestring,
            esp_err_to_name(ret), detail.failed_stage ? detail.failed_stage : "saved", detail.stored,
            esp_err_to_name(detail.cleanup_error));
    if (ret != ESP_OK) {
        const char *reason = ret == ESP_ERR_NO_MEM ? "Memory allocation failed" :
            ret == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "key_storage_full" :
            strcmp(detail.failed_stage, "id_occupied") == 0 ? "key_id_occupied" :
            strcmp(detail.failed_stage, "index_capacity") == 0 ? "key_limit_reached" :
            ret == ESP_ERR_INVALID_ARG ? "key_invalid_id" : "key_storage_failed";
        ts_api_result_error(result, ret == ESP_ERR_NO_MEM ? TS_API_ERR_NO_MEM :
                            ret == ESP_ERR_INVALID_ARG ? TS_API_ERR_INVALID_ARG : TS_API_ERR_INTERNAL, reason);
        cJSON *data = cJSON_CreateObject();
        if (data && cJSON_AddStringToObject(data, "id", id->valuestring) &&
            cJSON_AddStringToObject(data, "failed_stage", detail.failed_stage) &&
            cJSON_AddStringToObject(data, "esp_error", esp_err_to_name(ret)) &&
            cJSON_AddBoolToObject(data, "cleanup_complete", detail.cleanup_complete) &&
            cJSON_AddStringToObject(data, "cleanup_error", esp_err_to_name(detail.cleanup_error))) {
            result->data = data;
        } else {
            cJSON_Delete(data);
            TS_LOGE(TAG, "key.generate request=%s error response allocation failed", request_id);
        }
        return ret;
    }

    cJSON *data = cJSON_CreateObject();
    if (!data || !cJSON_AddBoolToObject(data, "generated", true) ||
        !cJSON_AddStringToObject(data, "id", id->valuestring) ||
        !cJSON_AddStringToObject(data, "type", ts_keystore_type_to_string(type))) {
        cJSON_Delete(data);
        TS_LOGE(TAG, "key.generate request=%s saved id=%s but response allocation failed", request_id, id->valuestring);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "key_response_failed");
        return ESP_ERR_NO_MEM;
    }

    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief key.delete - Delete a key
 * 
 * @param params { "id": "key_id" }
 */
static esp_err_t api_key_delete(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *id = cJSON_GetObjectItem(params, "id");
    if (!id || !cJSON_IsString(id)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'id' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_keystore_delete_key(id->valuestring);
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Key not found");
        return ESP_ERR_NOT_FOUND;
    } else if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to delete key");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "deleted", true);
    cJSON_AddStringToObject(data, "id", id->valuestring);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief key.export - Export public key
 * 
 * @param params { "id": "key_id" }
 * @return Public key in OpenSSH format
 */
static esp_err_t api_key_export(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *id = cJSON_GetObjectItem(params, "id");
    if (!id || !cJSON_IsString(id)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'id' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 加载公钥 */
    char *pubkey = NULL;
    size_t pubkey_len = 0;
    esp_err_t ret = ts_keystore_load_public_key(id->valuestring, &pubkey, &pubkey_len);
    
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Key not found");
        return ESP_ERR_NOT_FOUND;
    } else if (ret != ESP_OK || !pubkey) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to export public key");
        return ret;
    }
    
    /* 获取密钥信息 */
    ts_keystore_key_info_t info;
    ts_keystore_get_key_info(id->valuestring, &info);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "id", id->valuestring);
    cJSON_AddStringToObject(data, "type", ts_keystore_type_to_string(info.type));
    cJSON_AddStringToObject(data, "public_key", pubkey);
    cJSON_AddStringToObject(data, "comment", info.comment);
    
    free(pubkey);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief key.exportPrivate - Export private key (only for exportable keys)
 * 
 * @param params { "id": "key_id" }
 * @return Private key in PEM format
 */
static esp_err_t api_key_export_private(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *id = cJSON_GetObjectItem(params, "id");
    if (!id || !cJSON_IsString(id)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'id' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 获取密钥信息，检查是否可导出 */
    ts_keystore_key_info_t info;
    esp_err_t ret = ts_keystore_get_key_info(id->valuestring, &info);
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Key not found");
        return ESP_ERR_NOT_FOUND;
    } else if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to get key info");
        return ret;
    }
    
    /* 安全检查：只有 exportable=true 的密钥才能导出私钥 */
    if (!info.exportable) {
        ts_api_result_error(result, TS_API_ERR_NO_PERMISSION, 
            "Private key is not exportable. Generate key with --exportable flag to allow export.");
        return ESP_ERR_NOT_ALLOWED;
    }
    
    /* 加载私钥 */
    char *privkey = NULL;
    size_t privkey_len = 0;
    ret = ts_keystore_load_private_key(id->valuestring, &privkey, &privkey_len);
    
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Private key not found");
        return ESP_ERR_NOT_FOUND;
    } else if (ret != ESP_OK || !privkey) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to export private key");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "id", id->valuestring);
    cJSON_AddStringToObject(data, "type", ts_keystore_type_to_string(info.type));
    cJSON_AddStringToObject(data, "private_key", privkey);
    cJSON_AddStringToObject(data, "comment", info.comment);
    
    /* 安全警告 */
    cJSON_AddStringToObject(data, "warning", "Keep this private key secure! Never share it publicly.");
    
    free(privkey);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_api_key_register(void)
{
    static const ts_api_endpoint_t endpoints[] = {
        {
            .name = "key.list",
            .description = "List all stored keys",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_key_list,
            .requires_auth = false,
        },
        {
            .name = "key.info",
            .description = "Get key info",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_key_info,
            .requires_auth = false,
        },
        {
            .name = "key.generate",
            .description = "Generate a new key",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_key_generate,
            .requires_auth = true,
        },
        {
            .name = "key.delete",
            .description = "Delete a key",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_key_delete,
            .requires_auth = true,
        },
        {
            .name = "key.export",
            .description = "Export public key",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_key_export,
            .requires_auth = false,
        },
        {
            .name = "key.exportPrivate",
            .description = "Export private key (only for exportable keys)",
            .category = TS_API_CAT_SYSTEM,
            .handler = api_key_export_private,
            .requires_auth = true,  /* 导出私钥需要认证 */
        },
    };
    
    return ts_api_register_multiple(endpoints, sizeof(endpoints) / sizeof(endpoints[0]));
}
