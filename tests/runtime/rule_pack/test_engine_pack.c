/* Production engine functions + real codec, crypto and complete-set storage.
 * Only hardware/RTOS, credentials and NVS are fixture adapters. */
#define RULE_ENGINE_INTEGRATION
#include "test_store_set.c"
#include <pthread.h>
#include <math.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "ts_rule_engine.h"
#include "ts_rule_pack.h"
#define pdTRUE 1
typedef struct {int code;char *message;cJSON *data;} ts_api_result_t;
#define TS_API_OK 0
#define TS_API_ERR_INTERNAL 500
#define TS_API_ERR_INVALID_ARG 400
static void ts_api_result_error(ts_api_result_t *r,int code,const char *why){r->code=code;r->message=strdup(why);}
typedef pthread_mutex_t *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffff
static int xSemaphoreTake(SemaphoreHandle_t m,unsigned timeout){return !(timeout?pthread_mutex_lock(m):pthread_mutex_trylock(m));}
static int xSemaphoreGive(SemaphoreHandle_t m){return !pthread_mutex_unlock(m);}
#define xSemaphoreTakeRecursive xSemaphoreTake
#define xSemaphoreGiveRecursive xSemaphoreGive
static pthread_mutex_t binding,lock,transaction;
static bool sd_mounted=true;
static unsigned action_calls,snapshot_calls,service_pins,connection_calls,queue_calls,variable_writes;
struct ts_ssh_session_s;
esp_err_t ts_ssh_connect(struct ts_ssh_session_s *session){++connection_calls;return ESP_FAIL;}
esp_err_t ts_action_queue(const ts_auto_action_t *a,ts_action_callback_t cb,void *data,uint8_t priority){++queue_calls;return ESP_FAIL;}
esp_err_t ts_variable_set(const char *id,const ts_auto_value_t *value){++variable_writes;return ESP_FAIL;}
static ts_auto_action_t admitted_action;
static void (*before_admission)(void);
static int64_t esp_timer_get_time(void){return 1000000;}
static const char *esp_err_to_name(int e){return "fixture";}
bool ts_action_manager_accepting(void){return true;}
bool ts_ssh_service_start_admissible(const char *id){return true;}
void ts_ssh_service_set_owner(const char *id,uint32_t r,const char *owner){assert(!"unexpected service owner");}
void ts_ssh_service_unpin(const char *id,uint32_t r){assert(!"unexpected service pin");}
esp_err_t ts_ssh_service_pin(const ts_ssh_command_config_t *c,const char *host,uint16_t port,uint32_t *r){++service_pins;return ESP_ERR_INVALID_STATE;}
esp_err_t ts_variable_get(const char *id,ts_auto_value_t *out){out->type=TS_AUTO_VAL_BOOL;out->bool_val=true;return ESP_OK;}
static void record_execution(const char *id,ts_rule_exec_status_t st,ts_rule_trigger_source_t source,const char *why,uint8_t count,uint8_t failed){}
static esp_err_t execute_actions_with_stats(const ts_auto_action_t *a,int n,int *ok,int *failed){action_calls+=n;admitted_action=a[0];*ok=n;*failed=0;return ESP_OK;}
void ts_ssh_binding_lock(void){assert(!pthread_mutex_lock(&binding));}
bool ts_ssh_binding_try_lock(void){return pthread_mutex_trylock(&binding)==0;}
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
static struct {bool initialized,templates_ready;atomic_int templates_load_result;SemaphoreHandle_t ssh_hosts_mutex;ts_action_ssh_host_t ssh_hosts[1];int ssh_host_count;} manager;
static __typeof__(manager) *s_ctx=&manager;
static unsigned template_load_calls,delay_calls,host_mutations,template_removals;
static bool template_binding_protected(const char *id,const ts_action_template_t *next){return false;}
static esp_err_t template_remove_impl(const char *id){++template_removals;template_exists=false;return ESP_OK;}
bool ts_ssh_service_host_protected(const char *id){return false;}
static esp_err_t register_ssh_host_impl(const ts_action_ssh_host_t *h){++host_mutations;internal_host=true;internal_valid=h->port!=0;return ESP_OK;}
static esp_err_t unregister_ssh_host_impl(const char *id){++host_mutations;internal_host=false;return ESP_OK;}
static esp_err_t templates_load_impl(void){++template_load_calls;templates_ready=true;template_state=ESP_OK;return ESP_OK;}
#define ts_action_get_ssh_host_ex host_get_impl
#define ts_action_snapshot snapshot_impl
#include "pack_snapshot.inc"
#undef ts_action_snapshot
#undef ts_action_get_ssh_host_ex
esp_err_t ts_action_get_ssh_host_ex(const char *id,ts_action_ssh_host_t *out,bool *internal){
 manager.ssh_hosts_mutex=&lock;manager.ssh_host_count=internal_host?1:0;
 manager.ssh_hosts[0]=(ts_action_ssh_host_t){.id="host",.host="192.0.2.2",.username="internal",.port=internal_valid?2222:0};
 return host_get_impl(id,out,internal);
}
esp_err_t ts_action_get_ssh_host(const char *id,ts_action_ssh_host_t *out){return ts_action_get_ssh_host_ex(id,out,NULL);}
#define pdMS_TO_TICKS(ms) (ms)
static void (*delay_hook)(void);
static void vTaskDelay(unsigned ms){assert(++delay_calls<=3);assert(ms==3000||ms==1000);if(ms==1000&&delay_hook)delay_hook();}
static void vTaskDelete(void *task){}
esp_err_t ts_action_snapshot(const ts_auto_action_t *src,ts_auto_action_t *out){
 ++snapshot_calls;if(before_admission){void (*hook)(void)=before_admission;before_admission=NULL;hook();}
 return snapshot_impl(src,out);
}
#include "pack_engine.inc"
static int engine_capacity=4;
static ts_auto_condition_t ready_condition={.variable="ready",.value={.type=TS_AUTO_VAL_BOOL,.bool_val=true}};
static void boot(void){
 for(int i=0;i<s_rule_ctx.count;++i)payload_free(s_rule_ctx.rules[i].lease);
 free(s_rule_ctx.rules);
 s_rule_ctx=(ts_rule_engine_ctx_t){.initialized=true,.capacity=engine_capacity,.mutex=&lock,.transaction=&transaction,.rules=calloc(engine_capacity,sizeof(ts_auto_rule_t))};
 assert(ts_rules_load()==ESP_OK);
}
static char *make_pack(const char *dir,const char *id,const char *name,int reference){
 free(device_key);free(device_cert);device_key=read_file(dir,"developer.key");device_cert=read_file(dir,"developer.pem");
 char *recipient=read_file(dir,"recipient.pem");
 ts_auto_rule_t r=ordinary(id,name);ts_auto_action_t action=*r.actions;r.actions=&action;
 r.revision=999; /* Exporter's revision is never target-device CAS. */
 if(reference==1)strcpy(action.template_id,"template");
 if(reference==2){action.type=TS_AUTO_ACT_SSH_CMD_REF;strcpy(action.ssh_ref.cmd_id,"command");}
 if(reference==3){action.type=TS_AUTO_ACT_SSH_CMD;strcpy(action.ssh.host_ref,"host");strcpy(action.ssh.command,"true");}
 if(reference==4){action.type=TS_AUTO_ACT_WEBHOOK;strcpy(action.webhook.url,"https://example.invalid/");}
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
static void seed_pair(void){
 reset_disk();ts_auto_rule_t a=ordinary("a","Old A"),b=ordinary("b","B");ts_rule_commit_t result;
 a.allow_manual_trigger=b.allow_manual_trigger=true;
 a.presentation_fields=b.presentation_fields=3;
 a.conditions.conditions=&ready_condition;a.conditions.count=1;
 assert(ts_rule_store_set_adopt(NULL,0,TS_RULE_SOURCE_NVS,NULL)==ESP_OK);
 assert(ts_rule_store_set_commit(&a,"a",0,0,NULL,0,NULL,&result)==ESP_OK);
 assert(ts_rule_store_set_commit(&b,"b",0,1,NULL,0,NULL,&result)==ESP_OK);boot();
}
static void confirm_delete(void){
 ts_rule_commit_result_t result;fail_selector_read=true;
 assert(ts_rule_commit(NULL,"a",1,&result)!=ESP_OK&&!strcmp(result.error_code,"commit_unknown"));
 fail_selector_read=false;assert(ts_rule_refresh_saved()==ESP_OK);
}
static void r1_cases(const char *dir){
 ts_auto_rule_t a=ordinary("a","A edit"),b=ordinary("b","B edit"),c=ordinary("c","C");ts_rule_commit_result_t result;
 char *raw=make_pack(dir,"a","Replacement",0);ts_auto_rule_t held;assert(ts_rule_acquire("a",&held)==ESP_OK);
 assert(ts_rule_trigger("a")==ESP_OK);unsigned submitted=action_calls;ts_auto_action_t prior=admitted_action;
 fail_selector_read=true;
 assert(ts_rule_commit(NULL,"a",1,&result)!=ESP_OK&&!strcmp(result.error_code,"commit_unknown"));
 assert(ts_rule_refresh_saved()!=ESP_OK&&s_rule_ctx.recovery_error&&!s_rule_ctx.meta[0].pending_delete);
 cJSON *unknown=cJSON_CreateObject();ts_rule_saved_status("a",unknown);
 assert(!strcmp(cJSON_GetObjectItem(unknown,"pending_change")->valuestring,"unknown")&&cJSON_IsNull(cJSON_GetObjectItem(unknown,"saved_exists")));cJSON_Delete(unknown);
 fail_selector_read=false;assert(ts_rule_refresh_saved()==ESP_OK&&ts_rule_refresh_saved()==ESP_OK);
 assert(!s_rule_ctx.recovery_error&&ts_rule_restart_pending()&&s_rule_ctx.count==2&&ts_rule_store_set_count()==1);
 assert(!strcmp(held.name,"Old A")&&!memcmp(&prior,&admitted_action,sizeof prior));
 ts_api_result_t view={0};assert(api_automation_rules_list(NULL,&view)==ESP_OK&&view.code==0);
 cJSON *rows=cJSON_GetObjectItem(view.data,"rules"),*first=cJSON_GetArrayItem(rows,0);
 assert(cJSON_GetArraySize(rows)==2&&!strcmp(cJSON_GetObjectItem(first,"pending_change")->valuestring,"delete"));
 assert(cJSON_IsTrue(cJSON_GetObjectItem(first,"runtime_active"))&&cJSON_IsFalse(cJSON_GetObjectItem(first,"saved_exists")));
 assert(cJSON_IsNull(cJSON_GetObjectItem(first,"saved_revision"))&&cJSON_IsNull(cJSON_GetObjectItem(first,"saved_source"))&&cJSON_IsNull(cJSON_GetObjectItem(first,"package_digest")));
 assert(cJSON_GetObjectItem(first,"active_revision")->valueint==1);cJSON_Delete(view.data);free(view.message);
 unsigned triggers=s_rule_ctx.rules[0].trigger_count;bool triggered=true;
 assert(ts_rule_trigger("a")==ESP_ERR_INVALID_STATE);assert(execute_rule("a",false,&triggered)==ESP_OK&&!triggered);
 assert(action_calls==submitted&&s_rule_ctx.rules[0].trigger_count==triggers);
 assert(ts_rule_commit(&a,"a",0,&result)!=ESP_OK&&!strcmp(result.error_code,"restart_pending"));
 assert(ts_rule_commit(&a,"a",1,&result)!=ESP_OK&&!strcmp(result.error_code,"restart_pending"));
 assert(ts_rule_commit(NULL,"a",1,&result)!=ESP_OK&&!strcmp(result.error_code,"restart_pending"));
 cJSON *data=NULL;const char *reason;assert(ts_rule_import_pack(raw,strlen(raw),true,false,0,0,0,NULL,&data,&reason)!=ESP_OK&&!strcmp(reason,"restart_pending"));
 ts_config_pack_t *decoded=NULL;ts_config_pack_acceptance_t accepted;uint32_t credential;
 assert(ts_config_pack_load_verified_mem(raw,strlen(raw),NULL,&decoded,&accepted,&credential)==TS_CONFIG_PACK_OK);ts_config_pack_free(decoded);
 assert(ts_rule_import_pack(raw,strlen(raw),false,true,0,ts_rule_store_set_generation(),credential,accepted.package_sha256,&data,&reason)!=ESP_OK&&!strcmp(reason,"restart_pending"));
 assert(ts_rule_commit(&c,"c",0,&result)!=ESP_OK&&!strcmp(result.error_code,"capacity"));
 char *newraw=make_pack(dir,"c","C pack",0);
 assert(ts_rule_import_pack(newraw,strlen(newraw),true,false,0,0,0,NULL,&data,&reason)!=ESP_OK&&!strcmp(reason,"capacity"));
 assert(ts_config_pack_load_verified_mem(newraw,strlen(newraw),NULL,&decoded,&accepted,&credential)==TS_CONFIG_PACK_OK);ts_config_pack_free(decoded);
 assert(ts_rule_import_pack(newraw,strlen(newraw),false,true,0,ts_rule_store_set_generation(),credential,accepted.package_sha256,&data,&reason)!=ESP_OK&&!strcmp(reason,"capacity"));
 assert(ts_rule_commit(&b,"b",1,&result)==ESP_OK);ts_rule_saved_info_t info;
 assert(ts_rule_store_set_info("a",&info)==ESP_ERR_NOT_FOUND&&ts_rule_store_set_info("b",&info)==ESP_OK&&info.revision==2&&info.source==TS_RULE_SET_NVS_JSON);
 ts_rule_release(&held);boot();assert(s_rule_ctx.count==1&&!strcmp(s_rule_ctx.rules[0].id,"b")&&!ts_rule_restart_pending());
 assert(ts_rule_commit(&c,"c",0,&result)==ESP_OK);free(raw);free(newraw);
 /* A2: known old selector and known successful deletion preserve old semantics. */
 seed_pair();fail_selector_write=true;assert(ts_rule_commit(NULL,"a",1,&result)!=ESP_OK&&!result.applied);fail_selector_write=false;
 s_rule_ctx.recovery_error=true;assert(ts_rule_refresh_saved()==ESP_OK&&!ts_rule_restart_pending()&&!s_rule_ctx.meta[0].pending_delete);
 assert(ts_rule_store_set_info("a",&info)==ESP_OK&&info.revision==1);
 assert(ts_rule_commit(NULL,"a",1,&result)==ESP_OK&&s_rule_ctx.count==1&&!ts_rule_restart_pending());
 assert(ts_rule_acquire("a",&held)==ESP_ERR_NOT_FOUND&&ts_rule_commit(&c,"c",0,&result)==ESP_OK);
 /* A3: deletion is published after snapshot preparation but before final admission. */
 seed_pair();submitted=action_calls;before_admission=confirm_delete;
 triggered=false;
 assert(execute_rule("a",false,&triggered)==ESP_OK&&!triggered&&action_calls==submitted);
 assert(s_rule_ctx.meta[0].pending_delete&&s_rule_ctx.rules[0].trigger_count==0);
 assert(ts_rule_trigger("b")==ESP_OK&&action_calls==submitted+1);boot();
 puts("PASS A1-A3: confirmed deletion, unknown/old selector, unchanged active lease/admitted action, final admission barrier, union capacity, sibling edit and reboot");
}
static void empty_runtime(void){
 for(int i=0;i<s_rule_ctx.count;++i)payload_free(s_rule_ctx.rules[i].lease);free(s_rule_ctx.rules);
 s_rule_ctx=(ts_rule_engine_ctx_t){.initialized=true,.capacity=engine_capacity,.mutex=&lock,.transaction=&transaction,.rules=calloc(engine_capacity,sizeof(ts_auto_rule_t))};
}
static void initialize_templates(void){assert(ts_action_templates_load()==ESP_OK);}
static void r2_cases(const char *dir){
 cJSON *params=cJSON_Parse("{\"id\":\"a\",\"expected_revision\":1}");ts_api_result_t reply={0};
 assert(api_automation_rules_enable(params,&reply)==ESP_OK&&reply.code==TS_API_OK);cJSON_Delete(params);cJSON_Delete(reply.data);free(reply.message);
 assert(ts_rules_load()==ESP_OK&&s_rule_ctx.loaded&&s_rule_ctx.count==2);
 assert(s_rule_ctx.rules[0].enabled&&s_rule_ctx.rules[0].revision==2&&s_rule_ctx.meta[0].source==TS_RULE_SET_NVS_JSON);
 assert(!strcmp(s_rule_ctx.rules[0].actions[0].template_id,"template"));unsigned actions=action_calls;
 assert(ts_rule_trigger("b")==ESP_OK&&action_calls==actions+1);assert(ts_rule_trigger("a")!=ESP_OK&&action_calls==actions+1);
 /* Actual deferred entry has no unconditional template wait; actual initial load guard permits ordinary refs. */
 templates_ready=false;template_enabled=true;empty_runtime();delay_calls=0;ts_rule_deferred_load_task(NULL);
 assert(delay_calls==1&&s_rule_ctx.loaded&&s_rule_ctx.count==2);assert(ts_action_templates_load()==ESP_OK&&template_load_calls==1&&manager.templates_load_result==ESP_OK);
 assert(ts_rule_trigger("a")==ESP_OK&&action_calls==actions+2);
 /* No-template package is independent of template component failure. */
 char *raw=make_pack(dir,"plain-pack","Log pack",0);cJSON *pre=preview(raw),*data=NULL;const char *reason;
 assert(save(raw,pre,&reason,&data)==ESP_OK);cJSON_Delete(pre);cJSON_Delete(data);free(raw);
 templates_ready=false;template_state=ESP_FAIL;empty_runtime();delay_calls=0;ts_rule_deferred_load_task(NULL);
 assert(delay_calls==1&&s_rule_ctx.loaded&&s_rule_ctx.count==3&&s_rule_ctx.meta[2].source==TS_RULE_SET_SD_PACK);
 /* A true package dependency blocks complete target publication; real bootstrap guard breaks the initialization cycle. */
 templates_ready=true;template_state=ESP_OK;raw=make_pack(dir,"templated-pack","Package with T",1);pre=preview(raw);data=NULL;
 assert(save(raw,pre,&reason,&data)==ESP_OK);cJSON_Delete(data);cJSON_Delete(pre);free(raw);
 empty_runtime();templates_ready=false;template_state=ESP_FAIL;
 assert(ts_rules_load()!=ESP_OK&&!s_rule_ctx.loaded&&s_rule_ctx.count==0&&!strcmp(ts_rule_load_error(),"dependency_unavailable"));
 template_state=ESP_OK;assert(ts_rules_load()!=ESP_OK&&!strcmp(ts_rule_load_error(),"dependency_loading"));
 delay_calls=0;delay_hook=initialize_templates;ts_rule_deferred_load_task(NULL);delay_hook=NULL;
 assert(delay_calls==2&&s_rule_ctx.loaded&&s_rule_ctx.count==4&&template_load_calls==2);
 assert(s_rule_ctx.meta[0].source==TS_RULE_SET_NVS_JSON&&s_rule_ctx.meta[3].source==TS_RULE_SET_SD_PACK);
 unsigned loads=template_load_calls;assert(ts_action_templates_load()!=ESP_OK&&template_load_calls==loads);
 /* Old ordinary source is not relabeled by its pending package replacement. */
 seed_pair();raw=make_pack(dir,"a","A pending",0);pre=preview(raw);data=NULL;
 assert(save(raw,pre,&reason,&data)==ESP_OK);cJSON_Delete(data);cJSON_Delete(pre);free(raw);
 assert(s_rule_ctx.meta[0].source==TS_RULE_SET_NVS_JSON&&ts_rule_dependency_change(TS_RULE_DEP_TEMPLATE,"template",NULL)==ESP_OK);
 boot();assert(s_rule_ctx.meta[0].source==TS_RULE_SET_SD_PACK);
 puts("PASS B1-B3: actual enable/execute, ordinary source compatibility, bounded deferred loading, first template load, mixed complete publication and published source identity");
}
static void reject_pack(const char *raw,const char *wanted){
 cJSON *data=NULL;const char *reason;ts_config_pack_t *pack=NULL;ts_config_pack_acceptance_t accepted;uint32_t credential;
 assert(ts_rule_import_pack(raw,strlen(raw),true,false,0,0,0,NULL,&data,&reason)!=ESP_OK&&!strcmp(reason,wanted));cJSON_Delete(data);data=NULL;
 assert(ts_config_pack_load_verified_mem(raw,strlen(raw),NULL,&pack,&accepted,&credential)==TS_CONFIG_PACK_OK);
 ts_auto_rule_t decoded={0};assert(ts_rule_pack_decode(pack->content,pack->content_len,&decoded)==ESP_OK);
 ts_rule_saved_info_t info={0};bool exists=ts_rule_store_set_info(decoded.id,&info)==ESP_OK;ts_rule_dispose(&decoded);ts_config_pack_free(pack);
 assert(ts_rule_import_pack(raw,strlen(raw),false,true,exists?info.revision:0,ts_rule_store_set_generation(),credential,accepted.package_sha256,&data,&reason)!=ESP_OK&&!strcmp(reason,wanted));cJSON_Delete(data);
}
static void r3_cases(const char *dir,bool webhook){
 char *raw=make_pack(dir,"probe","Probe",webhook?4:3);cJSON *data=NULL,*pre=NULL;const char *reason=NULL;
 unsigned snapshots=snapshot_calls,pins=service_pins,actions=action_calls;
 assert(ts_rule_import_pack(raw,strlen(raw),true,false,0,0,0,NULL,&data,&reason)!=ESP_OK&&!strcmp(reason,webhook?"action_unsupported":"dependency_missing"));cJSON_Delete(data);data=NULL;
 ts_config_pack_t *decoded=NULL;ts_config_pack_acceptance_t accepted;uint32_t credential;
 assert(ts_config_pack_load_verified_mem(raw,strlen(raw),NULL,&decoded,&accepted,&credential)==TS_CONFIG_PACK_OK);ts_config_pack_free(decoded);
 assert(ts_rule_import_pack(raw,strlen(raw),false,true,0,ts_rule_store_set_generation(),credential,accepted.package_sha256,&data,&reason)!=ESP_OK&&!strcmp(reason,webhook?"action_unsupported":"dependency_missing"));cJSON_Delete(data);data=NULL;
 ts_auto_rule_t check=ordinary("check","Check");ts_auto_action_t direct={.type=TS_AUTO_ACT_SSH_CMD};strcpy(direct.ssh.host_ref,"host");strcpy(direct.ssh.command,"true");check.actions=&direct;
 if(webhook){
  direct.type=TS_AUTO_ACT_WEBHOOK;check.enabled=false;assert(ts_rule_pack_dependencies(&check,&reason,NULL)!=ESP_OK&&!strcmp(reason,"action_unsupported"));
  direct.type=TS_AUTO_ACT_LOG;strcpy(direct.template_id,"template");template_action.type=TS_AUTO_ACT_WEBHOOK;
  assert(ts_rule_pack_dependencies(&check,&reason,NULL)!=ESP_OK&&!strcmp(reason,"action_unsupported"));
  char *unsupported=make_pack(dir,"template-webhook","Unsupported template",1);reject_pack(unsupported,"action_unsupported");free(unsupported);
  template_action.type=TS_AUTO_ACT_LOG;char *templated=make_pack(dir,"probe","Template package",1);pre=preview(templated);
  assert(save(templated,pre,&reason,&data)==ESP_OK);cJSON_Delete(data);cJSON_Delete(pre);free(templated);
  ts_action_template_delete_result_t deletion;
  assert(ts_action_template_remove_checked("template",&deletion)!=ESP_OK&&template_removals==0&&!deletion.references_confirmed);free(deletion.rules);
  ts_action_template_t next={.enabled=true,.action={.type=TS_AUTO_ACT_WEBHOOK}};
  assert(ts_rule_dependency_change(TS_RULE_DEP_TEMPLATE,"template",&next)!=ESP_OK&&template_action.type==TS_AUTO_ACT_LOG);
  boot();assert(ts_rule_dependency_change(TS_RULE_DEP_TEMPLATE,"template",&next)!=ESP_OK);
  assert(ts_action_template_remove_checked("template",&deletion)!=ESP_OK&&template_removals==0&&deletion.references_confirmed&&deletion.rule_count==1);free(deletion.rules);
  /* Ordinary legacy Webhook remains load-compatible. */
  seed_pair();ts_rule_commit_result_t result;ts_auto_rule_t old=ordinary("a","Old Webhook");old.actions=&direct;direct.template_id[0]=0;direct.type=TS_AUTO_ACT_WEBHOOK;
  assert(ts_rule_commit(&old,"a",1,&result)==ESP_OK);templates_ready=false;assert(ts_rules_load()==ESP_OK&&s_rule_ctx.rules[0].actions[0].type==TS_AUTO_ACT_WEBHOOK);
 }else{
  host_exists=true;command_state=ESP_ERR_INVALID_STATE;pre=preview(raw);assert(save(raw,pre,&reason,&data)==ESP_OK);cJSON_Delete(pre);cJSON_Delete(data);
  ts_ssh_host_config_t proposed;assert(ts_ssh_hosts_config_get("host",&proposed)==ESP_OK);proposed.port=0;
  for(int stage=0;stage<2;++stage){
   if(stage)boot();assert(ts_rule_dependency_change(TS_RULE_DEP_HOST,"host",NULL)!=ESP_OK);
   assert(ts_rule_dependency_change(TS_RULE_DEP_HOST,"host",&proposed)!=ESP_OK&&config_valid&&host_exists);
   internal_host=true;ts_action_ssh_host_t selected;bool internal=false;
   assert(ts_action_get_ssh_host_ex("host",&selected,&internal)==ESP_OK&&internal&&selected.port==2222);
   host_state=ESP_ERR_INVALID_STATE;assert(ts_rule_pack_dependencies(&check,&reason,NULL)==ESP_OK);
   assert(ts_rule_dependency_change(TS_RULE_DEP_HOST,"host",&proposed)==ESP_OK); /* shadowed config */
   assert(ts_rule_dependency_change(TS_RULE_DEP_ACTION_HOST,"host",NULL)!=ESP_OK);host_state=ESP_OK;
   config_valid=false;assert(ts_rule_dependency_change(TS_RULE_DEP_ACTION_HOST,"host",NULL)!=ESP_OK);config_valid=true;
   assert(ts_rule_dependency_change(TS_RULE_DEP_ACTION_HOST,"host",NULL)==ESP_OK);
   config_read_error=ESP_FAIL;assert(ts_rule_dependency_change(TS_RULE_DEP_ACTION_HOST,"host",NULL)!=ESP_OK);config_read_error=ESP_OK;
   unsigned mutations=host_mutations;config_valid=false;
   assert(ts_action_unregister_ssh_host("host")!=ESP_OK&&internal_host&&host_mutations==mutations);config_valid=true;
   assert(ts_action_unregister_ssh_host("host")==ESP_OK&&!internal_host&&host_mutations==mutations+1);
   selected.port=0;assert(ts_action_register_ssh_host(&selected)!=ESP_OK&&!internal_host&&host_mutations==mutations+1);
   assert(ts_rule_dependency_change(TS_RULE_DEP_HOST,"unrelated",NULL)==ESP_OK);
  }
  /* Effective template wins over stale inline fields; template SSH uses same direct path. */
  strcpy(direct.template_id,"template");host_exists=false;template_action.type=TS_AUTO_ACT_LOG;
  assert(ts_rule_pack_dependencies(&check,&reason,NULL)==ESP_OK);
  template_action=direct;template_action.template_id[0]=0;
  assert(ts_rule_pack_dependencies(&check,&reason,NULL)!=ESP_OK&&!strcmp(reason,"dependency_missing"));
  char *template_ssh=make_pack(dir,"probe","Template SSH",1);reject_pack(template_ssh,"dependency_missing");host_exists=true;
  assert(ts_rule_pack_dependencies(&check,&reason,NULL)==ESP_OK);
  template_enabled=false;reject_pack(template_ssh,"dependency_disabled");template_enabled=true;
  pre=preview(template_ssh);data=NULL;assert(save(template_ssh,pre,&reason,&data)==ESP_OK);cJSON_Delete(pre);cJSON_Delete(data);free(template_ssh);
  /* Retained package and pending package can protect different effective references. */
  template_action.type=TS_AUTO_ACT_LOG;command_state=ESP_OK;char *templated=make_pack(dir,"probe","Pending template",1);pre=preview(templated);data=NULL;
  assert(save(templated,pre,&reason,&data)==ESP_OK);cJSON_Delete(pre);cJSON_Delete(data);free(templated);
  assert(ts_rule_dependency_change(TS_RULE_DEP_HOST,"host",NULL)!=ESP_OK); /* old running direct SSH */
  ts_action_template_t next={.enabled=true,.action=direct};next.action.template_id[0]=0;strcpy(next.action.ssh.host_ref,"missing");
  assert(ts_rule_dependency_change(TS_RULE_DEP_TEMPLATE,"template",&next)!=ESP_OK&&template_action.type==TS_AUTO_ACT_LOG);
  boot();assert(ts_rule_dependency_change(TS_RULE_DEP_HOST,"host",NULL)==ESP_OK);
 }
 assert(snapshot_calls==snapshots&&service_pins==pins&&action_calls==actions&&connection_calls==0&&queue_calls==0&&variable_writes==0);free(raw);
 puts(webhook?"PASS C3: direct/template/disabled Webhook refused, protected template unchanged, ordinary compatibility, no snapshot/pin/execute side effects":"PASS C1-C2: direct SSH preview/formal, real host source priority, proposed view/fallback fields and readiness, effective template, active/saved source protection; no snapshot/pin/execute");
}
int main(int argc,char **argv){
 assert(argc==2||argc==3);const char *dir=argv[1];pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
 pthread_mutex_init(&binding,&attr);pthread_mutex_init(&transaction,&attr);pthread_mutex_init(&lock,NULL);pthread_mutexattr_destroy(&attr);
 ca=read_file(dir,"root.pem");device_key=read_file(dir,"recipient.key");device_cert=read_file(dir,"recipient.pem");assert(ts_config_pack_init()==ESP_OK);
 reset_disk();ts_auto_rule_t a=ordinary("a","Old A"),b=ordinary("b","B");ts_rule_commit_t stored;
 a.allow_manual_trigger=b.allow_manual_trigger=true;
 a.presentation_fields=b.presentation_fields=3;
 if(argc==3&&!strcmp(argv[2],"r1")){engine_capacity=2;a.conditions.conditions=&ready_condition;a.conditions.count=1;}
 if(argc==3&&!strcmp(argv[2],"r2")){
  static ts_auto_action_t templated={.type=TS_AUTO_ACT_LOG};strcpy(templated.template_id,"template");a.actions=&templated;a.enabled=false;template_enabled=false;
 }
 assert(ts_rule_store_set_adopt(NULL,0,TS_RULE_SOURCE_NVS,NULL)==ESP_OK);
 assert(ts_rule_store_set_commit(&a,"a",0,0,NULL,0,NULL,&stored)==ESP_OK);
 assert(ts_rule_store_set_commit(&b,"b",0,1,NULL,0,NULL,&stored)==ESP_OK);boot();
 if(argc==3){
  if(!strcmp(argv[2],"r1")){
   r1_cases(dir);
  }else if(!strcmp(argv[2],"r2")){
   r2_cases(dir);
  }else{
   bool webhook=!strcmp(argv[2],"r3_webhook");assert(webhook||!strcmp(argv[2],"r3_ssh"));r3_cases(dir,webhook);
  }
  assert(ts_rule_engine_deinit()==ESP_OK);ts_rule_store_set_reset();reset_disk();free(device_key);free(device_cert);free(ca);return 0;
 }
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
