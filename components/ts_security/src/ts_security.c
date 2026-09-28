#include "ts_ssh_log_watch.h"
#include "ts_ssh_service.h"
/**
 * @file ts_security.c
 * @brief Security Subsystem Implementation
 */

#include "ts_security.h"
#include "ts_crypto.h"
#include "ts_ssh_hosts_config.h"
#include "ts_ssh_commands_config.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM */
#include "ts_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <string.h>

#define TAG "ts_security"
#define NVS_NAMESPACE "ts_security"
#define MAX_SESSIONS 8

typedef struct {
    bool active;
    ts_session_t session;
} session_slot_t;

static nvs_handle_t s_nvs;
static session_slot_t s_sessions[MAX_SESSIONS];
static bool s_initialized = false;

esp_err_t ts_security_init(void)
{
    if (s_initialized) return ESP_OK;
    
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS namespace");
        return ret;
    }
    
    memset(s_sessions, 0, sizeof(s_sessions));
    
    // 初始化 SSH 主机配置模块
    ret = ts_ssh_hosts_config_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Failed to init SSH hosts config: %s", esp_err_to_name(ret));
        // 非致命错误，继续运行
    }
    
    // 初始化 SSH 指令配置模块
    ret = ts_ssh_commands_config_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Failed to init SSH commands config: %s", esp_err_to_name(ret));
        // 非致命错误，继续运行
    }
    
    ret = ts_ssh_service_init();
    if (ret != ESP_OK) return ret;
    ret = ts_ssh_log_watch_init();
    if (ret != ESP_OK) return ret;
    s_initialized = true;
    TS_LOGI(TAG, "Security subsystem initialized");
    return ESP_OK;
}

esp_err_t ts_security_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    
    nvs_close(s_nvs);
    s_initialized = false;
    return ESP_OK;
}

esp_err_t ts_security_random(void *buf, size_t len)
{
    if (!buf) return ESP_ERR_INVALID_ARG;
    esp_fill_random(buf, len);
    return ESP_OK;
}

esp_err_t ts_security_generate_key(const char *name, ts_key_type_t type)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    
    size_t key_len;
    switch (type) {
        case TS_KEY_TYPE_AES128: key_len = 16; break;
        case TS_KEY_TYPE_AES256: key_len = 32; break;
        default:
            TS_LOGW(TAG, "Key type %d not yet implemented", type);
            return ESP_ERR_NOT_SUPPORTED;
    }
    
    uint8_t *key = TS_MALLOC_PSRAM(key_len);
    if (!key) return ESP_ERR_NO_MEM;
    
    ts_security_random(key, key_len);
    
    esp_err_t ret = ts_security_store_key(name, key, key_len);
    
    // Clear key from memory
    memset(key, 0, key_len);
    free(key);
    
    return ret;
}

esp_err_t ts_security_load_key(const char *name, void *key, size_t *key_len)
{
    if (!name || !key || !key_len) return ESP_ERR_INVALID_ARG;
    
    char nvs_key[32];
    snprintf(nvs_key, sizeof(nvs_key), "key_%s", name);
    
    return nvs_get_blob(s_nvs, nvs_key, key, key_len);
}

esp_err_t ts_security_store_key(const char *name, const void *key, size_t key_len)
{
    if (!name || !key) return ESP_ERR_INVALID_ARG;
    
    char nvs_key[32];
    snprintf(nvs_key, sizeof(nvs_key), "key_%s", name);
    
    esp_err_t ret = nvs_set_blob(s_nvs, nvs_key, key, key_len);
    if (ret == ESP_OK) {
        nvs_commit(s_nvs);
    }
    return ret;
}

esp_err_t ts_security_delete_key(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    
    char nvs_key[32];
    snprintf(nvs_key, sizeof(nvs_key), "key_%s", name);
    
    esp_err_t ret = nvs_erase_key(s_nvs, nvs_key);
    if (ret == ESP_OK) {
        nvs_commit(s_nvs);
    }
    return ret;
}

esp_err_t ts_security_load_cert(const char *name, ts_cert_type_t type,
                                 void *cert, size_t *cert_len)
{
    if (!name || !cert || !cert_len) return ESP_ERR_INVALID_ARG;
    
    char nvs_key[32];
    snprintf(nvs_key, sizeof(nvs_key), "cert_%s", name);
    
    return nvs_get_blob(s_nvs, nvs_key, cert, cert_len);
}

esp_err_t ts_security_store_cert(const char *name, ts_cert_type_t type,
                                  const void *cert, size_t cert_len)
{
    if (!name || !cert) return ESP_ERR_INVALID_ARG;
    
    char nvs_key[32];
    snprintf(nvs_key, sizeof(nvs_key), "cert_%s", name);
    
    esp_err_t ret = nvs_set_blob(s_nvs, nvs_key, cert, cert_len);
    if (ret == ESP_OK) {
        nvs_commit(s_nvs);
    }
    return ret;
}

/**
 * @brief 驱逐指定 client_id 的所有会话（释放槽位）
 * @note 仅内部使用，在 create_session 槽位满时按同用户驱逐
 */
static void destroy_sessions_by_client(const char *client_id)
{
    if (!client_id || client_id[0] == '\0') return;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].active &&
            strcmp(s_sessions[i].session.client_id, client_id) == 0) {
            s_sessions[i].active = false;
            TS_LOGI(TAG, "Evicted session for client: %s", client_id);
        }
    }
}

esp_err_t ts_security_create_session(const char *client_id, ts_perm_level_t level,
                                      uint32_t *session_id)
{
    if (!session_id) return ESP_ERR_INVALID_ARG;
    
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sessions[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        if (client_id && client_id[0] != '\0') {
            destroy_sessions_by_client(client_id);
            for (int i = 0; i < MAX_SESSIONS; i++) {
                if (!s_sessions[i].active) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot < 0) {
            TS_LOGW(TAG, "No free session slots");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Generate session ID
    uint32_t sid;
    ts_security_random(&sid, sizeof(sid));
    
    uint32_t now = esp_timer_get_time() / 1000000;
    
    s_sessions[slot].active = true;
    s_sessions[slot].session.session_id = sid;
    s_sessions[slot].session.level = level;
    s_sessions[slot].session.created_at = now;
#ifdef CONFIG_TS_SECURITY_TOKEN_EXPIRE_SEC
    s_sessions[slot].session.expires_at = now + CONFIG_TS_SECURITY_TOKEN_EXPIRE_SEC;
#else
    s_sessions[slot].session.expires_at = now + 3600;
#endif
    
    if (client_id) {
        strncpy(s_sessions[slot].session.client_id, client_id, 
                sizeof(s_sessions[slot].session.client_id) - 1);
    }
    
    *session_id = sid;
    TS_LOGI(TAG, "Session created: %08lx", (unsigned long)sid);
    return ESP_OK;
}

esp_err_t ts_security_validate_session(uint32_t session_id, ts_session_t *session)
{
    uint32_t now = esp_timer_get_time() / 1000000;
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].active && s_sessions[i].session.session_id == session_id) {
            if (now > s_sessions[i].session.expires_at) {
                s_sessions[i].active = false;
                return ESP_ERR_TIMEOUT;
            }
            if (session) {
                *session = s_sessions[i].session;
            }
            return ESP_OK;
        }
    }
    
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ts_security_destroy_session(uint32_t session_id)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].active && s_sessions[i].session.session_id == session_id) {
            s_sessions[i].active = false;
            TS_LOGI(TAG, "Session destroyed: %08lx", (unsigned long)session_id);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

bool ts_security_check_permission(uint32_t session_id, ts_perm_level_t required)
{
    ts_session_t session;
    if (ts_security_validate_session(session_id, &session) != ESP_OK) {
        return false;
    }
    return session.level >= required;
}

esp_err_t ts_security_generate_token(uint32_t session_id, char *token, size_t max_len)
{
    if (!token || max_len < 64) return ESP_ERR_INVALID_ARG;
    
    ts_session_t session;
    esp_err_t ret = ts_security_validate_session(session_id, &session);
    if (ret != ESP_OK) return ret;
    
    // Simple token: base64(session_id + random + timestamp)
    uint8_t data[16];
    memcpy(data, &session_id, 4);
    ts_security_random(data + 4, 8);
    uint32_t ts = esp_timer_get_time() / 1000;
    memcpy(data + 12, &ts, 4);
    
    size_t out_len = max_len;
    return ts_crypto_base64_encode(data, sizeof(data), token, &out_len);
}

esp_err_t ts_security_validate_token(const char *token, uint32_t *session_id)
{
    if (!token || !session_id) return ESP_ERR_INVALID_ARG;
    
    uint8_t data[16];
    size_t data_len = sizeof(data);
    
    esp_err_t ret = ts_crypto_base64_decode(token, strlen(token), data, &data_len);
    if (ret != ESP_OK || data_len < 4) return ESP_ERR_INVALID_ARG;
    
    uint32_t sid;
    memcpy(&sid, data, 4);
    
    ret = ts_security_validate_session(sid, NULL);
    if (ret == ESP_OK) {
        *session_id = sid;
    }
    return ret;
}
