/**
 * @file ts_webui.c
 * @brief WebUI Main Implementation
 */

#include "ts_webui.h"
#include "ts_http_server.h"
#include "ts_storage.h"
#include "ts_log.h"
#include "ts_security.h"
#include <string.h>

#define TAG "ts_webui"

extern esp_err_t ts_webui_api_init(void);
extern esp_err_t ts_webui_ws_init(void);

static bool s_initialized = false;
static bool s_running = false;

static esp_err_t static_file_handler(ts_http_request_t *req, void *user_data)
{
    char uri_buf[96];
    const char *query = strchr(req->uri, '?');
    size_t uri_len = query ? (size_t)(query - req->uri) : strlen(req->uri);
    if (uri_len >= sizeof(uri_buf)) {
        uri_len = sizeof(uri_buf) - 1;
    }
    memcpy(uri_buf, req->uri, uri_len);
    uri_buf[uri_len] = '\0';

    const char *uri = uri_buf;
    char filepath[128];
    
#ifdef CONFIG_TS_WEBUI_STATIC_PATH
    const char *static_path = CONFIG_TS_WEBUI_STATIC_PATH;
#else
    const char *static_path = "/www";
#endif
    
    // Default to index.html
    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }
    
    snprintf(filepath, sizeof(filepath), "%s%s", static_path, uri);
    
    // Check if file exists
    if (ts_storage_exists(filepath)) {
        return ts_http_send_file(req, filepath);
    }
    
    // Try with .html extension
    snprintf(filepath, sizeof(filepath), "%s%s.html", static_path, uri);
    if (ts_storage_exists(filepath)) {
        return ts_http_send_file(req, filepath);
    }
    
    // SPA fallback: serve index.html for all non-file routes
    snprintf(filepath, sizeof(filepath), "%s/index.html", static_path);
    if (ts_storage_exists(filepath)) {
        return ts_http_send_file(req, filepath);
    }
    
    return ts_http_send_error(req, 404, "Not Found");
}

esp_err_t ts_webui_init(void)
{
    if (s_initialized) return ESP_OK;
    
    TS_LOGI(TAG, "Initializing WebUI");
    
    // Initialize HTTP server
    esp_err_t ret = ts_http_server_init();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "HTTP server init failed");
        return ret;
    }
    
    s_initialized = true;
    TS_LOGI(TAG, "WebUI initialized");
    return ESP_OK;
}

esp_err_t ts_webui_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    
    esp_err_t ret = ts_webui_stop();
    if (ret != ESP_OK) return ret;
    ret = ts_http_server_deinit();
    if (ret != ESP_OK) return ret;
    
    s_initialized = false;
    return ESP_OK;
}

esp_err_t ts_webui_start(void)
{
    if (s_running && ts_http_server_get_handle() && !ts_http_server_is_stopping()) return ESP_OK;
    if (!s_initialized) {
        esp_err_t ret = ts_webui_init();
        if (ret != ESP_OK) return ret;
    }
    
    // Start HTTP server
    esp_err_t ret = ts_http_server_start();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }
    
    // Register API routes
    ret = ts_webui_api_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "API init failed");
    }
    
#ifdef CONFIG_TS_WEBUI_WS_ENABLE
    // Register WebSocket
    ret = ts_webui_ws_init();
    if (ret != ESP_OK) {
        TS_LOGW(TAG, "WebSocket init failed");
        esp_err_t stop_ret = ts_http_server_stop();
        return stop_ret != ESP_OK ? stop_ret : ret;
    }
#endif
    
    // Register static file handler (catch-all)
    ts_http_route_t static_route = {
        .uri = "/*",
        .method = TS_HTTP_GET,
        .handler = static_file_handler,
        .user_data = NULL,
        .requires_auth = false
    };
    ts_http_server_register_route(&static_route);
    
    s_running = true;
    TS_LOGI(TAG, "WebUI started");
    return ESP_OK;
}

esp_err_t ts_webui_stop(void)
{
    
    esp_err_t ret = ts_http_server_stop();
    if (ret != ESP_OK) return ret;
    s_running = false;
    
    TS_LOGI(TAG, "WebUI stopped");
    return ESP_OK;
}

bool ts_webui_is_running(void)
{
    return s_running && ts_http_server_get_handle() && !ts_http_server_is_stopping();
}
