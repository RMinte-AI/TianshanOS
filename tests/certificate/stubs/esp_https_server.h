#include "esp_http_server.h"
typedef struct {
    struct {int server_port,max_open_sockets,recv_wait_timeout,send_wait_timeout,task_caps,stack_size;bool lru_purge_enable,enable_so_linger;} httpd;
    uint16_t port_secure;
    const uint8_t *servercert,*prvtkey_pem,*cacert_pem;
    size_t servercert_len,prvtkey_len,cacert_len;
} httpd_ssl_config_t;
#define HTTPD_SSL_CONFIG_DEFAULT() {0}
int httpd_ssl_start(httpd_handle_t *,httpd_ssl_config_t *);
int httpd_ssl_stop(httpd_handle_t);
