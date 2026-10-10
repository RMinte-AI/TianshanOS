/**
 * @file ts_config_pack.h
 * @brief TianShanOS Encrypted Configuration Package System
 * 
 * 提供配置包的加密、解密、签名和验证功能。
 * 
 * 主要功能:
 * - 使用 ECDH + AES-256-GCM 混合加密
 * - 使用 ECDSA-SHA256 数字签名
 * - 支持官方签名验证（OU=Developer）
 * - 支持配置包的导入和导出
 */

#ifndef TS_CONFIG_PACK_H
#define TS_CONFIG_PACK_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

#define TS_CONFIG_PACK_VERSION          "1.0"
#define TS_CONFIG_PACK_ALGORITHM        "ECDH-P256+AES-256-GCM"
#define TS_CONFIG_PACK_KDF              "HKDF-SHA256"
#define TS_CONFIG_PACK_SIG_ALGORITHM    "ECDSA-SHA256"

/** HKDF info string for key derivation */
#define TS_CONFIG_PACK_HKDF_INFO        "tscfg-aes-key-v1"

/** File extension for encrypted config packs */
#define TS_CONFIG_PACK_EXT              ".tscfg"

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief Configuration pack result codes
 */
typedef enum {
    TS_CONFIG_PACK_OK = 0,              /**< Success */
    TS_CONFIG_PACK_ERR_PARSE,           /**< JSON parse error */
    TS_CONFIG_PACK_ERR_VERSION,         /**< Unsupported version */
    TS_CONFIG_PACK_ERR_RECIPIENT,       /**< Not intended for this device */
    TS_CONFIG_PACK_ERR_CERT_CHAIN,      /**< Certificate chain validation failed */
    TS_CONFIG_PACK_ERR_SIGNATURE,       /**< Signature verification failed */
    TS_CONFIG_PACK_ERR_DECRYPT,         /**< Decryption failed */
    TS_CONFIG_PACK_ERR_INTEGRITY,       /**< Content hash mismatch */
    TS_CONFIG_PACK_ERR_EXPIRED,         /**< Package expired */
    TS_CONFIG_PACK_ERR_NO_MEM,          /**< Memory allocation failed */
    TS_CONFIG_PACK_ERR_IO,              /**< File I/O error */
    TS_CONFIG_PACK_ERR_PERMISSION,      /**< Permission denied (not a developer device) */
    TS_CONFIG_PACK_ERR_INVALID_ARG,     /**< Invalid argument */
    TS_CONFIG_PACK_ERR_NOT_INIT,        /**< System not initialized */
    TS_CONFIG_PACK_ERR_TIME_UNVERIFIED,
    TS_CONFIG_PACK_ERR_TRUST_NOT_CONFIGURED,
    TS_CONFIG_PACK_ERR_SIGNER_ROLE,
    TS_CONFIG_PACK_ERR_CREDENTIAL_CHANGED,
} ts_config_pack_result_t;

/** Local acceptance of exact v1 package bytes; never constructed from package metadata. */
typedef struct {
    uint32_t policy;
    int64_t accepted_at;
    char package_sha256[65], signer_sha256[65], root_sha256[65], recipient[65];
} ts_config_pack_acceptance_t;

/** Bounded complete JSON, rejects duplicate keys and decoded NUL characters. */
cJSON *ts_config_pack_parse_json(const char *text, size_t length);
cJSON *ts_config_pack_parse_import_request(const char *text, size_t length);


/**
 * @brief Signature verification result
 */
typedef struct {
    bool valid;                          /**< Signature is valid */
    bool is_official;                    /**< Signed by official/developer device */
    char signer_cn[64];                  /**< Signer Common Name */
    char signer_ou[32];                  /**< Signer Organizational Unit */
    int64_t signed_at;                   /**< Signature timestamp (Unix epoch) */
} ts_config_pack_sig_info_t;

/**
 * @brief Loaded configuration pack
 */
typedef struct {
    char *name;                          /**< Config name (no extension) */
    char *description;                   /**< Description */
    char *content;                       /**< Decrypted JSON content */
    size_t content_len;                  /**< Content length */
    ts_config_pack_sig_info_t sig_info;  /**< Signature info */
    int64_t created_at;                  /**< Creation timestamp (Unix epoch) */
    char *source_file;                   /**< Original source filename */
    char *target_device;                 /**< Target device CN */
} ts_config_pack_t;

/** Explicit signing trust, one credential snapshot, optional local acceptance for offline reload. */
ts_config_pack_result_t ts_config_pack_load_verified_mem(
    const char *text, size_t length, const ts_config_pack_acceptance_t *accepted,
    ts_config_pack_t **pack, ts_config_pack_acceptance_t *acceptance, uint32_t *credential_generation);

/**
 * @brief Export options for creating a config pack
 */
typedef struct {
    const char *recipient_cert_pem;      /**< Target device certificate (PEM) */
    size_t recipient_cert_len;           /**< Certificate length */
    const char *description;             /**< Optional description */
} ts_config_pack_export_opts_t;

/*===========================================================================*/
/*                         Core Functions                                     */
/*===========================================================================*/

/**
 * @brief Initialize configuration pack system
 * 
 * Must be called after ts_cert_init().
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_config_pack_init(void);

/**
 * @brief Check if this device can export config packs
 * 
 * Only devices with OU=Developer in their certificate can export.
 * 
 * @return true if device can export, false otherwise
 */
bool ts_config_pack_can_export(void);

/**
 * @brief Load and decrypt a .tscfg file
 * 
 * Verifies signature and decrypts content using device private key.
 * 
 * @param path Path to .tscfg file
 * @param pack Output: loaded configuration (caller must free with ts_config_pack_free)
 * @return TS_CONFIG_PACK_OK on success, error code otherwise
 */
ts_config_pack_result_t ts_config_pack_load(const char *path, ts_config_pack_t **pack);

/**
 * @brief Load and decrypt from memory buffer
 * 
 * @param tscfg_json The .tscfg JSON content
 * @param tscfg_len Content length
 * @param pack Output: loaded configuration
 * @return TS_CONFIG_PACK_OK on success
 */
ts_config_pack_result_t ts_config_pack_load_mem(
    const char *tscfg_json,
    size_t tscfg_len,
    ts_config_pack_t **pack
);

/**
 * @brief Create an encrypted .tscfg package
 * 
 * Encrypts JSON content for a specific recipient and signs with device key.
 * Requires device to have OU=Developer in certificate.
 * 
 * @param name Config name (without extension)
 * @param json_content JSON content to encrypt
 * @param json_len Content length
 * @param opts Export options (recipient cert, description)
 * @param output Output buffer for .tscfg JSON (caller must free)
 * @param output_len Output length
 * @return TS_CONFIG_PACK_OK on success
 */
ts_config_pack_result_t ts_config_pack_create(
    const char *name,
    const char *json_content,
    size_t json_len,
    const ts_config_pack_export_opts_t *opts,
    char **output,
    size_t *output_len
);

/**
 * @brief Save config pack to file
 * 
 * @param path Output file path
 * @param tscfg_json Config pack JSON content
 * @param tscfg_len Content length
 * @return ESP_OK on success
 */
esp_err_t ts_config_pack_save(const char *path, const char *tscfg_json, size_t tscfg_len);

/**
 * @brief Free a loaded configuration pack
 * 
 * @param pack Pack to free (can be NULL)
 */
void ts_config_pack_free(ts_config_pack_t *pack);

/*===========================================================================*/
/*                      Verification Functions                                */
/*===========================================================================*/

/**
 * @brief Verify a .tscfg file without decrypting
 * 
 * Validates structure, certificate chain, and signature.
 * Does not require device private key.
 * 
 * @param path File path
 * @param info Output signature info (optional, can be NULL)
 * @return TS_CONFIG_PACK_OK if valid
 */
ts_config_pack_result_t ts_config_pack_verify(
    const char *path,
    ts_config_pack_sig_info_t *info
);

/**
 * @brief Verify from memory buffer
 * 
 * @param tscfg_json Config pack JSON
 * @param tscfg_len Content length
 * @param info Output signature info (optional)
 * @return TS_CONFIG_PACK_OK if valid
 */
ts_config_pack_result_t ts_config_pack_verify_mem(
    const char *tscfg_json,
    size_t tscfg_len,
    ts_config_pack_sig_info_t *info
);

/*===========================================================================*/
/*                      Import Functions                                      */
/*===========================================================================*/

/**
 * @brief Config pack metadata (without decryption)
 */
typedef struct {
    char name[64];                       /**< Config name */
    char description[128];               /**< Description */
    char source_file[64];                /**< Original filename */
    char target_device[64];              /**< Target device CN */
    int64_t created_at;                  /**< Creation timestamp */
    ts_config_pack_sig_info_t sig_info;  /**< Signature info */
} ts_config_pack_metadata_t;

/**
 * @brief Import config pack (verify and save encrypted file)
 * 
 * Validates signature, verifies recipient, and saves the encrypted
 * .tscfg file to the config directory without decryption.
 * The config is only decrypted on-demand when needed.
 * 
 * @param tscfg_json Config pack JSON content
 * @param tscfg_len Content length
 * @param metadata Output metadata (optional, can be NULL)
 * @param saved_path Output: path where file was saved (optional, can be NULL)
 * @param saved_path_len Buffer size for saved_path
 * @return TS_CONFIG_PACK_OK on success
 */
ts_config_pack_result_t ts_config_pack_import(
    const char *tscfg_json,
    size_t tscfg_len,
    ts_config_pack_metadata_t *metadata,
    char *saved_path,
    size_t saved_path_len
);

/**
 * @brief List imported config packs
 * 
 * @param names Array of config names (caller allocates)
 * @param max_count Maximum number of entries
 * @param count Output: actual number of configs found
 * @return ESP_OK on success
 */
esp_err_t ts_config_pack_list(
    char (*names)[64],
    size_t max_count,
    size_t *count
);

/**
 * @brief Get decrypted content of an imported config pack
 * 
 * Loads and decrypts a previously imported config pack by name.
 * 
 * @param name Config name (without .tscfg extension)
 * @param content Output: decrypted JSON content (caller must free)
 * @param content_len Output: content length
 * @return TS_CONFIG_PACK_OK on success
 */
ts_config_pack_result_t ts_config_pack_get_content(
    const char *name,
    char **content,
    size_t *content_len
);

/**
 * @brief Validate a .tscfg file at path (no copy)
 * 
 * Validates signature and recipient of a .tscfg file without copying
 * it to the config directory. Use this after file upload to validate
 * in-place.
 * 
 * @param path Full path to .tscfg file
 * @param metadata Output metadata (optional, can be NULL)
 * @return TS_CONFIG_PACK_OK if valid and intended for this device
 */
ts_config_pack_result_t ts_config_pack_validate_file(
    const char *path,
    ts_config_pack_metadata_t *metadata
);

/**
 * @brief Apply configuration from a validated .tscfg file
 * 
 * Decrypts and applies the configuration from a .tscfg file.
 * The file should have been validated first.
 * 
 * @param path Full path to .tscfg file
 * @param applied_modules Output: JSON array of applied module names (optional)
 * @return TS_CONFIG_PACK_OK on success
 */
ts_config_pack_result_t ts_config_pack_apply_file(
    const char *path,
    cJSON **applied_modules
);

/*===========================================================================*/
/*                      Utility Functions                                     */
/*===========================================================================*/

/**
 * @brief Get human-readable error message
 * 
 * @param result Result code
 * @return Error message string
 */
const char *ts_config_pack_strerror(ts_config_pack_result_t result);

/**
 * @brief Export device certificate for config encryption
 * 
 * Outputs the device certificate that others can use to encrypt configs
 * intended for this device.
 * 
 * @param cert_pem Output buffer for PEM certificate
 * @param cert_len Input: buffer size; Output: actual length
 * @return ESP_OK on success
 */
esp_err_t ts_config_pack_export_device_cert(char *cert_pem, size_t *cert_len);

/**
 * @brief Get device certificate fingerprint
 * 
 * Returns SHA256 fingerprint of device certificate in hex format.
 * 
 * @param fingerprint Output buffer (at least 65 bytes for hex + null)
 * @param len Buffer length
 * @return ESP_OK on success
 */
esp_err_t ts_config_pack_get_cert_fingerprint(char *fingerprint, size_t len);

/**
 * @brief Load JSON content with .tscfg priority
 * 
 * If a .tscfg version of the file exists, load and decrypt it.
 * Otherwise, load the plain .json file.
 * 
 * Example: For "/sdcard/config/network.json":
 *   - First check "/sdcard/config/network.tscfg"
 *   - If exists and valid, decrypt and return content
 *   - Otherwise load "/sdcard/config/network.json"
 * 
 * @param json_path Path to .json file (e.g., "/sdcard/config/network.json")
 * @param content Output: JSON content string (caller must free)
 * @param content_len Output: content length (optional, can be NULL)
 * @param used_tscfg Output: true if .tscfg was used (optional, can be NULL)
 * @return ESP_OK on success
 */
esp_err_t ts_config_pack_load_with_priority(
    const char *json_path,
    char **content,
    size_t *content_len,
    bool *used_tscfg
);

/**
 * @brief Check if .tscfg version exists for a .json file
 * 
 * @param json_path Path to .json file
 * @return true if corresponding .tscfg exists
 */
bool ts_config_pack_tscfg_exists(const char *json_path);

#ifdef __cplusplus
}
#endif

#endif /* TS_CONFIG_PACK_H */
