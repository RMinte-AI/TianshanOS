/**
 * @file ts_net_manager.h
 * @brief TianShanOS Network Manager
 *
 * 网络管理器，提供统一的网络接口管理
 * 
 * 特性：
 * - 以太网 (W5500) 和 WiFi 统一管理
 * - 配置驱动：从 pins.json 读取引脚配置
 * - 状态机管理网络生命周期
 * - 支持静态 IP 和 DHCP
 * - 配置持久化到 NVS
 * - 事件总线集成
 * - 线程安全
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_NET_MANAGER_H
#define TS_NET_MANAGER_H

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

#define TS_NET_HOSTNAME_MAX_LEN     32
#define TS_NET_IP_STR_MAX_LEN       16
#define TS_NET_MAC_ADDR_LEN         6

/** 默认配置 */
#define TS_NET_DEFAULT_IP           "10.10.99.97"
#define TS_NET_DEFAULT_GATEWAY      "10.10.99.100"
#define TS_NET_DEFAULT_NETMASK      "255.255.255.0"
#define TS_NET_DEFAULT_DNS          "8.8.8.8"
#define TS_NET_DEFAULT_HOSTNAME     "tianshaos"

/* ============================================================================
 * 类型定义
 * ========================================================================== */

/**
 * @brief 网络接口类型
 */
typedef enum {
    TS_NET_IF_ETH = 0,      /**< 以太网 (W5500) */
    TS_NET_IF_WIFI_STA,     /**< WiFi Station */
    TS_NET_IF_WIFI_AP,      /**< WiFi Access Point */
    TS_NET_IF_MAX
} ts_net_if_t;

/**
 * @brief 网络管理器状态
 */
typedef enum {
    TS_NET_STATE_UNINITIALIZED = 0, /**< 未初始化 */
    TS_NET_STATE_INITIALIZED,       /**< 已初始化，未启动 */
    TS_NET_STATE_STARTING,          /**< 启动中 */
    TS_NET_STATE_DISCONNECTED,      /**< 已启动，无连接 */
    TS_NET_STATE_CONNECTING,        /**< 连接中 */
    TS_NET_STATE_CONNECTED,         /**< 已连接，无 IP */
    TS_NET_STATE_GOT_IP,            /**< 已获取 IP，就绪 */
    TS_NET_STATE_ERROR,             /**< 错误状态 */
    TS_NET_STATE_MAX
} ts_net_state_t;

/**
 * @brief IP 配置模式
 */
typedef enum {
    TS_NET_IP_MODE_DHCP = 0,    /**< DHCP 自动获取 */
    TS_NET_IP_MODE_STATIC,      /**< 静态 IP */
} ts_net_ip_mode_t;

/**
 * @brief IP 配置信息
 */
typedef struct {
    char ip[TS_NET_IP_STR_MAX_LEN];         /**< IP 地址 */
    char netmask[TS_NET_IP_STR_MAX_LEN];    /**< 子网掩码 */
    char gateway[TS_NET_IP_STR_MAX_LEN];    /**< 网关 */
    char dns1[TS_NET_IP_STR_MAX_LEN];       /**< 主 DNS */
    char dns2[TS_NET_IP_STR_MAX_LEN];       /**< 备用 DNS */
} ts_net_ip_info_str_t;

/**
 * @brief 网络接口配置
 */
typedef struct {
    bool enabled;                               /**< 是否启用 */
    ts_net_ip_mode_t ip_mode;                   /**< IP 配置模式 */
    ts_net_ip_info_str_t static_ip;             /**< 静态 IP 配置 */
    char hostname[TS_NET_HOSTNAME_MAX_LEN];     /**< 主机名 */
    bool auto_start;                            /**< 是否自动启动 */
    
    /* WiFi 专用配置 */
    char ssid[32];                              /**< WiFi SSID */
    char password[64];                          /**< WiFi 密码 */
    uint8_t channel;                            /**< WiFi 信道（仅 AP） */
    uint8_t max_connections;                    /**< 最大连接数（仅 AP） */
} ts_net_if_config_t;

/**
 * @brief 网络接口状态信息
 */
typedef struct {
    ts_net_state_t state;                       /**< 当前状态 */
    esp_err_t config_error;                     /**< ETH host configuration/read error, ESP_OK when valid */
    bool link_up;                               /**< 物理链路状态 */
    bool has_ip;                                /**< 是否有 IP */
    uint8_t mac[TS_NET_MAC_ADDR_LEN];           /**< MAC 地址 */
    ts_net_ip_info_str_t ip_info;               /**< 当前 IP 信息 */
    
    /* 统计信息 */
    uint32_t rx_packets;                        /**< 接收包数 */
    uint32_t tx_packets;                        /**< 发送包数 */
    uint64_t rx_bytes;                          /**< 接收字节数 */
    uint64_t tx_bytes;                          /**< 发送字节数 */
    uint32_t rx_errors;                         /**< 接收错误数 */
    uint32_t tx_errors;                         /**< 发送错误数 */
    
    /* 时间信息 */
    uint32_t uptime_sec;                        /**< 连接时长(秒) */
    uint32_t last_activity_ms;                  /**< 最后活动时间 */
} ts_net_if_status_t;

/**
 * @brief 网络管理器全局状态
 */
typedef struct {
    bool initialized;                           /**< 是否已初始化 */
    ts_net_if_status_t eth;                     /**< 以太网状态 */
    ts_net_if_status_t wifi_sta;                /**< WiFi STA 状态 */
    ts_net_if_status_t wifi_ap;                 /**< WiFi AP 状态 */
    char hostname[TS_NET_HOSTNAME_MAX_LEN];     /**< 当前主机名 */
} ts_net_manager_status_t;

/**
 * @brief 网络事件回调函数类型
 */
typedef void (*ts_net_event_cb_t)(ts_net_if_t iface, ts_net_state_t state, void *user_data);

/* ============================================================================
 * 初始化和生命周期
 * ========================================================================== */

/**
 * @brief 初始化网络管理器
 * 
 * 从 pins.json 读取引脚配置，从 NVS 读取网络配置
 * 
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_init(void);

/**
 * @brief 反初始化网络管理器（生命周期所有者须先停止其他使用者）
 * @return ESP_OK 成功；ETH 操作忙或停止失败时返回错误，保留资源供查询/修复，不报告销毁成功。
 */
esp_err_t ts_net_manager_deinit(void);

/**
 * @brief 检查是否已初始化
 * @return true 已初始化
 */
bool ts_net_manager_is_initialized(void);

/**
 * @brief 启动指定网络接口
 * @param iface 接口类型
 * @return ESP_OK 成功；并发/回调重入 ETH 生命周期操作返回 ESP_ERR_NOT_FINISHED。
 * 状态查询不受此操作互斥限制；驱动调用及通知不持 manager 状态锁。
 */
esp_err_t ts_net_manager_start(ts_net_if_t iface);

/**
 * @brief 停止指定网络接口
 * @param iface 接口类型
 * @return ESP_OK 成功；并发/回调重入 ETH 生命周期操作返回 ESP_ERR_NOT_FINISHED。
 * 状态查询不受此操作互斥限制；驱动调用及通知不持 manager 状态锁。
 */
esp_err_t ts_net_manager_stop(ts_net_if_t iface);

/**
 * @brief 重启指定网络接口
 * @param iface 接口类型
 * @return ESP_OK 成功；并发/回调重入 ETH 生命周期操作返回 ESP_ERR_NOT_FINISHED。
 * 状态查询不受此操作互斥限制；驱动调用及通知不持 manager 状态锁。
 */
esp_err_t ts_net_manager_restart(ts_net_if_t iface);

/* ============================================================================
 * 状态查询
 * ========================================================================== */

/**
 * @brief 获取网络管理器全局状态
 * @param status 状态结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_get_status(ts_net_manager_status_t *status);

/**
 * @brief 获取指定接口状态
 * @param iface 接口类型
 * @param status 状态结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_get_if_status(ts_net_if_t iface, ts_net_if_status_t *status);

/**
 * @brief 获取接口状态枚举值
 * @param iface 接口类型
 * @return 状态枚举
 */
ts_net_state_t ts_net_manager_get_state(ts_net_if_t iface);

/**
 * @brief 检查接口是否已连接且有 IP
 * @param iface 接口类型
 * @return true 已就绪
 */
bool ts_net_manager_is_ready(ts_net_if_t iface);

/**
 * @brief 获取接口 netif 句柄
 * @param iface 接口类型
 * @return netif 句柄，失败返回 NULL
 */
esp_netif_t *ts_net_manager_get_netif(ts_net_if_t iface);

/* ============================================================================
 * 配置管理
 * ========================================================================== */

/**
 * @brief 获取接口配置
 * @param iface 接口类型
 * @param config 配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_get_config(ts_net_if_t iface, ts_net_if_config_t *config);

/**
 * @brief 设置接口配置
 * @param iface 接口类型
 * @param config 配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_set_config(ts_net_if_t iface, const ts_net_if_config_t *config);

/**
 * @brief 设置 IP 模式（DHCP 或静态）
 * @param iface 接口类型
 * @param mode IP 模式
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_set_ip_mode(ts_net_if_t iface, ts_net_ip_mode_t mode);

/**
 * @brief 设置静态 IP 配置
 * @param iface 接口类型
 * @param ip_info IP 配置
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_set_static_ip(ts_net_if_t iface, const ts_net_ip_info_str_t *ip_info);

/** Validate an ETH static host configuration before creating/applying a netif.
 * ETH is a DHCP server interface; client DHCP mode is not supported. */
esp_err_t ts_net_validate_eth_ip(const esp_netif_ip_info_t *info);
esp_err_t ts_net_validate_eth_config(const ts_net_if_config_t *config, esp_netif_ip_info_t *ip_info);

/**
 * @brief 设置主机名
 * @param hostname 主机名
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_set_hostname(const char *hostname);

/**
 * @brief 获取主机名
 * @return 主机名字符串
 */
const char *ts_net_manager_get_hostname(void);

/* ============================================================================
 * 配置持久化
 * ========================================================================== */

/**
 * @brief 保存配置到 NVS
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_save_config(void);

/**
 * @brief 从 NVS 加载配置
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_load_config(void);

/**
 * @brief 重置配置为默认值
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_reset_config(void);

/* ============================================================================
 * 事件回调
 * ========================================================================== */

/**
 * @brief 注册事件回调
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_register_event_callback(ts_net_event_cb_t callback, void *user_data);

/**
 * @brief 注销事件回调
 * @param callback 回调函数
 * @return ESP_OK 成功
 */
esp_err_t ts_net_manager_unregister_event_callback(ts_net_event_cb_t callback);

/* ============================================================================
 * 工具函数
 * ========================================================================== */

/**
 * @brief IP 字符串转 uint32
 * @param ip_str IP 字符串 (x.x.x.x)
 * @return uint32 格式的 IP
 */
uint32_t ts_net_ip_str_to_u32(const char *ip_str);

/**
 * @brief uint32 IP 转字符串
 * @param ip uint32 格式的 IP
 * @param buf 输出缓冲区
 * @param buf_len 缓冲区长度
 * @return 字符串指针
 */
const char *ts_net_ip_u32_to_str(uint32_t ip, char *buf, size_t buf_len);

/**
 * @brief 获取状态名称字符串
 * @param state 状态枚举
 * @return 状态名称
 */
const char *ts_net_state_to_str(ts_net_state_t state);

/**
 * @brief 获取接口名称字符串
 * @param iface 接口类型
 * @return 接口名称
 */
const char *ts_net_if_to_str(ts_net_if_t iface);

#ifdef __cplusplus
}
#endif

#endif /* TS_NET_MANAGER_H */
