/**
 * @file ts_sftp.c
 * @brief SFTP Client implementation using libssh2
 *
 * SFTP（安全文件传输协议）实现，支持：
 * - 文件读写操作
 * - 目录遍历和管理
 * - 高级文件传输（上传/下载）
 * 
 * 大缓冲区优先分配到 PSRAM
 */

#include "ts_storage.h"
#include "ts_sftp.h"
#include "ts_ssh_client.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM, TS_CALLOC_PSRAM */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

static const char *TAG = "ts_sftp";

/* ============================================================================
 * 内部数据结构
 * ============================================================================ */

/** SFTP 会话内部结构 */
struct ts_sftp_session_s {
    ts_ssh_session_t ssh;           /**< 关联的 SSH 会话 */
    LIBSSH2_SFTP *sftp;             /**< libssh2 SFTP 句柄 */
    char error_msg[256];            /**< 最后的错误消息 */
};

/** SFTP 文件内部结构 */
struct ts_sftp_file_s {
    ts_sftp_session_t sftp_session; /**< 关联的 SFTP 会话 */
    LIBSSH2_SFTP_HANDLE *handle;    /**< libssh2 文件句柄 */
    uint64_t offset;                /**< 当前偏移量 */
};

/** SFTP 目录内部结构 */
struct ts_sftp_dir_s {
    ts_sftp_session_t sftp_session; /**< 关联的 SFTP 会话 */
    LIBSSH2_SFTP_HANDLE *handle;    /**< libssh2 目录句柄 */
};

/* 传输缓冲区大小 */
#define SFTP_BUFFER_SIZE    (4 * 1024)  /* 4KB，适合嵌入式环境 */

/* ============================================================================
 * 外部声明 - 来自 ts_ssh_client.c
 * ============================================================================ */

/* 获取内部 libssh2 会话句柄 */
extern LIBSSH2_SESSION *ts_ssh_get_libssh2_session(ts_ssh_session_t session);
extern int ts_ssh_get_socket(ts_ssh_session_t session);

/* ============================================================================
 * 内部辅助函数
 * ============================================================================ */

/**
 * @brief 等待 socket 可读/可写
 */
static int sftp_wait_socket(ts_sftp_session_t sftp, int timeout_ms)
{
    int sock = ts_ssh_get_socket(sftp->ssh);
    LIBSSH2_SESSION *session = ts_ssh_get_libssh2_session(sftp->ssh);
    
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

/**
 * @brief 设置错误消息
 */
static void sftp_set_error(ts_sftp_session_t sftp, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(sftp->error_msg, sizeof(sftp->error_msg), fmt, args);
    va_end(args);
}

/**
 * @brief 将 ts_sftp 标志转换为 libssh2 标志
 */
static unsigned long sftp_flags_to_libssh2(int flags)
{
    unsigned long lflags = 0;
    
    if (flags & TS_SFTP_READ) {
        lflags |= LIBSSH2_FXF_READ;
    }
    if (flags & TS_SFTP_WRITE) {
        lflags |= LIBSSH2_FXF_WRITE;
    }
    if (flags & TS_SFTP_APPEND) {
        lflags |= LIBSSH2_FXF_APPEND;
    }
    if (flags & TS_SFTP_CREATE) {
        lflags |= LIBSSH2_FXF_CREAT;
    }
    if (flags & TS_SFTP_TRUNC) {
        lflags |= LIBSSH2_FXF_TRUNC;
    }
    if (flags & TS_SFTP_EXCL) {
        lflags |= LIBSSH2_FXF_EXCL;
    }
    
    return lflags;
}

/**
 * @brief 将 libssh2 属性转换为 ts_sftp 属性
 */
static void libssh2_attrs_to_ts(LIBSSH2_SFTP_ATTRIBUTES *lattr, ts_sftp_attr_t *attr)
{
    memset(attr, 0, sizeof(*attr));
    
    if (lattr->flags & LIBSSH2_SFTP_ATTR_SIZE) {
        attr->size = lattr->filesize;
    }
    if (lattr->flags & LIBSSH2_SFTP_ATTR_UIDGID) {
        attr->uid = lattr->uid;
        attr->gid = lattr->gid;
    }
    if (lattr->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
        attr->permissions = lattr->permissions;
        attr->is_dir = LIBSSH2_SFTP_S_ISDIR(lattr->permissions);
        attr->is_link = LIBSSH2_SFTP_S_ISLNK(lattr->permissions);
    }
    if (lattr->flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
        attr->atime = lattr->atime;
        attr->mtime = lattr->mtime;
    }
}

/* ============================================================================
 * 会话管理
 * ============================================================================ */

esp_err_t ts_sftp_open(ts_ssh_session_t ssh_session, ts_sftp_session_t *sftp_out)
{
    if (!ssh_session || !sftp_out) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *sftp_out = NULL;
    
    /* 检查 SSH 会话状态 */
    if (ts_ssh_get_state(ssh_session) != TS_SSH_STATE_CONNECTED) {
        ESP_LOGE(TAG, "SSH session not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 分配 SFTP 会话结构 */
    ts_sftp_session_t sftp = TS_CALLOC_PSRAM(1, sizeof(struct ts_sftp_session_s));
    if (!sftp) {
        ESP_LOGE(TAG, "Failed to allocate SFTP session");
        return ESP_ERR_NO_MEM;
    }
    
    sftp->ssh = ssh_session;
    
    /* 获取 libssh2 会话 */
    LIBSSH2_SESSION *session = ts_ssh_get_libssh2_session(ssh_session);
    if (!session) {
        free(sftp);
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 打开 SFTP 子系统 */
    ESP_LOGI(TAG, "Opening SFTP subsystem...");
    
    while (1) {
        sftp->sftp = libssh2_sftp_init(session);
        if (sftp->sftp) {
            break;
        }
        
        int err = libssh2_session_last_errno(session);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        }
        
        char *errmsg;
        libssh2_session_last_error(session, &errmsg, NULL, 0);
        ESP_LOGE(TAG, "Failed to init SFTP: %s (errno=%d)", errmsg, err);
        sftp_set_error(sftp, "Failed to init SFTP: %s", errmsg);
        free(sftp);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "SFTP session opened successfully");
    *sftp_out = sftp;
    return ESP_OK;
}

esp_err_t ts_sftp_close(ts_sftp_session_t sftp)
{
    if (!sftp) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (sftp->sftp) {
        libssh2_sftp_shutdown(sftp->sftp);
        sftp->sftp = NULL;
    }
    
    free(sftp);
    ESP_LOGI(TAG, "SFTP session closed");
    return ESP_OK;
}

const char *ts_sftp_get_error(ts_sftp_session_t sftp)
{
    return sftp ? sftp->error_msg : "Invalid session";
}

/* ============================================================================
 * 文件操作
 * ============================================================================ */

esp_err_t ts_sftp_file_open(ts_sftp_session_t sftp, const char *path,
                            int flags, int mode, ts_sftp_file_t *file_out)
{
    if (!sftp || !path || !file_out) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *file_out = NULL;
    
    if (!sftp->sftp) {
        sftp_set_error(sftp, "SFTP session not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 分配文件结构 */
    ts_sftp_file_t file = TS_CALLOC_PSRAM(1, sizeof(struct ts_sftp_file_s));
    if (!file) {
        return ESP_ERR_NO_MEM;
    }
    
    file->sftp_session = sftp;
    
    /* 转换标志 */
    unsigned long lflags = sftp_flags_to_libssh2(flags);
    long lmode = mode ? mode : LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                               LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH;
    
    /* 打开文件 */
    while (1) {
        file->handle = libssh2_sftp_open(sftp->sftp, path, lflags, lmode);
        if (file->handle) {
            break;
        }
        
        int err = libssh2_session_last_errno(ts_ssh_get_libssh2_session(sftp->ssh));
        if (err == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        }
        
        unsigned long sftp_err = libssh2_sftp_last_error(sftp->sftp);
        sftp_set_error(sftp, "Failed to open file: SFTP error %lu", sftp_err);
        ESP_LOGE(TAG, "Failed to open %s: SFTP error %lu", path, sftp_err);
        free(file);
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Opened file: %s", path);
    *file_out = file;
    return ESP_OK;
}

esp_err_t ts_sftp_file_close(ts_sftp_file_t file)
{
    if (!file) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (file->handle) {
        libssh2_sftp_close(file->handle);
        file->handle = NULL;
    }
    
    free(file);
    return ESP_OK;
}

esp_err_t ts_sftp_file_read(ts_sftp_file_t file, void *buffer, size_t size, size_t *bytes_read)
{
    if (!file || !buffer || !bytes_read) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *bytes_read = 0;
    
    if (!file->handle) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ts_sftp_session_t sftp = file->sftp_session;
    
    while (1) {
        ssize_t rc = libssh2_sftp_read(file->handle, buffer, size);
        
        if (rc > 0) {
            *bytes_read = rc;
            file->offset += rc;
            return ESP_OK;
        } else if (rc == 0) {
            /* EOF */
            return ESP_ERR_NOT_FOUND;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        } else {
            sftp_set_error(sftp, "Read error: %zd", rc);
            return ESP_FAIL;
        }
    }
}

esp_err_t ts_sftp_file_write(ts_sftp_file_t file, const void *buffer, size_t size, size_t *bytes_written)
{
    if (!file || !buffer || !bytes_written) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *bytes_written = 0;
    
    if (!file->handle) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ts_sftp_session_t sftp = file->sftp_session;
    const char *ptr = buffer;
    size_t remaining = size;
    
    while (remaining > 0) {
        ssize_t rc = libssh2_sftp_write(file->handle, ptr, remaining);
        
        if (rc > 0) {
            ptr += rc;
            remaining -= rc;
            *bytes_written += rc;
            file->offset += rc;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        } else {
            sftp_set_error(sftp, "Write error: %zd", rc);
            return ESP_FAIL;
        }
    }
    
    return ESP_OK;
}

esp_err_t ts_sftp_file_seek(ts_sftp_file_t file, uint64_t offset)
{
    if (!file || !file->handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    libssh2_sftp_seek64(file->handle, offset);
    file->offset = offset;
    return ESP_OK;
}

esp_err_t ts_sftp_stat(ts_sftp_session_t sftp, const char *path, ts_sftp_attr_t *attrs)
{
    if (!sftp || !path || !attrs) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!sftp->sftp) {
        return ESP_ERR_INVALID_STATE;
    }
    
    LIBSSH2_SFTP_ATTRIBUTES lattr;
    
    while (1) {
        int rc = libssh2_sftp_stat(sftp->sftp, path, &lattr);
        
        if (rc == 0) {
            libssh2_attrs_to_ts(&lattr, attrs);
            return ESP_OK;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        } else {
            unsigned long sftp_err = libssh2_sftp_last_error(sftp->sftp);
            sftp_set_error(sftp, "stat failed: SFTP error %lu", sftp_err);
            return ESP_FAIL;
        }
    }
}

esp_err_t ts_sftp_unlink(ts_sftp_session_t sftp, const char *path)
{
    if (!sftp || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!sftp->sftp) {
        return ESP_ERR_INVALID_STATE;
    }
    
    while (1) {
        int rc = libssh2_sftp_unlink(sftp->sftp, path);
        
        if (rc == 0) {
            ESP_LOGI(TAG, "Deleted: %s", path);
            return ESP_OK;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        } else {
            unsigned long sftp_err = libssh2_sftp_last_error(sftp->sftp);
            sftp_set_error(sftp, "unlink failed: SFTP error %lu", sftp_err);
            return ESP_FAIL;
        }
    }
}

esp_err_t ts_sftp_rename(ts_sftp_session_t sftp, const char *old_path, const char *new_path)
{
    if (!sftp || !old_path || !new_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!sftp->sftp) {
        return ESP_ERR_INVALID_STATE;
    }
    
    while (1) {
        int rc = libssh2_sftp_rename(sftp->sftp, old_path, new_path);
        
        if (rc == 0) {
            ESP_LOGI(TAG, "Renamed: %s -> %s", old_path, new_path);
            return ESP_OK;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        } else {
            unsigned long sftp_err = libssh2_sftp_last_error(sftp->sftp);
            sftp_set_error(sftp, "rename failed: SFTP error %lu", sftp_err);
            return ESP_FAIL;
        }
    }
}

/* ============================================================================
 * 目录操作
 * ============================================================================ */

esp_err_t ts_sftp_dir_open(ts_sftp_session_t sftp, const char *path, ts_sftp_dir_t *dir_out)
{
    if (!sftp || !path || !dir_out) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *dir_out = NULL;
    
    if (!sftp->sftp) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 分配目录结构 */
    ts_sftp_dir_t dir = TS_CALLOC_PSRAM(1, sizeof(struct ts_sftp_dir_s));
    if (!dir) {
        return ESP_ERR_NO_MEM;
    }
    
    dir->sftp_session = sftp;
    
    /* 打开目录 */
    while (1) {
        dir->handle = libssh2_sftp_opendir(sftp->sftp, path);
        if (dir->handle) {
            break;
        }
        
        int err = libssh2_session_last_errno(ts_ssh_get_libssh2_session(sftp->ssh));
        if (err == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        }
        
        unsigned long sftp_err = libssh2_sftp_last_error(sftp->sftp);
        sftp_set_error(sftp, "Failed to open dir: SFTP error %lu", sftp_err);
        free(dir);
        return ESP_FAIL;
    }
    
    *dir_out = dir;
    return ESP_OK;
}

esp_err_t ts_sftp_dir_read(ts_sftp_dir_t dir, ts_sftp_dirent_t *entry)
{
    if (!dir || !entry) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!dir->handle) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ts_sftp_session_t sftp = dir->sftp_session;
    LIBSSH2_SFTP_ATTRIBUTES lattr;
    char longentry[512];
    
    while (1) {
        int rc = libssh2_sftp_readdir_ex(dir->handle, entry->name, sizeof(entry->name) - 1,
                                         longentry, sizeof(longentry), &lattr);
        
        if (rc > 0) {
            entry->name[rc] = '\0';
            libssh2_attrs_to_ts(&lattr, &entry->attrs);
            return ESP_OK;
        } else if (rc == 0) {
            /* End of directory */
            return ESP_ERR_NOT_FOUND;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        } else {
            sftp_set_error(sftp, "readdir failed: %d", rc);
            return ESP_FAIL;
        }
    }
}

esp_err_t ts_sftp_dir_close(ts_sftp_dir_t dir)
{
    if (!dir) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (dir->handle) {
        libssh2_sftp_closedir(dir->handle);
        dir->handle = NULL;
    }
    
    free(dir);
    return ESP_OK;
}

esp_err_t ts_sftp_mkdir(ts_sftp_session_t sftp, const char *path, int mode)
{
    if (!sftp || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!sftp->sftp) {
        return ESP_ERR_INVALID_STATE;
    }
    
    long lmode = mode ? mode : LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP |
                               LIBSSH2_SFTP_S_IXGRP | LIBSSH2_SFTP_S_IROTH |
                               LIBSSH2_SFTP_S_IXOTH;
    
    while (1) {
        int rc = libssh2_sftp_mkdir(sftp->sftp, path, lmode);
        
        if (rc == 0) {
            ESP_LOGI(TAG, "Created directory: %s", path);
            return ESP_OK;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        } else {
            unsigned long sftp_err = libssh2_sftp_last_error(sftp->sftp);
            sftp_set_error(sftp, "mkdir failed: SFTP error %lu", sftp_err);
            return ESP_FAIL;
        }
    }
}

esp_err_t ts_sftp_rmdir(ts_sftp_session_t sftp, const char *path)
{
    if (!sftp || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!sftp->sftp) {
        return ESP_ERR_INVALID_STATE;
    }
    
    while (1) {
        int rc = libssh2_sftp_rmdir(sftp->sftp, path);
        
        if (rc == 0) {
            ESP_LOGI(TAG, "Removed directory: %s", path);
            return ESP_OK;
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            sftp_wait_socket(sftp, 100);
            continue;
        } else {
            unsigned long sftp_err = libssh2_sftp_last_error(sftp->sftp);
            sftp_set_error(sftp, "rmdir failed: SFTP error %lu", sftp_err);
            return ESP_FAIL;
        }
    }
}

/* ============================================================================
 * 高级传输函数
 * ============================================================================ */

esp_err_t ts_sftp_get(ts_sftp_session_t sftp, const char *remote_path,
                       const char *local_path, ts_sftp_progress_cb_t progress_cb,
                       void *user_data)
{
    if (!sftp || !remote_path || !local_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_storage_check_write_path(local_path, false);
    if (ret != ESP_OK) return ret;
    ts_sftp_file_t remote_file = NULL;
    FILE *local_file = NULL;
    uint8_t *buffer = NULL;
    
    /* 获取远程文件大小 */
    ts_sftp_attr_t attrs;
    ret = ts_sftp_stat(sftp, remote_path, &attrs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stat remote file: %s", remote_path);
        return ret;
    }
    
    uint64_t total_size = attrs.size;
    ESP_LOGI(TAG, "Downloading %s (%llu bytes)", remote_path, (unsigned long long)total_size);
    
    /* 打开远程文件 */
    ret = ts_sftp_file_open(sftp, remote_path, TS_SFTP_READ, 0, &remote_file);
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 打开本地文件 */
    ret = ts_storage_open_write(local_path, &local_file);
    if (ret != ESP_OK) goto cleanup;

    /* 分配传输缓冲区 */
    buffer = TS_MALLOC_PSRAM(SFTP_BUFFER_SIZE);
    if (!buffer) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    
    /* 传输数据 */
    uint64_t transferred = 0;
    while (1) {
        size_t bytes_read;
        ret = ts_sftp_file_read(remote_file, buffer, SFTP_BUFFER_SIZE, &bytes_read);
        
        if (ret == ESP_ERR_NOT_FOUND) {
            /* EOF */
            ret = ESP_OK;
            break;
        } else if (ret != ESP_OK) {
            goto cleanup;
        }
        
        size_t written = fwrite(buffer, 1, bytes_read, local_file);
        if (written != bytes_read) {
            ESP_LOGE(TAG, "Local write error");
            ret = ESP_FAIL;
            goto cleanup;
        }
        
        transferred += bytes_read;
        
        if (progress_cb) {
            progress_cb(transferred, total_size, user_data);
        }
        
        /* 让出 CPU */
        taskYIELD();
    }
    
    ESP_LOGI(TAG, "Downloaded %llu bytes to %s", (unsigned long long)transferred, local_path);
    
cleanup:
    if (buffer) free(buffer);
    if (local_file && fclose(local_file) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    if (remote_file) ts_sftp_file_close(remote_file);
    
    return ret;
}

esp_err_t ts_sftp_put(ts_sftp_session_t sftp, const char *local_path,
                       const char *remote_path, ts_sftp_progress_cb_t progress_cb,
                       void *user_data)
{
    if (!sftp || !local_path || !remote_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ESP_FAIL;
    ts_sftp_file_t remote_file = NULL;
    FILE *local_file = NULL;
    uint8_t *buffer = NULL;
    
    /* 打开本地文件 */
    local_file = fopen(local_path, "rb");
    if (!local_file) {
        ESP_LOGE(TAG, "Failed to open local file: %s", local_path);
        return ESP_FAIL;
    }
    
    /* 获取本地文件大小 */
    fseek(local_file, 0, SEEK_END);
    uint64_t total_size = ftell(local_file);
    fseek(local_file, 0, SEEK_SET);
    
    ESP_LOGI(TAG, "Uploading %s (%llu bytes)", local_path, (unsigned long long)total_size);
    
    /* 打开远程文件 */
    ret = ts_sftp_file_open(sftp, remote_path, 
                            TS_SFTP_WRITE | TS_SFTP_CREATE | TS_SFTP_TRUNC,
                            0644, &remote_file);
    if (ret != ESP_OK) {
        fclose(local_file);
        return ret;
    }
    
    /* 分配传输缓冲区 */
    buffer = TS_MALLOC_PSRAM(SFTP_BUFFER_SIZE);
    if (!buffer) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    
    /* 传输数据 */
    uint64_t transferred = 0;
    while (1) {
        size_t bytes_read = fread(buffer, 1, SFTP_BUFFER_SIZE, local_file);
        if (bytes_read == 0) {
            if (feof(local_file)) {
                break;
            }
            ESP_LOGE(TAG, "Local read error");
            ret = ESP_FAIL;
            goto cleanup;
        }
        
        size_t bytes_written;
        ret = ts_sftp_file_write(remote_file, buffer, bytes_read, &bytes_written);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        
        transferred += bytes_written;
        
        if (progress_cb) {
            progress_cb(transferred, total_size, user_data);
        }
        
        /* 让出 CPU */
        taskYIELD();
    }
    
    ESP_LOGI(TAG, "Uploaded %llu bytes to %s", (unsigned long long)transferred, remote_path);
    ret = ESP_OK;
    
cleanup:
    if (buffer) free(buffer);
    if (local_file) fclose(local_file);
    if (remote_file) ts_sftp_file_close(remote_file);
    
    return ret;
}

esp_err_t ts_sftp_get_to_buffer(ts_sftp_session_t sftp, const char *remote_path,
                                 uint8_t **buffer, size_t *size, size_t max_size)
{
    if (!sftp || !remote_path || !buffer || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *buffer = NULL;
    *size = 0;
    
    /* 获取文件大小 */
    ts_sftp_attr_t attrs;
    esp_err_t ret = ts_sftp_stat(sftp, remote_path, &attrs);
    if (ret != ESP_OK) {
        return ret;
    }
    
    if (max_size > 0 && attrs.size > max_size) {
        ESP_LOGE(TAG, "File too large: %llu > %zu", (unsigned long long)attrs.size, max_size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    /* 分配缓冲区 */
    uint8_t *buf = TS_MALLOC_PSRAM(attrs.size);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    
    /* 打开文件 */
    ts_sftp_file_t file;
    ret = ts_sftp_file_open(sftp, remote_path, TS_SFTP_READ, 0, &file);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }
    
    /* 读取数据 */
    size_t total_read = 0;
    while (total_read < attrs.size) {
        size_t bytes_read;
        ret = ts_sftp_file_read(file, buf + total_read, attrs.size - total_read, &bytes_read);
        
        if (ret == ESP_ERR_NOT_FOUND) {
            break;
        } else if (ret != ESP_OK) {
            ts_sftp_file_close(file);
            free(buf);
            return ret;
        }
        
        total_read += bytes_read;
    }
    
    ts_sftp_file_close(file);
    
    *buffer = buf;
    *size = total_read;
    return ESP_OK;
}

esp_err_t ts_sftp_put_from_buffer(ts_sftp_session_t sftp, const uint8_t *buffer,
                                   size_t size, const char *remote_path)
{
    if (!sftp || !buffer || !remote_path) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 打开远程文件 */
    ts_sftp_file_t file;
    esp_err_t ret = ts_sftp_file_open(sftp, remote_path,
                                       TS_SFTP_WRITE | TS_SFTP_CREATE | TS_SFTP_TRUNC,
                                       0644, &file);
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 写入数据 */
    size_t bytes_written;
    ret = ts_sftp_file_write(file, buffer, size, &bytes_written);
    
    ts_sftp_file_close(file);
    return ret;
}
