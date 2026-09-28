/**
 * @file ts_pki_client.c
 * @brief TianShanOS PKI Auto-Enrollment Client Implementation
 *
 * ESP32 与 PKI 服务器通信的客户端实现:
 * - CSR 提交 (POST /api/csr)
 * - 状态轮询 (GET /api/csr/{device_id})
 * - 证书下载 (GET /api/csr/{device_id}/certificate)
 * - CA 链下载 (GET /api/ca/chain)
 *
 * @copyright Copyright (c) 2026 TianShanOS Project
 */

#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "ts_pki_client.h"
#include "ts_cert.h"

static const char *TAG = "pki_client";

/*===========================================================================*/
/*                          Private Data                                      */
/*===========================================================================*/

/** Client state */
static struct {
    bool initialized;
    ts_pki_client_config_t config;
    char device_id[TS_CERT_DEVICE_ID_MAX_LEN];
    int request_id;  // CSR 提交后返回的请求 ID
    TaskHandle_t enroll_task;
    _Atomic bool enroll_running;
    SemaphoreHandle_t mutex;
    _Atomic bool stop_requested;
    ts_pki_enroll_callback_t callback;
    void *callback_data;
} s_client = {0};

/** HTTP response buffer */
#define HTTP_RESP_BUF_SIZE 8192
static char *s_http_resp_buf = NULL;
static size_t s_http_resp_len = 0;

/*===========================================================================*/
/*                       Forward Declarations                                 */
/*===========================================================================*/

static esp_err_t http_event_handler(esp_http_client_event_t *evt);
static esp_err_t do_http_request(const char *url, esp_http_client_method_t method,
                                  const char *post_data, int *http_code);
static void auto_enroll_task(void *arg);
static esp_err_t get_device_id(void);

/*===========================================================================*/
/*                         Initialization                                     */
/*===========================================================================*/

void ts_pki_client_get_default_config(ts_pki_client_config_t *config)
{
    if (!config) return;
    
    memset(config, 0, sizeof(*config));
    // 生产内网 PKI 服务器地址（也是出厂后用户网络中的地址）
    strncpy(config->server_host, "10.10.99.100", sizeof(config->server_host) - 1);
    config->server_port = TS_PKI_DEFAULT_PORT;
    config->use_https = false;  // PKI 服务器目前用 HTTP
    config->poll_interval_sec = TS_PKI_POLL_INTERVAL_SEC;
    config->max_poll_attempts = TS_PKI_MAX_POLL_ATTEMPTS;
    config->auto_start = false;
}

esp_err_t ts_pki_client_init(void)
{
    ts_pki_client_config_t config;
    ts_pki_client_get_default_config(&config);
    return ts_pki_client_init_with_config(&config);
}

esp_err_t ts_pki_client_init_with_config(const ts_pki_client_config_t *config)
{
    if (s_client.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }
    
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 分配 HTTP 响应缓冲区
    s_http_resp_buf = heap_caps_malloc(HTTP_RESP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_http_resp_buf) {
        s_http_resp_buf = malloc(HTTP_RESP_BUF_SIZE);
        if (!s_http_resp_buf) {
            ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // 创建互斥锁
    s_client.mutex = xSemaphoreCreateMutex();
    if (!s_client.mutex) {
        free(s_http_resp_buf);
        s_http_resp_buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // 复制配置
    memcpy(&s_client.config, config, sizeof(ts_pki_client_config_t));
    s_client.initialized = true;
    s_client.stop_requested = false;
    
    // 获取设备 ID
    esp_err_t ret = get_device_id();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not get device ID, will use MAC address");
    }
    
    ESP_LOGI(TAG, "PKI client initialized, server=%s:%d, device_id=%s",
             s_client.config.server_host, s_client.config.server_port, 
             s_client.device_id);
    
    // 如果配置了自动启动
    if (config->auto_start) {
        return ts_pki_client_start_auto_enroll(NULL, NULL);
    }
    
    return ESP_OK;
}

void ts_pki_client_deinit(void)
{
    if (!s_client.initialized) return;
    
    // 停止后台任务
    ts_pki_client_stop_auto_enroll();
    
    if (s_client.enroll_running) { ESP_LOGW(TAG, "Enrollment still exiting; retaining resources"); return; }

    // 释放资源
    if (s_client.mutex) {
        vSemaphoreDelete(s_client.mutex);
        s_client.mutex = NULL;
    }
    
    if (s_http_resp_buf) {
        free(s_http_resp_buf);
        s_http_resp_buf = NULL;
    }
    
    memset(&s_client, 0, sizeof(s_client));
    ESP_LOGI(TAG, "PKI client deinitialized");
}

/*===========================================================================*/
/*                          Server Configuration                              */
/*===========================================================================*/

esp_err_t ts_pki_client_set_server(const char *host, uint16_t port)
{
    if (!s_client.initialized || !host) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(s_client.mutex, portMAX_DELAY);
    strncpy(s_client.config.server_host, host, sizeof(s_client.config.server_host) - 1);
    s_client.config.server_port = port;
    xSemaphoreGive(s_client.mutex);
    
    ESP_LOGI(TAG, "PKI server set to %s:%d", host, port);
    return ESP_OK;
}

esp_err_t ts_pki_client_get_server(char *host, uint16_t *port)
{
    if (!s_client.initialized || !host || !port) {
        return ESP_ERR_INVALID_STATE;
    }
    
    xSemaphoreTake(s_client.mutex, portMAX_DELAY);
    strcpy(host, s_client.config.server_host);
    *port = s_client.config.server_port;
    xSemaphoreGive(s_client.mutex);
    
    return ESP_OK;
}

/*===========================================================================*/
/*                      Manual Enrollment Operations                          */
/*===========================================================================*/

esp_err_t ts_pki_client_submit_csr(void)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Submitting CSR for device: %s", s_client.device_id);
    
    // 检查是否已有有效证书
    ts_cert_pki_status_t cert_status;
    esp_err_t ret = ts_cert_get_status(&cert_status);
    if (ret == ESP_OK && (cert_status.has_certificate && cert_status.has_private_key &&
        cert_status.cert_info.validity != TS_CERT_VALIDITY_EXPIRED)) {
        ESP_LOGI(TAG, "Stored certificate retained; no automatic replacement");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 确保有密钥对
    if (!ts_cert_has_keypair()) {
        ESP_LOGI(TAG, "Generating new key pair...");
        ret = ts_cert_generate_keypair();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to generate key pair: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    // 生成 CSR
    char *csr_pem = heap_caps_malloc(TS_CERT_CSR_MAX_LEN, MALLOC_CAP_SPIRAM);
    if (!csr_pem) {
        csr_pem = malloc(TS_CERT_CSR_MAX_LEN);
        if (!csr_pem) {
            return ESP_ERR_NO_MEM;
        }
    }
    
    size_t csr_len = TS_CERT_CSR_MAX_LEN;
    ret = ts_cert_generate_csr_default(csr_pem, &csr_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate CSR: %s", esp_err_to_name(ret));
        free(csr_pem);
        return ret;
    }
    
    ESP_LOGI(TAG, "CSR generated, length=%zu", csr_len);
    
    // 构建 JSON 请求体
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", s_client.device_id);
    cJSON_AddStringToObject(root, "csr_pem", csr_pem);
    cJSON_AddStringToObject(root, "device_type", "ESP32-S3");
    cJSON_AddStringToObject(root, "device_ip", "10.10.99.97");  // 设备静态 IP
    cJSON_AddStringToObject(root, "description", "TianShanOS Rack Manager");
    // 建议的 SAN IP，管理员可在审批时修改
    cJSON *san_ips = cJSON_CreateArray();
    cJSON_AddItemToArray(san_ips, cJSON_CreateString("10.10.99.97"));
    cJSON_AddItemToObject(root, "suggested_san_ips", san_ips);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(csr_pem);
    
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }
    
    // 发送 HTTP 请求到 /api/csr/submit
    char url[256];
    snprintf(url, sizeof(url), "http%s://%s:%d/api/csr/submit",
             s_client.config.use_https ? "s" : "",
             s_client.config.server_host,
             s_client.config.server_port);
    
    int http_code = 0;
    ret = do_http_request(url, HTTP_METHOD_POST, json_str, &http_code);
    free(json_str);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (http_code == 200 || http_code == 201) {
        // 解析响应获取 request_id
        cJSON *resp = cJSON_Parse(s_http_resp_buf);
        if (resp) {
            cJSON *req_id = cJSON_GetObjectItem(resp, "request_id");
            if (req_id && cJSON_IsNumber(req_id)) {
                s_client.request_id = req_id->valueint;
                ESP_LOGI(TAG, "CSR submitted successfully, request_id=%d", s_client.request_id);
            }
            // 检查是否自动签发（设备在白名单且启用自动签发）
            cJSON *status = cJSON_GetObjectItem(resp, "status");
            cJSON *cert = cJSON_GetObjectItem(resp, "certificate");
            if (status && cJSON_IsString(status) && 
                strcmp(status->valuestring, "approved") == 0 &&
                cert && cJSON_IsString(cert)) {
                ESP_LOGI(TAG, "Certificate auto-issued!");
                cJSON *ca_chain = cJSON_GetObjectItem(resp, "ca_chain");
                // 安装证书 (mbedtls 需要包含 null 终止符)
                ret = ts_cert_install_certificate(cert->valuestring, strlen(cert->valuestring) + 1);
                // 安装 CA 链
                if (ret == ESP_OK && ca_chain && cJSON_IsString(ca_chain)) {
                    ret = ts_cert_install_ca_chain(ca_chain->valuestring, strlen(ca_chain->valuestring) + 1);
                }
                if (ret == ESP_OK) {
                    ts_cert_pki_status_t installed;
                    ret = ts_cert_get_status(&installed);
                    if (ret == ESP_OK && (!installed.has_ca_chain || !installed.ca_valid)) ret = ESP_ERR_INVALID_STATE;
                }
                if (ret != ESP_OK) { cJSON_Delete(resp); return ret; }
            }
            cJSON_Delete(resp);
        }
        return ESP_OK;
    } else if (http_code == 409) {
        ESP_LOGW(TAG, "CSR already exists for this device");
        return ESP_OK;  // 已存在也算成功
    } else {
        ESP_LOGE(TAG, "Server returned error: %d", http_code);
        return ESP_FAIL;
    }
}

esp_err_t ts_pki_client_check_status(ts_pki_enroll_status_t *status)
{
    if (!s_client.initialized || !status) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_client.request_id <= 0) {
        *status = TS_PKI_ENROLL_NOT_FOUND;
        return ESP_OK;
    }
    
    char url[256];
    snprintf(url, sizeof(url), "http%s://%s:%d/api/csr/status/%d",
             s_client.config.use_https ? "s" : "",
             s_client.config.server_host,
             s_client.config.server_port,
             s_client.request_id);
    
    int http_code = 0;
    esp_err_t ret = do_http_request(url, HTTP_METHOD_GET, NULL, &http_code);
    
    if (ret != ESP_OK) {
        *status = TS_PKI_ENROLL_ERROR;
        return ret;
    }
    
    if (http_code == 404) {
        *status = TS_PKI_ENROLL_NOT_FOUND;
        return ESP_OK;
    }
    
    if (http_code != 200) {
        *status = TS_PKI_ENROLL_ERROR;
        return ESP_OK;
    }
    
    // 解析 JSON 响应
    cJSON *root = cJSON_Parse(s_http_resp_buf);
    if (!root) {
        *status = TS_PKI_ENROLL_ERROR;
        return ESP_OK;
    }
    
    cJSON *status_obj = cJSON_GetObjectItem(root, "status");
    if (status_obj && cJSON_IsString(status_obj)) {
        const char *status_str = status_obj->valuestring;
        if (strcmp(status_str, "approved") == 0) {
            *status = TS_PKI_ENROLL_APPROVED;
        } else if (strcmp(status_str, "rejected") == 0) {
            *status = TS_PKI_ENROLL_REJECTED;
        } else if (strcmp(status_str, "pending") == 0) {
            *status = TS_PKI_ENROLL_PENDING;
        } else {
            *status = TS_PKI_ENROLL_ERROR;
        }
    } else {
        *status = TS_PKI_ENROLL_ERROR;
    }
    
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ts_pki_client_install_certificate(void)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_client.request_id <= 0) {
        ESP_LOGE(TAG, "No request_id, submit CSR first");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Downloading certificate for request_id: %d", s_client.request_id);
    
    // 从 /api/csr/status/{request_id} 获取证书
    char url[256];
    snprintf(url, sizeof(url), "http%s://%s:%d/api/csr/status/%d",
             s_client.config.use_https ? "s" : "",
             s_client.config.server_host,
             s_client.config.server_port,
             s_client.request_id);
    
    int http_code = 0;
    esp_err_t ret = do_http_request(url, HTTP_METHOD_GET, NULL, &http_code);
    
    if (ret != ESP_OK || http_code != 200) {
        ESP_LOGE(TAG, "Failed to get certificate: http_code=%d", http_code);
        return ESP_FAIL;
    }
    
    // 解析 JSON 响应
    cJSON *root = cJSON_Parse(s_http_resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse certificate response");
        return ESP_FAIL;
    }
    
    // 检查状态
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!status || !cJSON_IsString(status) || strcmp(status->valuestring, "approved") != 0) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Certificate not approved yet");
        return ESP_ERR_INVALID_STATE;
    }
    
    cJSON *cert_pem = cJSON_GetObjectItem(root, "certificate");
    if (!cert_pem || !cJSON_IsString(cert_pem)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "No certificate in response");
        return ESP_FAIL;
    }
    
    // 获取 CA 链（如果有）
    cJSON *ca_chain = cJSON_GetObjectItem(root, "ca_chain");
    const char *ca_chain_str = (ca_chain && cJSON_IsString(ca_chain)) ? ca_chain->valuestring : NULL;
    
    // 安装证书 (mbedtls 需要包含 null 终止符)
    ret = ts_cert_install_certificate(cert_pem->valuestring, strlen(cert_pem->valuestring) + 1);
    
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to install certificate: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 安装 CA 链
    if (ca_chain_str) {
        ret = ts_cert_install_ca_chain(ca_chain_str, strlen(ca_chain_str) + 1);
        if (ret != ESP_OK) { cJSON_Delete(root); return ret; }
    } else {
        ts_cert_pki_status_t installed;
        ret = ts_cert_get_status(&installed);
        if (ret != ESP_OK || (!installed.has_ca_chain || !installed.ca_valid)) {
            cJSON_Delete(root); return ESP_ERR_INVALID_STATE;
        }
    }

    cJSON_Delete(root);
    
    ESP_LOGI(TAG, "Device certificate installed successfully");
    
    return ESP_OK;
}

esp_err_t ts_pki_client_enroll_blocking(uint32_t timeout_sec)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // 提交 CSR
    esp_err_t ret = ts_pki_client_submit_csr();
    if (ret != ESP_OK) {
        return ret;
    }
    
    // 计算超时
    uint32_t max_attempts = timeout_sec ? 
        (timeout_sec / s_client.config.poll_interval_sec) :
        s_client.config.max_poll_attempts;
    
    // 轮询等待批准
    uint32_t attempts = 0;
    ts_pki_enroll_status_t status;
    
    while (attempts < max_attempts) {
        vTaskDelay(pdMS_TO_TICKS(s_client.config.poll_interval_sec * 1000));
        
        ret = ts_pki_client_check_status(&status);
        if (ret != ESP_OK) {
            attempts++;
            continue;
        }
        
        switch (status) {
            case TS_PKI_ENROLL_APPROVED:
                ESP_LOGI(TAG, "CSR approved! Installing certificate...");
                return ts_pki_client_install_certificate();
                
            case TS_PKI_ENROLL_REJECTED:
                ESP_LOGE(TAG, "CSR was rejected");
                return ESP_FAIL;
                
            case TS_PKI_ENROLL_PENDING:
                ESP_LOGI(TAG, "Waiting for approval... (%lu/%lu)", 
                         (unsigned long)attempts, (unsigned long)max_attempts);
                break;
                
            default:
                ESP_LOGW(TAG, "Unexpected status: %d", status);
                break;
        }
        
        attempts++;
    }
    
    ESP_LOGE(TAG, "Enrollment timed out");
    return ESP_ERR_TIMEOUT;
}

/*===========================================================================*/
/*                      Background Enrollment Service                         */
/*===========================================================================*/

esp_err_t ts_pki_client_start_auto_enroll(ts_pki_enroll_callback_t callback,
                                           void *user_data)
{
    if (!s_client.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_client.enroll_running, &expected, true)) {
        ESP_LOGW(TAG, "Auto-enroll already running");
        return ESP_OK;
    }
    
    s_client.callback = callback;
    s_client.callback_data = user_data;
    s_client.stop_requested = false;
    
    /* MUST use DRAM stack - PKI operations access NVS (SPI Flash).
     * SPI Flash operations disable cache, and PSRAM access requires cache.
     */
    BaseType_t ret = xTaskCreate(auto_enroll_task, "pki_enroll", 8192,
                                  NULL, 5, &s_client.enroll_task);
    if (ret != pdPASS) {
        s_client.enroll_running = false;
        ESP_LOGE(TAG, "Failed to create enroll task");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Auto-enrollment started");
    return ESP_OK;
}

void ts_pki_client_stop_auto_enroll(void)
{
    if (!s_client.enroll_running) return;
    
    s_client.stop_requested = true;
    
    // 等待任务退出
    for (int i = 0; i < 50; i++) {
        if (!s_client.enroll_running) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (s_client.enroll_running) {
        ESP_LOGW(TAG, "Enrollment cancellation pending; task retains its resources");
        return;
    }
    
    ESP_LOGI(TAG, "Auto-enrollment stopped");
}

bool ts_pki_client_is_enrolling(void)
{
    return s_client.enroll_running;
}

/*===========================================================================*/
/*                          Utility Functions                                 */
/*===========================================================================*/

const char *ts_pki_enroll_status_to_str(ts_pki_enroll_status_t status)
{
    switch (status) {
        case TS_PKI_ENROLL_PENDING:    return "PENDING";
        case TS_PKI_ENROLL_APPROVED:   return "APPROVED";
        case TS_PKI_ENROLL_REJECTED:   return "REJECTED";
        case TS_PKI_ENROLL_NOT_FOUND:  return "NOT_FOUND";
        case TS_PKI_ENROLL_ERROR:      return "ERROR";
        default:                       return "UNKNOWN";
    }
}

bool ts_pki_client_is_server_reachable(void)
{
    if (!s_client.initialized) return false;
    
    char url[128];
    snprintf(url, sizeof(url), "http%s://%s:%d/api/health",
             s_client.config.use_https ? "s" : "",
             s_client.config.server_host,
             s_client.config.server_port);
    
    int http_code = 0;
    esp_err_t ret = do_http_request(url, HTTP_METHOD_GET, NULL, &http_code);
    
    return (ret == ESP_OK && http_code == 200);
}

/*===========================================================================*/
/*                          Private Functions                                 */
/*===========================================================================*/

static esp_err_t get_device_id(void)
{
    // 尝试从证书状态获取设备 ID
    ts_cert_pki_status_t status;
    if (ts_cert_get_status(&status) == ESP_OK && 
        status.cert_info.subject_cn[0] != '\0') {
        strncpy(s_client.device_id, status.cert_info.subject_cn, 
                sizeof(s_client.device_id) - 1);
        return ESP_OK;
    }
    
    // 使用 MAC 地址生成设备 ID
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_client.device_id, sizeof(s_client.device_id),
             "TIANSHAN-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (s_http_resp_len + evt->data_len < HTTP_RESP_BUF_SIZE - 1) {
                    memcpy(s_http_resp_buf + s_http_resp_len, evt->data, evt->data_len);
                    s_http_resp_len += evt->data_len;
                    s_http_resp_buf[s_http_resp_len] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t do_http_request(const char *url, esp_http_client_method_t method,
                                  const char *post_data, int *http_code)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .event_handler = http_event_handler,
        .timeout_ms = TS_PKI_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    
    // 如果使用 HTTPS，配置证书验证
    if (s_client.config.use_https) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    
    // 设置 Content-Type
    if (post_data) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }
    
    // 清空响应缓冲区
    s_http_resp_len = 0;
    s_http_resp_buf[0] = '\0';
    
    // 执行请求
    esp_err_t ret = esp_http_client_perform(client);
    
    if (ret == ESP_OK) {
        *http_code = esp_http_client_get_status_code(client);
        ESP_LOGD(TAG, "HTTP %s %s => %d", 
                 method == HTTP_METHOD_POST ? "POST" : "GET",
                 url, *http_code);
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(ret));
    }
    
    esp_http_client_cleanup(client);
    return ret;
}

static void enroll_wait(uint32_t ms)
{
    while (ms && !s_client.stop_requested) {
        uint32_t step = ms < 100 ? ms : 100;
        vTaskDelay(pdMS_TO_TICKS(step)); ms -= step;
    }
}

static void auto_enroll_task(void *arg)
{
    ESP_LOGI(TAG, "Auto-enrollment task started");
    
    // 等待网络就绪
    enroll_wait(5000);
    
    if (s_client.stop_requested) goto done;

    // 检查是否已有有效证书
    ts_cert_pki_status_t cert_status;
    if (ts_cert_get_status(&cert_status) == ESP_OK && 
        (cert_status.has_certificate && cert_status.has_private_key &&
        cert_status.cert_info.validity != TS_CERT_VALIDITY_EXPIRED)) {
        ESP_LOGI(TAG, "Stored certificate retained; checking readiness");
        if (s_client.callback) {
            s_client.callback(ts_cert_prerequisites(&cert_status, true) ? TS_PKI_ENROLL_APPROVED : TS_PKI_ENROLL_PENDING,
                              "Credentials already stored; check time and CA readiness",
                              s_client.callback_data);
        }
        s_client.enroll_running = false;
        vTaskDelete(NULL);
        return;
    }
    
    // 等待 PKI 服务器可达
    int retry = 0;
    while (!s_client.stop_requested && retry < 30) {
        if (ts_pki_client_is_server_reachable()) {
            ESP_LOGI(TAG, "PKI server is reachable");
            break;
        }
        ESP_LOGW(TAG, "PKI server not reachable, retrying...");
        enroll_wait(10000);
        retry++;
    }
    
    if (s_client.stop_requested) {
        s_client.enroll_running = false;
        vTaskDelete(NULL);
        return;
    }
    
    // 提交 CSR
    esp_err_t ret = ts_pki_client_submit_csr();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to submit CSR: %s", esp_err_to_name(ret));
        if (s_client.callback) {
            s_client.callback(TS_PKI_ENROLL_ERROR, "CSR submission failed",
                              s_client.callback_data);
        }
        // 重试
        enroll_wait(60000);
    }
    
    if (s_client.callback) {
        s_client.callback(TS_PKI_ENROLL_PENDING, "CSR submitted, awaiting approval",
                          s_client.callback_data);
    }
    
    // 轮询等待批准
    uint32_t attempts = 0;
    ts_pki_enroll_status_t status;
    
    while (!s_client.stop_requested) {
        if (s_client.config.max_poll_attempts > 0 && 
            attempts >= s_client.config.max_poll_attempts) {
            ESP_LOGE(TAG, "Max poll attempts reached");
            if (s_client.callback) {
                s_client.callback(TS_PKI_ENROLL_ERROR, "Enrollment timed out",
                                  s_client.callback_data);
            }
            break;
        }
        
        enroll_wait(s_client.config.poll_interval_sec * 1000);
        
        if (s_client.stop_requested) break;
        ret = ts_pki_client_check_status(&status);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to check status, retrying...");
            attempts++;
            continue;
        }
        
        switch (status) {
            case TS_PKI_ENROLL_APPROVED:
                ESP_LOGI(TAG, "CSR approved!");
                ret = ts_pki_client_install_certificate();
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Certificate installed successfully!");
                    if (s_client.callback) {
                        s_client.callback(TS_PKI_ENROLL_APPROVED, 
                                          "Enrollment complete",
                                          s_client.callback_data);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to install certificate");
                    if (s_client.callback) {
                        s_client.callback(TS_PKI_ENROLL_ERROR,
                                          "Certificate installation failed",
                                          s_client.callback_data);
                    }
                }
                goto done;
                
            case TS_PKI_ENROLL_REJECTED:
                ESP_LOGE(TAG, "CSR was rejected");
                if (s_client.callback) {
                    s_client.callback(TS_PKI_ENROLL_REJECTED, 
                                      "CSR rejected by admin",
                                      s_client.callback_data);
                }
                goto done;
                
            case TS_PKI_ENROLL_PENDING:
                ESP_LOGD(TAG, "Still pending... (%lu attempts)", 
                         (unsigned long)attempts);
                break;
                
            case TS_PKI_ENROLL_NOT_FOUND:
                ESP_LOGW(TAG, "CSR not found, resubmitting...");
                ts_pki_client_submit_csr();
                break;
                
            default:
                break;
        }
        
        attempts++;
    }
    
done:
    ESP_LOGI(TAG, "Auto-enrollment task finished");
    s_client.enroll_running = false;
    vTaskDelete(NULL);
}
