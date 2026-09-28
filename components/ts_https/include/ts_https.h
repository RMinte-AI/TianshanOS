/**
 * @file ts_https.h
 * @brief TianShanOS mTLS HTTPS Server
 * 
 * Provides a secure HTTPS server with mutual TLS (mTLS) authentication.
 * Features:
 * - Loads device certificate and private key from ts_cert component
 * - Validates client certificates against CA chain
 * - Extracts user role from certificate OU field
 * - Role-based access control for API endpoints
 * 
 * AUTHENTICATION FLOW:
 * ====================
 * 1. Client connects with TLS
 * 2. Server presents device certificate
 * 3. Client presents user certificate (mTLS)
 * 4. Server validates certificate against CA chain
 * 5. Server extracts OU field → user role (admin/operator/viewer)
 * 6. API handler checks role against endpoint permission
 * 
 * @copyright Copyright (c) 2026 TianShanOS Project
 */

#ifndef TS_HTTPS_H
#define TS_HTTPS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/*                              Constants                                     */
/*===========================================================================*/

/** Default HTTPS port */
#define TS_HTTPS_DEFAULT_PORT       443

/** Maximum URI length */
#define TS_HTTPS_MAX_URI_LEN        128

/** Maximum username from certificate */
#define TS_HTTPS_MAX_USERNAME_LEN   64

/*===========================================================================*/
/*                              Types                                         */
/*===========================================================================*/

/**
 * @brief User roles extracted from certificate OU field
 */
typedef enum {
    TS_HTTPS_ROLE_ANONYMOUS = 0,    /**< No certificate provided */
    TS_HTTPS_ROLE_VIEWER,           /**< Read-only access (OU=viewer) */
    TS_HTTPS_ROLE_OPERATOR,         /**< Operation access (OU=operator) */
    TS_HTTPS_ROLE_ADMIN,            /**< Full access (OU=admin) */
    TS_HTTPS_ROLE_DEVICE            /**< Device identity (OU=device) */
} ts_https_role_t;

/**
 * @brief Authentication result from mTLS handshake
 */
typedef struct {
    bool authenticated;                         /**< Certificate validated */
    ts_https_role_t role;                       /**< User role from OU */
    char username[TS_HTTPS_MAX_USERNAME_LEN];   /**< CN from certificate */
    char organization[64];                      /**< O from certificate */
    int cert_days_remaining;                    /**< Days until cert expires */
} ts_https_auth_t;

/**
 * @brief HTTPS request context with authentication
 */
typedef struct {
    httpd_req_t *req;                           /**< Original HTTP request */
    ts_https_auth_t auth;                       /**< Authentication info */
} ts_https_req_t;

/**
 * @brief API endpoint definition
 */
typedef struct {
    const char *uri;                            /**< URI pattern (e.g., "/api/system/info") */
    httpd_method_t method;                      /**< HTTP method */
    ts_https_role_t min_role;                   /**< Minimum role required */
    esp_err_t (*handler)(ts_https_req_t *req);  /**< Request handler */
    const char *description;                    /**< Endpoint description */
} ts_https_endpoint_t;

/**
 * @brief HTTPS server configuration
 */
typedef struct {
    uint16_t port;                              /**< Server port (default: 443) */
    uint8_t max_clients;                        /**< Max concurrent connections */
    bool require_client_cert;                   /**< Require mTLS */
    uint8_t session_cache_size;                 /**< TLS session cache size */
} ts_https_config_t;

/** Default configuration */
#define TS_HTTPS_CONFIG_DEFAULT() { \
    .port = TS_HTTPS_DEFAULT_PORT, \
    .max_clients = 3, \
    .require_client_cert = true, \
    .session_cache_size = 4 \
}

/*===========================================================================*/
/*                         Server Lifecycle                                   */
/*===========================================================================*/

/**
 * @brief Initialize HTTPS server
 * 
 * Loads certificates from ts_cert and prepares server configuration.
 * Does NOT start the server - call ts_https_start() for that.
 * 
 * @param config Server configuration (NULL for defaults)
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if ts_cert is not activated
 */
esp_err_t ts_https_init(const ts_https_config_t *config);

/**
 * @brief Start HTTPS server
 * 
 * Begins accepting connections.
 * 
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ts_https_start(void);

/**
 * @brief Stop HTTPS server
 * 
 * Closes all connections and stops accepting new ones.
 * 
 * @return ESP_OK on success
 */
esp_err_t ts_https_stop(void);

/**
 * @brief Deinitialize and free resources
 */
void ts_https_deinit(void);

/**
 * @brief Check if server is running
 * 
 * @return true if server is accepting connections
 */
bool ts_https_is_running(void);

/*===========================================================================*/
/*                         Endpoint Registration                              */
/*===========================================================================*/

/**
 * @brief Register an API endpoint
 * 
 * @param endpoint Endpoint definition
 * @return ESP_OK on success
 */
esp_err_t ts_https_register_endpoint(const ts_https_endpoint_t *endpoint);

/**
 * @brief Register multiple endpoints
 * 
 * @param endpoints Array of endpoints (NULL-terminated uri)
 * @return ESP_OK on success
 */
esp_err_t ts_https_register_endpoints(const ts_https_endpoint_t *endpoints);

/**
 * @brief Unregister an endpoint
 * 
 * @param uri URI pattern
 * @param method HTTP method
 * @return ESP_OK on success
 */
esp_err_t ts_https_unregister_endpoint(const char *uri, httpd_method_t method);

/*===========================================================================*/
/*                         Response Helpers                                   */
/*===========================================================================*/

/**
 * @brief Send JSON response
 * 
 * @param req Request context
 * @param status HTTP status code
 * @param json_str JSON string
 * @return ESP_OK on success
 */
esp_err_t ts_https_send_json(ts_https_req_t *req, int status, const char *json_str);

/**
 * @brief Send JSON response with formatted data
 * 
 * @param req Request context
 * @param status HTTP status code
 * @param fmt Format string (like printf)
 * @param ... Format arguments
 * @return ESP_OK on success
 */
esp_err_t ts_https_send_jsonf(ts_https_req_t *req, int status, const char *fmt, ...);

/**
 * @brief Send error response
 * 
 * @param req Request context
 * @param status HTTP status code
 * @param message Error message
 * @return ESP_OK on success
 */
esp_err_t ts_https_send_error(ts_https_req_t *req, int status, const char *message);

/**
 * @brief Send 403 Forbidden (insufficient permissions)
 * 
 * @param req Request context
 * @return ESP_OK (always succeeds in sending)
 */
esp_err_t ts_https_send_forbidden(ts_https_req_t *req);

/**
 * @brief Send 401 Unauthorized (no/invalid certificate)
 * 
 * @param req Request context
 * @return ESP_OK (always succeeds in sending)
 */
esp_err_t ts_https_send_unauthorized(ts_https_req_t *req);

/*===========================================================================*/
/*                         Request Helpers                                    */
/*===========================================================================*/

/**
 * @brief Get request body as string
 * 
 * Caller must free the returned buffer.
 * 
 * @param req Request context
 * @param body Output pointer to body string
 * @param len Output body length
 * @return ESP_OK on success
 */
esp_err_t ts_https_get_body(ts_https_req_t *req, char **body, size_t *len);

/**
 * @brief Get query parameter value
 * 
 * @param req Request context
 * @param key Parameter name
 * @param value Output buffer
 * @param value_len Buffer size
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not present
 */
esp_err_t ts_https_get_query_param(ts_https_req_t *req, const char *key, 
                                    char *value, size_t value_len);

/*===========================================================================*/
/*                         Utility Functions                                  */
/*===========================================================================*/

/**
 * @brief Convert role enum to string
 * 
 * @param role Role value
 * @return Role string (e.g., "admin", "operator")
 */
const char *ts_https_role_to_str(ts_https_role_t role);

/**
 * @brief Parse role from string
 * 
 * @param str Role string
 * @return Role enum
 */
ts_https_role_t ts_https_str_to_role(const char *str);

/**
 * @brief Check if role has permission
 * 
 * @param user_role User's role
 * @param required_role Required role
 * @return true if user_role >= required_role
 */
bool ts_https_role_has_permission(ts_https_role_t user_role, ts_https_role_t required_role);

/**
 * @brief Get underlying httpd handle
 * 
 * For advanced use cases.
 * 
 * @return httpd_handle_t or NULL if not running
 */
httpd_handle_t ts_https_get_handle(void);

/* Published runtime view, copied under a short critical section. No TLS calls in GET. */
typedef struct {
    bool running;
    bool require_client_cert;
    uint16_t port;
    uint32_t loaded_generation;
    char loaded_certificate_sha256[65];
    char last_error_stage[24];
    esp_err_t last_error;
} ts_https_runtime_t;
void ts_https_get_runtime(ts_https_runtime_t *out);
void ts_https_record_error(const char *stage, esp_err_t error);

#ifdef __cplusplus
}
#endif

#endif /* TS_HTTPS_H */
