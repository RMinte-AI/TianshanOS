/**
 * @file ts_api.h
 * @brief TianShanOS Core API Layer
 * 
 * Unified API layer providing consistent interface for CLI, WebUI,
 * and internal components. All commands and operations go through
 * this layer.
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 * @date 2026-01-15
 */

#ifndef TS_API_H
#define TS_API_H

#include "esp_err.h"
#include "cJSON.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Version                                       */
/*===========================================================================*/

#define TS_API_VERSION_MAJOR    1
#define TS_API_VERSION_MINOR    0
#define TS_API_VERSION_PATCH    0

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief API result codes
 */
typedef enum {
    TS_API_OK = 0,               /**< Success */
    TS_API_ERR_INVALID_ARG,      /**< Invalid argument */
    TS_API_ERR_NOT_FOUND,        /**< Resource not found */
    TS_API_ERR_NO_PERMISSION,    /**< Permission denied */
    TS_API_ERR_BUSY,             /**< Resource busy */
    TS_API_ERR_TIMEOUT,          /**< Operation timeout */
    TS_API_ERR_NO_MEM,           /**< Out of memory */
    TS_API_ERR_INTERNAL,         /**< Internal error */
    TS_API_ERR_NOT_SUPPORTED,    /**< Operation not supported */
    TS_API_ERR_HARDWARE,         /**< Hardware error */
    TS_API_ERR_CONNECTION,       /**< Connection failed */
    TS_API_ERR_AUTH,             /**< Authentication failed */
    TS_API_ERR_MAX
} ts_api_result_code_t;

/**
 * @brief API categories/modules
 */
typedef enum {
    TS_API_CAT_SYSTEM = 0,       /**< System APIs */
    TS_API_CAT_CONFIG,           /**< Configuration APIs */
    TS_API_CAT_HAL,              /**< Hardware APIs */
    TS_API_CAT_LED,              /**< LED APIs */
    TS_API_CAT_FAN,              /**< Fan control APIs */
    TS_API_CAT_POWER,            /**< Power management APIs */
    TS_API_CAT_NETWORK,          /**< Network APIs */
    TS_API_CAT_DEVICE,           /**< Device control APIs */
    TS_API_CAT_STORAGE,          /**< Storage APIs */
    TS_API_CAT_SECURITY,         /**< Security APIs (SSH/SFTP/keys) */
    TS_API_CAT_MAX
} ts_api_category_t;

/**
 * @brief API result structure
 */
typedef struct {
    ts_api_result_code_t code;   /**< Result code */
    char *message;               /**< Human-readable message */
    cJSON *data;                 /**< Result data (JSON) */
} ts_api_result_t;

/**
 * @brief API handler function type
 * 
 * @param params Input parameters (JSON)
 * @param result Output result structure
 * @return ESP_OK on success
 */
typedef esp_err_t (*ts_api_handler_t)(const cJSON *params, ts_api_result_t *result);

/**
 * @brief API endpoint definition
 */
typedef struct {
    const char *name;            /**< API name (e.g., "system.reboot") */
    const char *description;     /**< API description */
    ts_api_category_t category;  /**< API category */
    ts_api_handler_t handler;    /**< Handler function */
    bool requires_auth;          /**< Requires authentication */
    const char *permission;      /**< Required permission (optional) */
} ts_api_endpoint_t;

/*===========================================================================*/
/*                              Core API                                      */
/*===========================================================================*/

/**
 * @brief Initialize API layer
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_api_init(void);

/**
 * @brief Deinitialize API layer
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_api_deinit(void);

/**
 * @brief Register an API endpoint
 * 
 * @param endpoint Endpoint definition
 * @return ESP_OK on success
 */
esp_err_t ts_api_register(const ts_api_endpoint_t *endpoint);

/**
 * @brief Register multiple API endpoints
 * 
 * @param endpoints Array of endpoint definitions
 * @param count Number of endpoints
 * @return ESP_OK on success
 */
esp_err_t ts_api_register_multiple(const ts_api_endpoint_t *endpoints, size_t count);

/**
 * @brief Unregister an API endpoint
 * 
 * @param name API name
 * @return ESP_OK on success
 */
esp_err_t ts_api_unregister(const char *name);

/**
 * @brief Call an API endpoint
 * 
 * @param name API name
 * @param params Input parameters (JSON)
 * @param result Output result structure
 * @return ESP_OK on success
 */
esp_err_t ts_api_call(const char *name, const cJSON *params, ts_api_result_t *result);

/**
 * @brief Call an API endpoint with string parameters
 * 
 * @param name API name
 * @param params_json JSON string of parameters
 * @param result Output result structure
 * @return ESP_OK on success
 */
esp_err_t ts_api_call_str(const char *name, const char *params_json, ts_api_result_t *result);

/**
 * @brief Get list of registered APIs
 * 
 * @param category Filter by category, or TS_API_CAT_MAX for all
 * @return JSON array of API names
 */
cJSON *ts_api_list(ts_api_category_t category);

/**
 * @brief Get API endpoint info
 * 
 * @param name API name
 * @return JSON object with endpoint info, or NULL
 */
cJSON *ts_api_get_info(const char *name);

/*===========================================================================*/
/*                          Result Helpers                                    */
/*===========================================================================*/

/**
 * @brief Initialize result structure
 * 
 * @param result Result structure
 */
void ts_api_result_init(ts_api_result_t *result);

/**
 * @brief Free result structure contents
 * 
 * @param result Result structure
 */
void ts_api_result_free(ts_api_result_t *result);

/**
 * @brief Set result success
 * 
 * @param result Result structure
 * @param data Optional JSON data (ownership transferred)
 */
void ts_api_result_ok(ts_api_result_t *result, cJSON *data);

/**
 * @brief Set result error
 * 
 * @param result Result structure
 * @param code Error code
 * @param message Error message
 */
void ts_api_result_error(ts_api_result_t *result, ts_api_result_code_t code, 
                          const char *message);

/**
 * @brief Set result error with a reason the Web UI can state exactly
 *
 * The reason token (lower_snake, e.g. "ssh_auth_failed") becomes the message: the page translates it into the user's
 * language. `detail` (optional) goes to data.detail and is only shown in the page's folded details. The numeric `code`
 * is still the nearest error class, so an older page degrades to the class sentence.
 *
 * @param result Result structure
 * @param code Nearest error class
 * @param token Reason token
 * @param detail Optional technical detail (the device's own text), may be NULL
 */
void ts_api_result_reason(ts_api_result_t *result, ts_api_result_code_t code,
                          const char *token, const char *detail);

/**
 * @brief Convert result to JSON
 * 
 * @param result Result structure
 * @return JSON object
 */
cJSON *ts_api_result_to_json(const ts_api_result_t *result);

/**
 * @brief Convert result to JSON string
 * 
 * @param result Result structure
 * @return JSON string (must be freed by caller)
 */
char *ts_api_result_to_string(const ts_api_result_t *result);

/**
 * @brief Get result code name
 * 
 * @param code Result code
 * @return Code name string
 */
const char *ts_api_code_name(ts_api_result_code_t code);

/*===========================================================================*/
/*                          Category Info                                     */
/*===========================================================================*/

/**
 * @brief Get category name
 * 
 * @param category Category enum
 * @return Category name string
 */
const char *ts_api_category_name(ts_api_category_t category);

/**
 * @brief Get category by name
 * 
 * @param name Category name
 * @return Category enum, or TS_API_CAT_MAX if not found
 */
ts_api_category_t ts_api_category_by_name(const char *name);

/*===========================================================================*/
/*                          API Module Registration                           */
/*===========================================================================*/

/**
 * @brief Register all API modules
 * 
 * This function registers all available API endpoints from all modules.
 * Should be called after ts_api_init() during system startup.
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_api_register_all(void);

/**
 * @brief Register system APIs
 */
esp_err_t ts_api_system_register(void);

/**
 * @brief Register config APIs
 */
esp_err_t ts_api_config_register(void);

/**
 * @brief Register device APIs
 */
esp_err_t ts_api_device_register(void);

/**
 * @brief Register LED APIs
 */
esp_err_t ts_api_led_register(void);

/**
 * @brief Register network APIs
 */
esp_err_t ts_api_network_register(void);

/**
 * @brief Register fan APIs
 */
esp_err_t ts_api_fan_register(void);

/**
 * @brief Register power APIs
 */
esp_err_t ts_api_power_register(void);

/**
 * @brief Register temperature APIs
 */
esp_err_t ts_api_temp_register(void);

/**
 * @brief Register service APIs
 */
esp_err_t ts_api_service_register(void);

/**
 * @brief Register storage APIs
 */
esp_err_t ts_api_storage_register(void);

/**
 * @brief Register GPIO APIs
 */
esp_err_t ts_api_gpio_register(void);

/**
 * @brief Register compute device monitor APIs
 */
esp_err_t ts_api_monitor_register(void);

/**
 * @brief Register WiFi APIs
 */
esp_err_t ts_api_wifi_register(void);

/**
 * @brief Register DHCP server APIs
 */
esp_err_t ts_api_dhcp_register(void);

/**
 * @brief Register NAT gateway APIs
 */
esp_err_t ts_api_nat_register(void);

/**
 * @brief Register LPMU upper-network access APIs
 */
esp_err_t ts_api_lpmu_access_register(void);

/**
 * @brief Register SSH known hosts APIs
 */
esp_err_t ts_api_hosts_register(void);

/**
 * @brief Register key management APIs
 */
esp_err_t ts_api_key_register(void);

/**
 * @brief Register SSH APIs
 */
esp_err_t ts_api_ssh_register(void);

/**
 * @brief Register SFTP APIs
 */
esp_err_t ts_api_sftp_register(void);

/**
 * @brief Register Time APIs
 */
esp_err_t ts_api_time_register(void);

/**
 * @brief Register OTA APIs
 */
esp_err_t ts_api_ota_register(void);

/**
 * @brief Register Log APIs
 */
void ts_api_log_register(void);

/**
 * @brief Register Automation APIs
 */
esp_err_t ts_api_automation_register(void);

/**
 * @brief Register Certificate (PKI) APIs
 */
esp_err_t ts_api_cert_register(void);

/**
 * @brief Register Authentication APIs
 */
esp_err_t ts_api_auth_register(void);

/**
 * @brief Register UI Configuration APIs
 */
esp_err_t ts_api_ui_register(void);

/**
 * @brief Register Config Pack APIs
 */
esp_err_t ts_api_config_pack_register(void);

/* ts_api_var_register 已废弃，统一使用 ts_api_automation_register */

#ifdef __cplusplus
}
#endif

#endif /* TS_API_H */
