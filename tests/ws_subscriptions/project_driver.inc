/* Real project SSH client + Shell; only libssh2/socket/clock boundaries fake. */
#include <libssh2.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <unistd.h>
#include "ts_known_hosts.h"
struct _LIBSSH2_SESSION {bool local_closed;};
struct _LIBSSH2_CHANNEL {LIBSSH2_SESSION *owner;};
static LIBSSH2_CHANNEL *driver_channel;
static int driver_channels,driver_sessions,driver_sockets,driver_writes,driver_phase,driver_reads;
static bool driver_eof,driver_write_fail,driver_write_again,driver_resize_reject,driver_close_again,driver_partial,driver_read_fail;
static int driver_socket(int d,int t,int p){(void)d;(void)t;(void)p;driver_sockets++;return 10;}
static int driver_close(int fd){(void)fd;driver_sockets--;return 0;}
static int driver_connect(int fd,const struct sockaddr*a,socklen_t n){(void)fd;(void)a;(void)n;return 0;}
static int driver_fcntl(int fd,int cmd,...){(void)fd;(void)cmd;return 0;}
static int driver_select(int n,fd_set*r,fd_set*w,fd_set*x,struct timeval*t){(void)n;(void)r;(void)w;(void)x;test_now+=(int64_t)t->tv_sec*1000000+t->tv_usec;return 0;}
int libssh2_init(int f){(void)f;return 0;}
void libssh2_exit(void){}
LIBSSH2_SESSION *libssh2_session_init_ex(LIBSSH2_ALLOC_FUNC((*a)),LIBSSH2_FREE_FUNC((*f)),LIBSSH2_REALLOC_FUNC((*r)),void*x){(void)a;(void)f;(void)r;(void)x;driver_sessions++;return calloc(1,sizeof(LIBSSH2_SESSION));}
int libssh2_channel_free(LIBSSH2_CHANNEL*c){if(driver_close_again && !c->owner->local_closed)return LIBSSH2_ERROR_EAGAIN;assert(c==driver_channel);driver_channel=NULL;driver_channels--;free(c);return 0;}
int libssh2_session_free(LIBSSH2_SESSION*s){if(driver_channel){int r=libssh2_channel_free(driver_channel);if(r)return r;}driver_sessions--;free(s);return 0;}
void libssh2_session_set_blocking(LIBSSH2_SESSION*s,int b){(void)s;assert(!b);}
void *libssh2_session_callback_set(LIBSSH2_SESSION*s,int c,void*f){(void)c;(void)f;s->local_closed=true;return NULL;}
int libssh2_session_handshake(LIBSSH2_SESSION*s,libssh2_socket_t f){(void)s;(void)f;return 0;}
int libssh2_session_block_directions(LIBSSH2_SESSION*s){(void)s;return LIBSSH2_SESSION_BLOCK_INBOUND;}
int libssh2_session_last_errno(LIBSSH2_SESSION*s){(void)s;return driver_phase==1?LIBSSH2_ERROR_EAGAIN:0;}
int libssh2_session_last_error(LIBSSH2_SESSION*s,char **m,int*n,int w){(void)s;(void)n;(void)w;*m="boundary rejection";return 0;}
int libssh2_userauth_password_ex(LIBSSH2_SESSION*s,const char*u,unsigned int l,const char*p,unsigned int n,LIBSSH2_PASSWD_CHANGEREQ_FUNC((*change))){(void)s;(void)u;(void)l;(void)p;(void)n;(void)change;return 0;}
int libssh2_userauth_publickey_fromfile_ex(LIBSSH2_SESSION*s,const char*u,unsigned int l,const char*a,const char*b,const char*p){(void)a;(void)b;return libssh2_userauth_password_ex(s,u,l,p,0,NULL);}
int libssh2_userauth_publickey_frommemory(LIBSSH2_SESSION*s,const char*u,size_t l,const char*a,size_t al,const char*b,size_t bl,const char*p){(void)a;(void)al;(void)b;(void)bl;return libssh2_userauth_password_ex(s,u,l,p,0,NULL);}
LIBSSH2_CHANNEL *libssh2_channel_open_ex(LIBSSH2_SESSION*s,const char*t,unsigned int l,unsigned int w,unsigned int p,const char*m,unsigned int ml){(void)t;(void)l;(void)w;(void)p;(void)m;(void)ml;if(driver_phase==1)return NULL;assert(!driver_channel);driver_channel=calloc(1,sizeof(*driver_channel));driver_channel->owner=s;driver_channels++;return driver_channel;}
int libssh2_channel_request_pty_ex(LIBSSH2_CHANNEL*c,const char*t,unsigned int l,const char*m,unsigned int ml,int w,int h,int wp,int hp){(void)c;(void)t;(void)l;(void)m;(void)ml;(void)w;(void)h;(void)wp;(void)hp;return driver_phase==2?LIBSSH2_ERROR_EAGAIN:driver_phase==4?-1:0;}
int libssh2_channel_process_startup(LIBSSH2_CHANNEL*c,const char*r,unsigned int rl,const char*m,unsigned int ml){(void)c;(void)r;(void)rl;(void)m;(void)ml;return driver_phase==3?LIBSSH2_ERROR_EAGAIN:0;}
int libssh2_channel_request_pty_size_ex(LIBSSH2_CHANNEL*c,int w,int h,int wp,int hp){(void)c;(void)w;(void)h;(void)wp;(void)hp;return driver_resize_reject?LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED:0;}
ssize_t libssh2_channel_write_ex(LIBSSH2_CHANNEL*c,int stream,const char*b,size_t n){(void)c;(void)stream;(void)b;if(driver_partial){driver_partial=false;driver_write_fail=true;driver_writes++;return 1;}if(driver_write_again)return LIBSSH2_ERROR_EAGAIN;if(driver_write_fail)return LIBSSH2_ERROR_SOCKET_SEND;driver_writes++;return n;}
ssize_t libssh2_channel_read_ex(LIBSSH2_CHANNEL*c,int stream,char*b,size_t n){(void)c;(void)stream;(void)b;(void)n;driver_reads++;return driver_read_fail?LIBSSH2_ERROR_SOCKET_RECV:0;}
int libssh2_channel_eof(LIBSSH2_CHANNEL*c){(void)c;return driver_eof;}
int libssh2_channel_send_eof(LIBSSH2_CHANNEL*c){(void)c;return driver_close_again?LIBSSH2_ERROR_EAGAIN:0;}
int libssh2_channel_wait_eof(LIBSSH2_CHANNEL*c){(void)c;return driver_close_again?LIBSSH2_ERROR_EAGAIN:0;}
int libssh2_channel_close(LIBSSH2_CHANNEL*c){return driver_close_again&&!c->owner->local_closed?LIBSSH2_ERROR_EAGAIN:0;}
int libssh2_channel_get_exit_status(LIBSSH2_CHANNEL*c){(void)c;return 0;}
esp_err_t ts_known_hosts_verify(ts_ssh_session_t s,ts_host_verify_result_t*r,ts_known_host_t*h){(void)s;(void)h;*r=TS_HOST_VERIFY_OK;return ESP_OK;}
#define socket driver_socket
#define close driver_close
#define connect driver_connect
#define fcntl driver_fcntl
#define select driver_select
#define malloc tracked_malloc
#define calloc host_calloc
#define free tracked_free
#define TAG client_tag
#define wait_socket client_wait_socket
#include "../../components/ts_security/src/ts_ssh_client.c"
#undef wait_socket
#undef TAG
#define TAG shell_tag
#include "../../components/ts_security/src/ts_ssh_shell.c"
#undef TAG
#undef malloc
#undef calloc
#undef free
#undef socket
#undef close
#undef connect
#undef fcntl
#undef select
