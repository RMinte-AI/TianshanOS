/**
 * @file ts_time_sync.c
 * @brief TianShanOS Time Synchronization Implementation
 * 
 * 支持两种时间同步方式：
 * 1. NTP 服务器同步（主要方式，网络可用时自动启动）
 * 2. HTTP API 从浏览器同步（备用方式）
 */

#include "ts_time_sync.h"
#include "ts_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "ts_time_sync";

/* 模块状态 */
static struct {
    bool initialized;
    bool ntp_started;
    ts_time_sync_info_t info;
    char ntp_server1[64];
    char ntp_server2[64];
    char ntp_server3[64];
    char timezone[32];
} s_time_sync = {
    .initialized = false,
    .ntp_started = false,
};

static void notify_clock_updated(void)
{
    esp_err_t err = ts_event_post(TS_EVENT_BASE_TIME, TS_EVENT_TIME_SYNCED,
                                 &s_time_sync.info, sizeof(s_time_sync.info), 0);
    if (err != ESP_OK) ESP_LOGW(TAG, "Time notification lost: %s", esp_err_to_name(err));
}

/* NTP 同步回调 */
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synchronized: %lld.%06ld", 
             (long long)tv->tv_sec, (long)tv->tv_usec);
    
    s_time_sync.info.status = TS_TIME_SYNC_COMPLETED;
    s_time_sync.info.source = TS_TIME_SOURCE_NTP;
    s_time_sync.info.last_sync = tv->tv_sec;
    s_time_sync.info.sync_count++;
    
    /* 打印本地时间 */
    struct tm timeinfo;
    localtime_r(&tv->tv_sec, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Local time: %s (%s)", strftime_buf, s_time_sync.timezone);
    
    /* 发布时间同步完成事件 */
    notify_clock_updated();
}

esp_err_t ts_time_sync_init(const ts_time_sync_config_t *config)
{
    if (s_time_sync.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    /* 使用默认配置 */
    ts_time_sync_config_t default_config = TS_TIME_SYNC_CONFIG_DEFAULT();
    if (!config) {
        config = &default_config;
    }
    
    /* 保存配置 */
    if (config->ntp_server1) {
        strncpy(s_time_sync.ntp_server1, config->ntp_server1, sizeof(s_time_sync.ntp_server1) - 1);
        strncpy(s_time_sync.info.ntp_server, config->ntp_server1, sizeof(s_time_sync.info.ntp_server) - 1);
    }
    if (config->ntp_server2) {
        strncpy(s_time_sync.ntp_server2, config->ntp_server2, sizeof(s_time_sync.ntp_server2) - 1);
    }
    if (config->ntp_server3) {
        strncpy(s_time_sync.ntp_server3, config->ntp_server3, sizeof(s_time_sync.ntp_server3) - 1);
    }
    if (config->timezone) {
        strncpy(s_time_sync.timezone, config->timezone, sizeof(s_time_sync.timezone) - 1);
    }
    
    /* 设置时区 */
    setenv("TZ", s_time_sync.timezone, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", s_time_sync.timezone);
    
    s_time_sync.info.status = TS_TIME_SYNC_NOT_STARTED;
    s_time_sync.info.source = TS_TIME_SOURCE_NONE;
    s_time_sync.initialized = true;
    
    ESP_LOGI(TAG, "Time sync module initialized");
    ESP_LOGI(TAG, "  NTP server 1: %s", s_time_sync.ntp_server1);
    if (s_time_sync.ntp_server2[0]) {
        ESP_LOGI(TAG, "  NTP server 2: %s", s_time_sync.ntp_server2);
    }
    if (s_time_sync.ntp_server3[0]) {
        ESP_LOGI(TAG, "  NTP server 3: %s", s_time_sync.ntp_server3);
    }
    
    /* 自动启动 NTP */
    if (config->auto_start) {
        ts_time_sync_start_ntp();
    }
    
    return ESP_OK;
}

void ts_time_sync_deinit(void)
{
    if (!s_time_sync.initialized) {
        return;
    }
    
    ts_time_sync_stop_ntp();
    
    memset(&s_time_sync, 0, sizeof(s_time_sync));
    ESP_LOGI(TAG, "Time sync module deinitialized");
}

esp_err_t ts_time_sync_start_ntp(void)
{
    if (!s_time_sync.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_time_sync.ntp_started) {
        ESP_LOGW(TAG, "NTP already started");
        return ESP_OK;
    }
    
    if (!s_time_sync.ntp_server1[0]) {
        ESP_LOGE(TAG, "No NTP server configured");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Starting NTP sync with primary: %s, secondary: %s", 
             s_time_sync.ntp_server1, 
             s_time_sync.ntp_server2[0] ? s_time_sync.ntp_server2 : "none");
    
    /* 配置 SNTP */
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_time_sync.ntp_server1);
    sntp_config.sync_cb = time_sync_notification_cb;
    
    /* 配置多个服务器 */
    int num_servers = 1;
    sntp_config.servers[0] = s_time_sync.ntp_server1;
    
    if (s_time_sync.ntp_server2[0]) {
        sntp_config.servers[1] = s_time_sync.ntp_server2;
        num_servers = 2;
    }
    if (s_time_sync.ntp_server3[0]) {
        sntp_config.servers[2] = s_time_sync.ntp_server3;
        num_servers = 3;
    }
    sntp_config.num_of_servers = num_servers;
    
    /* 不阻塞等待同步，服务器可能在启动初期不可用 */
    sntp_config.start = true;
    sntp_config.wait_for_sync = false;
    
    esp_err_t ret = esp_netif_sntp_init(&sntp_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SNTP: %s", esp_err_to_name(ret));
        return ret;
    }
    
    s_time_sync.ntp_started = true;
    s_time_sync.info.status = TS_TIME_SYNC_IN_PROGRESS;
    
    ESP_LOGI(TAG, "NTP synchronization started (retry enabled for unreliable servers)");
    return ESP_OK;
}

void ts_time_sync_stop_ntp(void)
{
    if (!s_time_sync.ntp_started) {
        return;
    }
    
    esp_netif_sntp_deinit();
    s_time_sync.ntp_started = false;
    
    ESP_LOGI(TAG, "NTP synchronization stopped");
}

esp_err_t ts_time_sync_set_ntp_server(const char *server, int index)
{
    if (!s_time_sync.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!server || index < 0 || index > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (index == 0) {
        strncpy(s_time_sync.ntp_server1, server, sizeof(s_time_sync.ntp_server1) - 1);
        strncpy(s_time_sync.info.ntp_server, server, sizeof(s_time_sync.info.ntp_server) - 1);
    } else if (index == 1) {
        strncpy(s_time_sync.ntp_server2, server, sizeof(s_time_sync.ntp_server2) - 1);
    } else {
        strncpy(s_time_sync.ntp_server3, server, sizeof(s_time_sync.ntp_server3) - 1);
    }
    
    /* 如果 NTP 已启动，需要重启 */
    if (s_time_sync.ntp_started) {
        ts_time_sync_stop_ntp();
        ts_time_sync_start_ntp();
    }
    
    ESP_LOGI(TAG, "NTP server %d set to: %s", index, server);
    return ESP_OK;
}

esp_err_t ts_time_sync_set_timezone(const char *tz)
{
    if (!tz) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(s_time_sync.timezone, tz, sizeof(s_time_sync.timezone) - 1);
    setenv("TZ", tz, 1);
    tzset();
    
    ESP_LOGI(TAG, "Timezone set to: %s", tz);
    return ESP_OK;
}

esp_err_t ts_time_sync_set_time(int64_t timestamp_ms, ts_time_source_t source)
{
    struct timeval tv;
    tv.tv_sec = timestamp_ms / 1000;
    tv.tv_usec = (timestamp_ms % 1000) * 1000;
    
    /* 计算偏移量 */
    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
    s_time_sync.info.offset_us = (timestamp_ms - now_ms) * 1000;
    
    /* 设置系统时间 */
    int ret = settimeofday(&tv, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set time: %d", ret);
        return ESP_FAIL;
    }
    
    s_time_sync.info.status = TS_TIME_SYNC_COMPLETED;
    s_time_sync.info.source = source;
    s_time_sync.info.last_sync = tv.tv_sec;
    s_time_sync.info.sync_count++;
    
    /* 打印本地时间 */
    struct tm timeinfo;
    localtime_r(&tv.tv_sec, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    const char *source_str = (source == TS_TIME_SOURCE_HTTP) ? "HTTP/Browser" : 
                             (source == TS_TIME_SOURCE_MANUAL) ? "Manual" : "Unknown";
    ESP_LOGI(TAG, "Time set from %s: %s (offset: %lld ms)", 
             source_str, strftime_buf, (long long)(s_time_sync.info.offset_us / 1000));
    
    notify_clock_updated();
    return ESP_OK;
}

esp_err_t ts_time_sync_get_info(ts_time_sync_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(info, &s_time_sync.info, sizeof(ts_time_sync_info_t));
    return ESP_OK;
}

bool ts_time_sync_is_synced(void)
{
    return s_time_sync.info.status == TS_TIME_SYNC_COMPLETED;
}

int64_t ts_time_sync_get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

esp_err_t ts_time_sync_force_ntp(void)
{
    if (!s_time_sync.initialized || !s_time_sync.ntp_started) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* 重启 SNTP 强制同步 */
    ts_time_sync_stop_ntp();
    return ts_time_sync_start_ntp();
}

bool ts_time_sync_needs_sync(void)
{
    time_t now;
    time(&now);
    struct tm timeinfo;
    if (now == (time_t)-1 || !gmtime_r(&now, &timeinfo)) return true;
    
    /* 如果年份 < 2025，说明系统时间无效（ESP32 默认 1970） */
    return (timeinfo.tm_year + 1900) < TS_TIME_MIN_VALID_YEAR;
}
