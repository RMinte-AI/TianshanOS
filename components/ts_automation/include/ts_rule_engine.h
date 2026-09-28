/**
 * @file ts_rule_engine.h
 * @brief TianShanOS Automation Engine - Rule Engine API
 *
 * Provides rule evaluation and action execution:
 * - Condition evaluation with multiple operators
 * - Action sequencing with delays
 * - Rule cooldown management
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_RULE_ENGINE_H
#define TS_RULE_ENGINE_H

#include "ts_automation_types.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                           Initialization                                   */
/*===========================================================================*/

/**
 * @brief Initialize rule engine
 *
 * @return ESP_OK on success
 */
esp_err_t ts_rule_engine_init(void);

/**
 * @brief Deinitialize rule engine
 *
 * @return ESP_OK on success
 */
esp_err_t ts_rule_engine_deinit(void);

/*===========================================================================*/
/*                           Rule Management                                  */
/*===========================================================================*/

/**
 * @brief Register a rule
 *
 * @param rule Rule definition
 * @return ESP_OK on success
 */
esp_err_t ts_rule_register(const ts_auto_rule_t *rule);

/**
 * @brief Unregister a rule
 *
 * @param id Rule ID
 * @return ESP_OK on success
 */
esp_err_t ts_rule_unregister(const char *id);

/**
 * @brief Enable a rule
 *
 * @param id Rule ID
 * @return ESP_OK on success
 */
esp_err_t ts_rule_enable(const char *id);

/**
 * @brief Disable a rule
 *
 * @param id Rule ID
 * @return ESP_OK on success
 */
esp_err_t ts_rule_disable(const char *id);

/**
 * @brief Get rule by ID
 *
 * @param id Rule ID
 * @return Rule pointer or NULL
 */
typedef struct {int applied,durable,mirror_synced;uint32_t revision;const char *error_code;} ts_rule_commit_result_t;
esp_err_t ts_rule_acquire(const char *id,ts_auto_rule_t *out);
void ts_rule_release(ts_auto_rule_t *rule);
esp_err_t ts_rule_commit(const ts_auto_rule_t *candidate,const char *id,uint32_t expected_revision,ts_rule_commit_result_t *result);

/**
 * @brief Get number of registered rules
 *
 * @return Rule count
 */
int ts_rule_count(void);

/*===========================================================================*/
/*                           Evaluation                                       */
/*===========================================================================*/

/**
 * @brief Evaluate all rules
 *
 * Checks all enabled rules and triggers actions for matching conditions.
 * Called periodically by automation engine.
 *
 * @return Number of rules triggered
 */
int ts_rule_evaluate_all(void);

/**
 * @brief Evaluate a specific rule
 *
 * @param id Rule ID
 * @param triggered Output: true if rule was triggered
 * @return ESP_OK on success
 */
esp_err_t ts_rule_evaluate(const char *id, bool *triggered);

/**
 * @brief Manually trigger a rule (bypass conditions)
 *
 * @param id Rule ID
 * @return ESP_OK on success
 */
esp_err_t ts_rule_trigger(const char *id);

/*===========================================================================*/
/*                           Condition Evaluation                             */
/*===========================================================================*/

/**
 * @brief Evaluate a single condition
 *
 * @param condition Condition to evaluate
 * @return true if condition is met
 */
bool ts_rule_eval_condition(const ts_auto_condition_t *condition);

/**
 * @brief Evaluate a condition group
 *
 * @param group Condition group
 * @return true if group conditions are met
 */
bool ts_rule_eval_condition_group(const ts_auto_condition_group_t *group);

/*===========================================================================*/
/*                           Action Execution                                 */
/*===========================================================================*/

/**
 * @brief Action result callback
 *
 * @param action Action that was executed
 * @param result Result code
 * @param user_data User data
 */
typedef void (*ts_action_result_cb_t)(const ts_auto_action_t *action, 
                                       esp_err_t result, void *user_data);

/**
 * @brief Execute a single action
 *
 * @param action Action to execute
 * @return ESP_OK on success
 */
esp_err_t ts_action_execute(const ts_auto_action_t *action);

/**
 * @brief Execute action array with delays
 *
 * @param actions Array of actions
 * @param count Number of actions
 * @param callback Optional result callback
 * @param user_data Callback user data
 * @return ESP_OK if execution started
 */
esp_err_t ts_action_execute_array(const ts_auto_action_t *actions, int count,
                                   ts_action_result_cb_t callback, void *user_data);

/*===========================================================================*/
/*                           Rule Access                                      */
/*===========================================================================*/

/**
 * @brief Get rule by index
 *
 * @param index Rule index (0 to rule_count-1)
 * @param rule Pinned output; caller MUST ts_rule_release after use
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if index out of range
 */
esp_err_t ts_rule_get_by_index(int index, ts_auto_rule_t *rule);

/**
 * @brief Get total number of registered rules
 *
 * @return Rule count
 */
int ts_rule_count(void);

/*===========================================================================*/
/*                           Statistics                                       */
/*===========================================================================*/

/**
 * @brief Rule engine statistics
 */
typedef struct {
    uint32_t total_evaluations;          /**< Total rule evaluations */
    uint32_t total_triggers;             /**< Total rule triggers */
    uint32_t total_actions;              /**< Total actions executed */
    uint32_t failed_actions;             /**< Failed action count */
    int64_t last_evaluation_ms;          /**< Last evaluation timestamp */
} ts_rule_engine_stats_t;

/**
 * @brief Get rule engine statistics
 *
 * @param stats Output statistics
 * @return ESP_OK on success
 */
esp_err_t ts_rule_engine_get_stats(ts_rule_engine_stats_t *stats);

/**
 * @brief Reset statistics
 *
 * @return ESP_OK on success
 */
esp_err_t ts_rule_engine_reset_stats(void);

/*===========================================================================*/
/*                        Execution History (Lightweight)                     */
/*===========================================================================*/

/** Maximum execution history records (ring buffer size) */
#define TS_RULE_EXEC_HISTORY_SIZE  16

/** Maximum message length in execution record */
#define TS_RULE_EXEC_MSG_LEN       48

/**
 * @brief Trigger source for rule execution
 */
typedef enum {
    TS_RULE_TRIGGER_CONDITION = 0,   /**< Triggered by condition evaluation */
    TS_RULE_TRIGGER_MANUAL,          /**< Manually triggered via API */
    TS_RULE_TRIGGER_TIMER,           /**< Triggered by timer */
    TS_RULE_TRIGGER_STARTUP,         /**< Triggered on startup */
} ts_rule_trigger_source_t;

/**
 * @brief Execution result status
 */
typedef enum {
    TS_RULE_EXEC_SUCCESS = 0,        /**< All actions succeeded */
    TS_RULE_EXEC_PARTIAL,            /**< Some actions failed */
    TS_RULE_EXEC_FAILED,             /**< All actions failed */
    TS_RULE_EXEC_SKIPPED,            /**< Rule skipped (cooldown/disabled) */
} ts_rule_exec_status_t;

/**
 * @brief Single rule execution record (~96 bytes)
 * 
 * Lightweight structure for tracking recent rule executions.
 * Uses fixed-size fields to minimize memory fragmentation.
 */
typedef struct {
    char rule_id[32];                /**< Rule ID */
    int64_t timestamp_ms;            /**< Execution timestamp (milliseconds since boot) */
    ts_rule_exec_status_t status;    /**< Execution result status */
    ts_rule_trigger_source_t source; /**< Trigger source */
    char message[TS_RULE_EXEC_MSG_LEN]; /**< Brief result message */
    uint8_t action_count;            /**< Total actions attempted */
    uint8_t failed_count;            /**< Failed action count */
    uint8_t _reserved[2];            /**< Alignment padding */
} ts_rule_exec_record_t;

/**
 * @brief Get recent execution history
 * 
 * Returns up to `max_count` most recent execution records.
 * Records are returned in reverse chronological order (newest first).
 *
 * @param records Output array for records
 * @param max_count Maximum records to retrieve
 * @param actual_count Output: actual number of records returned
 * @return ESP_OK on success
 */
esp_err_t ts_rule_get_exec_history(ts_rule_exec_record_t *records, 
                                    int max_count, int *actual_count);

/**
 * @brief Get execution history for a specific rule
 *
 * @param rule_id Rule ID to filter by
 * @param records Output array for records
 * @param max_count Maximum records to retrieve
 * @param actual_count Output: actual number of records returned
 * @return ESP_OK on success
 */
esp_err_t ts_rule_get_exec_history_by_id(const char *rule_id,
                                          ts_rule_exec_record_t *records,
                                          int max_count, int *actual_count);

/**
 * @brief Clear execution history
 *
 * @return ESP_OK on success
 */
esp_err_t ts_rule_clear_exec_history(void);

/**
 * @brief Get execution status as human-readable string
 *
 * @param status Execution status
 * @return Static string description
 */
const char *ts_rule_exec_status_str(ts_rule_exec_status_t status);

/*===========================================================================*/
/*                           Persistence                                      */
/*===========================================================================*/

/**
 * @brief Save all rules to NVS
 *
 * @return ESP_OK on success
 */
esp_err_t ts_rules_save(void);

/**
 * @brief Load all rules from NVS
 * 
 * If NVS is empty and SD card is mounted, will try to load from SD card file.
 *
 * @return ESP_OK on success
 */
esp_err_t ts_rules_load(void);

/**
 * @brief Load rules from SD card file
 * 
 * Used for config recovery when NVS is empty.
 *
 * @param filepath Path to JSON file (e.g., "/sdcard/config/rules.json")
 * @return ESP_OK on success
 */
esp_err_t ts_rules_load_from_file(const char *filepath);

/* Cold API/load path only: may allocate and resolve command/template references. */
void ts_rule_resolve_presentation(ts_auto_rule_t *rule);
bool ts_rule_edit_begin(void);
void ts_rule_edit_end(void);
void ts_rule_config_status(bool *loaded, bool *recovery);

#ifdef __cplusplus
}
#endif

#endif /* TS_RULE_ENGINE_H */
