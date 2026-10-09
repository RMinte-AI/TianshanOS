/**
 * @file ts_ssh_client.h
 * @brief SSH Client for remote command execution
 * 
 * This module provides SSH client functionality for TianShanOS,
 * enabling secure remote command execution on other devices.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SSH authentication method */
typedef enum {
    TS_SSH_AUTH_PASSWORD = 0,   /**< Password authentication */
    TS_SSH_AUTH_PUBLICKEY       /**< Public key authentication */
} ts_ssh_auth_method_t;

/** SSH connection state */
typedef enum {
    TS_SSH_STATE_DISCONNECTED = 0,
    TS_SSH_STATE_CONNECTING,
    TS_SSH_STATE_AUTHENTICATING,
    TS_SSH_STATE_CONNECTED,
    TS_SSH_STATE_ERROR
} ts_ssh_state_t;

/** SSH session handle */
typedef struct ts_ssh_session_s *ts_ssh_session_t;

/** SSH connection configuration */
typedef struct {
    const char *host;               /**< Remote host address */
    uint16_t port;                  /**< SSH port (default: 22) */
    const char *username;           /**< Username for authentication */
    ts_ssh_auth_method_t auth_method;
    union {
        const char *password;       /**< Password (for PASSWORD auth) */
        struct {
            const uint8_t *private_key;     /**< Private key (PEM format, memory) */
            size_t private_key_len;         /**< Private key length */
            const char *private_key_path;   /**< Private key file path (alternative) */
            const char *passphrase;         /**< Key passphrase (optional) */
        } key;
    } auth;
    uint32_t timeout_ms;            /**< Connection timeout in ms */
    bool (*cancelled)(void *context); /**< Optional owner cancellation callback */
    void *cancel_context;
    size_t max_output_bytes; /**< Per-stream bound, default 64 KiB */
    bool verify_host_key;           /**< Verify server host key */
} ts_ssh_config_t;

/** Default SSH configuration */
#define TS_SSH_DEFAULT_CONFIG() { \
    .host = NULL, \
    .port = 22, \
    .username = NULL, \
    .auth_method = TS_SSH_AUTH_PASSWORD, \
    .auth.password = NULL, \
    .timeout_ms = 10000, \
    .verify_host_key = true \
}

/** Command execution result */
typedef struct {
    char *stdout_data;              /**< Standard output (caller must free) */
    size_t stdout_len;              /**< Standard output length */
    char *stderr_data;              /**< Standard error (caller must free) */
    size_t stderr_len;              /**< Standard error length */
    int exit_code;                  /**< Command exit code */
} ts_ssh_exec_result_t;

/** Callback for streaming command output */
typedef void (*ts_ssh_output_cb_t)(const char *data, size_t len, bool is_stderr, void *user_data);

/**
 * @brief Initialize SSH client subsystem
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_client_init(void);

/**
 * @brief Deinitialize SSH client subsystem
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_client_deinit(void);

/**
 * @brief Create a new SSH session
 * 
 * @param config SSH configuration
 * @param session_out Pointer to receive session handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_session_create(const ts_ssh_config_t *config, ts_ssh_session_t *session_out);

/**
 * @brief Destroy SSH session
 * 
 * @param session Session handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t session);

/**
 * @brief Connect SSH session
 * 
 * @param session Session handle
 * @return esp_err_t ESP_OK on success
 */
#define TS_SSH_ERR_HOST_UNKNOWN (ESP_ERR_INVALID_STATE + 0x400)
#define TS_SSH_ERR_HOST_CHANGED (ESP_ERR_INVALID_STATE + 0x401)
typedef esp_err_t (*ts_ssh_verify_cb_t)(ts_ssh_session_t session, void *context);
/* Callback is invoked after handshake and BEFORE any user authentication. */
esp_err_t ts_ssh_connect_with_verifier(ts_ssh_session_t session, ts_ssh_verify_cb_t verify, void *context);
esp_err_t ts_ssh_connect(ts_ssh_session_t session);

/**
 * @brief Disconnect SSH session
 * 
 * @param session Session handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_disconnect(ts_ssh_session_t session);

/**
 * @brief Check if session is connected
 * 
 * @param session Session handle
 * @return true if connected
 */
bool ts_ssh_is_connected(ts_ssh_session_t session);

/**
 * @brief Get session state
 * 
 * @param session Session handle
 * @return ts_ssh_state_t Current state
 */
ts_ssh_state_t ts_ssh_get_state(ts_ssh_session_t session);

/**
 * @brief Execute command on remote host
 * 
 * @param session Session handle
 * @param command Command to execute
 * @param result Pointer to receive result (caller must free stdout/stderr)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_exec(ts_ssh_session_t session, const char *command, 
                       ts_ssh_exec_result_t *result);

/**
 * @brief Execute command with streaming output
 * 
 * @param session Session handle
 * @param command Command to execute
 * @param callback Output callback
 * @param user_data User data for callback
 * @param exit_code Pointer to receive exit code (optional)
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT if interrupted
 */
esp_err_t ts_ssh_exec_stream(ts_ssh_session_t session, const char *command,
                              ts_ssh_output_cb_t callback, void *user_data,
                              int *exit_code);

/**
 * @brief Execute a command with stdin bytes and streaming output
 *
 * Sends input without including it in command text, then closes stdin.
 * The caller owns the input buffer and must clear sensitive bytes after use.
 */
esp_err_t ts_ssh_exec_stream_input(ts_ssh_session_t session, const char *command,
                                 const char *input, size_t input_len,
                                 ts_ssh_output_cb_t callback, void *user_data,
                                 int *exit_code);

/**
 * @brief Abort ongoing SSH operation
 * 
 * Signals the SSH stream to stop. The ts_ssh_exec_stream function
 * will return ESP_ERR_TIMEOUT on next iteration.
 * 
 * @param session Session handle
 */
void ts_ssh_abort(ts_ssh_session_t session);

/**
 * @brief Free execution result
 * 
 * @param result Result to free
 */
void ts_ssh_exec_result_free(ts_ssh_exec_result_t *result);

/**
 * @brief Get last error message
 * 
 * @param session Session handle
 * @return Error message string
 */
const char *ts_ssh_get_error(ts_ssh_session_t session);

/** Where the last failed ts_ssh_connect*() call stopped. The error text stays the same for all of them. */
typedef enum {
    TS_SSH_FAIL_NONE = 0,
    TS_SSH_FAIL_DNS,          /**< the host name could not be resolved */
    TS_SSH_FAIL_REFUSED,      /**< the host refused the TCP connection (nothing listens on the port) */
    TS_SSH_FAIL_UNREACHABLE,  /**< no route to the host or network */
    TS_SSH_FAIL_TIMEOUT,      /**< the deadline expired while connecting, in the handshake or while authenticating */
    TS_SSH_FAIL_CONNECT,      /**< another socket failure while connecting */
    TS_SSH_FAIL_HANDSHAKE,    /**< the SSH handshake failed (not an SSH server, no common algorithms, ...) */
    TS_SSH_FAIL_HOST_KEY,     /**< the host key was rejected (unknown or changed) */
    TS_SSH_FAIL_AUTH,         /**< the server rejected the credentials */
    TS_SSH_FAIL_OTHER         /**< anything else (out of memory, invalid configuration, ...) */
} ts_ssh_fail_t;

/**
 * @brief Get the stage at which the last connection attempt failed
 *
 * @param session Session handle
 * @return TS_SSH_FAIL_NONE if the last attempt did not fail
 */
ts_ssh_fail_t ts_ssh_get_fail_stage(ts_ssh_session_t session);

/**
 * @brief Reason token for a failed connection attempt; the Web UI turns it into a sentence (failure.reasons.ssh_*)
 *
 * @param session SSH session whose connection failed
 * @param stage Output: short name of the stage that failed ("dns", "refused", "timeout", ...), never NULL after the call
 * @return Reason token (never NULL)
 */
const char *ts_ssh_connect_reason(ts_ssh_session_t session, const char **stage);

/**
 * @brief Get the remote host address
 * 
 * @param session Session handle
 * @return Host address string or NULL
 */
const char *ts_ssh_get_host(ts_ssh_session_t session);

/**
 * @brief Get the remote port
 * 
 * @param session Session handle
 * @return Port number
 */
uint16_t ts_ssh_get_port(ts_ssh_session_t session);

/**
 * @brief Execute a simple command (convenience function)
 * 
 * This creates a session, connects, executes, and disconnects.
 * 
 * @param config SSH configuration
 * @param command Command to execute
 * @param result Pointer to receive result
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_exec_simple(const ts_ssh_config_t *config, const char *command,
                              ts_ssh_exec_result_t *result);

#ifdef __cplusplus
}
#endif
