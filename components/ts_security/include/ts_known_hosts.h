/**
 * @file ts_known_hosts.h
 * @brief SSH Known Hosts Management
 *
 * This module provides SSH host key verification functionality,
 * storing and verifying server fingerprints to prevent MITM attacks.
 * Host keys are stored in NVS for persistence.
 */

#pragma once

#include "esp_err.h"
#include "ts_ssh_client.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/** Host key types */
typedef enum {
    TS_HOST_KEY_RSA,        /**< RSA key */
    TS_HOST_KEY_DSS,        /**< DSS/DSA key */
    TS_HOST_KEY_ECDSA_256,  /**< ECDSA 256-bit key */
    TS_HOST_KEY_ECDSA_384,  /**< ECDSA 384-bit key */
    TS_HOST_KEY_ECDSA_521,  /**< ECDSA 521-bit key */
    TS_HOST_KEY_ED25519,    /**< ED25519 key */
    TS_HOST_KEY_UNKNOWN     /**< Unknown key type */
} ts_host_key_type_t;

/** Host verification result */
typedef enum {
    TS_HOST_VERIFY_OK,          /**< Host key matches stored key */
    TS_HOST_VERIFY_NOT_FOUND,   /**< Host not in known hosts */
    TS_HOST_VERIFY_MISMATCH,    /**< Host key changed (potential MITM!) */
    TS_HOST_VERIFY_ERROR        /**< Verification error */
} ts_host_verify_result_t;

/** Host key information */
typedef struct {
    char host[64];              /**< Hostname or IP */
    uint16_t port;              /**< Port number */
    ts_host_key_type_t type;    /**< Key type */
    char fingerprint[64];       /**< SHA256 fingerprint (hex string) */
    uint32_t added_time;        /**< Unix timestamp when added */
} ts_known_host_t;

/** User prompt callback for unknown/changed hosts */
typedef bool (*ts_host_prompt_cb_t)(const ts_known_host_t *host, 
                                     ts_host_verify_result_t result,
                                     void *user_data);

/*===========================================================================*/
/*                          Initialization                                    */
/*===========================================================================*/

/**
 * @brief Initialize known hosts module
 *
 * Loads known hosts from NVS storage.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_known_hosts_init(void);

/**
 * @brief Deinitialize known hosts module
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_known_hosts_deinit(void);

/*===========================================================================*/
/*                          Host Verification                                 */
/*===========================================================================*/

/**
 * @brief Verify SSH server host key
 *
 * Checks if the server's host key matches the stored key.
 *
 * @param session Connected SSH session (before authentication)
 * @param result Pointer to receive verification result
 * @param host_info Pointer to receive host key info (optional, can be NULL)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_known_hosts_verify(ts_ssh_session_t session,
                                 ts_host_verify_result_t *result,
                                 ts_known_host_t *host_info);

/**
 * @brief Verify host with user prompt callback
 *
 * If host is unknown or key changed, calls the prompt callback
 * to let user decide whether to accept.
 *
 * @param session Connected SSH session
 * @param prompt_cb Callback function for user prompts
 * @param user_data User data passed to callback
 * @return esp_err_t ESP_OK if verification passed or user accepted
 */
esp_err_t ts_known_hosts_verify_interactive(ts_ssh_session_t session,
                                             ts_host_prompt_cb_t prompt_cb,
                                             void *user_data);

/*===========================================================================*/
/*                          Host Management                                   */
/*===========================================================================*/

/**
 * @brief Add or update host key
 *
 * @param session Connected SSH session to extract key from
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_known_hosts_add(ts_ssh_session_t session);

/**
 * @brief Add host key manually
 *
 * @param host Hostname or IP
 * @param port Port number
 * @param fingerprint SHA256 fingerprint (hex string)
 * @param type Key type
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_known_hosts_add_manual(const char *host, uint16_t port,
                                     const char *fingerprint,
                                     ts_host_key_type_t type);

/**
 * @brief Remove host from known hosts
 *
 * @param host Hostname or IP
 * @param port Port number (0 for any port)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_known_hosts_remove(const char *host, uint16_t port);

/**
 * @brief Get stored host key info
 *
 * @param host Hostname or IP
 * @param port Port number
 * @param info Pointer to receive host info
 * @return esp_err_t ESP_OK if found, ESP_ERR_NOT_FOUND if not stored
 */
esp_err_t ts_known_hosts_get(const char *host, uint16_t port,
                              ts_known_host_t *info);

/**
 * @brief List all known hosts
 *
 * @param hosts Array to store host info
 * @param max_hosts Maximum number of hosts to retrieve
 * @param count Pointer to receive actual count
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_known_hosts_list(ts_known_host_t *hosts, size_t max_hosts,
                               size_t *count);

/**
 * @brief Clear all known hosts
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_known_hosts_clear(void);

/**
 * @brief Get number of stored hosts
 *
 * @return Number of known hosts
 */
size_t ts_known_hosts_count(void);

/*===========================================================================*/
/*                          Utility Functions                                 */
/*===========================================================================*/

/**
 * @brief Convert key type to string
 *
 * @param type Key type
 * @return String representation
 */
const char *ts_host_key_type_str(ts_host_key_type_t type);

/**
 * @brief Get fingerprint from connected session
 *
 * @param session Connected SSH session
 * @param fingerprint Buffer to store fingerprint (at least 64 bytes)
 * @param type Pointer to receive key type (optional)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_known_hosts_get_fingerprint(ts_ssh_session_t session,
                                          char *fingerprint,
                                          ts_host_key_type_t *type);

#ifdef __cplusplus
}
#endif
