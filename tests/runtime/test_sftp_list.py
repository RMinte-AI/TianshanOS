"""Production SFTP list API and CLI with controlled remote-directory boundary."""
from test_console_lifecycle import body, run_native, IDF
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
