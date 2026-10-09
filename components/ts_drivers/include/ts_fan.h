/**
 * @file ts_fan.h
 * @brief Fan Control Driver
 * 
 * 支持以下功能（移植自 robOS）：
 * - PWM 速度控制
 * - 手动/自动/曲线模式
 * - 温度曲线插值
 * - 温度迟滞控制（防止频繁调速）
 * - 转速测量（可选）
 * - NVS 持久化配置
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                          Constants                                         */
/*===========================================================================*/

#define TS_FAN_MAX_CURVE_POINTS     10      /**< 最大曲线点数 */
#define TS_FAN_DEFAULT_HYSTERESIS   30      /**< 默认温度迟滞 0.1°C (3.0°C) */
#define TS_FAN_DEFAULT_MIN_INTERVAL 2000    /**< 默认最小调速间隔 ms */

/*===========================================================================*/
/*                          Type Definitions                                  */
/*===========================================================================*/

/** Fan IDs */
typedef enum {
    TS_FAN_1 = 0,
    TS_FAN_2,
    TS_FAN_3,
    TS_FAN_4,
    TS_FAN_MAX
} ts_fan_id_t;

/** Fan mode */
typedef enum {
    TS_FAN_MODE_OFF,        /**< 风扇关闭 */
    TS_FAN_MODE_MANUAL,     /**< 手动模式（固定占空比） */
    TS_FAN_MODE_AUTO,       /**< 自动模式（自适应温度控制） */
    TS_FAN_MODE_CURVE,      /**< 曲线模式（自定义温度曲线） */
} ts_fan_mode_t;

/** Adaptive auto state */
typedef enum {
    TS_FAN_AUTO_STATE_IDLE = 0,      /**< Not running adaptive control */
    TS_FAN_AUTO_STATE_BASELINE,      /**< Using curve baseline only */
    TS_FAN_AUTO_STATE_ACTIVE,        /**< Adaptive control active */
    TS_FAN_AUTO_STATE_GUARD,         /**< Hard guard active */
    TS_FAN_AUTO_STATE_STALE,         /**< Temperature input stale/invalid */
} ts_fan_auto_state_t;

/** Temperature curve point */
typedef struct {
    int16_t temp;           /**< Temperature in 0.1°C (e.g., 350 = 35.0°C) */
    uint8_t duty;           /**< Duty cycle 0-100% */
} ts_fan_curve_point_t;

/** Fan configuration */
typedef struct {
    int gpio_pwm;           /**< PWM GPIO 引脚 */
    int gpio_tach;          /**< 转速计 GPIO 引脚 (-1 = 不使用) */
    uint8_t min_duty;       /**< 最小占空比 (%) */
    uint8_t max_duty;       /**< 最大占空比 (%) */
    ts_fan_curve_point_t curve[TS_FAN_MAX_CURVE_POINTS];  /**< 温度曲线 */
    uint8_t curve_points;   /**< 曲线点数 */
    int16_t hysteresis;     /**< 温度迟滞 0.1°C */
    uint32_t min_interval;  /**< 最小调速间隔 ms */
    bool invert_pwm;        /**< PWM 反转 */
} ts_fan_config_t;

/** Fan status */
typedef struct {
    ts_fan_mode_t mode;         /**< 当前模式 */
    bool duty_valid;            /**< 已确认成功下发过 PWM */
    uint8_t duty_percent;       /**< 当前占空比 */
    uint8_t target_duty;        /**< 目标占空比（曲线计算） */
    uint16_t rpm;               /**< 当前转速 */
    int16_t temp;               /**< 当前温度源 0.1°C */
    int16_t last_stable_temp;   /**< 上次稳定温度 0.1°C */
    bool is_running;            /**< 已确认 PWM 输出非零；不表示机械转速测量 */
    bool enabled;               /**< 是否启用 */
    bool fault;                 /**< 故障标志 */
    int16_t control_temp;        /**< AUTO 控制温度 0.1°C */
    int16_t guard_temp;          /**< AUTO 保护温度 0.1°C */
    int16_t predicted_temp;      /**< AUTO 预测温度 0.1°C */
    float slope_c_per_min;       /**< AUTO 温度斜率 °C/min */
    float controller_gain;       /**< AUTO 自适应增益 */
    float cooling_response;      /**< AUTO 最近散热响应 °C/min */
    ts_fan_auto_state_t auto_state; /**< AUTO 控制状态 */
    bool guard_active;           /**< AUTO 硬保护是否激活 */
    bool temp_stale;             /**< AUTO 温度输入是否失效/过期 */
} ts_fan_status_t;

/*===========================================================================*/
/*                          Initialization                                    */
/*===========================================================================*/

/**
 * @brief Initialize fan subsystem
 */
esp_err_t ts_fan_init(void);

/**
 * @brief Deinitialize fan subsystem
 */
esp_err_t ts_fan_deinit(void);

/**
 * @brief Check if fan subsystem is initialized
 */
bool ts_fan_is_initialized(void);

/*===========================================================================*/
/*                          Configuration                                     */
/*===========================================================================*/

/**
 * @brief Configure a fan
 */
esp_err_t ts_fan_configure(ts_fan_id_t fan, const ts_fan_config_t *config);

/**
 * @brief Get current fan configuration
 * @param fan Fan ID
 * @param config Output configuration structure
 * @return ESP_OK on success
 */
esp_err_t ts_fan_get_config(ts_fan_id_t fan, ts_fan_config_t *config);

/**
 * @brief Get default configuration
 */
void ts_fan_get_default_config(ts_fan_config_t *config);

/*===========================================================================*/
/*                          Control                                           */
/*===========================================================================*/

/**
 * @brief Set fan mode
 */
esp_err_t ts_fan_set_mode(ts_fan_id_t fan, ts_fan_mode_t mode);

/**
 * @brief Get fan mode
 */
esp_err_t ts_fan_get_mode(ts_fan_id_t fan, ts_fan_mode_t *mode);

/**
 * @brief Set fan duty cycle (switches to manual mode)
 */
esp_err_t ts_fan_set_duty(ts_fan_id_t fan, uint8_t duty_percent);

/**
 * @brief Enable or disable a fan
 */
esp_err_t ts_fan_enable(ts_fan_id_t fan, bool enable);

/**
 * @brief Check if fan is enabled
 */
esp_err_t ts_fan_is_enabled(ts_fan_id_t fan, bool *enabled);

/*===========================================================================*/
/*                          Temperature Control                               */
/*===========================================================================*/

/**
 * @brief Set temperature for auto/curve mode
 */
esp_err_t ts_fan_set_temperature(ts_fan_id_t fan, int16_t temp_01c);

/**
 * @brief Update temperature curve
 * @param fan Fan ID
 * @param curve Array of curve points (will be sorted by temperature)
 * @param points Number of points (max TS_FAN_MAX_CURVE_POINTS)
 */
esp_err_t ts_fan_set_curve(ts_fan_id_t fan, const ts_fan_curve_point_t *curve, uint8_t points);

/**
 * @brief Configure temperature hysteresis and rate limiting
 * @param fan Fan ID
 * @param hysteresis Temperature dead zone in 0.1°C (0 to disable)
 * @param min_interval Minimum interval between speed changes in ms
 */
esp_err_t ts_fan_set_hysteresis(ts_fan_id_t fan, int16_t hysteresis, uint32_t min_interval);

/**
 * @brief Set duty cycle limits for fan
 * @param fan Fan ID
 * @param min_duty Minimum duty cycle (0-100%)
 * @param max_duty Maximum duty cycle (0-100%)
 * @return ESP_OK on success
 */
esp_err_t ts_fan_set_limits(ts_fan_id_t fan, uint8_t min_duty, uint8_t max_duty);

/*===========================================================================*/
/*                          Status                                            */
/*===========================================================================*/

/**
 * @brief Get fan status
 */
esp_err_t ts_fan_get_status(ts_fan_id_t fan, ts_fan_status_t *status);

/**
 * @brief Get all fans status
 */
esp_err_t ts_fan_get_all_status(ts_fan_status_t *status_array, uint8_t array_size);

/*===========================================================================*/
/*                          Emergency                                         */
/*===========================================================================*/

/**
 * @brief Set all fans to maximum speed (emergency)
 */
esp_err_t ts_fan_emergency_full(void);

/*===========================================================================*/
/*                          Temperature Source Integration                    */
/*===========================================================================*/

/**
 * @brief Enable/disable automatic temperature subscription
 * 
 * 当启用时，风扇控制器自动订阅 ts_temp_source 的温度事件，
 * 无需手动调用 ts_fan_set_temperature()
 * 
 * @param enable true 启用自动订阅，false 禁用
 * @return ESP_OK 成功
 */
esp_err_t ts_fan_set_auto_temp_enabled(bool enable);

/**
 * @brief Check if automatic temperature subscription is enabled
 * 
 * @return true 已启用
 */
bool ts_fan_is_auto_temp_enabled(void);

/*===========================================================================*/
/*===========================================================================*/

/**
 * @brief Save fan configuration to NVS
 * @param fan Fan ID (-1 for all fans)
 */
esp_err_t ts_fan_save_config(void);

/**
 * @brief Save complete configuration including runtime state
 * @param fan Fan ID
 */
esp_err_t ts_fan_save_full_config(ts_fan_id_t fan);

/**
 * @brief Load fan configuration from NVS
 */
esp_err_t ts_fan_load_config(void);

#ifdef __cplusplus
}
#endif
