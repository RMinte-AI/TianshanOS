/* Entire production HTTP unit, with local SDK/storage boundaries. */
#include "platform.h"
#include <sys/types.h>
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTP_PUT 2
#define HTTP_DELETE 3
#define HTTP_PATCH 4
#define HTTP_OPTIONS 5
#define API_PREFIX "/api/v1"
#define HTTPD_SOCK_ERR_TIMEOUT -2
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define ESP_LOG_ERROR 1
static unsigned body_reads, body_allocations, handled;
static int response_status;
static const char *authorization;
static void *body_malloc(size_t n) {body_allocations++;return malloc(n);}
#define TS_MALLOC_PSRAM body_malloc
static void esp_log_level_set(const char *tag,int level) {(void)tag;(void)level;}
typedef struct {const char *uri;int method;esp_err_t(*handler)(httpd_req_t*);void *user_ctx;} httpd_uri_t;
typedef struct {unsigned server_port,max_uri_handlers,recv_wait_timeout,send_wait_timeout,stack_size,task_caps;bool lru_purge_enable;void *uri_match_fn,*global_user_ctx,*global_user_ctx_free_fn,*global_transport_ctx,*global_transport_ctx_free_fn;esp_err_t(*open_fn)(httpd_handle_t,int);} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){.server_port=80})
#define MALLOC_CAP_INTERNAL 4
static void *httpd_uri_match_wildcard;
static esp_err_t httpd_start(httpd_handle_t *handle,const httpd_config_t *config) {(void)config;*handle=(void*)11;return ESP_OK;}
static esp_err_t httpd_stop(httpd_handle_t handle) {(void)handle;return ESP_OK;}
static httpd_uri_t captured[64];static unsigned route_count;
static esp_err_t httpd_register_uri_handler(httpd_handle_t handle,const httpd_uri_t *route) {(void)handle;captured[route_count++]=*route;return ESP_OK;}
static esp_err_t httpd_unregister_uri_handler(httpd_handle_t h,const char *uri,int method) {(void)h;(void)uri;(void)method;return ESP_OK;}
static int httpd_req_recv(httpd_req_t *req,char *body,size_t size) {(void)req;body_reads++;memset(body,'x',size);return (int)size;}
static esp_err_t httpd_resp_set_status(httpd_req_t *req,const char *status) {(void)req;response_status=atoi(status);return ESP_OK;}
static esp_err_t httpd_resp_set_type(httpd_req_t *req,const char *type) {(void)req;(void)type;return ESP_OK;}
static esp_err_t httpd_resp_set_hdr(httpd_req_t *req,const char *key,const char *value) {(void)req;(void)key;(void)value;return ESP_OK;}
static esp_err_t httpd_resp_send(httpd_req_t *req,const char *body,size_t size) {(void)req;(void)body;(void)size;return ESP_OK;}
static esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *body) {return httpd_resp_send(req,body,strlen(body));}
static esp_err_t httpd_resp_send_err(httpd_req_t *req,int status,const char *body) {response_status=status;return httpd_resp_sendstr(req,body);}
static esp_err_t httpd_resp_send_chunk(httpd_req_t *req,const char *body,size_t size) {return httpd_resp_send(req,body,size);}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *req,const char *key) {(void)req;return !strcmp(key,"Authorization")&&authorization?strlen(authorization):0;}
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req,const char *key,char *out,size_t size) {
 (void)req;if(strcmp(key,"Authorization")||!authorization)return ESP_ERR_NOT_FOUND;
 if(strlen(authorization)>=size)return ESP_ERR_INVALID_SIZE;snprintf(out,size,"%s",authorization);return ESP_OK;
}
static size_t httpd_req_get_url_query_len(httpd_req_t *req) {(void)req;return 0;}
static esp_err_t httpd_req_get_url_query_str(httpd_req_t *req,char *out,size_t size) {(void)req;(void)out;(void)size;return ESP_ERR_NOT_FOUND;}
static esp_err_t httpd_query_key_value(const char *q,const char *key,char *out,size_t size) {(void)q;(void)key;(void)out;(void)size;return ESP_ERR_NOT_FOUND;}
static bool ts_storage_exists(const char *path) {(void)path;return false;}
static ssize_t ts_storage_size(const char *path) {(void)path;return -1;}
static ssize_t ts_storage_read_file(const char *path,void *buffer,size_t size) {(void)path;(void)buffer;(void)size;assert(!"real storage reached");return -1;}
TaskHandle_t xTaskGetCurrentTaskHandle(void) {return (void*)1;}
#include "ts_security.h"
esp_err_t ts_auth_validate_request(const char *header,uint32_t *sid,ts_perm_level_t *level) {
 if(!strcmp(header,"Bearer admin")){*sid=8;*level=TS_PERM_ADMIN;return ESP_OK;}
 if(!strcmp(header,"Bearer root")){*sid=7;*level=TS_PERM_ROOT;return ESP_OK;}
 if(!strcmp(header,"Bearer read")){*sid=9;*level=TS_PERM_READ;return ESP_OK;}
 return ESP_ERR_NOT_FOUND;
}
#include "ts_http_server.c"
static esp_err_t upload_handler(ts_http_request_t *req,void *user_data) {
 assert(user_data==NULL);assert(req->body_len==4&&!memcmp(req->body,"xxxx",4));handled++;return ts_http_send_json(req,200,"{\"status\":\"success\"}");
}
static void request(unsigned index,const char *token,bool allowed) {
 unsigned reads=body_reads,allocations=body_allocations,handlers=handled;response_status=0;authorization=token;
 httpd_req_t req={.handle=(void*)11,.method=HTTP_POST,.uri=captured[index].uri,.user_ctx=captured[index].user_ctx,.content_len=4};
 assert(captured[index].handler(&req)==ESP_OK);
 assert(response_status==(allowed?200:401));assert(handled==handlers+allowed);
 assert(body_reads==reads+allowed&&body_allocations==allocations+allowed);
}
#define login_handler upload_handler
#define logout_handler upload_handler
#define file_download_handler upload_handler
#define file_upload_handler upload_handler
#define ota_firmware_upload_handler upload_handler
#define ota_www_upload_handler upload_handler
#define api_handler upload_handler
#define options_handler upload_handler
#include "webui_route_registration.inc"
int main(void) {
 assert(ts_http_server_start()==ESP_OK);assert(ts_webui_api_init()==ESP_OK);
 unsigned uploads=0,public_generic=0;
 for(unsigned i=0;i<route_count;i++) {
  const char *uri=captured[i].uri;
  if(!strcmp(uri,"/api/v1/file/upload")||!strcmp(uri,"/api/v1/ota/firmware")||!strcmp(uri,"/api/v1/ota/www")) {
   uploads++;assert(s_registered_routes[i].requires_auth);
#ifdef CONFIG_TS_WEBUI_AUTH_REQUIRED
   request(i,NULL,false);request(i,"Bearer expired",false);
#else
   request(i,NULL,true);request(i,"Bearer expired",true);
#endif
   request(i,"Bearer read",true);request(i,"Bearer admin",true);request(i,"Bearer root",true);
  }
  if(!strcmp(uri,"/api/v1/*") && captured[i].method!=HTTP_OPTIONS) {
   public_generic++;assert(!s_registered_routes[i].requires_auth);request(i,NULL,true);
  }
 }
 assert(uploads==3&&public_generic==4);assert(ts_http_server_stop()==ESP_OK);
 puts("PASS actual HTTP routes/wrapper: upload rejects before body/read/allocation; READ/Admin/Root and public generic routes unchanged (also auth-disabled build)");
}
