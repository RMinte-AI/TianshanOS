/**
 * @file ts_api_cert.c
 * @brief PKI Certificate Management API Handlers
 * 
 * Provides Core API endpoints for HTTPS certificate management:
 * - cert.status: Get current PKI status
 * - cert.generate_keypair: Generate ECDSA P-256 key pair
 * - cert.generate_csr: Generate Certificate Signing Request
 * - cert.install: Install signed certificate
 * - cert.install_ca: Install CA certificate chain
 * - cert.delete: Delete all PKI credentials
 * - cert.get_csr: Get the last generated CSR
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_api.h"
#include "ts_cert.h"
#include "ts_https.h"
#include "ts_log.h"
#include <string.h>
#include <time.h>
#include <esp_heap_caps.h>

#define TAG "api_cert"

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

/**
 * @brief Convert status enum to user-friendly string
 */
static const char *status_to_display(ts_cert_status_t status)
{
    switch (status) {
        case TS_CERT_STATUS_NOT_INITIALIZED:
            return "未初始化";
        case TS_CERT_STATUS_KEY_GENERATED:
            return "密钥已生成，等待 CSR";
        case TS_CERT_STATUS_CSR_PENDING:
            return "CSR 已生成，等待签发";
        case TS_CERT_STATUS_ACTIVATED:
            return "设备证书时间有效";
        case TS_CERT_STATUS_EXPIRED:
            return "已过期";
        case TS_CERT_STATUS_TIME_UNVERIFIED: return "待校时确认";
        case TS_CERT_STATUS_NOT_YET_VALID: return "尚未生效";
        case TS_CERT_STATUS_ERROR:
            return "错误";
        default:
            return "未知";
    }
}

static void add_validity(cJSON *data, const ts_cert_info_t *info)
{
    cJSON_AddStringToObject(data, "validity", ts_cert_validity_to_str(info->validity));
    cJSON_AddBoolToObject(data, "time_ready", info->time_ready);
    cJSON_AddBoolToObject(data, "serial_truncated", info->serial_truncated);
    if (info->time_ready && info->validity != TS_CERT_VALIDITY_INVALID && info->validity != TS_CERT_VALIDITY_NONE)
        cJSON_AddNumberToObject(data, "seconds_until_expiry", (double)info->seconds_until_expiry);
    else cJSON_AddNullToObject(data, "seconds_until_expiry");
    cJSON_AddBoolToObject(data, "expires_soon", info->is_valid && info->seconds_until_expiry < 30LL * 86400);
}

static void add_runtime(cJSON *data, const ts_cert_pki_status_t *s)
{
    ts_https_runtime_t runtime;
    ts_https_get_runtime(&runtime);
    cJSON_AddBoolToObject(data, "time_ready", s->time_ready);
    cJSON_AddStringToObject(data, "validity", ts_cert_validity_to_str(s->cert_info.validity));
    cJSON_AddNumberToObject(data, "configured_generation", s->generation);
    cJSON_AddBoolToObject(data, "prerequisites_satisfied", ts_cert_prerequisites(s, runtime.require_client_cert));
    cJSON_AddBoolToObject(data, "restart_required", runtime.running && runtime.loaded_generation != s->generation);
    cJSON *blocked = cJSON_AddArrayToObject(data, "blocked_by");
#define BLOCK(condition, name) if (condition) cJSON_AddItemToArray(blocked, cJSON_CreateString(name))
    BLOCK(!s->time_ready, "time_not_ready");
    BLOCK(!s->has_private_key, "private_key_missing");
    BLOCK(s->has_private_key && !s->key_valid, "private_key_invalid");
    BLOCK(!s->has_certificate, "certificate_missing");
    BLOCK(s->has_certificate && s->cert_info.validity == TS_CERT_VALIDITY_INVALID, "certificate_invalid");
    BLOCK(s->has_certificate && s->key_valid && s->cert_info.validity != TS_CERT_VALIDITY_INVALID && !s->key_matches, "key_mismatch");
    BLOCK(s->cert_info.validity == TS_CERT_VALIDITY_NOT_YET_VALID, "not_yet_valid");
    BLOCK(s->cert_info.validity == TS_CERT_VALIDITY_EXPIRED, "expired");
    BLOCK(runtime.require_client_cert && !s->has_ca_chain, "ca_missing");
    BLOCK(runtime.require_client_cert && s->has_ca_chain && !s->ca_valid, "ca_invalid");
    BLOCK(s->storage_error, "storage_error");
#undef BLOCK
    cJSON *https = cJSON_AddObjectToObject(data, "https");
    cJSON_AddBoolToObject(https, "running", runtime.running);
    cJSON_AddNumberToObject(https, "port", runtime.port);
    if (runtime.running) {
        cJSON_AddNumberToObject(https, "loaded_generation", runtime.loaded_generation);
        cJSON_AddStringToObject(https, "loaded_certificate_sha256", runtime.loaded_certificate_sha256);
    } else {
        cJSON_AddNullToObject(https, "loaded_generation");
        cJSON_AddNullToObject(https, "loaded_certificate_sha256");
    }
    if (runtime.last_error != ESP_OK) {
        cJSON_AddStringToObject(https, "last_error_stage", runtime.last_error_stage);
        cJSON_AddStringToObject(https, "last_error", esp_err_to_name(runtime.last_error));
    } else {
        cJSON_AddNullToObject(https, "last_error_stage"); cJSON_AddNullToObject(https, "last_error");
    }
}

static esp_err_t install_error(ts_api_result_t *result, esp_err_t err, ts_cert_op_error_t detail)
{
    ts_api_result_error(result, err == ESP_ERR_NO_MEM ? TS_API_ERR_NO_MEM :
        err == ESP_ERR_INVALID_ARG ? TS_API_ERR_INVALID_ARG : TS_API_ERR_INTERNAL,
        ts_cert_op_error_to_str(detail));
    return err;
}

/*===========================================================================*/
/*                          API Handlers                                      */
/*===========================================================================*/

/**
 * @brief cert.status - Get PKI certificate status
 * 
 * Returns:
 * {
 *   "status": "activated",
 *   "status_display": "已激活",
 *   "has_private_key": true,
 *   "has_certificate": true,
 *   "has_ca_chain": true,
 *   "cert_info": {
 *     "subject_cn": "TIANSHAN-RM01-0001",
 *     "issuer_cn": "TianShanOS CA",
 *     "not_before": 1737705600,
 *     "not_after": 1769241600,
 *     "serial": "01:23:45:67:89",
 *     "is_valid": true,
 *     "days_until_expiry": 365
 *   }
 * }
 */
static esp_err_t api_cert_status(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    TS_LOGI(TAG, "API: cert.status called");
    
    ts_cert_pki_status_t pki_status;
    esp_err_t ret = ts_cert_get_status(&pki_status);
    
    if (ret != ESP_OK) {
        ts_api_result_error(result, TS_API_ERR_INTERNAL, 
            "Failed to get PKI status");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }
    
    // 基本状态
    cJSON_AddStringToObject(data, "status", ts_cert_status_to_str(pki_status.status));
    cJSON_AddStringToObject(data, "status_display", status_to_display(pki_status.status));
    cJSON_AddBoolToObject(data, "has_private_key", pki_status.has_private_key);
    cJSON_AddBoolToObject(data, "has_certificate", pki_status.has_certificate);
    cJSON_AddBoolToObject(data, "has_ca_chain", pki_status.has_ca_chain);
    
    add_runtime(data, &pki_status);

    // 如果有证书，添加证书信息
    if (pki_status.has_certificate) {
        cJSON *cert_info = cJSON_CreateObject();
        cJSON_AddStringToObject(cert_info, "subject_cn", pki_status.cert_info.subject_cn);
        cJSON_AddStringToObject(cert_info, "issuer_cn", pki_status.cert_info.issuer_cn);
        cJSON_AddNumberToObject(cert_info, "not_before", (double)pki_status.cert_info.not_before);
        cJSON_AddNumberToObject(cert_info, "not_after", (double)pki_status.cert_info.not_after);
        cJSON_AddStringToObject(cert_info, "serial", pki_status.cert_info.serial);
        cJSON_AddBoolToObject(cert_info, "is_valid", pki_status.cert_info.is_valid);
        cJSON_AddNumberToObject(cert_info, "days_until_expiry", pki_status.cert_info.days_until_expiry);
        
        add_validity(cert_info, &pki_status.cert_info);

        cJSON_AddItemToObject(data, "cert_info", cert_info);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief cert.generate_keypair - Generate ECDSA P-256 key pair
 * 
 * Params: none
 * 
 * Returns:
 * {
 *   "success": true,
 *   "message": "Key pair generated successfully"
 * }
 */
static esp_err_t api_cert_generate_keypair(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    TS_LOGI(TAG, "API: cert.generate_keypair called");
    
    // 检查是否已有密钥
    if (ts_cert_has_keypair()) {
        // 不自动覆盖，需要先删除
        const cJSON *force = cJSON_GetObjectItem(params, "force");
        if (!force || !cJSON_IsTrue(force)) {
            ts_api_result_error(result, TS_API_ERR_INVALID_ARG,
                "Key pair already exists. Use force=true to overwrite.");
            return ESP_ERR_INVALID_STATE;
        }
        TS_LOGW(TAG, "Overwriting existing key pair");
    }
    
    esp_err_t ret = ts_cert_generate_keypair();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to generate key pair: %s", esp_err_to_name(ret));
        ts_api_result_error(result, TS_API_ERR_INTERNAL,
            "Failed to generate key pair");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", true);
    cJSON_AddStringToObject(data, "message", "ECDSA P-256 key pair generated successfully");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief cert.generate_csr - Generate Certificate Signing Request
 * 
 * Params:
 * {
 *   "device_id": "TIANSHAN-RM01-0001",  // optional, uses config default
 *   "organization": "HiddenPeak Labs",   // optional
 *   "org_unit": "Device"                 // optional
 * }
 * 
 * Returns:
 * {
 *   "success": true,
 *   "csr_pem": "-----BEGIN CERTIFICATE REQUEST-----\n..."
 * }
 */
static esp_err_t api_cert_generate_csr(const cJSON *params, ts_api_result_t *result)
{
    TS_LOGI(TAG, "API: cert.generate_csr called");
    
    // 检查是否有密钥
    if (!ts_cert_has_keypair()) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG,
            "No private key exists. Generate key pair first.");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 分配 CSR 缓冲区（从 PSRAM）
    char *csr_pem = heap_caps_malloc(TS_CERT_CSR_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!csr_pem) {
        csr_pem = malloc(TS_CERT_CSR_MAX_LEN);
        if (!csr_pem) {
            ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    
    size_t csr_len = TS_CERT_CSR_MAX_LEN;
    esp_err_t ret;
    
    // 如果提供了参数，使用自定义选项
    const cJSON *device_id_obj = cJSON_GetObjectItem(params, "device_id");
    const cJSON *org_obj = cJSON_GetObjectItem(params, "organization");
    const cJSON *ou_obj = cJSON_GetObjectItem(params, "org_unit");
    
    if (device_id_obj || org_obj || ou_obj) {
        ts_cert_csr_opts_t opts = {0};
        opts.device_id = cJSON_IsString(device_id_obj) ? device_id_obj->valuestring : NULL;
        opts.organization = cJSON_IsString(org_obj) ? org_obj->valuestring : NULL;
        opts.org_unit = cJSON_IsString(ou_obj) ? ou_obj->valuestring : NULL;
        
        ret = ts_cert_generate_csr(&opts, csr_pem, &csr_len);
    } else {
        // 使用默认选项
        ret = ts_cert_generate_csr_default(csr_pem, &csr_len);
    }
    
    if (ret != ESP_OK) {
        free(csr_pem);
        TS_LOGE(TAG, "Failed to generate CSR: %s", esp_err_to_name(ret));
        ts_api_result_error(result, TS_API_ERR_INTERNAL, 
            "Failed to generate CSR");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", true);
    cJSON_AddStringToObject(data, "csr_pem", csr_pem);
    
    free(csr_pem);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief cert.install - Install signed certificate
 * 
 * Params:
 * {
 *   "cert_pem": "-----BEGIN CERTIFICATE-----\n..."
 * }
 */
static esp_err_t api_cert_install(const cJSON *params, ts_api_result_t *result)
{
    TS_LOGI(TAG, "API: cert.install called");
    
    const cJSON *cert_obj = cJSON_GetObjectItem(params, "cert_pem");
    if (!cert_obj || !cJSON_IsString(cert_obj)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG,
            "Missing required parameter: cert_pem");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *cert_pem = cert_obj->valuestring;
    size_t cert_len = strlen(cert_pem) + 1;
    
    ts_cert_op_error_t detail;
    esp_err_t ret = ts_cert_install_certificate_ex(cert_pem, cert_len, &detail);
    if (ret != ESP_OK) return install_error(result, ret, detail);

    // 获取安装后的证书信息
    ts_cert_info_t info;
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", true);
    cJSON_AddStringToObject(data, "message", "Certificate installed successfully");
    
    if (ts_cert_get_info(&info) == ESP_OK) {
        cJSON *cert_info = cJSON_CreateObject();
        cJSON_AddStringToObject(cert_info, "subject_cn", info.subject_cn);
        cJSON_AddStringToObject(cert_info, "issuer_cn", info.issuer_cn);
        cJSON_AddNumberToObject(cert_info, "not_before", (double)info.not_before);
        cJSON_AddNumberToObject(cert_info, "not_after", (double)info.not_after);
        cJSON_AddNumberToObject(cert_info, "days_until_expiry", info.days_until_expiry);
        add_validity(cert_info, &info);
        cJSON_AddItemToObject(data, "cert_info", cert_info);
    }
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief cert.install_ca - Install CA certificate chain
 * 
 * Params:
 * {
 *   "ca_pem": "-----BEGIN CERTIFICATE-----\n..."
 * }
 */
static esp_err_t api_cert_install_ca(const cJSON *params, ts_api_result_t *result)
{
    TS_LOGI(TAG, "API: cert.install_ca called");
    
    const cJSON *ca_obj = cJSON_GetObjectItem(params, "ca_pem");
    if (!ca_obj || !cJSON_IsString(ca_obj)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG,
            "Missing required parameter: ca_pem");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *ca_pem = ca_obj->valuestring;
    size_t ca_len = strlen(ca_pem) + 1;
    
    ts_cert_op_error_t detail;
    esp_err_t ret = ts_cert_install_ca_chain_ex(ca_pem, ca_len, &detail);
    if (ret != ESP_OK) return install_error(result, ret, detail);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", true);
    cJSON_AddStringToObject(data, "message", "CA chain installed successfully");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief cert.delete - Delete all PKI credentials
 * 
 * WARNING: This removes private key, certificate and CA chain.
 */
static esp_err_t api_cert_delete(const cJSON *params, ts_api_result_t *result)
{
    TS_LOGW(TAG, "API: cert.delete called - factory reset");
    
    // 需要确认参数
    const cJSON *confirm = cJSON_GetObjectItem(params, "confirm");
    if (!confirm || !cJSON_IsTrue(confirm)) {
        ts_api_result_error(result, TS_API_ERR_INVALID_ARG,
            "Missing confirm=true parameter. This will delete all PKI credentials.");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ts_cert_factory_reset();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to delete PKI credentials: %s", esp_err_to_name(ret));
        ts_api_result_error(result, TS_API_ERR_INTERNAL,
            "Failed to delete PKI credentials");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "success", true);
    cJSON_AddStringToObject(data, "message", "All PKI credentials deleted");
    
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/**
 * @brief cert.get_certificate - Get the device certificate (PEM)
 */
static esp_err_t api_cert_get_certificate(const cJSON *params, ts_api_result_t *result)
{
    (void)params;
    
    TS_LOGI(TAG, "API: cert.get_certificate called");
    
    char *cert_pem = heap_caps_malloc(TS_CERT_PEM_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cert_pem) {
        cert_pem = malloc(TS_CERT_PEM_MAX_LEN);
        if (!cert_pem) {
            ts_api_result_error(result, TS_API_ERR_NO_MEM, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    
    size_t cert_len = TS_CERT_PEM_MAX_LEN;
    esp_err_t ret = ts_cert_get_certificate(cert_pem, &cert_len);
    
    if (ret == ESP_ERR_NOT_FOUND) {
        free(cert_pem);
        ts_api_result_error(result, TS_API_ERR_NOT_FOUND,
            "No certificate installed");
        return ret;
    }
    
    if (ret != ESP_OK) {
        free(cert_pem);
        ts_api_result_error(result, TS_API_ERR_INTERNAL,
            "Failed to get certificate");
        return ret;
    }
    
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "cert_pem", cert_pem);
    
    free(cert_pem);
    ts_api_result_ok(result, data);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_api_cert_register(void)
{
    static const ts_api_endpoint_t cert_apis[] = {
        {
            .name = "cert.status",
            .description = "Get PKI certificate status",
            .category = TS_API_CAT_SECURITY,
            .handler = api_cert_status,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "cert.generate_keypair",
            .description = "Generate ECDSA P-256 key pair",
            .category = TS_API_CAT_SECURITY,
            .handler = api_cert_generate_keypair,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "cert.generate_csr",
            .description = "Generate Certificate Signing Request",
            .category = TS_API_CAT_SECURITY,
            .handler = api_cert_generate_csr,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "cert.install",
            .description = "Install signed certificate",
            .category = TS_API_CAT_SECURITY,
            .handler = api_cert_install,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "cert.install_ca",
            .description = "Install CA certificate chain",
            .category = TS_API_CAT_SECURITY,
            .handler = api_cert_install_ca,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "cert.delete",
            .description = "Delete all PKI credentials",
            .category = TS_API_CAT_SECURITY,
            .handler = api_cert_delete,
            .requires_auth = false,
            .permission = NULL
        },
        {
            .name = "cert.get_certificate",
            .description = "Get device certificate PEM",
            .category = TS_API_CAT_SECURITY,
            .handler = api_cert_get_certificate,
            .requires_auth = false,
            .permission = NULL
        }
    };
    
    esp_err_t ret = ts_api_register_multiple(cert_apis, 
        sizeof(cert_apis) / sizeof(cert_apis[0]));
    
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Certificate APIs registered");
    }
    
    return ret;
}
