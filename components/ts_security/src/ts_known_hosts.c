/**
 * @file ts_known_hosts.c
 * @brief SSH Known Hosts Management Implementation
 *
 * 已知主机管理实现，用于存储和验证 SSH 服务器指纹，
 * 防止中间人攻击。主机指纹仅存储在系统 NVS 中。
 * 
 */

#include "ts_known_hosts.h"
#include "ts_ssh_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <libssh2.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "ts_known_hosts";

/* ============================================================================
 * 内部常量和数据结构
 * ============================================================================ */

#define NVS_NAMESPACE       "ts_ssh_hosts"
#define MAX_KNOWN_HOSTS     32
#define HOST_KEY_PREFIX     "host_"

#define MAX_HOST_LEN        64
#define MAX_FINGERPRINT_LEN 65  /* SHA256 hex = 64 chars + null terminator */

/** 内部存储格式（包含原始主机名以便列表显示） */
typedef struct {
    char host[MAX_HOST_LEN];        /**< 原始主机名/IP */
    uint16_t port;
    uint8_t type;
    uint32_t added_time;
    char fingerprint[MAX_FINGERPRINT_LEN];  /**< SHA256 fingerprint (64 hex + \0) */
} stored_host_t;

/** 模块状态 */
static struct {
    bool initialized;
    nvs_handle_t nvs;
    SemaphoreHandle_t mutex;
} s_state = {0};


/* ============================================================================
 * 外部声明 - 来自 ts_ssh_client.c
 * ============================================================================ */

extern LIBSSH2_SESSION *ts_ssh_get_libssh2_session(ts_ssh_session_t session);

/* ============================================================================
 * 内部辅助函数
 * ============================================================================ */

/**
 * @brief 简单哈希函数（djb2 算法）
 */
static uint32_t simple_hash(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

/**
 * @brief 生成 NVS 键名
 * 
 * NVS 键名最大 15 字符，使用 "h_" 前缀 + 哈希值
 * 格式: h_XXXXXXXX (10 字符)
 */
static void make_nvs_key(const char *host, uint16_t port, char *key, size_t key_size)
{
    /* 组合主机名和端口生成哈希 */
    char combined[128];
    snprintf(combined, sizeof(combined), "%s:%u", host, port);
    
    uint32_t hash = simple_hash(combined);
    
    /* 生成短键名: h_XXXXXXXX (10字符，符合NVS 15字符限制) */
    snprintf(key, key_size, "h_%08lx", (unsigned long)hash);
}

/**
 * @brief 将字节转换为十六进制字符串
 */
static void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex)
{
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex[i * 2] = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex[len * 2] = '\0';
}

/**
 * @brief 从 libssh2 获取主机密钥类型
 */
static ts_host_key_type_t get_key_type(int libssh2_type)
{
    switch (libssh2_type) {
        case LIBSSH2_HOSTKEY_TYPE_RSA:
            return TS_HOST_KEY_RSA;
        case LIBSSH2_HOSTKEY_TYPE_DSS:
            return TS_HOST_KEY_DSS;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
            return TS_HOST_KEY_ECDSA_256;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
            return TS_HOST_KEY_ECDSA_384;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
            return TS_HOST_KEY_ECDSA_521;
        case LIBSSH2_HOSTKEY_TYPE_ED25519:
            return TS_HOST_KEY_ED25519;
        default:
            return TS_HOST_KEY_UNKNOWN;
    }
}

/* ============================================================================
 * 初始化和清理
 * ============================================================================ */

esp_err_t ts_known_hosts_init(void)
{
    if (s_state.initialized) {
        return ESP_OK;
    }
    
    /* 创建互斥锁 */
    s_state.mutex = xSemaphoreCreateMutex();
    if (!s_state.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    
    /* 打开 NVS 命名空间 */
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_state.nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_state.mutex);
        return ret;
    }
    
    s_state.initialized = true;
    ESP_LOGI(TAG, "Known hosts module initialized");
    
    return ESP_OK;
}

esp_err_t ts_known_hosts_deinit(void)
{
    if (!s_state.initialized) {
        return ESP_OK;
    }
    
    nvs_close(s_state.nvs);
    vSemaphoreDelete(s_state.mutex);
    s_state.initialized = false;
    
    return ESP_OK;
}

/* ============================================================================
 * 主机验证
 * ============================================================================ */

esp_err_t ts_known_hosts_get_fingerprint(ts_ssh_session_t session,
                                          char *fingerprint,
                                          ts_host_key_type_t *type)
{
    if (!session || !fingerprint) {
        return ESP_ERR_INVALID_ARG;
    }
    
    LIBSSH2_SESSION *lsession = ts_ssh_get_libssh2_session(session);
    if (!lsession) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 获取主机密钥 */
    size_t key_len;
    int key_type;
    const char *hostkey = libssh2_session_hostkey(lsession, &key_len, &key_type);
    
    if (!hostkey) {
        ESP_LOGE(TAG, "Failed to get host key");
        return ESP_FAIL;
    }
    
    /* 计算 SHA256 指纹 */
    const char *hash = libssh2_hostkey_hash(lsession, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash) {
        ESP_LOGE(TAG, "Failed to get host key hash");
        return ESP_FAIL;
    }
    
    /* 转换为十六进制字符串 */
    bytes_to_hex((const uint8_t *)hash, 32, fingerprint);
    
    if (type) {
        *type = get_key_type(key_type);
    }
    
    return ESP_OK;
}

esp_err_t ts_known_hosts_verify(ts_ssh_session_t session,
                                 ts_host_verify_result_t *result,
                                 ts_known_host_t *host_info)
{
    if (!session || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_state.initialized) {
        esp_err_t ret = ts_known_hosts_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    /* 获取当前连接的指纹 */
    char current_fp[65];
    ts_host_key_type_t key_type;
    esp_err_t ret = ts_known_hosts_get_fingerprint(session, current_fp, &key_type);
    if (ret != ESP_OK) {
        *result = TS_HOST_VERIFY_ERROR;
        return ret;
    }
    
    /* 获取主机信息 */
    const char *host = ts_ssh_get_host(session);
    uint16_t port = ts_ssh_get_port(session);
    
    if (!host) {
        *result = TS_HOST_VERIFY_ERROR;
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 填充 host_info */
    if (host_info) {
        strncpy(host_info->host, host, sizeof(host_info->host) - 1);
        host_info->port = port;
        host_info->type = key_type;
        strncpy(host_info->fingerprint, current_fp, sizeof(host_info->fingerprint) - 1);
        host_info->added_time = 0;
    }
    
    /* 查找存储的主机密钥 */
    char nvs_key[32];
    make_nvs_key(host, port, nvs_key, sizeof(nvs_key));
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    stored_host_t stored;
    size_t len = sizeof(stored);
    ret = nvs_get_blob(s_state.nvs, nvs_key, &stored, &len);
    
    xSemaphoreGive(s_state.mutex);
    
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* 未知主机 */
        *result = TS_HOST_VERIFY_NOT_FOUND;
        ESP_LOGW(TAG, "Unknown host: %s:%u", host, port);
        return ESP_OK;
    } else if (ret != ESP_OK) {
        *result = TS_HOST_VERIFY_ERROR;
        return ret;
    }
    
    /* 比较指纹 */
    if (strcmp(current_fp, stored.fingerprint) == 0) {
        *result = TS_HOST_VERIFY_OK;
        if (host_info) {
            host_info->added_time = stored.added_time;
        }
        ESP_LOGI(TAG, "Host key verified: %s:%u", host, port);
        return ESP_OK;
    } else {
        /* 指纹不匹配 - 可能的中间人攻击！ */
        *result = TS_HOST_VERIFY_MISMATCH;
        ESP_LOGE(TAG, "HOST KEY MISMATCH for %s:%u!", host, port);
        ESP_LOGE(TAG, "Expected: %s", stored.fingerprint);
        ESP_LOGE(TAG, "Got:      %s", current_fp);
        return ESP_OK;
    }
}

esp_err_t ts_known_hosts_verify_interactive(ts_ssh_session_t session,
                                             ts_host_prompt_cb_t prompt_cb,
                                             void *user_data)
{
    ts_host_verify_result_t result;
    ts_known_host_t host_info;
    
    esp_err_t ret = ts_known_hosts_verify(session, &result, &host_info);
    if (ret != ESP_OK) {
        return ret;
    }
    
    switch (result) {
        case TS_HOST_VERIFY_OK:
            /* 验证通过 */
            return ESP_OK;
            
        case TS_HOST_VERIFY_NOT_FOUND:
        case TS_HOST_VERIFY_MISMATCH:
            /* 需要用户确认 */
            if (prompt_cb) {
                if (prompt_cb(&host_info, result, user_data)) {
                    /* 用户接受，添加/更新主机密钥 */
                    return ts_known_hosts_add(session);
                } else {
                    /* 用户拒绝 */
                    return ESP_ERR_INVALID_STATE;
                }
            }
            /* 无回调，默认拒绝 */
            return ESP_ERR_INVALID_STATE;
            
        default:
            return ESP_FAIL;
    }
}

/* ============================================================================
 * 主机管理
 * ============================================================================ */

esp_err_t ts_known_hosts_add(ts_ssh_session_t session)
{
    if (!session) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_state.initialized) {
        esp_err_t ret = ts_known_hosts_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    /* 获取指纹和类型 */
    char fingerprint[65];
    ts_host_key_type_t key_type;
    esp_err_t ret = ts_known_hosts_get_fingerprint(session, fingerprint, &key_type);
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 获取主机信息 */
    const char *host = ts_ssh_get_host(session);
    uint16_t port = ts_ssh_get_port(session);
    
    return ts_known_hosts_add_manual(host, port, fingerprint, key_type);
}

esp_err_t ts_known_hosts_add_manual(const char *host, uint16_t port,
                                     const char *fingerprint,
                                     ts_host_key_type_t type)
{
    if (!host || !fingerprint) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_state.initialized) {
        esp_err_t ret = ts_known_hosts_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    /* 准备存储数据 */
    stored_host_t stored = {
        .port = port,
        .type = (uint8_t)type,
        .added_time = (uint32_t)time(NULL)
    };
    strncpy(stored.host, host, sizeof(stored.host) - 1);
    stored.host[sizeof(stored.host) - 1] = '\0';
    strncpy(stored.fingerprint, fingerprint, sizeof(stored.fingerprint) - 1);
    stored.fingerprint[sizeof(stored.fingerprint) - 1] = '\0';
    
    /* 生成键名 */
    char nvs_key[32];
    make_nvs_key(host, port, nvs_key, sizeof(nvs_key));
    
    /* 存储到 NVS */
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    esp_err_t ret = nvs_set_blob(s_state.nvs, nvs_key, &stored, sizeof(stored));
    if (ret == ESP_OK) {
        ret = nvs_commit(s_state.nvs);
    }
    
    xSemaphoreGive(s_state.mutex);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Added known host: %s:%u (%s)", host, port, 
                 ts_host_key_type_str(type));
        
    } else {
        ESP_LOGE(TAG, "Failed to add host: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t ts_known_hosts_remove(const char *host, uint16_t port)
{
    if (!host) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    char nvs_key[32];
    make_nvs_key(host, port, nvs_key, sizeof(nvs_key));
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    esp_err_t ret = nvs_erase_key(s_state.nvs, nvs_key);
    if (ret == ESP_OK) {
        ret = nvs_commit(s_state.nvs);
    }
    
    xSemaphoreGive(s_state.mutex);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Removed known host: %s:%u", host, port);
        
    }
    
    return ret;
}

esp_err_t ts_known_hosts_get(const char *host, uint16_t port,
                              ts_known_host_t *info)
{
    if (!host || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    char nvs_key[32];
    make_nvs_key(host, port, nvs_key, sizeof(nvs_key));
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    stored_host_t stored;
    size_t len = sizeof(stored);
    esp_err_t ret = nvs_get_blob(s_state.nvs, nvs_key, &stored, &len);
    
    xSemaphoreGive(s_state.mutex);
    
    if (ret == ESP_OK) {
        strncpy(info->host, host, sizeof(info->host) - 1);
        info->port = port;
        info->type = (ts_host_key_type_t)stored.type;
        strncpy(info->fingerprint, stored.fingerprint, sizeof(info->fingerprint) - 1);
        info->added_time = stored.added_time;
    }
    
    return ret;
}

esp_err_t ts_known_hosts_list(ts_known_host_t *hosts, size_t max_hosts,
                               size_t *count)
{
    if (!hosts || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_state.initialized) {
        ESP_LOGW(TAG, "Module not initialized");
        *count = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Listing known hosts, max_hosts=%zu", max_hosts);
    
    *count = 0;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    nvs_iterator_t iter = NULL;
    esp_err_t ret = nvs_entry_find_in_handle(s_state.nvs, NVS_TYPE_BLOB, &iter);
    
    ESP_LOGI(TAG, "NVS iterator result: %s", esp_err_to_name(ret));
    
    while (ret == ESP_OK && *count < max_hosts) {
        nvs_entry_info_t info;
        nvs_entry_info(iter, &info);
        
        ESP_LOGI(TAG, "Found NVS entry: key='%s', type=%d", info.key, info.type);
        
        /* 检查是否是主机密钥条目（键名以 "h_" 开头） */
        if (strncmp(info.key, "h_", 2) == 0) {
            stored_host_t stored;
            size_t len = sizeof(stored);
            
            esp_err_t get_ret = nvs_get_blob(s_state.nvs, info.key, &stored, &len);
            ESP_LOGI(TAG, "Reading key '%s': %s, len=%zu", info.key, esp_err_to_name(get_ret), len);
            
            if (get_ret == ESP_OK) {
                /* 从存储的数据中读取主机信息 */
                strncpy(hosts[*count].host, stored.host, sizeof(hosts[*count].host) - 1);
                hosts[*count].host[sizeof(hosts[*count].host) - 1] = '\0';
                hosts[*count].port = stored.port;
                hosts[*count].type = (ts_host_key_type_t)stored.type;
                strncpy(hosts[*count].fingerprint, stored.fingerprint,
                        sizeof(hosts[*count].fingerprint) - 1);
                hosts[*count].fingerprint[sizeof(hosts[*count].fingerprint) - 1] = '\0';
                hosts[*count].added_time = stored.added_time;
                
                ESP_LOGI(TAG, "Added host[%zu]: %s:%u", *count, stored.host, stored.port);
                (*count)++;
            }
        }
        
        ret = nvs_entry_next(&iter);
    }
    
    if (iter) {
        nvs_release_iterator(iter);
    }
    
    xSemaphoreGive(s_state.mutex);
    
    ESP_LOGI(TAG, "Returning %zu known hosts", *count);
    
    return ESP_OK;
}

esp_err_t ts_known_hosts_clear(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    esp_err_t ret = nvs_erase_all(s_state.nvs);
    if (ret == ESP_OK) {
        ret = nvs_commit(s_state.nvs);
    }
    
    xSemaphoreGive(s_state.mutex);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Cleared all known hosts");
    }
    
    return ret;
}

size_t ts_known_hosts_count(void)
{
    if (!s_state.initialized) {
        return 0;
    }
    
    size_t count = 0;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    nvs_iterator_t iter = NULL;
    esp_err_t ret = nvs_entry_find_in_handle(s_state.nvs, NVS_TYPE_BLOB, &iter);
    
    while (ret == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(iter, &info);
        
        if (strncmp(info.key, HOST_KEY_PREFIX, strlen(HOST_KEY_PREFIX)) == 0) {
            count++;
        }
        
        ret = nvs_entry_next(&iter);
    }
    
    if (iter) {
        nvs_release_iterator(iter);
    }
    
    xSemaphoreGive(s_state.mutex);
    
    return count;
}

/* ============================================================================
 * 工具函数
 * ============================================================================ */

const char *ts_host_key_type_str(ts_host_key_type_t type)
{
    switch (type) {
        case TS_HOST_KEY_RSA:        return "RSA";
        case TS_HOST_KEY_DSS:        return "DSS";
        case TS_HOST_KEY_ECDSA_256:  return "ECDSA-256";
        case TS_HOST_KEY_ECDSA_384:  return "ECDSA-384";
        case TS_HOST_KEY_ECDSA_521:  return "ECDSA-521";
        case TS_HOST_KEY_ED25519:    return "ED25519";
        default:                     return "Unknown";
    }
}
