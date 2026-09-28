/**
 * @file ts_http_server.c
 * @brief HTTP Server Implementation
 * 
 * 请求体和响应缓冲区优先分配到 PSRAM
 */

#include "ts_http_server.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM */
#include "ts_log.h"
#include "ts_storage.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdatomic.h>
#include "freertos/task.h"

#define TAG "ts_http"
#define MAX_ROUTES 64

static httpd_handle_t s_server = NULL;
static bool s_initialized = false;
static _Atomic(TaskHandle_t) s_http_task;
static atomic_flag s_stop_busy = ATOMIC_FLAG_INIT;
static atomic_bool s_stopping;
bool ts_http_server_is_stopping(void) { return s_stopping; }
static esp_err_t session_open(httpd_handle_t server, int fd)
{
    (void)server; (void)fd;
    s_http_task = xTaskGetCurrentTaskHandle();
    return ESP_OK;
}
static esp_err_t (*s_before_stop)(httpd_handle_t);
static void (*s_after_stop)(httpd_handle_t);
void ts_http_server_set_stop_hooks(esp_err_t (*before)(httpd_handle_t), void (*after)(httpd_handle_t))
{
    s_before_stop = before; s_after_stop = after;
}

// 路由注册表（用于同步到 HTTPS）
typedef struct {
    const char *uri_ptr;     // 原始 URI 指针（来自 .rodata 字符串常量）
    ts_http_method_t method;
    ts_http_handler_t handler;
    void *user_data;         // 原始 user_data
    bool requires_auth;      // 认证要求
    ts_http_route_t *owned_copy; /* HTTPD borrows user_ctx; it does not free it. */
} registered_route_t;

static registered_route_t s_registered_routes[MAX_ROUTES];
static int s_route_count = 0;

esp_err_t ts_http_server_init(void)
{
    if (s_initialized) return ESP_OK;
    
    /* 降低 httpd 内部日志级别，避免连接重置警告刷屏 */
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    
    s_initialized = true;
    TS_LOGI(TAG, "HTTP server initialized");
    return ESP_OK;
}

esp_err_t ts_http_server_deinit(void)
{
    esp_err_t ret = ts_http_server_stop();
    if (ret != ESP_OK) return ret;
    s_initialized = false;
    return ESP_OK;
}

esp_err_t ts_http_server_start(void)
{
    if (s_stopping) return ESP_ERR_INVALID_STATE;
    if (s_server) return ESP_OK;
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
#ifdef CONFIG_TS_NET_HTTP_PORT
    config.server_port = CONFIG_TS_NET_HTTP_PORT;
#endif
    config.open_fn = session_open;
    config.max_uri_handlers = MAX_ROUTES;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.global_user_ctx = NULL;
    config.global_user_ctx_free_fn = NULL;
    config.global_transport_ctx = NULL;
    config.global_transport_ctx_free_fn = NULL;
    config.lru_purge_enable = true;  // 启用 LRU 清理，处理连接重置
    config.recv_wait_timeout = 5;    // 接收超时 5 秒
    config.send_wait_timeout = 5;    // 发送超时 5 秒
    config.stack_size = 8192;        // 栈大小（处理大型 JSON 响应需要更多空间）
    // 注意：任务栈必须在内部 RAM，因为 flash 操作时 PSRAM 不可用
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    }
    return ret;
}

esp_err_t ts_http_server_stop(void)
{
    if (s_http_task && s_http_task == xTaskGetCurrentTaskHandle()) return ESP_ERR_INVALID_STATE;
    if (atomic_flag_test_and_set(&s_stop_busy)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    if (!s_server) goto done;
    s_stopping = true;
    ret = s_before_stop ? s_before_stop(s_server) : ESP_OK;
    if (ret != ESP_OK) goto done;
    httpd_handle_t stopped = s_server;
    ret = httpd_stop(stopped);
    if (ret != ESP_OK) goto done;
    if (s_after_stop) s_after_stop(stopped);
    s_server = NULL;
    s_http_task = NULL;
    s_stopping = false;
    for (int i = 0; i < s_route_count; ++i) free(s_registered_routes[i].owned_copy);
    memset(s_registered_routes, 0, sizeof(s_registered_routes));
    s_route_count = 0;
    TS_LOGI(TAG, "HTTP server stopped");
done:
    atomic_flag_clear(&s_stop_busy);
    return ret;
}

static esp_err_t http_handler_wrapper(httpd_req_t *req)
{
    if(s_stopping && req->method!=HTTP_GET) {
        httpd_resp_set_status(req,"503 Service Unavailable");
        return httpd_resp_sendstr(req,"Service is stopping");
    }
    ts_http_route_t *route = (ts_http_route_t *)req->user_ctx;
    if (!route || !route->handler) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No handler");
        return ESP_FAIL;
    }
    
    ts_http_request_t ts_req = {
        .req = req,
        .uri = req->uri,
        .method = req->method,
        .body = NULL,
        .body_len = 0
    };
    
    // Read body if present - must loop to receive all data
    if (req->content_len > 0) {
        ts_req.body = TS_MALLOC_PSRAM(req->content_len + 1);
        if (ts_req.body) {
            size_t total_received = 0;
            while (total_received < req->content_len) {
                int ret = httpd_req_recv(req, ts_req.body + total_received, 
                                         req->content_len - total_received);
                if (ret <= 0) {
                    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                        // Timeout, continue trying
                        continue;
                    }
                    // Error or connection closed
                    TS_LOGW(TAG, "Body recv error at %zu/%zu bytes: %d", 
                            total_received, (size_t)req->content_len, ret);
                    break;
                }
                total_received += ret;
            }
            ts_req.body[total_received] = '\0';
            ts_req.body_len = total_received;
            
            if (total_received != req->content_len) {
                TS_LOGW(TAG, "Incomplete body: got %zu of %zu bytes", 
                        total_received, (size_t)req->content_len);
            }
        } else {
            TS_LOGE(TAG, "Failed to allocate %zu bytes for body", (size_t)req->content_len);
        }
    }
    
    esp_err_t result = route->handler(&ts_req, route->user_data);
    
    free(ts_req.body);
    return result;
}

esp_err_t ts_http_server_register_route(const ts_http_route_t *route)
{
    if (!s_server || !route || s_stopping) return ESP_ERR_INVALID_STATE;
    if (s_route_count >= MAX_ROUTES) return ESP_ERR_NO_MEM;
    
    // Allocate persistent copy of route (prefer PSRAM)
    ts_http_route_t *route_copy = TS_MALLOC_PSRAM(sizeof(ts_http_route_t));
    if (!route_copy) return ESP_ERR_NO_MEM;
    *route_copy = *route;
    
    httpd_uri_t uri = {
        .uri = route->uri,
        .method = route->method,
        .handler = http_handler_wrapper,
        .user_ctx = route_copy
    };
    
    // 注册到 HTTP 服务器
    esp_err_t ret = httpd_register_uri_handler(s_server, &uri);
    if (ret != ESP_OK) {
        free(route_copy);
        return ret;
    }
    
    // 保存路由信息到注册表
    if (s_route_count < MAX_ROUTES) {
        // 注意：不再复制 URI 字符串，直接保存原始指针（来自 .rodata）
        s_registered_routes[s_route_count].uri_ptr = route->uri;  // 保存原始 URI 指针
        s_registered_routes[s_route_count].method = route->method;
        s_registered_routes[s_route_count].handler = route->handler;
        s_registered_routes[s_route_count].user_data = route->user_data;  // 保存 user_data
        s_registered_routes[s_route_count].owned_copy = route_copy;
        s_registered_routes[s_route_count].requires_auth = route->requires_auth;  // 保存认证要求
        s_route_count++;
    }
    
    return ESP_OK;
}

esp_err_t ts_http_server_sync_routes_to_https(void)
{
    // HTTPS 功能未实现
    TS_LOGW(TAG, "HTTPS server not available");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ts_http_server_unregister_route(const char *uri, ts_http_method_t method)
{
    if (!s_server || !uri) return ESP_ERR_INVALID_STATE;
    return httpd_unregister_uri_handler(s_server, uri, method);
}

esp_err_t ts_http_send_response(ts_http_request_t *req, int status,
                                 const char *content_type, const char *body)
{
    if (!req || !req->req) return ESP_ERR_INVALID_ARG;
    
    httpd_resp_set_status(req->req, status == 200 ? "200 OK" :
                                    status == 201 ? "201 Created" :
                                    status == 400 ? "400 Bad Request" :
                                    status == 401 ? "401 Unauthorized" :
                                    status == 403 ? "403 Forbidden" :
                                    status == 404 ? "404 Not Found" :
                                    status == 500 ? "500 Internal Server Error" : "200 OK");
    
    if (content_type) {
        httpd_resp_set_type(req->req, content_type);
    }
    
    return httpd_resp_send(req->req, body, body ? strlen(body) : 0);
}

esp_err_t ts_http_send_json(ts_http_request_t *req, int status, const char *json)
{
    return ts_http_send_response(req, status, "application/json", json);
}

/**
 * @brief 检查客户端是否支持 gzip 编码
 */
static bool client_accepts_gzip(httpd_req_t *req)
{
    char buf[128];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", buf, sizeof(buf)) == ESP_OK) {
        return (strstr(buf, "gzip") != NULL);
    }
    return false;
}

/**
 * @brief 判断文件是否为可 gzip 压缩的文本资源
 */
static bool is_compressible(const char *ext)
{
    if (!ext) return false;
    return (strcmp(ext, ".js") == 0 || strcmp(ext, ".css") == 0 ||
            strcmp(ext, ".html") == 0 || strcmp(ext, ".json") == 0);
}

esp_err_t ts_http_send_file(ts_http_request_t *req, const char *filepath)
{
    if (!req || !filepath) return ESP_ERR_INVALID_ARG;
    
    /* ===== Content-Type 检测 ===== */
    const char *ext = strrchr(filepath, '.');
    const char *content_type = "application/octet-stream";
    if (ext) {
        if (strcmp(ext, ".html") == 0) content_type = "text/html";
        else if (strcmp(ext, ".css") == 0) content_type = "text/css";
        else if (strcmp(ext, ".js") == 0) content_type = "application/javascript";
        else if (strcmp(ext, ".json") == 0) content_type = "application/json";
        else if (strcmp(ext, ".png") == 0) content_type = "image/png";
        else if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) content_type = "image/jpeg";
        else if (strcmp(ext, ".svg") == 0) content_type = "image/svg+xml";
        else if (strcmp(ext, ".ico") == 0) content_type = "image/x-icon";
        else if (strcmp(ext, ".woff2") == 0) content_type = "font/woff2";
        else if (strcmp(ext, ".woff") == 0) content_type = "font/woff";
    }
    
    /* ===== 尝试发送 gzip 预压缩版本 ===== */
    const char *actual_filepath = filepath;
    char gz_path[140];
    bool use_gzip = false;
    
    if (is_compressible(ext) && client_accepts_gzip(req->req)) {
        snprintf(gz_path, sizeof(gz_path), "%s.gz", filepath);
        if (ts_storage_exists(gz_path)) {
            actual_filepath = gz_path;
            use_gzip = true;
        }
    }
    
    ssize_t size = ts_storage_size(actual_filepath);
    if (size < 0) {
        /* .gz 不存在时回退到原始文件 */
        actual_filepath = filepath;
        use_gzip = false;
        size = ts_storage_size(filepath);
        if (size < 0) {
            return ts_http_send_error(req, 404, "File not found");
        }
    }
    
    /* ===== 设置响应头 ===== */
    httpd_resp_set_type(req->req, content_type);
    
    if (use_gzip) {
        httpd_resp_set_hdr(req->req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req->req, "Vary", "Accept-Encoding");
    }
    
    /* index.html 不缓存（确保 OTA 后拿到新版本），其他静态资源长期缓存。
     * JS/CSS/字体等通过 index.html 中的 ?v= 版本参数实现缓存失效。
     */
    const char *basename = strrchr(filepath, '/');
    if (basename && strcmp(basename, "/index.html") == 0) {
        httpd_resp_set_hdr(req->req, "Cache-Control", "no-cache, no-store, must-revalidate");
    } else if (ext) {
        httpd_resp_set_hdr(req->req, "Cache-Control", "public, max-age=604800, immutable");
    }
    
    /* ===== 发送文件内容 ===== */
    
    /* 对于小文件（<32KB），直接发送 */
    if (size < 32 * 1024) {
        char *buf = TS_MALLOC_PSRAM(size);
        if (!buf) {
            return ts_http_send_error(req, 500, "Memory allocation failed");
        }
        
        if (ts_storage_read_file(actual_filepath, buf, size) != size) {
            free(buf);
            return ts_http_send_error(req, 500, "Failed to read file");
        }
        
        esp_err_t ret = httpd_resp_send(req->req, buf, size);
        free(buf);
        return ret;
    }
    
    /* 对于大文件，使用分块传输（chunked transfer） */
    FILE *f = fopen(actual_filepath, "r");
    if (!f) {
        return ts_http_send_error(req, 500, "Failed to open file");
    }
    
    #define CHUNK_SIZE 8192
    char *chunk = TS_MALLOC_PSRAM(CHUNK_SIZE);
    if (!chunk) {
        fclose(f);
        return ts_http_send_error(req, 500, "Memory allocation failed");
    }
    
    esp_err_t ret = ESP_OK;
    size_t read_len;
    
    do {
        read_len = fread(chunk, 1, CHUNK_SIZE, f);
        if (read_len > 0) {
            ret = httpd_resp_send_chunk(req->req, chunk, read_len);
            if (ret != ESP_OK) {
                ESP_LOGW("http", "Chunk send failed");
                break;
            }
        }
    } while (read_len == CHUNK_SIZE && ret == ESP_OK);
    
    /* 发送结束标记（空块） */
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req->req, NULL, 0);
    }
    
    free(chunk);
    fclose(f);
    return ret;
}

esp_err_t ts_http_send_error(ts_http_request_t *req, int status, const char *message)
{
    char json[128];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", message ? message : "Unknown error");
    return ts_http_send_json(req, status, json);
}

/**
 * @brief URL decode helper - decode %XX sequences in place
 */
static void url_decode_inplace(char *str)
{
    char *src = str;
    char *dst = str;
    
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            // Decode %XX
            int high = src[1];
            int low = src[2];
            
            // Convert hex to int
            if (high >= '0' && high <= '9') high -= '0';
            else if (high >= 'A' && high <= 'F') high = high - 'A' + 10;
            else if (high >= 'a' && high <= 'f') high = high - 'a' + 10;
            else { *dst++ = *src++; continue; }
            
            if (low >= '0' && low <= '9') low -= '0';
            else if (low >= 'A' && low <= 'F') low = low - 'A' + 10;
            else if (low >= 'a' && low <= 'f') low = low - 'a' + 10;
            else { *dst++ = *src++; continue; }
            
            *dst++ = (char)((high << 4) | low);
            src += 3;
        } else if (*src == '+') {
            // '+' decodes to space
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

esp_err_t ts_http_get_query_param(ts_http_request_t *req, const char *key,
                                   char *value, size_t max_len)
{
    if (!req || !req->req || !key || !value) return ESP_ERR_INVALID_ARG;
    
    size_t buf_len = httpd_req_get_url_query_len(req->req) + 1;
    if (buf_len <= 1) return ESP_ERR_NOT_FOUND;
    
    char *buf = TS_MALLOC_PSRAM(buf_len);
    if (!buf) return ESP_ERR_NO_MEM;
    
    esp_err_t ret = httpd_req_get_url_query_str(req->req, buf, buf_len);
    if (ret == ESP_OK) {
        ret = httpd_query_key_value(buf, key, value, max_len);
        if (ret == ESP_OK) {
            // URL decode the value
            url_decode_inplace(value);
        }
    }
    
    free(buf);
    return ret;
}

esp_err_t ts_http_get_header(ts_http_request_t *req, const char *key,
                              char *value, size_t max_len)
{
    if (!req || !req->req || !key || !value) return ESP_ERR_INVALID_ARG;
    
    size_t len = httpd_req_get_hdr_value_len(req->req, key);
    if (len == 0) return ESP_ERR_NOT_FOUND;
    
    return httpd_req_get_hdr_value_str(req->req, key, value, max_len);
}

esp_err_t ts_http_set_cors(ts_http_request_t *req, const char *origin)
{
    if (!req || !req->req) return ESP_ERR_INVALID_ARG;
    
    httpd_resp_set_hdr(req->req, "Access-Control-Allow-Origin", origin ? origin : "*");
    httpd_resp_set_hdr(req->req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req->req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    
    return ESP_OK;
}

httpd_handle_t ts_http_server_get_handle(void)
{
    return s_server;
}
