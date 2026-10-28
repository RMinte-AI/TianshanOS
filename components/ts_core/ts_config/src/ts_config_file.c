/**
 * @file ts_config_file.c
 * @brief TianShanOS Configuration - File Backend Implementation
 *
 * 文件系统配置后端实现
 * 支持从 SD 卡或 SPIFFS 读取 JSON 配置文件
 * 
 * 架构说明:
 * - 系统启动时，此后端在 ts_core 初始化阶段注册
 * - 此时 SD 卡尚未挂载，所以初始化时静默跳过目录检查
 * - 监听 TS_EVT_STORAGE_SD_MOUNTED 事件，收到后自动加载配置
 * - 这种事件驱动架构确保配置在 SD 卡挂载后自动加载
 *
 * @author TianShanOS Team
 * @version 0.2.0
 */

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include "ts_config.h"
#include "ts_config_file.h"
#include "ts_config_module.h"
#include "ts_config_pack.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_heap_caps.h"

static const char *TAG = "ts_config_file";

/* ============================================================================
 * 私有变量
 * ========================================================================== */

#ifndef CONFIG_TS_CONFIG_FILE_PATH
#define CONFIG_TS_CONFIG_FILE_PATH "/sdcard/config"
#endif

static char s_config_path[128] = CONFIG_TS_CONFIG_FILE_PATH;
static bool s_file_initialized = false;

/* ============================================================================
 * 私有函数声明
 * ========================================================================== */

static bool path_exists(const char *path);
static bool ensure_directory(const char *path);

/**
 * @brief 检查文件是否属于 schema-less 模块（这些模块有自己的加载逻辑）
 * 
 * Schema-less 模块（如 rules, actions, sources, temp）使用自定义的 NVS 存储
 * 格式，不应该通过通用配置系统加载其 JSON 文件。
 * 
 * @param filename 文件名（如 "rules.json"）
 * @return true 如果是 schema-less 模块的文件，应该跳过
 */
static bool is_schemaless_module_file(const char *filename)
{
    /* Schema-less 模块的配置文件列表 */
    static const char *schemaless_files[] = {
        "rules.json",
        "actions.json", 
        "sources.json",
        "temp.json",
        "ssh_commands.json",
        "ssh_hosts.json",
        NULL
    };
    
    for (int i = 0; schemaless_files[i] != NULL; i++) {
        if (strcmp(filename, schemaless_files[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * 后端操作函数
 * ========================================================================== */

/**
 * @brief 初始化文件后端
 * 
 * 注意：此函数在系统启动早期调用，此时 SD 卡通常尚未挂载，
 * 事件系统也可能未初始化。因此只进行基础初始化。
 * 导入和导出只由明确的用户操作调用。
 */
static esp_err_t file_backend_init(void)
{
    if (s_file_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing file configuration backend...");
    ESP_LOGD(TAG, "Explicit import/export path: %s", s_config_path);

    s_file_initialized = true;
    ESP_LOGI(TAG, "File import/export backend initialized");
    return ESP_OK;
}

/**
 * @brief 反初始化文件后端
 */
static esp_err_t file_backend_deinit(void)
{
    s_file_initialized = false;
    ESP_LOGI(TAG, "File backend deinitialized");
    return ESP_OK;
}

/**
 * @brief 从文件读取配置
 * 
 * 文件后端不支持按键读取单个值，而是加载整个配置文件
 */
static esp_err_t file_backend_get(const char *key, ts_config_type_t type,
                                   ts_config_value_t *value, size_t *size)
{
    // 文件后端主要用于批量加载，单独读取返回不支持
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 写入配置到文件
 *
 * 文件后端不支持按键写入单个值
 */
static esp_err_t file_backend_set(const char *key, ts_config_type_t type,
                                   const ts_config_value_t *value, size_t size)
{
    // 文件后端主要用于批量保存，单独写入返回不支持
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 从文件删除配置
 */
static esp_err_t file_backend_erase(const char *key)
{
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 检查文件中是否存在配置
 */
static esp_err_t file_backend_exists(const char *key, bool *exists)
{
    *exists = false;
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 清空配置目录
 */
static esp_err_t file_backend_clear(void)
{
    // TODO: 实现清空配置目录
    return ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 提交文件更改
 */
static esp_err_t file_backend_commit(void)
{
    // 文件写入是即时的，无需额外提交
    return ESP_OK;
}

/* ============================================================================
 * 后端操作函数集
 * ========================================================================== */

static const ts_config_backend_ops_t s_file_backend_ops = {
    .init = file_backend_init,
    .deinit = file_backend_deinit,
    .get = file_backend_get,
    .set = file_backend_set,
    .erase = file_backend_erase,
    .exists = file_backend_exists,
    .clear = file_backend_clear,
    .commit = file_backend_commit,
};

/* ============================================================================
 * 公共 API
 * ========================================================================== */

esp_err_t ts_config_file_register(void)
{
#ifdef CONFIG_TS_CONFIG_FILE_BACKEND
    uint8_t priority = CONFIG_TS_CONFIG_PRIORITY_FILE;
#else
    uint8_t priority = 60;
#endif

    return ts_config_register_backend(TS_CONFIG_BACKEND_FILE, &s_file_backend_ops, priority);
}

esp_err_t ts_config_file_set_path(const char *path)
{
    if (path == NULL || strlen(path) >= sizeof(s_config_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_config_path, path, sizeof(s_config_path) - 1);
    s_config_path[sizeof(s_config_path) - 1] = '\0';

    ESP_LOGI(TAG, "Configuration path set to: %s", s_config_path);
    return ESP_OK;
}

const char *ts_config_file_get_path(void)
{
    return s_config_path;
}

esp_err_t ts_config_file_load_all(void)
{
    if (!s_file_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Loading configuration files from: %s", s_config_path);

    // 检查目录是否存在
    if (!path_exists(s_config_path)) {
        ESP_LOGW(TAG, "Configuration path does not exist");
        return ESP_ERR_NOT_FOUND;
    }

    // 遍历目录，加载 JSON 配置文件
    // 注意: .tscfg 加密配置在 security 服务启动后由 ts_config_file_load_encrypted() 加载
    DIR *dir = opendir(s_config_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open configuration directory");
        return ESP_FAIL;
    }

    int loaded_count = 0;
    int skipped_count = 0;
    struct dirent *entry;

    // 堆分配大型数组，避免栈溢出（事件任务栈只有 4KB）
    // tscfg_names: 用于记录已有的 .tscfg 文件名（跳过同名 .json）
    #define MAX_TSCFG_FILES 16
    #define MAX_NAME_LEN 64
    #define MAX_PATH_LEN 256
    
    char (*tscfg_names)[MAX_NAME_LEN] = heap_caps_malloc(MAX_TSCFG_FILES * MAX_NAME_LEN, MALLOC_CAP_SPIRAM);
    char *filepath = heap_caps_malloc(MAX_PATH_LEN, MALLOC_CAP_SPIRAM);
    
    if (tscfg_names == NULL || filepath == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for config loading");
        if (tscfg_names) free(tscfg_names);
        if (filepath) free(filepath);
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    
    int tscfg_name_count = 0;
    
    // 第一遍：记录已有的 .tscfg 文件名（用于后续跳过同名 .json）
    while ((entry = readdir(dir)) != NULL && tscfg_name_count < MAX_TSCFG_FILES) {
        const char *ext = strrchr(entry->d_name, '.');
        if (ext != NULL && strcmp(ext, ".tscfg") == 0) {
            // 提取不含扩展名的名字
            size_t base_len = ext - entry->d_name;
            if (base_len < MAX_NAME_LEN) {
                strncpy(tscfg_names[tscfg_name_count], entry->d_name, base_len);
                tscfg_names[tscfg_name_count][base_len] = '\0';
                tscfg_name_count++;
            }
        }
    }
    
    // 重置目录指针
    rewinddir(dir);

    while ((entry = readdir(dir)) != NULL) {
        const char *ext = strrchr(entry->d_name, '.');
        if (ext == NULL) continue;
        
        // 只处理 .json 明文配置（.tscfg 由 ts_config_file_load_encrypted() 在 security 服务启动后加载）
        if (strcmp(ext, ".json") != 0) continue;
        
        snprintf(filepath, MAX_PATH_LEN, "%.127s/%.127s", s_config_path, entry->d_name);
        
        // 处理 .json 明文配置
        if (strcmp(ext, ".json") == 0) {
            /* 跳过 schema-less 模块的配置文件 */
            if (is_schemaless_module_file(entry->d_name)) {
                ESP_LOGD(TAG, "Skipping schema-less module file: %s", entry->d_name);
                skipped_count++;
                continue;
            }
            
            // 检查是否存在同名 .tscfg（已加载过，跳过）
            size_t base_len = ext - entry->d_name;
            bool has_tscfg = false;
            for (int i = 0; i < tscfg_name_count; i++) {
                if (strlen(tscfg_names[i]) == base_len &&
                    strncmp(tscfg_names[i], entry->d_name, base_len) == 0) {
                    has_tscfg = true;
                    break;
                }
            }
            
            if (has_tscfg) {
                ESP_LOGI(TAG, "Skipping %s (encrypted version exists)", entry->d_name);
                skipped_count++;
                continue;
            }

            ESP_LOGI(TAG, "Loading: %s", entry->d_name);
            esp_err_t ret = ts_config_load_json_file(filepath);
            if (ret == ESP_OK) {
                loaded_count++;
            } else {
                ESP_LOGW(TAG, "Failed to load %s: %s", entry->d_name, esp_err_to_name(ret));
            }
        }
    }

    // 释放堆内存
    free(tscfg_names);
    free(filepath);
    closedir(dir);

    if (tscfg_name_count > 0) {
        ESP_LOGI(TAG, "Loaded %d JSON configs, skipped %d (found %d .tscfg files for deferred loading)", 
                 loaded_count, skipped_count, tscfg_name_count);
    } else {
        ESP_LOGI(TAG, "Loaded %d JSON configs, skipped %d", loaded_count, skipped_count);
    }
    return ESP_OK;
}

esp_err_t ts_config_file_load_encrypted(void)
{
    if (!s_file_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // 首先初始化 config pack 子系统（需要证书已加载）
    esp_err_t init_ret = ts_config_pack_init();
    if (init_ret != ESP_OK) {
        ESP_LOGW(TAG, "Config pack init failed: %s, skipping encrypted configs", 
                 esp_err_to_name(init_ret));
        return ESP_OK;  // 不是致命错误
    }

    ESP_LOGI(TAG, "Loading encrypted configuration files from: %s", s_config_path);

    // 检查目录是否存在
    if (!path_exists(s_config_path)) {
        ESP_LOGD(TAG, "Configuration path does not exist");
        return ESP_OK;
    }

    DIR *dir = opendir(s_config_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open configuration directory");
        return ESP_FAIL;
    }

    int loaded_count = 0;
    int failed_count = 0;
    struct dirent *entry;
    
    // 堆分配避免栈溢出
    #define TSCFG_MAX_PATH_LEN 256
    char *filepath = heap_caps_malloc(TSCFG_MAX_PATH_LEN, MALLOC_CAP_SPIRAM);
    if (filepath == NULL) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    while ((entry = readdir(dir)) != NULL) {
        const char *ext = strrchr(entry->d_name, '.');
        if (ext == NULL || strcmp(ext, ".tscfg") != 0) {
            continue;
        }
        
        snprintf(filepath, TSCFG_MAX_PATH_LEN, "%.127s/%.127s", s_config_path, entry->d_name);
        
        ESP_LOGI(TAG, "Loading encrypted: %s", entry->d_name);
        
        // 解密并加载配置
        ts_config_pack_t *pack = NULL;
        ts_config_pack_result_t pack_ret = ts_config_pack_load(filepath, &pack);
        
        if (pack_ret == TS_CONFIG_PACK_OK && pack && pack->content) {
            // 解密成功，解析 JSON 并应用配置
            esp_err_t ret = ts_config_load_json_string(pack->content);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Loaded encrypted config: %s (signer: %s)", 
                         entry->d_name, pack->sig_info.signer_cn);
                loaded_count++;
            } else {
                ESP_LOGW(TAG, "Failed to parse decrypted content: %s", entry->d_name);
                failed_count++;
            }
            ts_config_pack_free(pack);
        } else {
            ESP_LOGW(TAG, "Failed to decrypt %s: %s", entry->d_name,
                     ts_config_pack_strerror(pack_ret));
            failed_count++;
        }
    }

    free(filepath);
    closedir(dir);

    if (loaded_count > 0 || failed_count > 0) {
        ESP_LOGI(TAG, "Encrypted configs: %d loaded, %d failed", loaded_count, failed_count);
    }
    return ESP_OK;
}

esp_err_t ts_config_file_save_all(void)
{
    if (!s_file_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 确保目录存在
    if (!ensure_directory(s_config_path)) {
        ESP_LOGE(TAG, "Failed to create configuration directory");
        return ESP_FAIL;
    }

    // 保存到默认文件
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/config.json", s_config_path);

    return ts_config_save_json_file(filepath);
}

const ts_config_backend_ops_t *ts_config_file_get_ops(void)
{
    return &s_file_backend_ops;
}

static bool path_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

static bool ensure_directory(const char *path)
{
    struct stat st;
    
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // 尝试创建目录
    if (mkdir(path, 0755) == 0) {
        ESP_LOGI(TAG, "Created directory: %s", path);
        return true;
    }

    // 可能需要递归创建父目录
    char temp[256];
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    for (char *p = temp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (stat(temp, &st) != 0) {
                if (mkdir(temp, 0755) != 0) {
                    return false;
                }
            }
            *p = '/';
        }
    }

    return (mkdir(path, 0755) == 0);
}
