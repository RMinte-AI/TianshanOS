/**
 * @file ts_event.h
 * @brief TianShanOS Event Bus System
 *
 * 事件总线系统主头文件
 * 提供发布/订阅模式的事件通信机制
 *
 * 特性：
 * - 发布/订阅模式
 * - 同步和异步事件投递
 * - 事件过滤和优先级
 * - 事务支持
 * - 事件统计
 *
 * @author TianShanOS Team
 * @version 0.1.0
 */

#ifndef TS_EVENT_H
#define TS_EVENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 常量定义
 * ========================================================================== */

#ifndef CONFIG_TS_EVENT_QUEUE_SIZE
#define CONFIG_TS_EVENT_QUEUE_SIZE 32
#endif

#ifndef CONFIG_TS_EVENT_MAX_HANDLERS
#define CONFIG_TS_EVENT_MAX_HANDLERS 64
#endif

#ifndef CONFIG_TS_EVENT_DATA_MAX_SIZE
#define CONFIG_TS_EVENT_DATA_MAX_SIZE 256
#endif

/** 事件队列大小 */
#define TS_EVENT_QUEUE_SIZE CONFIG_TS_EVENT_QUEUE_SIZE

/** 最大处理器数量 */
#define TS_EVENT_HANDLERS_MAX CONFIG_TS_EVENT_MAX_HANDLERS

/** 事件数据最大大小 */
#define TS_EVENT_DATA_MAX_SIZE CONFIG_TS_EVENT_DATA_MAX_SIZE

/** 任意事件基础 */
#define TS_EVENT_ANY_BASE ((ts_event_base_t)"*")

/** 任意事件 ID */
#define TS_EVENT_ANY_ID (-1)

/* ============================================================================
 * 类型定义
 * ========================================================================== */

/**
 * @brief 事件基础类型（字符串标识符）
 */
typedef const char *ts_event_base_t;

/**
 * @brief 事件 ID 类型
 */
typedef int32_t ts_event_id_t;

/**
 * @brief 事件优先级
 */
typedef enum {
    TS_EVENT_PRIORITY_LOW = 0,      /**< 低优先级 */
    TS_EVENT_PRIORITY_NORMAL = 1,   /**< 普通优先级 */
    TS_EVENT_PRIORITY_HIGH = 2,     /**< 高优先级 */
    TS_EVENT_PRIORITY_CRITICAL = 3, /**< 关键优先级 */
} ts_event_priority_t;

/**
 * @brief 事件数据结构
 */
typedef struct {
    ts_event_base_t base;           /**< 事件基础 */
    ts_event_id_t id;               /**< 事件 ID */
    void *data;                     /**< 事件数据 */
    size_t data_size;               /**< 数据大小 */
    ts_event_priority_t priority;   /**< 优先级 */
    uint32_t timestamp_ms;          /**< 时间戳 */
    void *source;                   /**< 事件来源（可选）*/
} ts_event_t;

/**
 * @brief 事件处理器回调函数类型
 *
 * @param event 事件数据
 * @param user_data 用户数据
 */
typedef void (*ts_event_handler_t)(const ts_event_t *event, void *user_data);

/**
 * @brief 事件处理器句柄
 */
typedef struct ts_event_handler_instance *ts_event_handler_handle_t;

/**
 * @brief 事件事务句柄
 */
typedef struct ts_event_transaction *ts_event_transaction_t;

/**
 * @brief 事件统计信息
 */
typedef struct {
    uint32_t events_posted;         /**< 已发布事件数 */
    uint32_t events_delivered;      /**< 已投递事件数 */
    uint32_t events_dropped;        /**< 已丢弃事件数 */
    uint32_t handlers_registered;   /**< 已注册处理器数 */
    uint32_t queue_high_watermark;  /**< 队列高水位 */
    uint32_t max_delivery_time_us;  /**< 最大投递时间（微秒）*/
    uint32_t avg_delivery_time_us;  /**< 平均投递时间（微秒）*/
} ts_event_stats_t;

/* ============================================================================
 * 预定义事件基础
 * ========================================================================== */

/** 系统事件 */
#define TS_EVENT_BASE_SYSTEM    "ts_system"

/** 配置事件 */
#define TS_EVENT_BASE_CONFIG    "ts_config"

/** 服务事件 */
#define TS_EVENT_BASE_SERVICE   "ts_service"

/** 网络事件 */
#define TS_EVENT_BASE_NETWORK   "ts_network"

/** LED 事件 */
#define TS_EVENT_BASE_LED       "ts_led"

/** 电源事件 */
#define TS_EVENT_BASE_POWER     "ts_power"

/** 存储事件 */
#define TS_EVENT_BASE_STORAGE   "ts_storage"

/** 温度事件 */
#define TS_EVENT_BASE_TEMP      "ts_temp"

/** 设备监控事件 */
#define TS_EVENT_BASE_DEVICE_MON "ts_device_mon"

/** 用户事件 */
#define TS_EVENT_BASE_USER      "ts_user"

/** OTA 事件 */
#define TS_EVENT_BASE_OTA       "ts_ota"

/** 时间事件 */
#define TS_EVENT_BASE_PKI       "ts_pki"
#define TS_EVENT_PKI_MATERIAL_CHANGED 0x0001

#define TS_EVENT_BASE_TIME      "ts_time"

/* ============================================================================
 * 时间事件 ID
 * ========================================================================== */

/** 时间同步完成 */
#define TS_EVENT_TIME_SYNCED    0x0001
/** 时间同步失败 */
#define TS_EVENT_TIME_SYNC_FAILED 0x0002

/* ============================================================================
 * 系统事件 ID
 * ========================================================================== */

/** 系统启动完成 */
#define TS_EVENT_SYSTEM_STARTED     0x0001
/** 系统关闭中 */
#define TS_EVENT_SYSTEM_SHUTDOWN    0x0002
/** 系统错误 */
#define TS_EVENT_SYSTEM_ERROR       0x0003
/** 系统警告 */
#define TS_EVENT_SYSTEM_WARNING     0x0004
/** 低内存警告 */
#define TS_EVENT_SYSTEM_LOW_MEMORY  0x0005
/** 系统信息变更 (WebSocket) */
#define TS_EVENT_SYSTEM_INFO_CHANGED 0x0006

/* ============================================================================
 * 网络事件 ID
 * ========================================================================== */

/** 以太网连接 */
#define TS_EVT_ETH_CONNECTED        0x0101
/** 以太网断开 */
#define TS_EVT_ETH_DISCONNECTED     0x0102
/** WiFi 连接 */
#define TS_EVT_WIFI_CONNECTED       0x0103
/** WiFi 断开 */
#define TS_EVT_WIFI_DISCONNECTED    0x0104
/** 获取到 IP 地址 */
#define TS_EVT_GOT_IP               0x0105
/** IP 地址丢失 */
#define TS_EVT_LOST_IP              0x0106
/** DHCP 服务器给客户端分配了 IP（用于监控连接的设备）*/
#define TS_EVT_DHCP_CLIENT_CONNECTED    0x0107

/* ============================================================================
 * 存储事件 ID
 * ========================================================================== */

/** SPIFFS 文件系统已挂载 */
#define TS_EVT_STORAGE_SPIFFS_MOUNTED       0x0201
/** SPIFFS 文件系统已卸载 */
#define TS_EVT_STORAGE_SPIFFS_UNMOUNTED     0x0202
/** SD 卡已挂载 */
#define TS_EVT_STORAGE_SD_MOUNTED           0x0203
/** SD 卡已卸载 */
#define TS_EVT_STORAGE_SD_UNMOUNTED         0x0204

/* ============================================================================
 * 电源策略事件 ID
 * ========================================================================== */

/** 电压读取更新 */
#define TS_EVT_POWER_VOLTAGE_UPDATE         0x0301
/** 低电压警告触发 */
#define TS_EVT_POWER_LOW_VOLTAGE            0x0302
/** 状态变更 */
#define TS_EVT_POWER_STATE_CHANGED          0x0303
/** 倒计时更新 */
#define TS_EVT_POWER_COUNTDOWN_TICK         0x0304
/** 开始关机 */
#define TS_EVT_POWER_SHUTDOWN_START         0x0305
/** 进入保护状态 */
#define TS_EVT_POWER_PROTECTED              0x0306
/** 开始恢复 */
#define TS_EVT_POWER_RECOVERY_START         0x0307
/** 恢复完成 */
#define TS_EVT_POWER_RECOVERY_COMPLETE      0x0308

/* ============================================================================
 * 温度事件 ID
 * ========================================================================== */

/** 温度值更新 */
#define TS_EVT_TEMP_UPDATED                 0x0401
/** 温度源切换 */
#define TS_EVT_TEMP_SOURCE_CHANGED          0x0402
/** 温度超过阈值 */
#define TS_EVT_TEMP_THRESHOLD_EXCEEDED      0x0403

/* ============================================================================
 * 设备监控事件 ID
 * ========================================================================== */

/** AGX 连接建立 */
#define TS_EVT_AGX_CONNECTED                0x0501
/** AGX 连接断开 */
#define TS_EVT_AGX_DISCONNECTED             0x0502
/** AGX 数据更新 */
#define TS_EVT_AGX_DATA_UPDATED             0x0503
/** AGX 错误 */
#define TS_EVT_AGX_ERROR                    0x0504
/** 设备状态变更 (WebSocket) */
#define TS_EVENT_DEVICE_STATUS_CHANGED      0x0505

/* ============================================================================
 * OTA 事件 ID
 * ========================================================================== */

/** OTA 进度更新 */
#define TS_EVENT_OTA_PROGRESS_UPDATE        0x0601
/** OTA 开始 */
#define TS_EVENT_OTA_STARTED                0x0602
/** OTA 完成 */
#define TS_EVENT_OTA_FINISHED               0x0603
/** OTA 失败 */
#define TS_EVENT_OTA_FAILED                 0x0604

/* ============================================================================
 * 事件基础别名（兼容旧代码）
 * ========================================================================== */

#define TS_EVENT_NETWORK    TS_EVENT_BASE_NETWORK
#define TS_EVENT_SYSTEM     TS_EVENT_BASE_SYSTEM
#define TS_EVENT_LED        TS_EVENT_BASE_LED
#define TS_EVENT_POWER      TS_EVENT_BASE_POWER

/* ============================================================================
 * 初始化和反初始化
 * ========================================================================== */

/**
 * @brief 初始化事件系统
 *
 * @return
 *      - ESP_OK: 成功
 *      - ESP_ERR_INVALID_STATE: 已经初始化
 *      - ESP_ERR_NO_MEM: 内存不足
 */
esp_err_t ts_event_init(void);

/**
 * @brief 反初始化事件系统
 *
 * @return ESP_OK 成功
 */
esp_err_t ts_event_deinit(void);

/**
 * @brief 检查事件系统是否已初始化
 *
 * @return true 已初始化
 */
bool ts_event_is_initialized(void);

/* ============================================================================
 * 事件注册
 * ========================================================================== */

/**
 * @brief 注册事件处理器
 *
 * @param event_base 事件基础（使用 TS_EVENT_ANY_BASE 订阅所有）
 * @param event_id 事件 ID（使用 TS_EVENT_ANY_ID 订阅所有该基础的事件）
 * @param handler 处理器回调函数
 * @param user_data 用户数据
 * @param[out] handle 输出处理器句柄
 * @return ESP_OK 成功
 */
esp_err_t ts_event_register(ts_event_base_t event_base,
                             ts_event_id_t event_id,
                             ts_event_handler_t handler,
                             void *user_data,
                             ts_event_handler_handle_t *handle);

/**
 * @brief 注册事件处理器（带优先级过滤）
 *
 * @param event_base 事件基础
 * @param event_id 事件 ID
 * @param min_priority 最小优先级
 * @param handler 处理器回调函数
 * @param user_data 用户数据
 * @param[out] handle 输出处理器句柄
 * @return ESP_OK 成功
 */
esp_err_t ts_event_register_with_priority(ts_event_base_t event_base,
                                           ts_event_id_t event_id,
                                           ts_event_priority_t min_priority,
                                           ts_event_handler_t handler,
                                           void *user_data,
                                           ts_event_handler_handle_t *handle);

/**
 * @brief 取消注册事件处理器
 *
 * @param handle 处理器句柄
 * @return ESP_OK 成功
 */
esp_err_t ts_event_unregister(ts_event_handler_handle_t handle);

/** Stop new invocations and drain existing callbacks. Timeout retains the handle
 * for retry; only ESP_OK / NOT_FOUND permits freeing user_data. Never call from
 * an event callback (including post_sync); use nonblocking unregister there. */
esp_err_t ts_event_unregister_sync(ts_event_handler_handle_t handle, uint32_t timeout_ms);
bool ts_event_in_callback(void);

/**
 * @brief 取消注册指定事件的所有处理器
 *
 * @param event_base 事件基础
 * @param event_id 事件 ID
 * @return ESP_OK 成功
 */
esp_err_t ts_event_unregister_all(ts_event_base_t event_base, ts_event_id_t event_id);

/* ============================================================================
 * 事件发布
 * ========================================================================== */

/**
 * @brief 异步发布事件
 *
 * 事件将被放入队列，由事件循环异步处理
 *
 * @param event_base 事件基础
 * @param event_id 事件 ID
 * @param data 事件数据（将被复制）
 * @param data_size 数据大小
 * @param timeout_ms 入队超时（毫秒）
 * @return ESP_OK 成功，ESP_ERR_TIMEOUT 超时
 */
esp_err_t ts_event_post(ts_event_base_t event_base,
                         ts_event_id_t event_id,
                         const void *data,
                         size_t data_size,
                         uint32_t timeout_ms);

/**
 * @brief 异步发布事件（带优先级）
 *
 * @param event_base 事件基础
 * @param event_id 事件 ID
 * @param data 事件数据
 * @param data_size 数据大小
 * @param priority 事件优先级
 * @param timeout_ms 入队超时
 * @return ESP_OK 成功
 */
esp_err_t ts_event_post_with_priority(ts_event_base_t event_base,
                                       ts_event_id_t event_id,
                                       const void *data,
                                       size_t data_size,
                                       ts_event_priority_t priority,
                                       uint32_t timeout_ms);

/**
 * @brief 同步发布事件
 *
 * 立即调用所有已注册的处理器，在当前任务上下文中执行
 *
 * @param event_base 事件基础
 * @param event_id 事件 ID
 * @param data 事件数据
 * @param data_size 数据大小
 * @return ESP_OK 成功
 */
esp_err_t ts_event_post_sync(ts_event_base_t event_base,
                              ts_event_id_t event_id,
                              const void *data,
                              size_t data_size);

/**
 * @brief 从 ISR 发布事件
 *
 * @param event_base 事件基础
 * @param event_id 事件 ID
 * @param data 事件数据
 * @param data_size 数据大小
 * @param[out] higher_priority_task_woken 是否唤醒更高优先级任务
 * @return ESP_OK 成功
 */
esp_err_t ts_event_post_from_isr(ts_event_base_t event_base,
                                  ts_event_id_t event_id,
                                  const void *data,
                                  size_t data_size,
                                  BaseType_t *higher_priority_task_woken);

/* ============================================================================
 * 事务支持
 * ========================================================================== */

/**
 * @brief 开始事件事务
 *
 * 事务内的事件将被暂存，直到提交或回滚
 *
 * @param[out] transaction 输出事务句柄
 * @return ESP_OK 成功
 */
esp_err_t ts_event_transaction_begin(ts_event_transaction_t *transaction);

/**
 * @brief 在事务中发布事件
 *
 * @param transaction 事务句柄
 * @param event_base 事件基础
 * @param event_id 事件 ID
 * @param data 事件数据
 * @param data_size 数据大小
 * @return ESP_OK 成功
 */
esp_err_t ts_event_transaction_post(ts_event_transaction_t transaction,
                                     ts_event_base_t event_base,
                                     ts_event_id_t event_id,
                                     const void *data,
                                     size_t data_size);

/**
 * @brief 提交事务
 *
 * 将事务中的所有事件发布到事件队列
 *
 * @param transaction 事务句柄
 * @return ESP_OK 成功
 */
esp_err_t ts_event_transaction_commit(ts_event_transaction_t transaction);

/**
 * @brief 回滚事务
 *
 * 丢弃事务中的所有事件
 *
 * @param transaction 事务句柄
 * @return ESP_OK 成功
 */
esp_err_t ts_event_transaction_rollback(ts_event_transaction_t transaction);

/* ============================================================================
 * 统计和调试
 * ========================================================================== */

/**
 * @brief 获取事件统计信息
 *
 * @param[out] stats 输出统计信息
 * @return ESP_OK 成功
 */
esp_err_t ts_event_get_stats(ts_event_stats_t *stats);

/**
 * @brief 重置事件统计信息
 */
void ts_event_reset_stats(void);

/**
 * @brief 打印事件统计信息
 */
void ts_event_dump_stats(void);

/**
 * @brief 获取当前队列中的事件数量
 *
 * @return 队列中的事件数量
 */
size_t ts_event_get_queue_count(void);

/* ============================================================================
 * 便捷宏
 * ========================================================================== */

/**
 * @brief 声明事件基础
 */
#define TS_EVENT_DECLARE_BASE(name) \
    extern ts_event_base_t name

/**
 * @brief 定义事件基础
 */
#define TS_EVENT_DEFINE_BASE(name, value) \
    ts_event_base_t name = value

/**
 * @brief 快速发布事件（无数据）
 */
#define TS_EVENT_POST(base, id) \
    ts_event_post(base, id, NULL, 0, 100)

/**
 * @brief 快速发布事件（带数据）
 */
#define TS_EVENT_POST_DATA(base, id, data_ptr) \
    ts_event_post(base, id, data_ptr, sizeof(*(data_ptr)), 100)

/**
 * @brief 快速同步发布事件
 */
#define TS_EVENT_POST_SYNC(base, id) \
    ts_event_post_sync(base, id, NULL, 0)

#ifdef __cplusplus
}
#endif

#endif /* TS_EVENT_H */
