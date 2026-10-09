/**
 * @file ts_temp_source.h
 * @brief Temperature Source Management for TianShanOS
 * 
 * 温度源管理服务 - 统一温度数据入口
 * 
 * 架构设计：
 * - 支持多个温度 provider（AGX/本地传感器/手动设置）
 * - 基于优先级的温度源选择
 * - 通过事件总线发布温度更新（TS_EVENT_BASE_TEMP）
 * - 风扇等订阅者通过事件获取温度
 * 
 * 数据流：
 *   Provider (AGX/Sensor/Manual) 
 *       → ts_temp_source_update()
 *       → 优先级选择
 *       → ts_event_post(TS_EVT_TEMP_UPDATED)
 *       → 订阅者（风扇曲线控制）
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_TEMP_SOURCE_H
#define TS_TEMP_SOURCE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Version                                       */
/*===========================================================================*/

#define TS_TEMP_SOURCE_VERSION "1.0.0"

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

/** 最大 provider 数量 */
#define TS_TEMP_MAX_PROVIDERS       8

/** 默认温度（0.1°C 单位，即 250 = 25.0°C）*/
#define TS_TEMP_DEFAULT_VALUE       250

/** 温度有效范围 */
#define TS_TEMP_MIN_VALID           (-500)   // -50.0°C
#define TS_TEMP_MAX_VALID           (1500)   // 150.0°C

/** 数据超时（毫秒）- 超过此时间认为数据过期 */
#define TS_TEMP_DATA_TIMEOUT_MS     10000

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief 温度源类型（按优先级排序）
 */
typedef enum {
    TS_TEMP_SOURCE_DEFAULT = 0,     /**< 默认值（25°C）- 最低优先级 */
    TS_TEMP_SOURCE_SENSOR_LOCAL,    /**< 本地温度传感器 */
    TS_TEMP_SOURCE_AGX_AUTO,        /**< AGX WebSocket 自动获取 */
    TS_TEMP_SOURCE_VARIABLE,        /**< 绑定系统变量 */
    TS_TEMP_SOURCE_MANUAL,          /**< 手动设置 - 最高优先级 */
    TS_TEMP_SOURCE_MAX
} ts_temp_source_type_t;

/**
 * @brief 温度数据结构
 */
typedef struct {
    int16_t value;                  /**< 温度值（0.1°C 单位）*/
    ts_temp_source_type_t source;   /**< 数据来源 */
    int64_t timestamp_ms;           /**< 更新时间戳 */
    uint32_t identity_revision;     /**< 源/绑定配置身份；普通温度更新不改变 */
    bool valid;                     /**< 数据有效性 */
    int16_t guard_value;            /**< 保护温度（0.1°C 单位），绑定变量最高 fresh 温度 */
    bool guard_valid;               /**< 保护温度是否有效 */
    bool partial_stale;             /**< 绑定变量是否只有部分有效 */
    uint8_t bound_valid_count;      /**< 有效正权重绑定数量 */
    uint8_t bound_total_count;      /**< 正权重绑定数量 */
    float bound_valid_weight;       /**< 有效权重和 */
    float bound_total_weight;       /**< 正权重和 */
} ts_temp_data_t;

/**
 * @brief 温度更新事件数据（通过 TS_EVT_TEMP_UPDATED 发布）
 */
typedef struct {
    int16_t temp;                   /**< 当前有效温度（0.1°C）*/
    ts_temp_source_type_t source;   /**< 当前温度源 */
    int16_t prev_temp;              /**< 前一次温度 */
    ts_temp_source_type_t prev_source;  /**< 前一次温度源 */
} ts_temp_event_data_t;

/**
 * @brief Provider 信息结构
 */
typedef struct {
    ts_temp_source_type_t type;     /**< Provider 类型 */
    const char *name;               /**< Provider 名称 */
    int16_t last_value;             /**< 最后报告的温度 */
    int64_t last_update_ms;         /**< 最后更新时间 */
    uint32_t update_count;          /**< 更新计数 */
    bool active;                    /**< 是否激活 */
} ts_temp_provider_info_t;

/** 最大绑定变量名长度 */
#define TS_TEMP_MAX_VARNAME_LEN     64

/** 最大加权绑定变量数量 */
#define TS_TEMP_MAX_BOUND_VARS      8

/**
 * @brief 加权绑定变量条目
 */
typedef struct {
    char name[TS_TEMP_MAX_VARNAME_LEN];     /**< 变量名 */
    float weight;                            /**< 权重 0.0 - 1.0 */
} ts_temp_bound_var_t;

/**
 * @brief 温度源状态信息
 */
typedef struct {
    bool initialized;                       /**< 初始化状态 */
    ts_temp_source_type_t active_source;    /**< 当前活动源 */
    ts_temp_source_type_t preferred_source; /**< 用户首选源（0 表示自动选择）*/
    char bound_variable[TS_TEMP_MAX_VARNAME_LEN]; /**< 绑定的变量名（向后兼容，取第一个） */
    ts_temp_bound_var_t bound_vars[TS_TEMP_MAX_BOUND_VARS]; /**< 加权绑定变量数组 */
    uint8_t bound_var_count;                /**< 绑定变量数量 */
    int16_t current_temp;                   /**< 当前温度 */
    int64_t current_timestamp_ms;           /**< 当前温度更新时间 */
    bool current_valid;                     /**< 当前温度是否有效 */
    bool manual_mode;                       /**< 手动模式启用 */
    bool variable_partial_stale;            /**< 变量温度源是否部分失效 */
    uint8_t variable_valid_count;           /**< 变量温度源有效绑定数量 */
    uint8_t variable_total_count;           /**< 变量温度源正权重绑定数量 */
    float variable_valid_weight;            /**< 变量温度源有效权重和 */
    float variable_total_weight;            /**< 变量温度源正权重和 */
    uint32_t provider_count;                /**< 注册的 provider 数量 */
    ts_temp_provider_info_t providers[TS_TEMP_MAX_PROVIDERS]; /**< Provider 信息 */
} ts_temp_status_t;

/*===========================================================================*/
/*                              API Functions                                 */
/*===========================================================================*/

/**
 * @brief 初始化温度源管理
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_source_init(void);

/**
 * @brief 反初始化温度源管理
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_source_deinit(void);

/**
 * @brief 检查是否已初始化
 * @return true 已初始化
 */
bool ts_temp_source_is_initialized(void);

/*---------------------------------------------------------------------------*/
/*                          Provider 接口                                     */
/*---------------------------------------------------------------------------*/

/**
 * @brief 注册温度 provider
 * 
 * @param type Provider 类型
 * @param name Provider 名称（用于日志和调试）
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_provider_register(ts_temp_source_type_t type, const char *name);

/**
 * @brief 注销温度 provider
 * 
 * @param type Provider 类型
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_provider_unregister(ts_temp_source_type_t type);

/**
 * @brief Provider 更新温度值
 * 
 * 由各 provider（AGX monitor、本地传感器等）调用。
 * 内部会进行优先级判断，如果是当前活动源则发布事件。
 * 
 * @param type Provider 类型
 * @param temp_01c 温度值（0.1°C 单位）
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_provider_update(ts_temp_source_type_t type, int16_t temp_01c);

/*---------------------------------------------------------------------------*/
/*                          消费者接口                                        */
/*---------------------------------------------------------------------------*/

/**
 * @brief 获取当前有效温度
 * 
 * 基于优先级返回最高优先级且未过期的温度。
 * 
 * @param[out] data 温度数据（可为 NULL 仅获取温度值）
 * @return 当前温度（0.1°C 单位）
 */
int16_t ts_temp_get_effective(ts_temp_data_t *data);

/**
 * @brief 获取当前有效温度（非阻塞）
 *
 * 与 ts_temp_get_effective() 相同，但不会等待温度源锁或变量锁。
 * 如果当前锁繁忙，返回默认温度且 data.valid=false。
 *
 * @param[out] data 温度数据（可为 NULL 仅获取温度值）
 * @return 当前温度（0.1°C 单位）
 */
int16_t ts_temp_get_effective_nonblocking(ts_temp_data_t *data);

/**
 * @brief 获取指定源的温度
 * 
 * @param type 温度源类型
 * @param[out] data 温度数据
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 未找到
 */
esp_err_t ts_temp_get_by_source(ts_temp_source_type_t type, ts_temp_data_t *data);

/*---------------------------------------------------------------------------*/
/*                          手动模式控制                                      */
/*---------------------------------------------------------------------------*/

/**
 * @brief 设置手动温度值
 * 
 * 启用手动模式并设置温度值。手动模式具有最高优先级。
 * 
 * @param temp_01c 温度值（0.1°C 单位）
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_set_manual(int16_t temp_01c);

/**
 * @brief 启用/禁用手动模式
 * 
 * @param enable true 启用手动模式（使用上次设置的手动温度）
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_set_manual_mode(bool enable);

/**
 * @brief 检查是否为手动模式
 * 
 * @return true 手动模式启用
 */
bool ts_temp_is_manual_mode(void);

/*---------------------------------------------------------------------------*/
/*                          首选温度源选择                                    */
/*---------------------------------------------------------------------------*/

/**
 * @brief 设置首选温度源
 * 
 * 在非手动模式下，系统将优先使用指定的温度源。
 * 如果首选源不可用或数据过期，会自动降级到下一个可用源。
 * 
 * @param type 首选温度源类型（不能选择 MANUAL 或 DEFAULT）
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_set_preferred_source(ts_temp_source_type_t type);

/**
 * @brief 获取首选温度源
 * 
 * @return 当前设置的首选温度源类型
 */
ts_temp_source_type_t ts_temp_get_preferred_source(void);

/**
 * @brief 清除首选温度源（恢复优先级自动选择）
 * 
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_clear_preferred_source(void);

/*---------------------------------------------------------------------------*/
/*                          变量绑定                                          */
/*---------------------------------------------------------------------------*/

/**
 * @brief 绑定温度源到系统变量（单变量，权重 1.0，向后兼容）
 * 
 * @param var_name 变量名（如 "agx.cpu_temp"）
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_bind_variable(const char *var_name);

/**
 * @brief 绑定多个加权温度变量
 * 
 * 系统将按权重加权求和计算有效温度。
 * 例如: cpu_temp*0.4 + gpu_temp*0.6 = 加权温度
 * 每个权重范围为 0.0 ~ 1.0，且总权重必须大于 0。
 * 
 * @param vars 加权变量数组
 * @param count 数组长度（1 ~ TS_TEMP_MAX_BOUND_VARS）
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_bind_variables(const ts_temp_bound_var_t *vars, uint8_t count);

/**
 * @brief 获取绑定的变量名（向后兼容，返回第一个变量名）
 * 
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 未绑定
 */
esp_err_t ts_temp_get_bound_variable(char *buffer, size_t buffer_size);

/**
 * @brief 获取所有加权绑定变量
 * 
 * @param[out] vars 输出数组（至少 TS_TEMP_MAX_BOUND_VARS 大小）
 * @param[out] count 输出实际数量
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 无绑定
 */
esp_err_t ts_temp_get_bound_variables(ts_temp_bound_var_t *vars, uint8_t *count);

/**
 * @brief 清除变量绑定
 * 
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_unbind_variable(void);

/*---------------------------------------------------------------------------*/
/*                          状态查询                                          */
/*---------------------------------------------------------------------------*/

/**
 * @brief 获取温度源状态
 * 
 * @param[out] status 状态信息
 * @return ESP_OK 成功
 */
esp_err_t ts_temp_get_status(ts_temp_status_t *status);

/**
 * @brief 获取当前活动的温度源类型
 * 
 * @return 当前活动的温度源类型
 */
ts_temp_source_type_t ts_temp_get_active_source(void);

/**
 * @brief 温度源类型转字符串
 * 
 * @param type 温度源类型
 * @return 类型名称字符串
 */
const char *ts_temp_source_type_to_str(ts_temp_source_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* TS_TEMP_SOURCE_H */
