#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include "ts_rule_engine.h"
#include "ts_rule_codec.h"
#include "ts_action_manager.h"
#include "ts_ssh_service.h"
#include "../../components/ts_automation/src/ts_rule_store.h"
#define CONFIG_TS_AUTOMATION_MAX_RULES 4
#define pdTRUE 1
#define xSemaphoreTakeRecursive xSemaphoreTake
#define xSemaphoreGiveRecursive xSemaphoreGive
static int fail_alloc=-1, commits, action_calls;
static bool independent_service;
static pthread_mutex_t binding=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t control=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int block_phase, reached, proceed;
static char observed[64];
static int64_t esp_timer_get_time(void){return 1000000;}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps){if(fail_alloc==0)return NULL;if(fail_alloc>0)--fail_alloc;return calloc(n,size);}
void *heap_caps_malloc(size_t n,unsigned caps){return heap_caps_calloc(1,n,caps);}
void ts_ssh_binding_lock(void){pthread_mutex_lock(&binding);}
void ts_ssh_binding_unlock(void){pthread_mutex_unlock(&binding);}
bool ts_action_manager_accepting(void){return true;}
bool ts_ssh_service_rule_protected(const char *rule){return independent_service;}
void ts_action_snapshot_owner(const ts_auto_action_t *action,const char *rule){}
bool ts_ssh_service_any_in_use(void){return independent_service;}
bool ts_ssh_service_start_admissible(const char *id){return true;}
bool ts_ssh_service_command_protected(const char *id){return false;}
esp_err_t ts_action_template_get(const char *id,ts_action_template_t *out){return ESP_ERR_NOT_FOUND;}
esp_err_t ts_ssh_commands_config_get(const char *id,ts_ssh_command_config_t *out){return ESP_ERR_NOT_FOUND;}
static void barrier(int phase){pthread_mutex_lock(&control);if(block_phase==phase){reached=1;pthread_cond_broadcast(&changed);while(!proceed)pthread_cond_wait(&changed,&control);}pthread_mutex_unlock(&control);}
esp_err_t ts_variable_get(const char *id,ts_auto_value_t *out){barrier(1);out->type=TS_AUTO_VAL_BOOL;out->bool_val=true;return ESP_OK;}
esp_err_t ts_action_snapshot(const ts_auto_action_t *source,ts_auto_action_t *out){*out=*source;return ESP_OK;}
void ts_action_snapshot_release(ts_auto_action_t *out){}
esp_err_t ts_rule_store_commit(const ts_auto_rule_t *rules,int n,const ts_auto_rule_t *candidate,const char *id,int source,ts_rule_commit_t *result){++commits;*result=(ts_rule_commit_t){.applied=1,.durable=1,.mirror_synced=1,.error_code="ok"};return ESP_OK;}
static void record_execution(const char*a,ts_rule_exec_status_t b,ts_rule_trigger_source_t c,const char*d,uint8_t e,uint8_t f){}
static esp_err_t execute_actions_with_stats(const ts_auto_action_t*a,int n,int*ok,int*fail){++action_calls;barrier(2);strcpy(observed,a[0].log.message);*ok=n;*fail=0;return ESP_OK;}
#include "engine.inc"
static ts_auto_rule_t candidate(const char *message){ts_auto_rule_t r;const char *text="{\"id\":\"rule\",\"name\":\"test\",\"enabled\":true,\"manual_trigger\":false,\"show_on_dashboard\":false,\"allow_manual_trigger\":true,\"conditions\":[{\"variable\":\"host.ready\",\"value\":true}],\"actions\":[{\"type\":\"log\",\"message\":\"old\"}]}";cJSON*j=cJSON_Parse(text);assert(ts_rule_decode(j,&r)==ESP_OK);cJSON_Delete(j);strcpy(r.actions[0].log.message,message);return r;}
static void *evaluate(void*arg){bool triggered;esp_err_t ret=execute_rule("rule",false,&triggered);assert(ret==ESP_OK);return NULL;}
static pthread_t start_block(int phase){pthread_t t;pthread_mutex_lock(&control);block_phase=phase;reached=proceed=0;pthread_create(&t,NULL,evaluate,NULL);while(!reached)pthread_cond_wait(&changed,&control);pthread_mutex_unlock(&control);return t;}
static void unblock(pthread_t t){pthread_mutex_lock(&control);proceed=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&control);pthread_join(t,NULL);block_phase=0;}
int main(void){
 pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER,transaction=PTHREAD_MUTEX_INITIALIZER;
 s_rule_ctx=(ts_rule_engine_ctx_t){.rules=calloc(4,sizeof(ts_auto_rule_t)),.capacity=4,.initialized=true,.loaded=true,.mutex=&lock,.transaction=&transaction};
 ts_auto_rule_t a=candidate("old");ts_rule_commit_result_t result;assert(ts_rule_commit(&a,a.id,0,&result)==ESP_OK);assert(result.revision==1);
 int before=commits;assert(ts_rule_commit(&a,a.id,1,&result)==ESP_OK&&commits==before);assert(ts_rule_commit(&a,a.id,0,&result)!=ESP_OK&&commits==before);
 for(int n=0;n<3;n++){fail_alloc=n;assert(ts_rule_commit(&a,a.id,1,&result)!=ESP_OK);assert(s_rule_ctx.count==1&&s_rule_ctx.rules[0].revision==1);}fail_alloc=-1;
 pthread_t t=start_block(1);strcpy(a.actions[0].log.message,"new");assert(ts_rule_commit(&a,a.id,1,&result)==ESP_OK);assert(s_rule_ctx.retired==1);unblock(t);assert(action_calls==0&&s_rule_ctx.retired==0);
 t=start_block(2);strcpy(a.actions[0].log.message,"third");assert(ts_rule_commit(&a,a.id,2,&result)==ESP_OK);assert(s_rule_ctx.retired==1);ts_auto_rule_t held;assert(ts_rule_acquire(a.id,&held)==ESP_OK);strcpy(a.name,"bounded");assert(ts_rule_commit(&a,a.id,3,&result)!=ESP_OK);ts_rule_release(&held);assert(ts_rule_commit(NULL,a.id,3,&result)!=ESP_OK);unblock(t);assert(!strcmp(observed,"new")&&s_rule_ctx.retired==0&&s_rule_ctx.rules[0].trigger_count==1);
 for(int enabled=0;enabled<2;enabled++)for(int manual=0;manual<2;manual++)for(int allow=0;allow<2;allow++){
  a.enabled=enabled;a.manual_trigger=manual;a.allow_manual_trigger=allow;assert(ts_rule_commit(&a,a.id,s_rule_ctx.rules[0].revision,&result)==ESP_OK);
  before=action_calls;bool triggered;execute_rule(a.id,false,&triggered);assert((action_calls>before)==(enabled&&!manual));
  before=action_calls;execute_rule(a.id,true,&triggered);assert((action_calls>before)==(enabled&&allow));
 }
 uint32_t old_instance=s_rule_ctx.rules[0].instance;assert(ts_rule_commit(NULL,a.id,s_rule_ctx.rules[0].revision,&result)==ESP_OK);assert(ts_rule_commit(&a,a.id,0,&result)==ESP_OK);assert(s_rule_ctx.rules[0].instance!=old_instance&&s_rule_ctx.rules[0].trigger_count==0);
 strcpy(a.actions[0].template_id,"missing");
 assert(ts_rule_commit(&a,a.id,s_rule_ctx.rules[0].revision,&result)==ESP_OK);
 independent_service=true; /* action can be finished while remote run is still owned */
 assert(!s_rule_ctx.meta[0].executing);
 assert(ts_rule_commit(NULL,a.id,s_rule_ctx.rules[0].revision,&result)!=ESP_OK);
 a.actions[0].template_id[0]=0;
 assert(ts_rule_commit(&a,a.id,s_rule_ctx.rules[0].revision,&result)!=ESP_OK);
 independent_service=false;
 assert(ts_rule_commit(&a,a.id,s_rule_ctx.rules[0].revision,&result)==ESP_OK);
 strcpy(a.actions[0].template_id,"missing");
 assert(ts_rule_commit(&a,a.id,s_rule_ctx.rules[0].revision,&result)==ESP_OK);
 assert(ts_rule_commit(NULL,a.id,s_rule_ctx.rules[0].revision,&result)==ESP_OK);
 ts_rule_dispose(&a);free(s_rule_ctx.rules);
 puts("PASS actual rule engine: revisions/no-op/allocation failures, stale-evaluation barrier, pinned old execution, retirement bound, enable/manual/allow matrix, recreate identity");
}
