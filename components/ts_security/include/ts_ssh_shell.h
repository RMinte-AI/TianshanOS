/**
 * @file ts_ssh_shell.h
 * @brief SSH Interactive Shell API
 * 
 * This module provides interactive shell functionality over SSH,
 * allowing PTY allocation and bidirectional terminal I/O.
 * 
 * Use cases:
 * - Remote terminal access to AGX/Jetson devices
 * - Interactive configuration sessions
 * - Remote debugging and maintenance
 */

#pragma once

#include "esp_err.h"
#include "ts_ssh_client.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Shell state */
typedef enum {
    TS_SHELL_STATE_IDLE = 0,
    TS_SHELL_STATE_RUNNING,
    TS_SHELL_STATE_CLOSED,
    TS_SHELL_STATE_ERROR
} ts_shell_state_t;

/** Shell handle */
typedef struct ts_ssh_shell_s *ts_ssh_shell_t;

/** Terminal type */
typedef enum {
    TS_TERM_XTERM = 0,      /**< xterm (default, most compatible) */
    TS_TERM_VT100,          /**< VT100 terminal */
    TS_TERM_VT220,          /**< VT220 terminal */
    TS_TERM_ANSI,           /**< ANSI terminal */
    TS_TERM_DUMB            /**< Dumb terminal (no escape sequences) */
} ts_term_type_t;

/** Terminal modes (optional, for advanced use) */
typedef struct {
    bool echo;              /**< Enable local echo (default: true) */
    bool raw_mode;          /**< Raw mode (no line editing) (default: false) */
    bool signal;            /**< Process signals (Ctrl+C, etc.) (default: true) */
} ts_term_modes_t;

/** Shell configuration */
typedef struct {
    ts_term_type_t term_type;   /**< Terminal type */
    uint16_t term_width;        /**< Terminal width in characters (default: 80) */
    uint16_t term_height;       /**< Terminal height in characters (default: 24) */
    ts_term_modes_t modes;      /**< Terminal modes */
    const char *env_vars;       /**< Environment variables (format: "VAR1=val1,VAR2=val2") */
    uint32_t read_timeout_ms;   /**< Read timeout in ms (default: 100) */
} ts_shell_config_t;

/** Default shell configuration */
#define TS_SHELL_DEFAULT_CONFIG() { \
    .term_type = TS_TERM_XTERM, \
    .term_width = 80, \
    .term_height = 24, \
    .modes = { \
        .echo = true, \
        .raw_mode = false, \
        .signal = true \
    }, \
    .env_vars = NULL, \
    .read_timeout_ms = 100 \
}

/** Shell output callback (called when data received from remote) */
typedef void (*ts_shell_output_cb_t)(const char *data, size_t len, void *user_data);

/** Shell close callback (called when shell closed) */
typedef void (*ts_shell_close_cb_t)(int exit_code, void *user_data);

/**
 * @brief Open an interactive shell session
 * 
 * This allocates a PTY and starts a shell on the remote host.
 * 
 * @param session SSH session (must be connected)
 * @param config Shell configuration (NULL for defaults)
 * @param shell_out Pointer to receive shell handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_shell_open(ts_ssh_session_t session,
                             const ts_shell_config_t *config,
                             ts_ssh_shell_t *shell_out);

/**
 * @brief Consume and release an allocated Shell handle, including EOF/ERROR.
 * Caller must clear its handle; a second call with a freed pointer is invalid.
 * If channel release would block, locally disconnect its owning SSH session.
 * This does not confirm that remote processes have terminated.
 * 
 * @param shell Shell handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_shell_close(ts_ssh_shell_t shell);

/** Request local loop exit without freeing the handle or confirming remote exit.
 * The caller serializes this with other Shell access and still calls close(). */
esp_err_t ts_ssh_shell_request_close(ts_ssh_shell_t shell);

/**
 * @brief Write data to the shell (send to remote)
 * 
 * @param shell Shell handle
 * @param data Data to write
 * @param len Length of data
 * @param written Bytes accepted by the transport, including partial failure (optional).
 * Write/setup waits are bounded; timeout leaves ERROR and must not be replayed.
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_shell_write(ts_ssh_shell_t shell,
                              const char *data, size_t len,
                              size_t *written);

/**
 * @brief Read data from the shell (receive from remote)
 * 
 * This is non-blocking if no data is available.
 * 
 * @param shell Shell handle
 * @param buffer Buffer to receive data
 * @param buffer_size Buffer size
 * @param read_len Pointer to receive bytes read
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT if no data
 */
esp_err_t ts_ssh_shell_read(ts_ssh_shell_t shell,
                             char *buffer, size_t buffer_size,
                             size_t *read_len);

/**
 * @brief Set output callback for async data reception
 * 
 * When set, this callback is invoked whenever data is available
 * from the remote shell (requires calling ts_ssh_shell_poll).
 * 
 * @param shell Shell handle
 * @param cb Output callback
 * @param user_data User data for callback
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_shell_set_output_cb(ts_ssh_shell_t shell,
                                      ts_shell_output_cb_t cb,
                                      void *user_data);

/**
 * @brief Set close callback
 * 
 * @param shell Shell handle
 * @param cb Close callback
 * @param user_data User data for callback
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_shell_set_close_cb(ts_ssh_shell_t shell,
                                     ts_shell_close_cb_t cb,
                                     void *user_data);

/**
 * @brief Poll for shell events (call periodically)
 * 
 * This reads available data and invokes callbacks.
 * Call this in a loop or timer to process shell I/O.
 * 
 * @param shell Shell handle
 * @return esp_err_t ESP_OK if processed, ESP_ERR_TIMEOUT if no data,
 *                   ESP_ERR_INVALID_STATE if shell closed
 */
esp_err_t ts_ssh_shell_poll(ts_ssh_shell_t shell);

/**
 * @brief Run interactive shell loop (blocking)
 * 
 * This runs a simple interactive loop that:
 * - Reads data from the shell and calls output_cb
 * - Reads data from input_cb and writes to shell
 * 
 * Returns when shell closes or error occurs.
 * 
 * @param shell Shell handle
 * @param output_cb Callback for shell output
 * @param input_cb Callback for user input (return data to send, len in *out_len)
 * @param user_data User data for callbacks
 * @return esp_err_t ESP_OK on normal close, ESP_ERR_* on error
 */
typedef const char *(*ts_shell_input_cb_t)(size_t *out_len, void *user_data);

esp_err_t ts_ssh_shell_run(ts_ssh_shell_t shell,
                            ts_shell_output_cb_t output_cb,
                            ts_shell_input_cb_t input_cb,
                            void *user_data);

/**
 * @brief Change terminal window size
 * 
 * Sends SIGWINCH to remote process if terminal size changes.
 * 
 * @param shell Shell handle
 * @param width New width in characters
 * @param height New height in characters
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_shell_resize(ts_ssh_shell_t shell,
                               uint16_t width, uint16_t height);

/**
 * @brief Send signal to remote process
 * 
 * @param shell Shell handle
 * @param signal_name Signal name (e.g., "INT", "TERM", "KILL")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ts_ssh_shell_send_signal(ts_ssh_shell_t shell, const char *signal_name);

/**
 * @brief Get shell state
 * 
 * @param shell Shell handle
 * @return ts_shell_state_t Current state
 */
ts_shell_state_t ts_ssh_shell_get_state(ts_ssh_shell_t shell);

/**
 * @brief Check if shell is still active
 * 
 * @param shell Shell handle
 * @return true if shell is running
 */
bool ts_ssh_shell_is_active(ts_ssh_shell_t shell);

/**
 * @brief Get exit code (after shell closes)
 * 
 * @param shell Shell handle
 * @return Exit code, or -1 if not available
 */
int ts_ssh_shell_get_exit_code(ts_ssh_shell_t shell);

#ifdef __cplusplus
}
#endif
