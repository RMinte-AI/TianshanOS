/**
 * @file ts_api_storage.c
 * @brief Storage API Handlers
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_storage.h"
#include "ts_log.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"

#define TAG "api_storage"

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief storage.status - Get storage status
 */
static esp_err_t api_storage_status(const cJSON *params, ts_api_result_t *result)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    bool spiffs_mounted = ts_storage_spiffs_mounted();
    bool sd_mounted = ts_storage_sd_mounted();
    
    /* SPIFFS info */
    cJSON *spiffs = cJSON_AddObjectToObject(data, "spiffs");
    cJSON_AddBoolToObject(spiffs, "mounted", spiffs_mounted);
    cJSON_AddStringToObject(spiffs, "path", "/spiffs");
    
    /* SD card info */
    cJSON *sd = cJSON_AddObjectToObject(data, "sd");
    cJSON_AddBoolToObject(sd, "mounted", sd_mounted);
    cJSON_AddStringToObject(sd, "path", "/sdcard");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief storage.mount - Mount SD card
 */
static esp_err_t api_storage_mount(const cJSON *params, ts_api_result_t *result)
{
    if (ts_storage_sd_mounted()) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "SD card already mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = ts_storage_mount_sd(NULL);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_HARDWARE, "Failed to mount SD card");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "status", "mounted");
    cJSON_AddStringToObject(data, "path", "/sdcard");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief storage.unmount - Unmount SD card
 */
static esp_err_t api_storage_unmount(const cJSON *params, ts_api_result_t *result)
{
    if (!ts_storage_sd_mounted()) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = ts_storage_unmount_sd();
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to unmount SD card");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "status", "unmounted");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief storage.list - List directory contents
 * 
 * @param params { "path": "/sdcard", "recursive": false }
 */
static esp_err_t api_storage_list(const cJSON *params, ts_api_result_t *result)
{
    const char *path = "/sdcard";
    
    if (params) {
        const cJSON *path_param = cJSON_GetObjectItem(params, "path");
        if (path_param && cJSON_IsString(path_param)) {
            path = path_param->valuestring;
        }
        
        // TODO: 实现递归目录遍历功能
        // const cJSON *recursive_param = cJSON_GetObjectItem(params, "recursive");
        // if (recursive_param && cJSON_IsBool(recursive_param)) {
        //     recursive = cJSON_IsTrue(recursive_param);
        // }
    }
    
    DIR *dir = opendir(path);
    if (!dir) {
        // 检查是否是 SD 卡未挂载
        if (strncmp(path, "/sdcard", 7) == 0 && !ts_storage_sd_mounted()) {
            ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "SD card not mounted");
            return ESP_ERR_NOT_FOUND;
        }
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Directory not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        closedir(dir);
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(data, "path", path);
    cJSON *entries = cJSON_AddArrayToObject(data, "entries");
    
    struct dirent *entry;
    struct stat st;
    char fullpath[512];
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        snprintf(fullpath, sizeof(fullpath), "%.255s/%.255s", path, entry->d_name);
        
        if (stat(fullpath, &st) != 0) {
            continue;
        }
        
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry->d_name);
        cJSON_AddStringToObject(item, "type", S_ISDIR(st.st_mode) ? "dir" : "file");
        if (!S_ISDIR(st.st_mode)) {
            cJSON_AddNumberToObject(item, "size", st.st_size);
        }
        cJSON_AddItemToArray(entries, item);
    }
    
    closedir(dir);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief storage.delete - Delete file or directory
 * @param params { "path": "/sdcard/file.txt" }
 */
static esp_err_t storage_mutation_failure(ts_api_result_t *result, esp_err_t ret, const char *message)
{
    if (ret == ESP_ERR_NOT_ALLOWED) {
        ts_api_result_error(result, TS_API_ERR_NO_PERMISSION, "automation_configuration_managed");
    } else if (ret == ESP_ERR_INVALID_ARG) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "storage_path_invalid");
    } else {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, message);
    }
    return ret;
}

static esp_err_t api_storage_delete(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *path_param = cJSON_GetObjectItem(params, "path");
    if (!path_param || !cJSON_IsString(path_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'path' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *path = path_param->valuestring;
    
    // 安全检查：不允许删除根目录
    if (strcmp(path, "/sdcard") == 0 || strcmp(path, "/spiffs") == 0 || strcmp(path, "/") == 0) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Cannot delete root directory");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_storage_check_write_path(path, true);
    if (ret != ESP_OK) return storage_mutation_failure(result, ret, "Failed to delete");
    if (!ts_storage_exists(path)) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "File not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    if (ts_storage_is_dir(path)) {
        ret = ts_storage_rmdir_r(path);
    } else {
        ret = ts_storage_delete(path);
    }
    
    if (ret != ESP_OK) {
        return storage_mutation_failure(result, ret, "Failed to delete");
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "deleted", path);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief storage.mkdir - Create directory
 * @param params { "path": "/sdcard/newdir" }
 */
static esp_err_t api_storage_mkdir(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *path_param = cJSON_GetObjectItem(params, "path");
    if (!path_param || !cJSON_IsString(path_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'path' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *path = path_param->valuestring;
    
    esp_err_t ret = ts_storage_mkdir_p(path);
    if (ret != ESP_OK) {
        return storage_mutation_failure(result, ret, "Failed to create directory");
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "created", path);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief storage.rename - Rename/move file
 * @param params { "from": "/sdcard/old.txt", "to": "/sdcard/new.txt" }
 */
static esp_err_t api_storage_rename(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *from_param = cJSON_GetObjectItem(params, "from");
    const cJSON *to_param = cJSON_GetObjectItem(params, "to");
    
    if (!from_param || !cJSON_IsString(from_param) || !to_param || !cJSON_IsString(to_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'from' or 'to' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *from = from_param->valuestring;
    const char *to = to_param->valuestring;
    
    esp_err_t ret = ts_storage_check_write_path(from, true);
    if (ret == ESP_OK) ret = ts_storage_check_write_path(to, true);
    if (ret != ESP_OK) return storage_mutation_failure(result, ret, "Failed to rename");
    if (!ts_storage_exists(from)) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "Source file not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ret = ts_storage_rename(from, to);
    if (ret != ESP_OK) {
        return storage_mutation_failure(result, ret, "Failed to rename");
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "from", from);
    cJSON_AddStringToObject(data, "to", to);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief storage.info - Get file/directory info
 * @param params { "path": "/sdcard/file.txt" }
 */
static esp_err_t api_storage_info(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *path_param = cJSON_GetObjectItem(params, "path");
    if (!path_param || !cJSON_IsString(path_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'path' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *path = path_param->valuestring;
    
    ts_file_info_t info;
    esp_err_t ret = ts_storage_stat(path, &info);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "File not found");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", path);
    cJSON_AddStringToObject(data, "name", info.name);
    cJSON_AddStringToObject(data, "type", info.is_directory ? "dir" : "file");
    cJSON_AddNumberToObject(data, "size", info.size);
    cJSON_AddNumberToObject(data, "modified", info.modified);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief storage.read - Read file content (text files only, max 8KB)
 * @param params { "path": "/sdcard/config/net.json" }
 */
static esp_err_t api_storage_read(const cJSON *params, ts_api_result_t *result)
{
    if (!params) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    const cJSON *path_param = cJSON_GetObjectItem(params, "path");
    if (!path_param || !cJSON_IsString(path_param)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Missing 'path' parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *path = path_param->valuestring;
    
    // 检查文件是否存在
    if (!ts_storage_exists(path)) {
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND, "File not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    // 检查是否是目录
    if (ts_storage_is_dir(path)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "Cannot read directory");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 获取文件大小
    ts_file_info_t info;
    esp_err_t ret = ts_storage_stat(path, &info);
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to stat file");
        return ret;
    }
    
    // 限制文件大小（最大 8KB）
    const size_t max_size = 8192;
    if (info.size > max_size) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG, "File too large (max 8KB)");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 分配缓冲区
    char *content = heap_caps_malloc(info.size + 1, MALLOC_CAP_SPIRAM);
    if (!content) {
        content = malloc(info.size + 1);
    }
    if (!content) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    // 读取文件
    FILE *f = fopen(path, "r");
    if (!f) {
        free(content);
        ts_api_result_error(result, TS_API_ERR_INTERNAL, "Failed to open file");
        return ESP_FAIL;
    }
    
    size_t read_size = fread(content, 1, info.size, f);
    fclose(f);
    content[read_size] = '\0';
    
    // 构建结果
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", path);
    cJSON_AddNumberToObject(data, "size", read_size);
    
    // 尝试解析为 JSON
    cJSON *json_content = cJSON_Parse(content);
    if (json_content) {
        cJSON_AddItemToObject(data, "content", json_content);
        cJSON_AddStringToObject(data, "type", "json");
    } else {
        cJSON_AddStringToObject(data, "content", content);
        cJSON_AddStringToObject(data, "type", "text");
    }
    
    free(content);
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

static const ts_api_endpoint_t s_storage_endpoints[] = {
    {
        .name = "storage.status",
        .description = "Get storage status",
        .category = TS_API_CAT_STORAGE,
        .handler = api_storage_status,
        .requires_auth = false
    },
    {
        .name = "storage.mount",
        .description = "Mount SD card",
        .category = TS_API_CAT_STORAGE,
        .handler = api_storage_mount,
        .requires_auth = false
    },
    {
        .name = "storage.unmount",
        .description = "Unmount SD card",
        .category = TS_API_CAT_STORAGE,
        .handler = api_storage_unmount,
        .requires_auth = false
    },
    {
        .name = "storage.list",
        .description = "List directory contents",
        .category = TS_API_CAT_STORAGE,
        .handler = api_storage_list,
        .requires_auth = false
    },
    {
        .name = "storage.delete",
        .description = "Delete file or directory",
        .category = TS_API_CAT_STORAGE,
        .handler = api_storage_delete,
        .requires_auth = false
    },
    {
        .name = "storage.mkdir",
        .description = "Create directory",
        .category = TS_API_CAT_STORAGE,
        .handler = api_storage_mkdir,
        .requires_auth = false
    },
    {
        .name = "storage.rename",
        .description = "Rename/move file",
        .category = TS_API_CAT_STORAGE,
        .handler = api_storage_rename,
        .requires_auth = false
    },
    {
        .name = "storage.info",
        .description = "Get file info",
        .category = TS_API_CAT_STORAGE,
        .handler = api_storage_info,
        .requires_auth = false
    },
    {
        .name = "storage.read",
        .description = "Read file content",
        .category = TS_API_CAT_STORAGE,
        .handler = api_storage_read,
        .requires_auth = false
    }
};

esp_err_t ts_api_storage_register(void)
{
    TS_LOGI(TAG, "Registering storage APIs...");
    
    esp_err_t ret = ts_api_register_multiple(
        s_storage_endpoints, 
        sizeof(s_storage_endpoints) / sizeof(s_storage_endpoints[0])
    );
    
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Storage APIs registered (%zu endpoints)", 
            sizeof(s_storage_endpoints) / sizeof(s_storage_endpoints[0]));
    }
    
    return ret;
}
