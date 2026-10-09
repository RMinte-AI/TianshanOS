/* Production engine functions + real codec, crypto and complete-set storage.
 * Only hardware/RTOS, credentials and NVS are fixture adapters. */
#define RULE_ENGINE_INTEGRATION
#include "test_store_set.c"
#include <pthread.h>
#include "ts_rule_engine.h"
#include "ts_rule_pack.h"
#define pdTRUE 1
typedef struct {int code;char *message;cJSON *data;} ts_api_result_t;
#define TS_API_OK 0
#define TS_API_ERR_INTERNAL 500
static void ts_api_result_error(ts_api_result_t *r,int code,const char *why){r->code=code;r->message=strdup(why);}
typedef pthread_mutex_t *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffff
static int xSemaphoreTake(SemaphoreHandle_t m,unsigned timeout){return !pthread_mutex_lock(m);}
static int xSemaphoreGive(SemaphoreHandle_t m){return !pthread_mutex_unlock(m);}
#define xSemaphoreTakeRecursive xSemaphoreTake
#define xSemaphoreGiveRecursive xSemaphoreGive
static pthread_mutex_t binding,lock,transaction;
static bool sd_mounted=true;
void ts_ssh_binding_lock(void){assert(!pthread_mutex_lock(&binding));}
void ts_ssh_binding_unlock(void){assert(!pthread_mutex_unlock(&binding));}
bool ts_storage_sd_mounted(void){return sd_mounted;}
bool ts_ssh_service_rule_protected(const char *id){return false;}
bool ts_ssh_service_command_protected(const char *id){return false;}
bool ts_ssh_service_any_in_use(void){return false;}
bool ts_cert_material_begin(uint32_t wanted){return wanted==generation;}
void ts_cert_material_end(void){}
esp_err_t ts_rule_store_commit(const ts_auto_rule_t *r,int n,const ts_auto_rule_t *c,const char *id,int source,ts_rule_commit_t *out){assert(!"legacy writer must not run");return ESP_FAIL;}
esp_err_t ts_rule_store_recover(bool sd,bool *stage,bool *required){assert(!"selected-set load must not fall back to legacy");return ESP_FAIL;}
esp_err_t ts_rule_store_load_bank(ts_auto_rule_t *r,int cap,int *count){assert(0);return ESP_FAIL;}
static esp_err_t load_legacy_file(const char *p,ts_auto_rule_t *r,int *n){assert(0);return ESP_FAIL;}
static esp_err_t load_legacy_nvs(ts_auto_rule_t *r,int *n){assert(0);return ESP_FAIL;}
static esp_err_t load_directory(ts_auto_rule_t *r,int *n,bool *ro){assert(0);return ESP_FAIL;}
#define RULES_SDCARD_DIR DIR_RULES
static int find_rule_index(const char *id);
#include "pack_engine.inc"
static void boot(void){
 for(int i=0;i<s_rule_ctx.count;++i)payload_free(s_rule_ctx.rules[i].lease);
 free(s_rule_ctx.rules);
 s_rule_ctx=(ts_rule_engine_ctx_t){.initialized=true,.capacity=4,.mutex=&lock,.transaction=&transaction,.rules=calloc(4,sizeof(ts_auto_rule_t))};
 assert(ts_rules_load()==ESP_OK);
}
static char *make_pack(const char *dir,const char *id,const char *name,int reference){
 free(device_key);free(device_cert);device_key=read_file(dir,"developer.key");device_cert=read_file(dir,"developer.pem");
 char *recipient=read_file(dir,"recipient.pem");
 ts_auto_rule_t r=ordinary(id,name);ts_auto_action_t action=*r.actions;r.actions=&action;
 r.revision=999; /* Exporter's revision is never target-device CAS. */
 if(reference==1)strcpy(action.template_id,"template");
 if(reference==2){action.type=TS_AUTO_ACT_SSH_CMD_REF;strcpy(action.ssh_ref.cmd_id,"command");}
 cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"type","automation_rule");cJSON_AddItemToObject(j,"rule",ts_rule_encode(&r));
 char *plain=cJSON_PrintUnformatted(j),*raw=NULL;size_t n=0;cJSON_Delete(j);
 ts_config_pack_export_opts_t opts={.recipient_cert_pem=recipient,.recipient_cert_len=strlen(recipient)};
 assert(ts_config_pack_create("metadata is not identity",plain,strlen(plain),&opts,&raw,&n)==TS_CONFIG_PACK_OK);
 free(plain);free(device_key);free(device_cert);device_key=read_file(dir,"recipient.key");device_cert=recipient;return raw;
}
static cJSON *preview(const char *raw){
 cJSON *data=NULL;const char *reason;
 assert(ts_rule_import_pack(raw,strlen(raw),true,false,0,0,0,NULL,&data,&reason)==ESP_OK);
 assert(!strcmp(reason,"ok")&&cJSON_IsTrue(cJSON_GetObjectItem(data,"trusted")));
 char *serialized=cJSON_PrintUnformatted(data);cJSON *wire=ts_config_pack_parse_import_request(serialized,strlen(serialized));assert(wire);cJSON_Delete(wire);free(serialized);return data;
}
static esp_err_t save(const char *raw,const cJSON *pre,const char **reason,cJSON **data){
 return ts_rule_import_pack(raw,strlen(raw),false,true,
  cJSON_GetObjectItem(pre,"expected_revision")->valueint,cJSON_GetObjectItem(pre,"expected_generation")->valueint,
  cJSON_GetObjectItem(pre,"credential_generation")->valueint,cJSON_GetObjectItem(pre,"package_digest")->valuestring,data,reason);
}
int main(int argc,char **argv){
 assert(argc==2);const char *dir=argv[1];pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
 pthread_mutex_init(&binding,&attr);pthread_mutex_init(&transaction,&attr);pthread_mutex_init(&lock,NULL);pthread_mutexattr_destroy(&attr);
 ca=read_file(dir,"root.pem");device_key=read_file(dir,"recipient.key");device_cert=read_file(dir,"recipient.pem");assert(ts_config_pack_init()==ESP_OK);
 reset_disk();ts_auto_rule_t a=ordinary("a","Old A"),b=ordinary("b","B");ts_rule_commit_t stored;
 assert(ts_rule_store_set_adopt(NULL,0,TS_RULE_SOURCE_NVS,NULL)==ESP_OK);
 assert(ts_rule_store_set_commit(&a,"a",0,0,NULL,0,NULL,&stored)==ESP_OK);
 assert(ts_rule_store_set_commit(&b,"b",0,1,NULL,0,NULL,&stored)==ESP_OK);boot();
 char *raw=make_pack(dir,"a","Imported A",true);cJSON *pre=preview(raw),*data=NULL;const char *reason;
 templates_ready=false;assert(ts_rule_import_pack(raw,strlen(raw),true,false,0,0,0,NULL,&data,&reason)!=ESP_OK&&!strcmp(reason,"dependency_loading"));cJSON_Delete(data);data=NULL;templates_ready=true;
 template_enabled=false;assert(ts_rule_import_pack(raw,strlen(raw),true,false,0,0,0,NULL,&data,&reason)!=ESP_OK&&!strcmp(reason,"dependency_disabled"));cJSON_Delete(data);data=NULL;template_enabled=true;
 ts_auto_rule_t held;assert(ts_rule_acquire("a",&held)==ESP_OK);
 fail_selector_read=true;
 assert(save(raw,pre,&reason,&data)!=ESP_OK&&!strcmp(reason,"commit_unknown"));
 assert(s_rule_ctx.recovery_error&&!strcmp(s_rule_ctx.rules[0].name,"Old A"));
 cJSON_Delete(data);data=NULL;fail_selector_read=false;
 assert(ts_rule_refresh_saved()==ESP_OK&&!s_rule_ctx.recovery_error&&ts_rule_restart_pending());
 ts_api_result_t view={0};assert(api_automation_rules_list(NULL,&view)==ESP_OK&&view.code==TS_API_OK);
 cJSON *rows=cJSON_GetObjectItem(view.data,"rules");assert(cJSON_GetArraySize(rows)==2);
 cJSON *first=cJSON_GetArrayItem(rows,0);assert(cJSON_GetObjectItem(first,"revision")->valueint==1&&cJSON_GetObjectItem(first,"saved_revision")->valueint==2&&cJSON_IsTrue(cJSON_GetObjectItem(first,"restart_required")));
 assert(!strcmp(cJSON_GetObjectItem(first,"package_digest")->valuestring,cJSON_GetObjectItem(pre,"package_digest")->valuestring));
 cJSON_Delete(view.data);free(view.message);
 assert(save(raw,pre,&reason,&data)==ESP_OK);assert(cJSON_IsFalse(cJSON_GetObjectItem(data,"runtime_applied")));cJSON_Delete(data);data=NULL;
 assert(!strcmp(s_rule_ctx.rules[0].name,"Old A")&&s_rule_ctx.rules[0].revision==1&&!strcmp(held.name,"Old A"));
 assert(ts_rule_restart_pending());ts_rule_commit_result_t result;
 assert(ts_rule_commit(&a,"a",1,&result)!=ESP_OK&&!strcmp(result.error_code,"restart_pending"));
 assert(ts_rule_commit(NULL,"a",1,&result)!=ESP_OK&&!strcmp(result.error_code,"restart_pending"));
 assert(ts_rules_load()!=ESP_OK&&ts_rules_load_from_file("anything")!=ESP_OK&&ts_rule_engine_deinit()!=ESP_OK);
 ts_rule_release(&held);strcpy(b.name,"Edited B");assert(ts_rule_commit(&b,"b",1,&result)==ESP_OK&&s_rule_ctx.rules[1].revision==2);
 assert(ts_rule_dependency_change(TS_RULE_DEP_TEMPLATE,"template",NULL)!=ESP_OK);
 ts_action_template_t tpl={.enabled=false,.action={.type=TS_AUTO_ACT_LOG}};assert(ts_rule_dependency_change(TS_RULE_DEP_TEMPLATE,"template",&tpl)!=ESP_OK);tpl.enabled=true;assert(ts_rule_dependency_change(TS_RULE_DEP_TEMPLATE,"template",&tpl)==ESP_OK);
 trusted_time=false;assert(save(raw,pre,&reason,&data)==ESP_OK&&!strcmp(reason,"no_change"));cJSON_Delete(data);data=NULL;trusted_time=true;cJSON_Delete(pre);
 command_exists=host_exists=true;
 char *newraw=make_pack(dir,"new","New",2);pre=preview(newraw);
 ++generation;assert(save(newraw,pre,&reason,&data)!=ESP_OK&&!strcmp(reason,"credential_changed"));cJSON_Delete(data);data=NULL;--generation;
 strcpy(b.name,"B changed after preview");assert(ts_rule_commit(&b,"b",2,&result)==ESP_OK);
 assert(save(newraw,pre,&reason,&data)!=ESP_OK&&!strcmp(reason,"revision_conflict"));cJSON_Delete(data);data=NULL;cJSON_Delete(pre);pre=preview(newraw);
 assert(save(newraw,pre,&reason,&data)==ESP_OK);cJSON_Delete(data);data=NULL;cJSON_Delete(pre);
 assert(s_rule_ctx.count==2&&ts_rule_store_set_count()==3);
 assert(ts_rule_dependency_change(TS_RULE_DEP_COMMAND,"command",NULL)!=ESP_OK);
 ts_ssh_command_config_t cmd;assert(ts_ssh_commands_config_get("command",&cmd)==ESP_OK);cmd.enabled=false;
 assert(ts_rule_dependency_change(TS_RULE_DEP_COMMAND,"command",&cmd)!=ESP_OK);cmd.enabled=true;
 strcpy(cmd.host_id,"missing");assert(ts_rule_dependency_change(TS_RULE_DEP_COMMAND,"command",&cmd)!=ESP_OK);strcpy(cmd.host_id,"host");
 assert(ts_rule_dependency_change(TS_RULE_DEP_COMMAND,"command",&cmd)==ESP_OK);
 assert(ts_rule_dependency_change(TS_RULE_DEP_HOST,"host",NULL)!=ESP_OK);
 internal_host=true;assert(ts_rule_dependency_change(TS_RULE_DEP_HOST,"host",NULL)==ESP_OK);internal_host=false;
 /* Dependency preflight follows template priority and explicit readiness. */
 ts_auto_action_t ref={.type=TS_AUTO_ACT_SSH_CMD_REF};strcpy(ref.ssh_ref.cmd_id,"command");
 ts_auto_rule_t check=ordinary("check","Check");check.actions=&ref;cJSON *warnings=NULL;
 command_state=ESP_ERR_INVALID_STATE;assert(ts_rule_pack_dependencies(&check,&reason,NULL)!=ESP_OK&&!strcmp(reason,"dependency_loading"));
 command_state=ESP_FAIL;assert(ts_rule_pack_dependencies(&check,&reason,NULL)!=ESP_OK&&!strcmp(reason,"dependency_unavailable"));command_state=ESP_OK;
 host_state=ESP_ERR_INVALID_STATE;assert(ts_rule_pack_dependencies(&check,&reason,NULL)!=ESP_OK&&!strcmp(reason,"dependency_loading"));
 internal_host=true;assert(ts_rule_pack_dependencies(&check,&reason,NULL)==ESP_OK);internal_host=false;host_state=ESP_OK;
 command_enabled=false;assert(ts_rule_pack_dependencies(&check,&reason,NULL)!=ESP_OK&&!strcmp(reason,"dependency_disabled"));
 check.enabled=false;assert(ts_rule_pack_dependencies(&check,&reason,&warnings)==ESP_OK&&cJSON_GetArraySize(warnings)==1);cJSON_Delete(warnings);command_enabled=true;
 check.enabled=true;strcpy(ref.template_id,"template");command_exists=false;assert(ts_rule_pack_dependencies(&check,&reason,NULL)==ESP_OK);command_exists=true;
 cJSON *pending=ts_rule_pending_list();assert(cJSON_GetArraySize(pending)==1&&!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(pending,0),"id")->valuestring,"new"));cJSON_Delete(pending);
 ts_auto_rule_t c=ordinary("c","C");assert(ts_rule_commit(&c,"c",0,&result)==ESP_OK);
 char *aba=make_pack(dir,"c","ABA C",0);pre=preview(aba);
 assert(ts_rule_commit(NULL,"c",1,&result)==ESP_OK&&ts_rule_commit(&c,"c",0,&result)==ESP_OK);
 assert(save(aba,pre,&reason,&data)!=ESP_OK&&!strcmp(reason,"revision_conflict"));cJSON_Delete(data);data=NULL;cJSON_Delete(pre);free(aba);
 ts_auto_rule_t d=ordinary("d","D");assert(ts_rule_commit(&d,"d",0,&result)!=ESP_OK&&!strcmp(result.error_code,"capacity"));
 trusted_time=false;boot();assert(!ts_rule_restart_pending()&&s_rule_ctx.count==4&&!strcmp(s_rule_ctx.rules[0].name,"Imported A"));
 assert(ts_rule_dependency_change(TS_RULE_DEP_TEMPLATE,"template",NULL)!=ESP_OK);
 sd_mounted=false;assert(ts_rules_load()!=ESP_OK&&!s_rule_ctx.loaded&&s_rule_ctx.recovery_error);sd_mounted=true;assert(ts_rules_load()==ESP_OK&&s_rule_ctx.count==4);
 assert(ts_rule_engine_deinit()==ESP_OK);ts_rule_store_set_reset();reset_disk();
 free(raw);free(newraw);free(device_key);free(device_cert);free(ca);
 pthread_mutex_destroy(&binding);pthread_mutex_destroy(&transaction);pthread_mutex_destroy(&lock);
 puts("PASS production import + engine + crypto + store: saved/active isolation, held old lease, pending edit/reload/deinit guards, unrelated merge, CAS/credential conflicts, offline idempotence/boot, dependency union, capacity and missing SD recovery");
}
