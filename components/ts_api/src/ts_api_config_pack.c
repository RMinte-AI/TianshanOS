/**
 * @file ts_api_config_pack.c
 * @brief Configuration Pack API Handlers
 * 
 * WebUI API endpoints for encrypted configuration pack operations.
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-02-04
 */

#include "ts_api.h"
#include "ts_config_pack.h"
#include "ts_api_pack.h"
#include "ts_cert.h"
#include "ts_log.h"
#include "ts_storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#define TAG "api_config_pack"

/* PSRAM 优先分配 */
#define PACK_MALLOC(size) ({ \
    void *p = heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); \
    p ? p : malloc(size); \
})

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief config.pack.info - Get config pack system information
 * 
 * Returns:
 * - can_export: Whether this device can export config packs
 * - device_type: "Developer" or "Device"
 * - cert_fingerprint: Device certificate fingerprint
 * - cert_cn: Device certificate Common Name
 */
static esp_err_t api_config_pack_info(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* 检查是否可以导出 */
    bool can_export = ts_config_pack_can_export();
    cJSON_AddBoolToObject(data, "can_export", can_export);
    cJSON_AddStringToObject(data, "device_type", can_export ? "Developer" : "Device");
    
    /* 获取证书指纹 */
    char fingerprint[65] = {0};
    if (ts_config_pack_get_cert_fingerprint(fingerprint, sizeof(fingerprint)) == ESP_OK) {
        cJSON_AddStringToObject(data, "cert_fingerprint", fingerprint);
    }
    
    /* 获取证书 CN */
    ts_cert_info_t cert_info;
    if (ts_cert_get_info(&cert_info) == ESP_OK) {
        cJSON_AddStringToObject(data, "cert_cn", cert_info.subject_cn);
        cJSON_AddStringToObject(data, "cert_ou", cert_info.subject_ou);
    }
    
    /* 系统版本 */
    cJSON_AddStringToObject(data, "pack_version", TS_CONFIG_PACK_VERSION);
    cJSON_AddStringToObject(data, "algorithm", TS_CONFIG_PACK_ALGORITHM);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.pack.export_cert - Export device certificate
 * 
 * Returns PEM-encoded device certificate that can be used by
 * other devices to encrypt configs for this device.
 */
static esp_err_t api_config_pack_export_cert(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    /* 分配证书缓冲区 */
    size_t cert_len = 4096;
    char *cert_pem = PACK_MALLOC(cert_len);
    if (!cert_pem) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t ret = ts_config_pack_export_device_cert(cert_pem, &cert_len);
    if (ret != ESP_OK) {
        free(cert_pem);
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to export certificate");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        free(cert_pem);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(data, "certificate", cert_pem);
    
    /* 添加指纹 */
    char fingerprint[65] = {0};
    if (ts_config_pack_get_cert_fingerprint(fingerprint, sizeof(fingerprint)) == ESP_OK) {
        cJSON_AddStringToObject(data, "fingerprint", fingerprint);
    }
    
    /* 添加 CN */
    ts_cert_info_t info;
    if (ts_cert_get_info(&info) == ESP_OK) {
        cJSON_AddStringToObject(data, "cn", info.subject_cn);
    }
    
    free(cert_pem);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.pack.verify - Verify a .tscfg package
 * 
 * Parameters:
 * - content: Base64 or raw JSON content of .tscfg file
 * - path: Alternative - path to .tscfg file on device
 * 
 * Returns signature verification info without decrypting.
 */
static esp_err_t api_config_pack_verify(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_config_pack_sig_info_t sig_info = {0};
    ts_config_pack_result_t pack_result;
    
    /* 优先使用 content 参数 */
    const cJSON *content = cJSON_GetObjectItem(params, "content");
    const cJSON *path = cJSON_GetObjectItem(params, "path");
    
    if (content && cJSON_IsString(content)) {
        pack_result = ts_config_pack_verify_mem(
            content->valuestring,
            strlen(content->valuestring),
            &sig_info
        );
    } else if (path && cJSON_IsString(path)) {
        pack_result = ts_config_pack_verify(path->valuestring, &sig_info);
    } else {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, 
                           "Missing 'content' or 'path' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddBoolToObject(data, "valid", pack_result == TS_CONFIG_PACK_OK);
    cJSON_AddNumberToObject(data, "result_code", pack_result);
    cJSON_AddStringToObject(data, "result_message", ts_config_pack_strerror(pack_result));
    cJSON_AddStringToObject(data, "reason", ts_config_pack_reason(pack_result));
    
    if (pack_result == TS_CONFIG_PACK_OK || sig_info.signer_cn[0] != '\0') {
        cJSON *sig = cJSON_CreateObject();
        cJSON_AddBoolToObject(sig, "valid", sig_info.valid);
        cJSON_AddBoolToObject(sig, "is_official", sig_info.is_official);
        cJSON_AddStringToObject(sig, "signer_cn", sig_info.signer_cn);
        cJSON_AddStringToObject(sig, "signer_ou", sig_info.signer_ou);
        cJSON_AddNumberToObject(sig, "signed_at", (double)sig_info.signed_at);
        cJSON_AddItemToObject(data, "signature", sig);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.pack.import - Validate a .tscfg file in place
 * 
 * Parameters:
 * - path: Path to already uploaded .tscfg file
 * 
 * Validates the file without copying. Use this after file upload via
 * storage API to get metadata and validation status.
 * 
 * Note: The upload hook already performs auto-validation and sends
 * WebSocket notification. This API is for explicit re-validation.
 */
static esp_err_t api_config_pack_import(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *path = cJSON_GetObjectItem(params, "path");
    if (!path || !cJSON_IsString(path)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'path' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 验证配置包（不复制） */
    ts_config_pack_metadata_t metadata = {0};
    ts_config_pack_result_t pack_result = ts_config_pack_validate_file(
        path->valuestring,
        &metadata
    );
    
    /* 构建响应 */
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddBoolToObject(data, "valid", pack_result == TS_CONFIG_PACK_OK);
    cJSON_AddNumberToObject(data, "result_code", pack_result);
    cJSON_AddStringToObject(data, "result_message", ts_config_pack_strerror(pack_result));
    cJSON_AddStringToObject(data, "reason", ts_config_pack_reason(pack_result));
    cJSON_AddStringToObject(data, "path", path->valuestring);
    
    if (pack_result == TS_CONFIG_PACK_OK) {
        /* 元数据 */
        cJSON_AddStringToObject(data, "name", metadata.name);
        cJSON_AddStringToObject(data, "description", metadata.description);
        cJSON_AddStringToObject(data, "source_file", metadata.source_file);
        cJSON_AddStringToObject(data, "target_device", metadata.target_device);
        cJSON_AddNumberToObject(data, "created_at", (double)metadata.created_at);
        
        /* 签名信息 */
        cJSON *sig = cJSON_CreateObject();
        cJSON_AddBoolToObject(sig, "valid", metadata.sig_info.valid);
        cJSON_AddBoolToObject(sig, "is_official", metadata.sig_info.is_official);
        cJSON_AddStringToObject(sig, "signer_cn", metadata.sig_info.signer_cn);
        cJSON_AddStringToObject(sig, "signer_ou", metadata.sig_info.signer_ou);
        cJSON_AddNumberToObject(sig, "signed_at", (double)metadata.sig_info.signed_at);
        cJSON_AddItemToObject(data, "signature", sig);
        
        TS_LOGI(TAG, "Config pack validated: %s (%s)", metadata.name, path->valuestring);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.pack.apply - Apply configuration from a validated .tscfg file
 * 
 * Parameters:
 * - path: Path to .tscfg file
 * 
 * Decrypts and applies the configuration to the system.
 */
static esp_err_t api_config_pack_apply(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *path = cJSON_GetObjectItem(params, "path");
    if (!path || !cJSON_IsString(path)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'path' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 应用配置包 */
    cJSON *applied_modules = NULL;
    ts_config_pack_result_t pack_result = ts_config_pack_apply_file(
        path->valuestring,
        &applied_modules
    );
    
    /* 构建响应 */
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        if (applied_modules) cJSON_Delete(applied_modules);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddBoolToObject(data, "success", pack_result == TS_CONFIG_PACK_OK);
    cJSON_AddNumberToObject(data, "result_code", pack_result);
    cJSON_AddStringToObject(data, "result_message", ts_config_pack_strerror(pack_result));
    cJSON_AddStringToObject(data, "reason", ts_config_pack_reason(pack_result));
    cJSON_AddStringToObject(data, "path", path->valuestring);
    
    if (pack_result == TS_CONFIG_PACK_OK && applied_modules) {
        cJSON_AddItemToObject(data, "applied_modules", applied_modules);
        TS_LOGI(TAG, "Config pack applied: %s", path->valuestring);
    } else {
        if (applied_modules) cJSON_Delete(applied_modules);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.pack.export - Export configuration as encrypted .tscfg
 * 
 * Parameters:
 * - name: Config name
 * - content: JSON content to encrypt (object or string)
 * - recipient_cert: PEM-encoded target device certificate
 * - description: Optional description
 * 
 * Requires device to have OU=Developer.
 */
static esp_err_t api_config_pack_export(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 检查权限 */
    if (!ts_config_pack_can_export()) {
        ts_api_result_error(result, TS_API_ERR_NO_PERMISSION,
                           "Export requires Developer device (OU=Developer)");
        return ESP_ERR_NOT_ALLOWED;
    }
    
    /* 获取参数 */
    const cJSON *name = cJSON_GetObjectItem(params, "name");
    const cJSON *content = cJSON_GetObjectItem(params, "content");
    const cJSON *recipient_cert = cJSON_GetObjectItem(params, "recipient_cert");
    const cJSON *description = cJSON_GetObjectItem(params, "description");
    
    if (!name || !cJSON_IsString(name)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'name' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!content) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'content' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!recipient_cert || !cJSON_IsString(recipient_cert)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, 
                           "Missing 'recipient_cert' parameter (PEM certificate)");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 准备 JSON 内容 */
    char *json_content = NULL;
    size_t json_len = 0;
    
    if (cJSON_IsString(content)) {
        json_content = strdup(content->valuestring);
        json_len = strlen(json_content);
    } else {
        json_content = cJSON_PrintUnformatted(content);
        json_len = strlen(json_content);
    }
    
    if (!json_content) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* 准备导出选项 */
    ts_config_pack_export_opts_t opts = {
        .recipient_cert_pem = recipient_cert->valuestring,
        .recipient_cert_len = strlen(recipient_cert->valuestring) + 1,  /* +1 for null terminator, required by mbedtls */
        .description = (description && cJSON_IsString(description)) 
                       ? description->valuestring : NULL
    };
    
    /* 创建配置包 */
    char *output = NULL;
    size_t output_len = 0;
    
    ts_config_pack_result_t pack_result = ts_config_pack_create(
        name->valuestring,
        json_content,
        json_len,
        &opts,
        &output,
        &output_len
    );
    
    free(json_content);
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        pack_failure(result, pack_result);
        return ESP_FAIL;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        free(output);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* 返回完整的 .tscfg JSON */
    cJSON_AddStringToObject(data, "tscfg", output);
    cJSON_AddNumberToObject(data, "size", (double)output_len);
    
    /* 建议的文件名 */
    char filename[128];
    snprintf(filename, sizeof(filename), "%s%s", name->valuestring, TS_CONFIG_PACK_EXT);
    cJSON_AddStringToObject(data, "filename", filename);
    
    /* 如果提供了保存路径，保存到文件 */
    cJSON *save_path = cJSON_GetObjectItem(params, "save_path");
    if (save_path && cJSON_IsString(save_path) && strlen(save_path->valuestring) > 0) {
        const char *path = save_path->valuestring;
        
        /* 确保目录存在 */
        char dir_path[256];
        strncpy(dir_path, path, sizeof(dir_path) - 1);
        dir_path[sizeof(dir_path) - 1] = '\0';
        char *last_slash = strrchr(dir_path, '/');
        if (last_slash && last_slash != dir_path) {
            *last_slash = '\0';
            struct stat st;
            if (stat(dir_path, &st) != 0) {
                /* 目录不存在，尝试创建 */
                mkdir(dir_path, 0755);
            }
        }
        
        /* 写入文件 */
        FILE *f = fopen(path, "w");
        if (f) {
            size_t written = fwrite(output, 1, output_len, f);
            fclose(f);
            if (written == output_len) {
                cJSON_AddStringToObject(data, "saved_path", path);
                TS_LOGI(TAG, "Config pack saved to: %s", path);
            } else {
                TS_LOGW(TAG, "Partial write to %s (%zu/%zu)", path, written, output_len);
            }
        } else {
            TS_LOGW(TAG, "Failed to save config pack to: %s", path);
        }
    }
    
    free(output);
    ts_api_result_ok(result, data);
    TS_LOGI(TAG, "Config pack exported: %s (%zu bytes)", name->valuestring, output_len);
    return ESP_OK;
}

/**
 * @brief config.pack.list - List .tscfg files in a directory
 * 
 * Parameters:
 * - path: Directory path (default: /sdcard/config)
 */
static esp_err_t api_config_pack_list(const cJSON *params, ts_api_result_t *result)
{
    const char *dir_path = "/sdcard/config";
    
    if (params) {
        const cJSON *path = cJSON_GetObjectItem(params, "path");
        if (path && cJSON_IsString(path)) {
            dir_path = path->valuestring;
        }
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    
    if (!data || !files) {
        cJSON_Delete(data);
        cJSON_Delete(files);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    /* 扫描目录 */
    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            /* 跳过 . 和 .. */
            if (entry->d_name[0] == '.') continue;
            
            /* 检查是否为 .tscfg 文件 */
            size_t name_len = strlen(entry->d_name);
            size_t ext_len = strlen(TS_CONFIG_PACK_EXT);
            
            if (name_len > ext_len && 
                strcmp(entry->d_name + name_len - ext_len, TS_CONFIG_PACK_EXT) == 0) {
                
                cJSON *file_info = cJSON_CreateObject();
                cJSON_AddStringToObject(file_info, "name", entry->d_name);
                
                /* 获取文件大小 - 使用更大的缓冲区避免截断 */
                char full_path[512];
                int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
                if (path_len < 0 || (size_t)path_len >= sizeof(full_path)) {
                    ESP_LOGW(TAG, "Path too long, skipping: %s/%s", dir_path, entry->d_name);
                    cJSON_Delete(file_info);
                    continue;
                }
                
                struct stat st;
                if (stat(full_path, &st) == 0) {
                    cJSON_AddNumberToObject(file_info, "size", (double)st.st_size);
                    cJSON_AddNumberToObject(file_info, "mtime", (double)st.st_mtime);
                }
                
                /* 尝试验证获取签名信息 */
                ts_config_pack_sig_info_t sig_info = {0};
                ts_config_pack_result_t verify_result = ts_config_pack_verify(full_path, &sig_info);
                
                cJSON_AddBoolToObject(file_info, "valid", verify_result == TS_CONFIG_PACK_OK);
                if (sig_info.signer_cn[0] != '\0') {
                    cJSON_AddStringToObject(file_info, "signer", sig_info.signer_cn);
                    cJSON_AddBoolToObject(file_info, "is_official", sig_info.is_official);
                }
                
                cJSON_AddItemToArray(files, file_info);
            }
        }
        closedir(dir);
    }
    
    cJSON_AddStringToObject(data, "path", dir_path);
    cJSON_AddItemToObject(data, "files", files);
    cJSON_AddNumberToObject(data, "count", cJSON_GetArraySize(files));
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief config.pack.content - Get decrypted content of an imported config
 * 
 * Parameters:
 * - name: Config pack name (without .tscfg extension)
 * 
 * Returns decrypted JSON content. Only works for configs imported to this device.
 */
static esp_err_t api_config_pack_content(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!name || !cJSON_IsString(name)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'name' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 获取解密内容 */
    char *content = NULL;
    size_t content_len = 0;
    
    ts_config_pack_result_t pack_result = ts_config_pack_get_content(
        name->valuestring,
        &content,
        &content_len
    );
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        pack_failure(result, pack_result);
        return ESP_FAIL;
    }
    
    /* 构建响应 */
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        free(content);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(data, "name", name->valuestring);
    
    /* 尝试解析为 JSON 对象 */
    cJSON *content_json = cJSON_Parse(content);
    if (content_json) {
        cJSON_AddItemToObject(data, "content", content_json);
    } else {
        /* 如果不是有效 JSON，作为字符串返回 */
        cJSON_AddStringToObject(data, "content_raw", content);
    }
    
    free(content);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_api_config_pack_register(void)
{
    static const ts_api_endpoint_t pack_apis[] = {
        {
            .name = "config.pack.info",
            .description = "Get config pack system information",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_pack_info,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "config.pack.export_cert",
            .description = "Export device certificate for encryption",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_pack_export_cert,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "config.pack.verify",
            .description = "Verify a .tscfg package signature",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_pack_verify,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "config.pack.import",
            .description = "Validate an uploaded .tscfg file in place",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_pack_import,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "config.pack.apply",
            .description = "Apply configuration from a validated .tscfg file",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_pack_apply,
            .requires_auth = true,
            .permission = "config.write"
        },
        {
            .name = "config.pack.export",
            .description = "Export configuration as encrypted .tscfg",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_pack_export,
            .requires_auth = true,
            .permission = "config.admin"
        },
        {
            .name = "config.pack.list",
            .description = "List .tscfg files in directory",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_pack_list,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "config.pack.content",
            .description = "Decrypt and get content of imported config pack",
            .category = TS_API_CAT_CONFIG,
            .handler = api_config_pack_content,
            .requires_auth = true,
            .permission = "config.read"
        }
    };
    
    /* 初始化 config pack 子系统 */
    esp_err_t init_ret = ts_config_pack_init();
    if (init_ret != ESP_OK) {
        ESP_LOGW(TAG, "Config pack init failed: %s (may not have certificate)", 
                 esp_err_to_name(init_ret));
        /* 继续注册 API，即使没有证书也可以查看信息 */
    }
    
    esp_err_t ret = ts_api_register_multiple(pack_apis,
                                              sizeof(pack_apis) / sizeof(pack_apis[0]));
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Config Pack API registered (%d endpoints)",
                (int)(sizeof(pack_apis) / sizeof(pack_apis[0])));
    }
    return ret;
}
