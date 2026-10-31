/**
 * @file ts_power_monitor.h
 * @brief Power Monitor Service for TianShanOS
 * 
 * 提供电源监控功能：
 * - ADC 供电电压监控（GPIO18, ADC2_CH7, 分压比 11.4:1）
 * - UART 电源芯片数据接收（GPIO47, 9600 8N1, [0xFF][V][I][CRC]）
 * - 后台任务持续监控
 * - 阈值报警和事件回调
 * 
 * 参考 robOS power_monitor 组件设计
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Version                                       */
/*===========================================================================*/

#define TS_POWER_MONITOR_VERSION "1.0.0"

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

/** 电压分压比（硬件电路: 11.4:1） */
#define TS_POWER_VOLTAGE_DIVIDER_RATIO  11.4f

/** ADC 参考电压 (mV) */
#define TS_POWER_ADC_REF_VOLTAGE_MV     3300

/** ADC 分辨率 */
#define TS_POWER_ADC_RESOLUTION_BITS    12
#define TS_POWER_ADC_MAX_VALUE          ((1 << TS_POWER_ADC_RESOLUTION_BITS) - 1)

/** 电源芯片数据包大小 */
#define TS_POWER_CHIP_PACKET_SIZE       4

/** 电源芯片帧头 */
#define TS_POWER_CHIP_HEADER            0xFF

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief 电压监控配置
 */
typedef struct {
    int gpio_pin;                   /**< ADC GPIO 引脚 (GPIO 18) */
    float divider_ratio;            /**< 分压比 (11.4) */
    uint32_t sample_interval_ms;    /**< 采样间隔 (ms) */
    float voltage_min_threshold;    /**< 最小电压阈值 (V) */
    float voltage_max_threshold;    /**< 最大电压阈值 (V) */
    bool enable_threshold_alarm;    /**< 启用阈值报警 */
} ts_power_voltage_config_t;

/**
 * @brief 电源芯片通信配置
 */
typedef struct {
    int uart_num;                   /**< UART 端口号 */
    int rx_gpio_pin;                /**< RX GPIO 引脚 (GPIO 47) */
    int baud_rate;                  /**< 波特率 (9600) */
    uint32_t timeout_ms;            /**< 超时时间 (ms) */
    bool enable_protocol_debug;     /**< 启用协议调试 */
} ts_power_chip_config_t;

/**
 * @brief 电源监控配置
 */
typedef struct {
    ts_power_voltage_config_t voltage_config;   /**< 电压监控配置 */
    ts_power_chip_config_t power_chip_config;   /**< 电源芯片配置 */
    bool auto_start_monitoring;                 /**< 初始化后自动启动监控 */
    uint32_t task_stack_size;                   /**< 监控任务栈大小 */
    int task_priority;                          /**< 监控任务优先级 */
} ts_power_monitor_config_t;

/**
 * @brief 电压监控数据
 */
typedef struct {
    float supply_voltage;           /**< 供电电压 (V) */
    int raw_adc;                    /**< ADC 原始值 */
    int voltage_mv;                 /**< 校准后电压 (mV) */
    uint32_t timestamp;             /**< 时间戳 (ms) */
} ts_power_voltage_data_t;

/**
 * @brief 电源芯片数据
 */
typedef struct {
    bool valid;                     /**< 数据有效性 */
    float voltage;                  /**< 电压 (V) */
    float current;                  /**< 电流 (A) */
    float power;                    /**< 功率 (W) */
    uint8_t raw_data[TS_POWER_CHIP_PACKET_SIZE]; /**< 原始数据包 */
    uint32_t timestamp;             /**< 时间戳 (ms) */
    bool crc_valid;                 /**< CRC 校验结果 */
} ts_power_chip_data_t;

/**
 * @brief 电源监控统计
 */
typedef struct {
    uint32_t voltage_samples;       /**< 电压采样次数 */
    uint32_t power_chip_packets;    /**< 电源芯片数据包数 */
    uint32_t crc_errors;            /**< CRC 错误数 */
    uint32_t timeout_errors;        /**< 超时错误数 */
    uint32_t threshold_violations;  /**< 阈值违规次数 */
    uint64_t uptime_ms;             /**< 运行时间 (ms) */
    float avg_voltage;              /**< 平均电压 (V) */
    float avg_current;              /**< 平均电流 (A) */
    float avg_power;                /**< 平均功率 (W) */
} ts_power_monitor_stats_t;

/**
 * @brief 电源监控事件类型
 */
typedef enum {
    TS_POWER_EVENT_VOLTAGE_THRESHOLD,   /**< 电压阈值事件 */
    TS_POWER_EVENT_POWER_DATA_RECEIVED, /**< 电源数据接收事件 */
    TS_POWER_EVENT_CRC_ERROR,           /**< CRC 错误事件 */
    TS_POWER_EVENT_TIMEOUT_ERROR,       /**< 超时错误事件 */
    TS_POWER_EVENT_MAX
} ts_power_event_type_t;

/**
 * @brief 电源监控事件回调函数
 */
typedef void (*ts_power_event_callback_t)(
    ts_power_event_type_t event_type,
    void *event_data,
    void *user_data
);

/*===========================================================================*/
/*                              API Functions                                 */
/*===========================================================================*/

/**
 * @brief 获取默认配置
 * @param config 配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_get_default_config(ts_power_monitor_config_t *config);

/**
 * @brief 初始化电源监控
 * @param config 配置参数（NULL 使用默认配置）
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_init(const ts_power_monitor_config_t *config);

/**
 * @brief 反初始化电源监控
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_deinit(void);

/**
 * @brief 启动电源监控
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_start(void);

/**
 * @brief 停止电源监控
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_stop(void);

/**
 * @brief 检查是否正在运行
 * @return true 正在运行
 */
bool ts_power_monitor_is_running(void);

/** Whether retained ADC/UART and monitor resources are initialized. */
bool ts_power_monitor_is_initialized(void);

/**
 * @brief 获取电压数据（从缓存）
 * @param data 数据结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_get_voltage_data(ts_power_voltage_data_t *data);

/**
 * @brief 立即读取电压（直接 ADC 读取）
 * @param data 数据结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_read_voltage_now(ts_power_voltage_data_t *data);

/**
 * @brief 获取电源芯片数据
 * @param data 数据结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_get_power_chip_data(ts_power_chip_data_t *data);

/**
 * @brief 获取统计信息
 * @param stats 统计结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_get_stats(ts_power_monitor_stats_t *stats);

/**
 * @brief 重置统计信息
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_reset_stats(void);

/**
 * @brief 设置电压阈值
 * @param min_voltage 最小电压 (V)
 * @param max_voltage 最大电压 (V)
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_set_voltage_thresholds(float min_voltage, float max_voltage);

/**
 * @brief 获取电压阈值
 * @param min_voltage 最小电压指针
 * @param max_voltage 最大电压指针
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_get_voltage_thresholds(float *min_voltage, float *max_voltage);

/**
 * @brief 设置采样间隔
 * @param interval_ms 采样间隔 (ms)
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_set_sample_interval(uint32_t interval_ms);

/**
 * @brief 获取采样间隔
 * @param interval_ms 采样间隔指针
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_get_sample_interval(uint32_t *interval_ms);

/**
 * @brief 注册事件回调
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_register_callback(ts_power_event_callback_t callback, void *user_data);

/**
 * @brief 注销事件回调
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_unregister_callback(void);

/**
 * @brief 设置调试模式
 * @param enable 启用/禁用
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_set_debug_mode(bool enable);

/**
 * @brief 注册控制台命令
 * @return ESP_OK 成功
 */
esp_err_t ts_power_monitor_register_commands(void);

#ifdef __cplusplus
}
#endif
