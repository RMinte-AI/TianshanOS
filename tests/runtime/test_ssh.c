#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
#include <libssh2.h>
#include "ts_known_hosts.h"
struct _LIBSSH2_SESSION {int dummy;};
struct _LIBSSH2_CHANNEL {int dummy;};
static int phase, trust, auth_calls, exec_calls, sessions, channels, sockets, waits;
static int read_index, cancel_after;
static int64_t now;
int64_t esp_timer_get_time(void){now+=100;return now;}
static bool cancelled(void *unused){return cancel_after && waits>=cancel_after;}
static int fake_socket(int d,int t,int p){++sockets;return 10;}
static int fake_close(int fd){--sockets;return 0;}
static int fake_connect(int fd,const struct sockaddr *a,socklen_t n){return 0;}
static int fake_fcntl(int fd,int op,...){return 0;}
static int fake_select(int n,fd_set*r,fd_set*w,fd_set*x,struct timeval*t){assert(t->tv_sec==0&&t->tv_usec<=100000);now+=t->tv_usec;++waits;return 1;}
int libssh2_init(int flags){return 0;}
void libssh2_exit(void){}
LIBSSH2_SESSION *libssh2_session_init_ex(LIBSSH2_ALLOC_FUNC((*a)),LIBSSH2_FREE_FUNC((*f)),LIBSSH2_REALLOC_FUNC((*r)),void *abstract){++sessions;return calloc(1,sizeof(LIBSSH2_SESSION));}
int libssh2_session_free(LIBSSH2_SESSION*s){--sessions;free(s);return 0;}
void libssh2_session_set_blocking(LIBSSH2_SESSION*s,int blocking){assert(!blocking);}
int libssh2_session_handshake(LIBSSH2_SESSION*s,libssh2_socket_t fd){return phase==1?LIBSSH2_ERROR_EAGAIN:0;}
int libssh2_session_block_directions(LIBSSH2_SESSION*s){return LIBSSH2_SESSION_BLOCK_INBOUND;}
void *libssh2_session_callback_set(LIBSSH2_SESSION*s,int cb,void*f){return NULL;}
int libssh2_session_last_errno(LIBSSH2_SESSION*s){return LIBSSH2_ERROR_EAGAIN;}
int libssh2_userauth_password_ex(LIBSSH2_SESSION*s,const char*u,unsigned int ul,const char*p,unsigned int pl,LIBSSH2_PASSWD_CHANGEREQ_FUNC((*change))){++auth_calls;assert(trust==0||trust==3);return phase==2?LIBSSH2_ERROR_EAGAIN:0;}
int libssh2_userauth_publickey_fromfile_ex(LIBSSH2_SESSION*s,const char*u,unsigned int ul,const char*pub,const char*priv,const char*pass){return libssh2_userauth_password_ex(s,u,ul,pass,0,NULL);}
int libssh2_userauth_publickey_frommemory(LIBSSH2_SESSION*s,const char*u,size_t ul,const char*pub,size_t publ,const char*priv,size_t privl,const char*pass){return libssh2_userauth_password_ex(s,u,ul,pass,0,NULL);}
LIBSSH2_CHANNEL *libssh2_channel_open_ex(LIBSSH2_SESSION*s,const char*t,unsigned int tl,unsigned int win,unsigned int pkt,const char*m,unsigned int ml){if(phase==3)return NULL;++channels;return calloc(1,sizeof(LIBSSH2_CHANNEL));}
int libssh2_channel_process_startup(LIBSSH2_CHANNEL*c,const char*r,unsigned int rl,const char*m,unsigned int ml){++exec_calls;return phase==4?LIBSSH2_ERROR_EAGAIN:0;}
ssize_t libssh2_channel_read_ex(LIBSSH2_CHANNEL*c,int stream,char*b,size_t size){if(phase==5)return LIBSSH2_ERROR_EAGAIN;if(stream&&!read_index++){memcpy(b,"err",3);return 3;}return 0;}
int libssh2_channel_eof(LIBSSH2_CHANNEL*c){return phase!=5;}
int libssh2_channel_close(LIBSSH2_CHANNEL*c){return phase==6?LIBSSH2_ERROR_EAGAIN:0;}
int libssh2_channel_free(LIBSSH2_CHANNEL*c){--channels;free(c);return 0;}
int libssh2_channel_get_exit_status(LIBSSH2_CHANNEL*c){return 0;}
esp_err_t ts_known_hosts_verify(ts_ssh_session_t s,ts_host_verify_result_t *out,ts_known_host_t *host){*out=trust==1?TS_HOST_VERIFY_NOT_FOUND:trust==2?TS_HOST_VERIFY_MISMATCH:TS_HOST_VERIFY_OK;return ESP_OK;}
/* Model libssh2 session ownership of all channels during teardown. */
static LIBSSH2_CHANNEL *owned_channel;
static LIBSSH2_CHANNEL *tracked_open(LIBSSH2_SESSION*s,const char*t,unsigned int tl,unsigned int w,unsigned int p,const char*m,unsigned int ml){owned_channel=libssh2_channel_open_ex(s,t,tl,w,p,m,ml);return owned_channel;}
static int tracked_free_channel(LIBSSH2_CHANNEL*c){owned_channel=NULL;return libssh2_channel_free(c);}
static int tracked_free_session(LIBSSH2_SESSION*s){if(owned_channel){libssh2_channel_free(owned_channel);owned_channel=NULL;}return libssh2_session_free(s);}
#define libssh2_channel_open_ex tracked_open
#define libssh2_channel_free tracked_free_channel
#define libssh2_session_free tracked_free_session
#define socket fake_socket
#define close fake_close
#define connect fake_connect
#define fcntl fake_fcntl
#define select fake_select
#include <stdarg.h>
static char captured_logs[32768];
static void capture_log(const char *tag,const char *fmt,...){size_t n=strlen(captured_logs);va_list a;va_start(a,fmt);vsnprintf(captured_logs+n,sizeof(captured_logs)-n,fmt,a);va_end(a);}
#undef ESP_LOGE
#undef ESP_LOGW
#undef ESP_LOGI
#undef ESP_LOGD
#undef ESP_LOGV
#define ESP_LOGE capture_log
#define ESP_LOGW capture_log
#define ESP_LOGI capture_log
#define ESP_LOGD capture_log
#define ESP_LOGV capture_log
#include "../../components/ts_security/src/ts_ssh_client.c"
static esp_err_t approved(ts_ssh_session_t s,void*x){trust=3;return ESP_OK;}
static void reset(void){assert(!sockets&&!sessions&&!channels);phase=trust=auth_calls=exec_calls=waits=read_index=cancel_after=0;now=0;}
static ts_ssh_session_t create(void){ts_ssh_config_t cfg=TS_SSH_DEFAULT_CONFIG();cfg.host="192.0.2.1";cfg.username="synthetic";cfg.auth.password="synthetic-secret";cfg.timeout_ms=250;cfg.cancelled=cancelled;ts_ssh_session_t s;assert(ts_ssh_session_create(&cfg,&s)==ESP_OK);return s;}
int main(void){
 for(int t=1;t<=2;t++){reset();trust=t;ts_ssh_session_t s=create();assert(ts_ssh_connect(s)==(t==1?TS_SSH_ERR_HOST_UNKNOWN:TS_SSH_ERR_HOST_CHANGED));assert(!auth_calls&&!exec_calls);ts_ssh_session_destroy(s);}
 reset();trust=1;ts_ssh_session_t s=create();assert(ts_ssh_connect_with_verifier(s,approved,NULL)==ESP_OK);ts_ssh_session_destroy(s);
 for(int p=1;p<=6;p++)for(int cancel=0;cancel<2;cancel++){
  reset();s=create();if(p>2)assert(ts_ssh_connect(s)==ESP_OK);phase=p;cancel_after=cancel?1:0;
  ts_ssh_exec_result_t r={0};esp_err_t ret=p<=2?ts_ssh_connect(s):ts_ssh_exec(s,"synthetic-command",&r);
  assert(ret==ESP_ERR_TIMEOUT);assert(waits<=3);if(p==1)assert(!auth_calls);ts_ssh_exec_result_free(&r);ts_ssh_session_destroy(s);
 }
 reset();s=create();assert(ts_ssh_connect(s)==ESP_OK);ts_ssh_exec_result_t r;assert(ts_ssh_exec(s,"true",&r)==ESP_OK);assert(!r.stdout_data&&r.stderr_len==3&&!strcmp(r.stderr_data,"err"));ts_ssh_exec_result_free(&r);ts_ssh_session_destroy(s);reset();
 assert(!strstr(captured_logs,"synthetic-secret"));puts("PASS actual SSH client: pre-auth trust gate, six EAGAIN phase deadlines/cancellation, stderr-only cleanup, bounded wait");
}
