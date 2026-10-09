/**
 * @file ts_ssh_hosts_config.h
 * @brief SSH Host Configuration Storage
 *
 * 管理 SSH 主机凭证配置，存储到 NVS 持久化。
 * 与 ts_known_hosts（存储主机指纹）不同，这里存储的是连接凭证：
 * - 主机地址、端口、用户名
 * - 认证方式（密钥ID 或 密码）
 *
 * 典型使用场景：
 * 1. ssh.copyid 部署公钥成功后自动注册
 * 2. 手动添加 SSH 主机配置
 * 3. 自动化引擎执行 SSH 动作时查找主机配置
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_SSH_HOSTS_CONFIG_H
#define TS_SSH_HOSTS_CONFIG_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

#define TS_SSH_HOST_ID_MAX      32   /**< 主机 ID 最大长度 */
#define TS_SSH_HOST_ADDR_MAX    64   /**< 主机地址最大长度 */
#define TS_SSH_USERNAME_MAX     32   /**< 用户名最大长度 */
#define TS_SSH_KEYID_MAX        32   /**< 密钥 ID 最大长度 */
#define TS_SSH_PASSWORD_MAX     64   /**< 密码最大长度（仅内存，不持久化） */
#define TS_SSH_HOSTS_MAX        16   /**< 最大主机数量 */

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief SSH 主机认证方式
 */
typedef enum {
    TS_SSH_HOST_AUTH_KEY = 0,       /**< 密钥认证（推荐） */
    TS_SSH_HOST_AUTH_PASSWORD,       /**< 密码认证（不推荐，密码不持久化） */
} ts_ssh_host_auth_type_t;

/**
 * @brief SSH 主机配置
 */
typedef struct {
    char id[TS_SSH_HOST_ID_MAX];        /**< 唯一 ID，如 "agx0", "jetson1" */
    char host[TS_SSH_HOST_ADDR_MAX];    /**< 主机地址 */
    uint16_t port;                       /**< SSH 端口 */
    char username[TS_SSH_USERNAME_MAX]; /**< 用户名 */
    ts_ssh_host_auth_type_t auth_type;  /**< 认证方式 */
    char keyid[TS_SSH_KEYID_MAX];       /**< 密钥 ID（auth_type=KEY 时使用） */
    /* 注意：密码不持久化到 NVS，仅运行时内存 */
    uint32_t created_time;               /**< 创建时间戳 */
    uint32_t last_used_time;             /**< 上次使用时间 */
    bool enabled;                        /**< 是否启用 */
} ts_ssh_host_config_t;

/*===========================================================================*/
/*                          Initialization                                    */
/*===========================================================================*/

/**
 * @brief 初始化 SSH 主机配置模块
 * 
 * @return ESP_OK 成功
 */
esp_err_t ts_ssh_hosts_config_init(void);

/**
 * @brief 反初始化模块
 */
void ts_ssh_hosts_config_deinit(void);

/**
 * @brief 检查模块是否已初始化
 */
bool ts_ssh_hosts_config_is_initialized(void);
esp_err_t ts_ssh_hosts_config_load_state(void);

/*===========================================================================*/
/*                          CRUD Operations                                   */
/*===========================================================================*/

/**
 * @brief 添加或更新 SSH 主机配置
 * 
 * @param config 主机配置
 * @return ESP_OK 成功, ESP_ERR_NO_MEM 已满, ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t ts_ssh_hosts_config_add(const ts_ssh_host_config_t *config);

/**
 * @brief 删除 SSH 主机配置
 * 
 * @param id 主机 ID
 * @return ESP_OK 成功, ESP_ERR_NOT_FOUND 未找到
 */
esp_err_t ts_ssh_hosts_config_remove(const char *id);

/**
 * @brief 获取 SSH 主机配置
 * 
 * @param id 主机 ID
 * @param config 输出配置
 * @return ESP_OK 成功, ESP_ERR_NOT_FOUND 未找到
 */
esp_err_t ts_ssh_hosts_config_get(const char *id, ts_ssh_host_config_t *config);

/**
 * @brief 通过主机地址查找配置
 * 
 * @param host 主机地址
 * @param port 端口
 * @param username 用户名（可选，NULL 表示任意）
 * @param config 输出配置
 * @return ESP_OK 成功, ESP_ERR_NOT_FOUND 未找到
 */
esp_err_t ts_ssh_hosts_config_find(const char *host, uint16_t port, 
                                    const char *username,
                                    ts_ssh_host_config_t *config);

/**
 * @brief 列出所有 SSH 主机配置
 * 
 * @param configs 输出数组
 * @param max_count 数组大小
 * @param count 实际数量
 * @return ESP_OK 成功
 */
esp_err_t ts_ssh_hosts_config_list(ts_ssh_host_config_t *configs, 
                                    size_t max_count, 
                                    size_t *count);

/**
 * @brief 获取主机数量
 */
int ts_ssh_hosts_config_count(void);

/**
 * @brief 更新上次使用时间
 * 
 * @param id 主机 ID
 * @return ESP_OK 成功
 */
esp_err_t ts_ssh_hosts_config_touch(const char *id);

/**
 * @brief 清除所有主机配置
 */
esp_err_t ts_ssh_hosts_config_clear(void);

/*===========================================================================*/
/*                    Iterator/Pagination API (内存优化)                       */
/*===========================================================================*/

/**
 * @brief 迭代器回调函数类型
 * 
 * @param config 当前主机配置（只读）
 * @param index 当前索引（0-based）
 * @param user_data 用户数据
 * @return true 继续遍历, false 停止遍历
 */
typedef bool (*ts_ssh_host_iterator_cb_t)(const ts_ssh_host_config_t *config,
                                           size_t index, void *user_data);

/**
 * @brief 流式遍历所有主机配置（内存优化）
 * 
 * 每次只加载一条配置到内存，通过回调处理。
 * 
 * @param callback 回调函数
 * @param user_data 传递给回调的用户数据
 * @param offset 起始偏移（分页用）
 * @param limit 最大返回数量（0 表示不限制）
 * @param[out] total_count 输出总数量（可为 NULL）
 * @return ESP_OK 成功
 */
esp_err_t ts_ssh_hosts_config_iterate(ts_ssh_host_iterator_cb_t callback,
                                       void *user_data,
                                       size_t offset,
                                       size_t limit,
                                       size_t *total_count);

/*===========================================================================*/
/*                    SD Card Export/Import (持久化备份)                       */
/*===========================================================================*/

/** SD 卡配置文件路径（主配置文件） */
#define TS_SSH_HOSTS_SDCARD_PATH  "/sdcard/config/ssh_hosts.json"

/** SD 卡独立文件目录（每个主机一个文件） */
#define TS_SSH_HOSTS_SDCARD_DIR   "/sdcard/config/ssh_hosts"

/**
 * @brief 导出所有主机配置到 SD 卡
 * 
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND SD卡未挂载
 */
esp_err_t ts_ssh_hosts_config_export_to_sdcard(void);

/**
 * @brief 从 SD 卡导入主机配置
 * 
 * @param merge 是否合并（true=保留现有，false=清空后导入）
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 文件不存在
 */
esp_err_t ts_ssh_hosts_config_import_from_sdcard(bool merge);

/**
 * @brief 同步到 SD 卡（内部调用，add/remove 后自动触发）
 */
void ts_ssh_hosts_config_sync_to_sdcard(void);

#ifdef __cplusplus
}
#endif

#endif /* TS_SSH_HOSTS_CONFIG_H */
