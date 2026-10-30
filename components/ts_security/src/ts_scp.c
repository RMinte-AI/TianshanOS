/**
 * @file ts_scp.c
 * @brief SCP Client implementation using libssh2
 *
 * SCP（安全复制协议）实现，用于简单的单文件传输。
 * 如需目录操作或复杂文件管理，请使用 SFTP。
 * 
 * 传输缓冲区优先分配到 PSRAM
 */

#include "ts_storage.h"
#include "ts_scp.h"
#include "ts_ssh_client.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <libssh2.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

static const char *TAG = "ts_scp";

/* 传输缓冲区大小 */
#define SCP_BUFFER_SIZE    (4 * 1024)  /* 4KB */
#define SCP_MAX_FILE_SIZE  (10 * 1024 * 1024)  /* 10MB 默认最大 */

/* ============================================================================
 * 外部声明 - 来自 ts_ssh_client.c
 * ============================================================================ */

extern LIBSSH2_SESSION *ts_ssh_get_libssh2_session(ts_ssh_session_t session);
extern int ts_ssh_get_socket(ts_ssh_session_t session);

/* ============================================================================
 * 内部辅助函数
 * ============================================================================ */

/**
 * @brief 等待 socket 可读/可写
 */
static int scp_wait_socket(ts_ssh_session_t ssh, int timeout_ms)
{
    int sock = ts_ssh_get_socket(ssh);
    LIBSSH2_SESSION *session = ts_ssh_get_libssh2_session(ssh);
    
    struct timeval timeout;
    fd_set fd;
    fd_set *readfd = NULL;
    fd_set *writefd = NULL;
    int dir;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    FD_ZERO(&fd);
    FD_SET(sock, &fd);

    dir = libssh2_session_block_directions(session);

    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) {
        readfd = &fd;
    }
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) {
        writefd = &fd;
    }

    return select(sock + 1, readfd, writefd, NULL, &timeout);
}

/* ============================================================================
 * SCP 传输函数
 * ============================================================================ */

esp_err_t ts_scp_send(ts_ssh_session_t ssh_session, const char *local_path,
                       const char *remote_path, int mode,
                       ts_scp_progress_cb_t progress_cb, void *user_data)
{
    if (!ssh_session || !local_path || !remote_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 检查 SSH 会话状态 */
    if (ts_ssh_get_state(ssh_session) != TS_SSH_STATE_CONNECTED) {
        ESP_LOGE(TAG, "SSH session not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    LIBSSH2_SESSION *session = ts_ssh_get_libssh2_session(ssh_session);
    if (!session) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = ESP_FAIL;
    LIBSSH2_CHANNEL *channel = NULL;
    FILE *local_file = NULL;
    uint8_t *buffer = NULL;
    
    /* 打开本地文件 */
    local_file = fopen(local_path, "rb");
    if (!local_file) {
        ESP_LOGE(TAG, "Failed to open local file: %s", local_path);
        return ESP_FAIL;
    }
    
    /* 获取文件大小 */
    fseek(local_file, 0, SEEK_END);
    size_t file_size = ftell(local_file);
    fseek(local_file, 0, SEEK_SET);
    
    ESP_LOGI(TAG, "SCP send: %s (%zu bytes) -> %s", local_path, file_size, remote_path);
    
    /* 默认权限 */
    if (mode == 0) {
        mode = 0644;
    }
    
    /* 启动 SCP 发送 */
    while (1) {
        channel = libssh2_scp_send(session, remote_path, mode, file_size);
        if (channel) {
            break;
        }
        
        int err = libssh2_session_last_errno(session);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            scp_wait_socket(ssh_session, 100);
            continue;
        }
        
        char *errmsg;
        libssh2_session_last_error(session, &errmsg, NULL, 0);
        ESP_LOGE(TAG, "Failed to initiate SCP send: %s", errmsg);
        ret = ESP_FAIL;
        goto cleanup;
    }
    
    /* 分配传输缓冲区 */
    buffer = TS_MALLOC_PSRAM(SCP_BUFFER_SIZE);
    if (!buffer) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    
    /* 传输数据 */
    size_t transferred = 0;
    while (transferred < file_size) {
        size_t to_read = (file_size - transferred > SCP_BUFFER_SIZE) ? 
                          SCP_BUFFER_SIZE : (file_size - transferred);
        
        size_t bytes_read = fread(buffer, 1, to_read, local_file);
        if (bytes_read == 0) {
            if (ferror(local_file)) {
                ESP_LOGE(TAG, "Local read error");
                ret = ESP_FAIL;
                goto cleanup;
            }
            break;
        }
        
        /* 写入 SCP 通道 */
        char *ptr = (char *)buffer;
        size_t remaining = bytes_read;
        
        while (remaining > 0) {
            ssize_t rc = libssh2_channel_write(channel, ptr, remaining);
            
            if (rc > 0) {
                ptr += rc;
                remaining -= rc;
                transferred += rc;
            } else if (rc == LIBSSH2_ERROR_EAGAIN) {
                scp_wait_socket(ssh_session, 100);
                continue;
            } else {
                ESP_LOGE(TAG, "SCP write error: %zd", rc);
                ret = ESP_FAIL;
                goto cleanup;
            }
        }
        
        if (progress_cb) {
            progress_cb(transferred, file_size, user_data);
        }
        
        taskYIELD();
    }
    
    /* 发送 EOF */
    while (1) {
        int rc = libssh2_channel_send_eof(channel);
        if (rc == 0) {
            break;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            scp_wait_socket(ssh_session, 100);
            continue;
        } else {
            ESP_LOGW(TAG, "Failed to send EOF: %d", rc);
            break;
        }
    }
    
    /* 等待 EOF 确认 */
    while (1) {
        int rc = libssh2_channel_wait_eof(channel);
        if (rc == 0) {
            break;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            scp_wait_socket(ssh_session, 100);
            continue;
        } else {
            break;
        }
    }
    
    ESP_LOGI(TAG, "SCP send complete: %zu bytes transferred", transferred);
    ret = ESP_OK;
    
cleanup:
    if (buffer) free(buffer);
    if (local_file) fclose(local_file);
    if (channel) {
        libssh2_channel_free(channel);
    }
    
    return ret;
}

esp_err_t ts_scp_recv(ts_ssh_session_t ssh_session, const char *remote_path,
                       const char *local_path, ts_scp_progress_cb_t progress_cb,
                       void *user_data)
{
    if (!ssh_session || !remote_path || !local_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 检查 SSH 会话状态 */
    if (ts_ssh_get_state(ssh_session) != TS_SSH_STATE_CONNECTED) {
        ESP_LOGE(TAG, "SSH session not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    LIBSSH2_SESSION *session = ts_ssh_get_libssh2_session(ssh_session);
    if (!session) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = ts_storage_check_write_path(local_path, false);
    if (ret != ESP_OK) return ret;
    LIBSSH2_CHANNEL *channel = NULL;
    FILE *local_file = NULL;
    uint8_t *buffer = NULL;
    libssh2_struct_stat fileinfo;
    
    /* 启动 SCP 接收 */
    while (1) {
        channel = libssh2_scp_recv2(session, remote_path, &fileinfo);
        if (channel) {
            break;
        }
        
        int err = libssh2_session_last_errno(session);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            scp_wait_socket(ssh_session, 100);
            continue;
        }
        
        char *errmsg;
        libssh2_session_last_error(session, &errmsg, NULL, 0);
        ESP_LOGE(TAG, "Failed to initiate SCP recv: %s", errmsg);
        return ESP_FAIL;
    }
    
    libssh2_uint64_t file_size = fileinfo.st_size;
    ESP_LOGI(TAG, "SCP recv: %s (%llu bytes) -> %s", 
             remote_path, (unsigned long long)file_size, local_path);
    
    /* 打开本地文件 */
    ret = ts_storage_open_write(local_path, &local_file);
    if (ret != ESP_OK) goto cleanup;

    /* 分配传输缓冲区 */
    buffer = TS_MALLOC_PSRAM(SCP_BUFFER_SIZE);
    if (!buffer) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    
    /* 接收数据 */
    libssh2_uint64_t transferred = 0;
    while (transferred < file_size) {
        size_t to_recv = ((file_size - transferred) > SCP_BUFFER_SIZE) ?
                          SCP_BUFFER_SIZE : (size_t)(file_size - transferred);
        
        ssize_t rc = libssh2_channel_read(channel, (char *)buffer, to_recv);
        
        if (rc > 0) {
            size_t written = fwrite(buffer, 1, rc, local_file);
            if (written != (size_t)rc) {
                ESP_LOGE(TAG, "Local write error");
                ret = ESP_FAIL;
                goto cleanup;
            }
            
            transferred += rc;
            
            if (progress_cb) {
                progress_cb(transferred, file_size, user_data);
            }
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            scp_wait_socket(ssh_session, 100);
            continue;
        } else if (rc == 0) {
            ESP_LOGE(TAG, "SCP recv ended early: %llu of %llu bytes",
                     (unsigned long long)transferred, (unsigned long long)file_size);
            ret = ESP_FAIL;
            goto cleanup;
        } else {
            ESP_LOGE(TAG, "SCP read error: %zd", rc);
            ret = ESP_FAIL;
            goto cleanup;
        }
        
        taskYIELD();
    }
    
    ESP_LOGI(TAG, "SCP recv complete: %llu bytes transferred", 
             (unsigned long long)transferred);
    ret = ESP_OK;
    
cleanup:
    if (buffer) free(buffer);
    if (local_file && fclose(local_file) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    if (channel) {
        libssh2_channel_free(channel);
    }
    
    return ret;
}

esp_err_t ts_scp_send_buffer(ts_ssh_session_t ssh_session, const uint8_t *buffer,
                              size_t size, const char *remote_path, int mode)
{
    if (!ssh_session || !buffer || !remote_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 检查 SSH 会话状态 */
    if (ts_ssh_get_state(ssh_session) != TS_SSH_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    LIBSSH2_SESSION *session = ts_ssh_get_libssh2_session(ssh_session);
    if (!session) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = ESP_FAIL;
    LIBSSH2_CHANNEL *channel = NULL;
    
    if (mode == 0) {
        mode = 0644;
    }
    
    ESP_LOGI(TAG, "SCP send buffer: %zu bytes -> %s", size, remote_path);
    
    /* 启动 SCP 发送 */
    while (1) {
        channel = libssh2_scp_send(session, remote_path, mode, size);
        if (channel) {
            break;
        }
        
        int err = libssh2_session_last_errno(session);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            scp_wait_socket(ssh_session, 100);
            continue;
        }
        
        char *errmsg;
        libssh2_session_last_error(session, &errmsg, NULL, 0);
        ESP_LOGE(TAG, "Failed to initiate SCP send: %s", errmsg);
        return ESP_FAIL;
    }
    
    /* 写入数据 */
    const char *ptr = (const char *)buffer;
    size_t remaining = size;
    
    while (remaining > 0) {
        ssize_t rc = libssh2_channel_write(channel, ptr, remaining);
        
        if (rc > 0) {
            ptr += rc;
            remaining -= rc;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            scp_wait_socket(ssh_session, 100);
            continue;
        } else {
            ESP_LOGE(TAG, "SCP write error: %zd", rc);
            goto cleanup;
        }
    }
    
    /* 发送 EOF */
    while (libssh2_channel_send_eof(channel) == LIBSSH2_ERROR_EAGAIN) {
        scp_wait_socket(ssh_session, 100);
    }
    
    while (libssh2_channel_wait_eof(channel) == LIBSSH2_ERROR_EAGAIN) {
        scp_wait_socket(ssh_session, 100);
    }
    
    ESP_LOGI(TAG, "SCP send buffer complete");
    ret = ESP_OK;
    
cleanup:
    if (channel) {
        libssh2_channel_free(channel);
    }
    
    return ret;
}

esp_err_t ts_scp_recv_buffer(ts_ssh_session_t ssh_session, const char *remote_path,
                              uint8_t **buffer, size_t *size, size_t max_size)
{
    if (!ssh_session || !remote_path || !buffer || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *buffer = NULL;
    *size = 0;
    
    /* 检查 SSH 会话状态 */
    if (ts_ssh_get_state(ssh_session) != TS_SSH_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    LIBSSH2_SESSION *session = ts_ssh_get_libssh2_session(ssh_session);
    if (!session) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (max_size == 0) {
        max_size = SCP_MAX_FILE_SIZE;
    }
    
    esp_err_t ret = ESP_FAIL;
    LIBSSH2_CHANNEL *channel = NULL;
    uint8_t *buf = NULL;
    libssh2_struct_stat fileinfo;
    
    /* 启动 SCP 接收 */
    while (1) {
        channel = libssh2_scp_recv2(session, remote_path, &fileinfo);
        if (channel) {
            break;
        }
        
        int err = libssh2_session_last_errno(session);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            scp_wait_socket(ssh_session, 100);
            continue;
        }
        
        char *errmsg;
        libssh2_session_last_error(session, &errmsg, NULL, 0);
        ESP_LOGE(TAG, "Failed to initiate SCP recv: %s", errmsg);
        return ESP_FAIL;
    }
    
    libssh2_uint64_t file_size = fileinfo.st_size;
    
    if (file_size > max_size) {
        ESP_LOGE(TAG, "File too large: %llu > %zu", 
                 (unsigned long long)file_size, max_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    
    ESP_LOGI(TAG, "SCP recv to buffer: %s (%llu bytes)", 
             remote_path, (unsigned long long)file_size);
    
    /* 分配缓冲区 */
    buf = TS_MALLOC_PSRAM((size_t)file_size);
    if (!buf) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    
    /* 接收数据 */
    size_t received = 0;
    while (received < file_size) {
        ssize_t rc = libssh2_channel_read(channel, (char *)buf + received, 
                                          (size_t)(file_size - received));
        
        if (rc > 0) {
            received += rc;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            scp_wait_socket(ssh_session, 100);
            continue;
        } else if (rc == 0) {
            break;
        } else {
            ESP_LOGE(TAG, "SCP read error: %zd", rc);
            free(buf);
            ret = ESP_FAIL;
            goto cleanup;
        }
    }
    
    *buffer = buf;
    *size = received;
    ret = ESP_OK;
    
    ESP_LOGI(TAG, "SCP recv to buffer complete: %zu bytes", received);
    
cleanup:
    if (channel) {
        libssh2_channel_free(channel);
    }
    
    return ret;
}
