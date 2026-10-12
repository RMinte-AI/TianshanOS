/**
 * @file ts_webui_api.c
 * @brief WebUI REST API Implementation
 */

#include "ts_webui.h"
#include "ts_http_server.h"
#include "ts_api.h"
#include "ts_security.h"
#include "ts_storage.h"
#include "ts_log.h"
#include "ts_ota.h"
#include "ts_config_pack.h"
#include "ts_ws_subscriptions.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"

/* PSRAM 优先分配宏 */
#define TS_API_MALLOC(size) ({ void *p = heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); p ? p : malloc(size); })

#define TAG "webui_api"

#define OTA_UPLOAD_REBOOT_DELAY_MS 2000

#ifdef CONFIG_TS_WEBUI_API_PREFIX
#define API_PREFIX CONFIG_TS_WEBUI_API_PREFIX
#else
#define API_PREFIX "/api/v1"
#endif

static bool is_binary_upload_complete(ts_http_request_t *req)
{
    return req && req->req && req->body_len == (size_t)req->req->content_len;
}

static void delayed_reboot_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

static void schedule_reboot_after_response(uint32_t delay_ms)
{
    BaseType_t ret = xTaskCreate(
        delayed_reboot_task,
        "ota_reboot",
        2048,
        (void *)(uintptr_t)delay_ms,
        1,
        NULL
    );
    if (ret != pdPASS) {
        TS_LOGE(TAG, "Failed to create OTA reboot task");
    }
}

static void clear_recovery_manifest(const char *reason)
{
    if (unlink(TS_OTA_RECOVERY_MANIFEST_PATH) == 0) {
        TS_LOGI(TAG, "Removed recovery manifest: %s", reason);
    }
}

static void clean_recovery_after_browser_ota_failure(const char *reason)
{
    TS_LOGW(TAG, "Cleaning recovery cache after browser OTA failure: %s", reason);
    esp_err_t ret = ts_ota_clean_recovery();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Recovery cleanup failed: %s", esp_err_to_name(ret));
    }
}

static bool is_browser_ota_busy(void)
{
    return ts_ota_sdcard_is_running()
        || ts_ota_https_is_running()
        || ts_ota_download_is_running()
        || ts_ota_www_is_running();
}

/**
 * @brief Check authentication for API requests
 * @note Reserved for WebUI authentication feature
 */
__attribute__((unused))
static esp_err_t check_auth(ts_http_request_t *req, uint32_t *session_id, ts_perm_level_t required)
{
#ifdef CONFIG_TS_WEBUI_AUTH_REQUIRED
    char auth[128];
    if (ts_http_get_header(req, "Authorization", auth, sizeof(auth)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    
    ts_perm_level_t level;
    extern esp_err_t ts_auth_validate_request(const char *, uint32_t *, ts_perm_level_t *);
    esp_err_t ret = ts_auth_validate_request(auth, session_id, &level);
    if (ret != ESP_OK) return ret;
    
    if (level < required) return ESP_ERR_NOT_ALLOWED;
    return ESP_OK;
#else
    (void)req;
    (void)session_id;
    (void)required;
    return ESP_OK;
#endif
}

/* This endpoint retains storage failure details; other response contracts stay unchanged. */
static esp_err_t send_key_generate_result(ts_http_request_t *req, ts_api_result_t *result,
                                          const char *request_id, int64_t started)
{
    cJSON *response = cJSON_CreateObject();
    bool built = response && cJSON_AddNumberToObject(response, "code", result->code);
    if (built && result->message) {
        built = cJSON_AddStringToObject(response, result->code == TS_API_OK ? "message" : "error", result->message) != NULL;
    }
    if (built && result->data) {
        cJSON *data = cJSON_Duplicate(result->data, true);
        if (!data) built = false;
        else if (!cJSON_AddItemToObject(response, "data", data)) { cJSON_Delete(data); built = false; }
    }
    char *json = built ? cJSON_PrintUnformatted(response) : NULL;
    cJSON_Delete(response);
    ts_api_result_free(result);
    TS_LOGI(TAG, "key.generate request=%s response=%s elapsed_ms=%lld", request_id,
            json ? "built" : "allocation_failed", (long long)((esp_timer_get_time() - started) / 1000));
    esp_err_t ret = json ? ts_http_send_json(req, 200, json)
                        : ts_http_send_error(req, 500, "Key response serialization failed");
    free(json);
    TS_LOGI(TAG, "key.generate request=%s send=%s elapsed_ms=%lld", request_id,
            esp_err_to_name(ret), (long long)((esp_timer_get_time() - started) / 1000));
    return ret;
}

static esp_err_t api_handler(ts_http_request_t *req, void *user_data)
{
    (void)user_data;
    
#ifdef CONFIG_TS_WEBUI_CORS_ENABLE
    ts_http_set_cors(req, "*");
#endif
    
    // Extract endpoint from URI
    const char *uri = req->uri;
    const char *endpoint = uri + strlen(API_PREFIX) + 1;
    
    // Convert slashes to dots for API lookup
    char api_name[64];
    strncpy(api_name, endpoint, sizeof(api_name) - 1);
    api_name[sizeof(api_name) - 1] = '\0';
    
    // Remove query string if present
    char *query = strchr(api_name, '?');
    if (query) {
        *query = '\0';
    }
    
    // Convert slashes to dots
    for (char *p = api_name; *p; p++) {
        if (*p == '/') *p = '.';
    }
    
    // API 请求日志使用 DEBUG 级别，避免干扰调试
    if (strncmp(api_name, "log.", 4) != 0) {
        TS_LOGD(TAG, "API request: method=%d uri=%s -> api_name=%s", 
                req->method, uri, api_name);
    }
    
    // TODO: 测试阶段暂时禁用认证检查
    // 功能测试完成后需要恢复以下代码：
    // Check authentication for write operations only
    // GET requests (read-only) are allowed without authentication
    // uint32_t session_id = 0;
    // if (req->method == TS_HTTP_POST || req->method == TS_HTTP_PUT || 
    //     req->method == TS_HTTP_DELETE) {
    //     if (check_auth(req, &session_id, TS_PERM_WRITE) != ESP_OK) {
    //         return ts_http_send_error(req, 401, "Unauthorized");
    //     }
    // }
    
    bool key_generation = strcmp(api_name, "key.generate") == 0;
    bool rule_import = strcmp(api_name,"automation.rules.import")==0;
    if(rule_import&&(!is_binary_upload_complete(req)||!req->body||httpd_req_get_url_query_len(req->req)>0))
        return ts_http_send_error(req,400,"invalid_pack");
    int64_t key_started = key_generation ? esp_timer_get_time() : 0;
    char key_request_id[32] = "legacy";
    if (key_generation) TS_LOGI(TAG, "key.generate received");
    // Build request JSON
    cJSON *request = cJSON_CreateObject();
    if (key_generation && !request) {
        TS_LOGE(TAG, "key.generate request=legacy request allocation failed");
        esp_err_t send_ret = ts_http_send_error(req, 500, "Key request allocation failed");
        TS_LOGI(TAG, "key.generate request=legacy send=%s", esp_err_to_name(send_ret));
        return send_ret;
    }
    
    // Parse URL query string parameters (for GET requests)
    size_t query_len = httpd_req_get_url_query_len(req->req);
    if (query_len > 0) {
        char *query_buf = TS_API_MALLOC(query_len + 1);
        if (query_buf) {
            if (httpd_req_get_url_query_str(req->req, query_buf, query_len + 1) == ESP_OK) {
                // Parse query string parameters
                char *saveptr = NULL;
                char *pair = strtok_r(query_buf, "&", &saveptr);
                while (pair) {
                    char *eq = strchr(pair, '=');
                    if (eq) {
                        *eq = '\0';
                        char *key = pair;
                        char *value = eq + 1;
                        
                        // URL decode the value
                        char *src = value;
                        char *dst = value;
                        while (*src) {
                            if (*src == '%' && src[1] && src[2]) {
                                int high = src[1];
                                int low = src[2];
                                // Convert hex
                                if (high >= '0' && high <= '9') high -= '0';
                                else if (high >= 'A' && high <= 'F') high = high - 'A' + 10;
                                else if (high >= 'a' && high <= 'f') high = high - 'a' + 10;
                                else { *dst++ = *src++; continue; }
                                
                                if (low >= '0' && low <= '9') low -= '0';
                                else if (low >= 'A' && low <= 'F') low = low - 'A' + 10;
                                else if (low >= 'a' && low <= 'f') low = low - 'a' + 10;
                                else { *dst++ = *src++; continue; }
                                
                                *dst++ = (char)((high << 4) | low);
                                src += 3;
                            } else if (*src == '+') {
                                *dst++ = ' ';
                                src++;
                            } else {
                                *dst++ = *src++;
                            }
                        }
                        *dst = '\0';
                        
                        // Try to parse as number first
                        char *endptr;
                        long num = strtol(value, &endptr, 10);
                        if (*endptr == '\0' && value[0] != '\0') {
                            // It's a number
                            cJSON_AddNumberToObject(request, key, num);
                        } else {
                            // It's a string
                            cJSON_AddStringToObject(request, key, value);
                        }
                    }
                    pair = strtok_r(NULL, "&", &saveptr);
                }
            }
            free(query_buf);
        }
    }
    
    // Add body data if present (POST/PUT requests - overrides query params)
    if (req->body && req->body_len > 0) {
        cJSON *body = rule_import ? ts_config_pack_parse_import_request(req->body,req->body_len) : cJSON_Parse(req->body);
        if(rule_import&&!cJSON_IsObject(body)){
            cJSON_Delete(body);cJSON_Delete(request);
            return ts_http_send_error(req,400,"invalid_pack");
        }
        if (body) {
            cJSON *item;
            cJSON_ArrayForEach(item, body) {
                cJSON_AddItemToObject(request, item->string, cJSON_Duplicate(item, true));
            }
            cJSON_Delete(body);
        }
    }
    
    if (key_generation) {
        const cJSON *rid = cJSON_GetObjectItem(request, "request_id");
        if (cJSON_IsString(rid) && strlen(rid->valuestring) < sizeof(key_request_id)) {
            bool valid = rid->valuestring[0] != '\0';
            for (const char *p = rid->valuestring; *p; ++p) {
                if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '-')) valid = false;
            }
            if (valid) strcpy(key_request_id, rid->valuestring);
        }
        TS_LOGI(TAG, "key.generate request=%s arrived", key_request_id);
    }
    // Call API
    ts_api_result_t result = {0};
    esp_err_t ret = ts_api_call(api_name, request, &result);
    
    // API 结果日志使用 DEBUG 级别
    if (strncmp(api_name, "log.", 4) != 0) {
        TS_LOGD(TAG, "API call result: api=%s ret=%d code=%d msg=%s", 
                api_name, ret, result.code, result.message ? result.message : "null");
    }
    
    if (key_generation) {
        const cJSON *generated = cJSON_GetObjectItem(result.data, "generated");
        TS_LOGI(TAG, "key.generate request=%s returned ret=%s code=%d generated_result=%d elapsed_ms=%lld",
                key_request_id, esp_err_to_name(ret), result.code, cJSON_IsTrue(generated),
                (long long)((esp_timer_get_time() - key_started) / 1000));
    }
    cJSON_Delete(request);
    if (key_generation) return send_key_generate_result(req, &result, key_request_id, key_started);

    if (ret == ESP_OK || result.code == TS_API_OK) {
        // Build response JSON
        cJSON *response = cJSON_CreateObject();
        cJSON_AddNumberToObject(response, "code", result.code);
        if (result.message) {
            cJSON_AddStringToObject(response, "message", result.message);
        }
        if (result.data) {
            cJSON_AddItemToObject(response, "data", cJSON_Duplicate(result.data, true));
        }
        
        char *json = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
        
        // Free result resources
        ts_api_result_free(&result);
        
        if (json) {
            esp_err_t send_ret = ts_http_send_json(req, 200, json);
            free(json);
            return send_ret;
        }
        return ts_http_send_error(req, 500, "JSON serialization failed");
    } else if (ret == ESP_ERR_NOT_FOUND && result.code == TS_API_ERR_NOT_FOUND && 
               result.message && strcmp(result.message, "API not found") == 0) {
        // Only report "API not found" if it's actually a missing API endpoint
        ts_api_result_free(&result);
        return ts_http_send_error(req, 404, "API not found");
    } else {
        // Return the actual error from the API handler with HTTP 200
        // API errors (like "no data") are business logic errors, not HTTP errors
        cJSON *response = cJSON_CreateObject();
        cJSON_AddNumberToObject(response, "code", result.code);
        cJSON_AddStringToObject(response, "error", result.message ? result.message : "Internal error");
        
        char *json = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
        ts_api_result_free(&result);
        
        if (json) {
            // Always return HTTP 200 for API business errors
            // The error is indicated by the "code" field in the JSON response
            esp_err_t send_ret = ts_http_send_json(req, 200, json);
            free(json);
            return send_ret;
        }
        return ts_http_send_error(req, 500, "Internal error");
    }
}

static esp_err_t login_handler(ts_http_request_t *req, void *user_data)
{
    (void)user_data;
    
#ifdef CONFIG_TS_WEBUI_CORS_ENABLE
    ts_http_set_cors(req, "*");
#endif
    
    if (!req->body) {
        return ts_http_send_error(req, 400, "Missing body");
    }
    
    cJSON *body = cJSON_Parse(req->body);
    if (!body) {
        return ts_http_send_error(req, 400, "Invalid JSON");
    }
    
    cJSON *username = cJSON_GetObjectItem(body, "username");
    cJSON *password = cJSON_GetObjectItem(body, "password");
    
    if (!username || !password || !cJSON_IsString(username) || !cJSON_IsString(password)) {
        cJSON_Delete(body);
        return ts_http_send_error(req, 400, "Missing username or password");
    }
    
    /* 保存用户名副本，因为后续要删除 body */
    char username_copy[32];
    strncpy(username_copy, username->valuestring, sizeof(username_copy) - 1);
    username_copy[sizeof(username_copy) - 1] = '\0';
    
    uint32_t session_id;
    char token[128];
    esp_err_t ret = ts_auth_login(username->valuestring, password->valuestring, &session_id, token, sizeof(token));
    cJSON_Delete(body);
    
    if (ret != ESP_OK) {
        return ts_http_send_error(req, 401, "Invalid credentials");
    }
    
    /* 获取用户权限等级 */
    ts_perm_level_t level = TS_PERM_ADMIN;  /* 默认 */
    ts_session_t session;
    if (ts_security_validate_session(session_id, &session) == ESP_OK) {
        level = session.level;
    }
    
    /* 将权限级别转换为字符串 */
    const char *level_str = (level == TS_PERM_ROOT) ? "root" : "admin";
    
    /* 检查是否需要修改密码 */
    bool password_changed = ts_auth_password_changed(username_copy);
    
    /* 构建符合前端期望的响应格式: {code: 0, data: {...}} */
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "code", 0);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "token", token);
    cJSON_AddNumberToObject(data, "session_id", session_id);
    cJSON_AddStringToObject(data, "username", username_copy);
    cJSON_AddStringToObject(data, "level", level_str);
#ifdef CONFIG_TS_SECURITY_TOKEN_EXPIRE_SEC
    cJSON_AddNumberToObject(data, "expires_in", CONFIG_TS_SECURITY_TOKEN_EXPIRE_SEC);
#else
    cJSON_AddNumberToObject(data, "expires_in", 86400);  /* 24 hours */
#endif
    cJSON_AddBoolToObject(data, "password_changed", password_changed);
    cJSON_AddItemToObject(response, "data", data);
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    esp_err_t send_ret = ts_http_send_json(req, 200, json);
    free(json);
    return send_ret;
}

static esp_err_t logout_handler(ts_http_request_t *req, void *user_data)
{
    (void)user_data;
    
#ifdef CONFIG_TS_WEBUI_CORS_ENABLE
    ts_http_set_cors(req, "*");
#endif
    
    char auth[128];
    if (ts_http_get_header(req, "Authorization", auth, sizeof(auth)) == ESP_OK) {
        uint32_t session_id;
        ts_perm_level_t level;
        extern esp_err_t ts_auth_validate_request(const char *, uint32_t *, ts_perm_level_t *);
        if (ts_auth_validate_request(auth, &session_id, &level) == ESP_OK) {
            extern esp_err_t ts_auth_logout(uint32_t);
            ts_auth_logout(session_id);
        }
    }
    
    return ts_http_send_json(req, 200, "{\"success\":true}");
}

static esp_err_t options_handler(ts_http_request_t *req, void *user_data)
{
    (void)user_data;
    
#ifdef CONFIG_TS_WEBUI_CORS_ENABLE
    ts_http_set_cors(req, "*");
#endif
    return ts_http_send_response(req, 204, NULL, NULL);
}

/*===========================================================================*/
/*                        File Upload/Download Handlers                       */
/*===========================================================================*/

/**
 * @brief URL decode a string in place
 */
static void url_decode(char *str)
{
    char *src = str;
    char *dst = str;
    
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int high = src[1];
            int low = src[2];
            
            // Convert hex chars to int
            if (high >= '0' && high <= '9') high -= '0';
            else if (high >= 'A' && high <= 'F') high = high - 'A' + 10;
            else if (high >= 'a' && high <= 'f') high = high - 'a' + 10;
            else { *dst++ = *src++; continue; }
            
            if (low >= '0' && low <= '9') low -= '0';
            else if (low >= 'A' && low <= 'F') low = low - 'A' + 10;
            else if (low >= 'a' && low <= 'f') low = low - 'a' + 10;
            else { *dst++ = *src++; continue; }
            
            *dst++ = (char)((high << 4) | low);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/**
 * @brief Handle file download request
 * GET /api/v1/file/download?path=/sdcard/xxx
 */
static esp_err_t file_download_handler(ts_http_request_t *req, void *user_data)
{
    (void)user_data;
    
#ifdef CONFIG_TS_WEBUI_CORS_ENABLE
    ts_http_set_cors(req, "*");
#endif

    // 获取文件路径参数
    char path[256] = {0};
    esp_err_t ret = ts_http_get_query_param(req, "path", path, sizeof(path));
    if (ret != ESP_OK || strlen(path) == 0) {
        return ts_http_send_error(req, 400, "Missing 'path' parameter");
    }
    
    // URL decode path
    url_decode(path);
    TS_LOGD(TAG, "Download request: %s", path);
    
    // 安全检查：只允许访问 /sdcard 和 /spiffs
    if (strncmp(path, "/sdcard", 7) != 0 && strncmp(path, "/spiffs", 7) != 0) {
        return ts_http_send_error(req, 403, "Access denied: invalid path");
    }
    
    // 检查文件是否存在
    if (!ts_storage_exists(path)) {
        return ts_http_send_error(req, 404, "File not found");
    }
    
    // 检查是否是目录
    if (ts_storage_is_dir(path)) {
        return ts_http_send_error(req, 400, "Cannot download directory");
    }
    
    // 发送文件
    return ts_http_send_file(req, path);
}

/**
 * @brief Handle file upload request
 * POST /api/v1/file/upload?path=/sdcard/xxx
 * Body: file content
 */
static esp_err_t file_upload_handler(ts_http_request_t *req, void *user_data)
{
    (void)user_data;
    
#ifdef CONFIG_TS_WEBUI_CORS_ENABLE
    ts_http_set_cors(req, "*");
#endif

    // 获取目标路径参数
    char path[256] = {0};
    esp_err_t ret = ts_http_get_query_param(req, "path", path, sizeof(path));
    if (ret != ESP_OK || strlen(path) == 0) {
        return ts_http_send_error(req, 400, "Missing 'path' parameter");
    }
    
    // URL 解码路径
    url_decode(path);
    TS_LOGD(TAG, "Upload request: path=%s, body_len=%d", path, (int)(req->body_len));
    
    // 安全检查：只允许写入 /sdcard
    if (strncmp(path, "/sdcard", 7) != 0) {
        return ts_http_send_error(req, 403, "Upload only allowed to /sdcard");
    }
    
    // 检查请求体
    if (!req->body || req->body_len == 0) {
        return ts_http_send_error(req, 400, "Empty file content");
    }
    
    // 确保目标目录存在
    char dir_path[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash && last_slash != dir_path) {
        *last_slash = '\0';
        ts_storage_mkdir_p(dir_path);
    }
    
    // 写入文件
    ret = ts_storage_write_file(path, req->body, req->body_len);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to write file %s: %s", path, esp_err_to_name(ret));
        return ts_http_send_error(req, 500, "Failed to write file");
    }
    
    TS_LOGD(TAG, "File uploaded: %s (%zu bytes)", path, req->body_len);
    
    // 返回成功响应
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "path", path);
    cJSON_AddNumberToObject(response, "size", req->body_len);
    cJSON_AddStringToObject(response, "status", "uploaded");
    
    // === 检测 .tscfg 配置包并自动验证 ===
    const char *ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, TS_CONFIG_PACK_EXT) == 0) {
        TS_LOGI(TAG, "Detected config pack upload: %s", path);
        
        // 验证配置包
        ts_config_pack_sig_info_t sig_info = {0};
        ts_config_pack_result_t pack_result = ts_config_pack_verify(path, &sig_info);
        
        // 添加验证结果到响应
        cJSON *validation = cJSON_CreateObject();
        cJSON_AddBoolToObject(validation, "valid", pack_result == TS_CONFIG_PACK_OK);
        cJSON_AddNumberToObject(validation, "result_code", pack_result);
        cJSON_AddStringToObject(validation, "result_message", ts_config_pack_strerror(pack_result));
        
        if (pack_result == TS_CONFIG_PACK_OK || sig_info.signer_cn[0] != '\0') {
            cJSON *sig = cJSON_CreateObject();
            cJSON_AddBoolToObject(sig, "valid", sig_info.valid);
            cJSON_AddBoolToObject(sig, "is_official", sig_info.is_official);
            cJSON_AddStringToObject(sig, "signer_cn", sig_info.signer_cn);
            cJSON_AddStringToObject(sig, "signer_ou", sig_info.signer_ou);
            cJSON_AddNumberToObject(sig, "signed_at", (double)sig_info.signed_at);
            cJSON_AddItemToObject(validation, "signature", sig);
        }
        
        cJSON_AddItemToObject(response, "config_pack", validation);
        
        // 通过 WebSocket 广播验证结果
        cJSON *ws_data = cJSON_CreateObject();
        cJSON_AddStringToObject(ws_data, "path", path);
        cJSON_AddStringToObject(ws_data, "status", 
                               pack_result == TS_CONFIG_PACK_OK ? "success" : "error");
        cJSON_AddNumberToObject(ws_data, "result_code", pack_result);
        cJSON_AddStringToObject(ws_data, "result_message", ts_config_pack_strerror(pack_result));
        
        if (pack_result == TS_CONFIG_PACK_OK || sig_info.signer_cn[0] != '\0') {
            cJSON *ws_sig = cJSON_CreateObject();
            cJSON_AddBoolToObject(ws_sig, "valid", sig_info.valid);
            cJSON_AddBoolToObject(ws_sig, "is_official", sig_info.is_official);
            cJSON_AddStringToObject(ws_sig, "signer_cn", sig_info.signer_cn);
            cJSON_AddItemToObject(ws_data, "signature", ws_sig);
        }
        
        ts_ws_broadcast_to_topic("config.pack.validated", ws_data);
        cJSON_Delete(ws_data);
    }
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    ret = ts_http_send_json(req, 200, json);
    free(json);
    return ret;
}

/**
 * @brief Handle OTA firmware upload request (Browser Proxy Upgrade)
 * POST /api/v1/ota/firmware
 * Body: firmware binary content
 * Query: auto_reboot (optional, default true)
 * 
 * This allows browsers to upload firmware directly to ESP32 devices
 * that cannot access external networks.
 * 
 * 使用统一 recovery 目录模式：保存到 /sdcard/recovery/ 然后刷入
 */
static esp_err_t ota_firmware_upload_handler(ts_http_request_t *req, void *user_data)
{
    (void)user_data;
    
#ifdef CONFIG_TS_WEBUI_CORS_ENABLE
    ts_http_set_cors(req, "*");
#endif

    if (!is_binary_upload_complete(req)) {
        TS_LOGW(TAG, "Incomplete firmware upload: got %zu of %zu bytes",
                req->body_len, req->req ? (size_t)req->req->content_len : 0);
        return ts_http_send_error(req, 400, "Incomplete upload");
    }

    // 检查请求体
    if (!req->body || req->body_len == 0) {
        return ts_http_send_error(req, 400, "Empty firmware content");
    }

    if (is_browser_ota_busy()) {
        return ts_http_send_error(req, 409, "OTA already in progress");
    }
    
    // 获取 auto_reboot 参数
    char auto_reboot_str[8] = "true";
    ts_http_get_query_param(req, "auto_reboot", auto_reboot_str, sizeof(auto_reboot_str));
    bool auto_reboot = (strcmp(auto_reboot_str, "false") != 0);
    
    TS_LOGI(TAG, "OTA firmware upload: %zu bytes, auto_reboot=%d", req->body_len, auto_reboot);
    
    // 新固件上传开始时先移除旧 manifest 和旧 WebUI，避免失败路径留下旧恢复指令。
    clear_recovery_manifest("firmware upload");
    if (unlink(TS_OTA_RECOVERY_WWW_PATH) == 0) {
        TS_LOGI(TAG, "Removed stale recovery www.bin");
    }

    // 先保存到 recovery 目录，保持恢复素材；刷写成功后再写 disabled manifest。
    esp_err_t ret = ts_ota_save_upload(req->body, req->body_len, true, false);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "ts_ota_save_upload failed: %s", esp_err_to_name(ret));
        clean_recovery_after_browser_ota_failure("firmware save failed");
        return ts_http_send_error(req, 500, "Firmware save failed");
    }

    ret = ts_ota_flash_file_sync(TS_OTA_RECOVERY_FIRMWARE_PATH, true, NULL, NULL);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "ts_ota_flash_file_sync failed: %s", esp_err_to_name(ret));
        clean_recovery_after_browser_ota_failure("firmware flash failed");
        if (ret == ESP_ERR_INVALID_STATE) {
            return ts_http_send_error(req, 409, "OTA already in progress");
        }
        if (ret == ESP_ERR_INVALID_SIZE || ret == ESP_ERR_INVALID_ARG) {
            return ts_http_send_error(req, 400, "Invalid firmware image");
        }
        return ts_http_send_error(req, 500, "Firmware flash failed");
    }

    ret = ts_ota_write_recovery_manifest_from_files(false, false, false);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "failed to write disabled app-only recovery manifest: %s", esp_err_to_name(ret));
        clean_recovery_after_browser_ota_failure("firmware manifest update failed");
        return ts_http_send_error(req, 500, "Recovery manifest update failed");
    }

    TS_LOGI(TAG, "OTA firmware upload successful");
    
    // 返回成功响应
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddNumberToObject(response, "size", req->body_len);
    cJSON_AddBoolToObject(response, "reboot_pending", true);
    cJSON_AddBoolToObject(response, "auto_reboot_requested", auto_reboot);
    cJSON_AddStringToObject(response, "message", "Firmware uploaded and flashed, pending reboot");
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    ret = ts_http_send_json(req, 200, json);
    free(json);
    if (ret == ESP_OK && auto_reboot) {
        schedule_reboot_after_response(OTA_UPLOAD_REBOOT_DELAY_MS);
    }
    return ret;
}

/**
 * @brief Handle WWW partition upload request
 * POST /api/v1/ota/www
 * Body: www.bin content
 * 
 * 使用统一 recovery 目录模式：保存到 /sdcard/recovery/www.bin 然后刷入
 */
static esp_err_t ota_www_upload_handler(ts_http_request_t *req, void *user_data)
{
    (void)user_data;
    
#ifdef CONFIG_TS_WEBUI_CORS_ENABLE
    ts_http_set_cors(req, "*");
#endif

    if (!is_binary_upload_complete(req)) {
        TS_LOGW(TAG, "Incomplete WWW upload: got %zu of %zu bytes",
                req->body_len, req->req ? (size_t)req->req->content_len : 0);
        return ts_http_send_error(req, 400, "Incomplete upload");
    }

    // 检查请求体
    if (!req->body || req->body_len == 0) {
        return ts_http_send_error(req, 400, "Empty www content");
    }

    if (is_browser_ota_busy()) {
        return ts_http_send_error(req, 409, "OTA already in progress");
    }
    
    TS_LOGI(TAG, "WWW partition upload: %zu bytes", req->body_len);
    
    // WebUI 上传会重写 disabled manifest；先清掉旧 manifest 避免失败路径沿用旧恢复指令。
    clear_recovery_manifest("www upload");

    // 先保存到 recovery 目录，保持完整恢复包；再同步刷入 www 分区。
    esp_err_t ret = ts_ota_save_upload(req->body, req->body_len, false, false);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "ts_ota_save_upload failed: %s", esp_err_to_name(ret));
        clean_recovery_after_browser_ota_failure("www save failed");
        return ts_http_send_error(req, 500, "WWW partition save failed");
    }

    ret = ts_ota_www_flash_file_sync(TS_OTA_RECOVERY_WWW_PATH, NULL, NULL);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "ts_ota_www_flash_file_sync failed: %s", esp_err_to_name(ret));
        clean_recovery_after_browser_ota_failure("www flash failed");
        if (ret == ESP_ERR_INVALID_STATE) {
            return ts_http_send_error(req, 409, "WWW OTA already in progress");
        }
        if (ret == ESP_ERR_INVALID_SIZE) {
            return ts_http_send_error(req, 400, "Invalid WWW image size");
        }
        return ts_http_send_error(req, 500, "WWW partition flash failed");
    }

    if (ts_storage_exists(TS_OTA_RECOVERY_FIRMWARE_PATH)) {
        ret = ts_ota_write_recovery_manifest_from_files(true, false, false);
        if (ret != ESP_OK) {
            TS_LOGW(TAG, "Full recovery manifest update failed: %s", esp_err_to_name(ret));
            clean_recovery_after_browser_ota_failure("www manifest update failed");
        }
    } else {
        TS_LOGW(TAG, "Recovery firmware missing after WebUI upload");
        clean_recovery_after_browser_ota_failure("recovery firmware missing after www upload");
    }
    
    TS_LOGI(TAG, "WWW partition upload successful");
    
    // 返回成功响应
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddNumberToObject(response, "size", req->body_len);
    cJSON_AddStringToObject(response, "message", "WWW partition updated");
    
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    ret = ts_http_send_json(req, 200, json);
    free(json);
    return ret;
}

esp_err_t ts_webui_api_init(void)
{
    TS_LOGI(TAG, "Initializing API routes");
    
    // Auth routes
    ts_http_route_t login = {
        .uri = API_PREFIX "/auth/login",
        .method = TS_HTTP_POST,
        .handler = login_handler,
        .requires_auth = false,
        .user_data = NULL
    };
    ts_http_server_register_route(&login);
    
    ts_http_route_t logout = {
        .uri = API_PREFIX "/auth/logout",
        .method = TS_HTTP_POST,
        .handler = logout_handler,
        .requires_auth = true,
        .user_data = NULL
    };
    ts_http_server_register_route(&logout);
    
    // File download route (must be registered BEFORE generic handler)
    ts_http_route_t file_download = {
        .uri = API_PREFIX "/file/download",
        .method = TS_HTTP_GET,
        .handler = file_download_handler,
        .requires_auth = false,
        .user_data = NULL
    };
    ts_http_server_register_route(&file_download);
    
    // File upload route
    ts_http_route_t file_upload = {
        .uri = API_PREFIX "/file/upload",
        .method = TS_HTTP_POST,
        .handler = file_upload_handler,
        .requires_auth = false,
        .user_data = NULL
    };
    ts_http_server_register_route(&file_upload);
    
    // OTA firmware upload route (Browser Proxy Upgrade)
    ts_http_route_t ota_firmware = {
        .uri = API_PREFIX "/ota/firmware",
        .method = TS_HTTP_POST,
        .handler = ota_firmware_upload_handler,
        .requires_auth = false,  // TODO: 生产环境应设为 true
        .user_data = NULL
    };
    ts_http_server_register_route(&ota_firmware);
    
    // WWW partition upload route
    ts_http_route_t ota_www = {
        .uri = API_PREFIX "/ota/www",
        .method = TS_HTTP_POST,
        .handler = ota_www_upload_handler,
        .requires_auth = false,  // TODO: 生产环境应设为 true
        .user_data = NULL
    };
    ts_http_server_register_route(&ota_www);
    
    // Generic API handler for all other routes
    ts_http_method_t methods[] = {TS_HTTP_GET, TS_HTTP_POST, TS_HTTP_PUT, TS_HTTP_DELETE};
    const char *method_names[] = {"GET", "POST", "PUT", "DELETE"};
    for (int i = 0; i < 4; i++) {
        ts_http_route_t route = {
            .uri = API_PREFIX "/*",
            .method = methods[i],
            .handler = api_handler,
            .requires_auth = true,
            .user_data = NULL
        };
        esp_err_t ret = ts_http_server_register_route(&route);
        TS_LOGD(TAG, "Register API %s handler: %s", method_names[i], esp_err_to_name(ret));
    }
    
    // CORS preflight handler
    ts_http_route_t options = {
        .uri = API_PREFIX "/*",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .requires_auth = false,
        .user_data = NULL
    };
    ts_http_server_register_route(&options);
    
    TS_LOGI(TAG, "API routes registered");
    return ESP_OK;
}
