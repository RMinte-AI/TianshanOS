/**
 * @file ts_ssh_log_watch.h
 * @brief SSH Service Log Watcher
 *
 * 监测 nohup SSH 命令的日志文件，检测服务就绪状态。
 * 
 * 工作原理：
 * 1. nohup 命令执行后，启动后台 FreeRTOS 任务
 * 2. 定期通过 SSH 使用 grep 搜索日志文件中的模式
 * 3. 匹配 ready_pattern → 设置 ${var_name}.status = "ready"
 * 4. 匹配 fail_pattern → 设置 ${var_name}.status = "failed"
 * 5. 超时未匹配 → 设置 ${var_name}.status = "timeout"
 * 6. 任务自动停止，避免资源浪费
 * 
 * 使用场景：
 * - 启动远程服务后等待服务就绪（如 "Application startup complete."）
 * - WebUI 规则引擎根据 status 变量判断下一步动作
 * - 快捷按钮显示服务状态（启动中/就绪/超时/失败）
 *
 * @author TianShanOS Team
 * @version 1.1.0
 */

#ifndef TS_SSH_LOG_WATCH_H
#define TS_SSH_LOG_WATCH_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief 日志监测配置
 */
typedef struct {
    uint32_t run_generation;
    bool recovery_only;
    char command_id[32];
    char host_id[32];                /**< 主机 ID */
    char log_file[256];              /**< 日志文件路径（如 /tmp/ts_nohup_xxx.log） */
    char ready_pattern[128];         /**< 就绪匹配模式 */
    char fail_pattern[128];          /**< 失败匹配模式（可选） */
    char var_name[32];               /**< 变量名前缀（如 agx_start） */
    uint16_t timeout_sec;            /**< 超时时间（秒） */
    uint16_t check_interval_ms;      /**< 检测间隔（毫秒） */
} ts_ssh_log_watch_config_t;

/**
 * @brief 监测任务句柄（内部使用）
 */
typedef uint32_t ts_ssh_log_watch_handle_t;
esp_err_t ts_ssh_log_watch_init(void);
/* Stop requests are asynchronous; false means worker cleanup has completed. */
bool ts_ssh_log_watch_pending(ts_ssh_log_watch_handle_t handle);
esp_err_t ts_ssh_log_watch_cancel_name(const char *var_name);

/*===========================================================================*/
/*                          Public API                                        */
/*===========================================================================*/

/**
 * @brief 启动日志监测任务
 * 
 * 创建一个定时器任务，定期通过 SSH 读取日志文件并匹配模式。
 * 任务会自动设置变量：
 * - ${var_name}.status = "checking" | "ready" | "timeout"
 * - ${var_name}.ready_time = <timestamp>（就绪时）
 * 
 * @param config 监测配置
 * @param[out] out_handle 输出任务句柄（可用于取消，可为 NULL）
 * @return 
 *   - ESP_OK 成功启动
 *   - ESP_ERR_INVALID_ARG 参数无效
 *   - ESP_ERR_NO_MEM 内存不足
 */
esp_err_t ts_ssh_log_watch_start(const ts_ssh_log_watch_config_t *config,
                                   ts_ssh_log_watch_handle_t *out_handle);

/**
 * @brief 停止日志监测任务
 * 
 * @param handle 任务句柄
 * @return ESP_OK 成功停止
 */
esp_err_t ts_ssh_log_watch_stop(ts_ssh_log_watch_handle_t handle);

/**
 * @brief 检查任务是否正在运行
 * 
 * @param var_name 变量名前缀
 * @return true 正在运行，false 已停止或不存在
 */
bool ts_ssh_log_watch_is_running(const char *var_name);

/**
 * @brief 停止所有监测任务（清理）
 */
void ts_ssh_log_watch_stop_all(void);

/**
 * @brief 获取当前活跃的监测任务数量
 * 
 * @return 活跃任务数量
 */
int ts_ssh_log_watch_get_active_count(void);

/**
 * @brief 打印所有活跃监测任务的状态（调试用）
 */
void ts_ssh_log_watch_list(void);

#ifdef __cplusplus
}
#endif

#endif // TS_SSH_LOG_WATCH_H
