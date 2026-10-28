/**
 * @file ts_api_sftp.c
 * @brief SFTP API Handlers
 * 
 * 提供 SFTP 文件传输相关的 API 端点：
 * - sftp.ls - 列出远程目录
 * - sftp.get - 下载文件
 * - sftp.put - 上传文件
 * - sftp.rm - 删除文件
 * - sftp.mkdir - 创建目录
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-20
 */

#include "ts_storage.h"
#include "ts_api.h"
#include "ts_ssh_client.h"
#include "ts_sftp.h"
#include "ts_keystore.h"
#include "ts_log.h"
#include <string.h>
#include <stdlib.h>

#define TAG "api_sftp"

/* 静态缓冲区用于存储主机、用户名等 */
static char s_host_buf[64];
static char s_user_buf[64];
static char s_pass_buf[128];
static char *s_key_buf = NULL;
static size_t s_key_len = 0;

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief 清理密钥缓冲区
 */
static void cleanup_key_buffer(void)
{
    if (s_key_buf) {
        memset(s_key_buf, 0, s_key_len);
        free(s_key_buf);
        s_key_buf = NULL;
        s_key_len = 0;
    }
}

/**
 * @brief 从参数中配置 SSH
 */
static esp_err_t configure_ssh_from_params(const cJSON *params, ts_ssh_config_t *config)
{
    const cJSON *host = cJSON_GetObjectItem(params, "host");
    const cJSON *user = cJSON_GetObjectItem(params, "user");
    const cJSON *port = cJSON_GetObjectItem(params, "port");
    
    if (!host || !cJSON_IsString(host) || !user || !cJSON_IsString(user)) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(s_host_buf, host->valuestring, sizeof(s_host_buf) - 1);
    s_host_buf[sizeof(s_host_buf) - 1] = '\0';
    strncpy(s_user_buf, user->valuestring, sizeof(s_user_buf) - 1);
    s_user_buf[sizeof(s_user_buf) - 1] = '\0';
    
    config->host = s_host_buf;
    config->username = s_user_buf;
    config->port = cJSON_IsNumber(port) ? port->valueint : 22;
    
    const cJSON *password = cJSON_GetObjectItem(params, "password");
    const cJSON *keyid = cJSON_GetObjectItem(params, "keyid");
    const cJSON *keypath = cJSON_GetObjectItem(params, "keypath");
    
    cleanup_key_buffer();
    
    if (cJSON_IsString(password) && password->valuestring[0]) {
        strncpy(s_pass_buf, password->valuestring, sizeof(s_pass_buf) - 1);
        s_pass_buf[sizeof(s_pass_buf) - 1] = '\0';
        config->auth_method = TS_SSH_AUTH_PASSWORD;
        config->auth.password = s_pass_buf;
    } else if (cJSON_IsString(keyid) && keyid->valuestring[0]) {
        esp_err_t ret = ts_keystore_load_private_key(keyid->valuestring, &s_key_buf, &s_key_len);
        if (ret != ESP_OK) {
            return ret;
        }
        config->auth_method = TS_SSH_AUTH_PUBLICKEY;
        config->auth.key.private_key = (const uint8_t *)s_key_buf;
        config->auth.key.private_key_len = s_key_len;
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
 * @brief 建立 SFTP 连接
 */
static esp_err_t connect_sftp(const cJSON *params, ts_ssh_session_t *ssh_out, ts_sftp_session_t *sftp_out)
{
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    esp_err_t ret = configure_ssh_from_params(params, &config);
    if (ret != ESP_OK) {
        return ret;
    }
    
    ret = ts_ssh_session_create(&config, ssh_out);
    if (ret != ESP_OK) {
        cleanup_key_buffer();
        return ret;
    }
    
    ret = ts_ssh_connect(*ssh_out);
    if (ret != ESP_OK) {
        ts_ssh_session_destroy(*ssh_out);
        cleanup_key_buffer();
        return ret;
    }
    
    ret = ts_sftp_open(*ssh_out, sftp_out);
    if (ret != ESP_OK) {
        ts_ssh_disconnect(*ssh_out);
        ts_ssh_session_destroy(*ssh_out);
        cleanup_key_buffer();
        return ret;
    }
    
    return ESP_OK;
}

/**
 * @brief 关闭 SFTP 连接
 */
static void close_sftp(ts_ssh_session_t ssh, ts_sftp_session_t sftp)
{
    if (sftp) {
        ts_sftp_close(sftp);
    }
    if (ssh) {
        ts_ssh_disconnect(ssh);
        ts_ssh_session_destroy(ssh);
    }
    cleanup_key_buffer();
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief sftp.ls - List remote directory
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx" | "keyid": "default",
 *   "path": "/home"
 * }
 */
static esp_err_t api_sftp_ls(const cJSON *params, ts_api_result_t *result)
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
    
    ts_ssh_session_t ssh = NULL;
    ts_sftp_session_t sftp = NULL;
    esp_err_t ret = connect_sftp(params, &ssh, &sftp);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_CONNECTION, "Failed to connect");
        return ret;
    }
    
    ts_sftp_dir_t dir = NULL;
    ret = ts_sftp_dir_open(sftp, path->valuestring, &dir);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Failed to open directory");
        close_sftp(ssh, sftp);
        return ret;
    }
    
    cJSON *files = cJSON_CreateArray();
    ts_sftp_dirent_t entry;
    
    while (ts_sftp_dir_read(dir, &entry) == ESP_OK) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry.name);
        cJSON_AddBoolToObject(item, "is_dir", entry.attrs.is_dir);
        cJSON_AddNumberToObject(item, "size", (double)entry.attrs.size);
        cJSON_AddNumberToObject(item, "permissions", entry.attrs.permissions);
        cJSON_AddNumberToObject(item, "mtime", entry.attrs.mtime);
        cJSON_AddItemToArray(files, item);
    }
    
    ts_sftp_dir_close(dir);
    close_sftp(ssh, sftp);
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", path->valuestring);
    cJSON_AddItemToObject(data, "files", files);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief sftp.get - Download file
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx",
 *   "remote": "/remote/path",
 *   "local": "/sdcard/local/path"
 * }
 */
static esp_err_t api_sftp_get(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *remote = cJSON_GetObjectItem(params, "remote");
    const cJSON *local = cJSON_GetObjectItem(params, "local");
    
    if (!remote || !cJSON_IsString(remote) || !local || !cJSON_IsString(local)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'remote' or 'local' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_storage_check_write_path(local->valuestring, false);
    if (ret != ESP_OK) {
        ts_api_result_error(result, ret == ESP_ERR_NOT_ALLOWED ? TS_API_ERR_NO_PERMISSION : TS_API_ERR_INVALID_ARG,
            ret == ESP_ERR_NOT_ALLOWED ? "automation_configuration_managed" : "storage_path_invalid");
        return ret;
    }
    ts_ssh_session_t ssh = NULL;
    ts_sftp_session_t sftp = NULL;
    ret = connect_sftp(params, &ssh, &sftp);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_CONNECTION, "Failed to connect");
        return ret;
    }
    
    ret = ts_sftp_get(sftp, remote->valuestring, local->valuestring, NULL, NULL);
    
    close_sftp(ssh, sftp);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, ret == ESP_ERR_NOT_ALLOWED ? TS_API_ERR_NO_PERMISSION : TS_API_ERR_INTERNAL,
            ret == ESP_ERR_NOT_ALLOWED ? "automation_configuration_managed" : "Download failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "remote", remote->valuestring);
    cJSON_AddStringToObject(data, "local", local->valuestring);
    cJSON_AddBoolToObject(data, "success", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief sftp.put - Upload file
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx",
 *   "local": "/sdcard/local/path",
 *   "remote": "/remote/path"
 * }
 */
static esp_err_t api_sftp_put(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *local = cJSON_GetObjectItem(params, "local");
    const cJSON *remote = cJSON_GetObjectItem(params, "remote");
    
    if (!local || !cJSON_IsString(local) || !remote || !cJSON_IsString(remote)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'local' or 'remote' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    ts_ssh_session_t ssh = NULL;
    ts_sftp_session_t sftp = NULL;
    esp_err_t ret = connect_sftp(params, &ssh, &sftp);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_CONNECTION, "Failed to connect");
        return ret;
    }
    
    ret = ts_sftp_put(sftp, local->valuestring, remote->valuestring, NULL, NULL);
    
    close_sftp(ssh, sftp);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Upload failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "local", local->valuestring);
    cJSON_AddStringToObject(data, "remote", remote->valuestring);
    cJSON_AddBoolToObject(data, "success", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief sftp.rm - Delete remote file
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx",
 *   "path": "/remote/file"
 * }
 */
static esp_err_t api_sftp_rm(const cJSON *params, ts_api_result_t *result)
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
    
    ts_ssh_session_t ssh = NULL;
    ts_sftp_session_t sftp = NULL;
    esp_err_t ret = connect_sftp(params, &ssh, &sftp);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_CONNECTION, "Failed to connect");
        return ret;
    }
    
    ret = ts_sftp_unlink(sftp, path->valuestring);
    
    close_sftp(ssh, sftp);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Delete failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", path->valuestring);
    cJSON_AddBoolToObject(data, "deleted", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief sftp.mkdir - Create remote directory
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx",
 *   "path": "/remote/newdir",
 *   "mode": 0755
 * }
 */
static esp_err_t api_sftp_mkdir(const cJSON *params, ts_api_result_t *result)
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
    
    const cJSON *mode_json = cJSON_GetObjectItem(params, "mode");
    int mode = cJSON_IsNumber(mode_json) ? mode_json->valueint : 0755;
    
    ts_ssh_session_t ssh = NULL;
    ts_sftp_session_t sftp = NULL;
    esp_err_t ret = connect_sftp(params, &ssh, &sftp);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_CONNECTION, "Failed to connect");
        return ret;
    }
    
    ret = ts_sftp_mkdir(sftp, path->valuestring, mode);
    
    close_sftp(ssh, sftp);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "mkdir failed");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", path->valuestring);
    cJSON_AddBoolToObject(data, "created", true);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief sftp.stat - Get file information
 * 
 * Params: { 
 *   "host": "192.168.1.100", 
 *   "user": "root",
 *   "password": "xxx",
 *   "path": "/remote/file"
 * }
 */
static esp_err_t api_sftp_stat(const cJSON *params, ts_api_result_t *result)
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
    
    ts_ssh_session_t ssh = NULL;
    ts_sftp_session_t sftp = NULL;
    esp_err_t ret = connect_sftp(params, &ssh, &sftp);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_CONNECTION, "Failed to connect");
        return ret;
    }
    
    ts_sftp_attr_t attrs;
    ret = ts_sftp_stat(sftp, path->valuestring, &attrs);
    
    close_sftp(ssh, sftp);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Cannot stat file");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", path->valuestring);
    cJSON_AddBoolToObject(data, "is_dir", attrs.is_dir);
    cJSON_AddBoolToObject(data, "is_link", attrs.is_link);
    cJSON_AddNumberToObject(data, "size", (double)attrs.size);
    cJSON_AddNumberToObject(data, "permissions", attrs.permissions);
    cJSON_AddNumberToObject(data, "uid", attrs.uid);
    cJSON_AddNumberToObject(data, "gid", attrs.gid);
    cJSON_AddNumberToObject(data, "atime", attrs.atime);
    cJSON_AddNumberToObject(data, "mtime", attrs.mtime);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t sftp_endpoints[] = {
    {
        .name = "sftp.ls",
        .description = "List remote directory via SFTP",
        .category = TS_API_CAT_SECURITY,
        .handler = api_sftp_ls,
        .requires_auth = true,
    },
    {
        .name = "sftp.get",
        .description = "Download file via SFTP",
        .category = TS_API_CAT_SECURITY,
        .handler = api_sftp_get,
        .requires_auth = true,
    },
    {
        .name = "sftp.put",
        .description = "Upload file via SFTP",
        .category = TS_API_CAT_SECURITY,
        .handler = api_sftp_put,
        .requires_auth = true,
    },
    {
        .name = "sftp.rm",
        .description = "Delete remote file via SFTP",
        .category = TS_API_CAT_SECURITY,
        .handler = api_sftp_rm,
        .requires_auth = true,
    },
    {
        .name = "sftp.mkdir",
        .description = "Create remote directory via SFTP",
        .category = TS_API_CAT_SECURITY,
        .handler = api_sftp_mkdir,
        .requires_auth = true,
    },
    {
        .name = "sftp.stat",
        .description = "Get remote file information via SFTP",
        .category = TS_API_CAT_SECURITY,
        .handler = api_sftp_stat,
        .requires_auth = true,
    },
};

esp_err_t ts_api_sftp_register(void)
{
    TS_LOGI(TAG, "Registering SFTP APIs...");
    return ts_api_register_multiple(sftp_endpoints, 
                                     sizeof(sftp_endpoints) / sizeof(sftp_endpoints[0]));
}
