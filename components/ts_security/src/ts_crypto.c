/**
 * @file ts_crypto.c
 * @brief Cryptographic Utilities Implementation
 * 
 * 密钥和临时缓冲区优先分配到 PSRAM
 */

#include "ts_crypto.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM, TS_CALLOC_PSRAM */
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/md.h"
#include "mbedtls/gcm.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pem.h"
#include "mbedtls/x509_crt.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "ts_crypto"

esp_err_t ts_crypto_hash(ts_hash_algo_t algo, const void *data, size_t len,
                          void *hash, size_t hash_len)
{
    if (!data || !hash) return ESP_ERR_INVALID_ARG;
    
    switch (algo) {
        case TS_HASH_SHA256:
            if (hash_len < 32) return ESP_ERR_INVALID_SIZE;
            return mbedtls_sha256(data, len, hash, 0) == 0 ? ESP_OK : ESP_FAIL;
        case TS_HASH_SHA384:
            if (hash_len < 48) return ESP_ERR_INVALID_SIZE;
            return mbedtls_sha512(data, len, hash, 1) == 0 ? ESP_OK : ESP_FAIL;
        case TS_HASH_SHA512:
            if (hash_len < 64) return ESP_ERR_INVALID_SIZE;
            return mbedtls_sha512(data, len, hash, 0) == 0 ? ESP_OK : ESP_FAIL;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t ts_crypto_hmac(ts_hash_algo_t algo, const void *key, size_t key_len,
                          const void *data, size_t data_len,
                          void *mac, size_t mac_len)
{
    if (!key || !data || !mac) return ESP_ERR_INVALID_ARG;
    
    mbedtls_md_type_t md_type;
    size_t required_len;
    
    switch (algo) {
        case TS_HASH_SHA256:
            md_type = MBEDTLS_MD_SHA256;
            required_len = 32;
            break;
        case TS_HASH_SHA384:
            md_type = MBEDTLS_MD_SHA384;
            required_len = 48;
            break;
        case TS_HASH_SHA512:
            md_type = MBEDTLS_MD_SHA512;
            required_len = 64;
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
    
    if (mac_len < required_len) return ESP_ERR_INVALID_SIZE;
    
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(md_type);
    int ret = mbedtls_md_hmac(md_info, key, key_len, data, data_len, mac);
    
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ts_crypto_aes_gcm_encrypt(const void *key, size_t key_len,
                                     const void *iv, size_t iv_len,
                                     const void *aad, size_t aad_len,
                                     const void *plaintext, size_t plaintext_len,
                                     void *ciphertext, void *tag)
{
    if (!key || !iv || !plaintext || !ciphertext || !tag) return ESP_ERR_INVALID_ARG;
    
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    
    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, key_len * 8);
    if (ret != 0) {
        mbedtls_gcm_free(&ctx);
        return ESP_FAIL;
    }
    
    ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                     plaintext_len, iv, iv_len,
                                     aad, aad_len,
                                     plaintext, ciphertext,
                                     16, tag);
    
    mbedtls_gcm_free(&ctx);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ts_crypto_aes_gcm_decrypt(const void *key, size_t key_len,
                                     const void *iv, size_t iv_len,
                                     const void *aad, size_t aad_len,
                                     const void *ciphertext, size_t ciphertext_len,
                                     const void *tag, void *plaintext)
{
    if (!key || !iv || !ciphertext || !tag || !plaintext) return ESP_ERR_INVALID_ARG;
    
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    
    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, key_len * 8);
    if (ret != 0) {
        mbedtls_gcm_free(&ctx);
        return ESP_FAIL;
    }
    
    ret = mbedtls_gcm_auth_decrypt(&ctx, ciphertext_len,
                                    iv, iv_len,
                                    aad, aad_len,
                                    tag, 16,
                                    ciphertext, plaintext);
    
    mbedtls_gcm_free(&ctx);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ts_crypto_base64_encode(const void *src, size_t src_len,
                                   char *dst, size_t *dst_len)
{
    if (!src || !dst || !dst_len) return ESP_ERR_INVALID_ARG;
    
    size_t olen;
    int ret = mbedtls_base64_encode((unsigned char *)dst, *dst_len, &olen, src, src_len);
    
    if (ret == 0) {
        *dst_len = olen;
        return ESP_OK;
    } else if (ret == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_FAIL;
}

esp_err_t ts_crypto_base64_decode(const char *src, size_t src_len,
                                   void *dst, size_t *dst_len)
{
    if (!src || !dst || !dst_len) return ESP_ERR_INVALID_ARG;
    
    size_t olen;
    int ret = mbedtls_base64_decode(dst, *dst_len, &olen, (const unsigned char *)src, src_len);
    
    if (ret == 0) {
        *dst_len = olen;
        return ESP_OK;
    } else if (ret == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_FAIL;
}

esp_err_t ts_crypto_hex_encode(const void *src, size_t src_len,
                                char *dst, size_t dst_len)
{
    if (!src || !dst) return ESP_ERR_INVALID_ARG;
    if (dst_len < src_len * 2 + 1) return ESP_ERR_INVALID_SIZE;
    
    const uint8_t *s = src;
    for (size_t i = 0; i < src_len; i++) {
        sprintf(dst + i * 2, "%02x", s[i]);
    }
    dst[src_len * 2] = '\0';
    return ESP_OK;
}

esp_err_t ts_crypto_hex_decode(const char *src, size_t src_len,
                                void *dst, size_t *dst_len)
{
    if (!src || !dst || !dst_len) return ESP_ERR_INVALID_ARG;
    if (src_len % 2 != 0) return ESP_ERR_INVALID_ARG;
    if (*dst_len < src_len / 2) return ESP_ERR_INVALID_SIZE;
    
    uint8_t *d = dst;
    for (size_t i = 0; i < src_len / 2; i++) {
        unsigned int val;
        if (sscanf(src + i * 2, "%2x", &val) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        d[i] = (uint8_t)val;
    }
    *dst_len = src_len / 2;
    return ESP_OK;
}

/*===========================================================================*/
/*                          RSA/EC Key Pair Implementation                    */
/*===========================================================================*/

struct ts_keypair_s {
    mbedtls_pk_context pk;
    ts_crypto_key_type_t type;
};

/* Random number generator context - 非 static 以便 ts_cert.c 共享 */
mbedtls_entropy_context s_entropy;
mbedtls_ctr_drbg_context s_ctr_drbg;
bool s_rng_initialized = false;

static esp_err_t init_rng(void)
{
    if (s_rng_initialized) return ESP_OK;
    
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctr_drbg);
    
    const char *pers = "ts_crypto";
    int ret = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func, &s_entropy,
                                     (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        return ESP_FAIL;
    }
    
    s_rng_initialized = true;
    return ESP_OK;
}

esp_err_t ts_crypto_keypair_generate(ts_crypto_key_type_t type, ts_keypair_t *keypair)
{
    if (!keypair) return ESP_ERR_INVALID_ARG;
    
    esp_err_t err = init_rng();
    if (err != ESP_OK) return err;
    
    struct ts_keypair_s *kp = TS_CALLOC_PSRAM(1, sizeof(struct ts_keypair_s));
    if (!kp) return ESP_ERR_NO_MEM;
    
    mbedtls_pk_init(&kp->pk);
    kp->type = type;
    
    int ret;
    
    switch (type) {
        case TS_CRYPTO_KEY_RSA_2048:
        case TS_CRYPTO_KEY_RSA_4096: {
            ret = mbedtls_pk_setup(&kp->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
            if (ret != 0) goto error;
            
            unsigned int nbits = (type == TS_CRYPTO_KEY_RSA_2048) ? 2048 : 4096;
            ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(kp->pk), 
                                       mbedtls_ctr_drbg_random, &s_ctr_drbg,
                                       nbits, 65537);
            if (ret != 0) goto error;
            break;
        }
        
        case TS_CRYPTO_KEY_EC_P256:
        case TS_CRYPTO_KEY_EC_P384: {
            ret = mbedtls_pk_setup(&kp->pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
            if (ret != 0) goto error;
            
            mbedtls_ecp_group_id grp_id = (type == TS_CRYPTO_KEY_EC_P256) ? 
                                           MBEDTLS_ECP_DP_SECP256R1 : 
                                           MBEDTLS_ECP_DP_SECP384R1;
            ret = mbedtls_ecp_gen_key(grp_id, mbedtls_pk_ec(kp->pk),
                                       mbedtls_ctr_drbg_random, &s_ctr_drbg);
            if (ret != 0) goto error;
            break;
        }
        
        default:
            free(kp);
            return ESP_ERR_NOT_SUPPORTED;
    }
    
    *keypair = kp;
    return ESP_OK;
    
error:
    mbedtls_pk_free(&kp->pk);
    free(kp);
    return ESP_FAIL;
}

void ts_crypto_keypair_free(ts_keypair_t keypair)
{
    if (!keypair) return;
    
    mbedtls_pk_free(&keypair->pk);
    free(keypair);
}

esp_err_t ts_crypto_keypair_export_public(ts_keypair_t keypair, char *pem, size_t *pem_len)
{
    if (!keypair || !pem || !pem_len) return ESP_ERR_INVALID_ARG;
    
    int ret = mbedtls_pk_write_pubkey_pem(&keypair->pk, (unsigned char *)pem, *pem_len);
    if (ret != 0) {
        return ESP_FAIL;
    }
    
    *pem_len = strlen(pem) + 1;
    return ESP_OK;
}

/**
 * @brief Write SSH-style uint32 in big-endian to buffer
 */
static size_t write_ssh_uint32(uint8_t *buf, uint32_t val)
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8)  & 0xFF;
    buf[3] = val & 0xFF;
    return 4;
}

/**
 * @brief Write SSH-style string (length + data) to buffer
 */
static size_t write_ssh_string(uint8_t *buf, const uint8_t *data, size_t len)
{
    size_t pos = write_ssh_uint32(buf, (uint32_t)len);
    memcpy(buf + pos, data, len);
    return pos + len;
}

/**
 * @brief Write SSH-style mpint (big integer with leading zero if MSB is set)
 */
static size_t write_ssh_mpint(uint8_t *buf, const uint8_t *data, size_t len)
{
    /* Skip leading zeros except when all zeros */
    while (len > 1 && data[0] == 0) {
        data++;
        len--;
    }
    
    /* Add leading zero if MSB is set (negative in two's complement) */
    if (data[0] & 0x80) {
        size_t pos = write_ssh_uint32(buf, (uint32_t)(len + 1));
        buf[pos] = 0;
        memcpy(buf + pos + 1, data, len);
        return pos + 1 + len;
    } else {
        return write_ssh_string(buf, data, len);
    }
}

esp_err_t ts_crypto_keypair_export_openssh(ts_keypair_t keypair, char *openssh, size_t *openssh_len, const char *comment)
{
    if (!keypair || !openssh || !openssh_len) return ESP_ERR_INVALID_ARG;
    
    mbedtls_pk_type_t pk_type = mbedtls_pk_get_type(&keypair->pk);
    const char *key_type_str = NULL;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    esp_err_t result = ESP_FAIL;
    
    /* Allocate temporary buffer for binary blob */
    blob = TS_MALLOC_PSRAM(4096);
    if (!blob) return ESP_ERR_NO_MEM;
    
    size_t pos = 0;
    
    if (pk_type == MBEDTLS_PK_RSA) {
        /* RSA public key format:
         * string    "ssh-rsa"
         * mpint     e (public exponent)
         * mpint     n (modulus)
         */
        key_type_str = "ssh-rsa";
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(keypair->pk);
        
        /* Write key type */
        pos += write_ssh_string(blob + pos, (const uint8_t *)"ssh-rsa", 7);
        
        /* Get RSA parameters */
        size_t n_len = mbedtls_rsa_get_len(rsa);
        uint8_t *n_buf = TS_MALLOC_PSRAM(n_len);
        uint8_t *e_buf = TS_MALLOC_PSRAM(n_len);  /* e is smaller but reuse buffer size */
        
        if (!n_buf || !e_buf) {
            free(n_buf);
            free(e_buf);
            free(blob);
            return ESP_ERR_NO_MEM;
        }
        
        /* Export N and E */
        mbedtls_mpi N, E;
        mbedtls_mpi_init(&N);
        mbedtls_mpi_init(&E);
        mbedtls_rsa_export(rsa, &N, NULL, NULL, NULL, &E);
        
        size_t e_len = mbedtls_mpi_size(&E);
        size_t actual_n_len = mbedtls_mpi_size(&N);
        mbedtls_mpi_write_binary(&E, e_buf, e_len);
        mbedtls_mpi_write_binary(&N, n_buf, actual_n_len);
        
        mbedtls_mpi_free(&N);
        mbedtls_mpi_free(&E);
        
        /* Write E then N (OpenSSH order) */
        pos += write_ssh_mpint(blob + pos, e_buf, e_len);
        pos += write_ssh_mpint(blob + pos, n_buf, actual_n_len);
        
        ESP_LOGI("ts_crypto", "OpenSSH RSA: e_len=%zu, n_len=%zu, blob_len=%zu", e_len, actual_n_len, pos);
        ESP_LOGI("ts_crypto", "OpenSSH blob[0..19] = %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                  blob[0], blob[1], blob[2], blob[3], blob[4], blob[5], blob[6], blob[7], blob[8], blob[9],
                  blob[10], blob[11], blob[12], blob[13], blob[14], blob[15], blob[16], blob[17], blob[18], blob[19]);
        
        free(n_buf);
        free(e_buf);
        
    } else if (pk_type == MBEDTLS_PK_ECKEY || pk_type == MBEDTLS_PK_ECDSA) {
        /* ECDSA public key format:
         * string    identifier ("ecdsa-sha2-nistp256" or "ecdsa-sha2-nistp384")
         * string    curve name ("nistp256" or "nistp384")
         * string    Q (public point in uncompressed format: 04 || x || y)
         */
        mbedtls_ecp_keypair *ec = mbedtls_pk_ec(keypair->pk);
        mbedtls_ecp_group_id grp_id = ec->private_grp.id;
        
        const char *identifier, *curve_name;
        size_t coord_len;
        
        if (grp_id == MBEDTLS_ECP_DP_SECP256R1) {
            identifier = "ecdsa-sha2-nistp256";
            curve_name = "nistp256";
            coord_len = 32;
        } else if (grp_id == MBEDTLS_ECP_DP_SECP384R1) {
            identifier = "ecdsa-sha2-nistp384";
            curve_name = "nistp384";
            coord_len = 48;
        } else {
            free(blob);
            return ESP_ERR_NOT_SUPPORTED;
        }
        
        key_type_str = identifier;
        
        /* Write identifier */
        pos += write_ssh_string(blob + pos, (const uint8_t *)identifier, strlen(identifier));
        
        /* Write curve name */
        pos += write_ssh_string(blob + pos, (const uint8_t *)curve_name, strlen(curve_name));
        
        /* Write public point Q (uncompressed: 0x04 || X || Y) */
        size_t q_len = 1 + 2 * coord_len;
        uint8_t *q_buf = TS_MALLOC_PSRAM(q_len);
        if (!q_buf) {
            free(blob);
            return ESP_ERR_NO_MEM;
        }
        
        size_t olen;
        int ret = mbedtls_ecp_point_write_binary(&ec->private_grp, &ec->private_Q,
                                                  MBEDTLS_ECP_PF_UNCOMPRESSED,
                                                  &olen, q_buf, q_len);
        if (ret != 0) {
            free(q_buf);
            free(blob);
            return ESP_FAIL;
        }
        
        pos += write_ssh_string(blob + pos, q_buf, olen);
        free(q_buf);
        
    } else {
        free(blob);
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    blob_len = pos;
    
    /* Base64 encode the blob */
    size_t b64_len = ((blob_len + 2) / 3) * 4 + 1;
    char *b64 = TS_MALLOC_PSRAM(b64_len);
    if (!b64) {
        free(blob);
        return ESP_ERR_NO_MEM;
    }
    
    size_t actual_b64_len;
    int ret = mbedtls_base64_encode((unsigned char *)b64, b64_len, &actual_b64_len, blob, blob_len);
    free(blob);
    
    if (ret != 0) {
        free(b64);
        return ESP_FAIL;
    }
    
    /* Build final OpenSSH format: <type> <base64> [comment] */
    size_t type_len = strlen(key_type_str);
    size_t comment_len = comment ? strlen(comment) : 0;
    size_t total_len = type_len + 1 + actual_b64_len + (comment_len > 0 ? 1 + comment_len : 0) + 2;
    
    if (*openssh_len < total_len) {
        free(b64);
        *openssh_len = total_len;
        return ESP_ERR_INVALID_SIZE;
    }
    
    pos = 0;
    memcpy(openssh + pos, key_type_str, type_len);
    pos += type_len;
    openssh[pos++] = ' ';
    memcpy(openssh + pos, b64, actual_b64_len);
    pos += actual_b64_len;
    
    if (comment_len > 0) {
        openssh[pos++] = ' ';
        memcpy(openssh + pos, comment, comment_len);
        pos += comment_len;
    }
    
    openssh[pos++] = '\n';
    openssh[pos] = '\0';
    *openssh_len = pos + 1;
    
    free(b64);
    result = ESP_OK;
    
    return result;
}

esp_err_t ts_crypto_keypair_export_private(ts_keypair_t keypair, char *pem, size_t *pem_len)
{
    if (!keypair || !pem || !pem_len) return ESP_ERR_INVALID_ARG;
    
    int ret = mbedtls_pk_write_key_pem(&keypair->pk, (unsigned char *)pem, *pem_len);
    if (ret != 0) {
        return ESP_FAIL;
    }
    
    *pem_len = strlen(pem) + 1;
    return ESP_OK;
}

esp_err_t ts_crypto_keypair_import(const char *pem, size_t pem_len, ts_keypair_t *keypair)
{
    if (!pem || !keypair) return ESP_ERR_INVALID_ARG;
    
    esp_err_t err = init_rng();
    if (err != ESP_OK) return err;
    
    struct ts_keypair_s *kp = TS_CALLOC_PSRAM(1, sizeof(struct ts_keypair_s));
    if (!kp) return ESP_ERR_NO_MEM;
    
    mbedtls_pk_init(&kp->pk);
    
    /* Try parsing as private key first */
    int ret = mbedtls_pk_parse_key(&kp->pk, (const unsigned char *)pem, pem_len,
                                    NULL, 0, mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0) {
        /* Try as public key */
        ret = mbedtls_pk_parse_public_key(&kp->pk, (const unsigned char *)pem, pem_len);
        if (ret != 0) {
            /* Try as X.509 certificate - extract public key from it */
            mbedtls_x509_crt crt;
            mbedtls_x509_crt_init(&crt);
            ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, pem_len);
            if (ret == 0) {
                /* Copy public key context from certificate */
                /* We need to serialize and re-parse to get an independent copy */
                unsigned char pubkey_buf[512];
                ret = mbedtls_pk_write_pubkey_der(&crt.pk, pubkey_buf, sizeof(pubkey_buf));
                mbedtls_x509_crt_free(&crt);
                
                if (ret > 0) {
                    /* DER data is written at the end of buffer */
                    int der_len = ret;
                    unsigned char *der_start = pubkey_buf + sizeof(pubkey_buf) - der_len;
                    ret = mbedtls_pk_parse_public_key(&kp->pk, der_start, der_len);
                }
            } else {
                mbedtls_x509_crt_free(&crt);
            }
            
            if (ret != 0) {
                mbedtls_pk_free(&kp->pk);
                free(kp);
                ESP_LOGD(TAG, "Failed to parse PEM: not a valid key or certificate");
                return ESP_FAIL;
            }
        }
    }
    
    /* Determine key type */
    mbedtls_pk_type_t pk_type = mbedtls_pk_get_type(&kp->pk);
    if (pk_type == MBEDTLS_PK_RSA) {
        size_t bits = mbedtls_pk_get_bitlen(&kp->pk);
        kp->type = (bits <= 2048) ? TS_CRYPTO_KEY_RSA_2048 : TS_CRYPTO_KEY_RSA_4096;
    } else if (pk_type == MBEDTLS_PK_ECKEY || pk_type == MBEDTLS_PK_ECDSA) {
        /* Use bit length to determine curve type since mbedtls 3.x changed API */
        size_t bits = mbedtls_pk_get_bitlen(&kp->pk);
        kp->type = (bits <= 256) ? TS_CRYPTO_KEY_EC_P256 : TS_CRYPTO_KEY_EC_P384;
    }
    
    *keypair = kp;
    return ESP_OK;
}

esp_err_t ts_crypto_rsa_sign(ts_keypair_t keypair, ts_hash_algo_t hash_algo,
                              const void *hash, size_t hash_len,
                              void *signature, size_t *sig_len)
{
    if (!keypair || !hash || !signature || !sig_len) return ESP_ERR_INVALID_ARG;
    
    if (mbedtls_pk_get_type(&keypair->pk) != MBEDTLS_PK_RSA) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t err = init_rng();
    if (err != ESP_OK) return err;
    
    mbedtls_md_type_t md_type;
    switch (hash_algo) {
        case TS_HASH_SHA256: md_type = MBEDTLS_MD_SHA256; break;
        case TS_HASH_SHA384: md_type = MBEDTLS_MD_SHA384; break;
        case TS_HASH_SHA512: md_type = MBEDTLS_MD_SHA512; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    
    int ret = mbedtls_pk_sign(&keypair->pk, md_type, hash, hash_len,
                               signature, *sig_len, sig_len,
                               mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_crypto_rsa_verify(ts_keypair_t keypair, ts_hash_algo_t hash_algo,
                                const void *hash, size_t hash_len,
                                const void *signature, size_t sig_len)
{
    if (!keypair || !hash || !signature) return ESP_ERR_INVALID_ARG;
    
    if (mbedtls_pk_get_type(&keypair->pk) != MBEDTLS_PK_RSA) {
        return ESP_ERR_INVALID_ARG;
    }
    
    mbedtls_md_type_t md_type;
    switch (hash_algo) {
        case TS_HASH_SHA256: md_type = MBEDTLS_MD_SHA256; break;
        case TS_HASH_SHA384: md_type = MBEDTLS_MD_SHA384; break;
        case TS_HASH_SHA512: md_type = MBEDTLS_MD_SHA512; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    
    int ret = mbedtls_pk_verify(&keypair->pk, md_type, hash, hash_len,
                                 signature, sig_len);
    if (ret != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_crypto_ecdsa_sign(ts_keypair_t keypair, 
                                const void *hash, size_t hash_len,
                                void *signature, size_t *sig_len)
{
    if (!keypair || !hash || !signature || !sig_len) return ESP_ERR_INVALID_ARG;
    
    mbedtls_pk_type_t pk_type = mbedtls_pk_get_type(&keypair->pk);
    if (pk_type != MBEDTLS_PK_ECKEY && pk_type != MBEDTLS_PK_ECDSA) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t err = init_rng();
    if (err != ESP_OK) return err;
    
    /* Use SHA256 for signing */
    int ret = mbedtls_pk_sign(&keypair->pk, MBEDTLS_MD_SHA256, hash, hash_len,
                               signature, *sig_len, sig_len,
                               mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t ts_crypto_ecdsa_verify(ts_keypair_t keypair,
                                  const void *hash, size_t hash_len,
                                  const void *signature, size_t sig_len)
{
    if (!keypair || !hash || !signature) return ESP_ERR_INVALID_ARG;
    
    mbedtls_pk_type_t pk_type = mbedtls_pk_get_type(&keypair->pk);
    if (pk_type != MBEDTLS_PK_ECKEY && pk_type != MBEDTLS_PK_ECDSA) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int ret = mbedtls_pk_verify(&keypair->pk, MBEDTLS_MD_SHA256, hash, hash_len,
                                 signature, sig_len);
    if (ret != 0) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          Random Number Generation                          */
/*===========================================================================*/

esp_err_t ts_crypto_random(void *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    
    esp_err_t err = init_rng();
    if (err != ESP_OK) return err;
    
    int ret = mbedtls_ctr_drbg_random(&s_ctr_drbg, buf, len);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

/*===========================================================================*/
/*                          HKDF Key Derivation                               */
/*===========================================================================*/

#include "mbedtls/hkdf.h"

esp_err_t ts_crypto_hkdf(const void *salt, size_t salt_len,
                          const void *ikm, size_t ikm_len,
                          const void *info, size_t info_len,
                          void *okm, size_t okm_len)
{
    if (!ikm || !okm || okm_len == 0) return ESP_ERR_INVALID_ARG;
    
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return ESP_FAIL;
    
    int ret = mbedtls_hkdf(md_info,
                           salt, salt_len,
                           ikm, ikm_len,
                           info, info_len,
                           okm, okm_len);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "HKDF failed: -0x%04x", (unsigned int)-ret);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/*===========================================================================*/
/*                          ECDH Key Exchange                                 */
/*===========================================================================*/

#include "mbedtls/ecdh.h"

esp_err_t ts_crypto_ecdh_compute_shared(ts_keypair_t local_keypair,
                                         const char *peer_pubkey_pem,
                                         void *shared_secret,
                                         size_t *shared_len)
{
    if (!local_keypair || !peer_pubkey_pem || !shared_secret || !shared_len) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Verify local key is EC type */
    mbedtls_pk_type_t pk_type = mbedtls_pk_get_type(&local_keypair->pk);
    if (pk_type != MBEDTLS_PK_ECKEY && pk_type != MBEDTLS_PK_ECDSA) {
        ESP_LOGE(TAG, "ECDH requires EC key, got type %d", pk_type);
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Parse peer public key (or extract from X.509 certificate) */
    mbedtls_pk_context peer_pk;
    mbedtls_pk_init(&peer_pk);
    
    size_t pem_len = strlen(peer_pubkey_pem) + 1;
    int ret = mbedtls_pk_parse_public_key(&peer_pk,
                                           (const unsigned char *)peer_pubkey_pem,
                                           pem_len);
    if (ret != 0) {
        /* Try parsing as X.509 certificate */
        mbedtls_x509_crt cert;
        mbedtls_x509_crt_init(&cert);
        ret = mbedtls_x509_crt_parse(&cert, (const unsigned char *)peer_pubkey_pem, pem_len);
        if (ret == 0) {
            /* Extract public key from certificate via DER serialization */
            unsigned char der_buf[256];
            ret = mbedtls_pk_write_pubkey_der(&cert.pk, der_buf, sizeof(der_buf));
            if (ret > 0) {
                /* DER data is written at end of buffer */
                unsigned char *der_start = der_buf + sizeof(der_buf) - ret;
                int parse_ret = mbedtls_pk_parse_public_key(&peer_pk, der_start, ret);
                if (parse_ret != 0) {
                    ESP_LOGE(TAG, "Failed to re-parse extracted pubkey: -0x%04x", (unsigned int)-parse_ret);
                    ret = parse_ret;
                } else {
                    ret = 0;  /* Success */
                }
            } else {
                ESP_LOGE(TAG, "Failed to export cert pubkey to DER");
            }
        }
        mbedtls_x509_crt_free(&cert);
        
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to parse peer public key or certificate: -0x%04x", (unsigned int)-ret);
            mbedtls_pk_free(&peer_pk);
            return ESP_ERR_INVALID_ARG;
        }
    }
    
    /* Verify peer key is EC type */
    pk_type = mbedtls_pk_get_type(&peer_pk);
    if (pk_type != MBEDTLS_PK_ECKEY && pk_type != MBEDTLS_PK_ECDSA) {
        ESP_LOGE(TAG, "Peer key is not EC type");
        mbedtls_pk_free(&peer_pk);
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Get EC contexts */
    mbedtls_ecp_keypair *local_ec = mbedtls_pk_ec(local_keypair->pk);
    mbedtls_ecp_keypair *peer_ec = mbedtls_pk_ec(peer_pk);
    
    /* Verify same curve */
    if (local_ec->private_grp.id != peer_ec->private_grp.id) {
        ESP_LOGE(TAG, "EC curve mismatch");
        mbedtls_pk_free(&peer_pk);
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Initialize RNG */
    esp_err_t err = init_rng();
    if (err != ESP_OK) {
        mbedtls_pk_free(&peer_pk);
        return err;
    }
    
    /* Compute shared secret: shared = d * Q (local private * peer public) */
    mbedtls_mpi shared_mpi;
    mbedtls_mpi_init(&shared_mpi);
    
    ret = mbedtls_ecdh_compute_shared(&local_ec->private_grp,
                                       &shared_mpi,
                                       &peer_ec->private_Q,  /* Peer public point */
                                       &local_ec->private_d, /* Local private key */
                                       mbedtls_ctr_drbg_random, &s_ctr_drbg);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "ECDH compute shared failed: -0x%04x", (unsigned int)-ret);
        mbedtls_mpi_free(&shared_mpi);
        mbedtls_pk_free(&peer_pk);
        return ESP_FAIL;
    }
    
    /* Export shared secret as big-endian bytes */
    size_t secret_len = mbedtls_mpi_size(&shared_mpi);
    if (*shared_len < secret_len) {
        ESP_LOGE(TAG, "Shared secret buffer too small: need %d, got %d", 
                 (int)secret_len, (int)*shared_len);
        mbedtls_mpi_free(&shared_mpi);
        mbedtls_pk_free(&peer_pk);
        return ESP_ERR_INVALID_SIZE;
    }
    
    ret = mbedtls_mpi_write_binary(&shared_mpi, shared_secret, secret_len);
    
    mbedtls_mpi_free(&shared_mpi);
    mbedtls_pk_free(&peer_pk);
    
    if (ret != 0) {
        return ESP_FAIL;
    }
    
    *shared_len = secret_len;
    return ESP_OK;
}

esp_err_t ts_crypto_ecdh_compute_shared_raw(ts_keypair_t local_keypair,
                                             const void *peer_pubkey,
                                             size_t peer_pubkey_len,
                                             void *shared_secret,
                                             size_t *shared_len)
{
    if (!local_keypair || !peer_pubkey || !shared_secret || !shared_len) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Verify local key is EC type */
    mbedtls_pk_type_t pk_type = mbedtls_pk_get_type(&local_keypair->pk);
    if (pk_type != MBEDTLS_PK_ECKEY && pk_type != MBEDTLS_PK_ECDSA) {
        ESP_LOGE(TAG, "ECDH requires EC key");
        return ESP_ERR_INVALID_ARG;
    }
    
    mbedtls_ecp_keypair *local_ec = mbedtls_pk_ec(local_keypair->pk);
    
    /* Parse raw public key point */
    mbedtls_ecp_point peer_Q;
    mbedtls_ecp_point_init(&peer_Q);
    
    int ret = mbedtls_ecp_point_read_binary(&local_ec->private_grp, &peer_Q,
                                             peer_pubkey, peer_pubkey_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse raw public key: -0x%04x", (unsigned int)-ret);
        mbedtls_ecp_point_free(&peer_Q);
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Validate peer public key point */
    ret = mbedtls_ecp_check_pubkey(&local_ec->private_grp, &peer_Q);
    if (ret != 0) {
        ESP_LOGE(TAG, "Invalid peer public key: -0x%04x", (unsigned int)-ret);
        mbedtls_ecp_point_free(&peer_Q);
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Initialize RNG */
    esp_err_t err = init_rng();
    if (err != ESP_OK) {
        mbedtls_ecp_point_free(&peer_Q);
        return err;
    }
    
    /* Compute shared secret */
    mbedtls_mpi shared_mpi;
    mbedtls_mpi_init(&shared_mpi);
    
    ret = mbedtls_ecdh_compute_shared(&local_ec->private_grp,
                                       &shared_mpi,
                                       &peer_Q,
                                       &local_ec->private_d,
                                       mbedtls_ctr_drbg_random, &s_ctr_drbg);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "ECDH compute shared failed: -0x%04x", (unsigned int)-ret);
        mbedtls_mpi_free(&shared_mpi);
        mbedtls_ecp_point_free(&peer_Q);
        return ESP_FAIL;
    }
    
    /* Export shared secret */
    size_t secret_len = mbedtls_mpi_size(&shared_mpi);
    if (*shared_len < secret_len) {
        mbedtls_mpi_free(&shared_mpi);
        mbedtls_ecp_point_free(&peer_Q);
        return ESP_ERR_INVALID_SIZE;
    }
    
    ret = mbedtls_mpi_write_binary(&shared_mpi, shared_secret, secret_len);
    
    mbedtls_mpi_free(&shared_mpi);
    mbedtls_ecp_point_free(&peer_Q);
    
    if (ret != 0) {
        return ESP_FAIL;
    }
    
    *shared_len = secret_len;
    return ESP_OK;
}

esp_err_t ts_crypto_keypair_export_public_raw(ts_keypair_t keypair,
                                               void *raw,
                                               size_t *raw_len)
{
    if (!keypair || !raw || !raw_len) return ESP_ERR_INVALID_ARG;
    
    mbedtls_pk_type_t pk_type = mbedtls_pk_get_type(&keypair->pk);
    if (pk_type != MBEDTLS_PK_ECKEY && pk_type != MBEDTLS_PK_ECDSA) {
        ESP_LOGE(TAG, "Raw export only supports EC keys");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    mbedtls_ecp_keypair *ec = mbedtls_pk_ec(keypair->pk);
    
    /* Calculate required buffer size */
    /* Uncompressed point format: 0x04 || X || Y */
    size_t point_len = 1 + 2 * ((ec->private_grp.pbits + 7) / 8);
    
    if (*raw_len < point_len) {
        *raw_len = point_len;
        return ESP_ERR_INVALID_SIZE;
    }
    
    size_t olen;
    int ret = mbedtls_ecp_point_write_binary(&ec->private_grp, &ec->private_Q,
                                              MBEDTLS_ECP_PF_UNCOMPRESSED,
                                              &olen, raw, *raw_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to export public key: -0x%04x", (unsigned int)-ret);
        return ESP_FAIL;
    }
    
    *raw_len = olen;
    return ESP_OK;
}

