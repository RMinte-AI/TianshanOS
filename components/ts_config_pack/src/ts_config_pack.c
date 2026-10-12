/**
 * @file ts_config_pack.c
 * @brief TianShanOS Encrypted Configuration Package Implementation
 * 
 * 实现配置包的加密、解密、签名和验证逻辑。
 * 
 * 加密流程:
 * 1. 生成临时 EC 密钥对 (ephemeral)
 * 2. ECDH 密钥协商 (ephemeral_priv × recipient_pub)
 * 3. HKDF 密钥派生 (shared_secret → AES key)
 * 4. AES-256-GCM 加密
 * 5. ECDSA 签名
 * 
 * 解密流程:
 * 1. 验证签名者证书链
 * 2. 验证签名
 * 3. ECDH 密钥协商 (device_priv × ephemeral_pub)
 * 4. HKDF 密钥派生
 * 5. AES-256-GCM 解密
 * 6. 验证内容哈希
 */

#include "ts_config_pack.h"
#include "ts_core.h"
#include "ts_cert.h"
#include "ts_crypto.h"
#include "ts_config_pack_trust.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>

#ifndef CONFIG_TS_CONFIG_PACK_MAX_SIZE
#define CONFIG_TS_CONFIG_PACK_MAX_SIZE 65536
#endif

static bool unambiguous_json(const cJSON *j, unsigned depth) {
    if (depth > 32) return false;
    for (const cJSON *a = j->child; a; a = a->next) {
        if (cJSON_IsObject(j)) {
            for (const cJSON *b = a->next; b; b = b->next)
                if (a->string && b->string && !strcmp(a->string, b->string)) return false;
        }
        if (!unambiguous_json(a, depth + 1)) return false;
    }
    return true;
}
static cJSON *parse_json_bounded(const char *text, size_t length, size_t limit) {
    if (!text || !length || length > limit || memchr(text, 0, length))
        return NULL;
    bool string = false;
    unsigned depth = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = text[i];
        if (string && c == '\\') {
            if (i + 5 < length && text[i+1] == 'u' && !memcmp(text+i+2, "0000", 4)) return NULL;
            ++i;
        } else if (c == '"') string = !string;
        else if (!string && (c == '{' || c == '[')) { if (++depth > 32) return NULL; }
        else if (!string && (c == '}' || c == ']')) { if (!depth) return NULL; --depth; }
    }
    const char *end = NULL;
    cJSON *j = cJSON_ParseWithLengthOpts(text, length, &end, false);
    if (!j) return NULL;
    while (end < text + length && isspace((unsigned char)*end)) ++end;
    if (end != text + length || !unambiguous_json(j, 0)) { cJSON_Delete(j); return NULL; }
    return j;
}
cJSON *ts_config_pack_parse_json(const char *text, size_t length) {
    return parse_json_bounded(text,length,CONFIG_TS_CONFIG_PACK_MAX_SIZE);
}
/* Validate the outer transport before cJSON can discard a decoded NUL or
 * choose one of duplicate parameters. JSON escaping can expand bytes sixfold. */
cJSON *ts_config_pack_parse_import_request(const char *text,size_t length) {
    return parse_json_bounded(text,length,(size_t)CONFIG_TS_CONFIG_PACK_MAX_SIZE*6+1024);
}

static const char *TAG = "ts_config_pack";

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

/** ECDH P-256 公钥长度 (uncompressed: 0x04 || X || Y) */
#define ECDH_PUBKEY_LEN         65

/** HKDF salt 长度 */
#define HKDF_SALT_LEN           32

/** AES-256 密钥长度 */
#define AES_KEY_LEN             32

/** AES-GCM IV 长度 */
#define AES_IV_LEN              12

/** AES-GCM tag 长度 */
#define AES_TAG_LEN             16

/** SHA-256 哈希长度 */
#define SHA256_LEN              32

/** 证书指纹长度 (hex string) */
#define CERT_FINGERPRINT_LEN    64

/** 最大签名长度 (ECDSA P-256 DER) */
#define MAX_SIGNATURE_LEN       72

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/** 内部：加密参数 */
typedef struct {
    uint8_t ephemeral_pubkey[ECDH_PUBKEY_LEN];
    uint8_t salt[HKDF_SALT_LEN];
    uint8_t iv[AES_IV_LEN];
    uint8_t tag[AES_TAG_LEN];
    char recipient_fingerprint[CERT_FINGERPRINT_LEN + 1];
} ts_config_pack_crypto_params_t;

/*===========================================================================*/
/*                          Static Variables                                  */
/*===========================================================================*/

static bool s_initialized = false;
static char s_device_fingerprint[CERT_FINGERPRINT_LEN + 1] = {0};

/*===========================================================================*/
/*                          Forward Declarations                              */
/*===========================================================================*/

static esp_err_t compute_cert_fingerprint(const char *cert_pem, size_t cert_len,
                                           char *fingerprint, size_t fp_len);
static ts_config_pack_result_t parse_tscfg_json(const char *json, size_t len,
                                                  cJSON **root,
                                                  ts_config_pack_crypto_params_t *params,
                                                  char **payload_b64,
                                                  char **signer_cert_pem,
                                                  char **signature_b64,
                                                  ts_config_pack_sig_info_t *sig_info);
static ts_config_pack_result_t verify_signature(const char *signer_cert_pem,
                                                  const uint8_t *data_to_sign,
                                                  size_t data_len,
                                                  const uint8_t *signature,
                                                  size_t sig_len);
static ts_config_pack_result_t decrypt_payload(const ts_config_pack_crypto_params_t *params,
                                                 const uint8_t *ciphertext,
                                                 size_t ciphertext_len,
                                                 char **plaintext,
                                                 size_t *plaintext_len);

/*===========================================================================*/
/*                          Error Messages                                    */
/*===========================================================================*/

static const char *s_error_messages[] = {
    [TS_CONFIG_PACK_OK]             = "Success",
    [TS_CONFIG_PACK_ERR_PARSE]      = "JSON parse error",
    [TS_CONFIG_PACK_ERR_VERSION]    = "Unsupported version",
    [TS_CONFIG_PACK_ERR_RECIPIENT]  = "Not intended for this device",
    [TS_CONFIG_PACK_ERR_CERT_CHAIN] = "Certificate chain validation failed",
    [TS_CONFIG_PACK_ERR_SIGNATURE]  = "Signature verification failed",
    [TS_CONFIG_PACK_ERR_DECRYPT]    = "Decryption failed",
    [TS_CONFIG_PACK_ERR_INTEGRITY]  = "Content hash mismatch",
    [TS_CONFIG_PACK_ERR_EXPIRED]    = "Package expired",
    [TS_CONFIG_PACK_ERR_NO_MEM]     = "Memory allocation failed",
    [TS_CONFIG_PACK_ERR_IO]         = "File I/O error",
    [TS_CONFIG_PACK_ERR_PERMISSION] = "Permission denied (not a developer device)",
    [TS_CONFIG_PACK_ERR_INVALID_ARG]= "Invalid argument",
    [TS_CONFIG_PACK_ERR_NOT_INIT]   = "System not initialized",
    [TS_CONFIG_PACK_ERR_TIME_UNVERIFIED] = "Trusted time is not available",
    [TS_CONFIG_PACK_ERR_TRUST_NOT_CONFIGURED] = "Configuration signing root is not configured",
    [TS_CONFIG_PACK_ERR_SIGNER_ROLE] = "Certificate is not authorized for configuration signing",
    [TS_CONFIG_PACK_ERR_CREDENTIAL_CHANGED] = "Credentials changed during validation",
};

const char *ts_config_pack_strerror(ts_config_pack_result_t result)
{
    if (result < 0 || result >= sizeof(s_error_messages) / sizeof(s_error_messages[0])) {
        return "Unknown error";
    }
    return s_error_messages[result];
}

/*===========================================================================*/
/*                          Initialization                                    */
/*===========================================================================*/

esp_err_t ts_config_pack_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    
    /* 获取设备证书指纹 - 使用堆分配避免栈溢出 */
    char *cert_pem = TS_MALLOC_PSRAM(TS_CERT_PEM_MAX_LEN);
    if (!cert_pem) {
        ESP_LOGE(TAG, "Failed to allocate cert buffer");
        return ESP_ERR_NO_MEM;
    }
    size_t cert_len = TS_CERT_PEM_MAX_LEN;
    
    esp_err_t ret = ts_cert_get_certificate(cert_pem, &cert_len);
    if (ret != ESP_OK) {
        free(cert_pem);
        ESP_LOGW(TAG, "No device certificate, config pack import disabled");
        /* 没有证书也可以初始化，但无法解密 */
        s_initialized = true;
        return ESP_OK;
    }
    
    ret = compute_cert_fingerprint(cert_pem, cert_len, 
                                    s_device_fingerprint, sizeof(s_device_fingerprint));
    free(cert_pem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to compute certificate fingerprint");
        return ret;
    }
    
    ESP_LOGI(TAG, "Config pack system initialized, device fingerprint: %.16s...", 
             s_device_fingerprint);
    s_initialized = true;
    return ESP_OK;
}

bool ts_config_pack_can_export(void)
{
    /* 检查设备证书的 OU 字段是否包含 "Developer" */
    ts_cert_info_t info;
    if (ts_cert_get_info(&info) != ESP_OK) {
        return false;
    }
    
    /* 检查 OU 字段是否为 "Developer" */
    bool can_export = strcmp(info.subject_ou, "Developer") == 0;
    if (!can_export) {
        ESP_LOGD(TAG, "Device OU='%s', export not allowed", info.subject_ou);
    }
    return can_export;
}

/*===========================================================================*/
/*                          Load Functions                                    */
/*===========================================================================*/

ts_config_pack_result_t ts_config_pack_load(const char *path, ts_config_pack_t **pack)
{
    if (!path || !pack) {
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    if (!s_initialized) {
        return TS_CONFIG_PACK_ERR_NOT_INIT;
    }
    
    /* 读取文件 */
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return TS_CONFIG_PACK_ERR_IO;
    }
    
    /* 获取文件大小 */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > CONFIG_TS_CONFIG_PACK_MAX_SIZE) {
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        fclose(f);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    char *json_buf = TS_MALLOC_PSRAM(file_size + 1);
    if (!json_buf) {
        fclose(f);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    size_t read_len = fread(json_buf, 1, file_size, f);
    fclose(f);
    
    if (read_len != (size_t)file_size) {
        free(json_buf);
        return TS_CONFIG_PACK_ERR_IO;
    }
    json_buf[file_size] = '\0';
    
    ts_config_pack_result_t result = ts_config_pack_load_mem(json_buf, file_size, pack);
    free(json_buf);
    
    return result;
}

ts_config_pack_result_t ts_config_pack_load_mem(
    const char *tscfg_json,
    size_t tscfg_len,
    ts_config_pack_t **pack)
{
    if (!tscfg_json || !pack) {
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    if (!s_initialized) {
        return TS_CONFIG_PACK_ERR_NOT_INIT;
    }
    
    /* 检查设备是否有私钥可用于解密 */
    if (s_device_fingerprint[0] == '\0') {
        ESP_LOGE(TAG, "No device certificate, cannot decrypt");
        return TS_CONFIG_PACK_ERR_RECIPIENT;
    }
    
    cJSON *root = NULL;
    ts_config_pack_crypto_params_t params = {0};
    char *payload_b64 = NULL;
    char *signer_cert_pem = NULL;
    char *signature_b64 = NULL;
    ts_config_pack_sig_info_t sig_info = {0};
    
    /* 解析 JSON */
    ts_config_pack_result_t result = parse_tscfg_json(
        tscfg_json, tscfg_len,
        &root, &params, &payload_b64, &signer_cert_pem, &signature_b64, &sig_info);
    
    if (result != TS_CONFIG_PACK_OK) {
        goto cleanup;
    }
    
    /* 验证接收方指纹 */
    if (strcmp(params.recipient_fingerprint, s_device_fingerprint) != 0) {
        ESP_LOGE(TAG, "Config pack not intended for this device");
        ESP_LOGE(TAG, "Device fingerprint: %s", s_device_fingerprint);
        ESP_LOGE(TAG, "Pack fingerprint:   %s", params.recipient_fingerprint);
        result = TS_CONFIG_PACK_ERR_RECIPIENT;
        goto cleanup;
    }
    
    /* Base64 解码 payload */
    size_t payload_b64_len = strlen(payload_b64);
    size_t ciphertext_max_len = (payload_b64_len * 3) / 4 + 4;
    uint8_t *ciphertext = TS_MALLOC_PSRAM(ciphertext_max_len);
    if (!ciphertext) {
        result = TS_CONFIG_PACK_ERR_NO_MEM;
        goto cleanup;
    }
    
    size_t ciphertext_len = ciphertext_max_len;
    if (ts_crypto_base64_decode(payload_b64, payload_b64_len, 
                                 ciphertext, &ciphertext_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode payload");
        free(ciphertext);
        result = TS_CONFIG_PACK_ERR_PARSE;
        goto cleanup;
    }
    
    /* Base64 解码签名 */
    size_t sig_b64_len = strlen(signature_b64);
    uint8_t signature[MAX_SIGNATURE_LEN];
    size_t sig_len = sizeof(signature);
    if (ts_crypto_base64_decode(signature_b64, sig_b64_len,
                                 signature, &sig_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode signature");
        free(ciphertext);
        result = TS_CONFIG_PACK_ERR_PARSE;
        goto cleanup;
    }
    
    /* 构建待签名数据并验证签名 */
    /* TODO: 完整的签名数据应包含 ciphertext + ephemeral_pub + fingerprint + timestamp + version */
    /* 这里简化为只对 ciphertext 签名 */
    result = verify_signature(signer_cert_pem, ciphertext, ciphertext_len,
                               signature, sig_len);
    if (result != TS_CONFIG_PACK_OK) {
        free(ciphertext);
        goto cleanup;
    }
    
    sig_info.valid = true;
    
    /* 解密 payload */
    char *plaintext = NULL;
    size_t plaintext_len = 0;
    result = decrypt_payload(&params, ciphertext, ciphertext_len,
                              &plaintext, &plaintext_len);
    free(ciphertext);
    
    if (result != TS_CONFIG_PACK_OK) {
        goto cleanup;
    }
    
    /* 构建返回结构 */
    ts_config_pack_t *p = TS_CALLOC_PSRAM(1, sizeof(ts_config_pack_t));
    if (!p) {
        free(plaintext);
        result = TS_CONFIG_PACK_ERR_NO_MEM;
        goto cleanup;
    }
    
    /* 从 JSON 提取元数据 */
    cJSON *metadata = cJSON_GetObjectItem(root, "metadata");
    if (metadata) {
        cJSON *name = cJSON_GetObjectItem(metadata, "name");
        cJSON *desc = cJSON_GetObjectItem(metadata, "description");
        cJSON *source = cJSON_GetObjectItem(metadata, "source_file");
        cJSON *target = cJSON_GetObjectItem(metadata, "target_device");
        
        if (name && cJSON_IsString(name)) {
            p->name = strdup(cJSON_GetStringValue(name));
        }
        if (desc && cJSON_IsString(desc)) {
            p->description = strdup(cJSON_GetStringValue(desc));
        }
        if (source && cJSON_IsString(source)) {
            p->source_file = strdup(cJSON_GetStringValue(source));
        }
        if (target && cJSON_IsString(target)) {
            p->target_device = strdup(cJSON_GetStringValue(target));
        }
        /* TODO: 解析 ISO 8601 时间戳 */
    }
    
    p->content = plaintext;
    p->content_len = plaintext_len;
    p->sig_info = sig_info;
    
    *pack = p;
    result = TS_CONFIG_PACK_OK;
    
cleanup:
    if (root) cJSON_Delete(root);
    return result;
}

void ts_config_pack_free(ts_config_pack_t *pack)
{
    if (!pack) return;
    
    free(pack->name);
    free(pack->description);
    free(pack->content);
    free(pack->source_file);
    free(pack->target_device);
    free(pack);
}

/*===========================================================================*/
/*                          Create Functions                                  */
/*===========================================================================*/

ts_config_pack_result_t ts_config_pack_create(
    const char *name,
    const char *json_content,
    size_t json_len,
    const ts_config_pack_export_opts_t *opts,
    char **output,
    size_t *output_len)
{
    if (!name || !json_content || !opts || !output || !output_len) {
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    if (!s_initialized) {
        return TS_CONFIG_PACK_ERR_NOT_INIT;
    }
    
    /* 检查导出权限 */
    if (!ts_config_pack_can_export()) {
        ESP_LOGE(TAG, "This device is not authorized to export config packs");
        return TS_CONFIG_PACK_ERR_PERMISSION;
    }
    
    /* PEM parsers require a terminating NUL; callers may supply exact bytes. */
    if (!opts->recipient_cert_pem || !opts->recipient_cert_len || opts->recipient_cert_len > TS_CERT_PEM_MAX_LEN)
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    const char *terminator = memchr(opts->recipient_cert_pem, 0, opts->recipient_cert_len);
    if (terminator && terminator != opts->recipient_cert_pem + opts->recipient_cert_len - 1)
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    size_t recipient_length = opts->recipient_cert_len - (terminator != NULL);
    char *recipient_pem = TS_MALLOC_PSRAM(recipient_length + 1);
    if (!recipient_pem) return TS_CONFIG_PACK_ERR_NO_MEM;
    memcpy(recipient_pem, opts->recipient_cert_pem, recipient_length);
    recipient_pem[recipient_length] = 0;
    /* 解析接收方证书，获取公钥 */
    ts_keypair_t recipient_key = NULL;
    esp_err_t ret = ts_crypto_keypair_import(recipient_pem,
                                              recipient_length + 1,
                                              &recipient_key);
    if (ret != ESP_OK) {
        free(recipient_pem);
        ESP_LOGE(TAG, "Failed to parse recipient certificate");
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    /* 计算接收方证书指纹 */
    char recipient_fingerprint[CERT_FINGERPRINT_LEN + 1];
    ret = compute_cert_fingerprint(recipient_pem, recipient_length + 1,
                                    recipient_fingerprint, sizeof(recipient_fingerprint));
    free(recipient_pem);
    if (ret != ESP_OK) {
        ts_crypto_keypair_free(recipient_key);
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Export: recipient fingerprint = %s (cert_len=%zu)", 
             recipient_fingerprint, opts->recipient_cert_len);
    
    /* 生成临时密钥对 */
    ts_keypair_t ephemeral_key = NULL;
    ret = ts_crypto_keypair_generate(TS_CRYPTO_KEY_EC_P256, &ephemeral_key);
    if (ret != ESP_OK) {
        ts_crypto_keypair_free(recipient_key);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    /* 导出临时公钥 */
    uint8_t ephemeral_pubkey[ECDH_PUBKEY_LEN];
    size_t pubkey_len = sizeof(ephemeral_pubkey);
    ret = ts_crypto_keypair_export_public_raw(ephemeral_key, ephemeral_pubkey, &pubkey_len);
    if (ret != ESP_OK) {
        ts_crypto_keypair_free(ephemeral_key);
        ts_crypto_keypair_free(recipient_key);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    /* ECDH 密钥协商 */
    uint8_t shared_secret[32];
    size_t shared_len = sizeof(shared_secret);
    uint8_t recipient_pubkey[ECDH_PUBKEY_LEN];
    size_t recipient_pubkey_len=sizeof recipient_pubkey;
    ret=ts_crypto_keypair_export_public_raw(recipient_key,recipient_pubkey,&recipient_pubkey_len);
    if(ret==ESP_OK&&recipient_pubkey_len==sizeof recipient_pubkey)
        ret = ts_crypto_ecdh_compute_shared_raw(ephemeral_key,recipient_pubkey,recipient_pubkey_len,shared_secret,&shared_len);
    else ret=ESP_ERR_INVALID_ARG;
    
    /* 立即销毁临时私钥 */
    ts_crypto_keypair_free(ephemeral_key);
    ts_crypto_keypair_free(recipient_key);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ECDH key agreement failed");
        return TS_CONFIG_PACK_ERR_DECRYPT;
    }
    
    /* 生成随机 salt 和 IV */
    uint8_t salt[HKDF_SALT_LEN];
    uint8_t iv[AES_IV_LEN];
    if(ts_crypto_random(salt,sizeof salt)!=ESP_OK||ts_crypto_random(iv,sizeof iv)!=ESP_OK){
        memset(shared_secret,0,sizeof shared_secret);
        return TS_CONFIG_PACK_ERR_DECRYPT;
    }
    
    /* HKDF 密钥派生 */
    uint8_t aes_key[AES_KEY_LEN];
    ret = ts_crypto_hkdf(salt, sizeof(salt),
                          shared_secret, shared_len,
                          TS_CONFIG_PACK_HKDF_INFO, strlen(TS_CONFIG_PACK_HKDF_INFO),
                          aes_key, sizeof(aes_key));
    
    /* 清除 shared secret */
    memset(shared_secret, 0, sizeof(shared_secret));
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HKDF key derivation failed");
        return TS_CONFIG_PACK_ERR_DECRYPT;
    }
    
    /* AES-GCM 加密 */
    size_t ciphertext_len = json_len;
    uint8_t *ciphertext = TS_MALLOC_PSRAM(ciphertext_len);
    uint8_t tag[AES_TAG_LEN];
    
    if (!ciphertext) {
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    ret = ts_crypto_aes_gcm_encrypt(aes_key, sizeof(aes_key),
                                     iv, sizeof(iv),
                                     NULL, 0,  /* 无 AAD */
                                     json_content, json_len,
                                     ciphertext, tag);
    
    /* 清除 AES 密钥 */
    memset(aes_key, 0, sizeof(aes_key));
    
    if (ret != ESP_OK) {
        free(ciphertext);
        ESP_LOGE(TAG, "AES-GCM encryption failed");
        return TS_CONFIG_PACK_ERR_DECRYPT;
    }
    
    /* 计算内容哈希 */
    uint8_t content_hash[SHA256_LEN];
    ts_crypto_hash(TS_HASH_SHA256, json_content, json_len, content_hash, sizeof(content_hash));
    
    /* 获取设备私钥用于签名 - 使用堆分配避免栈溢出 */
    char *key_pem = TS_MALLOC_PSRAM(TS_CERT_KEY_MAX_LEN);
    if (!key_pem) {
        free(ciphertext);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    size_t key_len = TS_CERT_KEY_MAX_LEN;
    ret = ts_cert_get_private_key(key_pem, &key_len);
    if (ret != ESP_OK) {
        free(key_pem);
        free(ciphertext);
        return TS_CONFIG_PACK_ERR_PERMISSION;
    }
    
    ts_keypair_t signer_key = NULL;
    ret = ts_crypto_keypair_import(key_pem, key_len, &signer_key);
    memset(key_pem, 0, TS_CERT_KEY_MAX_LEN);  /* 清除私钥 */
    free(key_pem);
    
    if (ret != ESP_OK) {
        free(ciphertext);
        return TS_CONFIG_PACK_ERR_PERMISSION;
    }
    
    /* 计算待签名数据的哈希 */
    uint8_t data_hash[SHA256_LEN];
    if (ts_crypto_hash(TS_HASH_SHA256, ciphertext, ciphertext_len, data_hash, sizeof(data_hash)) != ESP_OK) {
        ts_crypto_keypair_free(signer_key);free(ciphertext);
        return TS_CONFIG_PACK_ERR_SIGNATURE;
    }
    
    /* ECDSA 签名 */
    uint8_t signature[MAX_SIGNATURE_LEN];
    size_t sig_len = sizeof(signature);
    ret = ts_crypto_ecdsa_sign(signer_key, data_hash, sizeof(data_hash),
                                signature, &sig_len);
    ts_crypto_keypair_free(signer_key);
    
    if (ret != ESP_OK) {
        free(ciphertext);
        ESP_LOGE(TAG, "ECDSA signing failed");
        return TS_CONFIG_PACK_ERR_PERMISSION;
    }
    
    /* 获取设备证书 - 使用堆分配避免栈溢出 */
    char *cert_pem = TS_MALLOC_PSRAM(TS_CERT_PEM_MAX_LEN);
    if (!cert_pem) {
        free(ciphertext);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    size_t cert_len = TS_CERT_PEM_MAX_LEN;
    ret = ts_cert_get_certificate(cert_pem, &cert_len);
    if (ret != ESP_OK) {
        free(cert_pem);
        free(ciphertext);
        return TS_CONFIG_PACK_ERR_PERMISSION;
    }
    
    /* 构建 JSON 输出 */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(ciphertext);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(root, "tscfg_version", TS_CONFIG_PACK_VERSION);
    cJSON_AddStringToObject(root, "format", "encrypted");
    
    /* metadata */
    cJSON *metadata = cJSON_CreateObject();
    cJSON_AddStringToObject(metadata, "name", name);
    if (opts->description) {
        cJSON_AddStringToObject(metadata, "description", opts->description);
    }
    
    /* 获取当前时间 */
    time_t now = time(NULL);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    cJSON_AddStringToObject(metadata, "created_at", time_str);
    
    /* 获取设备 ID */
    ts_cert_info_t cert_info;
    if (ts_cert_get_info(&cert_info) == ESP_OK) {
        cJSON_AddStringToObject(metadata, "created_by", cert_info.subject_cn);
    }
    
    /* TODO: 解析接收方证书获取 target_device */
    cJSON_AddStringToObject(metadata, "target_device", "unknown");
    
    char source_file[128];
    snprintf(source_file, sizeof(source_file), "%s.json", name);
    cJSON_AddStringToObject(metadata, "source_file", source_file);
    
    /* Base64 编码 content_hash */
    char hash_b64[64];
    size_t hash_b64_len = sizeof(hash_b64);
    ts_crypto_base64_encode(content_hash, sizeof(content_hash), hash_b64, &hash_b64_len);
    cJSON_AddStringToObject(metadata, "content_hash", hash_b64);
    
    cJSON_AddItemToObject(root, "metadata", metadata);
    
    /* encryption */
    cJSON *encryption = cJSON_CreateObject();
    cJSON_AddStringToObject(encryption, "algorithm", TS_CONFIG_PACK_ALGORITHM);
    cJSON_AddStringToObject(encryption, "kdf", TS_CONFIG_PACK_KDF);
    
    /* Base64 编码各字段 */
    char pubkey_b64[128], salt_b64[64], iv_b64[32], tag_b64[32];
    size_t b64_len;
    
    b64_len = sizeof(pubkey_b64);
    ts_crypto_base64_encode(ephemeral_pubkey, pubkey_len, pubkey_b64, &b64_len);
    cJSON_AddStringToObject(encryption, "ephemeral_public_key", pubkey_b64);
    
    b64_len = sizeof(salt_b64);
    ts_crypto_base64_encode(salt, sizeof(salt), salt_b64, &b64_len);
    cJSON_AddStringToObject(encryption, "salt", salt_b64);
    
    b64_len = sizeof(iv_b64);
    ts_crypto_base64_encode(iv, sizeof(iv), iv_b64, &b64_len);
    cJSON_AddStringToObject(encryption, "iv", iv_b64);
    
    b64_len = sizeof(tag_b64);
    ts_crypto_base64_encode(tag, sizeof(tag), tag_b64, &b64_len);
    cJSON_AddStringToObject(encryption, "tag", tag_b64);
    
    cJSON_AddStringToObject(encryption, "recipient_cert_fingerprint", recipient_fingerprint);
    cJSON_AddItemToObject(root, "encryption", encryption);
    
    /* signature */
    cJSON *sig_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(sig_obj, "algorithm", TS_CONFIG_PACK_SIG_ALGORITHM);
    cJSON_AddStringToObject(sig_obj, "signer_certificate", cert_pem);
    
    char sig_b64[128];
    b64_len = sizeof(sig_b64);
    ts_crypto_base64_encode(signature, sig_len, sig_b64, &b64_len);
    cJSON_AddStringToObject(sig_obj, "signature", sig_b64);
    cJSON_AddStringToObject(sig_obj, "signed_at", time_str);
    cJSON_AddBoolToObject(sig_obj, "is_official", true);  /* Developer 设备签名 */
    cJSON_AddItemToObject(root, "signature", sig_obj);
    
    /* payload (Base64 ciphertext) */
    size_t payload_b64_max = ((ciphertext_len + 2) / 3) * 4 + 1;
    char *payload_b64 = TS_MALLOC_PSRAM(payload_b64_max);
    if (!payload_b64) {
        free(cert_pem);
        free(ciphertext);
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    b64_len = payload_b64_max;
    ts_crypto_base64_encode(ciphertext, ciphertext_len, payload_b64, &b64_len);
    cJSON_AddStringToObject(root, "payload", payload_b64);
    
    free(ciphertext);
    free(payload_b64);
    
    /* 输出 JSON */
    char *json_output = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(cert_pem);  /* cert_pem 内容已复制到 JSON，可以释放 */
    
    if (!json_output) {
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    *output = json_output;
    *output_len = strlen(json_output);
    
    ESP_LOGI(TAG, "Created config pack '%s' (%zu bytes)", name, *output_len);
    return TS_CONFIG_PACK_OK;
}

esp_err_t ts_config_pack_save(const char *path, const char *tscfg_json, size_t tscfg_len)
{
    if (!path || !tscfg_json) {
        return ESP_ERR_INVALID_ARG;
    }
    
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    
    size_t written = fwrite(tscfg_json, 1, tscfg_len, f);
    fclose(f);
    
    if (written != tscfg_len) {
        ESP_LOGE(TAG, "Failed to write file: %s", path);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Saved config pack to %s", path);
    return ESP_OK;
}

/*===========================================================================*/
/*                          Verify Functions                                  */
/*===========================================================================*/

ts_config_pack_result_t ts_config_pack_verify(
    const char *path,
    ts_config_pack_sig_info_t *info)
{
    if (!path) {
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    /* 读取文件 */
    FILE *f = fopen(path, "r");
    if (!f) {
        return TS_CONFIG_PACK_ERR_IO;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > CONFIG_TS_CONFIG_PACK_MAX_SIZE) {
        fclose(f);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    char *json_buf = TS_MALLOC_PSRAM(file_size + 1);
    if (!json_buf) {
        fclose(f);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    fread(json_buf, 1, file_size, f);
    fclose(f);
    json_buf[file_size] = '\0';
    
    ts_config_pack_result_t result = ts_config_pack_verify_mem(json_buf, file_size, info);
    free(json_buf);
    
    return result;
}

ts_config_pack_result_t ts_config_pack_verify_mem(
    const char *tscfg_json,
    size_t tscfg_len,
    ts_config_pack_sig_info_t *info)
{
    if (!tscfg_json) {
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    cJSON *root = NULL;
    ts_config_pack_crypto_params_t params = {0};
    char *payload_b64 = NULL;
    char *signer_cert_pem = NULL;
    char *signature_b64 = NULL;
    ts_config_pack_sig_info_t sig_info = {0};
    
    ts_config_pack_result_t result = parse_tscfg_json(
        tscfg_json, tscfg_len,
        &root, &params, &payload_b64, &signer_cert_pem, &signature_b64, &sig_info);
    
    if (result != TS_CONFIG_PACK_OK) {
        if (root) cJSON_Delete(root);
        return result;
    }
    
    /* Base64 解码 payload */
    size_t payload_b64_len = strlen(payload_b64);
    size_t ciphertext_max_len = (payload_b64_len * 3) / 4 + 4;
    uint8_t *ciphertext = TS_MALLOC_PSRAM(ciphertext_max_len);
    if (!ciphertext) {
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    size_t ciphertext_len = ciphertext_max_len;
    if (ts_crypto_base64_decode(payload_b64, payload_b64_len,
                                 ciphertext, &ciphertext_len) != ESP_OK) {
        free(ciphertext);
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* Base64 解码签名 */
    size_t sig_b64_len = strlen(signature_b64);
    uint8_t signature[MAX_SIGNATURE_LEN];
    size_t sig_len = sizeof(signature);
    if (ts_crypto_base64_decode(signature_b64, sig_b64_len,
                                 signature, &sig_len) != ESP_OK) {
        free(ciphertext);
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* 验证签名 */
    result = verify_signature(signer_cert_pem, ciphertext, ciphertext_len,
                               signature, sig_len);
    free(ciphertext);
    cJSON_Delete(root);
    
    if (result == TS_CONFIG_PACK_OK && info) {
        sig_info.valid = true;
        *info = sig_info;
    }
    
    return result;
}

/*===========================================================================*/
/*                          Import Functions                                  */
/*===========================================================================*/

/** 配置包存储目录（与 .json 配置文件同目录，便于优先级处理） */
#define CONFIG_PACK_DIR "/sdcard/config"

/**
 * @brief 确保目录存在
 */
static esp_err_t ensure_dir_exists(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) == 0) {
        return ESP_OK;
    }
    
    /* 递归创建父目录 */
    char *parent = strdup(dir);
    if (!parent) return ESP_ERR_NO_MEM;
    
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        ensure_dir_exists(parent);
    }
    free(parent);
    
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create directory: %s", dir);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

ts_config_pack_result_t ts_config_pack_import(
    const char *tscfg_json,
    size_t tscfg_len,
    ts_config_pack_metadata_t *metadata,
    char *saved_path,
    size_t saved_path_len)
{
    if (!tscfg_json || tscfg_len == 0) {
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    if (!s_initialized) {
        return TS_CONFIG_PACK_ERR_NOT_INIT;
    }
    
    /* 解析 JSON 获取元数据和加密参数 */
    cJSON *root = NULL;
    ts_config_pack_crypto_params_t params = {0};
    char *payload_b64 = NULL;
    char *signer_cert_pem = NULL;
    char *signature_b64 = NULL;
    ts_config_pack_sig_info_t sig_info = {0};
    
    ts_config_pack_result_t result = parse_tscfg_json(
        tscfg_json, tscfg_len,
        &root, &params, &payload_b64, &signer_cert_pem, &signature_b64, &sig_info);
    
    if (result != TS_CONFIG_PACK_OK) {
        if (root) cJSON_Delete(root);
        return result;
    }
    
    /* 验证接收方指纹 - 必须是本设备 */
    if (strcmp(params.recipient_fingerprint, s_device_fingerprint) != 0) {
        ESP_LOGE(TAG, "Config pack not intended for this device");
        ESP_LOGE(TAG, "Device fingerprint: %s", s_device_fingerprint);
        ESP_LOGE(TAG, "Pack fingerprint:   %s", params.recipient_fingerprint);
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_RECIPIENT;
    }
    
    /* Base64 解码 payload 用于签名验证 */
    size_t payload_b64_len = strlen(payload_b64);
    size_t ciphertext_max_len = (payload_b64_len * 3) / 4 + 4;
    uint8_t *ciphertext = TS_MALLOC_PSRAM(ciphertext_max_len);
    if (!ciphertext) {
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    size_t ciphertext_len = ciphertext_max_len;
    if (ts_crypto_base64_decode(payload_b64, payload_b64_len,
                                 ciphertext, &ciphertext_len) != ESP_OK) {
        free(ciphertext);
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* Base64 解码签名 */
    size_t sig_b64_len = strlen(signature_b64);
    uint8_t signature[MAX_SIGNATURE_LEN];
    size_t sig_len = sizeof(signature);
    if (ts_crypto_base64_decode(signature_b64, sig_b64_len,
                                 signature, &sig_len) != ESP_OK) {
        free(ciphertext);
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* 验证签名 */
    result = verify_signature(signer_cert_pem, ciphertext, ciphertext_len,
                               signature, sig_len);
    free(ciphertext);
    
    if (result != TS_CONFIG_PACK_OK) {
        cJSON_Delete(root);
        return result;
    }
    
    sig_info.valid = true;
    
    /* 提取元数据（不解密） */
    char name[64] = "unnamed";
    char description[128] = "";
    char source_file[64] = "";
    char target_device[64] = "";
    int64_t created_at = 0;
    
    cJSON *meta = cJSON_GetObjectItem(root, "metadata");
    if (meta) {
        cJSON *j_name = cJSON_GetObjectItem(meta, "name");
        cJSON *j_desc = cJSON_GetObjectItem(meta, "description");
        cJSON *j_src = cJSON_GetObjectItem(meta, "source_file");
        cJSON *j_target = cJSON_GetObjectItem(meta, "target_device");
        cJSON *j_created = cJSON_GetObjectItem(meta, "created_at");
        
        if (j_name && cJSON_IsString(j_name)) {
            strncpy(name, j_name->valuestring, sizeof(name) - 1);
        }
        if (j_desc && cJSON_IsString(j_desc)) {
            strncpy(description, j_desc->valuestring, sizeof(description) - 1);
        }
        if (j_src && cJSON_IsString(j_src)) {
            strncpy(source_file, j_src->valuestring, sizeof(source_file) - 1);
        }
        if (j_target && cJSON_IsString(j_target)) {
            strncpy(target_device, j_target->valuestring, sizeof(target_device) - 1);
        }
        if (j_created && cJSON_IsString(j_created)) {
            /* ISO 8601 时间戳解析 */
            int year, month, day, hour, min, sec;
            if (sscanf(j_created->valuestring, "%d-%d-%dT%d:%d:%dZ",
                       &year, &month, &day, &hour, &min, &sec) == 6) {
                struct tm tm_info = {
                    .tm_year = year - 1900,
                    .tm_mon = month - 1,
                    .tm_mday = day,
                    .tm_hour = hour,
                    .tm_min = min,
                    .tm_sec = sec,
                    .tm_isdst = 0
                };
                char *old_tz = getenv("TZ");
                setenv("TZ", "UTC0", 1);
                tzset();
                created_at = mktime(&tm_info);
                if (old_tz) {
                    setenv("TZ", old_tz, 1);
                } else {
                    unsetenv("TZ");
                }
                tzset();
            }
        }
    }
    
    cJSON_Delete(root);
    
    /* 确保目录存在 */
    if (ensure_dir_exists(CONFIG_PACK_DIR) != ESP_OK) {
        return TS_CONFIG_PACK_ERR_IO;
    }
    
    /* 构建保存路径 */
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.tscfg", CONFIG_PACK_DIR, name);
    
    /* 保存加密的配置包文件（原样保存，不解密） */
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
        return TS_CONFIG_PACK_ERR_IO;
    }
    
    size_t written = fwrite(tscfg_json, 1, tscfg_len, f);
    fclose(f);
    
    if (written != tscfg_len) {
        ESP_LOGE(TAG, "Failed to write config pack");
        return TS_CONFIG_PACK_ERR_IO;
    }
    
    ESP_LOGI(TAG, "Config pack imported: %s -> %s", name, path);
    
    /* 填充返回的元数据 */
    if (metadata) {
        strncpy(metadata->name, name, sizeof(metadata->name) - 1);
        strncpy(metadata->description, description, sizeof(metadata->description) - 1);
        strncpy(metadata->source_file, source_file, sizeof(metadata->source_file) - 1);
        strncpy(metadata->target_device, target_device, sizeof(metadata->target_device) - 1);
        metadata->created_at = created_at;
        metadata->sig_info = sig_info;
    }
    
    if (saved_path && saved_path_len > 0) {
        strncpy(saved_path, path, saved_path_len - 1);
        saved_path[saved_path_len - 1] = '\0';
    }
    
    return TS_CONFIG_PACK_OK;
}

ts_config_pack_result_t ts_config_pack_validate_file(
    const char *path,
    ts_config_pack_metadata_t *metadata)
{
    if (!path) {
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    if (!s_initialized) {
        return TS_CONFIG_PACK_ERR_NOT_INIT;
    }
    
    /* 读取文件内容 */
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return TS_CONFIG_PACK_ERR_IO;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > 65536) {
        fclose(f);
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    char *file_buf = TS_MALLOC_PSRAM(file_size + 1);
    if (!file_buf) {
        fclose(f);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    size_t read_len = fread(file_buf, 1, file_size, f);
    fclose(f);
    
    if (read_len != (size_t)file_size) {
        free(file_buf);
        return TS_CONFIG_PACK_ERR_IO;
    }
    file_buf[file_size] = '\0';
    
    /* 解析 JSON 获取元数据和加密参数 */
    cJSON *root = NULL;
    ts_config_pack_crypto_params_t params = {0};
    char *payload_b64 = NULL;
    char *signer_cert_pem = NULL;
    char *signature_b64 = NULL;
    ts_config_pack_sig_info_t sig_info = {0};
    
    ts_config_pack_result_t result = parse_tscfg_json(
        file_buf, file_size,
        &root, &params, &payload_b64, &signer_cert_pem, &signature_b64, &sig_info);
    
    free(file_buf);
    
    if (result != TS_CONFIG_PACK_OK) {
        if (root) cJSON_Delete(root);
        return result;
    }
    
    /* 验证接收方指纹 - 必须是本设备 */
    if (strcmp(params.recipient_fingerprint, s_device_fingerprint) != 0) {
        ESP_LOGE(TAG, "Config pack not intended for this device");
        ESP_LOGE(TAG, "Device fingerprint: %s", s_device_fingerprint);
        ESP_LOGE(TAG, "Pack fingerprint:   %s", params.recipient_fingerprint);
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_RECIPIENT;
    }
    
    /* Base64 解码 payload 用于签名验证 */
    size_t payload_b64_len = strlen(payload_b64);
    size_t ciphertext_max_len = (payload_b64_len * 3) / 4 + 4;
    uint8_t *ciphertext = TS_MALLOC_PSRAM(ciphertext_max_len);
    if (!ciphertext) {
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    size_t ciphertext_len = ciphertext_max_len;
    if (ts_crypto_base64_decode(payload_b64, payload_b64_len,
                                 ciphertext, &ciphertext_len) != ESP_OK) {
        free(ciphertext);
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* Base64 解码签名 */
    size_t sig_b64_len = strlen(signature_b64);
    uint8_t signature[MAX_SIGNATURE_LEN];
    size_t sig_len = sizeof(signature);
    if (ts_crypto_base64_decode(signature_b64, sig_b64_len,
                                 signature, &sig_len) != ESP_OK) {
        free(ciphertext);
        cJSON_Delete(root);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* 验证签名 */
    result = verify_signature(signer_cert_pem, ciphertext, ciphertext_len,
                               signature, sig_len);
    free(ciphertext);
    
    if (result != TS_CONFIG_PACK_OK) {
        cJSON_Delete(root);
        return result;
    }
    
    sig_info.valid = true;
    
    /* 提取元数据（不解密） */
    char name[64] = "unnamed";
    char description[128] = "";
    char source_file[64] = "";
    char target_device[64] = "";
    int64_t created_at = 0;
    
    cJSON *meta = cJSON_GetObjectItem(root, "metadata");
    if (meta) {
        cJSON *j_name = cJSON_GetObjectItem(meta, "name");
        cJSON *j_desc = cJSON_GetObjectItem(meta, "description");
        cJSON *j_src = cJSON_GetObjectItem(meta, "source_file");
        cJSON *j_target = cJSON_GetObjectItem(meta, "target_device");
        cJSON *j_created = cJSON_GetObjectItem(meta, "created_at");
        
        if (j_name && cJSON_IsString(j_name)) {
            strncpy(name, j_name->valuestring, sizeof(name) - 1);
        }
        if (j_desc && cJSON_IsString(j_desc)) {
            strncpy(description, j_desc->valuestring, sizeof(description) - 1);
        }
        if (j_src && cJSON_IsString(j_src)) {
            strncpy(source_file, j_src->valuestring, sizeof(source_file) - 1);
        }
        if (j_target && cJSON_IsString(j_target)) {
            strncpy(target_device, j_target->valuestring, sizeof(target_device) - 1);
        }
        if (j_created && cJSON_IsString(j_created)) {
            /* ISO 8601 时间戳解析 */
            int year, month, day, hour, min, sec;
            if (sscanf(j_created->valuestring, "%d-%d-%dT%d:%d:%dZ",
                       &year, &month, &day, &hour, &min, &sec) == 6) {
                struct tm tm_info = {
                    .tm_year = year - 1900,
                    .tm_mon = month - 1,
                    .tm_mday = day,
                    .tm_hour = hour,
                    .tm_min = min,
                    .tm_sec = sec,
                    .tm_isdst = 0
                };
                char *old_tz = getenv("TZ");
                setenv("TZ", "UTC0", 1);
                tzset();
                created_at = mktime(&tm_info);
                if (old_tz) {
                    setenv("TZ", old_tz, 1);
                } else {
                    unsetenv("TZ");
                }
                tzset();
            }
        }
    }
    
    cJSON_Delete(root);
    
    /* 填充返回的元数据 */
    if (metadata) {
        strncpy(metadata->name, name, sizeof(metadata->name) - 1);
        strncpy(metadata->description, description, sizeof(metadata->description) - 1);
        strncpy(metadata->source_file, source_file, sizeof(metadata->source_file) - 1);
        strncpy(metadata->target_device, target_device, sizeof(metadata->target_device) - 1);
        metadata->created_at = created_at;
        metadata->sig_info = sig_info;
    }
    
    ESP_LOGI(TAG, "Config pack validated: %s at %s", name, path);
    return TS_CONFIG_PACK_OK;
}

ts_config_pack_result_t ts_config_pack_apply_file(
    const char *path,
    cJSON **applied_modules)
{
    if (!path) {
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    if (!s_initialized) {
        return TS_CONFIG_PACK_ERR_NOT_INIT;
    }
    
    /* 读取并解密配置包 */
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return TS_CONFIG_PACK_ERR_IO;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > 65536) {
        fclose(f);
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    char *file_buf = TS_MALLOC_PSRAM(file_size + 1);
    if (!file_buf) {
        fclose(f);
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    size_t read_len = fread(file_buf, 1, file_size, f);
    fclose(f);
    
    if (read_len != (size_t)file_size) {
        free(file_buf);
        return TS_CONFIG_PACK_ERR_IO;
    }
    file_buf[file_size] = '\0';
    
    /* 解析并解密 */
    ts_config_pack_t *pack = NULL;
    ts_config_pack_result_t result = ts_config_pack_load_mem(file_buf, file_size, &pack);
    free(file_buf);
    
    if (result != TS_CONFIG_PACK_OK) {
        ESP_LOGE(TAG, "Failed to load config pack: %s", ts_config_pack_strerror(result));
        return result;
    }
    
    if (!pack || !pack->content) {
        ESP_LOGE(TAG, "No content in config pack");
        if (pack) ts_config_pack_free(pack);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* 解析解密后的配置内容 */
    cJSON *config = cJSON_Parse(pack->content);
    if (!config) {
        ESP_LOGE(TAG, "Failed to parse decrypted config content");
        ts_config_pack_free(pack);
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* 应用配置模块 */
    cJSON *modules = cJSON_CreateArray();
    
    /* TODO: 遍历配置模块并应用 */
    /* 示例模块: network, led, fan, security 等 */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, config) {
        const char *key = item->string;
        if (key) {
            cJSON_AddItemToArray(modules, cJSON_CreateString(key));
            ESP_LOGI(TAG, "Applied config module: %s", key);
            
            /* TODO: 调用各模块的配置应用函数 */
            /* 例如: ts_net_apply_config(item), ts_led_apply_config(item) 等 */
        }
    }
    
    cJSON_Delete(config);
    ts_config_pack_free(pack);
    
    if (applied_modules) {
        *applied_modules = modules;
    } else {
        cJSON_Delete(modules);
    }
    
    ESP_LOGI(TAG, "Config pack applied: %s", path);
    return TS_CONFIG_PACK_OK;
}

esp_err_t ts_config_pack_list(
    char (*names)[64],
    size_t max_count,
    size_t *count)
{
    if (!names || !count || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *count = 0;
    
    DIR *dir = opendir(CONFIG_PACK_DIR);
    if (!dir) {
        /* 目录不存在是正常的（没有导入过配置包） */
        return ESP_OK;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *count < max_count) {
        /* 跳过 . 和 .. */
        if (entry->d_name[0] == '.') continue;
        
        /* 检查 .tscfg 扩展名 */
        size_t name_len = strlen(entry->d_name);
        if (name_len > 6 && strcmp(entry->d_name + name_len - 6, ".tscfg") == 0) {
            /* 复制名字（去掉扩展名） */
            size_t copy_len = name_len - 6;
            if (copy_len >= 64) copy_len = 63;
            strncpy(names[*count], entry->d_name, copy_len);
            names[*count][copy_len] = '\0';
            (*count)++;
        }
    }
    
    closedir(dir);
    return ESP_OK;
}

ts_config_pack_result_t ts_config_pack_get_content(
    const char *name,
    char **content,
    size_t *content_len)
{
    if (!name || !content || !content_len) {
        return TS_CONFIG_PACK_ERR_INVALID_ARG;
    }
    
    /* 构建文件路径 */
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.tscfg", CONFIG_PACK_DIR, name);
    
    /* 使用现有的加载函数（会解密） */
    ts_config_pack_t *pack = NULL;
    ts_config_pack_result_t result = ts_config_pack_load(path, &pack);
    
    if (result != TS_CONFIG_PACK_OK) {
        return result;
    }
    
    /* 移交内容所有权 */
    *content = pack->content;
    *content_len = pack->content_len;
    pack->content = NULL;  /* 防止 free 释放 */
    
    ts_config_pack_free(pack);
    return TS_CONFIG_PACK_OK;
}

/*===========================================================================*/
/*                          Utility Functions                                 */
/*===========================================================================*/

esp_err_t ts_config_pack_export_device_cert(char *cert_pem, size_t *cert_len)
{
    return ts_cert_get_certificate(cert_pem, cert_len);
}

esp_err_t ts_config_pack_get_cert_fingerprint(char *fingerprint, size_t len)
{
    if (!fingerprint || len < CERT_FINGERPRINT_LEN + 1) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_initialized || s_device_fingerprint[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    
    strncpy(fingerprint, s_device_fingerprint, len - 1);
    fingerprint[len - 1] = '\0';
    return ESP_OK;
}

/*===========================================================================*/
/*                          Internal Functions                                */
/*===========================================================================*/

/**
 * @brief 计算证书 SHA-256 指纹
 * 
 * 注意：只计算实际 PEM 内容的哈希，去除尾部空白和 null 终止符，
 * 确保相同证书无论复制方式如何都能得到一致的指纹。
 */
static esp_err_t compute_cert_fingerprint(const char *cert_pem, size_t cert_len,
                                           char *fingerprint, size_t fp_len)
{
    if (!cert_pem || !fingerprint || fp_len < CERT_FINGERPRINT_LEN + 1) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 计算实际内容长度，去除尾部空白和 null */
    size_t actual_len = strnlen(cert_pem, cert_len);
    while (actual_len > 0 && 
           (cert_pem[actual_len - 1] == ' ' || 
            cert_pem[actual_len - 1] == '\t' ||
            cert_pem[actual_len - 1] == '\r' ||
            cert_pem[actual_len - 1] == '\n')) {
        actual_len--;
    }
    
    if (actual_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 计算 PEM 内容的 SHA-256（不包含尾部空白） */
    uint8_t hash[SHA256_LEN];
    esp_err_t ret = ts_crypto_hash(TS_HASH_SHA256, cert_pem, actual_len, hash, sizeof(hash));
    if (ret != ESP_OK) {
        return ret;
    }
    
    /* 转换为 hex 字符串 */
    return ts_crypto_hex_encode(hash, sizeof(hash), fingerprint, fp_len);
}

/**
 * @brief 解析 .tscfg JSON 文件
 */
static ts_config_pack_result_t parse_tscfg_json(
    const char *json, size_t len,
    cJSON **root,
    ts_config_pack_crypto_params_t *params,
    char **payload_b64,
    char **signer_cert_pem,
    char **signature_b64,
    ts_config_pack_sig_info_t *sig_info)
{
    *root = ts_config_pack_parse_json(json, len);
    if (!cJSON_IsObject(*root)) {
        ESP_LOGE(TAG, "JSON parse error");
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* 验证版本 */
    cJSON *version = cJSON_GetObjectItemCaseSensitive(*root, "tscfg_version");
    if (!version || !cJSON_IsString(version)) {
        ESP_LOGE(TAG, "Missing tscfg_version");
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    if (strcmp(cJSON_GetStringValue(version), TS_CONFIG_PACK_VERSION) != 0) {
        ESP_LOGE(TAG, "Unsupported version: %s", cJSON_GetStringValue(version));
        return TS_CONFIG_PACK_ERR_VERSION;
    }
    
    /* 提取加密参数 */
    cJSON *encryption = cJSON_GetObjectItemCaseSensitive(*root, "encryption");
    if (!cJSON_IsObject(encryption)) {
        ESP_LOGE(TAG, "Missing encryption section");
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    cJSON *ephemeral_pub = cJSON_GetObjectItemCaseSensitive(encryption, "ephemeral_public_key");
    cJSON *salt = cJSON_GetObjectItemCaseSensitive(encryption, "salt");
    cJSON *iv = cJSON_GetObjectItemCaseSensitive(encryption, "iv");
    cJSON *tag = cJSON_GetObjectItemCaseSensitive(encryption, "tag");
    cJSON *recipient_fp = cJSON_GetObjectItemCaseSensitive(encryption, "recipient_cert_fingerprint");
    cJSON *algorithm = cJSON_GetObjectItemCaseSensitive(encryption, "algorithm");
    cJSON *kdf = cJSON_GetObjectItemCaseSensitive(encryption, "kdf");

    if (!cJSON_IsString(ephemeral_pub) || !cJSON_IsString(salt) || !cJSON_IsString(iv) ||
        !cJSON_IsString(tag) || !cJSON_IsString(recipient_fp) ||
        !cJSON_IsString(algorithm) || strcmp(algorithm->valuestring, TS_CONFIG_PACK_ALGORITHM) ||
        !cJSON_IsString(kdf) || strcmp(kdf->valuestring, TS_CONFIG_PACK_KDF) ||
        strlen(recipient_fp->valuestring) != CERT_FINGERPRINT_LEN) {
        ESP_LOGE(TAG, "Missing encryption parameters");
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    /* Base64 解码加密参数 */
    const char *pub_str = cJSON_GetStringValue(ephemeral_pub);
    size_t out_len = sizeof(params->ephemeral_pubkey);
    if (ts_crypto_base64_decode(pub_str, strlen(pub_str), 
                                 params->ephemeral_pubkey, &out_len) != ESP_OK ||
        out_len != sizeof(params->ephemeral_pubkey) || params->ephemeral_pubkey[0] != 4) {
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    out_len = sizeof(params->salt);
    if (ts_crypto_base64_decode(cJSON_GetStringValue(salt), strlen(cJSON_GetStringValue(salt)),
                                 params->salt, &out_len) != ESP_OK || out_len != sizeof(params->salt)) {
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    out_len = sizeof(params->iv);
    if (ts_crypto_base64_decode(cJSON_GetStringValue(iv), strlen(cJSON_GetStringValue(iv)),
                                 params->iv, &out_len) != ESP_OK || out_len != sizeof(params->iv)) {
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    out_len = sizeof(params->tag);
    if (ts_crypto_base64_decode(cJSON_GetStringValue(tag), strlen(cJSON_GetStringValue(tag)),
                                 params->tag, &out_len) != ESP_OK || out_len != sizeof(params->tag)) {
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    strncpy(params->recipient_fingerprint, cJSON_GetStringValue(recipient_fp),
            sizeof(params->recipient_fingerprint) - 1);
    
    /* 提取 payload */
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(*root, "payload");
    if (!cJSON_IsString(payload) || !payload->valuestring[0]) {
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    *payload_b64 = cJSON_GetStringValue(payload);
    
    /* 提取签名信息 */
    cJSON *signature_obj = cJSON_GetObjectItemCaseSensitive(*root, "signature");
    if (!cJSON_IsObject(signature_obj)) {
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    cJSON *signer_cert = cJSON_GetObjectItemCaseSensitive(signature_obj, "signer_certificate");
    cJSON *sig = cJSON_GetObjectItemCaseSensitive(signature_obj, "signature");
    cJSON *is_official = cJSON_GetObjectItemCaseSensitive(signature_obj, "is_official");
    const cJSON *sig_algorithm = cJSON_GetObjectItemCaseSensitive(signature_obj, "algorithm");
    
    if (!cJSON_IsString(signer_cert) || !signer_cert->valuestring[0] ||
        strlen(signer_cert->valuestring) >= TS_CERT_PEM_MAX_LEN ||
        !cJSON_IsString(sig) || !sig->valuestring[0] || !cJSON_IsString(sig_algorithm) ||
        strcmp(sig_algorithm->valuestring, TS_CONFIG_PACK_SIG_ALGORITHM)) {
        return TS_CONFIG_PACK_ERR_PARSE;
    }
    
    *signer_cert_pem = cJSON_GetStringValue(signer_cert);
    *signature_b64 = cJSON_GetStringValue(sig);
    
    /* 填充签名信息 */
    if (sig_info) {
        sig_info->valid = false;  /* 稍后验证 */
        (void)is_official;
        sig_info->is_official = false; /* Unauthenticated metadata is never authorization. */
        sig_info->signed_at = 0;  /* 默认值 */
        
        /* Outer signed_at is unauthenticated display metadata; do not change process timezone. */
        /* 解析签名者证书获取 CN 和 OU */
        ts_cert_info_t cert_info;
        if (ts_cert_parse_certificate(*signer_cert_pem, strlen(*signer_cert_pem) + 1, 
                                       &cert_info) == ESP_OK) {
            strncpy(sig_info->signer_cn, cert_info.subject_cn, sizeof(sig_info->signer_cn) - 1);
            strncpy(sig_info->signer_ou, cert_info.subject_ou, sizeof(sig_info->signer_ou) - 1);
            /* 根据 OU 字段更新 is_official 标记 */
            /* Only the verified rule-pack path may report trusted Developer status. */
        }
    }
    
    return TS_CONFIG_PACK_OK;
}

/**
 * @brief 验证签名
 */
static ts_config_pack_result_t verify_signature(
    const char *signer_cert_pem,
    const uint8_t *data_to_sign,
    size_t data_len,
    const uint8_t *signature,
    size_t sig_len)
{
    /* 导入签名者公钥 */
    ts_keypair_t signer_key = NULL;
    esp_err_t ret = ts_crypto_keypair_import(signer_cert_pem, strlen(signer_cert_pem) + 1,
                                              &signer_key);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse signer certificate");
        return TS_CONFIG_PACK_ERR_CERT_CHAIN;
    }
    
    /* 计算数据哈希 */
    uint8_t hash[SHA256_LEN];
    if (ts_crypto_hash(TS_HASH_SHA256, data_to_sign, data_len, hash, sizeof(hash)) != ESP_OK) {
        ts_crypto_keypair_free(signer_key);
        return TS_CONFIG_PACK_ERR_SIGNATURE;
    }
    
    /* 验证签名 */
    ret = ts_crypto_ecdsa_verify(signer_key, hash, sizeof(hash), signature, sig_len);
    ts_crypto_keypair_free(signer_key);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Signature verification failed");
        return TS_CONFIG_PACK_ERR_SIGNATURE;
    }
    
    /* 
     * TODO: 验证证书链
     * 签名本身已验证成功，但还应该验证签名者证书是否由受信任的 CA 签发
     */
#if CONFIG_TS_CONFIG_PACK_VERIFY_CERT_CHAIN
    /* 
     * 应该验证 signer_cert_pem 是由受信任的 CA 签发的
     * 这需要加载 CA 链并进行验证
     */
    ESP_LOGD(TAG, "Certificate chain verification not yet implemented (signature OK)");
#endif
    
    return TS_CONFIG_PACK_OK;
}

/**
 * @brief 解密 payload
 */
static ts_config_pack_result_t decrypt_payload_key(
    const ts_config_pack_crypto_params_t *params,
    const char *key_pem,
    const uint8_t *ciphertext,
    size_t ciphertext_len,
    char **plaintext,
    size_t *plaintext_len)
{
    ts_keypair_t device_key = NULL;
    esp_err_t ret = ts_crypto_keypair_import(key_pem, strlen(key_pem) + 1, &device_key);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to import device key");
        return TS_CONFIG_PACK_ERR_RECIPIENT;
    }
    
    /* ECDH 密钥协商 */
    uint8_t shared_secret[32];
    size_t shared_len = sizeof(shared_secret);
    ret = ts_crypto_ecdh_compute_shared_raw(device_key,
                                             params->ephemeral_pubkey,
                                             ECDH_PUBKEY_LEN,
                                             shared_secret, &shared_len);
    ts_crypto_keypair_free(device_key);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ECDH key agreement failed");
        return TS_CONFIG_PACK_ERR_DECRYPT;
    }
    
    /* HKDF 密钥派生 */
    uint8_t aes_key[AES_KEY_LEN];
    ret = ts_crypto_hkdf(params->salt, HKDF_SALT_LEN,
                          shared_secret, shared_len,
                          TS_CONFIG_PACK_HKDF_INFO, strlen(TS_CONFIG_PACK_HKDF_INFO),
                          aes_key, sizeof(aes_key));
    
    memset(shared_secret, 0, sizeof(shared_secret));
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HKDF key derivation failed");
        return TS_CONFIG_PACK_ERR_DECRYPT;
    }
    
    /* AES-GCM 解密 */
    char *decrypted = TS_MALLOC_PSRAM(ciphertext_len + 1);
    if (!decrypted) {
        memset(aes_key, 0, sizeof(aes_key));
        return TS_CONFIG_PACK_ERR_NO_MEM;
    }
    
    ret = ts_crypto_aes_gcm_decrypt(aes_key, sizeof(aes_key),
                                     params->iv, AES_IV_LEN,
                                     NULL, 0,  /* 无 AAD */
                                     ciphertext, ciphertext_len,
                                     params->tag, decrypted);
    
    memset(aes_key, 0, sizeof(aes_key));
    
    if (ret != ESP_OK) {
        free(decrypted);
        ESP_LOGE(TAG, "AES-GCM decryption failed");
        return TS_CONFIG_PACK_ERR_DECRYPT;
    }
    
    decrypted[ciphertext_len] = '\0';
    *plaintext = decrypted;
    *plaintext_len = ciphertext_len;
    
    return TS_CONFIG_PACK_OK;
}

static ts_config_pack_result_t decrypt_payload(const ts_config_pack_crypto_params_t *params,
    const uint8_t *ciphertext, size_t ciphertext_len, char **plaintext, size_t *plaintext_len)
{
    char *key = TS_MALLOC_PSRAM(TS_CERT_KEY_MAX_LEN);
    if (!key) return TS_CONFIG_PACK_ERR_NO_MEM;
    size_t len = TS_CERT_KEY_MAX_LEN;
    ts_config_pack_result_t ret = TS_CONFIG_PACK_ERR_RECIPIENT;
    if (ts_cert_get_private_key(key, &len) == ESP_OK)
        ret = decrypt_payload_key(params, key, ciphertext, ciphertext_len, plaintext, plaintext_len);
    memset(key, 0, TS_CERT_KEY_MAX_LEN); free(key);
    return ret;
}

ts_config_pack_result_t ts_config_pack_load_verified_mem(const char *text, size_t length,
    const ts_config_pack_acceptance_t *accepted, ts_config_pack_t **pack,
    ts_config_pack_acceptance_t *acceptance, uint32_t *credential_generation)
{
    if (!pack) return TS_CONFIG_PACK_ERR_INVALID_ARG;
    *pack = NULL;
    cJSON *root = NULL;
    ts_config_pack_crypto_params_t params = {0};
    char *payload = NULL, *signer = NULL, *signature_text = NULL;
    ts_config_pack_sig_info_t info = {0};
    ts_cert_snapshot_t snapshot = {0};
    unsigned char *ciphertext = NULL;
    char *plaintext = NULL;
    ts_config_pack_acceptance_t local = {0};
    ts_config_pack_result_t ret = parse_tscfg_json(text, length, &root, &params,
                                                 &payload, &signer, &signature_text, &info);
    if (ret != TS_CONFIG_PACK_OK) goto done;
    esp_err_t snapshot_result = ts_cert_get_pack_snapshot(&snapshot);
    if (snapshot_result != ESP_OK) {
        ret = snapshot_result == ESP_ERR_NO_MEM ? TS_CONFIG_PACK_ERR_NO_MEM : TS_CONFIG_PACK_ERR_CERT_CHAIN; goto done;
    }
    char recipient[65], hash[65];
    unsigned char digest[32];
    if (compute_cert_fingerprint(snapshot.certificate, strlen(snapshot.certificate) + 1,
                                 recipient, sizeof recipient) != ESP_OK ||
        ts_crypto_hash(TS_HASH_SHA256, text, length, digest, sizeof digest) != ESP_OK ||
        ts_crypto_hex_encode(digest, sizeof digest, hash, sizeof hash) != ESP_OK) {
        ret = TS_CONFIG_PACK_ERR_INVALID_ARG; goto done;
    }
    if (strcmp(recipient, params.recipient_fingerprint)) { ret = TS_CONFIG_PACK_ERR_RECIPIENT; goto done; }
    ret = ts_config_pack_signer_trust(signer, &snapshot, hash, recipient, accepted, &local);
    if (ret != TS_CONFIG_PACK_OK) goto done;
    size_t cipher_len = strlen(payload) * 3 / 4 + 4;
    ciphertext = TS_MALLOC_PSRAM(cipher_len);
    if (!ciphertext) { ret = TS_CONFIG_PACK_ERR_NO_MEM; goto done; }
    unsigned char signature[MAX_SIGNATURE_LEN]; size_t sig_len = sizeof signature;
    if (ts_crypto_base64_decode(payload, strlen(payload), ciphertext, &cipher_len) != ESP_OK || !cipher_len ||
        ts_crypto_base64_decode(signature_text, strlen(signature_text), signature, &sig_len) != ESP_OK || !sig_len) {
        ret = TS_CONFIG_PACK_ERR_PARSE; goto done;
    }
    ret = verify_signature(signer, ciphertext, cipher_len, signature, sig_len);
    if (ret != TS_CONFIG_PACK_OK) goto done;
    size_t plain_len = 0;
    ret = decrypt_payload_key(&params, snapshot.key, ciphertext, cipher_len, &plaintext, &plain_len);
    if (ret != TS_CONFIG_PACK_OK) goto done;
    cJSON *content = ts_config_pack_parse_json(plaintext, plain_len);
    if (!content) { ret = TS_CONFIG_PACK_ERR_PARSE; goto done; }
    cJSON_Delete(content);
    ts_cert_pki_status_t status = {0};
    if (ts_cert_get_status(&status) != ESP_OK || status.generation != snapshot.generation) {
        ret = TS_CONFIG_PACK_ERR_CREDENTIAL_CHANGED; goto done;
    }
    ts_config_pack_t *loaded = TS_CALLOC_PSRAM(1, sizeof(*loaded));
    if (!loaded) { ret = TS_CONFIG_PACK_ERR_NO_MEM; goto done; }
    loaded->content = plaintext; plaintext = NULL; loaded->content_len = plain_len;
    info.valid = info.is_official = true; loaded->sig_info = info;
    if (acceptance) *acceptance = local;
    if (credential_generation) *credential_generation = snapshot.generation;
    *pack = loaded;
done:
    cJSON_Delete(root); free(ciphertext); free(plaintext); ts_cert_free_snapshot(&snapshot);
    return ret;
}

// ============================================================================
// 通用配置加载辅助函数 (Generic Config Loading Helpers)
// ============================================================================

/**
 * @brief 检查 .tscfg 版本是否存在
 * 
 * 给定 .json 文件路径，检查是否存在对应的 .tscfg 加密版本
 */
bool ts_config_pack_tscfg_exists(const char *json_path)
{
    if (!json_path) {
        return false;
    }
    
    // 检查是否以 .json 结尾
    size_t len = strlen(json_path);
    if (len < 5 || strcmp(json_path + len - 5, ".json") != 0) {
        return false;
    }
    
    // 构建 .tscfg 路径
    char tscfg_path[256];
    size_t base_len = len - 5;  // 去掉 ".json"
    if (base_len + 6 >= sizeof(tscfg_path)) {  // +6 for ".tscfg"
        return false;
    }
    
    memcpy(tscfg_path, json_path, base_len);
    strcpy(tscfg_path + base_len, ".tscfg");
    
    // 检查文件是否存在
    struct stat st;
    return (stat(tscfg_path, &st) == 0);
}

/**
 * @brief 智能加载配置文件（.tscfg 优先）
 * 
 * 给定 .json 文件路径，优先加载对应的 .tscfg 加密版本（如存在）
 * 否则回退到加载普通 .json 文件
 * 
 * @param json_path      原始 .json 文件路径
 * @param content        输出：文件内容（调用者负责释放）
 * @param content_len    输出：内容长度（可选，可传 NULL）
 * @param used_tscfg     输出：是否使用了 .tscfg 版本（可选，可传 NULL）
 * @return ESP_OK 成功，其他表示错误
 */
esp_err_t ts_config_pack_load_with_priority(
    const char *json_path,
    char **content,
    size_t *content_len,
    bool *used_tscfg)
{
    if (!json_path || !content) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *content = NULL;
    if (content_len) *content_len = 0;
    if (used_tscfg) *used_tscfg = false;
    
    // 检查是否以 .json 结尾
    size_t len = strlen(json_path);
    if (len < 5 || strcmp(json_path + len - 5, ".json") != 0) {
        ESP_LOGE(TAG, "Invalid path (not .json): %s", json_path);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 构建 .tscfg 路径
    char tscfg_path[256];
    size_t base_len = len - 5;
    if (base_len + 6 >= sizeof(tscfg_path)) {
        ESP_LOGE(TAG, "Path too long: %s", json_path);
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(tscfg_path, json_path, base_len);
    strcpy(tscfg_path + base_len, ".tscfg");
    
    // 检查 .tscfg 是否存在
    struct stat st;
    if (stat(tscfg_path, &st) == 0) {
        // .tscfg 存在，使用 ts_config_pack_load 解密
        ESP_LOGI(TAG, "Loading encrypted config: %s", tscfg_path);
        
        ts_config_pack_t *pack = NULL;
        ts_config_pack_result_t result = ts_config_pack_load(tscfg_path, &pack);
        
        if (result == TS_CONFIG_PACK_OK && pack != NULL) {
            // 成功解密，移交内容所有权
            *content = pack->content;
            if (content_len) *content_len = pack->content_len;
            if (used_tscfg) *used_tscfg = true;
            
            pack->content = NULL;  // 防止 free 释放
            ts_config_pack_free(pack);
            
            ESP_LOGI(TAG, "Successfully loaded encrypted config: %s", tscfg_path);
            return ESP_OK;
        }
        
        // 解密失败，回退到 .json
        if (pack) ts_config_pack_free(pack);
        ESP_LOGW(TAG, "Failed to decrypt %s (err=%d), falling back to .json", 
                 tscfg_path, result);
    }
    
    // 加载普通 .json 文件
    if (stat(json_path, &st) != 0) {
        ESP_LOGD(TAG, "Config file not found: %s", json_path);
        return ESP_ERR_NOT_FOUND;
    }
    
    FILE *f = fopen(json_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open: %s", json_path);
        return ESP_FAIL;
    }
    
    size_t file_size = st.st_size;
    char *json_content = heap_caps_malloc(file_size + 1, MALLOC_CAP_SPIRAM);
    if (!json_content) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate memory for %s", json_path);
        return ESP_ERR_NO_MEM;
    }
    
    size_t read_size = fread(json_content, 1, file_size, f);
    fclose(f);
    
    if (read_size != file_size) {
        free(json_content);
        ESP_LOGE(TAG, "Failed to read: %s", json_path);
        return ESP_FAIL;
    }
    
    json_content[file_size] = '\0';
    *content = json_content;
    if (content_len) *content_len = file_size;
    if (used_tscfg) *used_tscfg = false;
    
    ESP_LOGI(TAG, "Loaded plain config: %s (%zu bytes)", json_path, file_size);
    return ESP_OK;
}
