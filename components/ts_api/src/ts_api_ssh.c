#include "ts_api_auto_config.h"
/**
 * @file ts_api_ssh.c
 * @brief SSH API Handlers
 * 
 * 提供 SSH 相关操作的 API 端点：
 * - ssh.exec - 执行远程命令
 * - ssh.test - 测试连接
 * - ssh.copyid - 部署公钥
 * - ssh.revoke - 撤销公钥
 * - ssh.keygen - 生成密钥对
 * - ssh.hosts.* - SSH 主机凭证管理（持久化）
 * 
 * 所有 SSH 连接操作都包含主机指纹验证（Known Hosts），
 * 由 trust_new 和 accept_changed 参数控制行为。
 * 
 * 注意：交互式 Shell 不通过 API 实现
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-20
 */

#include "ts_api.h"
#include "ts_action_manager.h"
#include "ts_api_service.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM, TS_STRDUP_PSRAM */
#include "ts_ssh_client.h"
#include "ts_ssh_probe.h"
#include "ts_keystore.h"
#include "ts_known_hosts.h"
#include "ts_ssh_hosts_config.h"
#include "ts_ssh_config_codec.h"
#include "ts_ssh_commands_config.h"
#include "ts_config_pack.h"
#include "ts_api_pack.h"
#include "ts_cert.h"
#include "ts_webui.h"
#include "ts_log.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define TAG "api_ssh"

/* 自定义错误码用于指纹验证 */
#define TS_API_ERR_HOST_MISMATCH  1001  /* 主机指纹不匹配 */
#define TS_API_ERR_HOST_NEW       1002  /* 新主机需要确认 */

/* Request-local key lifetime; concurrent callers must never share credentials. */
typedef struct {
    char *data;
    size_t len;
} ssh_request_key_t;
static void cleanup_key_buffer(ssh_request_key_t *key) {
    if (key->data) {
        volatile unsigned char *p = (volatile unsigned char *)key->data;
        for (size_t i = 0; i < key->len; ++i)
            p[i] = 0;
        free(key->data);
        key->data = NULL;
        key->len = 0;
    }
}

/* Failure with the reason token as the error text and {stage, detail} as data (data is delivered with failures). */
static void ssh_connect_error(ts_api_result_t *result, ts_ssh_session_t session)
{
    const char *stage = "connect";
    const char *reason = ts_ssh_connect_reason(session, &stage);
    const char *detail = ts_ssh_get_error(session);
    ts_api_result_error(result, TS_API_ERR_CONNECTION, reason);
    cJSON *data = cJSON_CreateObject();
    if (data && cJSON_AddStringToObject(data, "stage", stage) &&
        (!detail || !detail[0] || cJSON_AddStringToObject(data, "detail", detail))) {
        result->data = data;
    } else {
        cJSON_Delete(data);
    }
}

/**
 * @brief 从参数中配置 SSH 会话
 */
static esp_err_t configure_ssh_from_params(const cJSON *params, ts_ssh_config_t *config,
                                           ssh_request_key_t *request_key) {
    /* 主机和用户名 */
    const cJSON *host = cJSON_GetObjectItem(params, "host");
    const cJSON *user = cJSON_GetObjectItem(params, "user");
    const cJSON *port = cJSON_GetObjectItem(params, "port");
    
    if (!host || !cJSON_IsString(host) || !user || !cJSON_IsString(user)) {
        return ESP_ERR_INVALID_ARG;
    }

    config->host = host->valuestring;
    config->username = user->valuestring;
    config->port = cJSON_IsNumber(port) ? port->valueint : 22;
    
    /* 认证方式：密码或密钥 */
    const cJSON *password = cJSON_GetObjectItem(params, "password");
    const cJSON *keyid = cJSON_GetObjectItem(params, "keyid");
    const cJSON *keypath = cJSON_GetObjectItem(params, "keypath");

    cleanup_key_buffer(request_key);

    if (cJSON_IsString(password) && password->valuestring[0]) {
        config->auth_method = TS_SSH_AUTH_PASSWORD;
        config->auth.password = password->valuestring;
    } else if (cJSON_IsString(keyid) && keyid->valuestring[0]) {
        /* 从密钥存储读取私钥 */
        esp_err_t ret =
            ts_keystore_load_private_key(keyid->valuestring, &request_key->data, &request_key->len);
        if (ret != ESP_OK) {
            TS_LOGE(TAG, "Failed to load key '%s': %s", keyid->valuestring, esp_err_to_name(ret));
            return ret;
        }
        config->auth_method = TS_SSH_AUTH_PUBLICKEY;
        config->auth.key.private_key = (const uint8_t *)request_key->data;
        config->auth.key.private_key_len = request_key->len;
        config->auth.key.private_key_path = NULL;
        config->auth.key.passphrase = NULL;
    } else if (cJSON_IsString(keypath) && keypath->valuestring[0]) {
        config->auth_method = TS_SSH_AUTH_PUBLICKEY;
        config->auth.key.private_key = NULL;
        config->auth.key.private_key_len = 0;
        config->auth.key.private_key_path = keypath->valuestring;
        config->auth.key.passphrase = NULL;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

/**
 * @brief 主机指纹验证辅助函数
 * 
 * 在 SSH 连接成功后验证主机指纹，根据参数决定行为：
 * - trust_new=true: 新主机自动信任并添加到 known hosts
 * - trust_new=false: 新主机返回错误，需要用户确认
 * - accept_changed=true: 指纹变化时接受新指纹
 * - accept_changed=false: 指纹变化时返回错误
 * 
 * @param session SSH 会话（已连接）
 * @param params API 参数（包含 trust_new, accept_changed）
 * @param result API 结果（用于返回错误信息）
 * @param host_info_out 输出主机信息（可选）
 * @param auto_trust_new 公钥部署沿用 0.5.1 的首次信任行为；显式 trust_new=false 仍生效
 * @return ESP_OK 验证通过，其他表示失败
 */
static esp_err_t verify_host_fingerprint(ts_ssh_session_t session,
                                          const cJSON *params,
                                          ts_api_result_t *result,
                                          ts_known_host_t *host_info_out,
                                          bool auto_trust_new)
{
    /* 获取验证参数 */
    const cJSON *trust_new_j = cJSON_GetObjectItem(params, "trust_new");
    const cJSON *accept_changed_j = cJSON_GetObjectItem(params, "accept_changed");

    bool trust_new = cJSON_IsBool(trust_new_j) ? cJSON_IsTrue(trust_new_j) : auto_trust_new;
    bool accept_changed = cJSON_IsBool(accept_changed_j) ? cJSON_IsTrue(accept_changed_j) : false;
    
    /* 验证主机指纹 */
    ts_host_verify_result_t verify_result;
    ts_known_host_t host_info = {0};
    esp_err_t ret = ts_known_hosts_verify(session, &verify_result, &host_info);
    
    if (ret != ESP_OK) {
        ts_api_result_reason(result, TS_API_ERR_INTERNAL, "known_host_failed", esp_err_to_name(ret));
        return ret;
    }

    const cJSON *confirmed = cJSON_GetObjectItem(params, "confirmed_fingerprint");
    bool confirmed_match =
        cJSON_IsString(confirmed) && !strcmp(confirmed->valuestring, host_info.fingerprint);
    trust_new = trust_new && (auto_trust_new || confirmed_match);
    accept_changed = accept_changed && confirmed_match;

    /* 输出主机信息 */
    if (host_info_out) {
        *host_info_out = host_info;
    }
    
    switch (verify_result) {
        case TS_HOST_VERIFY_OK:
            /* 指纹匹配，验证通过 */
            TS_LOGI(TAG, "Host key verified: %s:%u", host_info.host, host_info.port);
            return ESP_OK;
            
        case TS_HOST_VERIFY_NOT_FOUND:
            /* 新主机 */
            if (trust_new) {
                /* 自动信任新主机 */
                ret = ts_known_hosts_add(session);
                if (ret == ESP_OK) {
                    TS_LOGI(TAG, "New host trusted: %s:%u (fingerprint: %.16s...)", 
                            host_info.host, host_info.port, host_info.fingerprint);
                    return ESP_OK;
                } else {
                    ts_api_result_reason(result, TS_API_ERR_INTERNAL, "known_host_failed", esp_err_to_name(ret));
                    return ret;
                }
            } else {
                /* 需要用户确认 */
                cJSON *data = cJSON_CreateObject();
                cJSON_AddStringToObject(data, "status", "new_host");
                cJSON_AddStringToObject(data, "host", host_info.host);
                cJSON_AddNumberToObject(data, "port", host_info.port);
                cJSON_AddStringToObject(data, "fingerprint", host_info.fingerprint);
                cJSON_AddStringToObject(data, "message", 
                    "New host - set trust_new=true or use hosts.add to trust this host");
                result->code = TS_API_ERR_HOST_NEW;
                result->message = TS_STRDUP_PSRAM("ssh_host_key_unknown");
                result->data = data;
                return ESP_ERR_INVALID_STATE;
            }
            
        case TS_HOST_VERIFY_MISMATCH:
            /* 指纹变化 - 可能的中间人攻击！ */
            if (accept_changed) {
                /* 用户明确接受变化 */
                TS_LOGW(TAG, "Host key changed and accepted: %s:%u", host_info.host, host_info.port);
                ret = ts_known_hosts_add(session);
                if (ret == ESP_OK) {
                    return ESP_OK;
                } else {
                    ts_api_result_reason(result, TS_API_ERR_INTERNAL, "known_host_failed", esp_err_to_name(ret));
                    return ret;
                }
            } else {
                /* 拒绝连接 - 返回详细信息 */
                TS_LOGW(TAG, "Host key mismatch rejected: %s:%u", host_info.host, host_info.port);
                
                /* 获取存储的指纹 */
                ts_known_host_t stored_info = {0};
                ts_known_hosts_get(host_info.host, host_info.port, &stored_info);
                
                cJSON *data = cJSON_CreateObject();
                cJSON_AddStringToObject(data, "status", "mismatch");
                cJSON_AddStringToObject(data, "host", host_info.host);
                cJSON_AddNumberToObject(data, "port", host_info.port);
                cJSON_AddStringToObject(data, "current_fingerprint", host_info.fingerprint);
                cJSON_AddStringToObject(data, "stored_fingerprint", stored_info.fingerprint);
                cJSON_AddStringToObject(data, "message", 
                    "WARNING: Host key has changed! This could indicate a man-in-the-middle attack. "
                    "Set accept_changed=true only if you are sure the server was reinstalled.");
                result->code = TS_API_ERR_HOST_MISMATCH;
                result->message = TS_STRDUP_PSRAM("ssh_host_key_changed");
                result->data = data;
                return ESP_ERR_INVALID_STATE;
            }
            
        default:
            ts_api_result_error(result, TS_API_ERR_INTERNAL, "known_host_failed");
            return ESP_FAIL;
    }
}

typedef struct {
    const cJSON *params;
    ts_api_result_t *result;
    bool auto_trust_new;
} verify_context_t;
static esp_err_t verify_before_auth(ts_ssh_session_t session, void *opaque) {
    verify_context_t *ctx = opaque;
    return verify_host_fingerprint(session, ctx->params, ctx->result, NULL, ctx->auto_trust_new);
}
static esp_err_t connect_verified(ts_ssh_session_t session, const cJSON *params,
                                  ts_api_result_t *result, bool auto_trust_new) {
    verify_context_t ctx = {params, result, auto_trust_new};
    return ts_ssh_connect_with_verifier(session, verify_before_auth, &ctx);
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief ssh.exec - Execute remote command
 * 
 * Params: { 
 *   "active_version": "from automation.config.status",
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx" | "keyid": "default" | "keypath": "/sdcard/id_rsa",
 *   "port": 22,
 *   "command": "ls -la",
 *   "timeout_ms": 30000,
 *   "trust_new": true,        // 新主机是否自动信任（默认 true）
 *   "accept_changed": false   // 是否接受指纹变化（默认 false）
 * }
 * 
 * Response (success): {
 *   "exit_code": 0,
 *   "stdout": "...",
 *   "stderr": "...",
 *   "host_status": "trusted" | "new_trusted",
 *   "fingerprint": "sha256:..."
 * }
 * 
 * Response (host_mismatch): {
 *   "status": "mismatch",
 *   "current_fingerprint": "...",
 *   "stored_fingerprint": "...",
 *   "message": "WARNING: ..."
 * }
 */
static esp_err_t api_ssh_exec(const cJSON *params, ts_api_result_t *result)
{
    ssh_request_key_t request_key __attribute__((cleanup(cleanup_key_buffer))) = {0};
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *cmd = cJSON_GetObjectItem(params, "command");
    if (!cmd || !cJSON_IsString(cmd)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'command' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 配置 SSH */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    esp_err_t ret = configure_ssh_from_params(params, &config, &request_key);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "ssh_config_invalid");
        cleanup_key_buffer(&request_key);
        return ret;
    }
    
    /* 超时设置 */
    const cJSON *timeout = cJSON_GetObjectItem(params, "timeout_ms");
    if (cJSON_IsNumber(timeout)) {
        config.timeout_ms = timeout->valueint;
    }
    
    /* 创建会话 */
    ts_ssh_session_t session = NULL;
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        cleanup_key_buffer(&request_key);
        return ret;
    }
    
    /* 连接（TCP 层） */
    ret = connect_verified(session, params, result, false);
    if (ret != ESP_OK) {
        if (result->code == TS_API_OK)
            ssh_connect_error(result, session);
        ts_ssh_session_destroy(session);
        cleanup_key_buffer(&request_key);
        return ret;
    }
    
    /* 验证主机指纹 */
    ts_known_host_t host_info = {0};
    ret = verify_host_fingerprint(session, params, result, &host_info, false);
    if (ret != ESP_OK) {
        /* result 已在 verify_host_fingerprint 中设置 */
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        cleanup_key_buffer(&request_key);
        return ret;
    }
    
    /* 执行命令 */
    ts_ssh_exec_result_t exec_result = {0};
    ret = ts_ssh_exec(session, cmd->valuestring, &exec_result);
    
    if (ret == ESP_OK) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "exit_code", exec_result.exit_code);
        
        if (exec_result.stdout_data) {
            cJSON_AddStringToObject(data, "stdout", exec_result.stdout_data);
        } else {
            cJSON_AddStringToObject(data, "stdout", "");
        }
        
        if (exec_result.stderr_data) {
            cJSON_AddStringToObject(data, "stderr", exec_result.stderr_data);
        } else {
            cJSON_AddStringToObject(data, "stderr", "");
        }
        
        /* 添加主机验证信息 */
        cJSON_AddStringToObject(data, "host_status", "trusted");
        cJSON_AddStringToObject(data, "fingerprint", host_info.fingerprint);
        
        ts_api_result_ok(result, data);
        ts_ssh_exec_result_free(&exec_result);
    } else {
        ts_api_result_reason(result, TS_API_ERR_INTERNAL, "ssh_exec_failed", ts_ssh_get_error(session));
    }
    
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    cleanup_key_buffer(&request_key);

    return ret;
}

/**
 * @brief ssh.exec_stream - Execute remote command with streaming output
 * 
 * 流式执行命令，输出通过 WebSocket 实时推送。
 * 支持使用 ssh.cancel 中止执行。
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx" | "keyid": "default",
 *   "port": 22,
 *   "command": "ping -c 10 8.8.8.8"
 * }
 * 
 * Response: { "session_id": 12345 }
 * 
 * WebSocket Events:
 * - ssh_exec_start: { "session_id", "command" }
 * - ssh_exec_output: { "session_id", "data", "is_stderr" }
 * - ssh_exec_done: { "session_id", "exit_code", "success" }
 * - ssh_exec_error: { "session_id", "error" }
 * - ssh_exec_cancelled: { "session_id" }
 * 
 * Optional options for advanced usage:
 * - expect_pattern: Regex pattern to match for success
 * - fail_pattern: Regex pattern that indicates failure
 * - extract_pattern: Regex pattern with capture group to extract value
 * - timeout: Command timeout in milliseconds (default 30000)
 * - collect_output: Whether to collect output (default true)
 * - max_output_size: Maximum output to collect (default 65536)
 */
static esp_err_t api_ssh_exec_stream(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 检查必需参数 */
    const cJSON *host = cJSON_GetObjectItem(params, "host");
    const cJSON *user = cJSON_GetObjectItem(params, "user");
    const cJSON *cmd = cJSON_GetObjectItem(params, "command");
    
    if (!host || !cJSON_IsString(host) || 
        !user || !cJSON_IsString(user) ||
        !cmd || !cJSON_IsString(cmd)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, 
                            "Missing required parameters: host, user, command");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 可选参数 */
    const cJSON *port = cJSON_GetObjectItem(params, "port");
    const cJSON *password = cJSON_GetObjectItem(params, "password");
    const cJSON *keyid = cJSON_GetObjectItem(params, "keyid");
    
    uint16_t ssh_port = (port && cJSON_IsNumber(port)) ? (uint16_t)port->valueint : 22;
    const char *auth_password = (password && cJSON_IsString(password)) ? password->valuestring : NULL;
    const char *auth_keyid = (keyid && cJSON_IsString(keyid)) ? keyid->valuestring : NULL;
    
    /* 检查认证方式 */
    if (!auth_password && !auth_keyid) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, 
                            "Either 'password' or 'keyid' must be provided");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 解析可选的匹配选项 */
    const cJSON *expect_pattern = cJSON_GetObjectItem(params, "expect_pattern");
    const cJSON *fail_pattern = cJSON_GetObjectItem(params, "fail_pattern");
    const cJSON *extract_pattern = cJSON_GetObjectItem(params, "extract_pattern");
    const cJSON *var_name = cJSON_GetObjectItem(params, "var_name");
    const cJSON *timeout = cJSON_GetObjectItem(params, "timeout");
    const cJSON *collect_output = cJSON_GetObjectItem(params, "collect_output");
    const cJSON *max_output_size = cJSON_GetObjectItem(params, "max_output_size");
    const cJSON *stop_on_match = cJSON_GetObjectItem(params, "stop_on_match");
    
    /* 检查是否有任何可选参数 */
    bool has_options = (expect_pattern || fail_pattern || extract_pattern || var_name ||
                        timeout || collect_output || max_output_size || stop_on_match);
    
    esp_err_t ret;
    uint32_t session_id = 0;
    
    if (has_options) {
        /* 使用扩展 API */
        ts_webui_ssh_options_t options = {0};
        options.expect_pattern = (expect_pattern && cJSON_IsString(expect_pattern)) ? 
                                  expect_pattern->valuestring : NULL;
        options.fail_pattern = (fail_pattern && cJSON_IsString(fail_pattern)) ? 
                                fail_pattern->valuestring : NULL;
        options.extract_pattern = (extract_pattern && cJSON_IsString(extract_pattern)) ? 
                                   extract_pattern->valuestring : NULL;
        options.var_name = (var_name && cJSON_IsString(var_name)) ?
                            var_name->valuestring : NULL;
        options.timeout_ms = (timeout && cJSON_IsNumber(timeout)) ? 
                              (uint32_t)timeout->valueint : 0;
        options.collect_output = (collect_output && cJSON_IsBool(collect_output)) ? 
                                  cJSON_IsTrue(collect_output) : true;
        options.max_output_size = (max_output_size && cJSON_IsNumber(max_output_size)) ? 
                                   (uint32_t)max_output_size->valueint : 0;
        options.stop_on_match = (stop_on_match && cJSON_IsBool(stop_on_match)) ? 
                                 cJSON_IsTrue(stop_on_match) : false;
        
        TS_LOGW(TAG, "api_ssh_exec_stream: var_name param=%p, is_string=%d, value='%s'",
                 (void*)var_name, var_name ? cJSON_IsString(var_name) : 0,
                 options.var_name ? options.var_name : "(null)");
        
        ret = ts_webui_ssh_exec_start_ex(
            host->valuestring, ssh_port,
            user->valuestring, auth_keyid, auth_password,
            cmd->valuestring, &options, &session_id
        );
    } else {
        /* 使用基本 API */
        ret = ts_webui_ssh_exec_start(
            host->valuestring, ssh_port,
            user->valuestring, auth_keyid, auth_password,
            cmd->valuestring, &session_id
        );
    }
    
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE) {
            ts_api_result_error(result, TS_API_ERR_BUSY, "ssh_exec_busy");
        } else {
            ts_api_result_reason(result, TS_API_ERR_INTERNAL, "ssh_start_failed", esp_err_to_name(ret));
        }
        return ret;
    }
    
    /* 返回 session_id 和配置信息 */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "session_id", session_id);
    if (has_options) {
        if (expect_pattern && cJSON_IsString(expect_pattern)) {
            cJSON_AddStringToObject(data, "expect_pattern", expect_pattern->valuestring);
        }
        if (fail_pattern && cJSON_IsString(fail_pattern)) {
            cJSON_AddStringToObject(data, "fail_pattern", fail_pattern->valuestring);
        }
        if (extract_pattern && cJSON_IsString(extract_pattern)) {
            cJSON_AddStringToObject(data, "extract_pattern", extract_pattern->valuestring);
        }
    }
    ts_api_result_ok(result, data);
    
    return ESP_OK;
}

/**
 * @brief ssh.cancel - Cancel running SSH exec session
 * 
 * Params: { "session_id": 12345 }
 * 
 * Response: { "cancelled": true }
 */
static esp_err_t api_ssh_cancel(const cJSON *params, ts_api_result_t *result)
{
    const cJSON *sid = cJSON_GetObjectItem(params, "session_id");
    
    /* Exact instance identity; a status wildcard is never a control target. */
    if (!cJSON_IsNumber(sid) || sid->valuedouble < 1 || sid->valuedouble > UINT32_MAX ||
        sid->valuedouble != (double)(uint32_t)sid->valuedouble) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "ssh_no_session");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = ts_webui_ssh_exec_cancel((uint32_t)sid->valuedouble);
    if (ret != ESP_OK) {
        ts_api_result_error(result, ret == ESP_ERR_INVALID_STATE ? TS_API_ERR_NOT_FOUND : TS_API_ERR_INTERNAL,
                            ret == ESP_ERR_INVALID_STATE ? "ssh_no_session" : "ssh_cancel_failed");
        return ret;
    }
    /* Existing wire field means accepted intent, not confirmed remote exit. */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "cancelled", true);
    ts_api_result_ok(result, data);
    
    return ESP_OK;
}

/**
 * @brief ssh.test - Test SSH connection
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx" | "keyid": "default" | "keypath": "/sdcard/id_rsa",
 *   "port": 22,
 *   "trust_new": true,        // 新主机是否自动信任（默认 true）
 *   "accept_changed": false   // 是否接受指纹变化（默认 false）
 * }
 * 
 * Response (success): {
 *   "success": true,
 *   "host": "...",
 *   "port": 22,
 *   "user": "...",
 *   "host_status": "trusted" | "new_trusted",
 *   "fingerprint": "sha256:..."
 * }
 */
static esp_err_t api_ssh_test(const cJSON *params, ts_api_result_t *result)
{
    ssh_request_key_t request_key __attribute__((cleanup(cleanup_key_buffer))) = {0};
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 配置 SSH */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    esp_err_t ret = configure_ssh_from_params(params, &config, &request_key);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "ssh_config_invalid");
        cleanup_key_buffer(&request_key);
        return ret;
    }
    
    /* 创建会话 */
    ts_ssh_session_t session = NULL;
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        cleanup_key_buffer(&request_key);
        return ret;
    }
    
    /* 测试连接（TCP 层） */
    ret = connect_verified(session, params, result, false);

    if (ret != ESP_OK) {
        if (result->code != TS_API_OK) {
            ts_ssh_session_destroy(session);
            cleanup_key_buffer(&request_key);
            return ESP_OK;
        }
        const char *err = ts_ssh_get_error(session);
        const char *stage = "connect";
        const char *reason = ts_ssh_connect_reason(session, &stage);
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "success", false);
        cJSON_AddStringToObject(data, "error", err ? err : "Connection failed");
        cJSON_AddStringToObject(data, "reason", reason);   /* additive: older clients keep reading `error` */
        cJSON_AddStringToObject(data, "stage", stage);
        ts_api_result_ok(result, data);
        ts_ssh_session_destroy(session);
        cleanup_key_buffer(&request_key);
        return ESP_OK;
    }
    
    /* 验证主机指纹 */
    ts_known_host_t host_info = {0};
    ret = verify_host_fingerprint(session, params, result, &host_info, false);
    if (ret != ESP_OK) {
        /* 主机验证失败（新主机或密钥不匹配）
         * result 中已包含详细信息，直接返回成功（HTTP 200）
         * 前端通过 result.code 和 result.data.status 判断实际状态
         */
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        cleanup_key_buffer(&request_key);

        /* 如果是 MISMATCH 或 NEW_HOST，返回 ESP_OK 让 HTTP 层返回 200
         * 实际错误信息已在 result 中设置 */
        if (result->code == TS_API_ERR_HOST_MISMATCH || 
            result->code == TS_API_ERR_HOST_NEW) {
            return ESP_OK;  // HTTP 200，但 result.code 指示实际问题
        }
        return ret;  // 其他错误继续返回错误码
    }
    
    /* 连接成功 */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", true);
    cJSON_AddStringToObject(data, "host", config.host);
    cJSON_AddNumberToObject(data, "port", config.port);
    cJSON_AddStringToObject(data, "user", config.username);
    cJSON_AddStringToObject(data, "host_status", "trusted");
    cJSON_AddStringToObject(data, "fingerprint", host_info.fingerprint);
    
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    cleanup_key_buffer(&request_key);

    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief ssh.copyid - Deploy public key to remote server
 * 
 * 将密钥存储中的公钥部署到远程服务器的 ~/.ssh/authorized_keys
 * 流程：
 * 1. 使用密码认证连接到远程服务器
 * 2. 验证主机指纹（Known Hosts）
 * 3. 部署公钥到 authorized_keys
 * 4. 使用公钥认证验证部署是否成功
 * 5. 登记主机到当前自动化配置；登记失败保留 deployed/verified 并返回业务错误
 *
 * Result data adds host_id, registered (configuration committed), registration_error (on failure).
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx",
 *   "keyid": "default",
 *   "port": 22,
 *   "verify": true,
 *   "trust_new": true,        // 新主机是否自动信任（默认 true）
 *   "accept_changed": false   // 是否接受指纹变化（默认 false）
 * }
 */
static esp_err_t api_ssh_copyid(const cJSON *params, ts_api_result_t *result)
{
    ssh_request_key_t request_key __attribute__((cleanup(cleanup_key_buffer))) = {0};
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 获取必需参数 */
    const cJSON *host = cJSON_GetObjectItem(params, "host");
    const cJSON *user = cJSON_GetObjectItem(params, "user");
    const cJSON *password = cJSON_GetObjectItem(params, "password");
    const cJSON *keyid = cJSON_GetObjectItem(params, "keyid");
    const cJSON *port = cJSON_GetObjectItem(params, "port");
    const cJSON *verify = cJSON_GetObjectItem(params, "verify");
    
    if (!host || !cJSON_IsString(host) || !host->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'host' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    if (!user || !cJSON_IsString(user) || !user->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'user' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    if (!password || !cJSON_IsString(password) || !password->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'password' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    if (!keyid || !cJSON_IsString(keyid) || !keyid->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'keyid' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    int ssh_port = cJSON_IsNumber(port) ? port->valueint : 22;
    bool do_verify = cJSON_IsBool(verify) ? cJSON_IsTrue(verify) : true;
    
    const char *version=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params,"active_version"));
    ts_auto_config_error_t registration_error={0};
    esp_err_t checked=ts_auto_config_edit_begin(version,&registration_error);
    if(checked!=ESP_OK){ts_api_result_error(result,TS_API_ERR_INVALID_ARG,registration_error.reason);return ESP_OK;}
    ts_auto_config_edit_end();

    /* 加载公钥 */
    char *pubkey_data = NULL;
    size_t pubkey_len = 0;
    esp_err_t ret = ts_keystore_load_public_key(keyid->valuestring, &pubkey_data, &pubkey_len);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "key_not_found");
        return ret;
    }
    
    /* 配置 SSH 连接（使用密码认证） */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();

    config.host = host->valuestring;
    config.port = ssh_port;
    config.username = user->valuestring;
    config.auth_method = TS_SSH_AUTH_PASSWORD;
    config.auth.password = password->valuestring;

    /* 创建会话并连接 */
    ts_ssh_session_t session = NULL;
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        free(pubkey_data);
        return ret;
    }

    /* 首次部署自动保存新主机指纹；已知主机指纹变化仍在认证前拒绝。 */
    ret = connect_verified(session, params, result, true);
    if (ret != ESP_OK) {
        if (result->code == TS_API_OK)
            ssh_connect_error(result, session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        return ret;
    }
    
    /* 验证主机指纹 */
    ts_known_host_t host_info = {0};
    ret = verify_host_fingerprint(session, params, result, &host_info, true);
    if (ret != ESP_OK) {
        /* result 已在 verify_host_fingerprint 中设置 */
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        return ret;
    }
    
    /* 构建部署命令（与 CLI 逻辑一致） */
    size_t deploy_capacity = strlen(pubkey_data) * 4 + 256;
    char *deploy_cmd = TS_MALLOC_PSRAM(deploy_capacity);
    if (!deploy_cmd) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        return ESP_ERR_NO_MEM;
    }
    
    if (!ts_ssh_copyid_command(pubkey_data, deploy_cmd, deploy_capacity)) {
        free(deploy_cmd);
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "pubkey_invalid");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 执行部署命令 */
    ts_ssh_exec_result_t exec_result = {0};
    ret = ts_ssh_exec(session, deploy_cmd, &exec_result);
    free(deploy_cmd);
    
    bool deploy_ok = (ret == ESP_OK && exec_result.exit_code == 0);
    char *stderr_msg = NULL;
    if (exec_result.stderr_data && strlen(exec_result.stderr_data) > 0) {
        stderr_msg = TS_STRDUP_PSRAM(exec_result.stderr_data);
    }
    ts_ssh_exec_result_free(&exec_result);
    
    /* 断开密码连接 */
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    
    if (!deploy_ok) {
        ts_api_result_reason(result, TS_API_ERR_INTERNAL, "ssh_deploy_failed", stderr_msg);
        free(stderr_msg);
        free(pubkey_data);
        return ESP_FAIL;
    }
    free(stderr_msg);
    
    /* 验证公钥认证（可选） */
    bool verified = false;
    if (do_verify) {
        /* 加载私钥 */
        cleanup_key_buffer(&request_key);
        ret = ts_keystore_load_private_key(keyid->valuestring, &request_key.data, &request_key.len);
        if (ret == ESP_OK) {
            /* 使用公钥认证重新连接 */
            ts_ssh_config_t verify_config = TS_SSH_DEFAULT_CONFIG();
            verify_config.host = host->valuestring;
            verify_config.port = ssh_port;
            verify_config.username = user->valuestring;
            verify_config.auth_method = TS_SSH_AUTH_PUBLICKEY;
            verify_config.auth.key.private_key = (const uint8_t *)request_key.data;
            verify_config.auth.key.private_key_len = request_key.len;

            ts_ssh_session_t verify_session = NULL;
            if (ts_ssh_session_create(&verify_config, &verify_session) == ESP_OK) {
                if (ts_ssh_connect(verify_session) == ESP_OK) {
                    verified = true;
                    ts_ssh_disconnect(verify_session);
                }
                ts_ssh_session_destroy(verify_session);
            }
            cleanup_key_buffer(&request_key);
        }
    }
    
    free(pubkey_data);
    
    /* Registration is independent of remote deployment and login verification. */
    esp_err_t add_ret;
    char auto_id[TS_SSH_HOST_ID_MAX];
    {
        /* 生成主机 ID：user@host:port */
        int id_length=ssh_port==22?snprintf(auto_id,sizeof(auto_id),"%s@%s",user->valuestring,host->valuestring):
            snprintf(auto_id,sizeof(auto_id),"%s@%s:%d",user->valuestring,host->valuestring,ssh_port);
        
        ts_ssh_host_config_t host_config = {
            .port = (uint16_t)ssh_port,
            .auth_type = TS_SSH_HOST_AUTH_KEY,
            .enabled = true,
        };
        strncpy(host_config.id, auto_id, sizeof(host_config.id) - 1);
        strncpy(host_config.host, host->valuestring, sizeof(host_config.host) - 1);
        strncpy(host_config.username, user->valuestring, sizeof(host_config.username) - 1);
        strncpy(host_config.keyid, keyid->valuestring, sizeof(host_config.keyid) - 1);
        
        if(id_length<0||(size_t)id_length>=sizeof(auto_id)||strlen(host->valuestring)>=sizeof(host_config.host)||
            strlen(user->valuestring)>=sizeof(host_config.username)||strlen(keyid->valuestring)>=sizeof(host_config.keyid)){
            add_ret=ESP_ERR_INVALID_ARG;registration_error.reason="configuration_invalid";
        }else{
            add_ret=ts_auto_config_edit_begin(version,&registration_error);
            if(add_ret==ESP_OK){
                ts_ssh_host_config_t old;
                if(ts_ssh_hosts_config_get(auto_id,&old)==ESP_OK)host_config.created_time=old.created_time;
                else{time_t now=time(NULL);host_config.created_time=now>0?now:0;}
                cJSON *row=ts_ssh_host_encode(&host_config);
                if(row)add_ret=ts_auto_config_set(TS_AUTO_CONFIG_HOST,row,NULL,&registration_error);
                else{add_ret=ESP_ERR_NO_MEM;registration_error.reason="out_of_memory";}
                cJSON_Delete(row);ts_auto_config_edit_end();
            }
        }
        if (add_ret == ESP_OK) {
            TS_LOGI(TAG, "Auto-registered SSH host: %s", auto_id);
        } else {
            TS_LOGW(TAG, "Public key deployed but host %s registration failed: %s",
                    auto_id, esp_err_to_name(add_ret));
        }
    }
    
    /* 返回结果 */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "deployed", true);
    cJSON_AddBoolToObject(data, "verified", verified);
    cJSON_AddStringToObject(data, "host", host->valuestring);
    cJSON_AddNumberToObject(data, "port", ssh_port);
    cJSON_AddStringToObject(data, "user", user->valuestring);
    cJSON_AddStringToObject(data, "keyid", keyid->valuestring);
    cJSON_AddStringToObject(data, "host_id", auto_id);
    cJSON_AddBoolToObject(data, "registered", add_ret == ESP_OK);
    
    if (add_ret == ESP_OK) {
        ts_api_result_ok(result, data);
    } else {
        cJSON_AddStringToObject(data, "registration_error", registration_error.reason?registration_error.reason:"configuration_unavailable");
        ts_api_result_code_t code = add_ret == ESP_ERR_INVALID_STATE ? TS_API_ERR_BUSY
                                 : add_ret == ESP_ERR_NO_MEM ? TS_API_ERR_NO_MEM
                                 : TS_API_ERR_INTERNAL;
        ts_api_result_error(result, code, "host_registration_failed");
        result->data = data;
    }
    /* Keep partial-result data in the existing HTTP response serializer. */
    return ESP_OK;
}

/**
 * @brief ssh.revoke - Revoke (remove) deployed public key from remote server
 * 
 * 从远程服务器的 ~/.ssh/authorized_keys 中移除已部署的公钥
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx",
 *   "keyid": "default",
 *   "port": 22,
 *   "trust_new": true,        // 新主机是否自动信任（默认 true）
 *   "accept_changed": false   // 是否接受指纹变化（默认 false）
 * }
 */
static esp_err_t api_ssh_revoke(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 获取必需参数 */
    const cJSON *host = cJSON_GetObjectItem(params, "host");
    const cJSON *user = cJSON_GetObjectItem(params, "user");
    const cJSON *password = cJSON_GetObjectItem(params, "password");
    const cJSON *keyid = cJSON_GetObjectItem(params, "keyid");
    const cJSON *port = cJSON_GetObjectItem(params, "port");
    
    if (!host || !cJSON_IsString(host) || !host->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'host' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    if (!user || !cJSON_IsString(user) || !user->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'user' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    if (!password || !cJSON_IsString(password) || !password->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'password' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    if (!keyid || !cJSON_IsString(keyid) || !keyid->valuestring[0]) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'keyid' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    int ssh_port = cJSON_IsNumber(port) ? port->valueint : 22;
    
    /* 加载公钥 */
    char *pubkey_data = NULL;
    size_t pubkey_len = 0;
    esp_err_t ret = ts_keystore_load_public_key(keyid->valuestring, &pubkey_data, &pubkey_len);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "key_not_found");
        return ret;
    }
    
    /* 解析公钥：提取 key_type 和 key_data */
    char *pubkey_copy = TS_STRDUP_PSRAM(pubkey_data);
    if (!pubkey_copy) {
        free(pubkey_data);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        return ESP_ERR_NO_MEM;
    }
    
    char *key_type = strtok(pubkey_copy, " ");
    char *key_data = strtok(NULL, " ");
    
    if (!key_type || !key_data) {
        free(pubkey_data);
        free(pubkey_copy);
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "pubkey_invalid");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 构建用于匹配的密钥签名（type + 前100字符） */
    char key_signature[160];
    size_t sig_len = strlen(key_data) > 100 ? 100 : strlen(key_data);
    snprintf(key_signature, sizeof(key_signature), "%s %.*s", key_type, (int)sig_len, key_data);
    
    /* 配置 SSH 连接（使用密码认证） */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();

    config.host = host->valuestring;
    config.port = ssh_port;
    config.username = user->valuestring;
    config.auth_method = TS_SSH_AUTH_PASSWORD;
    config.auth.password = password->valuestring;

    /* 创建会话并连接 */
    ts_ssh_session_t session = NULL;
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        free(pubkey_data);
        free(pubkey_copy);
        return ret;
    }

    ret = connect_verified(session, params, result, false);
    if (ret != ESP_OK) {
        if (result->code == TS_API_OK)
            ssh_connect_error(result, session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return ret;
    }
    
    /* 验证主机指纹 */
    ts_known_host_t host_info = {0};
    ret = verify_host_fingerprint(session, params, result, &host_info, false);
    if (ret != ESP_OK) {
        /* result 已在 verify_host_fingerprint 中设置 */
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return ret;
    }
    
    /* 1. 检查密钥是否存在 */
    char *check_cmd = TS_MALLOC_PSRAM(512);
    if (!check_cmd) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return ESP_ERR_NO_MEM;
    }
    
    snprintf(check_cmd, 512,
        "if [ -f ~/.ssh/authorized_keys ]; then "
        "  grep -cF '%s' ~/.ssh/authorized_keys 2>/dev/null || echo '0'; "
        "else "
        "  echo '0'; "
        "fi",
        key_signature);
    
    ts_ssh_exec_result_t exec_result = {0};
    ret = ts_ssh_exec(session, check_cmd, &exec_result);
    free(check_cmd);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "ssh_revoke_check_failed");
        ts_ssh_exec_result_free(&exec_result);
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return ret;
    }
    
    int key_count = exec_result.stdout_data ? atoi(exec_result.stdout_data) : 0;
    ts_ssh_exec_result_free(&exec_result);
    
    if (key_count == 0) {
        /* 密钥不存在 */
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "revoked", false);
        cJSON_AddBoolToObject(data, "found", false);
        cJSON_AddStringToObject(data, "message", "Key not found on remote server");
        ts_api_result_ok(result, data);
        return ESP_OK;
    }
    
    /* 2. 执行删除操作 */
    char *revoke_cmd = TS_MALLOC_PSRAM(512);
    if (!revoke_cmd) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return ESP_ERR_NO_MEM;
    }
    
    snprintf(revoke_cmd, 512,
        "cp ~/.ssh/authorized_keys ~/.ssh/authorized_keys.bak 2>/dev/null; "
        "grep -vF '%s' ~/.ssh/authorized_keys > ~/.ssh/authorized_keys.tmp 2>/dev/null && "
        "mv ~/.ssh/authorized_keys.tmp ~/.ssh/authorized_keys && "
        "chmod 600 ~/.ssh/authorized_keys && "
        "echo 'REVOKE_OK'",
        key_signature);
    
    ret = ts_ssh_exec(session, revoke_cmd, &exec_result);
    free(revoke_cmd);
    
    bool revoke_ok = (ret == ESP_OK && exec_result.stdout_data && 
                      strstr(exec_result.stdout_data, "REVOKE_OK") != NULL);
    ts_ssh_exec_result_free(&exec_result);
    
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    free(pubkey_data);
    free(pubkey_copy);
    
    if (!revoke_ok) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "ssh_revoke_failed");
        return ESP_FAIL;
    }
    
    /* 返回结果 */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "revoked", true);
    cJSON_AddBoolToObject(data, "found", true);
    cJSON_AddNumberToObject(data, "removed_count", key_count);
    cJSON_AddStringToObject(data, "host", host->valuestring);
    cJSON_AddNumberToObject(data, "port", ssh_port);
    cJSON_AddStringToObject(data, "user", user->valuestring);
    cJSON_AddStringToObject(data, "keyid", keyid->valuestring);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief ssh.keygen - Generate SSH key pair
 * 
 * Params: { 
 *   "id": "mykey",
 *   "type": "ecdsa" | "rsa-2048" | "rsa-4096",
 *   "comment": "optional comment"
 * }
 */
static esp_err_t api_ssh_keygen(const cJSON *params, ts_api_result_t *result)
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
    
    /* 检查密钥是否已存在 */
    if (ts_keystore_key_exists(id->valuestring)) {
        ts_api_result_error(result, TS_API_ERR_BUSY, "key_exists");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 解析密钥类型 */
    const cJSON *type_json = cJSON_GetObjectItem(params, "type");
    const char *type_str = cJSON_IsString(type_json) ? type_json->valuestring : "ecdsa";
    
    ts_keystore_key_type_t type = TS_KEYSTORE_TYPE_ECDSA_P256;
    if (strcmp(type_str, "rsa-2048") == 0) {
        type = TS_KEYSTORE_TYPE_RSA_2048;
    } else if (strcmp(type_str, "rsa-4096") == 0) {
        type = TS_KEYSTORE_TYPE_RSA_4096;
    } else if (strcmp(type_str, "ecdsa-p384") == 0) {
        type = TS_KEYSTORE_TYPE_ECDSA_P384;
    }
    
    /* 注释 */
    const cJSON *comment = cJSON_GetObjectItem(params, "comment");
    const char *comment_str = cJSON_IsString(comment) ? comment->valuestring : NULL;
    
    /* 生成密钥 */
    esp_err_t ret = ts_keystore_generate_key(id->valuestring, type, comment_str);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Key generation failed");
        return ret;
    }
    
    /* 获取公钥 */
    char *pubkey = NULL;
    size_t pubkey_len = 0;
    ret = ts_keystore_load_public_key(id->valuestring, &pubkey, &pubkey_len);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "id", id->valuestring);
    cJSON_AddStringToObject(data, "type", type_str);
    
    if (ret == ESP_OK && pubkey) {
        cJSON_AddStringToObject(data, "public_key", pubkey);
        free(pubkey);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                      SSH Hosts Config API                                  */
/*===========================================================================*/

/**
 * @brief 迭代器回调用户数据结构（hosts）
 */

/**
 * @brief 迭代器回调函数 - 将主机添加到 JSON 数组
 */


/**
 * @brief ssh.hosts.list - 列出所有 SSH 主机配置（支持分页）
 * 
 * 使用流式迭代器，每次只加载一条配置。
 * 
 * Params: {
 *   "offset": 0   (可选，分页起始位置，默认 0)
 *   "limit": 0    (可选，每页数量，0 表示不限制，默认不限制)
 * }
 */
static esp_err_t api_ssh_hosts_list(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_list(TS_AUTO_CONFIG_HOST,params,"hosts",result); }

/**
 * @brief ssh.hosts.add - 添加 SSH 主机配置
 * 
 * Params: {
 *   "id": "agx0",
 *   "host": "192.168.55.100",
 *   "port": 22,
 *   "username": "root",
 *   "auth_type": "key" | "password",
 *   "keyid": "default" (如果 auth_type=key)
 * }
 */
static esp_err_t api_ssh_hosts_add(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_write(TS_AUTO_CONFIG_HOST,params,TS_API_AUTO_UPSERT,result); }

/**
 * @brief ssh.hosts.remove - 删除 SSH 主机配置
 */
static esp_err_t api_ssh_hosts_remove(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_delete(TS_AUTO_CONFIG_HOST,params,result); }

/**
 * @brief ssh.hosts.get - 获取 SSH 主机配置
 */
static esp_err_t api_ssh_hosts_get(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_get(TS_AUTO_CONFIG_HOST,params,result); }

/**
 * @brief ssh.hosts.export - 导出单个 SSH 主机配置为 .tscfg
 * 
 * Params: {
 *   "id": "agx0",                    // 主机 ID
 *   "recipient_cert": "-----BEGIN..." // 可选，目标设备证书（不提供则加密给本机）
 * }
 * 
 * Response: {
 *   "tscfg": "{ ... }",  // .tscfg JSON 内容
 *   "filename": "agx0.tscfg"
 * }
 */
static esp_err_t api_ssh_hosts_export(const cJSON *params, ts_api_result_t *result)
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
    
    /* 检查导出权限 */
    if (!ts_config_pack_can_export()) {
        ts_api_result_reason(result, TS_API_ERR_NO_PERMISSION, "pack_not_allowed", NULL);
        return ESP_OK;
    }
    
    /* 获取主机配置 */
    ts_ssh_host_config_t config;
    esp_err_t ret = ts_ssh_hosts_config_get(id->valuestring, &config);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Host not found");
        return ESP_OK;
    }
    
    /* 构建 JSON 内容 */
    cJSON *host_json = cJSON_CreateObject();
    cJSON_AddStringToObject(host_json, "id", config.id);
    cJSON_AddStringToObject(host_json, "host", config.host);
    cJSON_AddNumberToObject(host_json, "port", config.port);
    cJSON_AddStringToObject(host_json, "username", config.username);
    cJSON_AddStringToObject(host_json, "auth_type", 
        config.auth_type == TS_SSH_HOST_AUTH_KEY ? "key" : "password");
    if (config.keyid[0]) {
        cJSON_AddStringToObject(host_json, "keyid", config.keyid);
    }
    cJSON_AddBoolToObject(host_json, "enabled", config.enabled);
    
    char *json_str = cJSON_PrintUnformatted(host_json);
    cJSON_Delete(host_json);
    if (!json_str) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        return ESP_OK;
    }
    
    /* 获取接收方证书（默认使用本机证书） */
    const cJSON *recipient_cert = cJSON_GetObjectItem(params, "recipient_cert");
    char *cert_pem = NULL;
    size_t cert_len = 0;
    
    if (recipient_cert && cJSON_IsString(recipient_cert) && recipient_cert->valuestring[0]) {
        cert_pem = recipient_cert->valuestring;
        cert_len = strlen(cert_pem);
    } else {
        /* 使用本机证书 */
        cert_pem = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!cert_pem) {
            cJSON_free(json_str);
            ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
            return ESP_OK;
        }
        cert_len = 4096;
        ret = ts_cert_get_certificate(cert_pem, &cert_len);
        if (ret != ESP_OK) {
            free(cert_pem);
            cJSON_free(json_str);
            ts_api_result_reason(result, TS_API_ERR_INTERNAL, "device_cert_unavailable", esp_err_to_name(ret));
            return ESP_OK;
        }
    }
    
    /* 创建加密配置包 */
    ts_config_pack_export_opts_t opts = {
        .recipient_cert_pem = cert_pem,
        .recipient_cert_len = cert_len,
        .description = "SSH host configuration"
    };
    
    char *tscfg_output = NULL;
    size_t tscfg_len = 0;
    
    ts_config_pack_result_t pack_result = ts_config_pack_create(
        config.id, json_str, strlen(json_str), &opts, &tscfg_output, &tscfg_len);
    
    cJSON_free(json_str);
    if (!recipient_cert || !cJSON_IsString(recipient_cert)) {
        free(cert_pem);  /* 只有本机分配的才需要释放 */
    }
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        pack_failure(result, pack_result);
        return ESP_OK;
    }
    
    /* 返回结果 */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "tscfg", tscfg_output);
    
    char filename[64];
    snprintf(filename, sizeof(filename), "%s.tscfg", config.id);
    cJSON_AddStringToObject(data, "filename", filename);
    
    free(tscfg_output);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief ssh.hosts.import - 导入 SSH 主机配置（从 .tscfg）
 * 
 * Params: {
 *   "tscfg": "{ ... }",  // .tscfg JSON 内容
 *   "overwrite": false   // 可选，是否覆盖已存在的配置
 * }
 * 
 * Response: {
 *   "id": "agx0",
 *   "host": "192.168.1.100",
 *   "imported": true
 * }
 */
static esp_err_t api_ssh_hosts_import(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_pack_import(TS_AUTO_CONFIG_HOST,params,result); }

/*===========================================================================*/
/*                       SSH Command Config APIs                              */
/*===========================================================================*/

/**
 * @brief 迭代器回调用户数据结构
 */

/**
 * @brief 检查主机 ID 是否存在
 */


/**
 * @brief 将单个命令配置转换为 JSON 对象
 * 
 * 自动检测孤儿命令（引用了不存在的主机）
 */


/**
 * @brief 迭代器回调函数 - 将命令添加到 JSON 数组
 */


/**
 * @brief ssh.commands.list - 列出所有 SSH 指令配置（支持分页）
 * 
 * 使用流式迭代器，每次只加载一条命令，避免大块内存分配。
 * 
 * Params: {
 *   "host_id": "agx0" (可选，按主机过滤)
 *   "offset": 0       (可选，分页起始位置，默认 0)
 *   "limit": 20       (可选，每页数量，默认 20，0 表示不限制)
 * }
 * 
 * Response: {
 *   "commands": [...],
 *   "count": 5,         // 本次返回的数量
 *   "total": 64,        // 总数量
 *   "offset": 0,        // 当前偏移
 *   "limit": 20         // 当前限制
 * }
 */
static esp_err_t api_ssh_commands_list(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_list(TS_AUTO_CONFIG_COMMAND,params,"commands",result); }

/**
 * @brief ssh.commands.add - 添加 SSH 指令配置
 */
static esp_err_t api_ssh_commands_add(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_write(TS_AUTO_CONFIG_COMMAND,params,TS_API_AUTO_UPSERT,result); }

/**
 * @brief ssh.commands.remove - 删除 SSH 指令配置
 */
static esp_err_t api_ssh_commands_remove(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_delete(TS_AUTO_CONFIG_COMMAND,params,result); }

/**
 * @brief ssh.commands.get - 获取 SSH 指令配置
 */
static esp_err_t api_ssh_commands_get(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_get(TS_AUTO_CONFIG_COMMAND,params,result); }

/**
 * @brief ssh.commands.export - 导出单个 SSH 指令配置为 .tscfg
 * 
 * Params: {
 *   "id": "check_gpu",               // 指令 ID
 *   "include_host": false,           // 可选，是否包含依赖的主机配置
 *   "recipient_cert": "-----BEGIN..." // 可选，目标设备证书
 * }
 * 
 * Response: {
 *   "tscfg": "{ ... }",
 *   "filename": "check_gpu.tscfg",
 *   "host_included": false,
 *   "host_id": "agx0"               // 依赖的主机 ID
 * }
 */
static esp_err_t api_ssh_commands_export(const cJSON *params, ts_api_result_t *result)
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
    
    /* 检查导出权限 */
    if (!ts_config_pack_can_export()) {
        ts_api_result_reason(result, TS_API_ERR_NO_PERMISSION, "pack_not_allowed", NULL);
        return ESP_OK;
    }
    
    const cJSON *include_host = cJSON_GetObjectItem(params, "include_host");
    bool do_include_host = include_host && cJSON_IsTrue(include_host);
    
    /* 获取指令配置 */
    ts_ssh_command_config_t cmd_config;
    esp_err_t ret = ts_ssh_commands_config_get(id->valuestring, &cmd_config);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Command not found");
        return ESP_OK;
    }
    
    /* 构建导出内容 */
    cJSON *export_json = cJSON_CreateObject();
    cJSON_AddStringToObject(export_json, "type", "ssh_command");
    
    /* 指令配置 */
    cJSON *cmd_json = cJSON_CreateObject();
    cJSON_AddStringToObject(cmd_json, "id", cmd_config.id);
    cJSON_AddStringToObject(cmd_json, "host_id", cmd_config.host_id);
    cJSON_AddStringToObject(cmd_json, "name", cmd_config.name);
    cJSON_AddStringToObject(cmd_json, "command", cmd_config.command);
    if (cmd_config.desc[0]) cJSON_AddStringToObject(cmd_json, "desc", cmd_config.desc);
    if (cmd_config.icon[0]) cJSON_AddStringToObject(cmd_json, "icon", cmd_config.icon);
    if (cmd_config.expect_pattern[0]) cJSON_AddStringToObject(cmd_json, "expectPattern", cmd_config.expect_pattern);
    if (cmd_config.fail_pattern[0]) cJSON_AddStringToObject(cmd_json, "failPattern", cmd_config.fail_pattern);
    if (cmd_config.extract_pattern[0]) cJSON_AddStringToObject(cmd_json, "extractPattern", cmd_config.extract_pattern);
    if (cmd_config.var_name[0]) cJSON_AddStringToObject(cmd_json, "varName", cmd_config.var_name);
    cJSON_AddNumberToObject(cmd_json, "timeout", cmd_config.timeout_sec);
    cJSON_AddBoolToObject(cmd_json, "stopOnMatch", cmd_config.stop_on_match);
    cJSON_AddBoolToObject(cmd_json, "nohup", cmd_config.nohup);
    cJSON_AddBoolToObject(cmd_json, "enabled", cmd_config.enabled);
    if (cmd_config.service_mode) {
        cJSON_AddBoolToObject(cmd_json, "serviceMode", true);
        if (cmd_config.ready_pattern[0]) cJSON_AddStringToObject(cmd_json, "readyPattern", cmd_config.ready_pattern);
        if (cmd_config.service_fail_pattern[0]) cJSON_AddStringToObject(cmd_json, "serviceFailPattern", cmd_config.service_fail_pattern);
        if (cmd_config.ready_timeout_sec > 0) cJSON_AddNumberToObject(cmd_json, "readyTimeout", cmd_config.ready_timeout_sec);
        if (cmd_config.ready_check_interval_ms > 0) cJSON_AddNumberToObject(cmd_json, "readyInterval", cmd_config.ready_check_interval_ms);
    }
    cJSON_AddItemToObject(export_json, "command", cmd_json);
    
    /* 可选包含主机配置 */
    bool host_included = false;
    if (do_include_host && cmd_config.host_id[0]) {
        ts_ssh_host_config_t host_config;
        if (ts_ssh_hosts_config_get(cmd_config.host_id, &host_config) == ESP_OK) {
            cJSON *host_json = cJSON_CreateObject();
            cJSON_AddStringToObject(host_json, "id", host_config.id);
            cJSON_AddStringToObject(host_json, "host", host_config.host);
            cJSON_AddNumberToObject(host_json, "port", host_config.port);
            cJSON_AddStringToObject(host_json, "username", host_config.username);
            cJSON_AddStringToObject(host_json, "auth_type", 
                host_config.auth_type == TS_SSH_HOST_AUTH_KEY ? "key" : "password");
            if (host_config.keyid[0]) {
                cJSON_AddStringToObject(host_json, "keyid", host_config.keyid);
            }
            cJSON_AddItemToObject(export_json, "host", host_json);
            host_included = true;
        }
    }
    
    char *json_str = cJSON_PrintUnformatted(export_json);
    cJSON_Delete(export_json);
    if (!json_str) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
        return ESP_OK;
    }
    
    /* 获取接收方证书 */
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
            ts_api_result_error(result, TS_API_ERR_NO_MEM, "no_memory");
            return ESP_OK;
        }
        cert_len = 4096;
        ret = ts_cert_get_certificate(cert_pem, &cert_len);
        if (ret != ESP_OK) {
            free(cert_pem);
            cJSON_free(json_str);
            ts_api_result_reason(result, TS_API_ERR_INTERNAL, "device_cert_unavailable", esp_err_to_name(ret));
            return ESP_OK;
        }
        free_cert = true;
    }
    
    /* 创建加密配置包 */
    ts_config_pack_export_opts_t opts = {
        .recipient_cert_pem = cert_pem,
        .recipient_cert_len = cert_len,
        .description = host_included ? "SSH command with host" : "SSH command configuration"
    };
    
    char *tscfg_output = NULL;
    size_t tscfg_len = 0;
    
    ts_config_pack_result_t pack_result = ts_config_pack_create(
        cmd_config.id, json_str, strlen(json_str), &opts, &tscfg_output, &tscfg_len);
    
    cJSON_free(json_str);
    if (free_cert) free(cert_pem);
    
    if (pack_result != TS_CONFIG_PACK_OK) {
        pack_failure(result, pack_result);
        return ESP_OK;
    }
    
    /* 返回结果 */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "tscfg", tscfg_output);
    
    char filename[64];
    snprintf(filename, sizeof(filename), "%s.tscfg", cmd_config.id);
    cJSON_AddStringToObject(data, "filename", filename);
    cJSON_AddBoolToObject(data, "host_included", host_included);
    cJSON_AddStringToObject(data, "host_id", cmd_config.host_id);
    
    free(tscfg_output);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief ssh.commands.import - 导入 SSH 指令配置（从 .tscfg）
 * 
 * Params: {
 *   "tscfg": "{ ... }",
 *   "overwrite": false,    // 可选，是否覆盖已存在的指令
 *   "import_host": true,   // 可选，是否同时导入包含的主机配置
 *   "target_host_id": ""   // 可选，强制使用指定的主机 ID（覆盖配置中的 host_id）
 * }
 * 
 * Response: {
 *   "id": "check_gpu",
 *   "imported": true,
 *   "host_imported": false,
 *   "host_missing": false,  // 如果依赖的主机不存在且未包含
 *   "host_id": "agx0"
 * }
 */
static esp_err_t api_ssh_commands_import(const cJSON *params,ts_api_result_t *result)
{ return ts_api_auto_pack_import(TS_AUTO_CONFIG_COMMAND,params,result); }

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_api_service_control(const cJSON *params, ts_api_result_t *result, ts_service_operation_t kind) {
    const char *field=kind==TS_SERVICE_START?"command_id":"run_id";
    const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params,field));
    if(!id||!id[0]){
        ts_api_result_error(result,TS_API_ERR_INVALID_ARG,kind==TS_SERVICE_START?"command_id_required":"run_id_required");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *data=cJSON_CreateObject();if(!data)return ESP_ERR_NO_MEM;
    cJSON *operation=cJSON_AddNumberToObject(data,"operation_id",0);
    cJSON *run=cJSON_AddStringToObject(data,"run_id","0000000000000000000000000000000");
    if(!operation||!run){cJSON_Delete(data);return ESP_ERR_NO_MEM;}
    uint32_t operation_id=0;char run_id[TS_SSH_RUN_ID_MAX];
    const char *reason=NULL;
    esp_err_t ret=ts_action_service_control(id,kind,run_id,&operation_id,&reason);
    if(ret!=ESP_OK){
        cJSON_Delete(data);
        ts_api_result_error(result,ret==ESP_ERR_INVALID_STATE?TS_API_ERR_BUSY:
            ret==ESP_ERR_NO_MEM?TS_API_ERR_NO_MEM:ret==ESP_ERR_NOT_FOUND?TS_API_ERR_NOT_FOUND:
            ret==ESP_ERR_INVALID_ARG?TS_API_ERR_INVALID_ARG:ret==ESP_ERR_NOT_SUPPORTED?TS_API_ERR_NOT_SUPPORTED:TS_API_ERR_INTERNAL,reason);
        return ret;
    }
    cJSON_SetNumberValue(operation,operation_id);strcpy(run->valuestring,run_id);
    ts_api_result_ok(result,data);return ESP_OK;
}

static esp_err_t api_ssh_service_start(const cJSON *params, ts_api_result_t *result) {
    ts_auto_config_error_t error={0};
    esp_err_t ret=ts_auto_config_edit_begin(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(params,"active_version")),&error);
    if(ret!=ESP_OK){ts_api_result_error(result,TS_API_ERR_INVALID_ARG,error.reason);return ESP_OK;}
    ret=ts_api_service_control(params,result,TS_SERVICE_START);
    ts_auto_config_edit_end();return ret;
}

static const ts_api_endpoint_t ssh_endpoints[] = {
    {.name = "ssh.services.start", .description = "Start a stored managed service",
     .category = TS_API_CAT_SECURITY, .handler = api_ssh_service_start, .requires_auth = true},
    {
        .name = "ssh.exec",
        .description = "Execute remote command via SSH",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_exec,
        .requires_auth = true,
    },
    {
        .name = "ssh.exec_stream",
        .description = "Execute remote command with streaming output via WebSocket",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_exec_stream,
        .requires_auth = true,
    },
    {
        .name = "ssh.cancel",
        .description = "Cancel running SSH exec session",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_cancel,
        .requires_auth = false,
    },
    {
        .name = "ssh.test",
        .description = "Test SSH connection",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_test,
        .requires_auth = true,
    },
    {
        .name = "ssh.copyid",
        .description = "Deploy public key to remote server",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_copyid,
        .requires_auth = true,
    },
    {
        .name = "ssh.revoke",
        .description = "Revoke (remove) deployed public key from remote server",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_revoke,
        .requires_auth = true,
    },
    {
        .name = "ssh.keygen",
        .description = "Generate SSH key pair",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_keygen,
        .requires_auth = true,
    },
    /* SSH Host Config APIs */
    {
        .name = "ssh.hosts.list",
        .description = "List all SSH host configurations",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_hosts_list,
        .requires_auth = false,
    },
    {
        .name = "ssh.hosts.add",
        .description = "Add SSH host configuration",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_hosts_add,
        .requires_auth = true,
    },
    {
        .name = "ssh.hosts.remove",
        .description = "Remove SSH host configuration",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_hosts_remove,
        .requires_auth = true,
    },
    {
        .name = "ssh.hosts.get",
        .description = "Get SSH host configuration by ID",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_hosts_get,
        .requires_auth = false,
    },
    {
        .name = "ssh.hosts.export",
        .description = "Export SSH host configuration as .tscfg",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_hosts_export,
        .requires_auth = true,
    },
    {
        .name = "ssh.hosts.import",
        .description = "Import SSH host configuration from .tscfg",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_hosts_import,
        .requires_auth = true,
    },
    /* SSH Command Config APIs */
    {
        .name = "ssh.commands.list",
        .description = "List all SSH command configurations",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_commands_list,
        .requires_auth = false,
    },
    {
        .name = "ssh.commands.add",
        .description = "Add or update SSH command configuration",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_commands_add,
        .requires_auth = true,
    },
    {
        .name = "ssh.commands.remove",
        .description = "Remove SSH command configuration",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_commands_remove,
        .requires_auth = true,
    },
    {
        .name = "ssh.commands.get",
        .description = "Get SSH command configuration by ID",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_commands_get,
        .requires_auth = false,
    },
    {
        .name = "ssh.commands.export",
        .description = "Export SSH command configuration as .tscfg",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_commands_export,
        .requires_auth = true,
    },
    {
        .name = "ssh.commands.import",
        .description = "Import SSH command configuration from .tscfg",
        .category = TS_API_CAT_SECURITY,
        .handler = api_ssh_commands_import,
        .requires_auth = true,
    },
};

esp_err_t ts_api_ssh_register(void)
{
    TS_LOGI(TAG, "Registering SSH APIs...");
    return ts_api_register_multiple(ssh_endpoints, 
                                     sizeof(ssh_endpoints) / sizeof(ssh_endpoints[0]));
}
