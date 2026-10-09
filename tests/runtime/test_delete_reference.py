"""Production deletion/commit gates and HTTP bytes; synthetic storage, no device access."""
from pathlib import Path
import json, os, re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
os.chdir(root)
idf=Path(os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf'))
def extract(file,name):
 s=Path(file).read_text();m=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',s,re.M);assert m,name
 return s[m.start():s.index('\n}',m.start())+2]+'\n'
engine=Path('components/ts_automation/src/ts_rule_engine.c').read_text()
a=engine.index('typedef struct {\n    ts_auto_rule_t *rules;');b=engine.index('static void payload_free',a)
parts=[engine[a:b]]
for name in ['find_rule_index','payload_free','payload_adopt','ts_rule_resolve_presentation','ts_rule_acquire','ts_rule_release','same_config','protected_bindings','validate_new_template_refs','ts_rule_commit','compare_values','ts_rule_eval_condition','ts_rule_eval_condition_group','execute_rule','ts_rule_get_by_index','ts_rule_count','ts_rule_config_status']:
 parts.append(extract('components/ts_automation/src/ts_rule_engine.c',name))
prefix=Path('tests/runtime/test_engine.c').read_text().split('int main(void)')[0]
prefix=prefix.replace('#include "../../components/ts_automation/src/ts_rule_store.h"', '#include "'+str(root/'components/ts_automation/src/ts_rule_store.h')+'"')
prefix=prefix.replace('#define CONFIG_TS_AUTOMATION_MAX_RULES 4','#define CONFIG_TS_AUTOMATION_MAX_RULES 32')
prefix=prefix.replace('#include "engine.inc"','\n'.join(parts))
prefix=prefix.replace('++commits;*result=', '++commits;barrier(3);*result=')
prefix=prefix.replace('if(template_missing)return ESP_ERR_NOT_FOUND;', 'if(template_missing || !strcmp(id,"missing"))return ESP_ERR_NOT_FOUND;')
code=prefix+r'''
#include "ts_api.h"
#define TS_STRDUP_PSRAM strdup
#define TS_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static const char *TAG="audit";
static int removals,unlinks;
static bool service_protected;
bool ts_rule_edit_begin(void){return s_rule_ctx.initialized&&pthread_mutex_trylock(s_rule_ctx.transaction)==0;}
void ts_rule_edit_end(void){pthread_mutex_unlock(s_rule_ctx.transaction);}
static bool template_binding_protected(const char *id,const ts_action_template_t *next){return service_protected;}
static esp_err_t template_remove_impl(const char *id){++removals;template_missing=true;barrier(4);return ESP_OK;}
'''
for name in ['template_references','ts_action_template_remove_checked','ts_action_template_remove']:
 code+=extract('components/ts_automation/src/ts_action_manager.c',name)
code+=r'''
#define ACTIONS_SDCARD_DIR "/sdcard/config/actions"
static int fake_unlink(const char *path){assert(strstr(path,ACTIONS_SDCARD_DIR));++unlinks;return 0;}
#define unlink fake_unlink
'''
code+=extract('components/ts_api/src/ts_api_automation.c','api_automation_actions_delete')
code+=extract('components/ts_api/src/ts_api_automation.c','rule_commit_reply')
for name in ['ts_api_result_free','ts_api_result_ok','ts_api_result_error']:
 code+=extract('components/ts_api/src/ts_api.c',name)
web=Path('components/ts_webui/src/ts_webui_api.c').read_text();a=web.index('    if (ret == ESP_OK || result.code == TS_API_OK) {');b=web.index('\n}\n',a)
code+=r'''
static char *http_body;
static esp_err_t ts_http_send_json(void *req,int status,const char *body){assert(status==200);free(http_body);http_body=strdup(body);return ESP_OK;}
static esp_err_t ts_http_send_error(void *req,int status,const char *message){return ESP_FAIL;}
static esp_err_t send_result(esp_err_t ret,ts_api_result_t result){void *req=NULL;
'''+web[a:b]+'\n}\n'
code+=r'''
static const char *out_dir;
static void capture(const char *name,ts_api_result_t result){
 assert(send_result(ESP_OK,result)==ESP_OK);cJSON *wire=cJSON_Parse(http_body);assert(wire);cJSON_Delete(wire);
 char path[512];snprintf(path,sizeof(path),"%s/%s.json",out_dir,name);FILE*f=fopen(path,"w");assert(f);fputs(http_body,f);fputc('\n',f);fclose(f);
}
static void add(const char *id,const char *name,bool enabled,bool duplicate){
 ts_auto_rule_t r=candidate("fixture");strcpy(r.id,id);strcpy(r.name,name);r.enabled=enabled;
 strcpy(r.actions[0].template_id,"template");
 if(duplicate){r.actions=realloc(r.actions,2*sizeof(*r.actions));r.actions[1]=r.actions[0];r.action_count=2;}
 ts_rule_commit_result_t result;assert(ts_rule_commit(&r,id,0,&result)==ESP_OK);ts_rule_dispose(&r);
}
static void clear_rules(void){for(int i=0;i<s_rule_ctx.count;i++)payload_free(s_rule_ctx.rules[i].lease);s_rule_ctx.count=0;memset(s_rule_ctx.meta,0,sizeof(s_rule_ctx.meta));}
static void delete_api(const char *capture_name){
 cJSON *p=cJSON_Parse("{\"id\":\"template\"}");ts_api_result_t r={0};assert(api_automation_actions_delete(p,&r)==ESP_OK);
 if(capture_name)capture(capture_name,r);else ts_api_result_free(&r);cJSON_Delete(p);
}
static void *try_delete(void *unused){ts_action_template_delete_result_t r;assert(ts_action_template_remove_checked("template",&r)==ESP_ERR_INVALID_STATE);assert(!strcmp(r.check_reason,s_rule_ctx.loaded?"busy":"loading"));free(r.rules);return NULL;}

static int concurrent_ret;
static atomic_int writer_started;
static void *save_concurrent(void *unused){
 ts_auto_rule_t r=candidate("concurrent");strcpy(r.id,"concurrent");strcpy(r.actions[0].template_id,"template");ts_rule_commit_result_t result;
 atomic_store(&writer_started,1);concurrent_ret=ts_rule_commit(&r,r.id,0,&result);ts_rule_dispose(&r);return NULL;
}
static void *delete_concurrent(void *unused){assert(ts_action_template_remove("template")==ESP_OK);return NULL;}
static void wait_at(int phase,pthread_t *thread,void *(*fn)(void*)){
 pthread_mutex_lock(&control);block_phase=phase;reached=proceed=0;pthread_create(thread,NULL,fn,NULL);
 while(!reached)pthread_cond_wait(&changed,&control);pthread_mutex_unlock(&control);
}
static void release_at(void){pthread_mutex_lock(&control);proceed=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&control);}
int main(int argc,char**argv){
 assert(argc==2);out_dir=argv[1];pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER,txn;
 pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);pthread_mutex_init(&txn,&attr);pthread_mutexattr_destroy(&attr);
 s_rule_ctx=(ts_rule_engine_ctx_t){.rules=calloc(32,sizeof(ts_auto_rule_t)),.capacity=32,.initialized=true,.loaded=true,.mutex=&lock,.transaction=&txn};
 // H01/02: includes disabled/non-dashboard rules; repeated references are unique; 47-byte labels.
 const char *label="Model \" < & > 中文";add("rule-0",label,false,true);
 char id[64],name[48];memset(name,'N',47);name[47]=0;
 for(int i=1;i<32;i++){snprintf(id,sizeof(id),"rule-%d",i);add(id,name,i%2,false);}
 ts_action_template_delete_result_t d;int before=commits;
 assert(ts_action_template_remove_checked("template",&d)==ESP_ERR_INVALID_STATE&&d.references_confirmed&&d.rule_count==32&&d.rules);
 assert(!removals&&!unlinks&&commits==before);for(int i=0;i<32;i++){assert(!strcmp(d.rules[i].id,s_rule_ctx.rules[i].id));assert(!((rule_payload_t*)s_rule_ctx.rules[i].lease)->refs);}free(d.rules);
 delete_api("in-use");assert(!removals&&!unlinks);
 fail_alloc=0;delete_api("in-use-no-details");fail_alloc=-1;assert(!removals&&!unlinks);
 // H14: no waiting, actual transaction contention and distinct loaded flag.
 pthread_t worker;pthread_mutex_lock(&txn);s_rule_ctx.loaded=false;pthread_create(&worker,NULL,try_delete,NULL);pthread_join(worker,NULL);pthread_mutex_unlock(&txn);
 delete_api("check-loading");s_rule_ctx.loaded=true;
 pthread_mutex_lock(&txn);pthread_create(&worker,NULL,try_delete,NULL);pthread_join(worker,NULL);pthread_mutex_unlock(&txn);
 s_rule_ctx.initialized=false;delete_api("check-uninitialized");s_rule_ctx.initialized=true;
 s_rule_ctx.recovery_error=true;delete_api("check-recovery");s_rule_ctx.recovery_error=false;
 // H03/H04 deletion first -> a new write cannot use the deleted template.
 clear_rules();delete_api(NULL);assert(removals==1&&unlinks==2);
 ts_auto_rule_t r=candidate("valid");strcpy(r.actions[0].template_id,"template");ts_rule_commit_result_t result;
 before=commits;assert(ts_rule_commit(&r,r.id,0,&result)==ESP_ERR_NOT_FOUND&&!strcmp(result.error_code,"action_missing"));assert(!commits||commits==before);assert(s_rule_ctx.count==0);ts_rule_dispose(&r);
 // H13: denied update clears committing; the previous valid rule still triggers and saves.
 template_missing=false;r=candidate("valid");assert(ts_rule_commit(&r,r.id,0,&result)==ESP_OK);uint32_t revision=s_rule_ctx.rules[0].revision;
 strcpy(r.actions[0].template_id,"missing");before=commits;
 assert(ts_rule_commit(&r,r.id,revision,&result)==ESP_ERR_NOT_FOUND&&commits==before&&!s_rule_ctx.meta[0].committing);
 assert(!strcmp(result.missing_template_id,"missing")&&s_rule_ctx.rules[0].revision==revision);
 ts_api_result_t reply={0};rule_commit_reply(&reply,ESP_ERR_NOT_FOUND,&result);capture("action-missing",reply);
 bool triggered=false;assert(execute_rule(r.id,true,&triggered)==ESP_OK&&triggered);
 r.actions[0].template_id[0]=0;strcpy(r.name,"after failure");assert(ts_rule_commit(&r,r.id,revision,&result)==ESP_OK);
 r.enabled=false;assert(ts_rule_commit(&r,r.id,s_rule_ctx.rules[0].revision,&result)==ESP_OK);r.enabled=true;assert(ts_rule_commit(&r,r.id,s_rule_ctx.rules[0].revision,&result)==ESP_OK);
 ts_rule_dispose(&r);clear_rules();
 // H09: old/offline missing binding may remain; multiplicity cannot increase.
 template_missing=false;add("historic","Historic",true,false);template_missing=true;
 assert(ts_rule_acquire("historic",&r)==ESP_OK);strcpy(r.name,"Rename historic");revision=r.revision;ts_rule_release(&r);
 // Re-acquire a valid owned copy through codec, rather than retaining a released lease.
 cJSON*j=ts_rule_encode(&s_rule_ctx.rules[0]);assert(ts_rule_decode(j,&r)==ESP_OK);cJSON_Delete(j);strcpy(r.name,"Rename historic");
 assert(ts_rule_commit(&r,r.id,revision,&result)==ESP_OK);
 r.actions=realloc(r.actions,2*sizeof(*r.actions));r.actions[1]=r.actions[0];r.action_count=2;
 assert(ts_rule_commit(&r,r.id,s_rule_ctx.rules[0].revision,&result)==ESP_ERR_NOT_FOUND&&!s_rule_ctx.meta[0].committing);
 ts_rule_dispose(&r);clear_rules();template_missing=false;
 // H02 byte boundaries are distinct: ID 63, name 47.
 memset(id,'I',63);id[63]=0;add(id,name,false,true);delete_api("boundary");clear_rules();
 service_protected=true;delete_api("service-protected");assert(removals==1);service_protected=false;

 // Actual production gates: force both concurrent orders with persistent-operation barriers.
 pthread_t first,second;template_missing=false;atomic_store(&writer_started,0);
 wait_at(3,&first,save_concurrent);
 assert(ts_action_template_remove_checked("template",&d)==ESP_ERR_INVALID_STATE&&!strcmp(d.check_reason,"busy"));free(d.rules);
 release_at();pthread_join(first,NULL);block_phase=0;assert(concurrent_ret==ESP_OK);
 assert(ts_action_template_remove_checked("template",&d)==ESP_ERR_INVALID_STATE&&d.references_confirmed);free(d.rules);clear_rules();
 wait_at(4,&first,delete_concurrent);atomic_store(&writer_started,0);pthread_create(&second,NULL,save_concurrent,NULL);
 while(!atomic_load(&writer_started)){struct timespec delay={0,100000};nanosleep(&delay,NULL);}
 release_at();pthread_join(first,NULL);pthread_join(second,NULL);block_phase=0;
 assert(concurrent_ret==ESP_ERR_NOT_FOUND&&s_rule_ctx.count==0);
 free(http_body);free(s_rule_ctx.rules);pthread_mutex_destroy(&txn);pthread_mutex_destroy(&lock);
 puts("PASS production delete + rule commit + HTTP: H01-H14, leases, no side effects, historical multiplicity, committing cleared, 47/63-byte fields");
}
'''
with tempfile.TemporaryDirectory(prefix='tianshan-delete-ref-') as tmp:
 d=Path(tmp);f=d/'test.c';f.write_text(code);wire=d/'wire';wire.mkdir()
 env={**os.environ,'DEVELOPER_DIR':os.environ.get('DEVELOPER_DIR','/Library/Developer/CommandLineTools')}
 includes=['tests/runtime/stubs','tests/certificate/stubs','components/ts_automation/include','components/ts_security/include','components/ts_api/include',str(idf/'components/json/cJSON')]
 subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined','-Wno-deprecated-declarations',*[f'-I{x}' for x in includes],str(f),'components/ts_automation/src/ts_rule_codec.c','components/ts_automation/src/ts_action_filter.c',str(idf/'components/json/cJSON/cJSON.c'),'-lpthread','-lm','-o',str(d/'test')],check=True,env=env)
 subprocess.run([str(d/'test'),str(wire)],check=True,env=env)
 fixtures=Path('tests/fixtures/delete-reference')
 if os.environ.get('UPDATE_FIXTURES')=='1':fixtures.mkdir(parents=True,exist_ok=True)
 for f in wire.glob('*.json'):
  target=fixtures/f.name
  if os.environ.get('UPDATE_FIXTURES')=='1':target.write_bytes(f.read_bytes())
  assert target.read_bytes()==f.read_bytes(),f'wire bytes drift: {f.name}'
  assert json.loads(f.read_text())['code']!=0
 print('PASS exact HTTP byte fixtures:',len(list(wire.glob('*.json'))))
