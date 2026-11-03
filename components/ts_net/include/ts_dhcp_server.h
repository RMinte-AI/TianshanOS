/**
 * @file ts_dhcp_server.h
 * @brief TianShanOS DHCP Server
 *
 * DHCP 服务器模块，为连接到 AP 或以太网桥接的设备分配 IP
 * 
 * 特性：
 * - 支持 WiFi AP 和以太网接口
 * - 可配置地址池范围
 * - 客户端租约管理
 * - 静态绑定支持
 * - NVS 配置持久化
 * - 事件通知
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_DHCP_SERVER_H
#define TS_DHCP_SERVER_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 常量定义
 * ========================================================================== */

#define TS_DHCP_MAX_CLIENTS         32      /**< 最大客户端数 */
#define TS_DHCP_MAX_STATIC_BINDINGS 16      /**< 最大静态绑定数 */
#define TS_DHCP_HOSTNAME_MAX_LEN    32      /**< 最大主机名长度 */
#define TS_DHCP_IP_STR_MAX_LEN      16      /**< IP 字符串最大长度 */

/** 默认配置 - 参考 robOS 的配置 */
#define TS_DHCP_DEFAULT_START_IP    "10.10.99.100"
#define TS_DHCP_DEFAULT_END_IP      "10.10.99.103"   /* 只需要 4 个 IP (100-103) */
#define TS_DHCP_DEFAULT_NETMASK     "255.255.255.0"
#define TS_DHCP_DEFAULT_GATEWAY     "10.10.99.100"   /* 网关 = 网关设备的 USB 网卡 IP (DHCP 分配的第一个) */
#define TS_DHCP_DEFAULT_DNS         "8.8.8.8"        /* 使用公共DNS */
#define TS_DHCP_DEFAULT_LEASE_TIME  1440    /**< 默认租约时间24小时(分钟) */

/* Match ESP-IDF 5.5.2 SoftAP defaults; SDK drift is checked by host tests. */
#define TS_DHCP_AP_DEFAULT_GATEWAY    "192.168.4.1"
#define TS_DHCP_AP_DEFAULT_START_IP   "192.168.4.2"
#define TS_DHCP_AP_DEFAULT_END_IP     "192.168.4.101"
#define TS_DHCP_AP_DEFAULT_LEASE_TIME 120


/* ============================================================================
 * 类型定义
 * ========================================================================== */

/**
 * @brief DHCP 服务器所在接口
 */
typedef enum {
    TS_DHCP_IF_AP = 0,      /**< WiFi AP 接口 */
    TS_DHCP_IF_ETH,         /**< 以太网接口（作为网桥时） */
    TS_DHCP_IF_MAX
} ts_dhcp_if_t;

/**
 * @brief DHCP 服务器状态
 */
typedef enum {
    TS_DHCP_STATE_STOPPED = 0,  /**< 已停止 */
    TS_DHCP_STATE_STARTING,     /**< 启动中 */
    TS_DHCP_STATE_RUNNING,      /**< 运行中 */
    TS_DHCP_STATE_ERROR,        /**< 错误 */
} ts_dhcp_server_state_t;

/**
 * @brief 地址池配置
 */
typedef struct {
    char start_ip[TS_DHCP_IP_STR_MAX_LEN];  /**< 起始 IP */
    char end_ip[TS_DHCP_IP_STR_MAX_LEN];    /**< 结束 IP */
    char netmask[TS_DHCP_IP_STR_MAX_LEN];   /**< 子网掩码 */
    char gateway[TS_DHCP_IP_STR_MAX_LEN];   /**< 网关 */
    char dns1[TS_DHCP_IP_STR_MAX_LEN];      /**< 主 DNS */
    char dns2[TS_DHCP_IP_STR_MAX_LEN];      /**< 备用 DNS */
} ts_dhcp_pool_t;

/**
 * @brief DHCP 服务器配置
 */
typedef struct {
    bool enabled;                           /**< 是否启用 */
    ts_dhcp_pool_t pool;                    /**< 地址池 */
    uint32_t lease_time_min;                /**< 租约时间(分钟) */
    bool auto_start;                        /**< 自动启动 */
} ts_dhcp_config_t;

/**
 * @brief 客户端租约信息
 */
typedef struct {
    uint8_t mac[6];                         /**< MAC 地址 */
    char ip[TS_DHCP_IP_STR_MAX_LEN];        /**< 分配的 IP */
    char hostname[TS_DHCP_HOSTNAME_MAX_LEN]; /**< 客户端主机名 */
    uint32_t lease_start;                   /**< 租约开始时间(UNIX) */
    uint32_t lease_expire;                  /**< 租约到期时间(UNIX)，0 表示未知 */
    bool is_static;                         /**< 是否静态绑定 */
} ts_dhcp_client_t;

/**
 * @brief 静态绑定配置
 */
typedef struct {
    uint8_t mac[6];                         /**< MAC 地址 */
    char ip[TS_DHCP_IP_STR_MAX_LEN];        /**< 绑定的 IP */
    char hostname[TS_DHCP_HOSTNAME_MAX_LEN]; /**< 备注/主机名 */
    bool enabled;                           /**< 是否启用 */
} ts_dhcp_static_binding_t;

/**
 * @brief DHCP 服务器状态信息
 */
typedef struct {
    ts_dhcp_server_state_t state;           /**< 当前状态 */
    uint32_t total_pool_size;               /**< 地址池总大小 */
    uint32_t available_count;               /**< 可用地址数 */
    uint32_t active_leases;                 /**< 活动租约数 */
    uint32_t total_offers;                  /**< 累计分配次数 */
    uint32_t uptime_sec;                    /**< 运行时间(秒) */
} ts_dhcp_status_t;

/**
 * @brief DHCP 事件类型
 */
typedef enum {
    TS_DHCP_EVENT_STARTED = 0,      /**< 服务器启动 */
    TS_DHCP_EVENT_STOPPED,          /**< 服务器停止 */
    TS_DHCP_EVENT_LEASE_NEW,        /**< 新租约分配 */
    TS_DHCP_EVENT_LEASE_RENEW,      /**< 租约续期 */
    TS_DHCP_EVENT_LEASE_EXPIRE,     /**< 租约过期 */
    TS_DHCP_EVENT_CLIENT_CONNECT,   /**< 客户端连接 */
    TS_DHCP_EVENT_CLIENT_DISCONNECT,/**< 客户端断开 */
} ts_dhcp_event_t;

/**
 * @brief DHCP 事件回调
 */
typedef void (*ts_dhcp_event_cb_t)(ts_dhcp_if_t iface, ts_dhcp_event_t event, 
                                    const ts_dhcp_client_t *client, void *user_data);

/* ============================================================================
 * 初始化和生命周期
 * ========================================================================== */

/**
 * @brief 初始化 DHCP 服务器模块
 * @return ESP_OK 配置恢复成功；资源错误或配置恢复错误会返回具体错误。
 * 配置恢复失败时资源可能已就绪，使用 is_initialized() 判断资源生命周期；
 * 已就绪时重复 init 不重复申请资源，且仍报告未解决的恢复错误。
 */
esp_err_t ts_dhcp_server_init(void);

/**
 * @brief 反初始化 DHCP 服务器模块
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_deinit(void);

/**
 * @brief 检查是否已初始化
 * @return true 已初始化
 */
bool ts_dhcp_server_is_initialized(void);

/**
 * @brief 启动 DHCP 服务器
 * @param iface 接口类型
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_start(ts_dhcp_if_t iface);

/**
 * @brief 停止 DHCP 服务器
 * @param iface 接口类型
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_stop(ts_dhcp_if_t iface);

/**
 * @brief 重启 DHCP 服务器
 * @param iface 接口类型
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_restart(ts_dhcp_if_t iface);

/* ============================================================================
 * 状态查询
 * ========================================================================== */

/**
 * @brief 获取 DHCP 服务器状态
 * @param iface 接口类型
 * @param status 状态结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_get_status(ts_dhcp_if_t iface, ts_dhcp_status_t *status);

/**
 * @brief 获取服务器状态枚举
 * @param iface 接口类型
 * @return 状态枚举
 */
ts_dhcp_server_state_t ts_dhcp_server_get_state(ts_dhcp_if_t iface);

/**
 * @brief 检查服务器是否运行中
 * @param iface 接口类型
 * @return true 运行中
 */
bool ts_dhcp_server_is_running(ts_dhcp_if_t iface);

/* ============================================================================
 * 配置管理
 * ========================================================================== */

/**
 * @brief 获取待应用配置；编辑不会立即改变运行中的网段，使用 restart 显式应用。
 * @param iface 接口类型
 * @param config 配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_get_config(ts_dhcp_if_t iface, ts_dhcp_config_t *config);

/**
 * @brief 设置配置
 * @param iface 接口类型
 * @param config 配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_set_config(ts_dhcp_if_t iface, const ts_dhcp_config_t *config);

/**
 * @brief 设置地址池
 * @param iface 接口类型
 * @param pool 地址池配置
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_set_pool(ts_dhcp_if_t iface, const ts_dhcp_pool_t *pool);

/**
 * @brief 设置租约时间
 * @param iface 接口类型
 * @param lease_time_min 租约时间(分钟)
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_set_lease_time(ts_dhcp_if_t iface, uint32_t lease_time_min);

/**
 * @brief 保存配置到 NVS
 * 跳过恢复未完成的接口；另一接口仍可保存。非 ESP_OK 可能表示部分保存，
 * 每接口的具体结果见日志；不承诺多键写入的断电原子性。
 * @return ESP_OK 两接口均保存成功，否则返回具体错误。
 */
esp_err_t ts_dhcp_server_save_config(void);

/**
 * @brief 从 NVS 加载配置
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_load_config(void);

/** Storage recovery error only; application errors are reported separately.
 * An init restore error can be returned while module resources remain initialized.
 * Save skips an interface with unresolved storage recovery errors and reports failure. */
esp_err_t ts_dhcp_server_get_restore_error(ts_dhcp_if_t iface);

/** Last SDK application error; does not imply stored configuration is unread. */
esp_err_t ts_dhcp_server_get_apply_error(ts_dhcp_if_t iface);

/**
 * @brief 重置配置为默认值
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_reset_config(void);

/* ============================================================================
 * 客户端管理
 * ========================================================================== */

/**
 * @brief 获取客户端列表
 * @param iface 接口类型
 * @param clients 客户端数组（输出）
 * @param max_count 数组最大容量
 * @param count 实际数量（输出）
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_get_clients(ts_dhcp_if_t iface, ts_dhcp_client_t *clients,
                                      size_t max_count, size_t *count);

/**
 * @brief 通过 MAC 获取客户端信息
 * @param iface 接口类型
 * @param mac MAC 地址
 * @param client 客户端信息（输出）
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 未找到
 */
esp_err_t ts_dhcp_server_get_client_by_mac(ts_dhcp_if_t iface, const uint8_t mac[6],
                                            ts_dhcp_client_t *client);

/**
 * @brief 通过 IP 获取客户端信息
 * @param iface 接口类型
 * @param ip IP 地址字符串
 * @param client 客户端信息（输出）
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 未找到
 */
esp_err_t ts_dhcp_server_get_client_by_ip(ts_dhcp_if_t iface, const char *ip,
                                           ts_dhcp_client_t *client);

/**
 * @brief 释放客户端租约
 * @param iface 接口类型
 * @param mac MAC 地址
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_release_lease(ts_dhcp_if_t iface, const uint8_t mac[6]);

/**
 * @brief 释放所有租约
 * @param iface 接口类型
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_release_all_leases(ts_dhcp_if_t iface);

/* ============================================================================
 * 静态绑定
 * ========================================================================== */

/**
 * @brief 添加静态绑定
 * @param iface 接口类型
 * @param binding 绑定配置
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_add_static_binding(ts_dhcp_if_t iface, 
                                             const ts_dhcp_static_binding_t *binding);

/**
 * @brief 删除静态绑定
 * @param iface 接口类型
 * @param mac MAC 地址
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_remove_static_binding(ts_dhcp_if_t iface, const uint8_t mac[6]);

/**
 * @brief 获取静态绑定列表
 * @param iface 接口类型
 * @param bindings 绑定数组（输出）
 * @param max_count 数组最大容量
 * @param count 实际数量（输出）
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_get_static_bindings(ts_dhcp_if_t iface,
                                              ts_dhcp_static_binding_t *bindings,
                                              size_t max_count, size_t *count);

/**
 * @brief 清除所有静态绑定
 * 仅清除指定接口内存/生效表及其绑定恢复保护，不写 NVS，
 * 不解除独立的配置恢复错误；使用 save_config() 显式持久化。
 * @param iface 接口类型
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_clear_static_bindings(ts_dhcp_if_t iface);

/* ============================================================================
 * 事件回调
 * ========================================================================== */

/**
 * @brief 注册事件回调
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_register_event_cb(ts_dhcp_event_cb_t callback, void *user_data);

/**
 * @brief 注销事件回调
 * @param callback 回调函数
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_server_unregister_event_cb(ts_dhcp_event_cb_t callback);

/* ============================================================================
 * 工具函数
 * ========================================================================== */

/**
 * @brief 获取状态名称
 * @param state 状态枚举
 * @return 状态名称字符串
 */
const char *ts_dhcp_state_to_str(ts_dhcp_server_state_t state);

/**
 * @brief 获取接口名称
 * @param iface 接口类型
 * @return 接口名称字符串
 */
const char *ts_dhcp_if_to_str(ts_dhcp_if_t iface);

/**
 * @brief 获取事件名称
 * @param event 事件类型
 * @return 事件名称字符串
 */
const char *ts_dhcp_event_to_str(ts_dhcp_event_t event);

/**
 * @brief MAC 字符串转数组
 * @param mac_str MAC 字符串 (xx:xx:xx:xx:xx:xx)
 * @param mac MAC 数组（输出）
 * @return ESP_OK 成功
 */
esp_err_t ts_dhcp_mac_str_to_array(const char *mac_str, uint8_t mac[6]);

/**
 * @brief MAC 数组转字符串
 * @param mac MAC 数组
 * @param buf 输出缓冲区
 * @param buf_len 缓冲区长度
 * @return 字符串指针
 */
const char *ts_dhcp_mac_array_to_str(const uint8_t mac[6], char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* TS_DHCP_SERVER_H */
