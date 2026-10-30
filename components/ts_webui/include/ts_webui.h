/**
 * @file ts_webui.h
 * @brief TianShanOS Web UI API
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WebUI
 */
esp_err_t ts_webui_init(void);

/**
 * @brief Deinitialize WebUI
 */
esp_err_t ts_webui_deinit(void);

/**
 * @brief Start WebUI server
 */
esp_err_t ts_webui_start(void);

/**
 * @brief Stop WebUI server
 */
esp_err_t ts_webui_stop(void);

/**
 * @brief Check if WebUI is running
 */
bool ts_webui_is_running(void);

/**
 * @brief Broadcast message to all WebSocket clients
 */
esp_err_t ts_webui_broadcast(const char *message);

/**
 * @brief Broadcast event to WebSocket clients
 */
esp_err_t ts_webui_broadcast_event(const char *event_type, const char *data);

/*===========================================================================*/
/*                     SSH Exec Stream Functions                              */
/*===========================================================================*/

/**
 * @brief SSH 命令执行状态（WebUI 扩展）
 */
typedef enum {
    TS_WEBUI_SSH_STATUS_RUNNING,      /**< 命令执行中 */
    TS_WEBUI_SSH_STATUS_SUCCESS,      /**< 命令成功完成 */
    TS_WEBUI_SSH_STATUS_FAILED,       /**< 命令失败（exit_code != 0 或执行错误） */
    TS_WEBUI_SSH_STATUS_TIMEOUT,      /**< 命令超时 */
    TS_WEBUI_SSH_STATUS_CANCELLED,    /**< 用户取消 */
    TS_WEBUI_SSH_STATUS_MATCH_SUCCESS,/**< 成功且匹配到期望模式 */
    TS_WEBUI_SSH_STATUS_MATCH_FAILED, /**< 成功但未匹配到期望模式 */
} ts_webui_ssh_status_t;

/**
 * @brief SSH Exec 选项配置（可选）
 */
typedef struct {
    const char *expect_pattern;    /**< 期望匹配的模式（简单字符串包含检查）*/
    const char *fail_pattern;      /**< 失败模式（匹配则视为失败）*/
    const char *extract_pattern;   /**< 提取模式，格式: "prefix(capture)suffix" */
    uint32_t timeout_ms;           /**< 命令超时时间（0 = 使用默认 30s）*/
    bool collect_output;           /**< 是否收集完整输出（默认 true）*/
    uint32_t max_output_size;      /**< 最大输出收集大小（0 = 默认 64KB）*/
    bool stop_on_match;            /**< 匹配成功后是否自动终止命令（用于 ping 等持续命令）*/
    const char *var_name;          /**< 变量名（用于存储结果到变量系统）*/
    const char *command_id;        /**< 已保存命令的身份；临时命令为空 */
    uint64_t command_instance;     /**< 发起时的命令实例 */
} ts_webui_ssh_options_t;

/**
 * @brief SSH Exec 执行结果（WebUI 扩展）
 */
typedef struct {
    ts_webui_ssh_status_t status;  /**< 执行状态 */
    int exit_code;                 /**< 命令退出码 */
    bool pattern_matched;          /**< 是否匹配到 expect_pattern */
    bool fail_pattern_matched;     /**< 是否匹配到 fail_pattern */
    char *extracted_value;         /**< 提取的值（需要调用者释放）*/
    char *output;                  /**< 完整输出（需要调用者释放，如果 collect_output=true）*/
    size_t output_len;             /**< 输出长度 */
    char *error_msg;               /**< 错误信息（需要调用者释放）*/
} ts_webui_ssh_result_t;

/**
 * @brief 释放 SSH Exec 结果
 */
void ts_webui_ssh_result_free(ts_webui_ssh_result_t *result);

/**
 * @brief Start SSH exec stream session (basic)
 * 
 * @param host Remote host
 * @param port SSH port
 * @param user Username
 * @param keyid Key ID for authentication (optional, use NULL for password)
 * @param password Password (optional, use NULL for key auth)
 * @param command Command to execute
 * @param session_id Output session ID for cancel/tracking
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_webui_ssh_exec_start(const char *host, uint16_t port, 
                                   const char *user, const char *keyid,
                                   const char *password, const char *command,
                                   uint32_t *session_id);

/**
 * @brief Start SSH exec stream session with options
 * 
 * @param host Remote host
 * @param port SSH port
 * @param user Username
 * @param keyid Key ID for authentication (optional)
 * @param password Password (optional)
 * @param command Command to execute
 * @param options Execution options (can be NULL for defaults)
 * @param session_id Output session ID
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_webui_ssh_exec_start_ex(const char *host, uint16_t port, 
                                      const char *user, const char *keyid,
                                      const char *password, const char *command,
                                      const ts_webui_ssh_options_t *options,
                                      uint32_t *session_id);

/**
 * @brief Cancel running SSH exec session
 * 
 * @param session_id Session ID to cancel
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_webui_ssh_exec_cancel(uint32_t session_id);

/**
 * @brief Check if SSH exec session is running
 * 
 * @param session_id Session ID to check
 * @return true if running, false otherwise
 */
bool ts_webui_ssh_exec_is_running(uint32_t session_id);

#ifdef __cplusplus
}
#endif
