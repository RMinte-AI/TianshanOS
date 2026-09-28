#include "platform.h"
#define HTTP_GET 0
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_SOCK_ERR_TIMEOUT -3
#define TS_MALLOC_PSRAM malloc
static bool s_stopping;
static const char *status;
static unsigned called,read_body;
typedef struct{httpd_req_t*req;const char*uri;int method;char*body;size_t body_len;}ts_http_request_t;
typedef struct{int(*handler)(ts_http_request_t*,void*);void*user_data;}ts_http_route_t;
static int httpd_resp_set_status(httpd_req_t*r,const char*s){(void)r;status=s;return 0;}
static int httpd_resp_sendstr(httpd_req_t*r,const char*s){(void)r;(void)s;return 0;}
static int httpd_resp_send_err(httpd_req_t*r,int e,const char*s){(void)r;(void)e;(void)s;return 0;}
static int httpd_req_recv(httpd_req_t*r,char*b,size_t n){(void)r;(void)b;(void)n;read_body++;return -1;}
static int operation(ts_http_request_t*r,void*u){(void)r;(void)u;called++;return 0;}
#include "reviewer_http.inc"
int main(void){
 ts_http_route_t route={.handler=operation};httpd_req_t req={.method=1,.user_ctx=&route,.content_len=20};
 s_stopping=true;assert(http_handler_wrapper(&req)==0 && !strcmp(status,"503 Service Unavailable") && !called && !read_body);
 req.method=HTTP_GET;req.content_len=0;assert(http_handler_wrapper(&req)==0 && called==1);
 s_stopping=false;req.method=1;assert(http_handler_wrapper(&req)==0 && called==2);
 puts("PASS R1 HTTP admission: stopping rejects writes before body/handler execution; reads and resumed admission remain valid");
}
