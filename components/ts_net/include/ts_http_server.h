/**
 * @file ts_http_server.h
 * @brief HTTP/HTTPS Server
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HTTP method */
typedef enum {
    TS_HTTP_GET = HTTP_GET,
    TS_HTTP_POST = HTTP_POST,
    TS_HTTP_PUT = HTTP_PUT,
    TS_HTTP_DELETE = HTTP_DELETE,
    TS_HTTP_PATCH = HTTP_PATCH
} ts_http_method_t;

/** HTTP request context */
typedef struct {
    httpd_req_t *req;
    const char *uri;
    ts_http_method_t method;
    char *body;
    size_t body_len;
} ts_http_request_t;

/** HTTP handler callback */
typedef esp_err_t (*ts_http_handler_t)(ts_http_request_t *req, void *user_data);

/** Route registration */
typedef struct {
    const char *uri;
    ts_http_method_t method;
    ts_http_handler_t handler;
    void *user_data;
    bool requires_auth;
} ts_http_route_t;

/**
 * @brief Initialize HTTP server
 */
esp_err_t ts_http_server_init(void);

/**
 * @brief Deinitialize HTTP server
 */
esp_err_t ts_http_server_deinit(void);

/**
 * @brief Start HTTP server
 */
esp_err_t ts_http_server_start(void);

/**
 * @brief Stop HTTP server
 */
esp_err_t ts_http_server_stop(void);

/**
 * @brief Register route
 */
esp_err_t ts_http_server_register_route(const ts_http_route_t *route);

/**
 * @brief Unregister route
 */
esp_err_t ts_http_server_unregister_route(const char *uri, ts_http_method_t method);

/**
 * @brief Sync all registered routes to HTTPS server
 * @note Call this function after HTTPS server starts to register all routes
 */
esp_err_t ts_http_server_sync_routes_to_https(void);

/**
 * @brief Send response
 */
esp_err_t ts_http_send_response(ts_http_request_t *req, int status, 
                                 const char *content_type, const char *body);

/**
 * @brief Send JSON response
 */
esp_err_t ts_http_send_json(ts_http_request_t *req, int status, const char *json);

/**
 * @brief Send file response
 */
esp_err_t ts_http_send_file(ts_http_request_t *req, const char *filepath);

/**
 * @brief Send error response
 */
esp_err_t ts_http_send_error(ts_http_request_t *req, int status, const char *message);

/**
 * @brief Get query parameter
 */
esp_err_t ts_http_get_query_param(ts_http_request_t *req, const char *key, 
                                   char *value, size_t max_len);

/**
 * @brief Get header value
 */
esp_err_t ts_http_get_header(ts_http_request_t *req, const char *key,
                              char *value, size_t max_len);

/**
 * @brief Set CORS headers
 */
esp_err_t ts_http_set_cors(ts_http_request_t *req, const char *origin);

/**
 * @brief Get the underlying httpd handle for advanced operations
 * @return httpd_handle_t or NULL if not started
 */
httpd_handle_t ts_http_server_get_handle(void);

/* Owner hooks: before_stop must drain users; failure preserves the live handle. */
void ts_http_server_set_stop_hooks(esp_err_t (*before)(httpd_handle_t),
                                   void (*after)(httpd_handle_t));

bool ts_http_server_is_stopping(void);

#ifdef __cplusplus
}
#endif
