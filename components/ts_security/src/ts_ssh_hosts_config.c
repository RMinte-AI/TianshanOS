#include "ts_ssh_service.h"
/**
 * @file ts_ssh_hosts_config.c
 * @brief SSH Host Configuration Storage Implementation
 *
 * 实现 SSH 主机凭证配置的持久化存储（NVS + SD卡双写）。
 */

#include "ts_ssh_hosts_config.h"
#include "ts_config_pack.h"
#include "ts_cert.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>

static const char *TAG = "ts_ssh_hosts_cfg";

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

#define NVS_NAMESPACE       "ts_ssh_cfg"
#define NVS_KEY_COUNT       "count"
#define NVS_KEY_PREFIX      "h_"

/*===========================================================================*/
/*                              Internal State                                */
/*===========================================================================*/

/** NVS 存储格式（不含密码） */
typedef struct __attribute__((packed)) {
    char id[TS_SSH_HOST_ID_MAX];
    char host[TS_SSH_HOST_ADDR_MAX];
    uint16_t port;
    char username[TS_SSH_USERNAME_MAX];
    uint8_t auth_type;
    char keyid[TS_SSH_KEYID_MAX];
    uint32_t created_time;
    uint32_t last_used_time;
    uint8_t enabled;
} nvs_host_entry_t;

static struct {
    bool initialized;
    nvs_handle_t nvs;
    SemaphoreHandle_t mutex;
} s_state = {0};

/*===========================================================================*/
/*                          Internal Functions                                */
/*===========================================================================*/

static void make_nvs_key(int index, char *key, size_t key_size)
{
    snprintf(key, key_size, "%s%d", NVS_KEY_PREFIX, index);
}

static uint32_t get_current_time(void)
{
    time_t now;
    time(&now);
    return (uint32_t)now;
}

/* 前向声明 - SD 卡操作 */
static bool is_sdcard_mounted(void);
static void delete_host_file(const char *id);

/*===========================================================================*/
/*                          Initialization                                    */
/*===========================================================================*/

/** 延迟导出标志 */
static bool s_hosts_pending_export = false;

/** 正在从 SD 卡加载中（禁止触发同步） */
static bool s_loading_from_sdcard = false;
static TaskHandle_t initial_loader;

/**
 * @brief 延迟加载/导出任务 - 在独立任务中处理 SD 卡操作（避免 main 任务栈溢出）
 * 
 * 配置加载优先级：SD 卡 > NVS > 硬编码默认值
 */
static void hosts_deferred_export_task(void *arg)
{
    (void)arg;
    initial_loader = xTaskGetCurrentTaskHandle();
    vTaskDelay(pdMS_TO_TICKS(2500));  /* 等待系统稳定，错开 commands 加载 */
    
    if (!s_state.initialized) {
        initial_loader = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    size_t nvs_count = ts_ssh_hosts_config_count();
    
    /* 
     * 配置加载优先级：SD 卡 (.tscfg > .json) > NVS
     * 
     * 如果 SD 卡有配置文件，以 SD 卡为权威来源（清空 NVS 后导入）
     * 如果 SD 卡没有配置文件，保留 NVS 数据并导出到 SD 卡
     */
    ESP_LOGI(TAG, "Deferred: checking SD card for config (NVS has %d hosts)...", (int)nvs_count);
    
    /* 检查 SD 卡目录是否有配置文件（.tscfg 或 .json） */
    bool sdcard_has_config = false;
    
    DIR *dir = opendir(TS_SSH_HOSTS_SDCARD_DIR);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            size_t len = strlen(entry->d_name);
            if ((len >= 6 && strcmp(entry->d_name + len - 5, ".json") == 0) ||
                (len >= 7 && strcmp(entry->d_name + len - 6, ".tscfg") == 0)) {
                sdcard_has_config = true;
                break;
            }
        }
        closedir(dir);
    }
    
    if (sdcard_has_config) {
        /* SD 卡有配置，清空 NVS 后导入（SD 卡为权威来源） */
        ESP_LOGI(TAG, "SD card has config, clearing NVS and importing...");
        esp_err_t cleared = ts_ssh_hosts_config_clear();
        if (cleared != ESP_OK) {
            s_loading_from_sdcard = false;
            initial_loader = NULL;
            vTaskDelete(NULL);
            return;
        }

        esp_err_t import_ret = ts_ssh_hosts_config_import_from_sdcard(false);
        size_t count = ts_ssh_hosts_config_count();
        
        if (import_ret == ESP_OK) {
            ESP_LOGI(TAG, "Loaded %d hosts from SD card (.tscfg > .json)", (int)count);
        } else {
            ESP_LOGW(TAG, "SD card import failed: %s", esp_err_to_name(import_ret));
        }
    } else {
        /* SD 卡没有配置文件，保留 NVS 数据并导出到 SD 卡 */
        if (nvs_count > 0) {
            ESP_LOGI(TAG, "SD card has no config, exporting %d hosts from NVS", (int)nvs_count);
            ts_ssh_hosts_config_export_to_sdcard();
        } else {
            ESP_LOGI(TAG, "No hosts in SD card or NVS");
        }
    }
    
    s_hosts_pending_export = false;
    initial_loader = NULL;
    vTaskDelete(NULL);
}

esp_err_t ts_ssh_hosts_config_init(void)
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
    
    /* 打开 NVS */
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_state.nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_state.mutex);
        return ret;
    }
    
    s_state.initialized = true;
    
    /*
     * 配置加载策略（避免在 main 任务中执行 SD 卡 I/O）：
     * 
     * 1. init 时只初始化 NVS，统计现有数据
     * 2. 创建延迟任务处理 SD 卡加载/导出
     */
    
    size_t nvs_count = ts_ssh_hosts_config_count();
    ESP_LOGI(TAG, "NVS has %d hosts, will load from SD card in background", (int)nvs_count);
    
    /* 创建延迟加载任务
     * 必须使用 DRAM 栈，因为内部会访问 NVS */
    s_hosts_pending_export = true;
    xTaskCreate(hosts_deferred_export_task, "ssh_host_load", 8192, NULL, 2, NULL);
    
    ESP_LOGI(TAG, "SSH hosts config initialized (SD card loading deferred)");
    return ESP_OK;
}

void ts_ssh_hosts_config_deinit(void)
{
    if (!s_state.initialized) {
        return;
    }
    
    nvs_close(s_state.nvs);
    vSemaphoreDelete(s_state.mutex);
    s_state.initialized = false;
}

bool ts_ssh_hosts_config_is_initialized(void)
{
    return s_state.initialized;
}

/*===========================================================================*/
/*                          CRUD Operations                                   */
/*===========================================================================*/

static esp_err_t host_add_impl(const ts_ssh_host_config_t *config) {
    if (!s_state.initialized || !config || !config->id[0] || !config->host[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    /* 检查是否已存在（更新）或找空位（新增） */
    int existing_index = -1;
    int free_index = -1;
    char key[16];
    nvs_host_entry_t entry;
    size_t len;
    
    for (int i = 0; i < TS_SSH_HOSTS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        esp_err_t ret = nvs_get_blob(s_state.nvs, key, &entry, &len);
        
        if (ret == ESP_OK && len == sizeof(entry)) {
            if (strcmp(entry.id, config->id) == 0) {
                existing_index = i;
                break;
            }
        } else if (free_index < 0) {
            free_index = i;
        }
    }
    
    int target_index = (existing_index >= 0) ? existing_index : free_index;
    if (target_index < 0) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGW(TAG, "Max hosts reached");
        return ESP_ERR_NO_MEM;
    }
    
    /* 构建 NVS 条目 */
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.id, config->id, sizeof(entry.id) - 1);
    strncpy(entry.host, config->host, sizeof(entry.host) - 1);
    entry.port = config->port > 0 ? config->port : 22;
    strncpy(entry.username, config->username, sizeof(entry.username) - 1);
    entry.auth_type = (uint8_t)config->auth_type;
    strncpy(entry.keyid, config->keyid, sizeof(entry.keyid) - 1);
    entry.created_time = (existing_index >= 0) ? entry.created_time : get_current_time();
    entry.last_used_time = config->last_used_time;
    entry.enabled = config->enabled ? 1 : 0;
    
    /* 保存到 NVS */
    make_nvs_key(target_index, key, sizeof(key));
    esp_err_t ret = nvs_set_blob(s_state.nvs, key, &entry, sizeof(entry));
    if (ret == ESP_OK) {
        ret = nvs_commit(s_state.nvs);
    }
    
    xSemaphoreGive(s_state.mutex);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s SSH host: %s (%s@%s:%d)", 
                 existing_index >= 0 ? "Updated" : "Added",
                 config->id, config->username, config->host, entry.port);
        
        /* 同步到 SD 卡（加载期间不触发，避免文件描述符用尽） */
        if (!s_loading_from_sdcard) {
            ts_ssh_hosts_config_sync_to_sdcard();
        }
    }
    
    return ret;
}

static esp_err_t host_remove_impl(const char *id) {
    if (!s_state.initialized || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_host_entry_t entry;
    size_t len;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    
    for (int i = 0; i < TS_SSH_HOSTS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            if (strcmp(entry.id, id) == 0) {
                ret = nvs_erase_key(s_state.nvs, key);
                if (ret == ESP_OK) {
                    nvs_commit(s_state.nvs);
                    ESP_LOGI(TAG, "Removed SSH host: %s", id);
                }
                break;
            }
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    
    /* 同步删除 SD 卡文件（直接删除，不重新导出） */
    if (ret == ESP_OK) {
        delete_host_file(id);
    }
    
    return ret;
}

esp_err_t ts_ssh_hosts_config_get(const char *id, ts_ssh_host_config_t *config)
{
    if (!s_state.initialized || !id || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_host_entry_t entry;
    size_t len;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    
    for (int i = 0; i < TS_SSH_HOSTS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            if (strcmp(entry.id, id) == 0) {
                memset(config, 0, sizeof(*config));
                strncpy(config->id, entry.id, sizeof(config->id) - 1);
                strncpy(config->host, entry.host, sizeof(config->host) - 1);
                config->port = entry.port;
                strncpy(config->username, entry.username, sizeof(config->username) - 1);
                config->auth_type = (ts_ssh_host_auth_type_t)entry.auth_type;
                strncpy(config->keyid, entry.keyid, sizeof(config->keyid) - 1);
                config->created_time = entry.created_time;
                config->last_used_time = entry.last_used_time;
                config->enabled = entry.enabled != 0;
                ret = ESP_OK;
                break;
            }
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    return ret;
}

esp_err_t ts_ssh_hosts_config_find(const char *host, uint16_t port, 
                                    const char *username,
                                    ts_ssh_host_config_t *config)
{
    if (!s_state.initialized || !host || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (port == 0) port = 22;
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_host_entry_t entry;
    size_t len;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    
    for (int i = 0; i < TS_SSH_HOSTS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            if (strcmp(entry.host, host) == 0 && entry.port == port) {
                if (username == NULL || strcmp(entry.username, username) == 0) {
                    memset(config, 0, sizeof(*config));
                    strncpy(config->id, entry.id, sizeof(config->id) - 1);
                    strncpy(config->host, entry.host, sizeof(config->host) - 1);
                    config->port = entry.port;
                    strncpy(config->username, entry.username, sizeof(config->username) - 1);
                    config->auth_type = (ts_ssh_host_auth_type_t)entry.auth_type;
                    strncpy(config->keyid, entry.keyid, sizeof(config->keyid) - 1);
                    config->created_time = entry.created_time;
                    config->last_used_time = entry.last_used_time;
                    config->enabled = entry.enabled != 0;
                    ret = ESP_OK;
                    break;
                }
            }
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    return ret;
}

esp_err_t ts_ssh_hosts_config_list(ts_ssh_host_config_t *configs, 
                                    size_t max_count, 
                                    size_t *count)
{
    if (!s_state.initialized || !configs || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_host_entry_t entry;
    size_t len;
    size_t found = 0;
    
    for (int i = 0; i < TS_SSH_HOSTS_MAX && found < max_count; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            ts_ssh_host_config_t *cfg = &configs[found];
            memset(cfg, 0, sizeof(*cfg));
            strncpy(cfg->id, entry.id, sizeof(cfg->id) - 1);
            strncpy(cfg->host, entry.host, sizeof(cfg->host) - 1);
            cfg->port = entry.port;
            strncpy(cfg->username, entry.username, sizeof(cfg->username) - 1);
            cfg->auth_type = (ts_ssh_host_auth_type_t)entry.auth_type;
            strncpy(cfg->keyid, entry.keyid, sizeof(cfg->keyid) - 1);
            cfg->created_time = entry.created_time;
            cfg->last_used_time = entry.last_used_time;
            cfg->enabled = entry.enabled != 0;
            found++;
        }
    }
    
    *count = found;
    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

int ts_ssh_hosts_config_count(void)
{
    if (!s_state.initialized) {
        return 0;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_host_entry_t entry;
    size_t len;
    int count = 0;
    
    for (int i = 0; i < TS_SSH_HOSTS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            count++;
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    return count;
}

esp_err_t ts_ssh_hosts_config_touch(const char *id)
{
    if (!s_state.initialized || !id) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_host_entry_t entry;
    size_t len;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    
    for (int i = 0; i < TS_SSH_HOSTS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            if (strcmp(entry.id, id) == 0) {
                entry.last_used_time = get_current_time();
                ret = nvs_set_blob(s_state.nvs, key, &entry, sizeof(entry));
                if (ret == ESP_OK) {
                    nvs_commit(s_state.nvs);
                }
                break;
            }
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    return ret;
}

static esp_err_t host_clear_impl(void) {
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    esp_err_t ret = nvs_erase_all(s_state.nvs);
    if (ret == ESP_OK) {
        nvs_commit(s_state.nvs);
        ESP_LOGI(TAG, "Cleared all SSH host configs");
    }
    
    xSemaphoreGive(s_state.mutex);
    
    /* 同步到 SD 卡 */
    if (ret == ESP_OK) {
        ts_ssh_hosts_config_sync_to_sdcard();
    }
    
    return ret;
}

/*===========================================================================*/
/*                    Iterator/Pagination API (内存优化)                       */
/*===========================================================================*/

/**
 * @brief 内部辅助：从 NVS entry 填充 config 结构体
 */
static void host_entry_to_config(const nvs_host_entry_t *entry, ts_ssh_host_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id, entry->id, sizeof(cfg->id) - 1);
    strncpy(cfg->host, entry->host, sizeof(cfg->host) - 1);
    cfg->port = entry->port;
    strncpy(cfg->username, entry->username, sizeof(cfg->username) - 1);
    cfg->auth_type = (ts_ssh_host_auth_type_t)entry->auth_type;
    strncpy(cfg->keyid, entry->keyid, sizeof(cfg->keyid) - 1);
    cfg->created_time = entry->created_time;
    cfg->last_used_time = entry->last_used_time;
    cfg->enabled = entry->enabled != 0;
}

esp_err_t ts_ssh_hosts_config_iterate(ts_ssh_host_iterator_cb_t callback,
                                       void *user_data,
                                       size_t offset,
                                       size_t limit,
                                       size_t *total_count)
{
    if (!s_state.initialized || !callback) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_host_entry_t entry;
    ts_ssh_host_config_t cfg;
    size_t len;
    size_t total = 0;       /* 总有效条目数 */
    size_t returned = 0;    /* 实际返回给回调的数量 */
    size_t skipped = 0;     /* 跳过的条目数（offset） */
    
    for (int i = 0; i < TS_SSH_HOSTS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            total++;
            
            /* 处理分页偏移 */
            if (skipped < offset) {
                skipped++;
                continue;
            }
            
            /* 处理分页限制 */
            if (limit > 0 && returned >= limit) {
                continue;  /* 继续计数但不回调 */
            }
            
            /* 转换并回调 */
            host_entry_to_config(&entry, &cfg);
            bool cont = callback(&cfg, returned, user_data);
            returned++;
            
            if (!cont) {
                break;  /* 回调请求停止 */
            }
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    
    if (total_count) {
        *total_count = total;
    }
    
    return ESP_OK;
}

/*===========================================================================*/
/*                    SD Card Export/Import (持久化备份)                       */
/*===========================================================================*/

/** SD 卡配置目录 */
#define SDCARD_CONFIG_DIR  "/sdcard/config"

/**
 * @brief 检查 SD 卡是否已挂载
 */
static bool is_sdcard_mounted(void)
{
    struct stat st;
    return (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode));
}

/**
 * @brief 确保配置目录存在
 */
static esp_err_t ensure_config_dir(void)
{
    struct stat st;
    if (stat(SDCARD_CONFIG_DIR, &st) != 0) {
        if (mkdir(SDCARD_CONFIG_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create config dir: %s", SDCARD_CONFIG_DIR);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/**
 * @brief 确保主机文件夹存在
 */
static esp_err_t ensure_hosts_dir(void)
{
    esp_err_t ret = ensure_config_dir();
    if (ret != ESP_OK) return ret;
    
    struct stat st;
    if (stat(TS_SSH_HOSTS_SDCARD_DIR, &st) != 0) {
        if (mkdir(TS_SSH_HOSTS_SDCARD_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create hosts dir: %s", TS_SSH_HOSTS_SDCARD_DIR);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/**
 * @brief 删除 SD 卡上的主机配置文件
 * 
 * 同时尝试删除 .json 和 .tscfg 文件（如果存在）
 */
static void delete_host_file(const char *id)
{
    if (!id || !id[0] || !is_sdcard_mounted()) {
        return;
    }
    
    char filepath[128];
    
    /* 删除 .json 文件 */
    snprintf(filepath, sizeof(filepath), "%s/%s.json", TS_SSH_HOSTS_SDCARD_DIR, id);
    if (unlink(filepath) == 0) {
        ESP_LOGI(TAG, "Deleted SD card file: %s", filepath);
    }
    
    /* 删除 .tscfg 文件（如果存在） */
    snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", TS_SSH_HOSTS_SDCARD_DIR, id);
    if (unlink(filepath) == 0) {
        ESP_LOGI(TAG, "Deleted SD card file: %s", filepath);
    }
}

/**
 * @brief 将单条主机配置转换为 JSON 对象
 */
static cJSON *host_to_json(const ts_ssh_host_config_t *cfg)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    
    cJSON_AddStringToObject(obj, "id", cfg->id);
    cJSON_AddStringToObject(obj, "host", cfg->host);
    cJSON_AddNumberToObject(obj, "port", cfg->port);
    cJSON_AddStringToObject(obj, "username", cfg->username);
    cJSON_AddStringToObject(obj, "auth_type", cfg->auth_type == TS_SSH_HOST_AUTH_KEY ? "key" : "password");
    if (cfg->keyid[0]) cJSON_AddStringToObject(obj, "keyid", cfg->keyid);
    cJSON_AddBoolToObject(obj, "enabled", cfg->enabled);
    cJSON_AddNumberToObject(obj, "created_time", cfg->created_time);
    cJSON_AddNumberToObject(obj, "last_used_time", cfg->last_used_time);
    
    return obj;
}

/**
 * @brief 从 JSON 对象解析主机配置
 */
static esp_err_t json_to_host(const cJSON *obj, ts_ssh_host_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    
    cJSON *item;
    
    item = cJSON_GetObjectItem(obj, "id");
    if (item && cJSON_IsString(item)) strncpy(cfg->id, item->valuestring, sizeof(cfg->id) - 1);
    
    item = cJSON_GetObjectItem(obj, "host");
    if (item && cJSON_IsString(item)) strncpy(cfg->host, item->valuestring, sizeof(cfg->host) - 1);
    
    item = cJSON_GetObjectItem(obj, "port");
    if (item && cJSON_IsNumber(item)) cfg->port = (uint16_t)item->valueint;
    else cfg->port = 22;
    
    item = cJSON_GetObjectItem(obj, "username");
    if (item && cJSON_IsString(item)) strncpy(cfg->username, item->valuestring, sizeof(cfg->username) - 1);
    
    item = cJSON_GetObjectItem(obj, "auth_type");
    if (item && cJSON_IsString(item)) {
        cfg->auth_type = (strcmp(item->valuestring, "key") == 0) ? 
                         TS_SSH_HOST_AUTH_KEY : TS_SSH_HOST_AUTH_PASSWORD;
    }
    
    item = cJSON_GetObjectItem(obj, "keyid");
    if (item && cJSON_IsString(item)) strncpy(cfg->keyid, item->valuestring, sizeof(cfg->keyid) - 1);
    
    item = cJSON_GetObjectItem(obj, "enabled");
    if (item && cJSON_IsBool(item)) cfg->enabled = cJSON_IsTrue(item);
    else cfg->enabled = true;
    
    item = cJSON_GetObjectItem(obj, "created_time");
    if (item && cJSON_IsNumber(item)) cfg->created_time = (uint32_t)item->valueint;
    
    item = cJSON_GetObjectItem(obj, "last_used_time");
    if (item && cJSON_IsNumber(item)) cfg->last_used_time = (uint32_t)item->valueint;
    
    return ESP_OK;
}

/**
 * @brief 导出单条主机配置到独立文件（始终使用 .tscfg 加密格式）
 * 
 * 安全设计：SSH 主机配置包含密钥 ID，即使在本机也应加密存储
 */
static esp_err_t export_host_to_file(const ts_ssh_host_config_t *cfg)
{
    if (!cfg || !cfg->id[0]) return ESP_ERR_INVALID_ARG;
    
    /* 检查是否有导出权限 */
    if (!ts_config_pack_can_export()) {
        ESP_LOGW(TAG, "Device cannot export config packs (not a developer device)");
        /* 降级为明文导出（仅用于调试设备） */
        char filepath[128];
        snprintf(filepath, sizeof(filepath), "%s/%s.json", TS_SSH_HOSTS_SDCARD_DIR, cfg->id);
        
        cJSON *obj = host_to_json(cfg);
        if (!obj) return ESP_ERR_NO_MEM;
        
        char *str = cJSON_Print(obj);
        cJSON_Delete(obj);
        if (!str) return ESP_ERR_NO_MEM;
        
        FILE *fp = fopen(filepath, "w");
        if (!fp) {
            cJSON_free(str);
            return ESP_FAIL;
        }
        fprintf(fp, "%s\n", str);
        fclose(fp);
        cJSON_free(str);
        return ESP_OK;
    }
    
    /* 获取本机证书作为加密目标 */
    char *cert_pem = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!cert_pem) return ESP_ERR_NO_MEM;
    
    size_t cert_len = 4096;
    esp_err_t ret = ts_cert_get_certificate(cert_pem, &cert_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get device certificate: %s", esp_err_to_name(ret));
        free(cert_pem);
        /* 降级为明文导出 */
        char filepath[128];
        snprintf(filepath, sizeof(filepath), "%s/%s.json", TS_SSH_HOSTS_SDCARD_DIR, cfg->id);
        
        cJSON *obj = host_to_json(cfg);
        if (!obj) return ESP_ERR_NO_MEM;
        
        char *str = cJSON_Print(obj);
        cJSON_Delete(obj);
        if (!str) return ESP_ERR_NO_MEM;
        
        FILE *fp = fopen(filepath, "w");
        if (!fp) {
            cJSON_free(str);
            return ESP_FAIL;
        }
        fprintf(fp, "%s\n", str);
        fclose(fp);
        cJSON_free(str);
        return ESP_OK;
    }
    
    /* 生成 JSON 内容 */
    cJSON *obj = host_to_json(cfg);
    if (!obj) {
        free(cert_pem);
        return ESP_ERR_NO_MEM;
    }
    
    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json_str) {
        free(cert_pem);
        return ESP_ERR_NO_MEM;
    }
    
    /* 创建加密配置包（目标为本机） */
    char *tscfg_output = NULL;
    size_t tscfg_len = 0;
    
    ts_config_pack_export_opts_t opts = {
        .recipient_cert_pem = cert_pem,
        .recipient_cert_len = cert_len,
        .description = "SSH host configuration"
    };
    
    ts_config_pack_result_t result = ts_config_pack_create(
        cfg->id,
        json_str,
        strlen(json_str),
        &opts,
        &tscfg_output,
        &tscfg_len
    );
    
    cJSON_free(json_str);
    free(cert_pem);
    
    if (result != TS_CONFIG_PACK_OK) {
        ESP_LOGE(TAG, "Failed to create config pack: %d", result);
        return ESP_FAIL;
    }
    
    /* 保存到 .tscfg 文件 */
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", TS_SSH_HOSTS_SDCARD_DIR, cfg->id);
    
    ret = ts_config_pack_save(filepath, tscfg_output, tscfg_len);
    free(tscfg_output);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Exported host config to: %s", filepath);
        
        /* 删除旧的 .json 文件（如果存在） */
        char json_path[128];
        snprintf(json_path, sizeof(json_path), "%s/%s.json", TS_SSH_HOSTS_SDCARD_DIR, cfg->id);
        unlink(json_path);  /* 忽略错误 */
    }
    
    return ret;
}

/**
 * @brief 从独立文件目录加载所有 SSH 主机
 * 
 * 支持 .tscfg 加密配置优先加载
 * 设计逻辑与 rules/actions/sources/ssh_commands 目录一致
 */
static esp_err_t load_hosts_from_dir(void)
{
    DIR *dir = opendir(TS_SSH_HOSTS_SDCARD_DIR);
    if (!dir) {
        ESP_LOGD(TAG, "Hosts directory not found: %s", TS_SSH_HOSTS_SDCARD_DIR);
        return ESP_ERR_NOT_FOUND;
    }
    
    int loaded = 0;
    int skipped = 0;
    struct dirent *entry;
    
    /* 使用堆分配避免栈溢出 */
    ts_ssh_host_config_t *cfg = heap_caps_malloc(sizeof(ts_ssh_host_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) cfg = malloc(sizeof(ts_ssh_host_config_t));
    if (!cfg) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        /* 跳过非 .json 和非 .tscfg 文件 */
        size_t len = strlen(entry->d_name);
        bool is_json = (len >= 6 && strcmp(entry->d_name + len - 5, ".json") == 0);
        bool is_tscfg = (len >= 7 && strcmp(entry->d_name + len - 6, ".tscfg") == 0);
        
        if (!is_json && !is_tscfg) {
            continue;
        }
        
        /* 对于 .json 文件，检查是否存在对应的 .tscfg（跳过以使用加密版本） */
        if (is_json) {
            char tscfg_name[128];
            snprintf(tscfg_name, sizeof(tscfg_name), "%.*s.tscfg", (int)(len - 5), entry->d_name);
            char tscfg_path[192];
            snprintf(tscfg_path, sizeof(tscfg_path), "%s/%s", TS_SSH_HOSTS_SDCARD_DIR, tscfg_name);
            struct stat st;
            if (stat(tscfg_path, &st) == 0) {
                ESP_LOGD(TAG, "Skipping %s (will use .tscfg)", entry->d_name);
                continue;  /* 跳过 .json，稍后处理 .tscfg */
            }
        }
        
        /* 限制文件名长度避免缓冲区溢出 */
        if (len > 60) {
            continue;
        }
        
        char filepath[128];
        if (is_tscfg) {
            /* .tscfg 文件 - 构建对应的 .json 路径用于 load_with_priority */
            snprintf(filepath, sizeof(filepath), "%s/%.*s.json", 
                     TS_SSH_HOSTS_SDCARD_DIR, (int)(len - 6), entry->d_name);
        } else {
            snprintf(filepath, sizeof(filepath), "%s/%.60s", TS_SSH_HOSTS_SDCARD_DIR, entry->d_name);
        }
        
        /* 使用 .tscfg 优先加载 */
        char *content = NULL;
        size_t content_len = 0;
        bool used_tscfg = false;
        
        esp_err_t ret = ts_config_pack_load_with_priority(
            filepath, &content, &content_len, &used_tscfg);
        
        if (ret != ESP_OK) {
            continue;
        }
        
        /* 解析 JSON */
        cJSON *root = cJSON_Parse(content);
        free(content);
        
        if (!root) {
            ESP_LOGW(TAG, "Failed to parse JSON from %s", filepath);
            skipped++;
            continue;
        }
        
        /* 解析并添加 */
        memset(cfg, 0, sizeof(ts_ssh_host_config_t));
        if (json_to_host(root, cfg) == ESP_OK && cfg->host[0] && cfg->username[0]) {
            esp_err_t add_ret = ts_ssh_hosts_config_add(cfg);
            if (add_ret == ESP_OK) {
                loaded++;
                ESP_LOGD(TAG, "Loaded host from file: %s%s", cfg->id, 
                         used_tscfg ? " (encrypted)" : "");
            } else {
                skipped++;
            }
        } else {
            skipped++;
        }
        
        cJSON_Delete(root);
    }
    
    free(cfg);
    closedir(dir);
    
    if (loaded > 0) {
        ESP_LOGI(TAG, "Loaded %d SSH hosts from directory: %s (skipped %d)", 
                 loaded, TS_SSH_HOSTS_SDCARD_DIR, skipped);
        return ESP_OK;
    }
    
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief 导出迭代器回调 - 将单条主机导出到独立文件
 */
static bool host_export_iterator_cb(const ts_ssh_host_config_t *config, size_t index, void *user_data)
{
    (void)index;
    int *count = (int *)user_data;
    
    /* 导出到独立文件 */
    if (export_host_to_file(config) == ESP_OK) {
        (*count)++;
    }
    
    return true;
}

esp_err_t ts_ssh_hosts_config_export_to_sdcard(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!is_sdcard_mounted()) {
        ESP_LOGD(TAG, "SD card not mounted, skip export");
        return ESP_ERR_NOT_FOUND;
    }
    
    /* 确保独立文件目录存在 */
    esp_err_t ret = ensure_hosts_dir();
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 只导出到独立文件（不再生成主配置文件） */
    int count = 0;
    ret = ts_ssh_hosts_config_iterate(host_export_iterator_cb, &count, 0, 0, NULL);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Exported %d SSH hosts to %s/", count, TS_SSH_HOSTS_SDCARD_DIR);
    }
    
    return ret;
}

esp_err_t ts_ssh_hosts_config_import_from_sdcard(bool merge)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!is_sdcard_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }
    
    /* 设置加载标志，禁止 add 函数触发同步 */
    s_loading_from_sdcard = true;
    
    /* 如果不是合并模式，先清空现有配置 */
    if (!merge) {
        esp_err_t cleared = ts_ssh_hosts_config_clear();
        if (cleared != ESP_OK) {
            s_loading_from_sdcard = false;
            return cleared;
        }
    }
    
    /* 只从目录加载独立文件（.tscfg 优先于 .json） */
    esp_err_t ret = load_hosts_from_dir();
    
    /* 清除加载标志 */
    s_loading_from_sdcard = false;
    
    return ret;
}

/**
 * @brief 异步同步任务 - 延迟执行 SD 卡导出
 */
static void hosts_async_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));  /* 短延迟 */
    
    if (s_state.initialized) {
        ESP_LOGI(TAG, "Async sync to SD card...");
        ts_ssh_hosts_config_export_to_sdcard();
    }
    
    vTaskDelete(NULL);
}

void ts_ssh_hosts_config_sync_to_sdcard(void)
{
    /* 异步执行 SD 卡同步（避免在 API 处理任务中执行导致栈溢出/超时）
     * 必须使用 DRAM 栈，因为内部会访问 NVS */
    xTaskCreate(hosts_async_sync_task, "ssh_host_sync", 8192, NULL, 2, NULL);
}

esp_err_t ts_ssh_hosts_config_add(const ts_ssh_host_config_t *cfg) {
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    ts_ssh_binding_lock();
    ts_ssh_host_config_t old;
    esp_err_t got = ts_ssh_hosts_config_get(cfg->id, &old);
    bool changed =
        got == ESP_OK && (strcmp(old.host, cfg->host) || old.port != cfg->port ||
                          strcmp(old.username, cfg->username) || strcmp(old.keyid, cfg->keyid) ||
                          old.auth_type != cfg->auth_type);
    esp_err_t ret = (changed || got == ESP_ERR_NOT_FOUND) && ts_ssh_service_host_protected(cfg->id) ? ESP_ERR_INVALID_STATE
                                                                      : host_add_impl(cfg);
    ts_ssh_binding_unlock();
    return ret;
}
esp_err_t ts_ssh_hosts_config_remove(const char *id) {
    if (!id)
        return ESP_ERR_INVALID_ARG;
    ts_ssh_binding_lock();
    esp_err_t ret =
        ts_ssh_service_host_protected(id) ? ESP_ERR_INVALID_STATE : host_remove_impl(id);
    ts_ssh_binding_unlock();
    return ret;
}
esp_err_t ts_ssh_hosts_config_clear(void) {
    ts_ssh_binding_lock();
    esp_err_t ret =
        (xTaskGetCurrentTaskHandle() == initial_loader ? ts_ssh_service_any_in_use()
                                                       : ts_ssh_service_host_protected(NULL))
            ? ESP_ERR_INVALID_STATE
            : host_clear_impl();
    ts_ssh_binding_unlock();
    return ret;
}
