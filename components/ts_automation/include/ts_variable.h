/**
 * @file ts_variable.h
 * @brief TianShanOS Automation Engine - Variable Storage API
 *
 * Provides variable storage with:
 * - Hierarchical namespacing (e.g., "agx.power", "lpmu.0.status")
 * - Change notification via event bus
 * - Saved user definitions with runtime observations
 * - Expression evaluation for computed variables
 *
 * @author TianShanOS Team
 * @version 1.0.0
 */

#ifndef TS_VARIABLE_H
#define TS_VARIABLE_H

#include "ts_automation_types.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t ts_variable_instance(const char *name);

/*===========================================================================*/
/*                           Event Definitions                                */
/*===========================================================================*/

/**
 * @brief Variable change event data
 */
typedef struct {
    const char *name;                    /**< Variable name */
    ts_auto_value_t old_value;           /**< Previous value */
    ts_auto_value_t new_value;           /**< New value */
} ts_variable_change_event_t;

/**
 * @brief Public variable metadata snapshot
 */
typedef struct {
    char name[TS_AUTO_NAME_MAX_LEN];      /**< Variable name */
    char source_id[TS_AUTO_NAME_MAX_LEN]; /**< Owning source ID */
    ts_auto_value_t value;                /**< Current value */
    uint32_t flags;                       /**< Variable flags */
    int64_t last_change_ms;               /**< Last value-change timestamp */
    int64_t last_update_ms;               /**< Last real update timestamp */
} ts_variable_info_t;

/*===========================================================================*/
/*                           Initialization                                   */
/*===========================================================================*/

/**
 * @brief Initialize variable storage
 *
 * @return ESP_OK on success
 */
esp_err_t ts_variable_init(void);

/**
 * @brief Deinitialize variable storage
 *
 * @return ESP_OK on success
 */
esp_err_t ts_variable_deinit(void);

/**
 * @brief Check if variable storage is initialized
 *
 * @return true if initialized, false otherwise
 */
bool ts_variable_is_initialized(void);

/*===========================================================================*/
/*                           Variable Registration                            */
/*===========================================================================*/

/**
 * @brief Register a new variable
 *
 * @param var Variable definition
 * @return ESP_OK on success
 */
esp_err_t ts_variable_register(const ts_auto_variable_t *var);

/**
 * @brief Create or update a variable with a real observed value
 *
 * Unlike ts_variable_register(), this marks last_update_ms because the caller
 * is publishing current source data, not just declaring a variable.
 *
 * @param var Variable definition and current value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_upsert(const ts_auto_variable_t *var);

/**
 * @brief Unregister a variable
 *
 * @param name Variable name
 * @return ESP_OK on success
 */
esp_err_t ts_variable_unregister(const char *name);

/**
 * @brief Unregister all variables associated with a source
 *
 * @param source_id Source ID to match
 * @return Number of variables removed
 */
int ts_variable_unregister_by_source(const char *source_id);

/**
 * @brief Check if variable exists
 *
 * @param name Variable name
 * @return true if exists
 */
bool ts_variable_exists(const char *name);

/*===========================================================================*/
/*                           Value Access                                     */
/*===========================================================================*/

/**
 * @brief Get variable value
 *
 * @param name Variable name
 * @param value Output value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_get(const char *name, ts_auto_value_t *value);

/**
 * @brief Get variable as boolean
 *
 * @param name Variable name
 * @param value Output value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_get_bool(const char *name, bool *value);

/**
 * @brief Get variable as integer
 *
 * @param name Variable name
 * @param value Output value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_get_int(const char *name, int32_t *value);

/**
 * @brief Get variable as float
 *
 * @param name Variable name
 * @param value Output value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_get_float(const char *name, double *value);

/**
 * @brief Get variable as string
 *
 * @param name Variable name
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return ESP_OK on success
 */
esp_err_t ts_variable_get_string(const char *name, char *buffer, size_t buffer_size);

/**
 * @brief Get public variable metadata and value
 *
 * @param name Variable name
 * @param info Output metadata snapshot
 * @return ESP_OK on success
 */
esp_err_t ts_variable_get_info(const char *name, ts_variable_info_t *info);

/**
 * @brief Try to get public variable metadata and value without waiting
 *
 * @param name Variable name
 * @param info Output metadata snapshot
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if storage is busy
 */
esp_err_t ts_variable_get_info_try(const char *name, ts_variable_info_t *info);

/*===========================================================================*/
/*                           Value Modification                               */
/*===========================================================================*/

/**
 * @brief Set variable value
 *
 * @param name Variable name
 * @param value New value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_set(const char *name, const ts_auto_value_t *value);

/**
 * @brief Set variable value (internal use, bypasses readonly check)
 *
 * Used by system components to update readonly variables they own.
 *
 * @param name Variable name
 * @param value New value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_set_internal(const char *name, const ts_auto_value_t *value);

/**
 * @brief Set variable to boolean
 *
 * @param name Variable name
 * @param value Boolean value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_set_bool(const char *name, bool value);

/**
 * @brief Set variable to integer
 *
 * @param name Variable name
 * @param value Integer value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_set_int(const char *name, int32_t value);

/**
 * @brief Set variable to float
 *
 * @param name Variable name
 * @param value Float value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_set_float(const char *name, double value);

/**
 * @brief Set variable to string
 *
 * @param name Variable name
 * @param value String value
 * @return ESP_OK on success
 */
esp_err_t ts_variable_set_string(const char *name, const char *value);

/*===========================================================================*/
/*                           Enumeration                                      */
/*===========================================================================*/

/**
 * @brief Variable enumeration callback
 *
 * @param var Variable definition
 * @param user_data User data
 * @return true to continue, false to stop
 */
typedef bool (*ts_variable_enum_cb_t)(const ts_auto_variable_t *var, void *user_data);

/**
 * @brief Enumerate all variables
 *
 * @param prefix Optional prefix filter (NULL for all)
 * @param callback Callback function
 * @param user_data User data for callback
 * @return Number of variables enumerated
 */
int ts_variable_enumerate(const char *prefix, ts_variable_enum_cb_t callback, void *user_data);

/**
 * @brief Get number of registered variables
 *
 * @return Variable count
 */
int ts_variable_count(void);

/*===========================================================================*/
/*                           Iteration                                        */
/*===========================================================================*/

/**
 * @brief Variable iteration context
 */
typedef struct {
    int index;                           /**< Current index */
    int _internal;                       /**< Internal use */
} ts_variable_iterate_ctx_t;

/**
 * @brief Iterate over all variables
 *
 * Usage:
 *   ts_variable_iterate_ctx_t ctx = {0};
 *   ts_auto_variable_t var;
 *   while (ts_variable_iterate(&ctx, &var) == ESP_OK) { ... }
 *
 * @param ctx Iteration context (initialize with {0})
 * @param var Output variable
 * @return ESP_OK if variable returned, ESP_ERR_NOT_FOUND when done
 */
esp_err_t ts_variable_iterate(ts_variable_iterate_ctx_t *ctx, ts_auto_variable_t *var);

/* Bracket identity checks and writes against registry removal/recreation. */
void ts_variable_registration_begin(void);
void ts_variable_registration_end(void);

#ifdef __cplusplus
}
#endif

#endif /* TS_VARIABLE_H */
