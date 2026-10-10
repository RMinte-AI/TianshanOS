#!/usr/bin/env python3
"""Actual keystore with immediate-write NVS and crypto boundaries, no device access."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
idf=Path(os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf'))
source=Path(os.environ.get('TS_KEYSTORE_TEST_SOURCE', root/'components/ts_security/src/ts_keystore.c')).read_text()
api_source=(root/'components/ts_api/src/ts_api_key.c').read_text()
a=api_source.index('static esp_err_t api_key_generate(');api_handler=api_source[a:api_source.index('\n/**',a)]
web_source=(root/'components/ts_webui/src/ts_webui_api.c').read_text()
a=web_source.index('static esp_err_t send_key_generate_result(');web_handler=web_source[a:web_source.index('\nstatic esp_err_t',a+1)]
a=web_source.index('static esp_err_t api_handler(');web_handler+=web_source[a:web_source.index('\nstatic esp_err_t login_handler',a)]
a=web_source.index('static bool is_binary_upload_complete(');body_complete=web_source[a:web_source.index('\n}',a)+2]
pack_source=(root/'components/ts_config_pack/src/ts_config_pack.c').read_text()
a=pack_source.index('static bool unambiguous_json(');strict_json=pack_source[a:pack_source.index('\nstatic const char *TAG',a)]
source=re.sub(r'^#include .*$', '',source,flags=re.M)
platform=(root/'tests/certificate/stubs/platform.h').read_text()
platform=platform[:platform.index('typedef void *ts_keypair_t;')]+'\n#endif\n'
platform=platform.replace('#define TS_CRYPTO_KEY_EC_P256 1','')
prefix=r'''
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "ts_keystore.h"
#include "ts_crypto.h"
#include "ts_api.h"
#include <stdatomic.h>
#include <ctype.h>
#define CONFIG_TS_CONFIG_PACK_MAX_SIZE 65536
#define TS_LOGI(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define TS_LOGD(...) ((void)0)
#define API_PREFIX "/api/v1"
#define TS_API_MALLOC malloc
typedef struct {size_t content_len,query_len;} test_http_req_t;
typedef struct {const char *uri;const char *body;size_t body_len;int method;test_http_req_t *req;} ts_http_request_t;
static size_t httpd_req_get_url_query_len(void *p){return p?((test_http_req_t *)p)->query_len:0;}
static esp_err_t httpd_req_get_url_query_str(void *p,char *b,size_t n){return ESP_FAIL;}
esp_err_t ts_http_send_json(ts_http_request_t *,int,const char *);
esp_err_t ts_http_send_error(ts_http_request_t *,int,const char *);
static int64_t esp_timer_get_time(void){return 0;}
static unsigned rule_dispatch_calls;
#include "cJSON.h"
static size_t allocation_size_fail;
static void *key_alloc(size_t n){return n==allocation_size_fail?NULL:malloc(n);}
#define TS_MALLOC_PSRAM key_alloc
#define TS_STRDUP_PSRAM strdup
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NOT_ALLOWED 0x109
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define NVS_KEY_NAME_MAX_SIZE 16
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
static void vSemaphoreDelete(SemaphoreHandle_t p) { pthread_mutex_destroy(p);free(p); }
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { SemaphoreHandle_t p=malloc(sizeof(*p));pthread_mutex_init(p,NULL);return p; }
'''
boundaries=r'''
typedef struct {char key[40];unsigned char data[4096];size_t len;bool used;} entry;
static entry db[40];
static const char *fail_key,*fail_get,*fail_erase;
static int fail_write_at,fail_commit_at,commit_calls,writes,write_hits;
static bool fail_response_alloc;
static void *always_fail(size_t n);
static bool fail_partial,fail_restore;
static atomic_int crypto_calls;
static int crypto_error,private_error,public_error,live_crypto;
static bool private_empty;

static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static bool gate_enabled;static int gate_arrived,gate_target;
static char sent_json[4096];static int send_status;
static entry *find(const char *key) {for(int i=0;i<40;i++)if(db[i].used&&!strcmp(db[i].key,key))return &db[i];return NULL;}
static esp_err_t get(const char *key,void *out,size_t *len){if(fail_get&&!strcmp(fail_get,key))return ESP_FAIL;entry *e=find(key);if(!e)return ESP_ERR_NVS_NOT_FOUND;if(out&&*len<e->len)return ESP_ERR_NVS_INVALID_LENGTH;if(out)memcpy(out,e->data,e->len);*len=e->len;return ESP_OK;}
static esp_err_t set(const char *key,const void *data,size_t len){
 writes++;bool failed=fail_key&&!strcmp(fail_key,key)&&++write_hits==fail_write_at;
 if(fail_restore&&!strcmp(key,"_index")&&!strcmp(data,"keep"))failed=true;
 if(failed&&!fail_partial)return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
 entry *e=find(key);if(!e){for(int i=0;i<40;i++)if(!db[i].used){e=&db[i];break;}}assert(e&&len<=sizeof(e->data));e->used=true;strcpy(e->key,key);memcpy(e->data,data,len);e->len=len;
 return failed?ESP_ERR_NVS_NOT_ENOUGH_SPACE:ESP_OK;
}
esp_err_t nvs_open(const char *n,int mode,nvs_handle_t *h){*h=1;return ESP_OK;}
void nvs_close(nvs_handle_t h){}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *b,size_t *n){return get(k,b,n);}
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *b,size_t *n){return get(k,b,n);}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *b,size_t n){return set(k,b,n);}
esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *b){return set(k,b,strlen(b)+1);}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *k){if(fail_erase&&!strcmp(fail_erase,k))return ESP_FAIL;entry *e=find(k);if(!e)return ESP_ERR_NVS_NOT_FOUND;e->used=false;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){if(fail_response_alloc){cJSON_Hooks hooks={always_fail,free};cJSON_InitHooks(&hooks);}return ++commit_calls==fail_commit_at?ESP_FAIL:ESP_OK;}
esp_err_t ts_crypto_keypair_generate(ts_crypto_key_type_t t,ts_keypair_t *p){
 crypto_calls++;if(crypto_error)return crypto_error;
 pthread_mutex_lock(&gate_lock);
 if(gate_enabled){gate_arrived++;pthread_cond_broadcast(&gate_cond);while(gate_arrived<gate_target)pthread_cond_wait(&gate_cond,&gate_lock);}
 pthread_mutex_unlock(&gate_lock);
 *p=(ts_keypair_t)malloc(1);__atomic_add_fetch(&live_crypto,1,__ATOMIC_SEQ_CST);return ESP_OK;
}
void ts_crypto_keypair_free(ts_keypair_t p){free(p);__atomic_sub_fetch(&live_crypto,1,__ATOMIC_SEQ_CST);}
esp_err_t ts_crypto_keypair_export_private(ts_keypair_t p,char *b,size_t *n){if(private_error)return private_error;strcpy(b,"SYNTHETIC");*n=private_empty?0:10;return ESP_OK;}
esp_err_t ts_crypto_keypair_export_openssh(ts_keypair_t p,char *b,size_t *n,const char *c){if(public_error)return public_error;strcpy(b,"SYNTHETIC");*n=10;return ESP_OK;}
esp_err_t ts_api_call(const char *name,const cJSON *p,ts_api_result_t *r){
 if(!strcmp(name,"key.generate"))return api_key_generate(p,r);
 if(!strcmp(name,"automation.rules.import")){++rule_dispatch_calls;r->code=0;return ESP_OK;}
 r->code=TS_API_ERR_INTERNAL;r->message=strdup("fixture");r->data=cJSON_CreateObject();cJSON_AddBoolToObject(r->data,"should_omit",true);return ESP_FAIL;
}
void ts_api_result_error(ts_api_result_t *r,ts_api_result_code_t c,const char *s){r->code=c;r->message=strdup(s);}
void ts_api_result_ok(ts_api_result_t *r,cJSON *data){r->code=0;r->data=data;}
void ts_api_result_free(ts_api_result_t *r){free(r->message);cJSON_Delete(r->data);memset(r,0,sizeof(*r));}
esp_err_t ts_http_send_json(ts_http_request_t *r,int status,const char *j){strcpy(sent_json,j);send_status=status;return ESP_OK;}
esp_err_t ts_http_send_error(ts_http_request_t *r,int status,const char *j){strcpy(sent_json,j);send_status=status;return ESP_OK;}
static void reset(void){assert(!live_crypto);memset(db,0,sizeof(db));fail_key=fail_get=fail_erase=NULL;fail_write_at=fail_commit_at=commit_calls=writes=write_hits=0;fail_partial=fail_restore=false;crypto_calls=crypto_error=private_error=public_error=0;private_empty=false;gate_enabled=false;gate_arrived=gate_target=0;allocation_size_fail=0;fail_response_alloc=false;}
static esp_err_t store(const char *id){ts_keystore_keypair_t p={"SYNTHETIC",10,"SYNTHETIC",10};return ts_keystore_store_key(id,&p,TS_KEYSTORE_TYPE_RSA_2048,NULL);}
static ts_keystore_generate_result_t detail;
static esp_err_t generate(const char *id){return ts_keystore_generate_key_with_result(id,TS_KEYSTORE_TYPE_RSA_2048,NULL,&detail);}
static void absent(const char *id){char key[40];for(int i=0;i<3;i++){snprintf(key,sizeof(key),"%s%s",id,(const char*[]){"_priv","_pub","_meta"}[i]);assert(!find(key));}assert(!find("_index")||!index_contains((char*)find("_index")->data,id));}
static void kept(void){assert(!strcmp((char*)find("keep_priv")->data,"SYNTHETIC"));assert(find("keep_pub")&&find("keep_meta"));assert(index_contains((char*)find("_index")->data,"keep"));}
struct worker {const char *id;esp_err_t result;ts_keystore_generate_result_t detail;};
static void *work(void *arg){struct worker *w=arg;w->result=ts_keystore_generate_key_with_result(w->id,TS_KEYSTORE_TYPE_RSA_2048,NULL,&w->detail);return NULL;}
static void *always_fail(size_t n){return NULL;}
static int json_alloc_count,json_fail_at;
static void *fail_one_json_alloc(size_t n){return ++json_alloc_count==json_fail_at?NULL:malloc(n);}
int main(int argc,char **argv){
 assert(ts_keystore_init()==ESP_OK);reset();
 if(!strcmp(argv[1],"metadata")){fail_key="new_meta";fail_write_at=1;assert(store("new")!=ESP_OK);}
 if(!strcmp(argv[1],"index")){fail_key="_index";fail_write_at=1;assert(store("new")!=ESP_OK);}
 if(!strcmp(argv[1],"prefix")){assert(store("app")==ESP_OK);assert(store("p")==ESP_OK);assert(!strcmp((char*)find("_index")->data,"app,p"));assert(store("key10")==ESP_OK);assert(store("key1")==ESP_OK);}
 if(!strcmp(argv[1],"matrix")){
  const char *keys[]={"new_priv","new_pub","new_meta","_index",NULL};const char *stages[]={"private_write","public_write","metadata_write","index_write","commit"};
  for(int partial=0;partial<2;partial++)for(int i=0;i<5;i++){
   reset();assert(store("keep")==ESP_OK);entry snapshot[40];memcpy(snapshot,db,sizeof(db));
   fail_key=keys[i];fail_write_at=1;fail_partial=partial;if(i==4)fail_commit_at=commit_calls+1;
   assert(generate("new")!=ESP_OK);assert(!detail.stored&&detail.cleanup_complete&&!strcmp(detail.failed_stage,stages[i]));absent("new");kept();
   for(int j=0;j<40;j++)if(snapshot[j].used&&strcmp(snapshot[j].key,"_index"))assert(!memcmp(&snapshot[j],find(snapshot[j].key),sizeof(entry)));
  }
  reset();assert(store("keep")==ESP_OK);fail_commit_at=commit_calls+1;fail_restore=true;assert(generate("new")==ESP_FAIL);assert(!detail.cleanup_complete&&detail.cleanup_error!=ESP_OK);assert(find("new_priv")&&find("new_pub")&&find("new_meta"));kept();
  reset();assert(store("keep")==ESP_OK);fail_key="new_meta";fail_write_at=1;fail_erase="new_priv";assert(generate("new")!=ESP_OK&&!detail.cleanup_complete&&detail.cleanup_error==ESP_FAIL);assert(find("new_priv"));kept();
  reset();fail_key="new_meta";fail_write_at=1;fail_commit_at=1;assert(generate("new")!=ESP_OK&&!detail.cleanup_complete&&detail.cleanup_error==ESP_FAIL);absent("new");
 }
 if(!strcmp(argv[1],"admission")){
  const char *bad[]={"","comma,id","12345678901","中文中文"};for(int i=0;i<4;i++){assert(generate(bad[i])==ESP_ERR_INVALID_ARG);assert(!writes&&!crypto_calls);}
  assert(generate("1234567890")==ESP_OK&&detail.stored);entry snapshot[40];memcpy(snapshot,db,sizeof(db));int before=crypto_calls;assert(generate("1234567890")==ESP_ERR_INVALID_STATE&&crypto_calls==before&&!memcmp(db,snapshot,sizeof(db)));
  for(int i=0;i<3;i++){reset();set((const char*[]){"new_priv","new_pub","new_meta"}[i],"fragment",9);memcpy(snapshot,db,sizeof(db));assert(generate("new")==ESP_ERR_INVALID_STATE&&!crypto_calls&&!memcmp(db,snapshot,sizeof(db)));}
  reset();fail_get="_index";assert(generate("new")==ESP_FAIL&&!crypto_calls&&!writes);reset();fail_get="new_pub";assert(generate("new")==ESP_FAIL&&!crypto_calls&&!writes);
  reset();for(int i=0;i<8;i++){char id[10];snprintf(id,sizeof(id),"key%d",i);assert(generate(id)==ESP_OK);}before=crypto_calls;assert(generate("ninth")==ESP_ERR_INVALID_SIZE&&crypto_calls==before);assert(!strcmp(detail.failed_stage,"index_capacity"));
  reset();private_error=ESP_FAIL;assert(generate("new")==ESP_FAIL&&!writes&&!strcmp(detail.failed_stage,"private_export"));
  reset();private_empty=true;assert(generate("new")==ESP_ERR_INVALID_RESPONSE&&!writes&&!strcmp(detail.failed_stage,"private_export"));
  reset();public_error=ESP_FAIL;assert(generate("new")==ESP_FAIL&&!writes&&!strcmp(detail.failed_stage,"public_export"));absent("new");
  reset();allocation_size_fail=TS_KEYSTORE_PRIVKEY_MAX_LEN;assert(generate("new")==ESP_ERR_NO_MEM&&!writes&&!strcmp(detail.failed_stage,"private_export"));
  reset();allocation_size_fail=TS_KEYSTORE_PUBKEY_MAX_LEN;assert(generate("new")==ESP_ERR_NO_MEM&&!writes&&!strcmp(detail.failed_stage,"public_export"));
  reset();char too_long[MAX_INDEX_LEN+10];memset(too_long,'a',sizeof(too_long)-1);too_long[sizeof(too_long)-1]=0;set("_index",too_long,sizeof(too_long));int saved_writes=writes;assert(generate("new")==ESP_ERR_NVS_INVALID_LENGTH&&!crypto_calls&&writes==saved_writes);
  reset();char near_limit[MAX_INDEX_LEN];memset(near_limit,'a',MAX_INDEX_LEN-2);near_limit[MAX_INDEX_LEN-2]=0;set("_index",near_limit,strlen(near_limit)+1);assert(generate("new")==ESP_ERR_INVALID_SIZE&&!crypto_calls);
  reset();crypto_error=ESP_FAIL;assert(generate("new")==ESP_FAIL&&!writes&&!strcmp(detail.failed_stage,"generate"));
 }
 if(!strcmp(argv[1],"concurrency")){
  for(int same=0;same<2;same++){reset();gate_enabled=true;gate_target=2;struct worker a={"one"},b={same?"one":"two"};pthread_t t1,t2;pthread_create(&t1,NULL,work,&a);pthread_create(&t2,NULL,work,&b);pthread_join(t1,NULL);pthread_join(t2,NULL);assert((a.result==ESP_OK)+(b.result==ESP_OK)==(same?1:2));assert(index_contains((char*)find("_index")->data,"one"));if(!same)assert(index_contains((char*)find("_index")->data,"two"));}
  reset();assert(store("keep")==ESP_OK);gate_enabled=true;gate_target=2;struct worker a={"new"};pthread_t t;pthread_create(&t,NULL,work,&a);pthread_mutex_lock(&gate_lock);while(gate_arrived<1)pthread_cond_wait(&gate_cond,&gate_lock);pthread_mutex_unlock(&gate_lock);
  assert(ts_keystore_touch_key("keep")==ESP_OK);assert(ts_keystore_delete_key("keep")==ESP_OK);pthread_mutex_lock(&gate_lock);gate_arrived=2;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate_lock);pthread_join(t,NULL);assert(a.result==ESP_OK&&!find("keep_priv"));assert(!strcmp((char*)find("_index")->data,"new"));
 }
 if(!strcmp(argv[1],"lifecycle")){
  reset();gate_enabled=true;gate_target=2;struct worker a={"new"};pthread_t t;pthread_create(&t,NULL,work,&a);pthread_mutex_lock(&gate_lock);while(gate_arrived<1)pthread_cond_wait(&gate_cond,&gate_lock);pthread_mutex_unlock(&gate_lock);
  assert(ts_keystore_deinit()==ESP_OK);pthread_mutex_lock(&gate_lock);gate_arrived=2;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate_lock);pthread_join(t,NULL);assert(a.result==ESP_ERR_INVALID_STATE&&!writes);assert(ts_keystore_init()==ESP_OK);gate_enabled=false;assert(generate("after")==ESP_OK);
 }
 if(!strcmp(argv[1],"allocations")){
  for(int partial=0;partial<2;partial++)for(int i=0;i<5;i++){reset();fail_partial=partial;fail_key=(const char*[]){"new_priv","new_pub","new_meta","_index",NULL}[i];fail_write_at=1;if(i==4)fail_commit_at=1;assert(generate("new")!=ESP_OK&&detail.cleanup_complete);absent("new");assert(!find("_index"));}
  reset();fail_response_alloc=true;cJSON *p=cJSON_Parse("{\"id\":\"new\"}");ts_api_result_t r={0};assert(api_key_generate(p,&r)==ESP_ERR_NO_MEM&&r.code==TS_API_ERR_NO_MEM&&!strcmp(r.message,"key_response_failed"));cJSON_InitHooks(NULL);ts_api_result_free(&r);cJSON_Delete(p);assert(find("new_priv")&&find("new_meta"));
  for(int n=1;n<=60;n++){
   reset();cJSON *p=cJSON_Parse("{\"id\":\"new\"}");ts_api_result_t r={0};json_alloc_count=0;json_fail_at=n;cJSON_Hooks hooks={fail_one_json_alloc,free};cJSON_InitHooks(&hooks);
   esp_err_t ret=api_key_generate(p,&r);cJSON_InitHooks(NULL);
   if(ret==ESP_OK){assert(r.code==0&&cJSON_IsTrue(cJSON_GetObjectItem(r.data,"generated")));assert(cJSON_GetObjectItem(r.data,"id")&&cJSON_GetObjectItem(r.data,"type"));}
   else if(find("new_priv")){assert(find("new_meta")&&find("new_pub")&&index_contains((char*)find("_index")->data,"new"));assert(!strcmp(r.message,"key_response_failed"));}
   else {absent("new");assert(r.code!=0);}
   ts_api_result_free(&r);cJSON_Delete(p);
  }
  reset();ts_keystore_gen_opts_t opts={.comment="note",.alias="Alias",.hidden=true,.exportable=true};assert(ts_keystore_generate_key_with_result("flags",TS_KEYSTORE_TYPE_ECDSA_P384,&opts,&detail)==ESP_OK);ts_keystore_key_info_t info;assert(ts_keystore_get_key_info("flags",&info)==ESP_OK&&info.hidden&&info.exportable&&info.has_public_key&&!strcmp(info.alias,"Alias")&&!strcmp(info.comment,"note"));
 }
 if(!strcmp(argv[1],"compatibility")){
  ts_keystore_keypair_t p={"SYNTHETIC",10,NULL,0};assert(ts_keystore_store_key("old",&p,TS_KEYSTORE_TYPE_RSA_2048,"comment")==ESP_OK);ts_keystore_key_info_t info;assert(ts_keystore_get_key_info("old",&info)==ESP_OK&&!info.has_public_key&&!info.exportable);
  set("old_meta","{\"type\":\"rsa2048\",\"comment\":\"legacy\"}",strlen("{\"type\":\"rsa2048\",\"comment\":\"legacy\"}")+1);assert(ts_keystore_get_key_info("old",&info)==ESP_OK&&!info.exportable&&!info.hidden);assert(ts_keystore_generate_key_ex("wrapper",TS_KEYSTORE_TYPE_ECDSA_P256,NULL)==ESP_OK);assert(ts_keystore_generate_key("cli",TS_KEYSTORE_TYPE_RSA_2048,NULL)==ESP_OK);
 }
 if(!strcmp(argv[1],"api")){
  const char *types[]={"rsa2048","rsa4096","ec256","ec384"};const char *expected[]={"rsa2048","rsa4096","ecdsa-p256","ecdsa-p384"};
  for(int i=0;i<4;i++){reset();cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"id","new");cJSON_AddStringToObject(p,"type",types[i]);ts_api_result_t r={0};assert(api_key_generate(p,&r)==ESP_OK&&r.code==0);assert(!strcmp(cJSON_GetObjectItem(r.data,"type")->valuestring,expected[i]));send_key_generate_result(NULL,&r,"test",0);cJSON *response=cJSON_Parse(sent_json);assert(send_status==200&&cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(response,"data"),"generated")));cJSON_Delete(response);cJSON_Delete(p);}
  reset();fail_key="new_meta";fail_write_at=1;cJSON *p=cJSON_Parse("{\"id\":\"new\"}");ts_api_result_t r={0};assert(api_key_generate(p,&r)!=ESP_OK&&r.code==TS_API_ERR_INTERNAL);assert(!strcmp(r.message,"key_storage_full"));send_key_generate_result(NULL,&r,"test",0);cJSON *response=cJSON_Parse(sent_json);cJSON *data=cJSON_GetObjectItem(response,"data");assert(cJSON_IsTrue(cJSON_GetObjectItem(data,"cleanup_complete")));assert(!strcmp(cJSON_GetObjectItem(data,"failed_stage")->valuestring,"metadata_write"));cJSON_Delete(response);cJSON_Delete(p);absent("new");
  reset();assert(generate("new")==ESP_OK);p=cJSON_Parse("{\"id\":\"new\"}");assert(api_key_generate(p,&r)!=ESP_OK&&!strcmp(r.message,"key_id_occupied"));ts_api_result_free(&r);cJSON_Delete(p);
  reset();const char *body="{\"id\":\"new\",\"request_id\":\"kg-test\"}";ts_http_request_t req={.uri="/api/v1/key/generate",.body=body,.body_len=strlen(body)};
  fail_key="new_meta";fail_write_at=1;assert(api_handler(&req,NULL)==ESP_OK);response=cJSON_Parse(sent_json);assert(cJSON_GetObjectItem(response,"data")&&cJSON_GetObjectItem(response,"code")->valueint!=0);cJSON_Delete(response);absent("new");
  reset();assert(api_handler(&req,NULL)==ESP_OK);assert(find("new_priv"));response=cJSON_Parse(sent_json);assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(response,"data"),"generated")));cJSON_Delete(response);
  req.uri="/api/v1/other/endpoint";assert(api_handler(&req,NULL)==ESP_OK);response=cJSON_Parse(sent_json);assert(!cJSON_GetObjectItem(response,"data"));cJSON_Delete(response);
  req.uri="/api/v1/key/generate";cJSON_Hooks fail_request={always_fail,free};cJSON_InitHooks(&fail_request);assert(api_handler(&req,NULL)==ESP_OK);cJSON_InitHooks(NULL);assert(send_status==500&&find("new_priv"));
  r=(ts_api_result_t){.code=0,.data=cJSON_CreateObject()};cJSON_AddBoolToObject(r.data,"generated",true);cJSON_Hooks hooks={always_fail,free};cJSON_InitHooks(&hooks);send_key_generate_result(NULL,&r,"test",0);cJSON_InitHooks(NULL);assert(send_status==500&&find("new_priv"));
  const char *invalid[]={"{\"tscfg\":\"{}\\u0000tail\"}","{\"preview\":true,\"preview\":false}","{} {}","[]"};
  test_http_req_t native={0};req=(ts_http_request_t){.uri="/api/v1/automation/rules/import",.req=&native};
  for(unsigned i=0;i<sizeof invalid/sizeof *invalid;++i){req.body=invalid[i];req.body_len=native.content_len=strlen(req.body);assert(api_handler(&req,NULL)==ESP_OK&&send_status==400&&!rule_dispatch_calls);}
  char nul_body[]="{\"tscfg\":\"{}\0tail\"}";req.body=nul_body;req.body_len=native.content_len=sizeof nul_body-1;assert(api_handler(&req,NULL)==ESP_OK&&send_status==400&&!rule_dispatch_calls);
  req.body="{\"tscfg\":\"{}\"}";req.body_len=strlen(req.body);native.content_len=req.body_len+1;assert(api_handler(&req,NULL)==ESP_OK&&send_status==400&&!rule_dispatch_calls);
  native.content_len=req.body_len;native.query_len=10;assert(api_handler(&req,NULL)==ESP_OK&&send_status==400&&!rule_dispatch_calls);native.query_len=0;
  assert(api_handler(&req,NULL)==ESP_OK&&send_status==200&&rule_dispatch_calls==1);
 }
 ts_keystore_deinit();assert(!live_crypto);puts("PASS actual keystore regression");
}
'''

with tempfile.TemporaryDirectory(prefix='ts-keystore-') as tmp:
 p=Path(tmp);(p/'platform.h').write_text(platform);(p/'esp_err.h').write_text('#include "platform.h"\n');(p/'test.c').write_text(prefix+strict_json+body_complete+source+api_handler+web_handler+boundaries)
 env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
 subprocess.run(['cc','-std=c11','-g','-Wno-deprecated-declarations','-fsanitize=address,undefined','-I'+tmp,'-I'+str(root/'components/ts_security/include'),'-I'+str(root/'components/ts_api/include'),'-I'+str(idf/'components/json/cJSON'),str(p/'test.c'),str(idf/'components/json/cJSON/cJSON.c'),'-lm','-lpthread','-o',str(p/'test')],check=True,env=env)
 failures=[]
 for case in ['metadata','index','prefix','matrix','admission','concurrency','compatibility','api','allocations','lifecycle']:
  result=subprocess.run([str(p/'test'),case],env=env,capture_output=True,text=True)
  print(case,result.stdout or result.stderr.strip());
  if result.returncode:failures.append(case)
 if failures:raise SystemExit('FAILED: '+', '.join(failures))
