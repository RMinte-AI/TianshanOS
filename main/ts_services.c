/**
 * @file ts_services.c
 * @brief TianShanOS Service Registration
 *
 * 服务注册和初始化
 * 
 * 此文件将各个组件(storage, console等)包装成服务，
 * 并在系统启动时注册到服务管理器。
 *
 * @author TianShanOS Team
 * @version 0.1.0
 */

#include "ts_services.h"
#include "ts_service.h"
#include "ts_event.h"
#include "ts_storage.h"
#include "ts_console.h"
#include "ts_cmd_all.h"
#include "ts_hal.h"
#include "ts_led.h"
#include "ts_led_preset.h"
#include "ts_drivers.h"
#include "ts_fan.h"
#include "ts_net_manager.h"
#include "ts_dhcp_server.h"
#include "ts_time_sync.h"
#include "ts_security.h"
#include "ts_keystore.h"
#include "ts_known_hosts.h"
#include "ts_cert.h"
#include "ts_pki_client.h"
#include "ts_https.h"
#include "ts_https_api.h"
#include "ts_api.h"
#include "ts_webui.h"
#include "ts_power_monitor.h"
#include "ts_power_policy.h"
#include "ts_automation.h"
#include "ts_device_ctrl.h"
#include "ts_config_file.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ts_https_retry.h"

static const char *TAG = "ts_services";

/* ============================================================================
 * 服务句柄
 * ========================================================================== */

static ts_service_handle_t s_hal_handle = NULL;
static ts_service_handle_t s_storage_handle = NULL;
static ts_service_handle_t s_led_handle = NULL;
static ts_service_handle_t s_drivers_handle = NULL;
static ts_service_handle_t s_power_handle = NULL;
static ts_service_handle_t s_network_handle = NULL;
static ts_service_handle_t s_security_handle = NULL;
static ts_service_handle_t s_api_handle = NULL;
static ts_service_handle_t s_https_handle = NULL;
static ts_service_handle_t s_webui_handle = NULL;
static ts_service_handle_t s_console_handle = NULL;
static ts_service_handle_t s_automation_handle = NULL;

/* ============================================================================
 * HAL 服务回调
 * ========================================================================== */

static esp_err_t hal_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing HAL service...");
    
    ts_hal_config_t config = TS_HAL_CONFIG_DEFAULT();
    esp_err_t ret = ts_hal_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init HAL: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

static esp_err_t hal_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    return ESP_OK;  // HAL 在 init 时已启动
}

static esp_err_t hal_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    return ts_hal_deinit();
}

static bool hal_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    return ts_hal_is_initialized();
}

/* ============================================================================
 * Storage 服务回调
 * ========================================================================== */

static esp_err_t storage_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing storage service...");
    
    esp_err_t ret = ts_storage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init storage: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

// Forward declaration for recovery check
extern esp_err_t ts_ota_check_recovery(void);

static esp_err_t storage_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Starting storage service...");
    
    // 挂载 SPIFFS
    ts_spiffs_config_t spiffs_config = TS_SPIFFS_DEFAULT_CONFIG();
    esp_err_t ret = ts_storage_mount_spiffs(&spiffs_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        // 继续，SPIFFS 不是必须的
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");
    }
    
    // 挂载 SD 卡 - 传 NULL 使用 Kconfig 默认 GPIO 配置
    // 已添加任务看门狗保护，超时后会安全返回
    ret = ts_storage_mount_sd(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(ret));
        ESP_LOGI(TAG, "Use 'storage --mount' to try again after inserting card");
    } else {
        ESP_LOGI(TAG, "SD card mounted at /sdcard");
        
        // ====== SD 卡 Recovery 检查 ======
        // 检查 /sdcard/recovery/ 目录，如果存在且版本不同则自动恢复
        // 这是最低级别的恢复机制，即使 OTA 完全损坏也能工作
        ret = ts_ota_check_recovery();
        if (ret == ESP_OK) {
            // 如果执行了恢复，函数不会返回（会重启）
            // 返回 ESP_OK 表示无需恢复
            ESP_LOGI(TAG, "Recovery check passed");
        } else if (ret != ESP_ERR_NOT_FOUND) {
            // 恢复失败（不是"未找到"错误）
            ESP_LOGE(TAG, "Recovery check failed: %s", esp_err_to_name(ret));
        }
        // ESP_ERR_NOT_FOUND 表示没有 recovery 目录，正常情况
    }
    
    return ESP_OK;
}

static esp_err_t storage_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Stopping storage service...");
    
    ts_storage_unmount_sd();
    ts_storage_unmount_spiffs();
    
    return ESP_OK;
}

static bool storage_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    // 检查至少一个存储是否可用
    return ts_storage_spiffs_mounted() || ts_storage_sd_mounted();
}

/* ============================================================================
 * LED 服务回调
 * ========================================================================== */

static esp_err_t led_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing LED service...");
    
    esp_err_t ret = ts_led_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LED: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

static esp_err_t led_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Starting LED service...");
    
    // 初始化预设 LED 设备 (touch, board, matrix)
    esp_err_t ret = ts_led_preset_init_all();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Some LED devices failed to init: %s", esp_err_to_name(ret));
        // 不是致命错误，继续
    }
    
    // 加载并应用保存的启动配置
    ts_led_load_all_boot_config();
    
    return ESP_OK;
}

static esp_err_t led_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    return ESP_OK;  // LED 不需要特别停止
}

static bool led_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    return true;  // LED 始终可用
}

/* ============================================================================
 * Drivers 服务回调
 * ========================================================================== */

static esp_err_t drivers_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing drivers service...");
    
    esp_err_t ret = ts_drivers_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Some drivers failed to init: %s", esp_err_to_name(ret));
        // 不是致命错误
    }
    
    return ESP_OK;
}

static esp_err_t drivers_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    // 加载风扇配置
    ts_fan_load_config();
    
    return ESP_OK;
}

static esp_err_t drivers_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    return ts_drivers_deinit();
}

static bool drivers_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    return true;
}

/* ============================================================================
 * Power 服务回调 (电源监控 + 电压保护)
 * ========================================================================== */

static esp_err_t power_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing power service...");
    
    /* 初始化电压保护策略 */
    esp_err_t ret = ts_power_policy_init(NULL);  // 使用默认配置
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init power policy: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 初始化电源监控（不自动启动，由 start 阶段启动）*/
    ts_power_monitor_config_t pm_config;
    ts_power_monitor_get_default_config(&pm_config);
    pm_config.auto_start_monitoring = false;  // 禁用自动启动
    
    ret = ts_power_monitor_init(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init power monitor: %s", esp_err_to_name(ret));
        /* 电源监控失败不影响系统运行，继续 */
    }
    
    return ESP_OK;
}

static esp_err_t power_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Starting power service...");
    
    /* 启动电源监控 */
    esp_err_t ret = ts_power_monitor_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start power monitor: %s", esp_err_to_name(ret));
        /* 不是致命错误 */
    }
    
    /* 根据保存的配置决定是否启动电压保护策略 */
    if (ts_power_policy_should_auto_start()) {
        ret = ts_power_policy_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start power policy: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Power protection started (auto-start enabled)");
        }
    } else {
        ESP_LOGI(TAG, "Power protection skipped (auto-start disabled in config)");
    }
    
    return ESP_OK;
}

static esp_err_t power_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Stopping power service...");
    
    ts_power_policy_stop();
    ts_power_monitor_stop();
    
    return ESP_OK;
}

static bool power_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    /* 检查电源监控是否正在运行 */
    return ts_power_policy_is_running();
}

/* ============================================================================
 * Network 服务回调
 * ========================================================================== */

static esp_err_t network_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing network service...");
    
    esp_err_t ret = ts_net_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init network manager: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 初始化 DHCP 服务器 */
    ret = ts_dhcp_server_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init DHCP server: %s", esp_err_to_name(ret));
        /* 不是致命错误，继续 */
    }
    
    return ESP_OK;
}

static esp_err_t network_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Starting network service...");
    
    /* 启动以太网 */
    esp_err_t ret = ts_net_manager_start(TS_NET_IF_ETH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start Ethernet: %s", esp_err_to_name(ret));
        /* 不是致命错误，继续 */
    }
    
    /* 初始化时间同步（NTP） */
    ts_time_sync_config_t time_config = {
        .ntp_server1 = "10.10.99.99",    /* 本地 NTP 服务器（首选） */
        .ntp_server2 = "10.10.99.98",    /* 本地 NTP 服务器（备用 1） */
        .ntp_server3 = "10.10.99.100",   /* 本地 NTP 服务器（备用 2） */
        .timezone = "CST-8",            /* 中国标准时间 */
        .sync_interval_ms = 3600000,    /* 每小时同步一次 */
        .auto_start = true,             /* 自动启动 NTP */
    };
    ret = ts_time_sync_init(&time_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init time sync: %s", esp_err_to_name(ret));
        /* 不是致命错误，继续 */
    }
    
    /* 启动 LPMU 开机状态检测（异步任务）*/
    ret = ts_device_lpmu_start_detection();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start LPMU detection: %s", esp_err_to_name(ret));
    }
    
    return ESP_OK;
}

static esp_err_t network_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Stopping network service...");
    
    ts_net_manager_stop(TS_NET_IF_ETH);
    ts_net_manager_stop(TS_NET_IF_WIFI_STA);
    
    return ESP_OK;
}

static bool network_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    /* 检查是否有任意网络接口可用 */
    return ts_net_manager_is_ready(TS_NET_IF_ETH) || 
           ts_net_manager_is_ready(TS_NET_IF_WIFI_STA);
}

/* ============================================================================
 * Security 服务回调
 * ========================================================================== */

/**
 * @brief PKI 自动注册进度回调
 */
static void pki_enroll_callback(ts_pki_enroll_status_t status, 
                                 const char *message, 
                                 void *user_data)
{
    (void)user_data;
    
    switch (status) {
        case TS_PKI_ENROLL_PENDING:
            ESP_LOGI(TAG, "PKI: %s", message);
            break;
        case TS_PKI_ENROLL_APPROVED:
            ESP_LOGI(TAG, "PKI: Certificate enrollment complete!");
            break;
        case TS_PKI_ENROLL_REJECTED:
            ESP_LOGW(TAG, "PKI: CSR was rejected by admin");
            break;
        case TS_PKI_ENROLL_ERROR:
            ESP_LOGE(TAG, "PKI: Enrollment error - %s", message);
            break;
        default:
            break;
    }
}

static esp_err_t security_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing security service...");
    
    /* 初始化安全子系统 */
    esp_err_t ret = ts_security_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init security: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 初始化密钥存储 */
    ret = ts_keystore_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init keystore: %s", esp_err_to_name(ret));
        /* 不是致命错误，继续 */
    }
    
    /* 初始化已知主机管理 */
    ret = ts_known_hosts_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init known hosts: %s", esp_err_to_name(ret));
        /* 不是致命错误，继续 */
    }
    
    /* 初始化 PKI 证书管理 */
    ret = ts_cert_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init cert manager: %s", esp_err_to_name(ret));
        /* 不是致命错误，继续 */
    }
    
    /* 初始化 PKI 自动注册客户端 */
    ts_pki_client_config_t pki_config;
    ts_pki_client_get_default_config(&pki_config);
    pki_config.auto_start = false;  /* 在 service_start 时启动 */
    
    ret = ts_pki_client_init_with_config(&pki_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init PKI client: %s", esp_err_to_name(ret));
        /* 不是致命错误，继续 */
    }
    
    return ESP_OK;
}

static esp_err_t security_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    /* 检查当前证书状态 */
    ts_cert_pki_status_t cert_status;
    esp_err_t ret = ts_cert_get_status(&cert_status);
    
    if (ret == ESP_OK && (!cert_status.has_certificate || cert_status.cert_info.validity == TS_CERT_VALIDITY_EXPIRED)) {
        /* 缺证书或确实过期才自动注册；待校时/尚未生效不触发替换。 */
        ESP_LOGI(TAG, "No valid certificate, starting auto-enrollment...");
        ret = ts_pki_client_start_auto_enroll(pki_enroll_callback, NULL);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start auto-enrollment: %s", esp_err_to_name(ret));
        }
    } else if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Certificate status: %s, time validity: %s",
                 ts_cert_status_to_str(cert_status.status),
                 ts_cert_validity_to_str(cert_status.cert_info.validity));
    }
    
    /* 证书已就绪，现在加载加密的配置文件 (.tscfg) */
    ret = ts_config_file_load_encrypted();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load encrypted configs: %s", esp_err_to_name(ret));
        /* 不是致命错误，继续 */
    }
    
    return ESP_OK;  /* Security 在 init 时已启动 */
}

static esp_err_t security_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ts_pki_client_deinit();
    ts_cert_deinit();
    ts_known_hosts_deinit();
    ts_keystore_deinit();
    ts_security_deinit();
    
    return ESP_OK;
}

static bool security_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    return ts_keystore_is_initialized();
}

/* ============================================================================
 * API 服务回调
 * ========================================================================== */

static esp_err_t api_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing API service...");
    
    esp_err_t ret = ts_api_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init API layer: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

static esp_err_t api_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Starting API service...");
    
    /* 注册所有 API 端点 */
    esp_err_t ret = ts_api_register_all();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register APIs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

static esp_err_t api_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Stopping API service...");
    return ts_api_deinit();
}

static bool api_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    /* API 层只要初始化成功就一直可用 */
    return true;
}

/* ============================================================================
 * HTTPS 服务回调 (mTLS) - 事件驱动方式
 * ========================================================================== */

/* TLS lifecycle has exactly one owner. Events only wake it; no event stack TLS work. */
static struct {
    TaskHandle_t task;
    ts_event_handler_handle_t time_handler, material_handler;
    bool desired;
    uint32_t control, completed;
    esp_err_t stop_result;
} s_https_state;
static portMUX_TYPE s_https_control_lock = portMUX_INITIALIZER_UNLOCKED;

static void https_conditions_changed(const ts_event_t *event, void *user_data)
{
    (void)event; (void)user_data;
    portENTER_CRITICAL(&s_https_control_lock);
    TaskHandle_t task = s_https_state.task;
    portEXIT_CRITICAL(&s_https_control_lock);
    if (task) xTaskNotifyGive(task);
}

static esp_err_t https_cleanup(void)
{
    esp_err_t ret = ts_https_stop();
    if (ret == ESP_OK) ts_https_deinit();
    return ret;
}

static void https_coordinator(void *arg)
{
    (void)arg;
    ts_https_retry_t retry = {0};
    for (;;) {
        bool desired;
        uint32_t control, completed;
        portENTER_CRITICAL(&s_https_control_lock);
        desired = s_https_state.desired;
        control = s_https_state.control;
        completed = s_https_state.completed;
        portEXIT_CRITICAL(&s_https_control_lock);
        if (!desired) {
            if (completed != control) {
                esp_err_t ret = https_cleanup();
                portENTER_CRITICAL(&s_https_control_lock);
                s_https_state.stop_result = ret;
                s_https_state.completed = control;
                portEXIT_CRITICAL(&s_https_control_lock);
            }
        } else if (!ts_https_is_running()) {
            ts_cert_pki_status_t status = {0};
            ts_https_config_t config = TS_HTTPS_CONFIG_DEFAULT();
            esp_err_t ret = ts_cert_get_status(&status);
            bool ready = ret == ESP_OK && ts_cert_prerequisites(&status, config.require_client_cert);
            int64_t now = esp_timer_get_time() / 1000;
            if (ts_https_retry_due(&retry, status.generation, control, true, ready, now)) {
                const char *stage = "cleanup";
                ret = https_cleanup();
                if (ret == ESP_OK) { stage = "init"; ret = ts_https_init(&config); }
                if (ret == ESP_OK) { stage = "default_endpoints"; ret = ts_https_register_default_api(); }
                /* Stop intent supersedes this attempt before starting the listener. */
                portENTER_CRITICAL(&s_https_control_lock);
                bool current = s_https_state.desired && control == s_https_state.control;
                portEXIT_CRITICAL(&s_https_control_lock);
                if (ret == ESP_OK && current) { stage = "start"; ret = ts_https_start(); }
                if (ret != ESP_OK || !current) {
                    esp_err_t cleanup = https_cleanup();
                    if (cleanup != ESP_OK) { stage = "cleanup"; ret = cleanup; }
                    if (ret != ESP_OK) {
                        ts_https_record_error(stage, ret);
                        ts_https_retry_failed(&retry, esp_timer_get_time() / 1000);
                    }
                }
                /* A stop arriving inside TLS start is serviced before its acknowledgement. */
                portENTER_CRITICAL(&s_https_control_lock);
                current = s_https_state.desired && control == s_https_state.control;
                portEXIT_CRITICAL(&s_https_control_lock);
                if (!current) continue;
            }
        }
        uint32_t wait_ms = 5000;
        int64_t remaining = retry.due_ms - esp_timer_get_time() / 1000;
        if (desired && retry.failures > 0 && retry.failures < 4 && remaining > 0 && remaining < wait_ms)
            wait_ms = (uint32_t)remaining;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

static void https_unregister(ts_event_handler_handle_t *handler)
{
    if (!*handler) return;
    esp_err_t ret = ts_event_unregister(*handler);
    if (ret == ESP_OK) *handler = NULL;
    else ESP_LOGW(TAG, "HTTPS subscription cleanup: %s", esp_err_to_name(ret));
}

static esp_err_t https_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle; (void)user_data;
    if (s_https_state.task) return ESP_OK;
    esp_err_t ret = ESP_OK;
    if (!s_https_state.time_handler)
        ret = ts_event_register(TS_EVENT_BASE_TIME, TS_EVENT_TIME_SYNCED,
                https_conditions_changed, NULL, &s_https_state.time_handler);
    if (ret != ESP_OK) return ret;
    if (!s_https_state.material_handler)
        ret = ts_event_register(TS_EVENT_BASE_PKI, TS_EVENT_PKI_MATERIAL_CHANGED,
                https_conditions_changed, NULL, &s_https_state.material_handler);
    if (ret == ESP_OK) {
        /* Internal stack: TLS and material reads may touch Flash. Not a PSRAM stack. */
        TaskHandle_t task;
        if (xTaskCreate(https_coordinator, "https_coord", 8192, NULL, 5, &task) == pdPASS) {
            portENTER_CRITICAL(&s_https_control_lock);
            s_https_state.task = task;
            portEXIT_CRITICAL(&s_https_control_lock);
            return ESP_OK;
        }
        ret = ESP_ERR_NO_MEM;
    }
    https_unregister(&s_https_state.material_handler);
    https_unregister(&s_https_state.time_handler);
    return ret;
}

static esp_err_t https_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle; (void)user_data;
    if (!s_https_state.task) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_https_control_lock);
    s_https_state.desired = true;
    ++s_https_state.control;
    portEXIT_CRITICAL(&s_https_control_lock);
    xTaskNotifyGive(s_https_state.task);
    return ESP_OK; /* Scheduled; actual listening is reported by ts_https_get_runtime(). */
}

static esp_err_t https_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle; (void)user_data;
    if (!s_https_state.task) return ESP_OK;
    portENTER_CRITICAL(&s_https_control_lock);
    s_https_state.desired = false;
    uint32_t control = ++s_https_state.control;
    portEXIT_CRITICAL(&s_https_control_lock);
    xTaskNotifyGive(s_https_state.task);
    for (unsigned i = 0; i < 300; ++i) {
        portENTER_CRITICAL(&s_https_control_lock);
        bool done = s_https_state.completed == control;
        esp_err_t ret = s_https_state.stop_result;
        portEXIT_CRITICAL(&s_https_control_lock);
        if (done) return ret;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ts_https_record_error("stop_timeout", ESP_ERR_TIMEOUT);
    return ESP_ERR_TIMEOUT;
}

static bool https_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle; (void)user_data;
    return ts_https_is_running();
}

/* ============================================================================
 * Console 服务回调
 * ========================================================================== */

static esp_err_t console_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing console service...");
    
    ts_console_config_t config = TS_CONSOLE_DEFAULT_CONFIG();
    config.prompt = "TianShanOS> ";
    
    esp_err_t ret = ts_console_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init console: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 注册内置命令 (help, version, reboot 等)
    ret = ts_console_register_builtin_cmds();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register builtin commands: %s", esp_err_to_name(ret));
        // 不是致命错误，继续
    }
    
    // 注册所有扩展命令 (system, service, config, fan, storage, net, device, led)
    ret = ts_cmd_register_all();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Some extended commands failed to register");
        // 不是致命错误，继续
    }
    
    return ESP_OK;
}

static esp_err_t console_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Starting console service...");
    
    esp_err_t ret = ts_console_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start console: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

static esp_err_t console_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Stopping console service...");
    return ts_console_stop();
}

static bool console_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    return ts_console_is_running();
}

/* ============================================================================
 * WebUI 服务回调
 * ========================================================================== */

#include "esp_spiffs.h"

static esp_err_t webui_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing WebUI service...");
    
    /* 挂载 www SPIFFS 分区（WebUI 静态文件） */
    esp_vfs_spiffs_conf_t www_conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 5,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&www_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount www partition: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "WebUI static files will not be available");
        /* 继续，让 HTTP 服务器启动，API 仍可用 */
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("www", &total, &used);
        ESP_LOGI(TAG, "Mounted www partition at /www (%u/%u bytes)", used, total);
    }
    
    ret = ts_webui_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init WebUI: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

static esp_err_t webui_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Starting WebUI service...");
    
    esp_err_t ret = ts_webui_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebUI: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WebUI server started on port 80");
    return ESP_OK;
}

static esp_err_t webui_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Stopping WebUI service...");
    return ts_webui_stop();
}

static bool webui_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    return ts_webui_is_running();
}

/* ============================================================================
 * Automation 服务回调
 * ========================================================================== */

static esp_err_t automation_service_init(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Initializing automation service...");
    
    /* 使用默认配置初始化（不自动启动） */
    ts_automation_config_t config = TS_AUTOMATION_CONFIG_DEFAULT();
    config.auto_start = false;  /* 在 service_start 时启动 */
    
    esp_err_t ret = ts_automation_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init automation: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

static esp_err_t automation_service_start(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Starting automation service...");
    
    esp_err_t ret = ts_automation_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start automation: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ts_automation_status_t status;
    ts_automation_get_status(&status);
    ESP_LOGI(TAG, "Automation engine running: %d sources, %d rules, %d variables",
             status.sources_count, status.rules_count, status.variables_count);
    
    return ESP_OK;
}

static esp_err_t automation_service_stop(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ESP_LOGI(TAG, "Stopping automation service...");
    return ts_automation_stop();
}

static bool automation_service_health(ts_service_handle_t handle, void *user_data)
{
    (void)handle;
    (void)user_data;
    
    ts_automation_status_t status;
    if (ts_automation_get_status(&status) != ESP_OK) {
        return false;
    }
    return status.state == TS_AUTO_STATE_RUNNING;
}

/* ============================================================================
 * 服务定义
 * ========================================================================== */

static const ts_service_def_t s_hal_service_def = {
    .name = "hal",
    .phase = TS_SERVICE_PHASE_HAL,
    .capabilities = 0,
    .dependencies = {NULL},
    .init = hal_service_init,
    .start = hal_service_start,
    .stop = hal_service_stop,
    .health_check = hal_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_storage_service_def = {
    .name = "storage",
    .phase = TS_SERVICE_PHASE_DRIVER,
    .capabilities = TS_SERVICE_CAP_RESTARTABLE | TS_SERVICE_CAP_CONFIGURABLE,
    .dependencies = {"hal", NULL},  // 依赖 HAL
    .init = storage_service_init,
    .start = storage_service_start,
    .stop = storage_service_stop,
    .health_check = storage_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_led_service_def = {
    .name = "led",
    .phase = TS_SERVICE_PHASE_DRIVER,
    .capabilities = TS_SERVICE_CAP_RESTARTABLE,
    .dependencies = {"hal", NULL},  // 不依赖 storage，在加载图像时容错处理
    .init = led_service_init,
    .start = led_service_start,
    .stop = led_service_stop,
    .health_check = led_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_drivers_service_def = {
    .name = "drivers",
    .phase = TS_SERVICE_PHASE_DRIVER,
    .capabilities = TS_SERVICE_CAP_RESTARTABLE,
    .dependencies = {"hal", NULL},
    .init = drivers_service_init,
    .start = drivers_service_start,
    .stop = drivers_service_stop,
    .health_check = drivers_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_power_service_def = {
    .name = "power",
    .phase = TS_SERVICE_PHASE_DRIVER,
    .capabilities = TS_SERVICE_CAP_RESTARTABLE | TS_SERVICE_CAP_CONFIGURABLE,
    .dependencies = {"hal", NULL},  // 只依赖 HAL (ADC)
    .init = power_service_init,
    .start = power_service_start,
    .stop = power_service_stop,
    .health_check = power_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_network_service_def = {
    .name = "network",
    .phase = TS_SERVICE_PHASE_NETWORK,
    .capabilities = TS_SERVICE_CAP_RESTARTABLE | TS_SERVICE_CAP_CONFIGURABLE,
    .dependencies = {"hal", "storage", NULL},  // 依赖 HAL 和 storage
    .init = network_service_init,
    .start = network_service_start,
    .stop = network_service_stop,
    .health_check = network_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_security_service_def = {
    .name = "security",
    .phase = TS_SERVICE_PHASE_SECURITY,
    .capabilities = 0,
    .dependencies = {"storage", NULL},  /* 依赖 storage（密钥可能从 SD 卡导入） */
    .init = security_service_init,
    .start = security_service_start,
    .stop = security_service_stop,
    .health_check = security_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_api_service_def = {
    .name = "api",
    .phase = TS_SERVICE_PHASE_SERVICE,
    .capabilities = 0,
    .dependencies = {"storage", "drivers", "network", "security", NULL},  /* 依赖所有硬件服务 */
    .init = api_service_init,
    .start = api_service_start,
    .stop = api_service_stop,
    .health_check = api_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_https_service_def = {
    .name = "https",
    .phase = TS_SERVICE_PHASE_SERVICE,
    .capabilities = TS_SERVICE_CAP_RESTARTABLE,
    .dependencies = {"security", "network", NULL},  /* 依赖安全（证书）和网络 */
    .init = https_service_init,
    .start = https_service_start,
    .stop = https_service_stop,
    .health_check = https_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_webui_service_def = {
    .name = "webui",
    .phase = TS_SERVICE_PHASE_UI,
    .capabilities = TS_SERVICE_CAP_RESTARTABLE,
    .dependencies = {"api", "network", "storage", NULL},  /* 依赖 API、网络和存储（静态文件） */
    .init = webui_service_init,
    .start = webui_service_start,
    .stop = webui_service_stop,
    .health_check = webui_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_console_service_def = {
    .name = "console",
    .phase = TS_SERVICE_PHASE_UI,
    .capabilities = TS_SERVICE_CAP_RESTARTABLE,
    .dependencies = {"api", NULL},  /* Console 只依赖 API 服务 */
    .init = console_service_init,
    .start = console_service_start,
    .stop = console_service_stop,
    .health_check = console_service_health,
    .user_data = NULL,
};

static const ts_service_def_t s_automation_service_def = {
    .name = "automation",
    .phase = TS_SERVICE_PHASE_SERVICE,
    .capabilities = TS_SERVICE_CAP_RESTARTABLE | TS_SERVICE_CAP_CONFIGURABLE,
    .dependencies = {"storage", "hal", "network", NULL},  /* 依赖 storage、HAL 和 network */
    .init = automation_service_init,
    .start = automation_service_start,
    .stop = automation_service_stop,
    .health_check = automation_service_health,
    .user_data = NULL,
};

/* ============================================================================
 * 公开 API
 * ========================================================================== */

esp_err_t ts_services_register_all(void)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "Registering services...");
    
    // 注册 HAL 服务 (最先)
    ret = ts_service_register(&s_hal_service_def, &s_hal_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register HAL service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - hal service registered");
    
    // 注册 storage 服务
    ret = ts_service_register(&s_storage_service_def, &s_storage_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register storage service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - storage service registered");
    
    // 注册 LED 服务
    ret = ts_service_register(&s_led_service_def, &s_led_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register LED service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - led service registered");
    
    // 注册 Drivers 服务
    ret = ts_service_register(&s_drivers_service_def, &s_drivers_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register drivers service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - drivers service registered");
    
    // 注册 Power 服务 (电源监控 + 电压保护)
    ret = ts_service_register(&s_power_service_def, &s_power_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register power service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - power service registered");
    
    // 注册 network 服务
    ret = ts_service_register(&s_network_service_def, &s_network_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register network service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - network service registered");
    
    // 注册 security 服务
    ret = ts_service_register(&s_security_service_def, &s_security_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register security service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - security service registered");
    
    // 注册 API 服务
    ret = ts_service_register(&s_api_service_def, &s_api_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register API service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - api service registered");
    
    // 注册 HTTPS 服务 (mTLS)
    ret = ts_service_register(&s_https_service_def, &s_https_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register HTTPS service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - https service registered");
    
    // 注册 WebUI 服务
    ret = ts_service_register(&s_webui_service_def, &s_webui_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebUI service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - webui service registered");
    
    // 注册 console 服务 (最后)
    ret = ts_service_register(&s_console_service_def, &s_console_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register console service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - console service registered");
    
    // 注册 automation 服务
    ret = ts_service_register(&s_automation_service_def, &s_automation_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register automation service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  - automation service registered");
    
    ESP_LOGI(TAG, "All core services registered");
    
    return ESP_OK;
}
