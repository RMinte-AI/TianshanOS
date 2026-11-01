#!/usr/bin/env python3
"""Exercise actual generated SDK selection + actual application admission."""
import os,sys,tempfile,subprocess,importlib.util
from pathlib import Path
root=Path(__file__).resolve().parents[2]
idf=Path(os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf'))
spec=importlib.util.spec_from_file_location('policy',root/'tools/prepare_httpd_admission.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
def function(s,name):
 a=s.index('esp_err_t '+name+'(');return s[a:s.index('\n}',a)+2]
with tempfile.TemporaryDirectory() as td:
 d=Path(td);mod.prepare(idf,d);s=(d/'httpd_sess.c').read_text();prefix=s[s.index('typedef enum {'):s.index('static void httpd_sess_close(void *arg)')]
 harness='''#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#define CONFIG_HTTPD_WS_SUPPORT 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
struct sock_db {int fd;void *ctx;bool for_async_req,lru_socket,ws_handshake_done;uint64_t lru_counter;};
struct httpd_data {struct sock_db *hd_sd;struct {int max_open_sockets;}config;};
typedef int (*httpd_session_enum_function)(struct sock_db*,void*);
static int selected=-1;
static void httpd_sess_delete(struct httpd_data *hd,struct sock_db *s){(void)hd;s->fd=-1;}
static int httpd_sess_trigger_close_(struct httpd_data *hd,struct sock_db *s){(void)hd;selected=s->fd;return ESP_OK;}
'''+prefix+function(s,'httpd_sess_close_lru')+'''
#define TS_HTTP_MAX_CLIENTS 7
#define TS_HTTP_MAX_WS_CLIENTS 5
#define HTTPD_WS_CLIENT_WEBSOCKET 2
#define HTTPD_WS_TYPE_TEXT 1
#define HTTPD_WS_TYPE_CLOSE 8
typedef struct {void *handle;}httpd_req_t;
typedef struct {int type;uint8_t *payload;size_t len;}httpd_ws_frame_t;
static int upgraded,frames,list_error;
static int httpd_get_client_list(void *h,size_t *n,int *fds){(void)h;if(list_error)return ESP_FAIL;*n=7;for(int i=0;i<7;i++)fds[i]=i;return ESP_OK;}
static int httpd_ws_get_fd_info(void *h,int fd){(void)h;return fd<upgraded?2:1;}
static int httpd_ws_send_frame(httpd_req_t *r,httpd_ws_frame_t *f){(void)r;if(!frames){assert(f->type==1);assert(strstr((char*)f->payload,"connection_busy"));}else{assert(f->type==8&&f->len==2&&f->payload[0]==3&&f->payload[1]==0xf5);}frames++;return ESP_OK;}
'''+function((root/'components/ts_net/src/ts_http_server.c').read_text(),'ts_http_server_admit_ws')+'''
int main(void){
 struct sock_db s[7];for(int i=0;i<7;i++)s[i]=(struct sock_db){.fd=i,.lru_counter=i+1};
 struct httpd_data hd={.hd_sd=s,.config={.max_open_sockets=7}};
 s[0].ws_handshake_done=true;s[2].ws_handshake_done=true;s[2].lru_counter=0;
 s[4].for_async_req=true;s[4].lru_counter=0;s[5].lru_socket=true;s[5].lru_counter=0;
 assert(httpd_sess_close_lru(&hd)==ESP_OK&&selected==1);
 assert(httpd_sess_close_lru(&hd)==ESP_OK&&selected==3);
 assert(!s[0].lru_socket&&!s[2].lru_socket);
 for(int i=0;i<7;i++){s[i].ws_handshake_done=true;s[i].lru_socket=false;}
 selected=-1;assert(httpd_sess_close_lru(&hd)==ESP_ERR_NOT_FOUND&&selected==-1);
 puts("PASS SDK: idle HTTP selected; WebSockets, asynchronous and queued-close sessions protected; exhaustion detected");
 httpd_req_t req={0};upgraded=5;assert(ts_http_server_admit_ws(&req,8)==ESP_OK&&frames==0);
 upgraded=6;assert(ts_http_server_admit_ws(&req,8)==ESP_FAIL&&frames==2);
 frames=0;upgraded=2;assert(ts_http_server_admit_ws(&req,1)==ESP_FAIL&&frames==2);
 frames=0;list_error=1;assert(ts_http_server_admit_ws(&req,8)==ESP_FAIL&&frames==0);
 puts("PASS admission: five persistent slots, busy+1013 for excess, smaller application limit, enumeration failure");
}
'''
 (d/'test.c').write_text(harness)
 subprocess.run(['cc','-std=gnu11','-g','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
 # Source drift must fail closed, without touching SDK source.
 try:mod.replace_once('changed','expected','new')
 except ValueError:pass
 else:raise AssertionError('source drift was accepted')
 print('PASS SDK drift rejected; SDK installation unchanged')
