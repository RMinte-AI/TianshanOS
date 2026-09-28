#ifndef TEST_HTTP_H
#define TEST_HTTP_H
#include "platform.h"
typedef void *httpd_handle_t;
typedef struct { int dummy; } httpd_req_t;
typedef int httpd_method_t;
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTP_PUT 2
#define HTTP_DELETE 3
typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    void *user_ctx;
} httpd_uri_t;
int httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t *);
#endif
