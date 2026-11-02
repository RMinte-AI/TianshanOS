"""Production file sender and HTTP wrapper, real stdio incl. injected read EIO."""
from test_console_lifecycle import body, run_native

code = r'''
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_err.h"
#define ESP_LOGW(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define TS_MALLOC_PSRAM malloc
#define HTTP_GET 0
#define HTTPD_SOCK_ERR_TIMEOUT -7
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define TAG "fixture"
typedef struct {void *user_ctx;const char *uri;int method;size_t content_len;} httpd_req_t;
typedef struct {httpd_req_t *req;const char *uri;int method;char *body;size_t body_len;} ts_http_request_t;
typedef struct {int (*handler)(ts_http_request_t*,void*);void *user_data;} ts_http_route_t;
static bool s_stopping,accept_gzip;
static size_t fault_after=SIZE_MAX,position,received;
static int opened,closed,ends,chunks,fail_chunk,fail_end,json_errors;
static char payload[100000],wire[100000];
static bool is_compressible(const char *ext){return ext&&(!strcmp(ext,".js")||!strcmp(ext,".html")||!strcmp(ext,".css"));}
static bool client_accepts_gzip(httpd_req_t *req){(void)req;return accept_gzip;}
static bool ts_storage_exists(const char *p){struct stat st;return !stat(p,&st);}
static ssize_t ts_storage_size(const char *p){struct stat st;return stat(p,&st)?-1:st.st_size;}
static ssize_t ts_storage_read_file(const char *p,void *b,size_t n){FILE *f=fopen(p,"rb");assert(f);size_t r=fread(b,1,n,f);assert(!fclose(f));return r;}
static int reader(void *cookie,char *buf,int n){
 FILE *f=cookie;if(position>=fault_after){errno=EIO;return -1;}
 if((size_t)n>fault_after-position)n=(int)(fault_after-position);
 int got=(int)fread(buf,1,(size_t)n,f);position+=got;return got;
}
static int closer(void *cookie){++closed;return fclose(cookie);}
#ifndef __APPLE__
static ssize_t cookie_reader(void *p,char *b,size_t n){return reader(p,b,(int)n);}
#endif
static FILE *open_stream(const char *p,const char *m){
 FILE *f=fopen(p,m);if(!f)return NULL;++opened;position=0;
#ifdef __APPLE__
 return funopen(f,reader,NULL,NULL,closer);
#else
 cookie_io_functions_t io={.read=cookie_reader,.close=closer};return fopencookie(f,"r",io);
#endif
}
static int httpd_resp_set_type(httpd_req_t *r,const char *s){(void)r;(void)s;return ESP_OK;}
static char encoding[20],cache[100];
static int httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *s){(void)r;if(!strcmp(k,"Content-Encoding"))strcpy(encoding,s);if(!strcmp(k,"Cache-Control"))strcpy(cache,s);return ESP_OK;}
static int httpd_resp_send(httpd_req_t *r,const char *p,size_t n){(void)r;memcpy(wire+received,p,n);received+=n;return ESP_OK;}
static int httpd_resp_send_chunk(httpd_req_t *r,const char *p,size_t n){
 if(!n){++ends;return fail_end?ESP_FAIL:ESP_OK;}
 if(++chunks==fail_chunk)return ESP_FAIL;return httpd_resp_send(r,p,n);
}
static int ts_http_send_error(ts_http_request_t *r,int status,const char *s){(void)r;(void)status;(void)s;++json_errors;return ESP_FAIL;}
static int httpd_resp_set_status(httpd_req_t *r,const char *s){(void)r;(void)s;return ESP_OK;}
static int httpd_resp_sendstr(httpd_req_t *r,const char *s){(void)r;(void)s;return ESP_OK;}
static int httpd_resp_send_err(httpd_req_t *r,int c,const char *s){(void)r;(void)c;(void)s;return ESP_OK;}
static int httpd_req_recv(httpd_req_t *r,void *p,size_t n){(void)r;(void)p;(void)n;assert(false);return -1;}
#define fopen open_stream
'''
code += body('components/ts_net/src/ts_http_server.c', 'ts_http_send_file')
code += '\n#undef fopen\n'
code += body('components/ts_net/src/ts_http_server.c', 'http_handler_wrapper')
code += r'''
static int route_file(ts_http_request_t *r,void *p){return ts_http_send_file(r,p);}
static void reset(void){opened=closed=ends=chunks=fail_chunk=fail_end=json_errors=0;received=0;encoding[0]=cache[0]=0;fault_after=SIZE_MAX;}
static void make(const char *p,size_t n){FILE *f=fopen(p,"wb");assert(f);assert(fwrite(payload,1,n,f)==n);assert(!fclose(f));}
static int request(const char *p){ts_http_route_t route={route_file,(void*)p};httpd_req_t req={.user_ctx=&route,.uri=p,.method=HTTP_GET};return http_handler_wrapper(&req);}
int main(void){
 for(size_t i=0;i<sizeof(payload);i++)payload[i]=(char)(i%251);
 size_t sizes[]={12,32767,32768,32769,65536,73791};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++){
  reset();make("file+v1.bin",sizes[i]);assert(request("file+v1.bin")==ESP_OK);
  assert(received==sizes[i]&&!memcmp(wire,payload,received)&&!json_errors);
  assert(ends==(sizes[i]>=32768));assert(opened==closed);
 }
 make("file+v1.bin",65536);
 size_t boundaries[]={0,1,8192,17000,65535};
 for(unsigned i=0;i<sizeof(boundaries)/sizeof(*boundaries);i++){
  reset();fault_after=boundaries[i];assert(request("file+v1.bin")==ESP_FAIL);
  assert(!ends&&!json_errors&&opened==1&&closed==1&&received<=boundaries[i]);
 }
 reset();fail_chunk=2;assert(request("file+v1.bin")==ESP_FAIL&&!ends&&!json_errors&&opened==closed);
 reset();fail_end=1;assert(request("file+v1.bin")==ESP_FAIL&&ends==1&&!json_errors&&opened==closed);
 const char *assets[]={"./index.html","./app.js","./style.css"};
 for(unsigned i=0;i<3;i++){
  reset();make(assets[i],40000);assert(request(assets[i])==ESP_OK&&received==40000&&!memcmp(wire,payload,received));
  assert(strstr(cache,i?"max-age":"no-cache"));
  char gz[80];snprintf(gz,sizeof(gz),"%s.gz",assets[i]);make(gz,33333);
  reset();accept_gzip=true;assert(request(assets[i])==ESP_OK&&!strcmp(encoding,"gzip")&&received==33333&&!memcmp(wire,payload,received));accept_gzip=false;
 }
 puts("PASS F07 actual sender + HTTP wrapper: threshold/whole/partial chunks, real stdio EIO, send/terminator failures, closed streams, index/JS/CSS/gzip/plus-name content");
}
'''
run_native(code)
