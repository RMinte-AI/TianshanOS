/**
 * @file ts_action_manager.h
 * @brief TianShanOS Automation Engine - Action Manager
 *
 * Provides unified action execution for automation rules.
 * 
 * Supported action types:
 *   - SSH command execution (sync/async)
 *   - LED control (board/touch/matrix)
 *   - GPIO control (set level, pulse)
 *   - Log, variable, delay, etc.
 *
 * Features:
 *   - Action queue with priority support
 *   - Async execution with callbacks
 *   - Result storage in variables
 *   - Timeout and error handling
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_ACTION_MANAGER_H
#define TS_ACTION_MANAGER_H

#include "esp_err.h"
#include "ts_automation_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

/** Maximum action queue size */
#define TS_ACTION_QUEUE_SIZE        16

/** Default SSH command timeout (ms) */
#define TS_ACTION_SSH_TIMEOUT_MS    30000

/** Default GPIO pulse duration (ms) */
#define TS_ACTION_GPIO_PULSE_MS     100

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief Action execution status
 */
typedef enum {
    TS_ACTION_STATUS_PENDING = 0,   /**< Waiting to execute */
    TS_ACTION_STATUS_RUNNING,        /**< Currently executing */
    TS_ACTION_STATUS_SUCCESS,        /**< Completed successfully */
    TS_ACTION_STATUS_FAILED,         /**< Execution failed */
    TS_ACTION_STATUS_TIMEOUT,        /**< Execution timed out */
    TS_ACTION_STATUS_CANCELLED,      /**< Cancelled */
    TS_ACTION_STATUS_QUEUED          /**< Queued for async execution */
} ts_action_status_t;

/**
 * @brief SSH host configuration for action manager
 */
typedef struct {
    char id[TS_AUTO_NAME_MAX_LEN];      /**< Host reference ID */
    char host[64];                       /**< Hostname or IP */
    uint16_t port;                       /**< SSH port */
    char username[32];                   /**< Username */
    char password[64];                   /**< Password (if using password auth) */
    char key_path[96];                   /**< Private key path (if using key auth) */
    bool use_key_auth;                   /**< Use key authentication */
} ts_action_ssh_host_t;

/**
 * @brief Action execution result
 */
typedef struct {
    ts_action_status_t status;          /**< Execution status */
    int exit_code;                       /**< Exit code (for SSH) */
    char output[256];                    /**< Output or error message */
    uint32_t duration_ms;                /**< Execution duration */
    int64_t timestamp;                   /**< Completion timestamp */
} ts_action_result_t;

/**
 * @brief Action completion callback
 */
typedef void (*ts_action_callback_t)(const ts_auto_action_t *action,
                                      const ts_action_result_t *result,
                                      void *user_data);

/**
 * @brief Action queue entry
 */
typedef struct {
    ts_auto_action_t action;            /**< Action definition */
    ts_action_callback_t callback;       /**< Completion callback */
    void *user_data;                     /**< User data for callback */
    uint8_t priority;                    /**< Priority (0=highest) */
    int64_t enqueue_time;                /**< When queued */
    /* Sync execution support */
    void *completion; /**< Shared completion lifetime, independent of waiter stack. */
    SemaphoreHandle_t done_sem;          /**< Signaled when done (sync mode) */
    ts_action_result_t *result_ptr;      /**< Where to store result (sync mode) */
} ts_action_queue_entry_t;

/**
 * @brief Action template definition
 * 
 * A named, reusable action configuration that can be referenced by rules
 * or executed manually via API.
 */
typedef struct {
    char id[TS_AUTO_NAME_MAX_LEN];       /**< Unique template ID */
    char name[TS_AUTO_LABEL_MAX_LEN];    /**< Human-readable name */
    char description[64];                 /**< Optional description */
    ts_auto_action_t action;              /**< Action configuration */
    bool enabled;                         /**< Is template enabled */
    bool async;                           /**< Execute asynchronously (non-blocking) */
    int64_t created_at;                   /**< Creation timestamp */
    int64_t last_used_at;                 /**< Last execution timestamp */
    uint32_t use_count;                   /**< Total execution count */
} ts_action_template_t;

/** Maximum number of action templates */
#define TS_ACTION_TEMPLATE_MAX  32

/*===========================================================================*/
/*                          Initialization                                    */
/*===========================================================================*/

/**
 * @brief Initialize action manager
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_action_manager_init(void);

/**
 * @brief Deinitialize action manager
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_action_manager_deinit(void);
esp_err_t ts_action_manager_quiesce(void);
bool ts_action_manager_accepting(void);
esp_err_t ts_action_manager_resume(void);

/**
 * @brief Check if action manager is initialized
 * 
 * @return true if initialized
 */
bool ts_action_manager_is_initialized(void);

/*===========================================================================*/
/*                          SSH Host Management                               */
/*===========================================================================*/

/**
 * @brief Register SSH host for action execution
 * 
 * @param host SSH host configuration
 * @return ESP_OK on success
 */
esp_err_t ts_action_register_ssh_host(const ts_action_ssh_host_t *host);

/**
 * @brief Unregister SSH host
 * 
 * @param host_id Host ID to remove
 * @return ESP_OK on success
 */
esp_err_t ts_action_unregister_ssh_host(const char *host_id);

/**
 * @brief Get SSH host by ID
 * 
 * @param host_id Host ID
 * @param host_out Output host config
 * @return ESP_OK if found
 */
esp_err_t ts_action_get_ssh_host(const char *host_id, ts_action_ssh_host_t *host_out);

/**
 * @brief Get number of registered SSH hosts
 * 
 * @return Number of hosts
 */
int ts_action_get_ssh_host_count(void);

/**
 * @brief Get all registered SSH hosts
 * 
 * @param hosts_out Output array (caller allocates, at least count items)
 * @param max_count Maximum number of hosts to return
 * @param count_out Actual number of hosts returned
 * @return ESP_OK on success
 */
esp_err_t ts_action_get_ssh_hosts(ts_action_ssh_host_t *hosts_out, 
                                   size_t max_count, 
                                   size_t *count_out);

/*===========================================================================*/
/*                          Action Execution                                  */
/*===========================================================================*/

/**
 * @brief Execute action immediately (blocking) with result
 * 
 * This is the Action Manager version with detailed result output.
 * Use ts_action_execute() from ts_rule_engine.h for simple execution.
 * 
 * @param action Action to execute
 * @param result Output result (optional, can be NULL)
 * @return ESP_OK on success
 */
esp_err_t ts_action_manager_execute(const ts_auto_action_t *action, 
                                     ts_action_result_t *result);

/**
 * @brief Queue action for async execution
 * 
 * @param action Action to execute
 * @param callback Completion callback (optional)
 * @param user_data User data for callback
 * @param priority Priority (0=highest)
 * @return ESP_OK on success
 */
esp_err_t ts_action_queue(const ts_auto_action_t *action,
                          ts_action_callback_t callback,
                          void *user_data,
                          uint8_t priority);

/**
 * @brief Execute multiple actions in sequence
 * 
 * @param actions Array of actions
 * @param count Number of actions
 * @param stop_on_error Stop if any action fails
 * @return ESP_OK if all succeeded
 */
esp_err_t ts_action_execute_sequence(const ts_auto_action_t *actions,
                                      int count,
                                      bool stop_on_error);

/**
 * @brief Cancel all pending actions
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_action_cancel_all(void);

/*===========================================================================*/
/*                       Individual Action Executors                          */
/*===========================================================================*/

/**
 * @brief Execute SSH command
 * 
 * @param ssh SSH action parameters
 * @param result Output result
 * @return ESP_OK on success
 */
esp_err_t ts_action_exec_ssh(const ts_auto_action_ssh_t *ssh,
                              ts_action_result_t *result);

/**
 * @brief Execute LED action
 * 
 * @param led LED action parameters
 * @param result Output result
 * @return ESP_OK on success
 */
esp_err_t ts_action_exec_led(const ts_auto_action_led_t *led,
                              ts_action_result_t *result);

/**
 * @brief Execute GPIO action
 * 
 * @param gpio GPIO action parameters
 * @param result Output result
 * @return ESP_OK on success
 */
esp_err_t ts_action_exec_gpio(const ts_auto_action_gpio_t *gpio,
                               ts_action_result_t *result);

/**
 * @brief Execute log action
 * 
 * @param log Log action parameters
 * @param result Output result
 * @return ESP_OK on success
 */
esp_err_t ts_action_exec_log(const ts_auto_action_log_t *log,
                              ts_action_result_t *result);

/**
 * @brief Execute set variable action
 * 
 * @param set_var Set variable parameters
 * @param result Output result
 * @return ESP_OK on success
 */
esp_err_t ts_action_exec_set_var(const ts_auto_action_set_var_t *set_var,
                                  ts_action_result_t *result);

/**
 * @brief Execute device control action
 * 
 * @param device Device control parameters
 * @param result Output result
 * @return ESP_OK on success
 */
esp_err_t ts_action_exec_device(const ts_auto_action_device_t *device,
                                 ts_action_result_t *result);

/**
 * @brief Execute SSH command reference action
 * 
 * Looks up a pre-registered SSH command by ID and executes it.
 * This allows actions to reference commands created in the SSH management UI.
 * 
 * @param ssh_ref SSH command reference parameters (contains cmd_id)
 * @param result Output result
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if command not found
 */
esp_err_t ts_action_exec_ssh_ref(const ts_auto_action_ssh_ref_t *ssh_ref,
                                  ts_action_result_t *result);

/**
 * @brief Execute CLI command action
 * 
 * Executes a TianShanOS CLI command locally. This is the unified method
 * for hardware control (GPIO, device power, fan, LED, etc.)
 * 
 * @param cli CLI command parameters
 * @param result Output result
 * @return ESP_OK on success
 */
esp_err_t ts_action_exec_cli(const ts_auto_action_cli_t *cli,
                              ts_action_result_t *result);

/*===========================================================================*/
/*                          Status & Statistics                               */
/*===========================================================================*/

/**
 * @brief Get action queue status
 * 
 * @param pending_out Number of pending actions
 * @param running_out Number of running actions
 * @return ESP_OK on success
 */
esp_err_t ts_action_get_queue_status(int *pending_out, int *running_out);

/**
 * @brief Action manager statistics
 */
typedef struct {
    uint32_t total_executed;            /**< Total actions executed */
    uint32_t total_success;             /**< Successful executions */
    uint32_t total_failed;              /**< Failed executions */
    uint32_t total_timeout;             /**< Timed out executions */
    uint32_t ssh_commands;              /**< SSH commands executed */
    uint32_t led_actions;               /**< LED actions executed */
    uint32_t gpio_actions;              /**< GPIO actions executed */
    uint32_t queue_high_water;          /**< Queue high water mark */
} ts_action_stats_t;

/**
 * @brief Get action manager statistics
 * 
 * @param stats_out Output statistics
 * @return ESP_OK on success
 */
esp_err_t ts_action_get_stats(ts_action_stats_t *stats_out);

/**
 * @brief Reset statistics
 */
void ts_action_reset_stats(void);

/*===========================================================================*/
/*                          Utility Functions                                 */
/*===========================================================================*/

/**
 * @brief Expand variable references in string
 * 
 * Replaces ${var_name} with actual variable values.
 * 
 * @param input Input string with variable references
 * @param output Output buffer
 * @param output_size Output buffer size
 * @return Number of characters written, -1 on error
 */
int ts_action_expand_variables(const char *input, char *output, size_t output_size);

/**
 * @brief Parse color string to RGB
 * 
 * Supports: "#RRGGBB", "rgb(r,g,b)", named colors
 * 
 * @param color_str Color string
 * @param r Output red
 * @param g Output green
 * @param b Output blue
 * @return ESP_OK if parsed successfully
 */
esp_err_t ts_action_parse_color(const char *color_str, uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * @brief Get action type name
 * 
 * @param type Action type
 * @return Type name string
 */
const char *ts_action_type_name(ts_auto_action_type_t type);

/**
 * @brief Get status name
 * 
 * @param status Action status
 * @return Status name string
 */
const char *ts_action_status_name(ts_action_status_t status);

/*===========================================================================*/
/*                       Action Template Management                           */
/*===========================================================================*/

/**
 * @brief Register an action template
 * 
 * @param tpl Template to register
 * @return ESP_OK on success, ESP_ERR_NO_MEM if max reached
 */
esp_err_t ts_action_template_add(const ts_action_template_t *tpl);

/**
 * @brief Unregister an action template
 * 
 * @param id Template ID to remove
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not exists
 */
esp_err_t ts_action_template_remove(const char *id);

/**
 * @brief Get action template by ID
 * 
 * @param id Template ID
 * @param tpl_out Output template (caller-provided)
 * @return ESP_OK if found
 */
esp_err_t ts_action_template_get(const char *id, ts_action_template_t *tpl_out);

/**
 * @brief Get number of registered templates
 * 
 * @return Number of templates
 */
int ts_action_template_count(void);

/**
 * @brief Get all registered templates
 * 
 * @param tpls_out Output array (caller allocates)
 * @param max_count Maximum templates to return
 * @param count_out Actual count returned
 * @return ESP_OK on success
 */
esp_err_t ts_action_template_list(ts_action_template_t *tpls_out,
                                   size_t max_count,
                                   size_t *count_out);

/**
 * @brief Execute an action template by ID
 * 
 * @param id Template ID
 * @param result Output result (optional)
 * @return ESP_OK on success
 */
esp_err_t ts_action_template_execute(const char *id, ts_action_result_t *result);

/**
 * @brief Update an action template
 * 
 * @param id Template ID to update
 * @param tpl Updated template data
 * @return ESP_OK on success
 */
esp_err_t ts_action_template_update(const char *id, const ts_action_template_t *tpl);

/*===========================================================================*/
/*                       Persistence Functions                                */
/*===========================================================================*/

/**
 * @brief Save all action templates to NVS
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_action_templates_save(void);

/**
 * @brief Load all action templates from NVS
 * 
 * If NVS is empty and SD card is mounted, will try to load from SD card file.
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_action_templates_load(void);

/**
 * @brief Load action templates from SD card file
 * 
 * Used for config recovery when NVS is empty.
 * 
 * @param filepath Path to JSON file (e.g., "/sdcard/config/actions.json")
 * @return ESP_OK on success
 */
esp_err_t ts_action_templates_load_from_file(const char *filepath);

/* Only actual admissions allocate snapshots; ordinary condition evaluation does not. */
esp_err_t ts_action_snapshot(const ts_auto_action_t *source, ts_auto_action_t *out);
void ts_action_snapshot_owner(const ts_auto_action_t *action, const char *rule);
void ts_action_snapshot_retain(const ts_auto_action_t *action);
void ts_action_snapshot_release(ts_auto_action_t *action);

#ifdef __cplusplus
}
#endif

#endif /* TS_ACTION_MANAGER_H */
