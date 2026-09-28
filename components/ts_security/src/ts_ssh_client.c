/**
 * @file ts_ssh_client.c
 * @brief SSH Client implementation using libssh2
 *
 * This module provides SSH client functionality for TianShanOS,
 * using libssh2 library for full SSH2 protocol support.
 *
 * 会话结构和缓冲区优先分配到 PSRAM
 */

#include "ts_ssh_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ts_core.h" /* TS_MALLOC_PSRAM, TS_CALLOC_PSRAM */
#include "ts_known_hosts.h"
#include <stdatomic.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char *TAG = "ts_ssh";

/* ============================================================================
 * 内部数据结构
 * ============================================================================ */

/** SSH 会话内部结构 */
struct ts_ssh_session_s {
    ts_ssh_config_t config;   /**< 会话配置 */
    ts_ssh_state_t state;     /**< 当前状态 */
    int sock;                 /**< Socket 文件描述符 */
    LIBSSH2_SESSION *session; /**< libssh2 会话句柄 */
    char error_msg[256];      /**< 最后的错误消息 */
    char *host_copy;          /**< 主机地址副本 */
    char *username_copy;      /**< 用户名副本 */
    char *password_copy;      /**< 密码副本 */
    atomic_bool abort_flag;
    int64_t deadline_us; /**< 中止标志 */
};

/** 全局初始化标志 */
static bool s_initialized = false;

/* ============================================================================
 * 内部辅助函数
 * ============================================================================ */

/**
 * @brief 等待 socket 可读/可写
 */
static bool operation_expired(ts_ssh_session_t s) {
    return atomic_load(&s->abort_flag) ||
           (s->config.cancelled && s->config.cancelled(s->config.cancel_context)) ||
           esp_timer_get_time() >= s->deadline_us;
}

static int wait_socket(ts_ssh_session_t s) {
    if (operation_expired(s))
        return -1;
    int64_t left = s->deadline_us - esp_timer_get_time();
    if (left > 100000)
        left = 100000;
    if (left <= 0)
        return -1;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = left};
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    int dir =
        s->session ? libssh2_session_block_directions(s->session) : LIBSSH2_SESSION_BLOCK_OUTBOUND;
    if (!dir)
        dir = LIBSSH2_SESSION_BLOCK_INBOUND;
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        FD_SET(s->sock, &rd);
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        FD_SET(s->sock, &wr);
    int rc = select(s->sock + 1, &rd, &wr, NULL, &timeout);
    return (rc < 0 && errno != EINTR) || operation_expired(s) ? -1 : rc;
}

/* Teardown cannot wait for a remote peer. These callbacks make all remaining
 * libssh2 channel cleanup observe a disconnected transport (never EAGAIN). */
static LIBSSH2_RECV_FUNC(closed_recv) { return 0; }
static LIBSSH2_SEND_FUNC(closed_send) { return -ENOTCONN; }

/**
 * @brief 设置错误消息
 */
static void set_error(ts_ssh_session_t session, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(session->error_msg, sizeof(session->error_msg), fmt, args);
    va_end(args);
    ESP_LOGE(TAG, "%s", session->error_msg);
}

/**
 * @brief 复制字符串
 */
static char *strdup_safe(const char *str) {
    if (str == NULL) {
        return NULL;
    }
    char *copy = TS_MALLOC_PSRAM(strlen(str) + 1);
    if (copy) {
        strcpy(copy, str);
    }
    return copy;
}

/* ============================================================================
 * 公开 API 实现
 * ============================================================================ */

esp_err_t ts_ssh_client_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "libssh2_init failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SSH client initialized (libssh2 version: %s)", LIBSSH2_VERSION);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ts_ssh_client_deinit(void) {
    if (!s_initialized) {
        return ESP_OK;
    }

    libssh2_exit();
    s_initialized = false;
    ESP_LOGI(TAG, "SSH client deinitialized");
    return ESP_OK;
}

esp_err_t ts_ssh_session_create(const ts_ssh_config_t *config, ts_ssh_session_t *session_out) {
    if (!config || !session_out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!config->host || !config->username) {
        ESP_LOGE(TAG, "Host and username are required");
        return ESP_ERR_INVALID_ARG;
    }

    /* 确保全局初始化 */
    if (!s_initialized) {
        esp_err_t ret = ts_ssh_client_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    /* 分配会话结构 */
    ts_ssh_session_t session = TS_CALLOC_PSRAM(1, sizeof(struct ts_ssh_session_s));
    if (!session) {
        return ESP_ERR_NO_MEM;
    }

    /* 复制配置 */
    session->config = *config;
    session->config.port = config->port ? config->port : 22;
    session->config.timeout_ms = config->timeout_ms ? config->timeout_ms : 10000;

    atomic_init(&session->abort_flag, false);
    /* 复制字符串（因为调用者可能释放原始字符串） */
    session->host_copy = strdup_safe(config->host);
    session->username_copy = strdup_safe(config->username);
    session->config.host = session->host_copy;
    session->config.username = session->username_copy;

    if (config->auth_method == TS_SSH_AUTH_PASSWORD && config->auth.password) {
        session->password_copy = strdup_safe(config->auth.password);
        session->config.auth.password = session->password_copy;
    }

    if (!session->host_copy || !session->username_copy ||
        (config->auth_method == TS_SSH_AUTH_PASSWORD && config->auth.password &&
         !session->password_copy)) {
        free(session->host_copy);
        free(session->username_copy);
        free(session->password_copy);
        free(session);
        return ESP_ERR_NO_MEM;
    }
    session->sock = -1;
    session->session = NULL;
    session->state = TS_SSH_STATE_DISCONNECTED;
    session->error_msg[0] = '\0';

    *session_out = session;
    ESP_LOGD(TAG, "Session created for %s@%s:%d", session->config.username, session->config.host,
             session->config.port);
    return ESP_OK;
}

esp_err_t ts_ssh_session_destroy(ts_ssh_session_t session) {
    if (!session) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先断开连接 */
    if (session->state != TS_SSH_STATE_DISCONNECTED) {
        ts_ssh_disconnect(session);
    }

    /* 释放字符串副本 */
    free(session->host_copy);
    free(session->username_copy);
    free(session->password_copy);

    /* 释放会话结构 */
    free(session);
    return ESP_OK;
}

esp_err_t ts_ssh_connect(ts_ssh_session_t s) { return ts_ssh_connect_with_verifier(s, NULL, NULL); }

esp_err_t ts_ssh_connect_with_verifier(ts_ssh_session_t s, ts_ssh_verify_cb_t verify,
                                       void *context) {
    if (!s)
        return ESP_ERR_INVALID_ARG;
    if (s->state == TS_SSH_STATE_CONNECTED)
        return ESP_OK;
    if (s->session || s->sock >= 0)
        ts_ssh_disconnect(s);
    s->deadline_us = esp_timer_get_time() + (int64_t)s->config.timeout_ms * 1000;
    s->state = TS_SSH_STATE_CONNECTING;
    esp_err_t ret = ESP_FAIL;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(s->config.port)};
    if (inet_pton(AF_INET, s->config.host, &addr.sin_addr) != 1) {
        /* lwIP DNS is synchronous; cancellation is observed immediately after
         * its configured resolver timeout. No extra resolver task is created. */
        struct hostent *host = gethostbyname(s->config.host);
        if (!host || host->h_length != sizeof(addr.sin_addr))
            goto failed;
        memcpy(&addr.sin_addr, host->h_addr, sizeof(addr.sin_addr));
    }
    if (operation_expired(s))
        goto timeout;
    s->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s->sock < 0 || fcntl(s->sock, F_SETFL, O_NONBLOCK) < 0)
        goto failed;
    int rc = connect(s->sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc && errno != EINPROGRESS)
        goto failed;
    while (rc) {
        int ready = wait_socket(s);
        if (ready < 0)
            goto timeout;
        if (!ready)
            continue;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(s->sock, SOL_SOCKET, SO_ERROR, &error, &size) || error)
            goto failed;
        break;
    }
    s->session = libssh2_session_init();
    if (!s->session) {
        ret = ESP_ERR_NO_MEM;
        goto failed;
    }
    libssh2_session_set_blocking(s->session, 0);
    while ((rc = libssh2_session_handshake(s->session, s->sock)) == LIBSSH2_ERROR_EAGAIN)
        if (wait_socket(s) < 0)
            goto timeout;
    if (rc)
        goto failed;
    if (verify)
        ret = verify(s, context);
    else {
        ts_host_verify_result_t status;
        ret = ts_known_hosts_verify(s, &status, NULL);
        if (ret == ESP_OK && status != TS_HOST_VERIFY_OK)
            ret = status == TS_HOST_VERIFY_NOT_FOUND  ? TS_SSH_ERR_HOST_UNKNOWN
                  : status == TS_HOST_VERIFY_MISMATCH ? TS_SSH_ERR_HOST_CHANGED
                                                      : ESP_FAIL;
    }
    if (ret != ESP_OK)
        goto failed;
    if (operation_expired(s))
        goto timeout;
    s->state = TS_SSH_STATE_AUTHENTICATING;
    do {
        if (s->config.auth_method == TS_SSH_AUTH_PASSWORD) {
            if (!s->config.auth.password) {
                ret = ESP_ERR_INVALID_ARG;
                goto failed;
            }
            rc = libssh2_userauth_password(s->session, s->config.username, s->config.auth.password);
        } else if (s->config.auth.key.private_key_path) {
            rc = libssh2_userauth_publickey_fromfile_ex(
                s->session, s->config.username, strlen(s->config.username), NULL,
                s->config.auth.key.private_key_path, s->config.auth.key.passphrase);
        } else if (s->config.auth.key.private_key && s->config.auth.key.private_key_len) {
            rc = libssh2_userauth_publickey_frommemory(
                s->session, s->config.username, strlen(s->config.username), NULL, 0,
                (const char *)s->config.auth.key.private_key, s->config.auth.key.private_key_len,
                s->config.auth.key.passphrase);
        } else {
            ret = ESP_ERR_INVALID_ARG;
            goto failed;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN && wait_socket(s) < 0)
            goto timeout;
    } while (rc == LIBSSH2_ERROR_EAGAIN);
    if (rc) {
        ret = ESP_ERR_INVALID_STATE;
        goto failed;
    }
    if (operation_expired(s))
        goto timeout;
    s->state = TS_SSH_STATE_CONNECTED;
    return ESP_OK;
timeout:
    ret = ESP_ERR_TIMEOUT;
failed:
    if (ret == ESP_OK)
        ret = ESP_FAIL;
    set_error(s, "SSH connection failed (code=%d)", ret);
    ts_ssh_disconnect(s);
    s->state = TS_SSH_STATE_ERROR;
    return ret;
}

esp_err_t ts_ssh_disconnect(ts_ssh_session_t s) {
    if (!s)
        return ESP_ERR_INVALID_ARG;
    if (s->session) {
        libssh2_session_callback_set(s->session, LIBSSH2_CALLBACK_RECV, (void *)closed_recv);
        libssh2_session_callback_set(s->session, LIBSSH2_CALLBACK_SEND, (void *)closed_send);
        libssh2_session_free(s->session);
        s->session = NULL;
    }
    if (s->sock >= 0) {
        close(s->sock);
        s->sock = -1;
    }
    s->state = TS_SSH_STATE_DISCONNECTED;
    return ESP_OK;
}

bool ts_ssh_is_connected(ts_ssh_session_t session) {
    return session && session->state == TS_SSH_STATE_CONNECTED;
}

ts_ssh_state_t ts_ssh_get_state(ts_ssh_session_t session) {
    return session ? session->state : TS_SSH_STATE_DISCONNECTED;
}

static esp_err_t exec_command(ts_ssh_session_t s, const char *command, ts_ssh_output_cb_t callback,
                              void *context, ts_ssh_exec_result_t *result, int *exit_code) {
    if (!s || !command || (!callback && !result))
        return ESP_ERR_INVALID_ARG;
    if (!ts_ssh_is_connected(s))
        return ESP_ERR_INVALID_STATE;
    if (result) {
        memset(result, 0, sizeof(*result));
        result->exit_code = -1;
    }
    if (exit_code)
        *exit_code = -1;
    s->deadline_us = esp_timer_get_time() + (int64_t)s->config.timeout_ms * 1000;
    esp_err_t ret = ESP_FAIL;
    LIBSSH2_CHANNEL *channel = NULL;
    int rc;
    while (!operation_expired(s)) {
        channel = libssh2_channel_open_session(s->session);
        if (channel)
            break;
        if (libssh2_session_last_errno(s->session) != LIBSSH2_ERROR_EAGAIN)
            goto failed;
        if (wait_socket(s) < 0)
            goto timeout;
    }
    if (!channel || operation_expired(s))
        goto timeout;
    while ((rc = libssh2_channel_exec(channel, command)) == LIBSSH2_ERROR_EAGAIN)
        if (wait_socket(s) < 0)
            goto timeout;
    if (rc)
        goto failed;
    size_t capacity[2] = {0, 0};
    size_t limit = s->config.max_output_bytes ? s->config.max_output_bytes : 65536;
    for (;;) {
        bool got_data = false;
        for (int stream = 0; stream < 2; stream++) {
            /* One bounded chunk per stream prevents stdout starving stderr/cancel. */
            if (operation_expired(s))
                goto timeout;
            char buffer[512];
            rc = libssh2_channel_read_ex(channel, stream ? SSH_EXTENDED_DATA_STDERR : 0, buffer,
                                         sizeof(buffer));
            if (rc < 0 && rc != LIBSSH2_ERROR_EAGAIN)
                goto failed;
            if (rc > 0) {
                got_data = true;
                if (callback)
                    callback(buffer, rc, stream != 0, context);
                if (result) {
                    char **data = stream ? &result->stderr_data : &result->stdout_data;
                    size_t *len = stream ? &result->stderr_len : &result->stdout_len;
                    if ((size_t)rc > limit - *len) {
                        ret = ESP_ERR_INVALID_SIZE;
                        goto failed;
                    }
                    if (*len + rc + 1 > capacity[stream]) {
                        size_t next = capacity[stream] ? capacity[stream] * 2 : 1024;
                        if (next > limit + 1)
                            next = limit + 1;
                        char *grown = TS_REALLOC_PSRAM(*data, next);
                        if (!grown) {
                            ret = ESP_ERR_NO_MEM;
                            goto failed;
                        }
                        *data = grown;
                        capacity[stream] = next;
                    }
                    memcpy(*data + *len, buffer, rc);
                    *len += rc;
                    (*data)[*len] = 0;
                }
            }
        }
        /* Drain buffered bytes even when EOF arrived with the last packet. */
        if (!got_data && libssh2_channel_eof(channel))
            break;
        if (!got_data && wait_socket(s) < 0)
            goto timeout;
    }
    while ((rc = libssh2_channel_close(channel)) == LIBSSH2_ERROR_EAGAIN)
        if (wait_socket(s) < 0)
            goto timeout;
    if (rc)
        goto failed;
    int status = libssh2_channel_get_exit_status(channel);
    if (exit_code)
        *exit_code = status;
    if (result)
        result->exit_code = status;
    libssh2_channel_free(channel);
    return ESP_OK;
timeout:
    ret = ESP_ERR_TIMEOUT;
failed:
    /* A timed-out command has an unknown remote outcome. Dispose the transport
     * and all its channels once; never return partial output as success. */
    ts_ssh_disconnect(s);
    if (result)
        ts_ssh_exec_result_free(result);
    return ret;
}

esp_err_t ts_ssh_exec(ts_ssh_session_t s, const char *command, ts_ssh_exec_result_t *result) {
    return exec_command(s, command, NULL, NULL, result, NULL);
}

esp_err_t ts_ssh_exec_stream(ts_ssh_session_t s, const char *command, ts_ssh_output_cb_t callback,
                             void *context, int *exit_code) {
    return exec_command(s, command, callback, context, NULL, exit_code);
}

void ts_ssh_abort(ts_ssh_session_t session) {
    if (session) {
        session->abort_flag = true;
    }
}

void ts_ssh_exec_result_free(ts_ssh_exec_result_t *result) {
    if (result) {
        free(result->stdout_data);
        free(result->stderr_data);
        result->stdout_data = NULL;
        result->stderr_data = NULL;
        result->stdout_len = 0;
        result->stderr_len = 0;
    }
}

const char *ts_ssh_get_error(ts_ssh_session_t session) {
    return session ? session->error_msg : "Invalid session";
}

esp_err_t ts_ssh_exec_simple(const ts_ssh_config_t *config, const char *command,
                             ts_ssh_exec_result_t *result) {
    ts_ssh_session_t session = NULL;
    esp_err_t ret;

    ret = ts_ssh_session_create(config, &session);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ts_ssh_connect(session);
    if (ret != ESP_OK) {
        ts_ssh_session_destroy(session);
        return ret;
    }

    ret = ts_ssh_exec(session, command, result);

    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);

    return ret;
}

/* ============================================================================
 * SFTP/SCP 辅助函数 - 供 ts_sftp.c 和 ts_scp.c 调用
 * ============================================================================ */

/**
 * @brief 获取内部 libssh2 会话句柄
 */
LIBSSH2_SESSION *ts_ssh_get_libssh2_session(ts_ssh_session_t session) {
    return session ? session->session : NULL;
}

/**
 * @brief 获取 socket 文件描述符
 */
int ts_ssh_get_socket(ts_ssh_session_t session) { return session ? session->sock : -1; }

/**
 * @brief 获取远程主机地址
 */
const char *ts_ssh_get_host(ts_ssh_session_t session) {
    return session ? session->host_copy : NULL;
}

/**
 * @brief 获取远程端口
 */
uint16_t ts_ssh_get_port(ts_ssh_session_t session) { return session ? session->config.port : 0; }