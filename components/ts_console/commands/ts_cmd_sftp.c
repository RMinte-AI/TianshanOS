/**
 * @file ts_cmd_sftp.c
 * @brief SFTP Console Commands
 * 
 * 实现 sftp 命令：
 * - sftp --ls     远程目录列表
 * - sftp --get    下载文件
 * - sftp --put    上传文件
 * - sftp --rm     删除文件
 * - sftp --mkdir  创建目录
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 */

#include "ts_storage.h"
#include "ts_console.h"
#include "ts_api.h"
#include "ts_log.h"
#include "ts_ssh_client.h"
#include "ts_sftp.h"
#include "ts_scp.h"
#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define TAG "cmd_sftp"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    /* 连接参数 */
    struct arg_str *host;       /**< SSH 服务器地址 */
    struct arg_int *port;       /**< SSH 端口 (默认 22) */
    struct arg_str *user;       /**< 用户名 */
    struct arg_str *password;   /**< 密码 */
    struct arg_int *timeout;    /**< 超时时间（秒） */
    
    /* 操作类型 */
    struct arg_lit *ls;         /**< 列出目录 */
    struct arg_lit *get;        /**< 下载文件 */
    struct arg_lit *put;        /**< 上传文件 */
    struct arg_lit *rm;         /**< 删除文件 */
    struct arg_lit *mkdir;      /**< 创建目录 */
    struct arg_lit *stat;       /**< 查看文件信息 */
    struct arg_lit *scp;        /**< 使用 SCP 协议（更简单快速） */
    
    /* 文件路径参数 */
    struct arg_str *remote;     /**< 远程路径 */
    struct arg_str *local;      /**< 本地路径 */
    
    /* 其他选项 */
    struct arg_lit *verbose;    /**< 详细输出 */
    struct arg_lit *progress;   /**< 显示进度 */
    struct arg_lit *help;
    struct arg_end *end;
} s_sftp_args;

/*===========================================================================*/
/*                          进度显示回调                                      */
/*===========================================================================*/

typedef struct {
    uint64_t last_update_time;
    uint64_t last_transferred;
    bool verbose;
} progress_ctx_t;

static void progress_callback(uint64_t transferred, uint64_t total, void *user_data)
{
    progress_ctx_t *ctx = (progress_ctx_t *)user_data;
    if (!ctx || !ctx->verbose) {
        return;
    }
    
    /* 限制更新频率 */
    uint64_t now = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - ctx->last_update_time < 500) {
        return;
    }
    ctx->last_update_time = now;
    
    /* 计算百分比 */
    int percent = (total > 0) ? (int)(transferred * 100 / total) : 0;
    
    /* 计算速度 */
    float speed = 0;
    if (now > 0 && transferred > ctx->last_transferred) {
        uint64_t elapsed_ms = 500;  /* 假设 500ms */
        speed = (float)(transferred - ctx->last_transferred) / elapsed_ms * 1000 / 1024;
    }
    ctx->last_transferred = transferred;
    
    /* 进度条 */
    ts_console_printf("\r  [");
    int bar_width = 30;
    int filled = percent * bar_width / 100;
    for (int i = 0; i < bar_width; i++) {
        ts_console_printf(i < filled ? "=" : " ");
    }
    ts_console_printf("] %3d%% %.1f KB/s", percent, speed);
}

/*===========================================================================*/
/*                          SSH 连接辅助函数                                  */
/*===========================================================================*/

static esp_err_t connect_ssh(const char *host, int port, const char *user,
                             const char *password, int timeout_sec,
                             ts_ssh_session_t *session_out)
{
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    config.host = host;
    config.port = port;
    config.username = user;
    config.auth_method = TS_SSH_AUTH_PASSWORD;
    config.auth.password = password;
    config.timeout_ms = timeout_sec * 1000;
    
    esp_err_t ret = ts_ssh_session_create(&config, session_out);
    if (ret != ESP_OK) {
        return ret;
    }
    
    ret = ts_ssh_connect(*session_out);
    if (ret != ESP_OK) {
        ts_ssh_session_destroy(*session_out);
        *session_out = NULL;
    }
    
    return ret;
}

/*===========================================================================*/
/*                          SFTP 操作实现                                     */
/*===========================================================================*/

/**
 * @brief 列出远程目录（通过 API）
 * 
 * 使用 ts_api_call("sftp.ls") 实现业务逻辑分离
 */
static int do_sftp_ls(const char *host, int port, const char *user,
                      const char *password, const char *path, int timeout_sec)
{
    ts_console_printf("Connecting to %s@%s...\n", user, host);
    
    /* 构建 API 参数 */
    cJSON *params = cJSON_CreateObject();
    if (!params) {
        ts_console_printf("Error: Out of memory\n");
        return 1;
    }
    
    cJSON_AddStringToObject(params, "host", host);
    cJSON_AddNumberToObject(params, "port", port);
    cJSON_AddStringToObject(params, "user", user);
    cJSON_AddStringToObject(params, "password", password);
    cJSON_AddNumberToObject(params, "timeout", timeout_sec);
    cJSON_AddStringToObject(params, "path", path);
    
    /* 调用 API */
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("sftp.ls", params, &result);
    cJSON_Delete(params);
    
    if (ret != ESP_OK || result.code != TS_API_OK) {
        ts_console_printf("Error: %s\n", result.message ? result.message : "SFTP operation failed");
        ts_api_result_free(&result);
        return 1;
    }
    
    /* 格式化输出 */
    ts_console_printf("\nDirectory: %s\n", path);
    ts_console_printf("═══════════════════════════════════════════════════════════════\n");
    
    cJSON *files = cJSON_GetObjectItem(result.data, "files");
    int count = 0;
    
    if (files && cJSON_IsArray(files)) {
        cJSON *file = NULL;
        cJSON_ArrayForEach(file, files) {
            cJSON *name = cJSON_GetObjectItem(file, "name");
            cJSON *is_dir = cJSON_GetObjectItem(file, "is_dir");
            cJSON *is_link = cJSON_GetObjectItem(file, "is_link");
            cJSON *size_obj = cJSON_GetObjectItem(file, "size");
            cJSON *perms = cJSON_GetObjectItem(file, "permissions");
            cJSON *uid_obj = cJSON_GetObjectItem(file, "uid");
            cJSON *gid_obj = cJSON_GetObjectItem(file, "gid");
            
            char type = '-';
            if (is_dir && cJSON_IsTrue(is_dir)) {
                type = 'd';
            } else if (is_link && cJSON_IsTrue(is_link)) {
                type = 'l';
            }
            
            /* 格式化权限 */
            char perms_str[11] = "----------";
            perms_str[0] = type;
            uint32_t p = perms ? (uint32_t)cJSON_GetNumberValue(perms) : 0;
            if (p & 0400) perms_str[1] = 'r';
            if (p & 0200) perms_str[2] = 'w';
            if (p & 0100) perms_str[3] = 'x';
            if (p & 0040) perms_str[4] = 'r';
            if (p & 0020) perms_str[5] = 'w';
            if (p & 0010) perms_str[6] = 'x';
            if (p & 0004) perms_str[7] = 'r';
            if (p & 0002) perms_str[8] = 'w';
            if (p & 0001) perms_str[9] = 'x';
            
            /* 格式化大小 */
            char size_str[16];
            uint64_t sz = size_obj ? (uint64_t)cJSON_GetNumberValue(size_obj) : 0;
            if (sz >= 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1fM", (float)sz / (1024 * 1024));
            } else if (sz >= 1024) {
                snprintf(size_str, sizeof(size_str), "%.1fK", (float)sz / 1024);
            } else {
                snprintf(size_str, sizeof(size_str), "%llu", (unsigned long long)sz);
            }
            
            uint32_t uid = uid_obj ? (uint32_t)cJSON_GetNumberValue(uid_obj) : 0;
            uint32_t gid = gid_obj ? (uint32_t)cJSON_GetNumberValue(gid_obj) : 0;
            const char *fname = name ? cJSON_GetStringValue(name) : "?";
            
            ts_console_printf("%s %5u %5u %8s %s%s\n",
                              perms_str, uid, gid, size_str, fname,
                              (is_dir && cJSON_IsTrue(is_dir)) ? "/" : "");
            count++;
        }
    }
    
    ts_console_printf("═══════════════════════════════════════════════════════════════\n");
    ts_console_printf("Total: %d items\n", count);
    
    ts_api_result_free(&result);
    return 0;
}

/**
 * @brief 下载文件 (SFTP 或 SCP)
 * 
 * 注意：保持直接调用底层 SFTP/SCP 函数，不通过 API 层
 * 原因：需要进度回调实时更新 UI，API 的请求-响应模式不适合
 */
static int do_file_get(const char *host, int port, const char *user,
                       const char *password, const char *remote_path,
                       const char *local_path, int timeout_sec, bool use_scp, bool verbose)
{
    esp_err_t ret = ts_storage_check_write_path(local_path, false);
    if (ret != ESP_OK) {
        ts_console_printf(ret == ESP_ERR_NOT_ALLOWED
            ? "This file belongs to Automation. Choose another download path.\n"
            : "Invalid local download path.\n");
        return 1;
    }
    ts_ssh_session_t ssh = NULL;

    if (verbose) {
        ts_console_printf("Connecting to %s@%s...\n", user, host);
    }
    
    ret = connect_ssh(host, port, user, password, timeout_sec, &ssh);
    if (ret != ESP_OK) {
        ts_console_printf("Error: SSH connection failed\n");
        return 1;
    }
    
    progress_ctx_t progress_ctx = {
        .last_update_time = 0,
        .last_transferred = 0,
        .verbose = verbose
    };
    
    if (verbose) {
        ts_console_printf("Downloading: %s -> %s\n", remote_path, local_path);
        ts_console_printf("Protocol: %s\n\n", use_scp ? "SCP" : "SFTP");
    }
    
    if (use_scp) {
        /* 使用 SCP */
        ret = ts_scp_recv(ssh, remote_path, local_path,
                          verbose ? progress_callback : NULL, &progress_ctx);
    } else {
        /* 使用 SFTP */
        ts_sftp_session_t sftp = NULL;
        ret = ts_sftp_open(ssh, &sftp);
        if (ret == ESP_OK) {
            ret = ts_sftp_get(sftp, remote_path, local_path,
                              verbose ? progress_callback : NULL, &progress_ctx);
            ts_sftp_close(sftp);
        }
    }
    
    ts_ssh_disconnect(ssh);
    ts_ssh_session_destroy(ssh);
    
    if (verbose) {
        ts_console_printf("\n");
    }
    
    if (ret == ESP_OK) {
        ts_console_printf("✓ Download complete: %s\n", local_path);
        return 0;
    } else {
        ts_console_printf(ret == ESP_ERR_NOT_ALLOWED
            ? "This file belongs to Automation. Choose another download path.\n" : "✗ Download failed\n");
        return 1;
    }
}

/**
 * @brief 上传文件 (SFTP 或 SCP)
 * 
 * 注意：保持直接调用底层 SFTP/SCP 函数，不通过 API 层
 * 原因：需要进度回调实时更新 UI，API 的请求-响应模式不适合
 */
static int do_file_put(const char *host, int port, const char *user,
                       const char *password, const char *local_path,
                       const char *remote_path, int timeout_sec, bool use_scp, bool verbose)
{
    ts_ssh_session_t ssh = NULL;
    esp_err_t ret;
    
    if (verbose) {
        ts_console_printf("Connecting to %s@%s...\n", user, host);
    }
    
    ret = connect_ssh(host, port, user, password, timeout_sec, &ssh);
    if (ret != ESP_OK) {
        ts_console_printf("Error: SSH connection failed\n");
        return 1;
    }
    
    progress_ctx_t progress_ctx = {
        .last_update_time = 0,
        .last_transferred = 0,
        .verbose = verbose
    };
    
    if (verbose) {
        ts_console_printf("Uploading: %s -> %s\n", local_path, remote_path);
        ts_console_printf("Protocol: %s\n\n", use_scp ? "SCP" : "SFTP");
    }
    
    if (use_scp) {
        /* 使用 SCP */
        ret = ts_scp_send(ssh, local_path, remote_path, 0644,
                          verbose ? progress_callback : NULL, &progress_ctx);
    } else {
        /* 使用 SFTP */
        ts_sftp_session_t sftp = NULL;
        ret = ts_sftp_open(ssh, &sftp);
        if (ret == ESP_OK) {
            ret = ts_sftp_put(sftp, local_path, remote_path,
                              verbose ? progress_callback : NULL, &progress_ctx);
            ts_sftp_close(sftp);
        }
    }
    
    ts_ssh_disconnect(ssh);
    ts_ssh_session_destroy(ssh);
    
    if (verbose) {
        ts_console_printf("\n");
    }
    
    if (ret == ESP_OK) {
        ts_console_printf("✓ Upload complete: %s\n", remote_path);
        return 0;
    } else {
        ts_console_printf("✗ Upload failed\n");
        return 1;
    }
}

/**
 * @brief 删除远程文件（通过 API）
 * 
 * 使用 ts_api_call("sftp.rm") 实现业务逻辑分离
 */
static int do_sftp_rm(const char *host, int port, const char *user,
                      const char *password, const char *path, int timeout_sec)
{
    /* 构建 API 参数 */
    cJSON *params = cJSON_CreateObject();
    if (!params) {
        ts_console_printf("Error: Out of memory\n");
        return 1;
    }
    
    cJSON_AddStringToObject(params, "host", host);
    cJSON_AddNumberToObject(params, "port", port);
    cJSON_AddStringToObject(params, "user", user);
    cJSON_AddStringToObject(params, "password", password);
    cJSON_AddNumberToObject(params, "timeout", timeout_sec);
    cJSON_AddStringToObject(params, "path", path);
    
    /* 调用 API */
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("sftp.rm", params, &result);
    cJSON_Delete(params);
    
    if (ret == ESP_OK && result.code == TS_API_OK) {
        ts_console_printf("✓ Deleted: %s\n", path);
        ts_api_result_free(&result);
        return 0;
    } else {
        ts_console_printf("✗ Failed to delete: %s\n", path);
        ts_api_result_free(&result);
        return 1;
    }
}

/**
 * @brief 创建远程目录（通过 API）
 * 
 * 使用 ts_api_call("sftp.mkdir") 实现业务逻辑分离
 */
static int do_sftp_mkdir(const char *host, int port, const char *user,
                         const char *password, const char *path, int timeout_sec)
{
    /* 构建 API 参数 */
    cJSON *params = cJSON_CreateObject();
    if (!params) {
        ts_console_printf("Error: Out of memory\n");
        return 1;
    }
    
    cJSON_AddStringToObject(params, "host", host);
    cJSON_AddNumberToObject(params, "port", port);
    cJSON_AddStringToObject(params, "user", user);
    cJSON_AddStringToObject(params, "password", password);
    cJSON_AddNumberToObject(params, "timeout", timeout_sec);
    cJSON_AddStringToObject(params, "path", path);
    
    /* 调用 API */
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("sftp.mkdir", params, &result);
    cJSON_Delete(params);
    
    if (ret == ESP_OK && result.code == TS_API_OK) {
        ts_console_printf("✓ Created directory: %s\n", path);
        ts_api_result_free(&result);
        return 0;
    } else {
        ts_console_printf("✗ Failed to create directory: %s\n", path);
        ts_api_result_free(&result);
        return 1;
    }
}

/**
 * @brief 显示文件信息（通过 API）
 * 
 * 使用 ts_api_call("sftp.stat") 实现业务逻辑分离
 */
static int do_sftp_stat(const char *host, int port, const char *user,
                        const char *password, const char *path, int timeout_sec)
{
    /* 构建 API 参数 */
    cJSON *params = cJSON_CreateObject();
    if (!params) {
        ts_console_printf("Error: Out of memory\n");
        return 1;
    }
    
    cJSON_AddStringToObject(params, "host", host);
    cJSON_AddNumberToObject(params, "port", port);
    cJSON_AddStringToObject(params, "user", user);
    cJSON_AddStringToObject(params, "password", password);
    cJSON_AddNumberToObject(params, "timeout", timeout_sec);
    cJSON_AddStringToObject(params, "path", path);
    
    /* 调用 API */
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("sftp.stat", params, &result);
    cJSON_Delete(params);
    
    if (ret != ESP_OK || result.code != TS_API_OK) {
        ts_console_printf("Error: Cannot stat file\n");
        ts_api_result_free(&result);
        return 1;
    }
    
    /* 解析结果 */
    cJSON *is_dir = cJSON_GetObjectItem(result.data, "is_dir");
    cJSON *is_link = cJSON_GetObjectItem(result.data, "is_link");
    cJSON *size_obj = cJSON_GetObjectItem(result.data, "size");
    cJSON *perms = cJSON_GetObjectItem(result.data, "permissions");
    cJSON *uid_obj = cJSON_GetObjectItem(result.data, "uid");
    cJSON *gid_obj = cJSON_GetObjectItem(result.data, "gid");
    
    const char *type_str = "File";
    if (is_dir && cJSON_IsTrue(is_dir)) {
        type_str = "Directory";
    } else if (is_link && cJSON_IsTrue(is_link)) {
        type_str = "Symlink";
    }
    
    uint64_t size = size_obj ? (uint64_t)cJSON_GetNumberValue(size_obj) : 0;
    uint32_t perm = perms ? (uint32_t)cJSON_GetNumberValue(perms) : 0;
    uint32_t uid = uid_obj ? (uint32_t)cJSON_GetNumberValue(uid_obj) : 0;
    uint32_t gid = gid_obj ? (uint32_t)cJSON_GetNumberValue(gid_obj) : 0;
    
    ts_console_printf("\nFile: %s\n", path);
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Type:        %s\n", type_str);
    ts_console_printf("  Size:        %llu bytes\n", (unsigned long long)size);
    ts_console_printf("  Permissions: %04o\n", perm & 07777);
    ts_console_printf("  UID/GID:     %u/%u\n", uid, gid);
    ts_console_printf("═══════════════════════════════════════\n");
    
    ts_api_result_free(&result);
    return 0;
}

/*===========================================================================*/
/*                          Command Handler                                   */
/*===========================================================================*/

static int sftp_cmd_handler(int argc, char **argv)
{
    /* 解析参数 */
    int nerrors = arg_parse(argc, argv, (void **)&s_sftp_args);
    
    /* 显示帮助 */
    if (s_sftp_args.help->count > 0) {
        ts_console_printf("\nUsage: sftp [options]\n\n");
        ts_console_printf("SFTP/SCP file transfer client\n\n");
        ts_console_printf("Connection Options:\n");
        ts_console_printf("  --host <ip>       Remote host address (required)\n");
        ts_console_printf("  --port <num>      SSH port (default: 22)\n");
        ts_console_printf("  --user <name>     Username (required)\n");
        ts_console_printf("  --password <pwd>  Password (required)\n");
        ts_console_printf("  --timeout <sec>   Connection timeout (default: 10)\n");
        ts_console_printf("\nOperations (choose one):\n");
        ts_console_printf("  --ls              List remote directory\n");
        ts_console_printf("  --get             Download file (remote -> local)\n");
        ts_console_printf("  --put             Upload file (local -> remote)\n");
        ts_console_printf("  --rm              Delete remote file\n");
        ts_console_printf("  --mkdir           Create remote directory\n");
        ts_console_printf("  --stat            Show file information\n");
        ts_console_printf("\nFile Options:\n");
        ts_console_printf("  --remote <path>   Remote file/directory path\n");
        ts_console_printf("  --local <path>    Local file path (SD card)\n");
        ts_console_printf("  --scp             Use SCP protocol (faster for single files)\n");
        ts_console_printf("  --verbose, -v     Show progress and details\n");
        ts_console_printf("\nExamples:\n");
        ts_console_printf("  sftp --ls --host 192.168.1.100 --user root --password root --remote /home\n");
        ts_console_printf("  sftp --get --host 192.168.1.100 --user root --password root --remote /var/log/syslog --local /sdcard/syslog.txt -v\n");
        ts_console_printf("  sftp --put --host 192.168.1.100 --user root --password root --local /sdcard/config.json --remote /tmp/config.json --scp\n");
        ts_console_printf("  sftp --rm --host 192.168.1.100 --user root --password root --remote /tmp/test.txt\n");
        return 0;
    }
    
    /* 参数错误检查 */
    if (nerrors > 0) {
        arg_print_errors(stderr, s_sftp_args.end, "sftp");
        ts_console_printf("Use 'sftp --help' for usage information\n");
        return 1;
    }
    
    /* 必需参数检查 */
    if (s_sftp_args.host->count == 0) {
        ts_console_printf("Error: --host is required\n");
        return 1;
    }
    if (s_sftp_args.user->count == 0) {
        ts_console_printf("Error: --user is required\n");
        return 1;
    }
    if (s_sftp_args.password->count == 0) {
        ts_console_printf("Error: --password is required\n");
        return 1;
    }
    
    /* 获取连接参数 */
    const char *host = s_sftp_args.host->sval[0];
    int port = (s_sftp_args.port->count > 0) ? s_sftp_args.port->ival[0] : 22;
    const char *user = s_sftp_args.user->sval[0];
    const char *password = s_sftp_args.password->sval[0];
    int timeout = (s_sftp_args.timeout->count > 0) ? s_sftp_args.timeout->ival[0] : 10;
    bool verbose = (s_sftp_args.verbose->count > 0) || (s_sftp_args.progress->count > 0);
    bool use_scp = (s_sftp_args.scp->count > 0);
    
    /* 执行操作 */
    if (s_sftp_args.ls->count > 0) {
        const char *path = (s_sftp_args.remote->count > 0) ? s_sftp_args.remote->sval[0] : "/";
        return do_sftp_ls(host, port, user, password, path, timeout);
    } 
    else if (s_sftp_args.get->count > 0) {
        if (s_sftp_args.remote->count == 0) {
            ts_console_printf("Error: --remote is required for --get\n");
            return 1;
        }
        if (s_sftp_args.local->count == 0) {
            ts_console_printf("Error: --local is required for --get\n");
            return 1;
        }
        return do_file_get(host, port, user, password, 
                           s_sftp_args.remote->sval[0], s_sftp_args.local->sval[0],
                           timeout, use_scp, verbose);
    }
    else if (s_sftp_args.put->count > 0) {
        if (s_sftp_args.local->count == 0) {
            ts_console_printf("Error: --local is required for --put\n");
            return 1;
        }
        if (s_sftp_args.remote->count == 0) {
            ts_console_printf("Error: --remote is required for --put\n");
            return 1;
        }
        return do_file_put(host, port, user, password,
                           s_sftp_args.local->sval[0], s_sftp_args.remote->sval[0],
                           timeout, use_scp, verbose);
    }
    else if (s_sftp_args.rm->count > 0) {
        if (s_sftp_args.remote->count == 0) {
            ts_console_printf("Error: --remote is required for --rm\n");
            return 1;
        }
        return do_sftp_rm(host, port, user, password, s_sftp_args.remote->sval[0], timeout);
    }
    else if (s_sftp_args.mkdir->count > 0) {
        if (s_sftp_args.remote->count == 0) {
            ts_console_printf("Error: --remote is required for --mkdir\n");
            return 1;
        }
        return do_sftp_mkdir(host, port, user, password, s_sftp_args.remote->sval[0], timeout);
    }
    else if (s_sftp_args.stat->count > 0) {
        if (s_sftp_args.remote->count == 0) {
            ts_console_printf("Error: --remote is required for --stat\n");
            return 1;
        }
        return do_sftp_stat(host, port, user, password, s_sftp_args.remote->sval[0], timeout);
    }
    else {
        ts_console_printf("Error: Operation required (--ls, --get, --put, --rm, --mkdir, --stat)\n");
        ts_console_printf("Use 'sftp --help' for usage information\n");
        return 1;
    }
}

/*===========================================================================*/
/*                          Command Registration                              */
/*===========================================================================*/

esp_err_t ts_cmd_sftp_register(void)
{
    /* 连接参数 */
    s_sftp_args.host = arg_str0(NULL, "host", "<ip>", "Remote host address");
    s_sftp_args.port = arg_int0(NULL, "port", "<num>", "SSH port (default: 22)");
    s_sftp_args.user = arg_str0(NULL, "user", "<name>", "Username");
    s_sftp_args.password = arg_str0(NULL, "password", "<pwd>", "Password");
    s_sftp_args.timeout = arg_int0(NULL, "timeout", "<sec>", "Timeout in seconds");
    
    /* 操作类型 */
    s_sftp_args.ls = arg_lit0(NULL, "ls", "List remote directory");
    s_sftp_args.get = arg_lit0(NULL, "get", "Download file");
    s_sftp_args.put = arg_lit0(NULL, "put", "Upload file");
    s_sftp_args.rm = arg_lit0(NULL, "rm", "Delete remote file");
    s_sftp_args.mkdir = arg_lit0(NULL, "mkdir", "Create directory");
    s_sftp_args.stat = arg_lit0(NULL, "stat", "Show file info");
    s_sftp_args.scp = arg_lit0(NULL, "scp", "Use SCP protocol");
    
    /* 文件路径 */
    s_sftp_args.remote = arg_str0(NULL, "remote", "<path>", "Remote path");
    s_sftp_args.local = arg_str0(NULL, "local", "<path>", "Local path");
    
    /* 其他选项 */
    s_sftp_args.verbose = arg_lit0("v", "verbose", "Verbose output");
    s_sftp_args.progress = arg_lit0("p", "progress", "Show progress");
    s_sftp_args.help = arg_lit0("h", "help", "Show help");
    s_sftp_args.end = arg_end(8);
    
    /* 注册命令 */
    const esp_console_cmd_t cmd = {
        .command = "sftp",
        .help = "SFTP/SCP file transfer. Use 'sftp --help' for details.",
        .hint = NULL,
        .func = sftp_cmd_handler,
        .argtable = &s_sftp_args,
    };
    
    esp_err_t ret = esp_console_cmd_register(&cmd);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register sftp command: %s", esp_err_to_name(ret));
    } else {
        TS_LOGI(TAG, "Registered command: sftp");
    }
    
    return ret;
}
