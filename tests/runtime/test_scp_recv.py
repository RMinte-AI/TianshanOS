"""Exercise the production SCP receive function with an in-process transport fake."""
from pathlib import Path
import re, subprocess, tempfile

source=Path('components/ts_security/src/ts_scp.c').read_text()
m=re.search(r'^esp_err_t ts_scp_recv\([^;]+?\)\s*\{',source,re.M)
assert m
function=source[m.start():source.index('\n}',m.start())+2]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "ts_scp.h"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define TS_MALLOC_PSRAM(n) malloc(n)
#define taskYIELD() ((void)0)
#define SCP_BUFFER_SIZE 4096
#define LIBSSH2_ERROR_EAGAIN (-37)
typedef struct {int unused;} LIBSSH2_SESSION;
typedef struct {int unused;} LIBSSH2_CHANNEL;
typedef uint64_t libssh2_uint64_t;
typedef struct {uint64_t st_size;} libssh2_struct_stat;
static LIBSSH2_SESSION session;static LIBSSH2_CHANNEL channel;
static const char *content;static size_t length,offset,declared;static unsigned freed,progress_calls;static uint64_t progress_last;
static int wait_once;
ts_ssh_state_t ts_ssh_get_state(ts_ssh_session_t s){(void)s;return TS_SSH_STATE_CONNECTED;}
static LIBSSH2_SESSION *ts_ssh_get_libssh2_session(ts_ssh_session_t s){(void)s;return &session;}
static int ts_storage_check_write_path(const char *path,bool internal){(void)path;(void)internal;return ESP_OK;}
static int ts_storage_open_write(const char *path,FILE **file){*file=fopen(path,"wb");return *file?ESP_OK:ESP_FAIL;}
static LIBSSH2_CHANNEL *libssh2_scp_recv2(LIBSSH2_SESSION *s,const char *path,libssh2_struct_stat *info){assert(s==&session&&!strcmp(path,"remote"));info->st_size=declared;return &channel;}
static int libssh2_session_last_errno(LIBSSH2_SESSION *s){(void)s;return 0;}
static int libssh2_session_last_error(LIBSSH2_SESSION *s,char **message,int *len,int copy){(void)s;(void)len;(void)copy;*message="fake";return 0;}
static int scp_wait_socket(ts_ssh_session_t s,int timeout){(void)s;(void)timeout;return 0;}
static ssize_t libssh2_channel_read(LIBSSH2_CHANNEL *c,char *out,size_t count){assert(c==&channel);if(wait_once){wait_once=0;return LIBSSH2_ERROR_EAGAIN;}size_t n=length-offset;if(n>count)n=count;if(n){memcpy(out,content+offset,n);offset+=n;}return n;}
static void libssh2_channel_free(LIBSSH2_CHANNEL *c){assert(c==&channel);++freed;}
static void progress(uint64_t n,uint64_t total,void *user){(void)user;assert(total==declared);++progress_calls;progress_last=n;}
'''+function+r'''
int main(int argc,char **argv){assert(argc==2);const char *path=argv[1];ts_ssh_session_t ssh=(ts_ssh_session_t)1;
 content="abc";length=3;declared=5;offset=0;wait_once=1;
 assert(ts_scp_recv(ssh,"remote",path,progress,NULL)==ESP_FAIL&&freed==1&&progress_calls==1&&progress_last==3);
 FILE *file=fopen(path,"rb");assert(file);char buf[8]={0};assert(fread(buf,1,sizeof(buf),file)==3&&!strcmp(buf,"abc"));fclose(file);
 offset=0;declared=3;progress_calls=0;
 assert(ts_scp_recv(ssh,"remote",path,progress,NULL)==ESP_OK&&freed==2&&progress_last==3);
 offset=0;length=declared=0;progress_calls=0;
 assert(ts_scp_recv(ssh,"remote",path,progress,NULL)==ESP_OK&&freed==3&&!progress_calls);
 file=fopen(path,"rb");assert(file&&fgetc(file)==EOF);fclose(file);
 puts("PASS SCP receive: premature EOF fails and retains partial file; complete and empty files succeed");
}
'''
with tempfile.TemporaryDirectory(prefix='ts-063-scp-') as tmp:
    c=Path(tmp)/'scp.c';exe=Path(tmp)/'scp';c.write_text(code)
    subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined','-Itests/runtime/storage_stubs','-Icomponents/ts_security/include',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe),str(Path(tmp)/'received')],check=True)
