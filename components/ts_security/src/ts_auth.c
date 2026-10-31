/**
 * @file ts_auth.c
 * @brief Authentication Helpers - 用户认证与会话管理
 * 
 * 支持 admin 和 root 两个用户，密码使用 SHA256+salt 哈希存储于 NVS
 * 
 * 安全设计：
 * - 密码哈希仅存储在 NVS，不导出到 SD 卡
 * - admin 密码可由 root 会话恢复；root/admin 均可通过受控恢复固件恢复默认密码
 */

#include "ts_security.h"
#include "ts_auth_layout.h"
#include "ts_crypto.h"
#include "ts_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <string.h>

#define TAG "ts_auth"

#define MAX_LOGIN_ATTEMPTS    5
#define LOGIN_LOCKOUT_SEC     300  /* 5 分钟锁定 */

typedef ts_auth_user_credential_t user_credential_t;

static nvs_handle_t s_auth_nvs;
static bool s_auth_initialized = false;

/*===========================================================================*/
/*                          内部函数                                          */
/*===========================================================================*/

/**
 * @brief 计算密码哈希 (SHA256(salt + password))
 */
static esp_err_t compute_password_hash(const uint8_t *salt, const char *password,
                                        uint8_t *hash_out)
{
    /* 拼接 salt + password */
    size_t pwd_len = strlen(password);
    size_t total_len = TS_AUTH_SALT_LEN + pwd_len;
    uint8_t *buf = malloc(total_len);
    if (!buf) return ESP_ERR_NO_MEM;
    
    memcpy(buf, salt, TS_AUTH_SALT_LEN);
    memcpy(buf + TS_AUTH_SALT_LEN, password, pwd_len);
    
    esp_err_t ret = ts_crypto_hash(TS_HASH_SHA256, buf, total_len, hash_out, TS_AUTH_HASH_LEN);
    
    /* 清除敏感数据 */
    memset(buf, 0, total_len);
    free(buf);
    
    return ret;
}

/**
 * @brief 获取用户凭据
 */
static esp_err_t load_user_credential(const char *username, user_credential_t *cred)
{
    char key[32];
    snprintf(key, sizeof(key), "cred_%s", username);
    
    size_t len = sizeof(user_credential_t);
    esp_err_t ret = nvs_get_blob(s_auth_nvs, key, cred, &len);
    
    return ret;
}

/**
 * @brief 保存用户凭据
 */
static esp_err_t save_user_credential(const char *username, const user_credential_t *cred)
{
    char key[32];
    snprintf(key, sizeof(key), "cred_%s", username);
    
    esp_err_t ret = nvs_set_blob(s_auth_nvs, key, cred, sizeof(user_credential_t));
    if (ret == ESP_OK) {
        nvs_commit(s_auth_nvs);
    }
    return ret;
}

/**
 * @brief 写入用户密码凭据，并清除失败计数/锁定状态
 */
static esp_err_t write_user_password_credential(const char *username, const char *password,
                                                 bool password_changed)
{
    if (!username || !password) return ESP_ERR_INVALID_ARG;

    size_t pwd_len = strlen(password);
    if (pwd_len < 4 || pwd_len > 64) {
        TS_LOGW(TAG, "Password length invalid (4-64 chars required)");
        return ESP_ERR_INVALID_ARG;
    }

    user_credential_t cred = {0};

    esp_fill_random(cred.salt, TS_AUTH_SALT_LEN);

    esp_err_t ret = compute_password_hash(cred.salt, password, cred.hash);
    if (ret != ESP_OK) return ret;

    cred.password_changed = password_changed;
    cred.failed_attempts = 0;
    cred.lockout_until = 0;

    return save_user_credential(username, &cred);
}

/**
 * @brief 强制重新创建用户凭据
 */
static esp_err_t force_create_user(const char *username, ts_perm_level_t level)
{
    /* 根据用户类型使用不同默认密码 */
    const char *default_pwd = (level == TS_PERM_ROOT)
                                  ? TS_AUTH_DEFAULT_ROOT_PASSWORD
                                  : TS_AUTH_DEFAULT_ADMIN_PASSWORD;
    TS_LOGI(TAG, "Creating/resetting user '%s'", username);

    return write_user_password_credential(username, default_pwd, false);
}

/**
 * @brief 初始化用户（如果不存在则创建默认密码）
 */
static esp_err_t init_user_if_needed(const char *username, ts_perm_level_t level)
{
    user_credential_t cred;
    
    if (load_user_credential(username, &cred) == ESP_OK) {
        /* 开机计时相关的临时锁定不能沿用到本次启动。 */
        if (cred.failed_attempts || cred.lockout_until) {
            cred.failed_attempts = 0;
            cred.lockout_until = 0;
            return save_user_credential(username, &cred);
        }
        return ESP_OK;
    }
    
    return force_create_user(username, level);
}

/**
 * @brief 获取当前时间（秒）
 */
static uint32_t get_current_time_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/*===========================================================================*/
/*                          公开 API                                          */
/*===========================================================================*/

/**
 * @brief 初始化认证模块
 */
esp_err_t ts_auth_init(void)
{
    if (s_auth_initialized) return ESP_OK;
    
    esp_err_t ret = nvs_open(TS_AUTH_NVS_NAMESPACE, NVS_READWRITE, &s_auth_nvs);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 检查配置版本，版本变化时强制重置所有用户 */
    uint8_t stored_version = 0;
    nvs_get_u8(s_auth_nvs, TS_AUTH_CONFIG_VERSION_KEY, &stored_version);
    
    if (stored_version != TS_AUTH_CONFIG_VERSION) {
        TS_LOGW(TAG, "Auth config version changed (%d -> %d), resetting all users",
                stored_version, TS_AUTH_CONFIG_VERSION);
        /* 强制重新创建所有用户 */
        force_create_user("admin", TS_PERM_ADMIN);
        force_create_user("root", TS_PERM_ROOT);
        /* 保存新版本号 */
        nvs_set_u8(s_auth_nvs, TS_AUTH_CONFIG_VERSION_KEY, TS_AUTH_CONFIG_VERSION);
        nvs_commit(s_auth_nvs);
    } else {
        /* 初始化默认用户（如果不存在） */
        ret = init_user_if_needed("admin", TS_PERM_ADMIN);
        if (ret == ESP_OK) ret = init_user_if_needed("root", TS_PERM_ROOT);
        if (ret != ESP_OK) {
            nvs_close(s_auth_nvs);
            return ret;
        }
    }
    
    s_auth_initialized = true;
    TS_LOGI(TAG, "Auth module initialized (version %d)", TS_AUTH_CONFIG_VERSION);
    return ESP_OK;
}

/**
 * @brief 验证用户密码
 */
esp_err_t ts_auth_verify_password(const char *username, const char *password,
                                   ts_perm_level_t *level)
{
    if (!username || !password) return ESP_ERR_INVALID_ARG;
    
    /* 只允许 admin 和 root 用户 */
    ts_perm_level_t user_level;
    if (strcmp(username, "admin") == 0) {
        user_level = TS_PERM_ADMIN;
    } else if (strcmp(username, "root") == 0) {
        user_level = TS_PERM_ROOT;
    } else {
        TS_LOGW(TAG, "Unknown user: %s", username);
        return ESP_ERR_NOT_FOUND;
    }
    
    user_credential_t cred;
    esp_err_t ret = load_user_credential(username, &cred);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to load credential for %s", username);
        return ret;
    }
    
    /* 检查是否被锁定 */
    uint32_t now = get_current_time_sec();
    if (cred.lockout_until > now) {
        TS_LOGW(TAG, "User %s is locked out for %lu more seconds",
                username, (unsigned long)(cred.lockout_until - now));
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 计算输入密码的哈希 */
    uint8_t computed_hash[TS_AUTH_HASH_LEN];
    ret = compute_password_hash(cred.salt, password, computed_hash);
    if (ret != ESP_OK) return ret;
    
    /* 比较哈希（恒定时间比较防止时序攻击） */
    uint8_t diff = 0;
    for (int i = 0; i < TS_AUTH_HASH_LEN; i++) {
        diff |= computed_hash[i] ^ cred.hash[i];
    }
    
    /* 清除敏感数据 */
    memset(computed_hash, 0, sizeof(computed_hash));
    
    if (diff != 0) {
        /* 密码错误，增加失败计数 */
        cred.failed_attempts++;
        if (cred.failed_attempts >= MAX_LOGIN_ATTEMPTS) {
            cred.lockout_until = now + LOGIN_LOCKOUT_SEC;
            cred.failed_attempts = 0;
            TS_LOGW(TAG, "User %s locked out due to too many failed attempts", username);
        }
        save_user_credential(username, &cred);
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 密码正确，清除失败计数及已过期的锁定。 */
    if (cred.failed_attempts || cred.lockout_until) {
        cred.failed_attempts = 0;
        cred.lockout_until = 0;
        save_user_credential(username, &cred);
    }
    
    if (level) *level = user_level;
    return ESP_OK;
}

/**
 * @brief 修改密码
 */
esp_err_t ts_auth_change_password(const char *username, const char *old_password,
                                   const char *new_password)
{
    if (!username || !old_password || !new_password) return ESP_ERR_INVALID_ARG;
    
    /* 新密码长度检查 */
    size_t new_len = strlen(new_password);
    if (new_len < 4 || new_len > 64) {
        TS_LOGW(TAG, "New password length invalid (4-64 chars required)");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 验证旧密码 */
    esp_err_t ret = ts_auth_verify_password(username, old_password, NULL);
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Old password verification failed for %s", username);
        return ret;
    }
    
    ret = write_user_password_credential(username, new_password, true);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Password changed for user %s", username);
    }
    
    return ret;
}

/**
 * @brief Root 管理功能：设置 admin 密码
 */
esp_err_t ts_auth_set_admin_password(const char *new_password)
{
    esp_err_t ret = write_user_password_credential("admin", new_password, true);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Password set for admin by root");
    }

    return ret;
}

/**
 * @brief Root 管理功能：设置 root 密码
 */
esp_err_t ts_auth_set_root_password(const char *new_password)
{
    esp_err_t ret = write_user_password_credential("root", new_password, true);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Password set for root by root");
    }

    return ret;
}

/**
 * @brief 检查用户是否已修改初始密码
 */
bool ts_auth_password_changed(const char *username)
{
    if (!username) return true;
    
    user_credential_t cred;
    if (load_user_credential(username, &cred) != ESP_OK) {
        return true;  /* 找不到用户，视为已修改 */
    }
    
    return cred.password_changed;
}

/**
 * @brief 登录并创建会话
 */
esp_err_t ts_auth_login(const char *username, const char *password,
                         uint32_t *session_id, char *token, size_t token_len)
{
    ts_perm_level_t level;
    esp_err_t ret = ts_auth_verify_password(username, password, &level);
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "Login failed for user: %s", username);
        return ret;
    }
    
    ret = ts_security_create_session(username, level, session_id);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to create session for %s", username);
        return ret;
    }
    
    /* 生成 token */
    if (token && token_len > 0) {
        ret = ts_security_generate_token(*session_id, token, token_len);
        if (ret != ESP_OK) {
            ts_security_destroy_session(*session_id);
            return ret;
        }
    }
    
    TS_LOGI(TAG, "User %s logged in (level %d)", username, level);
    return ESP_OK;
}

/**
 * @brief 登出
 */
esp_err_t ts_auth_logout(uint32_t session_id)
{
    return ts_security_destroy_session(session_id);
}

/**
 * @brief 验证请求认证
 */
esp_err_t ts_auth_validate_request(const char *auth_header, uint32_t *session_id,
                                    ts_perm_level_t *level)
{
    if (!auth_header) return ESP_ERR_INVALID_ARG;
    
    /* 支持 "Bearer <token>" 格式 */
    if (strncmp(auth_header, "Bearer ", 7) == 0) {
        const char *token = auth_header + 7;
        esp_err_t ret = ts_security_validate_token(token, session_id);
        if (ret == ESP_OK && level) {
            ts_session_t session;
            if (ts_security_validate_session(*session_id, &session) == ESP_OK) {
                *level = session.level;
            }
        }
        return ret;
    }
    
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 重置用户密码为默认值（管理功能）
 */
esp_err_t ts_auth_reset_password(const char *username)
{
    if (!username) return ESP_ERR_INVALID_ARG;
    
    /* 只允许重置 admin 和 root */
    const char *default_pwd;
    if (strcmp(username, "admin") == 0) {
        default_pwd = TS_AUTH_DEFAULT_ADMIN_PASSWORD;
    } else if (strcmp(username, "root") == 0) {
        default_pwd = TS_AUTH_DEFAULT_ROOT_PASSWORD;
    } else {
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_err_t ret = write_user_password_credential(username, default_pwd, false);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Password reset to default for user %s", username);
    }
    
    return ret;
}
