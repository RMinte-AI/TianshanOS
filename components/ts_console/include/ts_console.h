/**
 * @file ts_console.h
 * @brief TianShanOS Console System
 * 
 * CLI system based on esp_console with unified command registration,
 * argument parsing, and help system.
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#ifndef TS_CONSOLE_H
#define TS_CONSOLE_H

#include "esp_err.h"
#include "esp_console.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Version                                       */
/*===========================================================================*/

#define TS_CONSOLE_VERSION_MAJOR    1
#define TS_CONSOLE_VERSION_MINOR    0
#define TS_CONSOLE_VERSION_PATCH    0

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

#define TS_CONSOLE_MAX_LINE_LENGTH      256
#define TS_CONSOLE_MAX_HISTORY          50
#define TS_CONSOLE_MAX_ARGS             16
#define TS_CONSOLE_MAX_PROMPT_LENGTH    32
#define TS_CONSOLE_MAX_CMD_NAME         32
#define TS_CONSOLE_MAX_HELP_TEXT        256

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief Console output destination
 */
typedef enum {
    TS_CONSOLE_OUTPUT_UART = 0,     /**< UART output */
    TS_CONSOLE_OUTPUT_USB,          /**< USB CDC output */
    TS_CONSOLE_OUTPUT_TELNET,       /**< Telnet output */
    TS_CONSOLE_OUTPUT_MAX
} ts_console_output_t;

/**
 * @brief Command category for grouping
 */
typedef enum {
    TS_CMD_CAT_SYSTEM = 0,          /**< System commands */
    TS_CMD_CAT_CONFIG,              /**< Configuration commands */
    TS_CMD_CAT_HAL,                 /**< Hardware commands */
    TS_CMD_CAT_LED,                 /**< LED commands */
    TS_CMD_CAT_FAN,                 /**< Fan commands */
    TS_CMD_CAT_POWER,               /**< Power commands */
    TS_CMD_CAT_NETWORK,             /**< Network commands */
    TS_CMD_CAT_DEVICE,              /**< Device control commands */
    TS_CMD_CAT_DEBUG,               /**< Debug commands */
    TS_CMD_CAT_USER,                /**< User-defined commands */
    TS_CMD_CAT_MAX
} ts_cmd_category_t;

/**
 * @brief Command execution result
 */
typedef struct {
    int code;                        /**< Result code (0 = success) */
    char *message;                   /**< Result message (optional) */
    void *data;                      /**< Result data (optional) */
    size_t data_size;                /**< Size of result data */
} ts_cmd_result_t;

/**
 * @brief Console configuration
 */
typedef struct {
    const char *prompt;              /**< Console prompt string */
    int max_history;                 /**< Maximum history entries */
    ts_console_output_t output;      /**< Primary output destination */
    bool echo_enabled;               /**< Enable command echo */
    int task_priority;               /**< Console task priority */
    int task_stack_size;             /**< Console task stack size */
} ts_console_config_t;

/**
 * @brief Extended command registration structure
 */
typedef struct {
    const char *command;             /**< Command name */
    const char *help;                /**< Help text */
    const char *hint;                /**< Hint for argument completion */
    ts_cmd_category_t category;      /**< Command category */
    esp_console_cmd_func_t func;     /**< Command function */
    void *argtable;                  /**< Argument table (optional) */
} ts_console_cmd_t;

/**
 * @brief Output callback for custom output handling
 */
typedef void (*ts_console_output_cb_t)(const char *data, size_t len, void *user_data);

/*===========================================================================*/
/*                         Default Configuration                              */
/*===========================================================================*/

#define TS_CONSOLE_DEFAULT_CONFIG() { \
    .prompt = "tianshan> ", \
    .max_history = TS_CONSOLE_MAX_HISTORY, \
    .output = TS_CONSOLE_OUTPUT_UART, \
    .echo_enabled = true, \
    .task_priority = 5, \
    .task_stack_size = 4096 \
}

/*===========================================================================*/
/*                              Core API                                      */
/*===========================================================================*/

/**
 * @brief Initialize console system
 * 
 * @param config Console configuration, or NULL for defaults
 * @return ESP_OK on success
 */
esp_err_t ts_console_init(const ts_console_config_t *config);

/**
 * @brief Deinitialize console system
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_console_deinit(void);

/**
 * @brief Start console task
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_console_start(void);

/**
 * @brief Stop console task
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_console_stop(void);

/**
 * @brief Check if console is running
 * 
 * @return true if running
 */
bool ts_console_is_running(void);

/** Check whether console resources and command registration are initialized. */
bool ts_console_is_initialized(void);

/*===========================================================================*/
/*                           Command Registration                             */
/*===========================================================================*/

/**
 * @brief Register a command with extended information
 * 
 * @param cmd Command registration structure
 * @return ESP_OK on success
 */
esp_err_t ts_console_register_cmd(const ts_console_cmd_t *cmd);

/**
 * @brief Register multiple commands at once
 * 
 * @param cmds Array of command registrations
 * @param count Number of commands
 * @return ESP_OK on success
 */
esp_err_t ts_console_register_cmds(const ts_console_cmd_t *cmds, size_t count);

/**
 * @brief Unregister a command
 * 
 * @param cmd_name Command name
 * @return ESP_OK on success
 */
esp_err_t ts_console_unregister_cmd(const char *cmd_name);

/**
 * @brief Get command count
 * 
 * @return Number of registered commands
 */
size_t ts_console_get_cmd_count(void);

/**
 * @brief Get raw command line string
 * 
 * Returns the original command line before parsing.
 * Useful for extracting UTF-8 text that may be corrupted by argument parsing.
 * 
 * @return Pointer to raw command line (valid only during command execution)
 */
const char *ts_console_get_raw_cmdline(void);

/**
 * @brief Get commands by category
 * 
 * @param category Command category
 * @param cmds Array to store command names (can be NULL for count only)
 * @param max_cmds Maximum commands to retrieve
 * @return Number of commands in category
 */
size_t ts_console_get_cmds_by_category(ts_cmd_category_t category, 
                                        const char **cmds, size_t max_cmds);

/*===========================================================================*/
/*                           Command Execution                                */
/*===========================================================================*/

/**
 * @brief Execute a command line
 * 
 * @param cmdline Command line string
 * @param result Optional result structure
 * @return ESP_OK on success
 */
esp_err_t ts_console_exec(const char *cmdline, ts_cmd_result_t *result);

/**
 * @brief Execute a command with arguments
 * 
 * @param cmd_name Command name
 * @param argc Argument count
 * @param argv Argument values
 * @param result Optional result structure
 * @return ESP_OK on success
 */
esp_err_t ts_console_exec_cmd(const char *cmd_name, int argc, char **argv,
                               ts_cmd_result_t *result);

/*===========================================================================*/
/*                              Output                                        */
/*===========================================================================*/

/**
 * @brief Print formatted output to console
 * 
 * @param fmt Format string
 * @param ... Arguments
 * @return Number of characters printed
 */
int ts_console_printf(const char *fmt, ...);

/**
 * @brief Print error message to console
 * 
 * @param fmt Format string
 * @param ... Arguments
 * @return Number of characters printed
 */
int ts_console_error(const char *fmt, ...);

/**
 * @brief Print warning message to console
 * 
 * @param fmt Format string
 * @param ... Arguments
 * @return Number of characters printed
 */
int ts_console_warn(const char *fmt, ...);

/**
 * @brief Print success message to console
 * 
 * @param fmt Format string
 * @param ... Arguments
 * @return Number of characters printed
 */
int ts_console_success(const char *fmt, ...);

/**
 * @brief Set custom output callback
 * 
 * @param cb Output callback function
 * @param user_data User data passed to callback
 * @return ESP_OK on success
 */
esp_err_t ts_console_set_output_cb(ts_console_output_cb_t cb, void *user_data);

/**
 * @brief Clear custom output callback
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_console_clear_output_cb(void);

/*===========================================================================*/
/*                           Prompt Management                                */
/*===========================================================================*/

/**
 * @brief Set console prompt
 * 
 * @param prompt New prompt string
 * @return ESP_OK on success
 */
esp_err_t ts_console_set_prompt(const char *prompt);

/**
 * @brief Get current prompt
 * 
 * @return Current prompt string
 */
const char *ts_console_get_prompt(void);

/*===========================================================================*/
/*                           History Management                               */
/*===========================================================================*/

/**
 * @brief Add command to history
 * 
 * @param cmdline Command line to add
 * @return ESP_OK on success
 */
esp_err_t ts_console_history_add(const char *cmdline);

/**
 * @brief Clear command history
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_console_history_clear(void);

/**
 * @brief Save history to file
 * 
 * @param path File path
 * @return ESP_OK on success
 */
esp_err_t ts_console_history_save(const char *path);

/**
 * @brief Load history from file
 * 
 * @param path File path
 * @return ESP_OK on success
 */
esp_err_t ts_console_history_load(const char *path);

/*===========================================================================*/
/*                         Interrupt Handling                                 */
/*===========================================================================*/

/**
 * @brief Check if command interrupt (Ctrl+C) was requested
 * 
 * Long-running commands should periodically call this function
 * to check if the user has requested to abort the operation.
 * 
 * @return true if interrupt was requested
 */
bool ts_console_interrupted(void);

/**
 * @brief Clear the interrupt flag
 * 
 * Call this after handling an interrupt to reset the flag.
 */
void ts_console_clear_interrupt(void);

/**
 * @brief Request command interrupt
 * 
 * Called internally when Ctrl+C is detected during command execution.
 */
void ts_console_request_interrupt(void);

/**
 * @brief Start monitoring for Ctrl+C during command execution
 * 
 * This starts a background check for Ctrl+C input.
 * Call before executing long-running commands.
 */
void ts_console_begin_interruptible(void);

/**
 * @brief Stop monitoring for Ctrl+C
 * 
 * Call after command execution completes.
 */
void ts_console_end_interruptible(void);

/*===========================================================================*/
/*                          Built-in Commands                                 */
/*===========================================================================*/

/**
 * @brief Register all built-in commands
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_console_register_builtin_cmds(void);

/**
 * @brief Get category name
 * 
 * @param category Category enum
 * @return Category name string
 */
const char *ts_console_category_name(ts_cmd_category_t category);

/*===========================================================================*/
/*                          Command Modules                                   */
/*===========================================================================*/

/**
 * @brief Register LED commands
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_cmd_led_register(void);

#ifdef __cplusplus
}
#endif

#endif /* TS_CONSOLE_H */
