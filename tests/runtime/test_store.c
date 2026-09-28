#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ts_rule_codec.h"
#include "ts_action_manager.h"
#include "nvs.h"
static int fail_at, crash_at, operations;
static jmp_buf crash_jump;
static bool failure(void) { ++operations; if (crash_at && operations == crash_at) longjmp(crash_jump, 1); return fail_at && operations == fail_at; }
typedef struct {char name[32];void *data;size_t size;} blob;
static blob nv[80];
void *heap_caps_malloc(size_t n,unsigned caps){return malloc(n);}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps){return calloc(n,size);}
esp_err_t ts_action_template_get(const char *id,ts_action_template_t *tpl){return ESP_ERR_NOT_FOUND;}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h){*h=1;return failure()?ESP_FAIL:ESP_OK;}
void nvs_close(nvs_handle_t h){}
esp_err_t nvs_commit(nvs_handle_t h){return failure()?ESP_FAIL:ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *data,size_t *size){
 if(failure())return ESP_FAIL;
 for(int i=0;i<80;i++)if(!strcmp(nv[i].name,key)){if(!data){*size=nv[i].size;return ESP_OK;}if(*size<nv[i].size)return ESP_ERR_INVALID_SIZE;memcpy(data,nv[i].data,nv[i].size);*size=nv[i].size;return ESP_OK;}
 return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t size){
 if(failure())return ESP_FAIL;
 for(int i=0;i<80;i++)if(!nv[i].name[0]||!strcmp(nv[i].name,key)){free(nv[i].data);strcpy(nv[i].name,key);nv[i].data=malloc(size);memcpy(nv[i].data,data,size);nv[i].size=size;return ESP_OK;}
 return ESP_ERR_NO_MEM;
}
static int test_rename(const char *a,const char *b){if(failure())return -1;return rename(a,b);}
static int test_unlink(const char *a){if(failure()){errno=EIO;return -1;}return unlink(a);}
static size_t test_write(const void *p,size_t s,size_t n,FILE *f){if(failure())return 0;return fwrite(p,s,n,f);}
static int test_flush(FILE *f){int rc=fflush(f);return failure()?-1:rc;}
static int test_sync(int fd){int rc=fsync(fd);return failure()?-1:rc;}
static int test_close(FILE *f){int rc=fclose(f);return failure()?-1:rc;}
#define rename test_rename
#define unlink test_unlink
#define fwrite test_write
#define fflush test_flush
#define fsync test_sync
#define fclose test_close
#define DIR_RULES "/tmp/tianshan-runtime-store"
#include "../../components/ts_automation/src/ts_rule_store.c"
#undef rename
#undef unlink
#undef fwrite
#undef fflush
#undef fsync
#undef fclose
static void clear_all(void){
 for(int i=0;i<80;i++){free(nv[i].data);memset(&nv[i],0,sizeof(nv[i]));}
 unlink(DIR_RULES "/model.json");unlink(DIR_RULES "/.model.pending");unlink(DIR_RULES "/.model.previous");
 memset(&guard,0,sizeof(guard));recovery_error=false;fail_at=crash_at=operations=0;
}
static ts_auto_rule_t rule(const char *name){static ts_auto_action_t a={.type=TS_AUTO_ACT_LOG}; ts_auto_rule_t r={.actions=&a,.action_count=1};strcpy(r.id,"model");strcpy(r.name,name);r.enabled=true;r.revision=1;return r;}
static void assert_bank(const char *name){ts_auto_rule_t r[2]={0};int n=0;assert(ts_rule_store_load_bank(r,2,&n)==ESP_OK);assert(n==1&&!strcmp(r[0].name,name));ts_rule_dispose(&r[0]);}
static void assert_sd_complete(void){FILE*f=fopen(DIR_RULES "/model.json","rb");assert(f);char b[4096];size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);b[n]=0;cJSON*j=cJSON_Parse(b);assert(j);const char*name=cJSON_GetObjectItem(j,"name")->valuestring;assert(!strcmp(name,"old")||!strcmp(name,"new"));cJSON_Delete(j);}
int main(void){
 mkdir(DIR_RULES,0700);ts_auto_rule_t old=rule("old"),next=rule("new");next.revision=2;ts_rule_commit_t result;
 for(int source=0;source<=1;source++)for(int fault=1;fault<95;fault++){
  clear_all();assert(ts_rule_store_commit(NULL,0,&old,old.id,source,&result)==ESP_OK);
  operations=0;fail_at=fault;esp_err_t ret=ts_rule_store_commit(&old,1,&next,next.id,source,&result);fail_at=0;
  bool bank=false,sd=false;esp_err_t recovery=ts_rule_store_recover(source==1,&bank,&sd);
  if(recovery==ESP_OK){if(source==1)assert_sd_complete();else{ts_auto_rule_t r[2]={0};int n;esp_err_t loaded=ts_rule_store_load_bank(r,2,&n);if(loaded!=ESP_OK||n!=1)fprintf(stderr,"source=%d fault=%d ret=%d guardbank=%u loaded=%d count=%d\n",source,fault,ret,guard.bank,loaded,n);assert(loaded==ESP_OK&&n==1);assert(!strcmp(r[0].name,"old")||!strcmp(r[0].name,"new"));if(ret==ESP_OK)assert(!strcmp(r[0].name,"new"));ts_rule_dispose(&r[0]);}}
  if(ret==ESP_OK)assert(result.applied==1&&result.durable==1);
 }
 /* Crash at every persistent mutation boundary (read calls included). */
 for(int fault=1;fault<80;fault++){
  clear_all();assert(ts_rule_store_commit(NULL,0,&old,old.id,1,&result)==ESP_OK);
  operations=0;crash_at=fault;
  if(!setjmp(crash_jump))ts_rule_store_commit(&old,1,&next,next.id,1,&result);
  crash_at=0;bool bank=false,sd=false;
  if(ts_rule_store_recover(true,&bank,&sd)==ESP_OK)assert_sd_complete();
 }
 clear_all();assert(ts_rule_store_commit(NULL,0,&old,old.id,0,&result)==ESP_OK);assert_bank("old");
 assert(ts_rule_store_commit(&old,1,&next,next.id,2,&result)==ESP_ERR_NOT_SUPPORTED);assert_bank("old");
 clear_all();puts("PASS actual rule persistence: NVS and SD fault injection, recovery boundaries, read-only rejection");
}
