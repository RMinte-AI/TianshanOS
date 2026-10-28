/**
 * @file ts_power_policy.h
 * @brief Power Policy Engine - Voltage Protection and Auto Recovery
 * 
 * 电压保护策略引擎，移植自 robOS voltage_protection 组件
 * 
 * 功能：
 * - 低电压检测和保护
 * - 自动关机和恢复
 * - 设备状态联动控制
 * - LED 状态反馈
 * 
 * 状态机：
 *   NORMAL → LOW_VOLTAGE → SHUTDOWN → PROTECTED → RECOVERY → (esp_restart)
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-19
 */

#ifndef TS_POWER_POLICY_H
#define TS_POWER_POLICY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                          Version and Constants                             */
/*===========================================================================*/

#define TS_POWER_POLICY_VERSION "1.0.0"

/** 默认低电压阈值 (V) - 低于此值开始倒计时 */
#define TS_POWER_POLICY_LOW_VOLTAGE_DEFAULT     12.6f

/** 默认恢复电压阈值 (V) - 高于此值允许恢复 */
#define TS_POWER_POLICY_RECOVERY_VOLTAGE_DEFAULT 18.0f

/** 默认关机倒计时 (秒) */
#define TS_POWER_POLICY_SHUTDOWN_DELAY_DEFAULT  60

/** 默认恢复稳定等待时间 (秒) */
#define TS_POWER_POLICY_RECOVERY_HOLD_DEFAULT   5

/** 默认风扇停止延迟 (秒) */
#define TS_POWER_POLICY_FAN_STOP_DELAY_DEFAULT  60

/** 最小有效电压读数 (V) - 低于此值视为无效 */
#define TS_POWER_POLICY_MIN_VALID_VOLTAGE       5.0f

/*===========================================================================*/
/*                          Type Definitions                                  */
/*===========================================================================*/

/**
 * @brief 电压保护状态
 */
typedef enum {
    TS_POWER_POLICY_STATE_NORMAL = 0,       /**< 正常运行 */
    TS_POWER_POLICY_STATE_LOW_VOLTAGE,      /**< 低电压保护（倒计时中）*/
    TS_POWER_POLICY_STATE_SHUTDOWN,         /**< 正在执行关机 */
    TS_POWER_POLICY_STATE_PROTECTED,        /**< 保护状态（等待电压恢复）*/
    TS_POWER_POLICY_STATE_RECOVERY,         /**< 电压恢复中（稳定等待）*/
    TS_POWER_POLICY_STATE_MAX
} ts_power_policy_state_t;

/**
 * @brief 设备状态信息
 */
typedef struct {
    bool agx_powered;               /**< AGX 电源状态 */
    bool lpmu_powered;              /**< LPMU 电源状态 */
    bool agx_connected;             /**< AGX WebSocket 连接状态 */
    bool lpmu_connected;            /**< LPMU 连接状态 */
    uint32_t agx_disconnect_sec;    /**< AGX 断开时长 (秒) */
    uint32_t lpmu_disconnect_sec;   /**< LPMU 断开时长 (秒) */
} ts_power_policy_device_status_t;

/**
 * @brief 电压保护配置
 */
typedef struct {
    float low_voltage_threshold;        /**< 低电压阈值 (V) */
    float recovery_voltage_threshold;   /**< 恢复电压阈值 (V) */
    uint32_t shutdown_delay_sec;        /**< 关机倒计时 (秒) */
    uint32_t recovery_hold_sec;         /**< 恢复稳定等待 (秒) */
    uint32_t fan_stop_delay_sec;        /**< 设备关机后风扇停止延迟 (秒) */
    bool auto_recovery_enabled;         /**< 是否启用自动恢复 */
    bool enable_led_feedback;           /**< 是否启用 LED 反馈 */
    bool enable_device_shutdown;        /**< 是否执行设备关机 */
    bool enable_fan_control;            /**< 是否在保护时控制风扇 */
    bool lpmu_ping_before_shutdown;     /**< 关机前 ping 检测 LPMU */
} ts_power_policy_config_t;

/**
 * @brief 电压保护状态信息
 */
typedef struct {
    bool initialized;                   /**< 是否已初始化 */
    bool running;                       /**< 是否正在运行 */
    ts_power_policy_state_t state;      /**< 当前状态 */
    float current_voltage;              /**< 当前电压 (V) */
    uint32_t countdown_remaining_sec;   /**< 剩余倒计时 (秒) */
    uint32_t recovery_timer_sec;        /**< 恢复计时器 (秒) */
    uint32_t protection_count;          /**< 保护触发次数 */
    uint64_t uptime_ms;                 /**< 运行时间 (毫秒) */
    ts_power_policy_device_status_t device_status; /**< 设备状态 */
} ts_power_policy_status_t;

/**
 * @brief 电压保护事件类型
 */
typedef enum {
    TS_POWER_POLICY_EVENT_STATE_CHANGED,    /**< 状态变化 */
    TS_POWER_POLICY_EVENT_LOW_VOLTAGE,      /**< 进入低电压状态 */
    TS_POWER_POLICY_EVENT_SHUTDOWN_START,   /**< 开始关机 */
    TS_POWER_POLICY_EVENT_PROTECTED,        /**< 进入保护状态 */
    TS_POWER_POLICY_EVENT_RECOVERY_START,   /**< 开始恢复 */
    TS_POWER_POLICY_EVENT_RECOVERY_COMPLETE,/**< 恢复完成（即将重启）*/
    TS_POWER_POLICY_EVENT_COUNTDOWN_TICK,   /**< 倒计时每秒 */
    TS_POWER_POLICY_EVENT_DEBUG_TICK,       /**< 调试模式每秒更新 */
    TS_POWER_POLICY_EVENT_MAX
} ts_power_policy_event_t;

/**
 * @brief 事件回调函数类型
 */
typedef void (*ts_power_policy_callback_t)(
    ts_power_policy_event_t event,
    const ts_power_policy_status_t *status,
    void *user_data
);

/*===========================================================================*/
/*                          API Functions                                     */
/*===========================================================================*/

/**
 * @brief 获取默认配置
 * @param config 配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_get_default_config(ts_power_policy_config_t *config);

/** Get the current effective configuration, including unsaved changes. */
esp_err_t ts_power_policy_get_config(ts_power_policy_config_t *config);


/**
 * @brief 初始化电压保护策略
 * @param config 配置参数（NULL 使用默认配置）
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_init(const ts_power_policy_config_t *config);

/**
 * @brief 反初始化电压保护策略
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_deinit(void);

/**
 * @brief 启动电压保护监控
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_start(void);

/**
 * @brief 停止电压保护监控
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_stop(void);

/**
 * @brief 获取当前状态
 * @param status 状态结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_get_status(ts_power_policy_status_t *status);

/**
 * @brief 检查是否已初始化
 * @return true 已初始化
 */
bool ts_power_policy_is_initialized(void);

/**
 * @brief 检查是否正在运行
 * @return true 正在运行
 */
bool ts_power_policy_is_running(void);

/**
 * @brief 获取保存的启用状态（用于服务启动时决定是否自动启动保护）
 * @return true 应该启用保护
 */
bool ts_power_policy_should_auto_start(void);

/**
 * @brief 获取当前状态枚举
 * @return 当前状态
 */
ts_power_policy_state_t ts_power_policy_get_state(void);

/**
 * @brief 获取状态名称
 * @param state 状态枚举
 * @return 状态名称字符串
 */
const char *ts_power_policy_get_state_name(ts_power_policy_state_t state);

/**
 * @brief 触发测试模式（模拟低电压）
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_trigger_test(void);

/**
 * @brief 重置保护状态（会重启 ESP32）
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_reset(void);

/**
 * @brief 设置电压阈值
 * @param low_threshold 低电压阈值 (V)
 * @param recovery_threshold 恢复电压阈值 (V)
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_set_thresholds(float low_threshold, float recovery_threshold);

/**
 * @brief 获取电压阈值
 * @param low_threshold 输出低电压阈值 (V)
 * @param recovery_threshold 输出恢复电压阈值 (V)
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_get_thresholds(float *low_threshold, float *recovery_threshold);

/**
 * @brief 设置关机倒计时
 * @param delay_sec 倒计时秒数
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_set_shutdown_delay(uint32_t delay_sec);

/**
 * @brief 注册事件回调
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_register_callback(ts_power_policy_callback_t callback, void *user_data);

/**
 * @brief 启用/禁用调试模式
 * @param enable true 启用，false 禁用
 * @param duration_sec 调试模式持续时间（秒），0 表示永久
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_set_debug_mode(bool enable, uint32_t duration_sec);

/**
 * @brief 检查调试模式是否启用
 * @return true 已启用
 */
bool ts_power_policy_is_debug_mode(void);

/**
 * @brief 设置关机倒计时时间
 * @param delay_sec 倒计时秒数（10-600秒）
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_set_shutdown_delay(uint32_t delay_sec);

/**
 * @brief 设置恢复等待时间
 * @param hold_sec 等待秒数（1-300秒）
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_set_recovery_hold(uint32_t hold_sec);

/**
 * @brief 设置风扇停止延迟
 * @param delay_sec 延迟秒数（10-600秒）
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_set_fan_stop_delay(uint32_t delay_sec);

/**
 * @brief 保存当前配置到系统 NVS
 * 
 * 将当前配置导出为 JSON 文件保存到 /sdcard/config/power_policy.json
 * 
 * @return ESP_OK 成功，否则为实际保存错误
 */
esp_err_t ts_power_policy_save_config(void);

/**
 * @brief 注册 automation 变量
 * 
 * 在 ts_variable 系统初始化后调用此函数注册 power policy 的监控变量
 * 通常由 automation 服务在初始化完成后调用
 * 
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_register_variables(void);

/**
 * @brief 更新自动化变量（供数据源使用）
 * 自动更新以下变量：
 *   - power_policy.state (string)
 *   - power_policy.voltage (float)
 *   - power_policy.countdown (int)
 *   - power_policy.recovery_timer (int)
 *   - power_policy.protection_count (int)
 */
esp_err_t ts_power_policy_update_variables(void);

/**
 * @brief 注册控制台命令
 * @return ESP_OK 成功
 */
esp_err_t ts_power_policy_register_commands(void);

#ifdef __cplusplus
}
#endif

#endif /* TS_POWER_POLICY_H */
