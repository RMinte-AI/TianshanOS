#include "ts_ssh_service.h"
/**
 * @file ts_ssh_commands_config.c
 * @brief SSH Command Configuration Storage Implementation
 *
 * 实现 SSH 快捷指令配置的持久化存储（NVS + SD卡双写）。
 * 启动时自动为有 var_name 的指令预创建变量。
 */

#include "ts_ssh_commands_config.h"
#include "ts_variable.h"
#include "ts_automation_types.h"
#include "ts_config_pack.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_vfs.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

static const char *TAG = "ts_ssh_cmd_cfg";

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

#define NVS_NAMESPACE       "ts_ssh_cmd"
#define NVS_KEY_PREFIX      "c_"

/*===========================================================================*/
/*                              Internal State                                */
/*===========================================================================*/

/** NVS 存储格式 - Version 3 (添加 service_fail_pattern 字段) */
typedef struct __attribute__((packed)) {
    char id[TS_SSH_CMD_ID_MAX];
    char host_id[TS_SSH_CMD_HOST_ID_MAX];
    char name[TS_SSH_CMD_NAME_MAX];
    char command[TS_SSH_CMD_COMMAND_MAX];
    char desc[TS_SSH_CMD_DESC_MAX];
    char icon[TS_SSH_CMD_ICON_MAX];
    char expect_pattern[TS_SSH_CMD_PATTERN_MAX];
    char fail_pattern[TS_SSH_CMD_PATTERN_MAX];
    char extract_pattern[TS_SSH_CMD_PATTERN_MAX];
    char var_name[TS_SSH_CMD_VARNAME_MAX];
    uint16_t timeout_sec;
    uint8_t stop_on_match;
    uint8_t nohup;
    uint8_t enabled;
    uint32_t created_time;
    uint32_t last_exec_time;
    /* 服务模式字段 (Version 2) */
    uint8_t service_mode;
    char ready_pattern[TS_SSH_CMD_PATTERN_MAX];
    uint16_t ready_timeout_sec;
    uint16_t ready_check_interval_ms;
    /* 服务失败模式 (Version 3) */
    char service_fail_pattern[TS_SSH_CMD_PATTERN_MAX];
} nvs_cmd_entry_t;

static struct {
    bool initialized;
    nvs_handle_t nvs;
    SemaphoreHandle_t mutex;
} s_state = {0};

/* 前向声明 - SD 卡操作 */
static bool is_sdcard_mounted(void);
static void delete_command_file(const char *id);

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

/**
 * @brief 验证 ID 格式是否安全
 * 
 * 安全的 ID 格式：
 * - 只允许字母（a-z, A-Z）、数字（0-9）、下划线（_）、连字符（-）
 * - 长度至少 1 个字符
 * - 不能以下划线或连字符开头/结尾
 * 
 * @param id 待验证的 ID
 * @return true 格式合法，false 格式非法
 */
static bool is_valid_id(const char *id)
{
    if (!id || !id[0]) return false;
    
    size_t len = strlen(id);
    if (len > TS_SSH_CMD_ID_MAX - 1) return false;
    
    /* 不能以下划线或连字符开头/结尾 */
    if (id[0] == '_' || id[0] == '-') return false;
    if (id[len - 1] == '_' || id[len - 1] == '-') return false;
    
    /* 只允许字母、数字、下划线、连字符 */
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    
    return true;
}

/**
 * @brief 为指令预创建变量（7 个标准变量）
 */
static void precreate_command_variables(const char *var_name)
{
    if (!var_name || !var_name[0]) {
        return;
    }
    
    /* 检查变量系统是否已初始化 */
    /* 注意：此时可能变量系统还未初始化，延迟到服务启动后处理 */
    
    const struct {
        const char *suffix;
        ts_auto_value_type_t type;
    } var_defs[] = {
        { "status", TS_AUTO_VAL_STRING },
        { "exit_code", TS_AUTO_VAL_INT },
        { "extracted", TS_AUTO_VAL_STRING },
        { "expect_matched", TS_AUTO_VAL_BOOL },
        { "fail_matched", TS_AUTO_VAL_BOOL },
        { "host", TS_AUTO_VAL_STRING },
        { "timestamp", TS_AUTO_VAL_INT },
    };
    
    char full_name[96];
    ts_auto_variable_t var = {0};
    strncpy(var.source_id, var_name, sizeof(var.source_id) - 1);
    var.flags = 0;
    
    for (size_t i = 0; i < sizeof(var_defs) / sizeof(var_defs[0]); i++) {
        snprintf(full_name, sizeof(full_name), "%s.%s", var_name, var_defs[i].suffix);
        strncpy(var.name, full_name, sizeof(var.name) - 1);
        var.value.type = var_defs[i].type;
        
        /* 设置默认值 */
        switch (var_defs[i].type) {
            case TS_AUTO_VAL_STRING:
                var.value.str_val[0] = '\0';
                break;
            case TS_AUTO_VAL_INT:
                var.value.int_val = 0;
                break;
            case TS_AUTO_VAL_BOOL:
                var.value.bool_val = false;
                break;
            default:
                break;
        }
        var.default_value = var.value;
        
        /* 注册变量（如果已存在则忽略） */
        esp_err_t ret = ts_variable_register(&var);
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Pre-created variable: %s", full_name);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            /* 变量系统未初始化，稍后重试 */
            ESP_LOGD(TAG, "Variable system not ready, will retry: %s", full_name);
        }
        /* ESP_ERR_INVALID_SIZE 表示变量已存在，忽略 */
    }
}

/**
 * @brief 初始化时为所有指令预创建变量
 */
static void precreate_all_variables(void)
{
    char key[16];
    nvs_cmd_entry_t entry;
    size_t len;
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            if (entry.var_name[0]) {
                precreate_command_variables(entry.var_name);
            }
        }
    }
}

/*===========================================================================*/
/*                          Initialization                                    */
/*===========================================================================*/

/** 延迟导出标志 */
static bool s_pending_export = false;

/** 正在从 SD 卡加载中（禁止触发同步） */
static bool s_loading_from_sdcard = false;
static TaskHandle_t initial_loader;

/**
 * @brief 延迟加载/导出任务 - 在独立任务中处理 SD 卡操作（避免 main 任务栈溢出）
 * 
 * 配置加载优先级：SD 卡 > NVS > 硬编码默认值
 */
static void deferred_export_task(void *arg)
{
    (void)arg;
    initial_loader = xTaskGetCurrentTaskHandle();
    vTaskDelay(pdMS_TO_TICKS(2000));  /* 等待系统稳定、SD 卡挂载完成 */
    
    if (!s_state.initialized) {
        initial_loader = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    size_t nvs_count = ts_ssh_commands_config_count();
    
    /* 
     * 配置加载优先级：SD 卡 (.tscfg > .json) > NVS
     * 
     * 如果 SD 卡有配置文件，以 SD 卡为权威来源（清空 NVS 后导入）
     * 如果 SD 卡没有配置文件，保留 NVS 数据并导出到 SD 卡
     */
    ESP_LOGI(TAG, "Deferred: checking SD card for config (NVS has %d commands)...", (int)nvs_count);
    
    /* 检查 SD 卡目录是否有配置文件（.tscfg 或 .json） */
    bool sdcard_has_config = false;
    
    DIR *dir = opendir(TS_SSH_COMMANDS_SDCARD_DIR);
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
        esp_err_t cleared = ts_ssh_commands_config_clear();
        if (cleared != ESP_OK) {
            s_loading_from_sdcard = false;
            initial_loader = NULL;
            vTaskDelete(NULL);
            return;
        }

        esp_err_t import_ret = ts_ssh_commands_config_import_from_sdcard(false);
        size_t count = ts_ssh_commands_config_count();
        
        if (import_ret == ESP_OK) {
            ESP_LOGI(TAG, "Loaded %d commands from SD card (.tscfg > .json)", (int)count);
        } else {
            ESP_LOGW(TAG, "SD card import failed: %s", esp_err_to_name(import_ret));
        }
    } else {
        /* SD 卡没有配置文件，保留 NVS 数据并导出到 SD 卡 */
        if (nvs_count > 0) {
            ESP_LOGI(TAG, "SD card has no config, exporting %d commands from NVS", (int)nvs_count);
            ts_ssh_commands_config_export_to_sdcard();
        } else {
            ESP_LOGI(TAG, "No commands in SD card or NVS");
        }
    }
    
    s_pending_export = false;
    initial_loader = NULL;
    vTaskDelete(NULL);
}

esp_err_t ts_ssh_commands_config_init(void)
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
     * 3. 延迟任务中：SD 卡优先，不存在则导出 NVS 数据
     */
    
    size_t nvs_count = ts_ssh_commands_config_count();
    ESP_LOGI(TAG, "NVS has %d commands, will load from SD card in background", (int)nvs_count);
    
    /* 创建延迟加载任务（在独立任务中处理 SD 卡操作，避免 main 任务栈溢出）
     * 必须使用 DRAM 栈，因为内部会调用 ts_ssh_commands_config_count() 访问 NVS */
    s_pending_export = true;  /* 标记需要处理 SD 卡 */
    xTaskCreate(deferred_export_task, "ssh_cmd_load", 8192, NULL, 2, NULL);
    
    ESP_LOGI(TAG, "SSH commands config initialized (SD card loading deferred)");
    
    /* 预创建所有变量（延迟到变量系统初始化后） */
    /* 这里不调用，由服务启动时调用 ts_ssh_commands_precreate_variables() */
    
    return ESP_OK;
}

void ts_ssh_commands_config_deinit(void)
{
    if (!s_state.initialized) {
        return;
    }
    
    nvs_close(s_state.nvs);
    vSemaphoreDelete(s_state.mutex);
    s_state.initialized = false;
}

bool ts_ssh_commands_config_is_initialized(void)
{
    return s_state.initialized;
}

/*===========================================================================*/
/*                          CRUD Operations                                   */
/*===========================================================================*/

static esp_err_t command_add_impl(const ts_ssh_command_config_t *config, char *out_id,
                                  size_t out_id_size) {
    /* 强制要求 ID：前端必须提供格式正确的语义化 ID */
    if (!s_state.initialized || !config || !config->id[0] || 
        !config->host_id[0] || !config->name[0]) {
        ESP_LOGE(TAG, "Invalid args: id=%s, host_id=%s, name=%s",
                 config ? config->id : "null",
                 config ? config->host_id : "null", 
                 config ? config->name : "null");
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 验证 ID 格式 */
    if (!is_valid_id(config->id)) {
        ESP_LOGE(TAG, "Invalid ID format: '%s' (allowed: a-z, A-Z, 0-9, _, -)", config->id);
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    /* 检查是否已存在（更新）或找空位（新增） */
    int existing_index = -1;
    int free_index = -1;
    char key[16];
    nvs_cmd_entry_t entry;
    size_t len;
    
    /* 遍历所有槽位，检查 ID 是否冲突 */
    for (int i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
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
        ESP_LOGW(TAG, "Max commands reached");
        return ESP_ERR_NO_MEM;
    }
    
    /* 构建 NVS 条目 */
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.id, config->id, sizeof(entry.id) - 1);
    strncpy(entry.host_id, config->host_id, sizeof(entry.host_id) - 1);
    strncpy(entry.name, config->name, sizeof(entry.name) - 1);
    strncpy(entry.command, config->command, sizeof(entry.command) - 1);
    strncpy(entry.desc, config->desc, sizeof(entry.desc) - 1);
    strncpy(entry.icon, config->icon, sizeof(entry.icon) - 1);
    strncpy(entry.expect_pattern, config->expect_pattern, sizeof(entry.expect_pattern) - 1);
    strncpy(entry.fail_pattern, config->fail_pattern, sizeof(entry.fail_pattern) - 1);
    strncpy(entry.extract_pattern, config->extract_pattern, sizeof(entry.extract_pattern) - 1);
    strncpy(entry.var_name, config->var_name, sizeof(entry.var_name) - 1);
    entry.timeout_sec = config->timeout_sec > 0 ? config->timeout_sec : 30;
    entry.stop_on_match = config->stop_on_match ? 1 : 0;
    entry.nohup = config->nohup ? 1 : 0;
    entry.enabled = config->enabled ? 1 : 0;
    entry.created_time = (existing_index >= 0) ? entry.created_time : get_current_time();
    entry.last_exec_time = config->last_exec_time;
    /* 服务模式字段 */
    entry.service_mode = config->service_mode ? 1 : 0;
    strncpy(entry.ready_pattern, config->ready_pattern, sizeof(entry.ready_pattern) - 1);
    strncpy(entry.service_fail_pattern, config->service_fail_pattern, sizeof(entry.service_fail_pattern) - 1);
    entry.ready_timeout_sec = config->ready_timeout_sec > 0 ? config->ready_timeout_sec : 60;
    entry.ready_check_interval_ms = config->ready_check_interval_ms > 0 ? config->ready_check_interval_ms : 3000;
    
    /* 保存到 NVS */
    make_nvs_key(target_index, key, sizeof(key));
    esp_err_t ret = nvs_set_blob(s_state.nvs, key, &entry, sizeof(entry));
    if (ret == ESP_OK) {
        ret = nvs_commit(s_state.nvs);
    }
    
    xSemaphoreGive(s_state.mutex);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s SSH command: %s (%s)", 
                 existing_index >= 0 ? "Updated" : "Added",
                 entry.name, entry.id);
        
        /* 输出 ID */
        if (out_id && out_id_size > 0) {
            strncpy(out_id, config->id, out_id_size - 1);
            out_id[out_id_size - 1] = '\0';
        }
        
        /* 为新指令预创建变量 */
        if (entry.var_name[0]) {
            precreate_command_variables(entry.var_name);
        }
        
        /* 同步到 SD 卡（加载期间不触发，避免文件描述符用尽） */
        if (!s_loading_from_sdcard) {
            ts_ssh_commands_config_sync_to_sdcard();
        }
    }
    
    return ret;
}

static esp_err_t command_remove_impl(const char *id) {
    if (!s_state.initialized || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_cmd_entry_t entry;
    size_t len;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            if (strcmp(entry.id, id) == 0) {
                ret = nvs_erase_key(s_state.nvs, key);
                if (ret == ESP_OK) {
                    ret = nvs_commit(s_state.nvs);
                    ESP_LOGI(TAG, "Removed SSH command: %s (%s)", entry.name, id);
                }
                break;
            }
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    
    /* 同步删除 SD 卡文件（直接删除，不重新导出） */
    if (ret == ESP_OK) {
        delete_command_file(id);
    }
    
    return ret;
}

esp_err_t ts_ssh_commands_config_get(const char *id, ts_ssh_command_config_t *config)
{
    if (!s_state.initialized || !id || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_cmd_entry_t entry;
    size_t len;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            if (strcmp(entry.id, id) == 0) {
                memset(config, 0, sizeof(*config));
                strncpy(config->id, entry.id, sizeof(config->id) - 1);
                strncpy(config->host_id, entry.host_id, sizeof(config->host_id) - 1);
                strncpy(config->name, entry.name, sizeof(config->name) - 1);
                strncpy(config->command, entry.command, sizeof(config->command) - 1);
                strncpy(config->desc, entry.desc, sizeof(config->desc) - 1);
                strncpy(config->icon, entry.icon, sizeof(config->icon) - 1);
                strncpy(config->expect_pattern, entry.expect_pattern, sizeof(config->expect_pattern) - 1);
                strncpy(config->fail_pattern, entry.fail_pattern, sizeof(config->fail_pattern) - 1);
                strncpy(config->extract_pattern, entry.extract_pattern, sizeof(config->extract_pattern) - 1);
                strncpy(config->var_name, entry.var_name, sizeof(config->var_name) - 1);
                config->timeout_sec = entry.timeout_sec;
                config->stop_on_match = entry.stop_on_match != 0;
                config->nohup = entry.nohup != 0;
                config->enabled = entry.enabled != 0;
                config->created_time = entry.created_time;
                config->last_exec_time = entry.last_exec_time;
                /* 服务模式字段 */
                config->service_mode = entry.service_mode != 0;
                strncpy(config->ready_pattern, entry.ready_pattern, sizeof(config->ready_pattern) - 1);
                strncpy(config->service_fail_pattern, entry.service_fail_pattern, sizeof(config->service_fail_pattern) - 1);
                config->ready_timeout_sec = entry.ready_timeout_sec;
                config->ready_check_interval_ms = entry.ready_check_interval_ms;
                ret = ESP_OK;
                break;
            }
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    return ret;
}

esp_err_t ts_ssh_commands_config_list(ts_ssh_command_config_t *configs,
                                       size_t max_count, size_t *count)
{
    if (!s_state.initialized || !configs || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_cmd_entry_t entry;
    size_t len;
    size_t n = 0;
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX && n < max_count; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            ts_ssh_command_config_t *cfg = &configs[n];
            memset(cfg, 0, sizeof(*cfg));
            strncpy(cfg->id, entry.id, sizeof(cfg->id) - 1);
            strncpy(cfg->host_id, entry.host_id, sizeof(cfg->host_id) - 1);
            strncpy(cfg->name, entry.name, sizeof(cfg->name) - 1);
            strncpy(cfg->command, entry.command, sizeof(cfg->command) - 1);
            strncpy(cfg->desc, entry.desc, sizeof(cfg->desc) - 1);
            strncpy(cfg->icon, entry.icon, sizeof(cfg->icon) - 1);
            strncpy(cfg->expect_pattern, entry.expect_pattern, sizeof(cfg->expect_pattern) - 1);
            strncpy(cfg->fail_pattern, entry.fail_pattern, sizeof(cfg->fail_pattern) - 1);
            strncpy(cfg->extract_pattern, entry.extract_pattern, sizeof(cfg->extract_pattern) - 1);
            strncpy(cfg->var_name, entry.var_name, sizeof(cfg->var_name) - 1);
            cfg->timeout_sec = entry.timeout_sec;
            cfg->stop_on_match = entry.stop_on_match != 0;
            cfg->nohup = entry.nohup != 0;
            cfg->enabled = entry.enabled != 0;
            cfg->created_time = entry.created_time;
            cfg->last_exec_time = entry.last_exec_time;
            /* 服务模式字段 */
            cfg->service_mode = entry.service_mode != 0;
            strncpy(cfg->ready_pattern, entry.ready_pattern, sizeof(cfg->ready_pattern) - 1);
            strncpy(cfg->service_fail_pattern, entry.service_fail_pattern, sizeof(cfg->service_fail_pattern) - 1);
            cfg->ready_timeout_sec = entry.ready_timeout_sec;
            cfg->ready_check_interval_ms = entry.ready_check_interval_ms;
            n++;
        }
    }
    
    *count = n;
    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

esp_err_t ts_ssh_commands_config_list_by_host(const char *host_id,
                                               ts_ssh_command_config_t *configs,
                                               size_t max_count, size_t *count)
{
    if (!s_state.initialized || !host_id || !configs || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_cmd_entry_t entry;
    size_t len;
    size_t n = 0;
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX && n < max_count; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            if (strcmp(entry.host_id, host_id) == 0) {
                ts_ssh_command_config_t *cfg = &configs[n];
                memset(cfg, 0, sizeof(*cfg));
                strncpy(cfg->id, entry.id, sizeof(cfg->id) - 1);
                strncpy(cfg->host_id, entry.host_id, sizeof(cfg->host_id) - 1);
                strncpy(cfg->name, entry.name, sizeof(cfg->name) - 1);
                strncpy(cfg->command, entry.command, sizeof(cfg->command) - 1);
                strncpy(cfg->desc, entry.desc, sizeof(cfg->desc) - 1);
                strncpy(cfg->icon, entry.icon, sizeof(cfg->icon) - 1);
                strncpy(cfg->expect_pattern, entry.expect_pattern, sizeof(cfg->expect_pattern) - 1);
                strncpy(cfg->fail_pattern, entry.fail_pattern, sizeof(cfg->fail_pattern) - 1);
                strncpy(cfg->extract_pattern, entry.extract_pattern, sizeof(cfg->extract_pattern) - 1);
                strncpy(cfg->var_name, entry.var_name, sizeof(cfg->var_name) - 1);
                cfg->timeout_sec = entry.timeout_sec;
                cfg->stop_on_match = entry.stop_on_match != 0;
                cfg->nohup = entry.nohup != 0;
                cfg->enabled = entry.enabled != 0;
                cfg->created_time = entry.created_time;
                cfg->last_exec_time = entry.last_exec_time;
                /* 服务模式字段 */
                cfg->service_mode = entry.service_mode != 0;
                strncpy(cfg->ready_pattern, entry.ready_pattern, sizeof(cfg->ready_pattern) - 1);
                strncpy(cfg->service_fail_pattern, entry.service_fail_pattern, sizeof(cfg->service_fail_pattern) - 1);
                cfg->ready_timeout_sec = entry.ready_timeout_sec;
                cfg->ready_check_interval_ms = entry.ready_check_interval_ms;
                n++;
            }
        }
    }
    
    *count = n;
    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

size_t ts_ssh_commands_config_count(void)
{
    if (!s_state.initialized) {
        return 0;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_cmd_entry_t entry;
    size_t len;
    size_t count = 0;
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            count++;
        }
    }
    
    xSemaphoreGive(s_state.mutex);
    return count;
}

static esp_err_t command_clear_impl(void) {
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        nvs_erase_key(s_state.nvs, key);
    }
    nvs_commit(s_state.nvs);
    
    xSemaphoreGive(s_state.mutex);
    ESP_LOGI(TAG, "Cleared all SSH commands");
    
    /* 同步到 SD 卡（清空后的空数组） */
    ts_ssh_commands_config_sync_to_sdcard();
    
    return ESP_OK;
}

esp_err_t ts_ssh_commands_config_update_exec_time(const char *id)
{
    if (!s_state.initialized || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_cmd_entry_t entry;
    size_t len;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            if (strcmp(entry.id, id) == 0) {
                entry.last_exec_time = get_current_time();
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

/*===========================================================================*/
/*                          Variable Pre-creation                             */
/*===========================================================================*/

esp_err_t ts_ssh_commands_precreate_variables(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Pre-creating variables for all SSH commands");
    precreate_all_variables();
    return ESP_OK;
}

/*===========================================================================*/
/*                    Iterator/Pagination API (内存优化)                       */
/*===========================================================================*/

/**
 * @brief 内部辅助：从 NVS entry 填充 config 结构体
 */
static void entry_to_config(const nvs_cmd_entry_t *entry, ts_ssh_command_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id, entry->id, sizeof(cfg->id) - 1);
    strncpy(cfg->host_id, entry->host_id, sizeof(cfg->host_id) - 1);
    strncpy(cfg->name, entry->name, sizeof(cfg->name) - 1);
    strncpy(cfg->command, entry->command, sizeof(cfg->command) - 1);
    strncpy(cfg->desc, entry->desc, sizeof(cfg->desc) - 1);
    strncpy(cfg->icon, entry->icon, sizeof(cfg->icon) - 1);
    strncpy(cfg->expect_pattern, entry->expect_pattern, sizeof(cfg->expect_pattern) - 1);
    strncpy(cfg->fail_pattern, entry->fail_pattern, sizeof(cfg->fail_pattern) - 1);
    strncpy(cfg->extract_pattern, entry->extract_pattern, sizeof(cfg->extract_pattern) - 1);
    strncpy(cfg->var_name, entry->var_name, sizeof(cfg->var_name) - 1);
    cfg->timeout_sec = entry->timeout_sec;
    cfg->stop_on_match = entry->stop_on_match != 0;
    cfg->nohup = entry->nohup != 0;
    cfg->enabled = entry->enabled != 0;
    cfg->created_time = entry->created_time;
    cfg->last_exec_time = entry->last_exec_time;
    /* 服务模式字段 */
    cfg->service_mode = entry->service_mode != 0;
    strncpy(cfg->ready_pattern, entry->ready_pattern, sizeof(cfg->ready_pattern) - 1);
    strncpy(cfg->service_fail_pattern, entry->service_fail_pattern, sizeof(cfg->service_fail_pattern) - 1);
    cfg->ready_timeout_sec = entry->ready_timeout_sec;
    cfg->ready_check_interval_ms = entry->ready_check_interval_ms;
}

esp_err_t ts_ssh_commands_config_iterate(ts_ssh_cmd_iterator_cb_t callback,
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
    nvs_cmd_entry_t entry;
    ts_ssh_command_config_t cfg;
    size_t len;
    size_t total = 0;       /* 总有效条目数 */
    size_t returned = 0;    /* 实际返回给回调的数量 */
    size_t skipped = 0;     /* 跳过的条目数（offset） */
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
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
            entry_to_config(&entry, &cfg);
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

esp_err_t ts_ssh_commands_config_iterate_by_host(const char *host_id,
                                                   ts_ssh_cmd_iterator_cb_t callback,
                                                   void *user_data,
                                                   size_t offset,
                                                   size_t limit,
                                                   size_t *total_count)
{
    if (!s_state.initialized || !host_id || !callback) {
        return ESP_ERR_INVALID_ARG;
    }
    
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    char key[16];
    nvs_cmd_entry_t entry;
    ts_ssh_command_config_t cfg;
    size_t len;
    size_t total = 0;       /* 该主机的总条目数 */
    size_t returned = 0;    /* 实际返回给回调的数量 */
    size_t skipped = 0;     /* 跳过的条目数（offset） */
    
    for (int i = 0; i < TS_SSH_COMMANDS_MAX; i++) {
        make_nvs_key(i, key, sizeof(key));
        len = sizeof(entry);
        if (nvs_get_blob(s_state.nvs, key, &entry, &len) == ESP_OK && len == sizeof(entry)) {
            /* 检查 host_id 匹配 */
            if (strcmp(entry.host_id, host_id) != 0) {
                continue;
            }
            
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
            entry_to_config(&entry, &cfg);
            bool cont = callback(&cfg, returned, user_data);
            returned++;
            
            if (!cont) {
                break;
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

/* 前向声明 */
static cJSON *cmd_to_json(const ts_ssh_command_config_t *cfg);

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
 * @brief 确保指令文件夹存在
 */
static esp_err_t ensure_commands_dir(void)
{
    esp_err_t ret = ensure_config_dir();
    if (ret != ESP_OK) return ret;
    
    struct stat st;
    if (stat(TS_SSH_COMMANDS_SDCARD_DIR, &st) != 0) {
        if (mkdir(TS_SSH_COMMANDS_SDCARD_DIR, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create commands dir: %s", TS_SSH_COMMANDS_SDCARD_DIR);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/**
 * @brief 删除 SD 卡上的指令配置文件
 * 
 * 同时尝试删除 .json 和 .tscfg 文件（如果存在）
 */
static void delete_command_file(const char *id)
{
    if (!id || !id[0] || !is_sdcard_mounted()) {
        return;
    }
    
    char filepath[128];
    
    /* 删除 .json 文件 */
    snprintf(filepath, sizeof(filepath), "%s/%s.json", TS_SSH_COMMANDS_SDCARD_DIR, id);
    if (unlink(filepath) == 0) {
        ESP_LOGI(TAG, "Deleted SD card file: %s", filepath);
    }
    
    /* 删除 .tscfg 文件（如果存在） */
    snprintf(filepath, sizeof(filepath), "%s/%s.tscfg", TS_SSH_COMMANDS_SDCARD_DIR, id);
    if (unlink(filepath) == 0) {
        ESP_LOGI(TAG, "Deleted SD card file: %s", filepath);
    }
}

/* 前向声明 */
static esp_err_t json_to_cmd(const cJSON *obj, ts_ssh_command_config_t *cfg);

/**
 * @brief 从独立文件目录加载所有 SSH 命令
 * 
 * 支持 .tscfg 加密配置优先加载
 * 设计逻辑与 rules/actions/sources 目录一致
 */
static esp_err_t load_commands_from_dir(void)
{
    DIR *dir = opendir(TS_SSH_COMMANDS_SDCARD_DIR);
    if (!dir) {
        ESP_LOGD(TAG, "Commands directory not found: %s", TS_SSH_COMMANDS_SDCARD_DIR);
        return ESP_ERR_NOT_FOUND;
    }
    
    int loaded = 0;
    int skipped = 0;
    struct dirent *entry;
    
    /* 使用堆分配避免栈溢出 */
    ts_ssh_command_config_t *cfg = heap_caps_malloc(sizeof(ts_ssh_command_config_t), MALLOC_CAP_SPIRAM);
    if (!cfg) cfg = malloc(sizeof(ts_ssh_command_config_t));
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
            snprintf(tscfg_path, sizeof(tscfg_path), "%s/%s", TS_SSH_COMMANDS_SDCARD_DIR, tscfg_name);
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
                     TS_SSH_COMMANDS_SDCARD_DIR, (int)(len - 6), entry->d_name);
        } else {
            snprintf(filepath, sizeof(filepath), "%s/%.60s", TS_SSH_COMMANDS_SDCARD_DIR, entry->d_name);
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
        
        /* 判断 JSON 格式：
         * - 配置包格式（.tscfg 解密后）: {"type":"ssh_command", "command":{...}}
         * - 直接格式（.json）: {"id":"...", "host_id":"...", ...}
         */
        cJSON *cmd_obj = root;
        cJSON *type_item = cJSON_GetObjectItem(root, "type");
        if (type_item && cJSON_IsString(type_item) && 
            strcmp(type_item->valuestring, "ssh_command") == 0) {
            /* 配置包格式，提取 command 对象 */
            cJSON *cmd_item = cJSON_GetObjectItem(root, "command");
            if (cmd_item && cJSON_IsObject(cmd_item)) {
                cmd_obj = cmd_item;
                ESP_LOGD(TAG, "Detected config pack format for %s", filepath);
            } else {
                ESP_LOGW(TAG, "Invalid config pack format (missing 'command'): %s", filepath);
                cJSON_Delete(root);
                skipped++;
                continue;
            }
        }
        
        /* 解析并添加 */
        memset(cfg, 0, sizeof(ts_ssh_command_config_t));
        if (json_to_cmd(cmd_obj, cfg) == ESP_OK && cfg->host_id[0] && cfg->name[0]) {
            esp_err_t add_ret = ts_ssh_commands_config_add(cfg, NULL, 0);
            if (add_ret == ESP_OK) {
                loaded++;
                ESP_LOGD(TAG, "Loaded command from file: %s%s", cfg->id, 
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
        ESP_LOGI(TAG, "Loaded %d SSH commands from directory: %s (skipped %d)", 
                 loaded, TS_SSH_COMMANDS_SDCARD_DIR, skipped);
        return ESP_OK;
    }
    
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief 导出单条指令到独立文件
 */
static esp_err_t export_command_to_file(const ts_ssh_command_config_t *cfg)
{
    if (!cfg || !cfg->id[0]) return ESP_ERR_INVALID_ARG;
    
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s.json", TS_SSH_COMMANDS_SDCARD_DIR, cfg->id);
    
    cJSON *obj = cmd_to_json(cfg);
    if (!obj) return ESP_ERR_NO_MEM;
    
    char *str = cJSON_Print(obj);  /* 格式化输出，便于阅读 */
    cJSON_Delete(obj);
    
    if (!str) return ESP_ERR_NO_MEM;
    
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        cJSON_free(str);
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return ESP_FAIL;
    }
    
    fprintf(fp, "%s\n", str);
    fclose(fp);
    cJSON_free(str);
    
    return ESP_OK;
}

/**
 * @brief 将单条命令转换为 JSON 对象
 */
static cJSON *cmd_to_json(const ts_ssh_command_config_t *cfg)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    
    cJSON_AddStringToObject(obj, "id", cfg->id);
    cJSON_AddStringToObject(obj, "host_id", cfg->host_id);
    cJSON_AddStringToObject(obj, "name", cfg->name);
    cJSON_AddStringToObject(obj, "command", cfg->command);
    if (cfg->desc[0]) cJSON_AddStringToObject(obj, "desc", cfg->desc);
    if (cfg->icon[0]) cJSON_AddStringToObject(obj, "icon", cfg->icon);
    if (cfg->expect_pattern[0]) cJSON_AddStringToObject(obj, "expect_pattern", cfg->expect_pattern);
    if (cfg->fail_pattern[0]) cJSON_AddStringToObject(obj, "fail_pattern", cfg->fail_pattern);
    if (cfg->extract_pattern[0]) cJSON_AddStringToObject(obj, "extract_pattern", cfg->extract_pattern);
    if (cfg->var_name[0]) cJSON_AddStringToObject(obj, "var_name", cfg->var_name);
    cJSON_AddNumberToObject(obj, "timeout_sec", cfg->timeout_sec);
    cJSON_AddBoolToObject(obj, "stop_on_match", cfg->stop_on_match);
    cJSON_AddBoolToObject(obj, "nohup", cfg->nohup);
    cJSON_AddBoolToObject(obj, "enabled", cfg->enabled);
    /* 服务模式字段 */
    cJSON_AddBoolToObject(obj, "service_mode", cfg->service_mode);
    if (cfg->ready_pattern[0]) cJSON_AddStringToObject(obj, "ready_pattern", cfg->ready_pattern);
    if (cfg->service_fail_pattern[0]) cJSON_AddStringToObject(obj, "service_fail_pattern", cfg->service_fail_pattern);
    if (cfg->ready_timeout_sec > 0) cJSON_AddNumberToObject(obj, "ready_timeout_sec", cfg->ready_timeout_sec);
    if (cfg->ready_check_interval_ms > 0) cJSON_AddNumberToObject(obj, "ready_check_interval_ms", cfg->ready_check_interval_ms);
    cJSON_AddNumberToObject(obj, "created_time", cfg->created_time);
    cJSON_AddNumberToObject(obj, "last_exec_time", cfg->last_exec_time);
    
    return obj;
}

/**
 * @brief 从 JSON 对象解析命令配置
 */
static esp_err_t json_to_cmd(const cJSON *obj, ts_ssh_command_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    
    cJSON *item;
    
    item = cJSON_GetObjectItem(obj, "id");
    if (item && cJSON_IsString(item)) strncpy(cfg->id, item->valuestring, sizeof(cfg->id) - 1);
    
    item = cJSON_GetObjectItem(obj, "host_id");
    if (item && cJSON_IsString(item)) strncpy(cfg->host_id, item->valuestring, sizeof(cfg->host_id) - 1);
    
    item = cJSON_GetObjectItem(obj, "name");
    if (item && cJSON_IsString(item)) strncpy(cfg->name, item->valuestring, sizeof(cfg->name) - 1);
    
    item = cJSON_GetObjectItem(obj, "command");
    if (item && cJSON_IsString(item)) strncpy(cfg->command, item->valuestring, sizeof(cfg->command) - 1);
    
    item = cJSON_GetObjectItem(obj, "desc");
    if (item && cJSON_IsString(item)) strncpy(cfg->desc, item->valuestring, sizeof(cfg->desc) - 1);
    
    item = cJSON_GetObjectItem(obj, "icon");
    if (item && cJSON_IsString(item)) strncpy(cfg->icon, item->valuestring, sizeof(cfg->icon) - 1);
    
    /* 支持 snake_case 和 camelCase 两种格式 */
    item = cJSON_GetObjectItem(obj, "expect_pattern");
    if (!item) item = cJSON_GetObjectItem(obj, "expectPattern");
    if (item && cJSON_IsString(item)) strncpy(cfg->expect_pattern, item->valuestring, sizeof(cfg->expect_pattern) - 1);
    
    item = cJSON_GetObjectItem(obj, "fail_pattern");
    if (!item) item = cJSON_GetObjectItem(obj, "failPattern");
    if (item && cJSON_IsString(item)) strncpy(cfg->fail_pattern, item->valuestring, sizeof(cfg->fail_pattern) - 1);
    
    item = cJSON_GetObjectItem(obj, "extract_pattern");
    if (!item) item = cJSON_GetObjectItem(obj, "extractPattern");
    if (item && cJSON_IsString(item)) strncpy(cfg->extract_pattern, item->valuestring, sizeof(cfg->extract_pattern) - 1);
    
    item = cJSON_GetObjectItem(obj, "var_name");
    if (!item) item = cJSON_GetObjectItem(obj, "varName");
    if (item && cJSON_IsString(item)) strncpy(cfg->var_name, item->valuestring, sizeof(cfg->var_name) - 1);
    
    item = cJSON_GetObjectItem(obj, "timeout_sec");
    if (!item) item = cJSON_GetObjectItem(obj, "timeout");
    if (item && cJSON_IsNumber(item)) cfg->timeout_sec = (uint16_t)item->valueint;
    else cfg->timeout_sec = 30;
    
    item = cJSON_GetObjectItem(obj, "stop_on_match");
    if (!item) item = cJSON_GetObjectItem(obj, "stopOnMatch");
    if (item && cJSON_IsBool(item)) cfg->stop_on_match = cJSON_IsTrue(item);
    
    item = cJSON_GetObjectItem(obj, "nohup");
    if (item && cJSON_IsBool(item)) cfg->nohup = cJSON_IsTrue(item);
    
    item = cJSON_GetObjectItem(obj, "enabled");
    if (item && cJSON_IsBool(item)) cfg->enabled = cJSON_IsTrue(item);
    else cfg->enabled = true;  /* 默认启用 */
    
    /* 服务模式字段 */
    item = cJSON_GetObjectItem(obj, "service_mode");
    if (!item) item = cJSON_GetObjectItem(obj, "serviceMode");
    if (item && cJSON_IsBool(item)) cfg->service_mode = cJSON_IsTrue(item);
    
    item = cJSON_GetObjectItem(obj, "ready_pattern");
    if (!item) item = cJSON_GetObjectItem(obj, "readyPattern");
    if (item && cJSON_IsString(item)) strncpy(cfg->ready_pattern, item->valuestring, sizeof(cfg->ready_pattern) - 1);
    
    item = cJSON_GetObjectItem(obj, "service_fail_pattern");
    if (!item) item = cJSON_GetObjectItem(obj, "serviceFailPattern");
    if (item && cJSON_IsString(item)) strncpy(cfg->service_fail_pattern, item->valuestring, sizeof(cfg->service_fail_pattern) - 1);
    
    item = cJSON_GetObjectItem(obj, "ready_timeout_sec");
    if (!item) item = cJSON_GetObjectItem(obj, "readyTimeout");
    if (item && cJSON_IsNumber(item)) cfg->ready_timeout_sec = (uint16_t)item->valueint;
    
    item = cJSON_GetObjectItem(obj, "ready_check_interval_ms");
    if (!item) item = cJSON_GetObjectItem(obj, "readyInterval");
    if (item && cJSON_IsNumber(item)) cfg->ready_check_interval_ms = (uint16_t)item->valueint;
    
    item = cJSON_GetObjectItem(obj, "created_time");
    if (item && cJSON_IsNumber(item)) cfg->created_time = (uint32_t)item->valueint;
    
    item = cJSON_GetObjectItem(obj, "last_exec_time");
    if (item && cJSON_IsNumber(item)) cfg->last_exec_time = (uint32_t)item->valueint;
    
    return ESP_OK;
}

/**
 * @brief 导出迭代器回调上下文
 */
typedef struct {
    FILE *fp;           /* 单文件输出 */
    int count;          /* 计数 */
    bool first;         /* 首条标记 */
    bool export_dir;    /* 是否同时导出到独立文件 */
} export_ctx_t;

/**
 * @brief 导出迭代器回调 - 将单条命令写入文件（同时导出到独立文件）
 */
static bool export_iterator_cb(const ts_ssh_command_config_t *config, size_t index, void *user_data)
{
    (void)index;
    export_ctx_t *ctx = (export_ctx_t *)user_data;
    
    /* 导出到独立文件 */
    if (ctx->export_dir) {
        export_command_to_file(config);
    }
    
    /* 导出到单文件（JSON 数组） */
    if (ctx->fp) {
        cJSON *obj = cmd_to_json(config);
        if (!obj) return true;
        
        char *str = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        
        if (str) {
            if (!ctx->first) {
                fprintf(ctx->fp, ",\n");
            }
            fprintf(ctx->fp, "  %s", str);
            cJSON_free(str);
            ctx->first = false;
        }
    }
    
    ctx->count++;
    return true;
}

esp_err_t ts_ssh_commands_config_export_to_sdcard(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!is_sdcard_mounted()) {
        ESP_LOGD(TAG, "SD card not mounted, skip export");
        return ESP_ERR_NOT_FOUND;
    }
    
    /* 确保目录存在 */
    esp_err_t ret = ensure_commands_dir();
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 只导出到独立文件（不再生成主配置文件） */
    export_ctx_t ctx = {
        .fp = NULL,           /* 不写主配置文件 */
        .count = 0,
        .first = true,
        .export_dir = true,   /* 导出到独立文件 */
    };
    
    /* 使用迭代器逐条导出 */
    ret = ts_ssh_commands_config_iterate(export_iterator_cb, &ctx, 0, 0, NULL);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Exported %d SSH commands to %s/", ctx.count, TS_SSH_COMMANDS_SDCARD_DIR);
    }
    
    return ret;
}

esp_err_t ts_ssh_commands_config_import_from_sdcard(bool merge)
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
        esp_err_t cleared = ts_ssh_commands_config_clear();
        if (cleared != ESP_OK) {
            s_loading_from_sdcard = false;
            return cleared;
        }
    }
    
    /* 只从目录加载独立文件（.tscfg 优先于 .json） */
    esp_err_t ret = load_commands_from_dir();
    
    /* 清除加载标志 */
    s_loading_from_sdcard = false;
    
    return ret;
}

/**
 * @brief 异步同步任务 - 延迟执行 SD 卡导出
 */
static void async_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));  /* 短延迟，等待当前操作完成 */
    
    if (s_state.initialized) {
        ESP_LOGI(TAG, "Async sync to SD card...");
        ts_ssh_commands_config_export_to_sdcard();
    }
    
    vTaskDelete(NULL);
}

void ts_ssh_commands_config_sync_to_sdcard(void)
{
    /* 异步执行 SD 卡同步（避免在 API 处理任务中执行导致栈溢出/超时）
     * 必须使用 DRAM 栈，因为内部会访问 NVS */
    xTaskCreate(async_sync_task, "ssh_cmd_sync", 8192, NULL, 2, NULL);
}

esp_err_t ts_ssh_commands_config_add(const ts_ssh_command_config_t *cfg, char *out, size_t size) {
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    ts_ssh_binding_lock();
    ts_ssh_command_config_t *old =
        heap_caps_malloc(sizeof(*old), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!old) {
        ts_ssh_binding_unlock();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t got = ts_ssh_commands_config_get(cfg->id, old);
    bool changed = got == ESP_OK &&
                   (strcmp(old->host_id, cfg->host_id) || strcmp(old->name, cfg->name) ||
                    strcmp(old->command, cfg->command) || strcmp(old->var_name, cfg->var_name) ||
                    old->nohup != cfg->nohup || old->service_mode != cfg->service_mode);
    bool busy = (changed || got == ESP_ERR_NOT_FOUND) && ts_ssh_service_command_protected(cfg->id);
    free(old);
    esp_err_t ret = busy ? ESP_ERR_INVALID_STATE : command_add_impl(cfg, out, size);
    ts_ssh_binding_unlock();
    return ret;
}
esp_err_t ts_ssh_commands_config_remove(const char *id) {
    if (!id)
        return ESP_ERR_INVALID_ARG;
    ts_ssh_binding_lock();
    uint32_t credential = 0;
    esp_err_t ret = ts_ssh_service_command_protected(id) ? ESP_ERR_INVALID_STATE
                                                        : ts_ssh_service_delete_begin(id, &credential);
    if (ret == ESP_OK) {
        ret = command_remove_impl(id);
        ts_ssh_service_delete_finish(id, credential, ret == ESP_OK);
    }
    ts_ssh_binding_unlock();
    return ret;
}
esp_err_t ts_ssh_commands_config_clear(void) {
    ts_ssh_binding_lock();
    esp_err_t ret =
        (xTaskGetCurrentTaskHandle() == initial_loader ? ts_ssh_service_any_in_use()
                                                       : ts_ssh_service_host_protected(NULL))
            ? ESP_ERR_INVALID_STATE
            : command_clear_impl();
    if (ret == ESP_OK) ts_ssh_service_forget_stopped();
    ts_ssh_binding_unlock();
    return ret;
}
