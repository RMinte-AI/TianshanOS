/**
 * @file ts_config_schemas.c
 * @brief TianShanOS Configuration Module Schema Definitions
 *
 * 各模块的配置 Schema 定义和注册
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_config_module.h"
#include "ts_config_meta.h"
#include "esp_log.h"

static const char *TAG = "ts_config_schemas";

/* ============================================================================
 * NET 模块 Schema (v1)
 * 网络基础配置
 * ========================================================================== */

static const ts_config_schema_entry_t s_net_schema_entries[] = {
    /* 以太网配置 */
    {
        .key = "eth.enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "是否启用以太网"
    },
    {
        .key = "eth.dhcp",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "是否使用 DHCP 获取 IP"
    },
    {
        .key = "eth.ip",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "192.168.1.100",
        .description = "静态 IP 地址"
    },
    {
        .key = "eth.netmask",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "255.255.255.0",
        .description = "子网掩码"
    },
    {
        .key = "eth.gateway",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "192.168.1.1",
        .description = "默认网关"
    },
    {
        .key = "eth.dns",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "8.8.8.8",
        .description = "DNS 服务器"
    },
    /* 主机名 */
    {
        .key = "hostname",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "tianshan",
        .description = "设备主机名"
    },
};

static const ts_config_module_schema_t s_net_schema = {
    .version = 1,
    .entries = s_net_schema_entries,
    .entry_count = sizeof(s_net_schema_entries) / sizeof(s_net_schema_entries[0]),
    .migrate = NULL,
};

/* ============================================================================
 * DHCP 模块 Schema (v1)
 * DHCP 服务器配置
 * ========================================================================== */

static const ts_config_schema_entry_t s_dhcp_schema_entries[] = {
    {
        .key = "enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = false,
        .description = "是否启用 DHCP 服务器"
    },
    {
        .key = "start_ip",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "192.168.4.100",
        .description = "地址池起始 IP"
    },
    {
        .key = "end_ip",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "192.168.4.150",
        .description = "地址池结束 IP"
    },
    {
        .key = "lease_time",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 3600,
        .description = "租约时间（秒）"
    },
    {
        .key = "dns1",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "192.168.4.1",
        .description = "DNS 服务器 1"
    },
    {
        .key = "dns2",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "8.8.8.8",
        .description = "DNS 服务器 2"
    },
};

static const ts_config_module_schema_t s_dhcp_schema = {
    .version = 1,
    .entries = s_dhcp_schema_entries,
    .entry_count = sizeof(s_dhcp_schema_entries) / sizeof(s_dhcp_schema_entries[0]),
    .migrate = NULL,
};

/* ============================================================================
 * WIFI 模块 Schema (v1)
 * WiFi 配置
 * ========================================================================== */

static const ts_config_schema_entry_t s_wifi_schema_entries[] = {
    {
        .key = "mode",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "ap",
        .description = "WiFi 模式: ap, sta, apsta, off"
    },
    /* AP 模式配置 */
    {
        .key = "ap.ssid",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "TianShanOS",
        .description = "AP 热点名称"
    },
    {
        .key = "ap.password",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "12345678",
        .description = "AP 热点密码"
    },
    {
        .key = "ap.channel",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 1,
        .description = "AP 频道 (1-13)"
    },
    {
        .key = "ap.max_conn",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 4,
        .description = "最大连接数"
    },
    {
        .key = "ap.hidden",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = false,
        .description = "是否隐藏 SSID"
    },
    /* STA 模式配置 */
    {
        .key = "sta.ssid",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "",
        .description = "要连接的 WiFi 名称"
    },
    {
        .key = "sta.password",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "",
        .description = "要连接的 WiFi 密码"
    },
    {
        .key = "sta.dhcp",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "STA 模式是否使用 DHCP"
    },
};

static const ts_config_module_schema_t s_wifi_schema = {
    .version = 1,
    .entries = s_wifi_schema_entries,
    .entry_count = sizeof(s_wifi_schema_entries) / sizeof(s_wifi_schema_entries[0]),
    .migrate = NULL,
};

/* ============================================================================
 * NAT 模块 Schema (v1)
 * NAT 网关配置
 * ========================================================================== */

static const ts_config_schema_entry_t s_nat_schema_entries[] = {
    {
        .key = "enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = false,
        .description = "是否启用 NAT 网关"
    },
    {
        .key = "auto_start",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "WiFi 连接后自动启动 NAT"
    },
};

static const ts_config_module_schema_t s_nat_schema = {
    .version = 1,
    .entries = s_nat_schema_entries,
    .entry_count = sizeof(s_nat_schema_entries) / sizeof(s_nat_schema_entries[0]),
    .migrate = NULL,
};

/* ============================================================================
 * LED 模块 Schema (v1)
 * LED 配置
 * ========================================================================== */

static const ts_config_schema_entry_t s_led_schema_entries[] = {
    {
        .key = "brightness",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 128,
        .description = "全局亮度 (0-255)"
    },
    {
        .key = "power_on_effect",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "rainbow",
        .description = "开机效果"
    },
    {
        .key = "idle_effect",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "breathing",
        .description = "空闲效果"
    },
    {
        .key = "effect_speed",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 50,
        .description = "效果速度 (1-100)"
    },
    /* Matrix 配置 */
    {
        .key = "matrix.enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "是否启用 Matrix 显示"
    },
    {
        .key = "matrix.rotation",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 0,
        .description = "旋转角度 (0, 90, 180, 270)"
    },
    /* Touch Bar 配置 */
    {
        .key = "touch.enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "是否启用 Touch Bar"
    },
    {
        .key = "touch.sensitivity",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 50,
        .description = "触摸灵敏度 (1-100)"
    },
};

static const ts_config_module_schema_t s_led_schema = {
    .version = 1,
    .entries = s_led_schema_entries,
    .entry_count = sizeof(s_led_schema_entries) / sizeof(s_led_schema_entries[0]),
    .migrate = NULL,
};

/* ============================================================================
 * FAN 模块 Schema (v1)
 * 风扇配置
 * ========================================================================== */

static const ts_config_schema_entry_t s_fan_schema_entries[] = {
    {
        .key = "mode",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "auto",
        .description = "控制模式: auto, manual, curve"
    },
    {
        .key = "min_duty",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 20,
        .description = "最小占空比 (%)"
    },
    {
        .key = "max_duty",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 100,
        .description = "最大占空比 (%)"
    },
    {
        .key = "target_temp",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 45,
        .description = "目标温度 (°C)"
    },
    {
        .key = "hysteresis",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 5,
        .description = "温度滞后 (°C)"
    },
    /* 风扇曲线点 */
    {
        .key = "curve.t1",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 30,
        .description = "曲线温度点 1 (°C)"
    },
    {
        .key = "curve.d1",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 20,
        .description = "曲线占空比 1 (%)"
    },
    {
        .key = "curve.t2",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 50,
        .description = "曲线温度点 2 (°C)"
    },
    {
        .key = "curve.d2",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 60,
        .description = "曲线占空比 2 (%)"
    },
    {
        .key = "curve.t3",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 70,
        .description = "曲线温度点 3 (°C)"
    },
    {
        .key = "curve.d3",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 100,
        .description = "曲线占空比 3 (%)"
    },
};

static const ts_config_module_schema_t s_fan_schema = {
    .version = 1,
    .entries = s_fan_schema_entries,
    .entry_count = sizeof(s_fan_schema_entries) / sizeof(s_fan_schema_entries[0]),
    .migrate = NULL,
};

/* ============================================================================
 * DEVICE 模块 Schema (v1)
 * 设备控制配置
 * ========================================================================== */

static const ts_config_schema_entry_t s_device_schema_entries[] = {
    /* AGX 电源控制 */
    {
        .key = "agx.auto_power_on",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "系统启动时自动开机 AGX"
    },
    {
        .key = "agx.power_on_delay",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 2000,
        .description = "开机延迟 (ms)"
    },
    {
        .key = "agx.force_off_timeout",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 10000,
        .description = "强制关机超时 (ms)"
    },
    /* LPMU 配置 */
    {
        .key = "lpmu.auto_config",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "自动配置 LPMU"
    },
    /* USB 切换 */
    {
        .key = "usb.default_host",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "agx",
        .description = "默认 USB 主机: agx, host"
    },
    /* 监控 */
    {
        .key = "monitor.enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "是否启用设备监控"
    },
    {
        .key = "monitor.interval",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 1000,
        .description = "监控间隔 (ms)"
    },
};

static const ts_config_module_schema_t s_device_schema = {
    .version = 1,
    .entries = s_device_schema_entries,
    .entry_count = sizeof(s_device_schema_entries) / sizeof(s_device_schema_entries[0]),
    .migrate = NULL,
};

/* ============================================================================
 * SYSTEM 模块 Schema (v1)
 * 系统配置
 * ========================================================================== */

static const ts_config_schema_entry_t s_system_schema_entries[] = {
    {
        .key = "timezone",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "CST-8",
        .description = "时区设置"
    },
    {
        .key = "log_level",
        .type = TS_CONFIG_TYPE_STRING,
        .default_str = "info",
        .description = "日志级别: none, error, warn, info, debug, verbose"
    },
    {
        .key = "console.enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "是否启用串口控制台"
    },
    {
        .key = "console.baudrate",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 115200,
        .description = "控制台波特率"
    },
    {
        .key = "webui.enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "是否启用 WebUI"
    },
    {
        .key = "webui.port",
        .type = TS_CONFIG_TYPE_UINT32,
        .default_uint32 = 80,
        .description = "WebUI HTTP 端口"
    },
    {
        .key = "ota.enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = true,
        .description = "是否启用 OTA 更新"
    },
    {
        .key = "telemetry.enabled",
        .type = TS_CONFIG_TYPE_BOOL,
        .default_bool = false,
        .description = "是否启用遥测数据"
    },
};

static const ts_config_module_schema_t s_system_schema = {
    .version = 1,
    .entries = s_system_schema_entries,
    .entry_count = sizeof(s_system_schema_entries) / sizeof(s_system_schema_entries[0]),
    .migrate = NULL,
};

/* ============================================================================
 * 模块注册
 * ========================================================================== */

/**
 * @brief 初始化模块配置系统并注册所有模块
 */
esp_err_t ts_config_schemas_init(void)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "Initializing configuration module system...");
    
    /* 初始化模块系统 */
    ret = ts_config_module_system_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init module system: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 注册各模块 */
    ESP_LOGI(TAG, "Registering configuration modules...");
    
    ret = ts_config_module_register(TS_CONFIG_MODULE_NET, "ts_net", &s_net_schema);
    if (ret != ESP_OK && ret != TS_CONFIG_ERR_ALREADY_REGISTERED) {
        ESP_LOGW(TAG, "Failed to register NET module: %s", esp_err_to_name(ret));
    }
    
    ret = ts_config_module_register(TS_CONFIG_MODULE_DHCP, "ts_dhcp", &s_dhcp_schema);
    if (ret != ESP_OK && ret != TS_CONFIG_ERR_ALREADY_REGISTERED) {
        ESP_LOGW(TAG, "Failed to register DHCP module: %s", esp_err_to_name(ret));
    }
    
    ret = ts_config_module_register(TS_CONFIG_MODULE_WIFI, "ts_wifi", &s_wifi_schema);
    if (ret != ESP_OK && ret != TS_CONFIG_ERR_ALREADY_REGISTERED) {
        ESP_LOGW(TAG, "Failed to register WIFI module: %s", esp_err_to_name(ret));
    }
    
    ret = ts_config_module_register(TS_CONFIG_MODULE_NAT, "ts_nat", &s_nat_schema);
    if (ret != ESP_OK && ret != TS_CONFIG_ERR_ALREADY_REGISTERED) {
        ESP_LOGW(TAG, "Failed to register NAT module: %s", esp_err_to_name(ret));
    }
    
    ret = ts_config_module_register(TS_CONFIG_MODULE_LED, "ts_led", &s_led_schema);
    if (ret != ESP_OK && ret != TS_CONFIG_ERR_ALREADY_REGISTERED) {
        ESP_LOGW(TAG, "Failed to register LED module: %s", esp_err_to_name(ret));
    }
    
    ret = ts_config_module_register(TS_CONFIG_MODULE_FAN, "ts_fan", &s_fan_schema);
    if (ret != ESP_OK && ret != TS_CONFIG_ERR_ALREADY_REGISTERED) {
        ESP_LOGW(TAG, "Failed to register FAN module: %s", esp_err_to_name(ret));
    }
    
    ret = ts_config_module_register(TS_CONFIG_MODULE_DEVICE, "ts_device", &s_device_schema);
    if (ret != ESP_OK && ret != TS_CONFIG_ERR_ALREADY_REGISTERED) {
        ESP_LOGW(TAG, "Failed to register DEVICE module: %s", esp_err_to_name(ret));
    }
    
    ret = ts_config_module_register(TS_CONFIG_MODULE_SYSTEM, "ts_system", &s_system_schema);
    if (ret != ESP_OK && ret != TS_CONFIG_ERR_ALREADY_REGISTERED) {
        ESP_LOGW(TAG, "Failed to register SYSTEM module: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "All system configuration modules registered");
    
    /* 加载所有模块配置 */
    ESP_LOGI(TAG, "Loading module configurations...");
    ret = ts_config_module_load(TS_CONFIG_MODULE_MAX);  /* TS_CONFIG_MODULE_MAX = 加载全部 */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load system settings: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 打印元信息 */
    ts_config_meta_dump();
    
    ESP_LOGI(TAG, "Configuration module system initialized");
    return ESP_OK;
}
