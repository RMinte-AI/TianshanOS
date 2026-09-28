/**
 * @file ts_cert.c
 * @brief TianShanOS PKI Certificate Management Implementation
 * 
 * 实现 X.509 证书和 CSR 操作，包括：
 * - ECDSA P-256 密钥对生成与存储
 * - CSR 生成（带 SAN IP 扩展）
 * - 证书安装与验证
 * - NVS 持久化存储
 * - CA 链保存到 SD 卡（供用户下载信任）
 * 
 * 内存分配优先使用 PSRAM
 */

#include "ts_cert.h"
#include "ts_cert_time.h"
#include "ts_event.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include <ctype.h>
#include "ts_crypto.h"
#include "ts_core.h"
#include "ts_time_sync.h"

#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/oid.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/error.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

static esp_err_t ts_cert_init_locked(void);
static void ts_cert_deinit_locked(void);
static esp_err_t ts_cert_generate_keypair_locked(void);
static bool ts_cert_has_keypair_locked(void);
static esp_err_t ts_cert_delete_keypair_locked(void);
static esp_err_t ts_cert_generate_csr_locked(const ts_cert_csr_opts_t *opts,
                                char *csr_pem, size_t *csr_len);
static esp_err_t ts_cert_generate_csr_default_locked(char *csr_pem, size_t *csr_len);
static esp_err_t ts_cert_install_certificate_ex_locked(const char *pem, size_t len, ts_cert_op_error_t *detail);
static esp_err_t ts_cert_install_ca_chain_ex_locked(const char *pem, size_t len, ts_cert_op_error_t *detail);
static esp_err_t ts_cert_install_certificate_locked(const char *pem, size_t len);
static esp_err_t ts_cert_install_ca_chain_locked(const char *pem, size_t len);
static esp_err_t ts_cert_get_certificate_locked(char *cert_pem, size_t *cert_len);
static esp_err_t ts_cert_get_private_key_locked(char *key_pem, size_t *key_len);
static esp_err_t ts_cert_get_ca_chain_locked(char *ca_chain_pem, size_t *ca_chain_len);
static esp_err_t ts_cert_refresh_status_locked(void);
static esp_err_t ts_cert_get_status_locked(ts_cert_pki_status_t *status);
static esp_err_t ts_cert_get_info_locked(ts_cert_info_t *info);
static bool ts_cert_is_valid_locked(void);
static int ts_cert_days_until_expiry_locked(void);
static esp_err_t ts_cert_factory_reset_locked(void);
static esp_err_t ts_cert_get_snapshot_locked(bool require_ca, ts_cert_snapshot_t *snapshot);
static const char *TAG = "ts_cert";

/*===========================================================================*/
/*                              NVS Keys                                      */
/*===========================================================================*/

#define NVS_NAMESPACE           "ts_pki"
#define NVS_KEY_PRIVKEY         "privkey"
#define NVS_KEY_CERT            "cert"
#define NVS_KEY_CA_CHAIN        "ca_chain"
#define NVS_KEY_STATUS          "status"

/** CA chain file path on SD card for user download */
#define CA_CHAIN_SDCARD_PATH    "/sdcard/pki/ca-chain.crt"
#define CA_CHAIN_SDCARD_DIR     "/sdcard/pki"

/*===========================================================================*/
/*                          Static Variables                                  */
/*===========================================================================*/

/* Initialized once during boot, retained across deinit to protect late readers. */
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static void material_lock(void)
{
    portENTER_CRITICAL(&s_mutex_init_lock);
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    portEXIT_CRITICAL(&s_mutex_init_lock);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}
static uint32_t s_generation = 1, s_metadata_generation;
static ts_cert_info_t s_metadata;
static bool s_key_valid, s_key_matches, s_ca_valid, s_storage_error;
static char s_fingerprint[65];
static void refresh_metadata(void);
static void evaluate_time(ts_cert_info_t *info, int64_t now);
static esp_err_t info_from_crt(const mbedtls_x509_crt *crt, ts_cert_info_t *info);

static bool s_initialized = false;
static nvs_handle_t s_nvs_handle = 0;

/* Cached credentials (loaded on init) */
static char *s_private_key_pem = NULL;
static char *s_certificate_pem = NULL;
static char *s_ca_chain_pem = NULL;
static ts_cert_status_t s_status = TS_CERT_STATUS_NOT_INITIALIZED;

/* RNG context */
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_ctr_drbg;
static bool s_rng_initialized = false;

/*===========================================================================*/
/*                          Internal Functions                                */
/*===========================================================================*/

/* mbedTLS can combine a high-level error with a low-level allocation error. */
static bool allocation_error(int error)
{
    if (error >= 0) return false;
    int high = (-error) & 0xFF80;
    int low = (-error) & 0x007F;
    return high == -MBEDTLS_ERR_X509_ALLOC_FAILED || high == -MBEDTLS_ERR_PK_ALLOC_FAILED ||
           high == -MBEDTLS_ERR_ECP_ALLOC_FAILED || low == -MBEDTLS_ERR_MPI_ALLOC_FAILED ||
           low == -MBEDTLS_ERR_ASN1_ALLOC_FAILED;
}

static esp_err_t init_rng(void)
{
    if (s_rng_initialized) return ESP_OK;
    
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctr_drbg);
    
    const char *pers = "ts_cert_csr";
    int ret = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func, &s_entropy,
                                     (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        char err_buf[128];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "RNG seed failed: %s", err_buf);
        mbedtls_ctr_drbg_free(&s_ctr_drbg);
        mbedtls_entropy_free(&s_entropy);
        return allocation_error(ret) ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    
    s_rng_initialized = true;
    return ESP_OK;
}

static esp_err_t nvs_read_string(const char *key, char **out_str)
{
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(s_nvs_handle, key, NULL, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_str = NULL;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    
    *out_str = heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*out_str) {
        *out_str = malloc(required_size);  /* Fallback to DRAM */
    }
    if (!*out_str) return ESP_ERR_NO_MEM;
    
    err = nvs_get_str(s_nvs_handle, key, *out_str, &required_size);
    if (err != ESP_OK) { free(*out_str); *out_str = NULL; }
    return err;
}

static esp_err_t nvs_write_string(const char *key, const char *str)
{
    const char *stage = "set";
    esp_err_t err = nvs_set_str(s_nvs_handle, key, str);
    if (err == ESP_OK) { stage = "commit"; err = nvs_commit(s_nvs_handle); }
    if (err != ESP_OK) {
        s_storage_error = true; /* Persistence uncertain: block future TLS loading. */
        ESP_LOGE(TAG, "NVS %s (%s): %s", stage, key, esp_err_to_name(err));
    }
    return err;
}

static void set_status(const ts_cert_info_t *current)
{
    ts_cert_info_t info = *current;
    if (s_storage_error || (s_private_key_pem && !s_key_valid) ||
        (s_certificate_pem && (info.validity == TS_CERT_VALIDITY_INVALID ||
                              (s_private_key_pem && !s_key_matches)))) {
        s_status = TS_CERT_STATUS_ERROR;
    } else if (s_private_key_pem && s_certificate_pem) {
        switch (info.validity) {
        case TS_CERT_VALIDITY_TIME_UNVERIFIED: s_status = TS_CERT_STATUS_TIME_UNVERIFIED; break;
        case TS_CERT_VALIDITY_NOT_YET_VALID: s_status = TS_CERT_STATUS_NOT_YET_VALID; break;
        case TS_CERT_VALIDITY_VALID: s_status = TS_CERT_STATUS_ACTIVATED; break;
        case TS_CERT_VALIDITY_EXPIRED: s_status = TS_CERT_STATUS_EXPIRED; break;
        default: s_status = TS_CERT_STATUS_ERROR; break;
        }
    } else if (s_private_key_pem) {
        if (s_status != TS_CERT_STATUS_CSR_PENDING) s_status = TS_CERT_STATUS_KEY_GENERATED;
    } else s_status = TS_CERT_STATUS_NOT_INITIALIZED;
    /* Derived status never writes NVS. */
}

static void update_status(void)
{
    refresh_metadata();
    ts_cert_info_t info = s_metadata;
    evaluate_time(&info, (int64_t)time(NULL));
    set_status(&info);
}

/*===========================================================================*/
/*                              SAN Extension                                 */
/*===========================================================================*/

/**
 * @brief Build SAN extension with IP addresses
 * 
 * ASN.1 structure:
 * SubjectAltName ::= GeneralNames
 * GeneralNames ::= SEQUENCE SIZE (1..MAX) OF GeneralName
 * GeneralName ::= CHOICE {
 *     iPAddress  [7] OCTET STRING
 * }
 */
static int build_san_extension(const ts_cert_csr_opts_t *opts,
                                unsigned char *buf, size_t buf_size,
                                size_t *olen)
{
    unsigned char *p = buf + buf_size;
    int ret;
    size_t len = 0;
    
    /* Build from end of buffer (mbedTLS style) */
    
    /* Add IP addresses (GeneralName with tag [7]) */
    for (int i = opts->ip_san_count - 1; i >= 0; i--) {
        uint32_t ip = opts->ip_sans[i];
        unsigned char ip_bytes[4];
        
        /* Convert to network byte order (big-endian) */
        ip_bytes[0] = (ip >> 24) & 0xFF;
        ip_bytes[1] = (ip >> 16) & 0xFF;
        ip_bytes[2] = (ip >> 8) & 0xFF;
        ip_bytes[3] = ip & 0xFF;
        
        /* Write IP address octets */
        MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_raw_buffer(&p, buf, ip_bytes, 4));
        
        /* Context tag [7] for iPAddress */
        MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_len(&p, buf, 4));
        MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_tag(&p, buf, 
            MBEDTLS_ASN1_CONTEXT_SPECIFIC | 7));
    }
    
    /* Add DNS names (GeneralName with tag [2]) */
    for (int i = opts->dns_san_count - 1; i >= 0; i--) {
        if (opts->dns_sans[i]) {
            size_t dns_len = strlen(opts->dns_sans[i]);
            MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_raw_buffer(&p, buf, 
                (const unsigned char *)opts->dns_sans[i], dns_len));
            MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_len(&p, buf, dns_len));
            MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_tag(&p, buf,
                MBEDTLS_ASN1_CONTEXT_SPECIFIC | 2));
        }
    }
    
    /* Wrap in SEQUENCE */
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_len(&p, buf, len));
    MBEDTLS_ASN1_CHK_ADD(len, mbedtls_asn1_write_tag(&p, buf, 
        MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    
    /* Move to start of buffer */
    memmove(buf, p, len);
    *olen = len;
    
    return 0;
}

/*===========================================================================*/
/*                           Public Functions                                 */
/*===========================================================================*/

static esp_err_t ts_cert_init_locked(void)
{
    if (s_initialized) return ESP_OK;
    
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_read_string(NVS_KEY_PRIVKEY, &s_private_key_pem);
    if (err == ESP_OK) err = nvs_read_string(NVS_KEY_CERT, &s_certificate_pem);
    if (err == ESP_OK) err = nvs_read_string(NVS_KEY_CA_CHAIN, &s_ca_chain_pem);
    if (err != ESP_OK) {
        if (s_private_key_pem) mbedtls_platform_zeroize(s_private_key_pem, strlen(s_private_key_pem));
        free(s_private_key_pem); free(s_certificate_pem); free(s_ca_chain_pem);
        s_private_key_pem = s_certificate_pem = s_ca_chain_pem = NULL;
        nvs_close(s_nvs_handle); s_nvs_handle = 0;
        s_storage_error = true;
        return err;
    }
    s_storage_error = false;
    s_metadata_generation = 0;
    /* 必须先设置 initialized，因为 update_status 会调用 ts_cert_get_info_locked */
    s_initialized = true;
    
    update_status();
    
    ESP_LOGI(TAG, "Initialized, status: %s, has_key=%d, has_cert=%d", 
             ts_cert_status_to_str(s_status),
             s_private_key_pem != NULL,
             s_certificate_pem != NULL);
    
    return ESP_OK;
}

static void ts_cert_deinit_locked(void)
{
    if (!s_initialized) return;
    
    if (s_private_key_pem) mbedtls_platform_zeroize(s_private_key_pem, strlen(s_private_key_pem));
    free(s_private_key_pem);
    free(s_certificate_pem);
    free(s_ca_chain_pem);
    s_private_key_pem = NULL;
    s_certificate_pem = NULL;
    s_ca_chain_pem = NULL;
    
    if (s_nvs_handle) {
        nvs_close(s_nvs_handle);
        s_nvs_handle = 0;
    }
    
    if (s_rng_initialized) {
        mbedtls_ctr_drbg_free(&s_ctr_drbg);
        mbedtls_entropy_free(&s_entropy);
        s_rng_initialized = false;
    }
    
    s_initialized = false;
}

/*===========================================================================*/
/*                         Key Pair Management                                */
/*===========================================================================*/

static esp_err_t ts_cert_generate_keypair_locked(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    esp_err_t err = init_rng();
    if (err != ESP_OK) return err;
    
    ESP_LOGI(TAG, "Generating ECDSA P-256 key pair...");
    
    /* Generate key pair using ts_crypto */
    ts_keypair_t keypair = NULL;
    err = ts_crypto_keypair_generate(TS_CRYPTO_KEY_EC_P256, &keypair);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Key generation failed: %s", esp_err_to_name(err));
        return err;
    }
    
    /* Export private key to PEM */
    char *key_pem = heap_caps_malloc(TS_CERT_KEY_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!key_pem) {
        key_pem = malloc(TS_CERT_KEY_MAX_LEN);
    }
    if (!key_pem) {
        ts_crypto_keypair_free(keypair);
        return ESP_ERR_NO_MEM;
    }
    
    size_t key_len = TS_CERT_KEY_MAX_LEN;
    err = ts_crypto_keypair_export_private(keypair, key_pem, &key_len);
    ts_crypto_keypair_free(keypair);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Key export failed: %s", esp_err_to_name(err));
        mbedtls_platform_zeroize(key_pem, TS_CERT_KEY_MAX_LEN);
        free(key_pem);
        return err;
    }
    
    /* Store in NVS */
    err = nvs_write_string(NVS_KEY_PRIVKEY, key_pem);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store key: %s", esp_err_to_name(err));
        mbedtls_platform_zeroize(key_pem, TS_CERT_KEY_MAX_LEN);
        free(key_pem);
        return err;
    }
    
    err = nvs_erase_key(s_nvs_handle, NVS_KEY_CERT);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        s_storage_error = true;
        mbedtls_platform_zeroize(key_pem, strlen(key_pem)); free(key_pem);
        return err;
    }
    if (s_private_key_pem) mbedtls_platform_zeroize(s_private_key_pem, strlen(s_private_key_pem));
    free(s_private_key_pem); s_private_key_pem = key_pem;
    free(s_certificate_pem); s_certificate_pem = NULL;
    ++s_generation;
    s_status = TS_CERT_STATUS_KEY_GENERATED;
    update_status();

    ESP_LOGI(TAG, "Key pair generated and stored");
    return ESP_OK;
}

static bool ts_cert_has_keypair_locked(void)
{
    return s_private_key_pem != NULL;
}

static esp_err_t ts_cert_delete_keypair_locked(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    esp_err_t err = ESP_OK;
    if (err == ESP_OK) {
        err = nvs_erase_key(s_nvs_handle, NVS_KEY_PRIVKEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(s_nvs_handle, NVS_KEY_CERT);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) { s_storage_error = true; return err; }
    ++s_generation;

    /* Securely clear memory */
    if (s_private_key_pem) {
        mbedtls_platform_zeroize(s_private_key_pem, strlen(s_private_key_pem));
        free(s_private_key_pem);
        s_private_key_pem = NULL;
    }
    
    free(s_certificate_pem);
    s_certificate_pem = NULL;
    
    update_status();
    
    ESP_LOGI(TAG, "Key pair deleted");
    return ESP_OK;
}

/*===========================================================================*/
/*                           CSR Generation                                   */
/*===========================================================================*/

static esp_err_t ts_cert_generate_csr_locked(const ts_cert_csr_opts_t *opts,
                                char *csr_pem, size_t *csr_len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!opts || !csr_pem || !csr_len) return ESP_ERR_INVALID_ARG;
    if (!opts->device_id || strlen(opts->device_id) == 0) return ESP_ERR_INVALID_ARG;
    if (!s_private_key_pem) {
        ESP_LOGE(TAG, "No private key, generate first");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t err = init_rng();
    if (err != ESP_OK) return err;
    
    int ret;
    char err_buf[128];
    mbedtls_pk_context pk;
    mbedtls_x509write_csr csr;
    
    mbedtls_pk_init(&pk);
    mbedtls_x509write_csr_init(&csr);
    
    /* Parse private key */
    ret = mbedtls_pk_parse_key(&pk, (const unsigned char *)s_private_key_pem,
                                strlen(s_private_key_pem) + 1, NULL, 0,
                                mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0) {
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "Failed to parse private key: %s", err_buf);
        err = ESP_FAIL;
        goto cleanup;
    }
    
    /* Set CSR parameters */
    mbedtls_x509write_csr_set_key(&csr, &pk);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    
    /* Build subject DN */
    char subject[256];
    int subject_len = snprintf(subject, sizeof(subject), "CN=%s", opts->device_id);
    
    if (opts->organization && strlen(opts->organization) > 0) {
        subject_len += snprintf(subject + subject_len, sizeof(subject) - subject_len,
                                ",O=%s", opts->organization);
    }
    if (opts->org_unit && strlen(opts->org_unit) > 0) {
        subject_len += snprintf(subject + subject_len, sizeof(subject) - subject_len,
                                ",OU=%s", opts->org_unit);
    }
    
    ret = mbedtls_x509write_csr_set_subject_name(&csr, subject);
    if (ret != 0) {
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "Failed to set subject: %s", err_buf);
        err = ESP_FAIL;
        goto cleanup;
    }
    
    /* Add SAN extension if IP addresses specified */
    if (opts->ip_san_count > 0 || opts->dns_san_count > 0) {
        unsigned char san_buf[256];
        size_t san_len = 0;
        
        ret = build_san_extension(opts, san_buf, sizeof(san_buf), &san_len);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to build SAN extension");
            err = ESP_FAIL;
            goto cleanup;
        }
        
        /* OID for Subject Alternative Name: 2.5.29.17 */
        ret = mbedtls_x509write_csr_set_extension(&csr,
            MBEDTLS_OID_SUBJECT_ALT_NAME, MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
            0,  /* not critical */
            san_buf, san_len);
        if (ret != 0) {
            mbedtls_strerror(ret, err_buf, sizeof(err_buf));
            ESP_LOGE(TAG, "Failed to set SAN extension: %s", err_buf);
            err = ESP_FAIL;
            goto cleanup;
        }
        
        ESP_LOGI(TAG, "Added SAN extension with %d IP(s), %d DNS name(s)",
                 opts->ip_san_count, opts->dns_san_count);
    }
    
    /* Generate CSR PEM */
    ret = mbedtls_x509write_csr_pem(&csr, (unsigned char *)csr_pem, *csr_len,
                                     mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0) {
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "Failed to write CSR PEM: %s", err_buf);
        err = ESP_FAIL;
        goto cleanup;
    }
    
    *csr_len = strlen(csr_pem) + 1;
    s_status = TS_CERT_STATUS_CSR_PENDING;

    
    ESP_LOGI(TAG, "CSR generated for %s", opts->device_id);
    err = ESP_OK;
    
cleanup:
    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&pk);
    return err;
}

static esp_err_t ts_cert_generate_csr_default_locked(char *csr_pem, size_t *csr_len)
{
    /* Get device IP address */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    }
    
    uint32_t ip_addr = 0;
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            /* Convert to host byte order for our SAN builder */
            ip_addr = ntohl(ip_info.ip.addr);
        }
    }
    
    /* TODO: Get device ID from configuration */
    const char *device_id = "TIANSHAN-DEVICE-001";
    
    ts_cert_csr_opts_t opts = {
        .device_id = device_id,
        .organization = "TianShanOS",
        .org_unit = "Device",
        .ip_san_count = ip_addr ? 1 : 0,
        .dns_san_count = 0
    };
    opts.ip_sans[0] = ip_addr;
    
    return ts_cert_generate_csr_locked(&opts, csr_pem, csr_len);
}

/*===========================================================================*/
/*                        Certificate Management                              */
/*===========================================================================*/

static char *copy_pem(const char *pem, size_t len)
{
    char *p = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(len);
    if (p) memcpy(p, pem, len);
    return p;
}

static void save_ca_copy(const char *pem)
{
    struct stat st;
    if (stat(CA_CHAIN_SDCARD_DIR, &st) != 0 && mkdir(CA_CHAIN_SDCARD_DIR, 0755) != 0) {
        ESP_LOGW(TAG, "CA saved in NVS; SD copy unavailable"); return;
    }
    FILE *file = fopen(CA_CHAIN_SDCARD_PATH, "w");
    if (!file) { ESP_LOGW(TAG, "CA saved in NVS; SD copy open failed"); return; }
    size_t len = strlen(pem);
    size_t written = fwrite(pem, 1, len, file);
    int closed = fclose(file);
    if (written != len || closed != 0) ESP_LOGW(TAG, "CA saved in NVS; SD copy write failed");
}

static esp_err_t install_material(const char *pem, size_t len, bool ca, ts_cert_op_error_t *detail)
{
    ts_cert_op_error_t why = TS_CERT_OP_OK;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    char *replacement = NULL;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    mbedtls_x509_crt_init(&crt); mbedtls_pk_init(&pk);
    if (!s_initialized) { why = TS_CERT_OP_NOT_INITIALIZED; err = ESP_ERR_INVALID_STATE; goto done; }
    if (!pem || len <= 1) { why = TS_CERT_OP_INVALID_INPUT; goto done; }
    size_t cap = ca ? TS_CERT_CA_CHAIN_MAX_LEN : TS_CERT_PEM_MAX_LEN;
    if (cap > TS_CERT_INSTALL_MAX_LEN) cap = TS_CERT_INSTALL_MAX_LEN;
    if (len > cap) { why = TS_CERT_OP_INPUT_TOO_LARGE; goto done; }
    if (pem[len-1] != 0 || memchr(pem, 0, len-1)) { why = TS_CERT_OP_INVALID_INPUT; goto done; }
    size_t i = 0;
    while (i < len-1 && isspace((unsigned char)pem[i])) ++i;
    if (i == len-1) { why = TS_CERT_OP_INVALID_INPUT; goto done; }
    if (!ca && !s_private_key_pem) {
        why = TS_CERT_OP_PRIVATE_KEY_MISSING; err = ESP_ERR_INVALID_STATE; goto done;
    }
    if (s_storage_error) { why = TS_CERT_OP_STORAGE_FAILED; err = ESP_FAIL; goto done; }
    char *old = ca ? s_ca_chain_pem : s_certificate_pem;
    replacement = copy_pem(pem, len);
    if (!replacement) { why = TS_CERT_OP_NO_MEMORY; err = ESP_ERR_NO_MEM; goto done; }
    int ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, len);
    if (ret != 0) {
        ESP_LOGW(TAG, "PEM parse: %d", ret);
        why = allocation_error(ret) ? TS_CERT_OP_NO_MEMORY : TS_CERT_OP_PEM_PARSE_FAILED;
        err = why == TS_CERT_OP_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
        goto done;
    }
    if (!ca) {
        err = init_rng();
        if (err != ESP_OK) { why = err == ESP_ERR_NO_MEM ? TS_CERT_OP_NO_MEMORY : TS_CERT_OP_CRYPTO_FAILED; goto done; }
        ret = mbedtls_pk_parse_key(&pk, (const unsigned char *)s_private_key_pem,
                strlen(s_private_key_pem)+1, NULL, 0, mbedtls_ctr_drbg_random, &s_ctr_drbg);
        if (ret != 0) {
            why = allocation_error(ret) ? TS_CERT_OP_NO_MEMORY : TS_CERT_OP_PRIVATE_KEY_INVALID;
            err = why == TS_CERT_OP_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_FAIL;
            goto done;
        }
        ret = mbedtls_pk_check_pair(&crt.pk, &pk, mbedtls_ctr_drbg_random, &s_ctr_drbg);
        if (ret != 0) {
            why = allocation_error(ret) ? TS_CERT_OP_NO_MEMORY : TS_CERT_OP_KEY_MISMATCH;
            err = why == TS_CERT_OP_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
            goto done;
        }
    }
    /* Reuse the candidate parser for metadata: no second parse or allocation after persistence. */
    ts_cert_info_t info = {0};
    unsigned char hash[32];
    if (!ca) {
        info_from_crt(&crt, &info);
        ret = mbedtls_sha256(crt.raw.p, crt.raw.len, hash, 0);
        if (ret != 0) { why = TS_CERT_OP_CRYPTO_FAILED; err = ESP_FAIL; goto done; }
    }
    bool changed = !old || strcmp(old, pem) != 0;
    if (changed) {
        err = nvs_write_string(ca ? NVS_KEY_CA_CHAIN : NVS_KEY_CERT, pem);
        if (err != ESP_OK) { why = TS_CERT_OP_STORAGE_FAILED; goto done; }
        if (ca) s_ca_chain_pem = replacement; else s_certificate_pem = replacement;
        replacement = NULL;
        free(old);
        ++s_generation;
    }
    if (ca) s_ca_valid = true;
    else {
        s_metadata = info;
        s_key_valid = s_key_matches = true;
        ts_cert_hex(hash, sizeof(hash), s_fingerprint, sizeof(s_fingerprint));
    }
    s_metadata_generation = s_generation;
    update_status();
    if (ca && changed) save_ca_copy(pem);
    err = ESP_OK;
 done:
    free(replacement);
    mbedtls_pk_free(&pk); mbedtls_x509_crt_free(&crt);
    if (detail) *detail = why;
    return err;
}

static esp_err_t ts_cert_install_certificate_ex_locked(const char *pem, size_t len, ts_cert_op_error_t *detail)
{ return install_material(pem, len, false, detail); }
static esp_err_t ts_cert_install_ca_chain_ex_locked(const char *pem, size_t len, ts_cert_op_error_t *detail)
{ return install_material(pem, len, true, detail); }
static esp_err_t ts_cert_install_certificate_locked(const char *pem, size_t len)
{ return ts_cert_install_certificate_ex_locked(pem, len, NULL); }
static esp_err_t ts_cert_install_ca_chain_locked(const char *pem, size_t len)
{ return ts_cert_install_ca_chain_ex_locked(pem, len, NULL); }

static esp_err_t ts_cert_get_certificate_locked(char *cert_pem, size_t *cert_len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!cert_pem || !cert_len) return ESP_ERR_INVALID_ARG;
    
    if (!s_certificate_pem) {
        return ESP_ERR_NOT_FOUND;
    }
    
    size_t required = strlen(s_certificate_pem) + 1;
    if (*cert_len < required) {
        *cert_len = required;
        return ESP_ERR_INVALID_SIZE;
    }
    
    strcpy(cert_pem, s_certificate_pem);
    *cert_len = required;
    return ESP_OK;
}

static esp_err_t ts_cert_get_private_key_locked(char *key_pem, size_t *key_len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!key_pem || !key_len) return ESP_ERR_INVALID_ARG;
    
    if (!s_private_key_pem) {
        return ESP_ERR_NOT_FOUND;
    }
    
    size_t required = strlen(s_private_key_pem) + 1;
    if (*key_len < required) {
        *key_len = required;
        return ESP_ERR_INVALID_SIZE;
    }
    
    strcpy(key_pem, s_private_key_pem);
    *key_len = required;
    return ESP_OK;
}

static esp_err_t ts_cert_get_ca_chain_locked(char *ca_chain_pem, size_t *ca_chain_len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!ca_chain_pem || !ca_chain_len) return ESP_ERR_INVALID_ARG;
    
    if (!s_ca_chain_pem) {
        return ESP_ERR_NOT_FOUND;
    }
    
    size_t required = strlen(s_ca_chain_pem) + 1;
    if (*ca_chain_len < required) {
        *ca_chain_len = required;
        return ESP_ERR_INVALID_SIZE;
    }
    
    strcpy(ca_chain_pem, s_ca_chain_pem);
    *ca_chain_len = required;
    return ESP_OK;
}

/*===========================================================================*/
/*                           Status & Info                                    */
/*===========================================================================*/

static esp_err_t ts_cert_refresh_status_locked(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    ts_cert_status_t old_status = s_status;
    update_status();
    
    if (old_status != s_status) {
        ESP_LOGI(TAG, "PKI status updated: %s -> %s",
                 ts_cert_status_to_str(old_status),
                 ts_cert_status_to_str(s_status));
    } else {
        ESP_LOGD(TAG, "PKI status refreshed: %s (unchanged)", 
                 ts_cert_status_to_str(s_status));
    }
    
    return ESP_OK;
}

static esp_err_t ts_cert_get_status_locked(ts_cert_pki_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    refresh_metadata();
    status->has_private_key = s_private_key_pem != NULL;
    status->has_certificate = s_certificate_pem != NULL;
    status->has_ca_chain = s_ca_chain_pem != NULL;
    status->generation = s_generation;
    status->storage_error = s_storage_error;
    status->key_valid = s_key_valid;
    status->key_matches = s_key_matches;
    status->ca_valid = s_ca_valid;
    status->cert_info = s_metadata;
    int64_t now = (int64_t)time(NULL);
    status->time_ready = ts_cert_time_ready(now, TS_TIME_MIN_VALID_YEAR);
    evaluate_time(&status->cert_info, now);
    set_status(&status->cert_info);
    status->status = s_status;
    return ESP_OK;
}

static esp_err_t ts_cert_get_info_locked(ts_cert_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!s_certificate_pem) return ESP_ERR_NOT_FOUND;
    refresh_metadata();
    *info = s_metadata;
    evaluate_time(info, (int64_t)time(NULL));
    return info->validity == TS_CERT_VALIDITY_INVALID ? ESP_ERR_INVALID_ARG : ESP_OK;
}

static bool ts_cert_is_valid_locked(void)
{
    if (!s_certificate_pem) return false;
    
    ts_cert_info_t info;
    if (ts_cert_get_info_locked(&info) != ESP_OK) return false;
    
    return info.is_valid;
}

static int ts_cert_days_until_expiry_locked(void)
{
    if (!s_certificate_pem) return INT32_MAX;
    
    ts_cert_info_t info;
    if (ts_cert_get_info_locked(&info) != ESP_OK) return INT32_MAX;
    
    return info.days_until_expiry;
}

/*===========================================================================*/
/*                          Factory Reset                                     */
/*===========================================================================*/

static esp_err_t ts_cert_factory_reset_locked(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    
    /* Erase all NVS keys */
    esp_err_t err = ESP_OK;
    if (err == ESP_OK) {
        err = nvs_erase_key(s_nvs_handle, NVS_KEY_PRIVKEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(s_nvs_handle, NVS_KEY_CERT);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(s_nvs_handle, NVS_KEY_CA_CHAIN);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(s_nvs_handle, NVS_KEY_STATUS);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) { s_storage_error = true; return err; }
    ++s_generation;

    /* Securely clear memory */
    if (s_private_key_pem) {
        mbedtls_platform_zeroize(s_private_key_pem, strlen(s_private_key_pem));
        free(s_private_key_pem);
        s_private_key_pem = NULL;
    }
    
    free(s_certificate_pem);
    s_certificate_pem = NULL;
    
    free(s_ca_chain_pem);
    s_ca_chain_pem = NULL;
    
    s_status = TS_CERT_STATUS_NOT_INITIALIZED;
    
    ESP_LOGI(TAG, "Factory reset complete");
    return ESP_OK;
}

/*===========================================================================*/
/*                          Utility Functions                                 */
/*===========================================================================*/

const char *ts_cert_status_to_str(ts_cert_status_t status)
{
    switch (status) {
        case TS_CERT_STATUS_NOT_INITIALIZED: return "not_initialized";
        case TS_CERT_STATUS_KEY_GENERATED:   return "key_generated";
        case TS_CERT_STATUS_CSR_PENDING:     return "csr_pending";
        case TS_CERT_STATUS_ACTIVATED:       return "activated";
        case TS_CERT_STATUS_EXPIRED:         return "expired";
        case TS_CERT_STATUS_ERROR:           return "error";
        case TS_CERT_STATUS_TIME_UNVERIFIED: return "time_unverified";
        case TS_CERT_STATUS_NOT_YET_VALID: return "not_yet_valid";
        default:                             return "unknown";
    }
}

static esp_err_t info_from_crt(const mbedtls_x509_crt *crt, ts_cert_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->validity = TS_CERT_VALIDITY_INVALID;
    /* Extract subject CN */
    const mbedtls_x509_name *name = &crt->subject;
    info->subject_cn[0] = '\0';
    info->subject_ou[0] = '\0';
    while (name) {
        if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &name->oid) == 0) {
            size_t len = name->val.len;
            if (len >= sizeof(info->subject_cn)) len = sizeof(info->subject_cn) - 1;
            memcpy(info->subject_cn, name->val.p, len);
            info->subject_cn[len] = '\0';
        }
        if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_ORG_UNIT, &name->oid) == 0) {
            size_t len = name->val.len;
            if (len >= sizeof(info->subject_ou)) len = sizeof(info->subject_ou) - 1;
            memcpy(info->subject_ou, name->val.p, len);
            info->subject_ou[len] = '\0';
        }
        name = name->next;
    }
    
    /* Extract issuer CN */
    name = &crt->issuer;
    info->issuer_cn[0] = '\0';
    while (name) {
        if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &name->oid) == 0) {
            size_t len = name->val.len;
            if (len >= sizeof(info->issuer_cn)) len = sizeof(info->issuer_cn) - 1;
            memcpy(info->issuer_cn, name->val.p, len);
            info->issuer_cn[len] = '\0';
            break;
        }
        name = name->next;
    }
    
    if (!ts_cert_time_utc(crt->valid_from.year, crt->valid_from.mon, crt->valid_from.day,
                         crt->valid_from.hour, crt->valid_from.min, crt->valid_from.sec, &info->not_before) ||
        !ts_cert_time_utc(crt->valid_to.year, crt->valid_to.mon, crt->valid_to.day,
                         crt->valid_to.hour, crt->valid_to.min, crt->valid_to.sec, &info->not_after) ||
        info->not_after < info->not_before) {
        return ESP_ERR_INVALID_ARG;
    }
    info->serial_truncated = ts_cert_hex(crt->serial.p, crt->serial.len, info->serial, sizeof(info->serial));
    info->validity = TS_CERT_VALIDITY_VALID;
    return ESP_OK;
}

esp_err_t ts_cert_parse_certificate(const char *cert_pem, size_t cert_len,
                                     ts_cert_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    info->validity = TS_CERT_VALIDITY_INVALID;
    if (!cert_pem || cert_len <= 1 || cert_pem[cert_len-1] != 0 || memchr(cert_pem, 0, cert_len-1))
        return ESP_ERR_INVALID_ARG;
    
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    
    int ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert_pem, cert_len);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        return ESP_ERR_INVALID_ARG;
    }
    
    ret = info_from_crt(&crt, info);
    if (ret == ESP_OK) evaluate_time(info, (int64_t)time(NULL));
    mbedtls_x509_crt_free(&crt);
    return ret;
}

static void evaluate_time(ts_cert_info_t *info, int64_t now)
{
    info->is_valid = false;
    info->seconds_until_expiry = 0;
    info->days_until_expiry = 0;
    info->time_ready = ts_cert_time_ready(now, TS_TIME_MIN_VALID_YEAR);
    if (info->validity == TS_CERT_VALIDITY_NONE || info->validity == TS_CERT_VALIDITY_INVALID) return;
    if (!info->time_ready) { info->validity = TS_CERT_VALIDITY_TIME_UNVERIFIED; return; }
    info->seconds_until_expiry = info->not_after - now;
    info->days_until_expiry = ts_cert_time_days(info->seconds_until_expiry);
    info->validity = now < info->not_before ? TS_CERT_VALIDITY_NOT_YET_VALID :
                     now > info->not_after ? TS_CERT_VALIDITY_EXPIRED : TS_CERT_VALIDITY_VALID;
    info->is_valid = info->validity == TS_CERT_VALIDITY_VALID;
}

static void refresh_metadata(void)
{
    if (s_metadata_generation == s_generation) return;
    memset(&s_metadata, 0, sizeof(s_metadata));
    s_key_valid = s_key_matches = s_ca_valid = false;
    s_fingerprint[0] = 0;
    mbedtls_pk_context key;
    mbedtls_x509_crt crt, ca;
    mbedtls_pk_init(&key); mbedtls_x509_crt_init(&crt); mbedtls_x509_crt_init(&ca);
    if (s_private_key_pem && init_rng() == ESP_OK)
        s_key_valid = mbedtls_pk_parse_key(&key, (const unsigned char *)s_private_key_pem,
            strlen(s_private_key_pem)+1, NULL, 0, mbedtls_ctr_drbg_random, &s_ctr_drbg) == 0;
    if (s_certificate_pem) {
        s_metadata.validity = TS_CERT_VALIDITY_INVALID;
        if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)s_certificate_pem, strlen(s_certificate_pem)+1) == 0) {
            info_from_crt(&crt, &s_metadata);
            s_key_matches = s_key_valid && mbedtls_pk_check_pair(&crt.pk, &key, mbedtls_ctr_drbg_random, &s_ctr_drbg) == 0;
            unsigned char hash[32];
            if (mbedtls_sha256(crt.raw.p, crt.raw.len, hash, 0) == 0)
                ts_cert_hex(hash, sizeof(hash), s_fingerprint, sizeof(s_fingerprint));
        }
    }
    if (s_ca_chain_pem)
        s_ca_valid = mbedtls_x509_crt_parse(&ca, (const unsigned char *)s_ca_chain_pem, strlen(s_ca_chain_pem)+1) == 0;
    mbedtls_pk_free(&key); mbedtls_x509_crt_free(&crt); mbedtls_x509_crt_free(&ca);
    s_metadata_generation = s_generation;
}

bool ts_cert_prerequisites(const ts_cert_pki_status_t *s, bool require_ca)
{
    return s && !s->storage_error && s->time_ready && s->has_private_key && s->key_valid &&
        s->has_certificate && s->key_matches && s->cert_info.is_valid &&
        (!require_ca || (s->has_ca_chain && s->ca_valid));
}

const char *ts_cert_validity_to_str(ts_cert_validity_t v)
{
    static const char *names[] = {"none", "invalid", "time_unverified", "not_yet_valid", "valid", "expired"};
    return (unsigned)v < sizeof(names)/sizeof(names[0]) ? names[v] : "invalid";
}
const char *ts_cert_op_error_to_str(ts_cert_op_error_t e)
{
    static const char *names[] = {"Saved", "Certificate module not ready", "Provide non-empty PEM text",
        "PEM exceeds 3999 content bytes (4000 including NUL)", "Cannot parse certificate PEM",
        "Generate the device key first", "Stored private key is invalid", "Certificate does not match device key; use its CSR",
        "Insufficient device memory", "Credential storage failed; restart and check stored materials", "Device cryptographic operation failed"};
    return (unsigned)e < sizeof(names)/sizeof(names[0]) ? names[e] : "Certificate operation failed";
}

void ts_cert_free_snapshot(ts_cert_snapshot_t *s)
{
    if (!s) return;
    if (s->key) mbedtls_platform_zeroize(s->key, strlen(s->key));
    free(s->key); free(s->certificate); free(s->ca);
    memset(s, 0, sizeof(*s));
}

static esp_err_t ts_cert_get_snapshot_locked(bool require_ca, ts_cert_snapshot_t *snapshot)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    memset(snapshot, 0, sizeof(*snapshot));
    ts_cert_pki_status_t status;
    esp_err_t err = ts_cert_get_status_locked(&status);
    if (err != ESP_OK) return err;
    if (!ts_cert_prerequisites(&status, require_ca)) return ESP_ERR_INVALID_STATE;
    snapshot->key = copy_pem(s_private_key_pem, strlen(s_private_key_pem)+1);
    snapshot->certificate = copy_pem(s_certificate_pem, strlen(s_certificate_pem)+1);
    if (s_ca_chain_pem && s_ca_valid) snapshot->ca = copy_pem(s_ca_chain_pem, strlen(s_ca_chain_pem)+1);
    if (!snapshot->key || !snapshot->certificate || (require_ca && !snapshot->ca)) {
        ts_cert_free_snapshot(snapshot); return ESP_ERR_NO_MEM;
    }
    snapshot->generation = s_generation;
    memcpy(snapshot->certificate_sha256, s_fingerprint, sizeof(s_fingerprint));
    return ESP_OK;
}

/* All public cache/RNG operations take the same lock; events are posted after release.
 * Lock order: material only -> unlock -> event; never material -> TLS/service manager.
 */
static void material_unlock(uint32_t previous, uint32_t kind)
{
    uint32_t generation = s_generation;
    xSemaphoreGive(s_mutex);
    if (generation != previous) {
        struct { uint32_t generation; uint32_t kind; } event = {generation, kind};
        esp_err_t err = ts_event_post(TS_EVENT_BASE_PKI, TS_EVENT_PKI_MATERIAL_CHANGED, &event, sizeof(event), 0);
        if (err != ESP_OK) ESP_LOGW(TAG, "Material notification lost: %s", esp_err_to_name(err));
    }
}

esp_err_t ts_cert_init(void)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_init_locked();
    material_unlock(previous, 0);
    return result;
}

void ts_cert_deinit(void)
{
    material_lock();
    uint32_t previous = s_generation;
    ts_cert_deinit_locked();
    material_unlock(previous, 0);
}

esp_err_t ts_cert_generate_keypair(void)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_generate_keypair_locked();
    material_unlock(previous, 3);
    return result;
}

bool ts_cert_has_keypair(void)
{
    material_lock();
    uint32_t previous = s_generation;
    bool result = ts_cert_has_keypair_locked();
    material_unlock(previous, 0);
    return result;
}

esp_err_t ts_cert_delete_keypair(void)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_delete_keypair_locked();
    material_unlock(previous, 4);
    return result;
}

esp_err_t ts_cert_generate_csr(const ts_cert_csr_opts_t *opts,
                                char *csr_pem, size_t *csr_len)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_generate_csr_locked(opts, csr_pem, csr_len);
    material_unlock(previous, 0);
    return result;
}

esp_err_t ts_cert_generate_csr_default(char *csr_pem, size_t *csr_len)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_generate_csr_default_locked(csr_pem, csr_len);
    material_unlock(previous, 0);
    return result;
}

esp_err_t ts_cert_install_certificate_ex(const char *pem, size_t len, ts_cert_op_error_t *detail)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_install_certificate_ex_locked(pem, len, detail);
    material_unlock(previous, 1);
    return result;
}

esp_err_t ts_cert_install_ca_chain_ex(const char *pem, size_t len, ts_cert_op_error_t *detail)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_install_ca_chain_ex_locked(pem, len, detail);
    material_unlock(previous, 2);
    return result;
}

esp_err_t ts_cert_install_certificate(const char *pem, size_t len)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_install_certificate_locked(pem, len);
    material_unlock(previous, 1);
    return result;
}

esp_err_t ts_cert_install_ca_chain(const char *pem, size_t len)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_install_ca_chain_locked(pem, len);
    material_unlock(previous, 2);
    return result;
}

esp_err_t ts_cert_get_certificate(char *cert_pem, size_t *cert_len)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_get_certificate_locked(cert_pem, cert_len);
    material_unlock(previous, 0);
    return result;
}

esp_err_t ts_cert_get_private_key(char *key_pem, size_t *key_len)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_get_private_key_locked(key_pem, key_len);
    material_unlock(previous, 0);
    return result;
}

esp_err_t ts_cert_get_ca_chain(char *ca_chain_pem, size_t *ca_chain_len)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_get_ca_chain_locked(ca_chain_pem, ca_chain_len);
    material_unlock(previous, 0);
    return result;
}

esp_err_t ts_cert_refresh_status(void)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_refresh_status_locked();
    material_unlock(previous, 0);
    return result;
}

esp_err_t ts_cert_get_status(ts_cert_pki_status_t *status)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_get_status_locked(status);
    material_unlock(previous, 0);
    return result;
}

esp_err_t ts_cert_get_info(ts_cert_info_t *info)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_get_info_locked(info);
    material_unlock(previous, 0);
    return result;
}

bool ts_cert_is_valid(void)
{
    material_lock();
    uint32_t previous = s_generation;
    bool result = ts_cert_is_valid_locked();
    material_unlock(previous, 0);
    return result;
}

int ts_cert_days_until_expiry(void)
{
    material_lock();
    uint32_t previous = s_generation;
    int result = ts_cert_days_until_expiry_locked();
    material_unlock(previous, 0);
    return result;
}

esp_err_t ts_cert_factory_reset(void)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_factory_reset_locked();
    material_unlock(previous, 4);
    return result;
}

esp_err_t ts_cert_get_snapshot(bool require_ca, ts_cert_snapshot_t *snapshot)
{
    material_lock();
    uint32_t previous = s_generation;
    esp_err_t result = ts_cert_get_snapshot_locked(require_ca, snapshot);
    material_unlock(previous, 0);
    return result;
}
