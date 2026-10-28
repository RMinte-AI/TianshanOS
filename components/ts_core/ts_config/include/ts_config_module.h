/**
 * @file ts_config_module.h
 * @brief TianShanOS Unified Configuration Module System
 *
 * 统一配置模块系统
 * - 模块化配置管理（每个组件独立模块）
 * - 系统配置只写默认 NVS
 * - SD 卡仅显式导入／导出
 * - Schema 版本迁移支持
 * - 无 RTC 设计（使用序列号而非时间戳）
 *
 * 配置优先级（高→低）：
 * 1. 内存缓存（CLI/API 运行时修改）
 * 2. 系统 NVS（各模块独立命名空间）
 * 3. Schema 默认值
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_CONFIG_MODULE_H
#define TS_CONFIG_MODULE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "ts_config.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 常量定义
 * ========================================================================== */

/** SD卡配置目录 */
#define TS_CONFIG_SDCARD_PATH       "/sdcard/config"

/** 模块配置文件名最大长度 */
#define TS_CONFIG_MODULE_NAME_MAX   32

/** Schema 条目键名最大长度 */
#define TS_CONFIG_SCHEMA_KEY_MAX    48

/** 元配置 NVS 命名空间 */
#define TS_CONFIG_META_NAMESPACE    "ts_meta"

/* ============================================================================
 * 错误码定义
 * ========================================================================== */

#define TS_CONFIG_ERR_BASE              0x10000

#define TS_CONFIG_ERR_NOT_FOUND         (TS_CONFIG_ERR_BASE + 1)   /**< 配置不存在 */
#define TS_CONFIG_ERR_TYPE_MISMATCH     (TS_CONFIG_ERR_BASE + 2)   /**< 类型不匹配 */
#define TS_CONFIG_ERR_BUFFER_TOO_SMALL  (TS_CONFIG_ERR_BASE + 3)   /**< 缓冲区太小 */
#define TS_CONFIG_ERR_SD_NOT_MOUNTED    (TS_CONFIG_ERR_BASE + 4)   /**< SD卡未挂载 */
#define TS_CONFIG_ERR_PARSE_FAILED      (TS_CONFIG_ERR_BASE + 5)   /**< JSON 解析失败 */
#define TS_CONFIG_ERR_SCHEMA_MISMATCH   (TS_CONFIG_ERR_BASE + 6)   /**< Schema 版本不兼容 */
#define TS_CONFIG_ERR_MIGRATE_FAILED    (TS_CONFIG_ERR_BASE + 7)   /**< 迁移失败 */
#define TS_CONFIG_ERR_MODULE_NOT_FOUND  (TS_CONFIG_ERR_BASE + 8)   /**< 模块未注册 */
#define TS_CONFIG_ERR_ALREADY_REGISTERED (TS_CONFIG_ERR_BASE + 9)  /**< 模块已注册 */

/* ============================================================================
 * 类型定义
 * ========================================================================== */

/**
 * @brief 配置模块枚举
 * 
 * 每个模块对应独立的配置文件和 NVS 命名空间
 */
typedef enum {
    TS_CONFIG_MODULE_NET = 0,   /**< 网络配置 (net.json / ts_net) */
    TS_CONFIG_MODULE_DHCP,      /**< DHCP 配置 (dhcp.json / ts_dhcp) */
    TS_CONFIG_MODULE_WIFI,      /**< WiFi 配置 (wifi.json / ts_wifi) */
    TS_CONFIG_MODULE_NAT,       /**< NAT 配置 (nat.json / ts_nat) */
    TS_CONFIG_MODULE_LED,       /**< LED 配置 (led.json / ts_led) */
    TS_CONFIG_MODULE_FAN,       /**< 风扇配置 (fan.json / ts_fan) */
    TS_CONFIG_MODULE_DEVICE,    /**< 设备控制配置 (device.json / ts_device) */
    TS_CONFIG_MODULE_SYSTEM,    /**< 系统配置 (system.json / ts_system) */
    TS_CONFIG_MODULE_TEMP,      /**< 温度源配置 (temp.json / ts_temp) */
    TS_CONFIG_MODULE_RULES,     /**< 自动化规则配置 (rules.json / ts_rules) */
    TS_CONFIG_MODULE_ACTIONS,   /**< 动作模板配置 (actions.json / ts_actions) */
    TS_CONFIG_MODULE_SOURCES,   /**< 数据源配置 (sources.json / ts_sources) */
    TS_CONFIG_MODULE_MAX        /**< 模块数量（用于边界检查）*/
} ts_config_module_t;

/**
 * @brief Schema 条目定义
 * 
 * 定义单个配置项的键名、类型、默认值
 * 注意：使用 ts_config.h 中定义的类型（TS_CONFIG_TYPE_INT32 等）
 */
typedef struct {
    const char         *key;            /**< 配置键（如 "eth.ip"） */
    ts_config_type_t    type;           /**< 值类型（使用 TS_CONFIG_TYPE_* 常量） */
    union {
        bool            default_bool;
        int32_t         default_int32;
        uint32_t        default_uint32;
        int64_t         default_int64;
        float           default_float;
        double          default_double;
        const char     *default_str;
    };
    const char         *description;    /**< 配置项描述（可选） */
} ts_config_schema_entry_t;

/**
 * @brief Schema 迁移函数类型
 * 
 * @param old_version 旧版本号
 * @return ESP_OK 迁移成功
 */
typedef esp_err_t (*ts_config_migrate_fn_t)(uint16_t old_version);

/**
 * @brief 模块 Schema 定义
 */
typedef struct {
    uint16_t                        version;        /**< Schema 版本号 */
    const ts_config_schema_entry_t *entries;        /**< Schema 条目数组 */
    size_t                          entry_count;    /**< 条目数量 */
    ts_config_migrate_fn_t          migrate;        /**< 迁移函数（可选） */
} ts_config_module_schema_t;

/**
 * @brief 模块配置变更回调
 * 
 * @param module 模块ID
 * @param key 变更的键名
 * @param user_data 用户数据
 */
typedef void (*ts_config_module_change_cb_t)(ts_config_module_t module,
                                              const char *key,
                                              void *user_data);

/**
 * @brief 模块信息（内部使用）
 */
typedef struct {
    bool                            registered;     /**< 是否已注册 */
    char                            nvs_namespace[16]; /**< NVS 命名空间 */
    const ts_config_module_schema_t *schema;        /**< Schema 定义 */
    uint16_t                        loaded_version; /**< 已加载的 Schema 版本 */
    uint32_t                        seq;            /**< 当前序列号 */
    bool                            dirty;          /**< 是否有未保存更改 */
} ts_config_module_info_t;

/* ============================================================================
 * 系统初始化 API
 * ========================================================================== */

/**
 * @brief 初始化配置模块系统
 * 
 * 必须在注册任何模块之前调用
 * 
 * @return ESP_OK 成功
 */
esp_err_t ts_config_module_system_init(void);

/* ============================================================================
 * 模块注册 API
 * ========================================================================== */

/**
 * @brief 注册配置模块
 * 
 * @param module 模块ID
 * @param nvs_namespace NVS 命名空间名（最长15字符）
 * @param schema Schema 定义
 * @return 
 *      - ESP_OK: 成功
 *      - ESP_ERR_INVALID_ARG: 参数无效
 *      - TS_CONFIG_ERR_ALREADY_REGISTERED: 模块已注册
 */
esp_err_t ts_config_module_register(
    ts_config_module_t module,
    const char *nvs_namespace,
    const ts_config_module_schema_t *schema
);

/**
 * @brief 检查模块是否已注册
 * 
 * @param module 模块ID
 * @return true 已注册，false 未注册
 */
bool ts_config_module_is_registered(ts_config_module_t module);

/**
 * @brief 获取模块名称
 * 
 * @param module 模块ID
 * @return 模块名称字符串
 */
const char *ts_config_module_get_name(ts_config_module_t module);

/* ============================================================================
 * 配置加载 API
 * ========================================================================== */

/**
 * @brief 从存储加载模块配置
 * 
 * 加载逻辑：
 * 从系统 NVS 加载；仅不存在时应用 Schema 默认值。
 * SD 文件只在一次性迁移或明确导入时读取。
 * 
 * @param module 模块ID，传入 TS_CONFIG_MODULE_MAX 表示加载全部
 * @return ESP_OK 成功
 */
esp_err_t ts_config_module_load(ts_config_module_t module);

/**
 * @brief 强制从 SD卡加载配置
 * 
 * @param module 模块ID
 * @return ESP_OK 成功，TS_CONFIG_ERR_SD_NOT_MOUNTED SD卡未挂载
 */
esp_err_t ts_config_module_load_from_sdcard(ts_config_module_t module);

/**
 * @brief 强制从 NVS 加载配置
 * 
 * @param module 模块ID
 * @return ESP_OK 成功
 */
esp_err_t ts_config_module_load_from_nvs(ts_config_module_t module);

/* ============================================================================
 * 配置读取 API（按优先级：内存缓存 > 默认值）
 * ========================================================================== */

/**
 * @brief 获取布尔配置值
 */
esp_err_t ts_config_module_get_bool(ts_config_module_t module, const char *key, bool *value);

/**
 * @brief 获取整数配置值
 */
esp_err_t ts_config_module_get_int(ts_config_module_t module, const char *key, int32_t *value);

/**
 * @brief 获取无符号整数配置值
 */
esp_err_t ts_config_module_get_uint(ts_config_module_t module, const char *key, uint32_t *value);

/**
 * @brief 获取字符串配置值
 */
esp_err_t ts_config_module_get_string(ts_config_module_t module, const char *key, 
                                       char *buf, size_t len);

/**
 * @brief 获取浮点配置值
 */
esp_err_t ts_config_module_get_float(ts_config_module_t module, const char *key, float *value);

/* ============================================================================
 * 配置写入 API（仅写入内存缓存）
 * ========================================================================== */

/**
 * @brief 设置布尔配置值（临时）
 */
esp_err_t ts_config_module_set_bool(ts_config_module_t module, const char *key, bool value);

/**
 * @brief 设置整数配置值（临时）
 */
esp_err_t ts_config_module_set_int(ts_config_module_t module, const char *key, int32_t value);

/**
 * @brief 设置无符号整数配置值（临时）
 */
esp_err_t ts_config_module_set_uint(ts_config_module_t module, const char *key, uint32_t value);

/**
 * @brief 设置字符串配置值（临时）
 */
esp_err_t ts_config_module_set_string(ts_config_module_t module, const char *key, 
                                       const char *value);

/**
 * @brief 设置浮点配置值（临时）
 */
esp_err_t ts_config_module_set_float(ts_config_module_t module, const char *key, float value);

/* ============================================================================
 * 持久化 API（系统 NVS）
 * ========================================================================== */

/**
 * @brief 持久化模块配置
 * 
 * 写入系统 NVS 并确认提交结果；不自动写 SD 卡。
 * 
 * @param module 模块ID，传入 TS_CONFIG_MODULE_MAX 表示全部模块
 * @return ESP_OK 成功
 */
esp_err_t ts_config_module_persist(ts_config_module_t module);

/**
 * @brief 导出模块配置到 SD卡
 * 
 * @param module 模块ID，传入 TS_CONFIG_MODULE_MAX 表示全部模块
 * @return ESP_OK 成功
 */
esp_err_t ts_config_module_export_to_sdcard(ts_config_module_t module);

/**
 * @brief 导出自定义 JSON 到 SD卡配置文件
 * 
 * 用于 Schema-less 模块（如 RULES、ACTIONS、SOURCES）直接导出自己的 JSON 数据。
 * 这些模块有独立的 NVS 存储逻辑，无法通过 ts_config_module_persist() 自动生成 JSON。
 * 
 * @param module 模块ID，必须是已注册的模块
 * @param json 自定义 JSON 对象（调用者负责创建和释放）
 * @return 
 *      - ESP_OK: 成功
 *      - TS_CONFIG_ERR_MODULE_NOT_FOUND: 模块未注册
 *      - TS_CONFIG_ERR_SD_NOT_MOUNTED: SD卡未挂载
 *      - ESP_ERR_INVALID_ARG: json 为 NULL
 */
esp_err_t ts_config_module_export_custom_json(ts_config_module_t module, const cJSON *json);

/**
 * @brief 从 SD卡导入模块配置到 NVS
 * 
 * @param module 模块ID
 * @return ESP_OK 成功
 */
esp_err_t ts_config_module_import_from_sdcard(ts_config_module_t module);

/* ============================================================================
 * 重置 API
 * ========================================================================== */

/**
 * @brief 重置模块配置到 Schema 默认值
 * 
 * @param module 模块ID
 * @param persist 是否持久化到系统 NVS
 * @return ESP_OK 成功
 */
esp_err_t ts_config_module_reset(ts_config_module_t module, bool persist);

/* ============================================================================
 * 变更订阅 API
 * ========================================================================== */

/**
 * @brief 订阅模块配置变更
 * 
 * @param module 模块ID
 * @param key_pattern 键模式（支持通配符 *，NULL 表示全部）
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return ESP_OK 成功
 */
esp_err_t ts_config_module_subscribe(ts_config_module_t module,
                                      const char *key_pattern,
                                      ts_config_module_change_cb_t callback,
                                      void *user_data);

/* ============================================================================
 * 查询 API
 * ========================================================================== */

/**
 * @brief 获取模块的 Schema 版本
 */
uint16_t ts_config_module_get_schema_version(ts_config_module_t module);

/**
 * @brief 获取全局序列号
 */
uint32_t ts_config_module_get_global_seq(void);

/**
 * @brief 获取模块在 SD卡中的序列号
 */
uint32_t ts_config_module_get_sdcard_seq(ts_config_module_t module);

/**
 * @brief 检查模块是否有未保存的更改
 */
bool ts_config_module_is_dirty(ts_config_module_t module);

/**
 * @brief 获取模块的 NVS 命名空间
 */
const char *ts_config_module_get_nvs_namespace(ts_config_module_t module);

/**
 * @brief 获取模块的 SD卡配置文件路径
 */
esp_err_t ts_config_module_get_sdcard_path(ts_config_module_t module, char *path, size_t len);

/* ============================================================================
 * 事件处理（内部使用）
 * ========================================================================== */

/**
 * @brief 注册存储事件处理器
 * 
 * 监听 SD卡挂载/卸载事件，自动触发同步
 * 在 ts_config_init() 后调用
 * 
 * @return ESP_OK 成功
 */
esp_err_t ts_config_module_register_storage_events(void);

#ifdef __cplusplus
}
#endif

#endif /* TS_CONFIG_MODULE_H */
