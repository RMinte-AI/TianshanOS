"""Production SFTP list API and CLI with controlled remote-directory boundary."""
from test_console_lifecycle import body, run_native, IDF
from pathlib import Path
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ts_api.h"
typedef void *ts_ssh_session_t,*ts_sftp_session_t,*ts_sftp_dir_t;
typedef struct {char name[256];struct {bool is_dir;uint64_t size;unsigned permissions,mtime;} attrs;} ts_sftp_dirent_t;
static int entries,at,stop_error,dirs,connections,live_json;
static void *json_alloc(size_t n){void *p=malloc(n);if(p)++live_json;return p;}
static void json_free(void *p){if(p)--live_json;free(p);}
static esp_err_t connect_sftp(const cJSON *p,ts_ssh_session_t *ssh,ts_sftp_session_t *sftp){(void)p;assert(!connections);++connections;*ssh=(void*)1;*sftp=(void*)2;return ESP_OK;}
static void close_sftp(ts_ssh_session_t ssh,ts_sftp_session_t sftp){assert(ssh==(void*)1&&sftp==(void*)2&&connections==1);--connections;}
static int ts_sftp_dir_open(ts_sftp_session_t s,const char *p,ts_sftp_dir_t *d){assert(s==(void*)2&&!strcmp(p,"/fixture")&&!dirs);++dirs;*d=(void*)3;at=0;return ESP_OK;}
static int ts_sftp_dir_read(ts_sftp_dir_t d,ts_sftp_dirent_t *e){assert(d==(void*)3&&dirs);if(at==entries)return stop_error;snprintf(e->name,sizeof(e->name),"entry%d",at);e->attrs.is_dir=at==0;e->attrs.size=10+at;e->attrs.permissions=0755;e->attrs.mtime=1234;++at;return ESP_OK;}
static void ts_sftp_dir_close(ts_sftp_dir_t d){assert(d==(void*)3&&dirs==1);--dirs;}
void ts_api_result_ok(ts_api_result_t *r,cJSON *d){r->code=TS_API_OK;r->data=d;}
void ts_api_result_error(ts_api_result_t *r,ts_api_result_code_t c,const char *m){r->code=c;r->message=(char*)m;}
'''
code+=body('components/ts_api/src/ts_api_sftp.c','api_sftp_ls')
code+=r'''
void ts_api_result_init(ts_api_result_t *r){memset(r,0,sizeof(*r));}
void ts_api_result_free(ts_api_result_t *r){cJSON_Delete(r->data);r->data=NULL;}
esp_err_t ts_api_call(const char *name,const cJSON *p,ts_api_result_t *r){assert(!strcmp(name,"sftp.ls"));return api_sftp_ls(p,r);}
static char output[8192];
static int ts_console_printf(const char *fmt,...){va_list ap;va_start(ap,fmt);int n=vsnprintf(output+strlen(output),sizeof(output)-strlen(output),fmt,ap);va_end(ap);return n;}
'''
code+=body('components/ts_console/commands/ts_cmd_sftp.c','do_sftp_ls')
code+=r'''
int main(void){
 cJSON_Hooks hooks={json_alloc,json_free};cJSON_InitHooks(&hooks);
 int counts[]={0,3,0,2,3};int errors[]={ESP_ERR_NOT_FOUND,ESP_ERR_NOT_FOUND,ESP_FAIL,ESP_FAIL,ESP_ERR_NOT_FOUND};
 for(unsigned i=0;i<5;i++){
  entries=counts[i];stop_error=errors[i];cJSON *params=cJSON_Parse("{\"path\":\"/fixture\"}");ts_api_result_t r={0};
  int rc=api_sftp_ls(params,&r);assert(!dirs&&!connections);
  if(stop_error==ESP_ERR_NOT_FOUND){assert(rc==ESP_OK&&r.code==TS_API_OK);cJSON *files=cJSON_GetObjectItem(r.data,"files");assert(cJSON_GetArraySize(files)==entries);for(int j=0;j<entries;j++){cJSON *f=cJSON_GetArrayItem(files,j);assert(cJSON_GetObjectItem(f,"size")->valueint==10+j&&cJSON_GetObjectItem(f,"mtime")->valueint==1234);}}
  else {assert(rc==ESP_FAIL&&r.code==TS_API_ERR_INTERNAL&&!r.data&&!strcmp(r.message,"Failed to read directory"));}
  cJSON_Delete(params);cJSON_Delete(r.data);assert(!live_json);
  output[0]=0;assert(do_sftp_ls("fixture.invalid",22,"fixture","unused","/fixture",5)==(stop_error==ESP_ERR_NOT_FOUND?0:1));
  if(stop_error!=ESP_ERR_NOT_FOUND){assert(strstr(output,"Error:")&&!strstr(output,"Directory:")&&!strstr(output,"entry"));}
  assert(!live_json&&!dirs&&!connections);
 }
 puts("PASS F17 real API/CLI: empty/multiple EOF, first/midstream failure with no success list, next request succeeds, each directory/connection/JSON freed");
}
'''
run_native(code,[IDF/'components/json/cJSON/cJSON.c'])

# Execute the actual F70 connection helpers as well; SSH transport/keystore are
# leaves. One authentication waits while another request connects and frees its key.
keys = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "ts_ssh_client.h"
#include "ts_sftp.h"
struct ts_ssh_session_s {ts_ssh_config_t config;};
struct ts_sftp_session_s {int unused;};
static _Thread_local unsigned failure,request_id,key_len,freed,created,destroyed,disconnected,closed;
static _Thread_local char *owned;
static _Thread_local bool concurrent;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool first_authenticating,second_finished;
static const char *key_name(void){return request_id?"second-key":"first-key";}
esp_err_t ts_keystore_load_private_key(const char *id,char **data,size_t *len){
    if(failure==1)return ESP_FAIL;assert(!owned&&!strcmp(id,key_name()));
    *data=owned=strdup(id);*len=key_len=strlen(id)+1;return ESP_OK;
}
esp_err_t ts_ssh_session_create(const ts_ssh_config_t *config,ts_ssh_session_t *session){
    if(failure==2)return ESP_FAIL;
    *session=malloc(sizeof(**session));(*session)->config=*config;created++;return ESP_OK;
}
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t session){destroyed++;free(session);return ESP_OK;}
esp_err_t ts_ssh_disconnect(ts_ssh_session_t session){disconnected++;return ESP_OK;}
esp_err_t ts_ssh_connect(ts_ssh_session_t session){
    const ts_ssh_config_t *c=&session->config;
    assert(!strcmp(c->host,request_id?"host-b":"host-a")&&!strcmp(c->username,"user"));
    if(concurrent&&!request_id){pthread_mutex_lock(&gate);first_authenticating=true;pthread_cond_broadcast(&changed);
        while(!second_finished)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);}
    if(c->auth_method==TS_SSH_AUTH_PASSWORD)assert(!strcmp(c->auth.password,"password"));
    else if(c->auth.key.private_key){assert(c->auth.key.private_key==(void*)owned&&!strcmp((char*)c->auth.key.private_key,key_name()));}
    else assert(!strcmp(c->auth.key.private_key_path,"/key.pem"));
    return failure==3?ESP_FAIL:ESP_OK;
}
esp_err_t ts_sftp_open(ts_ssh_session_t ssh,ts_sftp_session_t *sftp){if(failure==4)return ESP_FAIL;*sftp=malloc(sizeof(**sftp));return ESP_OK;}
esp_err_t ts_sftp_close(ts_sftp_session_t sftp){closed++;free(sftp);return ESP_OK;}
static void free_key(void *p){assert(p==owned);for(unsigned i=0;i<key_len;i++)assert(!owned[i]);freed++;free(p);owned=NULL;}
#define free free_key
'''
keys += ''.join(body('components/ts_api/src/ts_api_sftp.c', n) for n in
                ['configure_ssh_from_params', 'connect_sftp', 'close_sftp'])
# close_sftp only uses the transport destruction leaves, not free directly.
keys += r'''
#undef free
static void run_request(unsigned auth){
    freed=created=destroyed=disconnected=closed=0;assert(!owned);
    cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"host",request_id?"host-b":"host-a");cJSON_AddStringToObject(p,"user","user");
    cJSON_AddStringToObject(p,auth==0?"keyid":auth==1?"password":"keypath",auth==0?key_name():auth==1?"password":"/key.pem");
    ts_ssh_session_t ssh=NULL;ts_sftp_session_t sftp=NULL;esp_err_t ret=connect_sftp(p,&ssh,&sftp);
    assert(ret==(failure?ESP_FAIL:ESP_OK));assert(!owned&&freed==(auth==0&&failure!=1));
    if(!failure)close_sftp(ssh,sftp);
    assert(created==destroyed&&closed==!failure&&disconnected==(!failure||failure==4));
    cJSON_Delete(p);
}
static void *worker(void *arg){request_id=(unsigned)(uintptr_t)arg;concurrent=true;run_request(0);
    if(request_id){pthread_mutex_lock(&gate);second_finished=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);}return NULL;}
int main(void){
    for(failure=0;failure<=4;failure++)run_request(0);
    failure=0;run_request(1);run_request(2);
    pthread_t first,second;assert(!pthread_create(&first,NULL,worker,(void*)0));
    pthread_mutex_lock(&gate);while(!first_authenticating)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);
    assert(!pthread_create(&second,NULL,worker,(void*)1));assert(!pthread_join(first,NULL)&&!pthread_join(second,NULL));
    puts("PASS F70 real SFTP helpers: overlapping keys, password/path, load/create/auth/open failures, key wiped/freed once and session cleanup");
}
'''
run_native(keys, [IDF/'components/json/cJSON/cJSON.c'], {
    name: Path('components/ts_security/include', name).read_text()
    for name in ['ts_ssh_client.h', 'ts_sftp.h']})
