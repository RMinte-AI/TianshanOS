/**
 * @file ts_log.h
 * @brief TianShanOS Logging System
 *
 * 日志系统主头文件
 * 提供多级别、多输出目标的日志功能
 *
 * 特性：
 * - 多日志级别（ERROR, WARN, INFO, DEBUG, VERBOSE）
 * - 多输出目标（控制台、文件、内存缓冲）
 * - 运行时级别调整
 * - 日志文件轮转
 * - 彩色输出支持
 *
 * @author TianShanOS Team
 * @version 0.1.0
 */

#ifndef TS_LOG_H
#define TS_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 常量定义
 * ========================================================================== */

#ifndef CONFIG_TS_LOG_TAG_MAX_LENGTH
#define CONFIG_TS_LOG_TAG_MAX_LENGTH 16
#endif

#ifndef CONFIG_TS_LOG_MESSAGE_MAX_LENGTH
#define CONFIG_TS_LOG_MESSAGE_MAX_LENGTH 256
#endif

#ifndef CONFIG_TS_LOG_BUFFER_SIZE
#define CONFIG_TS_LOG_BUFFER_SIZE 100
#endif

#ifndef CONFIG_TS_LOG_DEFAULT_LEVEL
#define CONFIG_TS_LOG_DEFAULT_LEVEL 3  // INFO
#endif

/** 日志标签最大长度 */
#define TS_LOG_TAG_MAX_LEN CONFIG_TS_LOG_TAG_MAX_LENGTH

/** 日志消息最大长度 */
#define TS_LOG_MSG_MAX_LEN CONFIG_TS_LOG_MESSAGE_MAX_LENGTH

/** 日志缓冲区大小（条目数）*/
#define TS_LOG_BUFFER_SIZE CONFIG_TS_LOG_BUFFER_SIZE

/* ============================================================================
 * 类型定义
 * ========================================================================== */

/**
 * @brief 日志级别
 */
typedef enum {
    TS_LOG_NONE = 0,    /**< 禁用日志 */
    TS_LOG_ERROR,       /**< 错误 */
    TS_LOG_WARN,        /**< 警告 */
    TS_LOG_INFO,        /**< 信息 */
    TS_LOG_DEBUG,       /**< 调试 */
    TS_LOG_VERBOSE,     /**< 详细 */
    TS_LOG_MAX          /**< 级别数量 */
} ts_log_level_t;

/**
 * @brief 日志输出目标
 */
typedef enum {
    TS_LOG_OUTPUT_CONSOLE = (1 << 0),   /**< 控制台（UART）*/
    TS_LOG_OUTPUT_FILE    = (1 << 1),   /**< 文件 */
    TS_LOG_OUTPUT_BUFFER  = (1 << 2),   /**< 内存缓冲 */
    TS_LOG_OUTPUT_ALL     = 0xFF        /**< 所有输出 */
} ts_log_output_t;

/**
 * @brief 日志条目结构
 */
typedef struct {
    uint32_t timestamp_ms;                      /**< 时间戳（毫秒）*/
    ts_log_level_t level;                       /**< 日志级别 */
    char tag[TS_LOG_TAG_MAX_LEN];              /**< 日志标签 */
    char message[TS_LOG_MSG_MAX_LEN];          /**< 日志消息 */
    char task_name[16];                         /**< 任务名称 */
} ts_log_entry_t;

/**
 * @brief 日志回调函数类型
 *
 * @param entry 日志条目
 * @param user_data 用户数据
 */
typedef void (*ts_log_callback_t)(const ts_log_entry_t *entry, void *user_data);

/**
 * @brief 日志回调句柄
 */
typedef struct ts_log_callback_handle *ts_log_callback_handle_t;

/**
 * @brief 日志统计信息
 */
typedef struct {
    size_t buffer_capacity;         /**< 缓冲区容量 */
    size_t buffer_count;            /**< 当前缓冲区中的日志数 */
    uint32_t total_captured;        /**< 总捕获日志数（含溢出覆盖的）*/
    uint32_t capture_dropped;
    uint32_t capture_truncated;
    uint32_t dropped;               /**< 丢弃的日志数 */
    bool esp_log_capture_enabled;   /**< ESP_LOG 捕获是否启用 */
} ts_log_stats_t;

/* ============================================================================
 * 初始化和反初始化
 * ========================================================================== */

/**
 * @brief 初始化日志系统
 *
 * @return
 *      - ESP_OK: 成功
 *      - ESP_ERR_INVALID_STATE: 已经初始化
 *      - ESP_ERR_NO_MEM: 内存不足
 */
esp_err_t ts_log_init(void);

/**
 * @brief 反初始化日志系统
 *
 * @return ESP_OK 成功
 */
esp_err_t ts_log_deinit(void);

/**
 * @brief 检查日志系统是否已初始化
 *
 * @return true 已初始化
 */
bool ts_log_is_initialized(void);

/* ============================================================================
 * 日志输出 API
 * ========================================================================== */

/**
 * @brief 输出日志（格式化字符串）
 *
 * @param level 日志级别
 * @param tag 日志标签
 * @param format 格式化字符串
 * @param ... 可变参数
 */
void ts_log(ts_log_level_t level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

/**
 * @brief 输出日志（可变参数列表）
 *
 * @param level 日志级别
 * @param tag 日志标签
 * @param format 格式化字符串
 * @param args 可变参数列表
 */
void ts_log_v(ts_log_level_t level, const char *tag, const char *format, va_list args);

/**
 * @brief 输出十六进制数据
 *
 * @param level 日志级别
 * @param tag 日志标签
 * @param data 数据指针
 * @param length 数据长度
 */
void ts_log_hex(ts_log_level_t level, const char *tag, const void *data, size_t length);

/* ============================================================================
 * 便捷宏定义
 * ========================================================================== */

/**
 * @brief 错误日志宏
 */
#define TS_LOGE(tag, format, ...) \
    ts_log(TS_LOG_ERROR, tag, format, ##__VA_ARGS__)

/**
 * @brief 警告日志宏
 */
#define TS_LOGW(tag, format, ...) \
    ts_log(TS_LOG_WARN, tag, format, ##__VA_ARGS__)

/**
 * @brief 信息日志宏
 */
#define TS_LOGI(tag, format, ...) \
    ts_log(TS_LOG_INFO, tag, format, ##__VA_ARGS__)

/**
 * @brief 调试日志宏
 */
#define TS_LOGD(tag, format, ...) \
    ts_log(TS_LOG_DEBUG, tag, format, ##__VA_ARGS__)

/**
 * @brief 详细日志宏
 */
#define TS_LOGV(tag, format, ...) \
    ts_log(TS_LOG_VERBOSE, tag, format, ##__VA_ARGS__)

/**
 * @brief 十六进制数据日志宏
 */
#define TS_LOG_HEX(level, tag, data, len) \
    ts_log_hex(level, tag, data, len)

/* ============================================================================
 * 日志级别控制
 * ========================================================================== */

/**
 * @brief 设置全局日志级别
 *
 * @param level 日志级别
 */
void ts_log_set_level(ts_log_level_t level);

/**
 * @brief 获取当前全局日志级别
 *
 * @return 当前日志级别
 */
ts_log_level_t ts_log_get_level(void);

/**
 * @brief 设置指定标签的日志级别
 *
 * @param tag 日志标签
 * @param level 日志级别
 * @return ESP_OK 成功
 */
esp_err_t ts_log_set_tag_level(const char *tag, ts_log_level_t level);

/**
 * @brief 获取指定标签的日志级别
 *
 * @param tag 日志标签
 * @return 日志级别
 */
ts_log_level_t ts_log_get_tag_level(const char *tag);

/**
 * @brief 重置所有标签级别为全局级别
 */
void ts_log_reset_tag_levels(void);

/* ============================================================================
 * 输出控制
 * ========================================================================== */

/**
 * @brief 启用指定输出目标
 *
 * @param output 输出目标（可组合）
 */
void ts_log_enable_output(ts_log_output_t output);

/**
 * @brief 禁用指定输出目标
 *
 * @param output 输出目标（可组合）
 */
void ts_log_disable_output(ts_log_output_t output);

/**
 * @brief 获取当前启用的输出目标
 *
 * @return 输出目标掩码
 */
uint32_t ts_log_get_outputs(void);

/**
 * @brief 设置文件输出路径
 *
 * @param path 日志文件目录路径
 * @return ESP_OK 成功
 */
esp_err_t ts_log_set_file_path(const char *path);

/**
 * @brief 刷新文件输出缓冲
 *
 * @return ESP_OK 成功
 */
esp_err_t ts_log_flush(void);

/* ============================================================================
 * 日志缓冲区操作
 * ========================================================================== */

/**
 * @brief 获取缓冲区中的日志条目数量
 *
 * @return 条目数量
 */
size_t ts_log_buffer_count(void);

/**
 * @brief 获取缓冲区中的日志条目
 *
 * @param entries 输出条目数组
 * @param max_count 最大条目数
 * @param start_index 起始索引
 * @return 实际返回的条目数
 */
size_t ts_log_buffer_get(ts_log_entry_t *entries, size_t max_count, size_t start_index);

/**
 * @brief 清空日志缓冲区
 */
void ts_log_buffer_clear(void);

/* ============================================================================
 * 日志回调
 * ========================================================================== */

/**
 * @brief 注册日志回调
 *
 * @param callback 回调函数
 * @param min_level 最小日志级别
 * @param user_data 用户数据
 * @param[out] handle 输出回调句柄
 * @return ESP_OK 成功
 */
esp_err_t ts_log_add_callback(ts_log_callback_t callback,
                               ts_log_level_t min_level,
                               void *user_data,
                               ts_log_callback_handle_t *handle);

/**
 * @brief 取消注册日志回调
 *
 * @param handle 回调句柄
 * @return ESP_OK 成功
 */
esp_err_t ts_log_remove_callback(ts_log_callback_handle_t handle);

/* ============================================================================
 * 工具函数
 * ========================================================================== */

/**
 * @brief 获取日志级别名称
 *
 * @param level 日志级别
 * @return 级别名称字符串
 */
const char *ts_log_level_to_string(ts_log_level_t level);

/**
 * @brief 从字符串解析日志级别
 *
 * @param str 级别字符串
 * @return 日志级别
 */
ts_log_level_t ts_log_level_from_string(const char *str);

/**
 * @brief 获取日志级别的颜色代码
 *
 * @param level 日志级别
 * @return ANSI 颜色代码字符串
 */
const char *ts_log_level_color(ts_log_level_t level);

/* ============================================================================
 * 日志统计和高级查询 API
 * ========================================================================== */

/**
 * @brief 获取日志系统统计信息
 *
 * @param[out] stats 输出统计信息
 * @return
 *      - ESP_OK: 成功
 *      - ESP_ERR_INVALID_ARG: 参数无效
 *      - ESP_ERR_INVALID_STATE: 未初始化
 */
esp_err_t ts_log_get_stats(ts_log_stats_t *stats);

/**
 * @brief 搜索日志缓冲区（带过滤条件）
 *
 * @param entries 输出条目数组
 * @param max_count 最大返回条目数
 * @param min_level 最小日志级别（含）
 * @param max_level 最大日志级别（含）
 * @param tag_filter TAG 过滤字符串（NULL 或空字符串表示不过滤）
 * @param keyword 关键字搜索（在消息和 TAG 中搜索，NULL 表示不过滤）
 * @return 实际返回的条目数
 */
size_t ts_log_buffer_search(ts_log_entry_t *entries, size_t max_count,
                            ts_log_level_t min_level, ts_log_level_t max_level,
                            const char *tag_filter, const char *keyword);

/**
 * @brief 启用/禁用 ESP_LOG 捕获
 *
 * @param enable true 启用，false 禁用
 */
void ts_log_enable_esp_capture(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* TS_LOG_H */
