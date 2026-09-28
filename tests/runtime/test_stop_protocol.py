"""Production top-level stop/task + actual engine cleanup with a blocked action."""
from pathlib import Path
import re,subprocess,os
build=Path('/tmp/tianshan-runtime-tests');build.mkdir(exist_ok=True)
def extract(source,name):
 m=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',source,re.M);assert m,name
 return source[m.start():source.index('\n}',m.start())+2]
s=Path('tests/runtime/test_engine.c').read_text().split('int main(void)')[0]
s=s.replace('#include "../../components/ts_automation/src/ts_rule_store.h"',f'#include "{Path.cwd()}/components/ts_automation/src/ts_rule_store.h"')
s=s.replace('bool ts_action_manager_accepting(void){return true;}','static bool wait_accepting=true; bool ts_action_manager_accepting(void){return wait_accepting;}')
s+='''
#define TS_AUTO_STATE_UNINITIALIZED 0
#define TS_AUTO_STATE_INITIALIZED 1
#define TS_AUTO_STATE_RUNNING 2
#define TS_AUTO_STATE_PAUSED 3
#define CONFIG_TS_AUTOMATION_TASK_STACK_SIZE 8192
#define CONFIG_TS_AUTOMATION_TASK_PRIORITY 5
#define pdMS_TO_TICKS(n) (n)
#define pdPASS 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
typedef int ts_automation_state_t;typedef unsigned TickType_t;typedef int BaseType_t;
static struct {int state;bool stopping;void *task_handle;pthread_mutex_t *mutex;unsigned rule_triggers;} auto_ctx;
static atomic_bool async_pending, accepting=true;
static int source_stops, delay_ticks, cut_delay, executions;
esp_err_t ts_action_manager_quiesce(void){accepting=false;wait_accepting=false;return async_pending?ESP_ERR_TIMEOUT:ESP_OK;}
esp_err_t ts_action_manager_resume(void){if(async_pending)return ESP_ERR_INVALID_STATE;accepting=true;wait_accepting=true;return ESP_OK;}
esp_err_t ts_source_start_all(void){return ESP_OK;}
void ts_source_stop_all(void){++source_stops;}
int ts_source_poll_all(void){return 0;}
int ts_rule_evaluate_all(void){bool triggered=false;execute_rule("rule",false,&triggered);return triggered;}
static void vTaskDelay(unsigned ms){++delay_ticks;if(cut_delay)wait_accepting=false;struct timespec t={0,1000000};nanosleep(&t,NULL);}
static void vTaskDelete(void *task){assert(!task);pthread_exit(NULL);}
static int xTaskCreatePinnedToCore(void(*fn)(void*),const char*n,unsigned stack,void*arg,unsigned prio,void**handle,int core){*handle=(void*)1;return pdPASS;}
static void automation_task(void *arg);
'''
auto=Path('components/ts_automation/src/ts_automation.c').read_text()
for name in ['ts_automation_start','ts_automation_stop','automation_task']:
 s+=extract(auto,name).replace('s_ctx','auto_ctx')+'\n'
s+='''
static bool check_action_condition(const ts_auto_action_t *a){return true;}
esp_err_t ts_action_execute(const ts_auto_action_t *a){++executions;wait_accepting=false;return ESP_OK;}
'''
engine=Path('components/ts_automation/src/ts_rule_engine.c').read_text()
for name in ['rule_wait','execute_action_with_repeat']:
 s+=extract(engine,name)+'\n'
s+='''
static void *run_auto(void *arg){automation_task(arg);return NULL;}
int main(void){
 pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER,transaction=PTHREAD_MUTEX_INITIALIZER,automutex=PTHREAD_MUTEX_INITIALIZER;
 s_rule_ctx=(ts_rule_engine_ctx_t){.rules=calloc(4,sizeof(ts_auto_rule_t)),.capacity=4,.initialized=true,.loaded=true,.mutex=&lock,.transaction=&transaction};
 ts_auto_rule_t a=candidate("active");ts_rule_commit_result_t result;assert(ts_rule_commit(&a,a.id,0,&result)==ESP_OK);
 auto_ctx.state=TS_AUTO_STATE_RUNNING;auto_ctx.task_handle=(void*)1;auto_ctx.mutex=&automutex;
 block_phase=2;reached=proceed=0;pthread_t task;pthread_create(&task,NULL,run_auto,NULL);
 pthread_mutex_lock(&control);while(!reached)pthread_cond_wait(&changed,&control);pthread_mutex_unlock(&control);
 independent_service=true;
 assert(ts_automation_stop()==ESP_ERR_TIMEOUT);
 assert(ts_automation_start()==ESP_ERR_INVALID_STATE);
 assert(s_rule_ctx.meta[0].executing&&((rule_payload_t*)s_rule_ctx.rules[0].lease)->refs==1);
 async_pending=true;unblock(task);
 assert(!s_rule_ctx.meta[0].executing&&((rule_payload_t*)s_rule_ctx.rules[0].lease)->refs==0);
 assert(ts_automation_stop()==ESP_ERR_TIMEOUT&&ts_automation_start()==ESP_ERR_INVALID_STATE);
 assert(!source_stops&&independent_service);
 async_pending=false;assert(ts_automation_stop()==ESP_OK&&source_stops==1);
 assert(independent_service); /* stopping automation never forgets running/unknown remote services */
 assert(ts_automation_start()==ESP_OK&&accepting);
 wait_accepting=true;delay_ticks=0;cut_delay=1;
 assert(!rule_wait(60000)&&delay_ticks==1);cut_delay=0;wait_accepting=true;
 a.actions[0].repeat_mode=TS_AUTO_REPEAT_COUNT;a.actions[0].repeat_count=10;
 assert(execute_action_with_repeat(&a.actions[0],NULL,NULL)==ESP_ERR_INVALID_STATE&&executions==1);
 ts_rule_dispose(&a);payload_free(s_rule_ctx.rules[0].lease);free(s_rule_ctx.rules);
 puts("PASS actual automation stop + engine: blocked action returns normally, leases/executing clear, async work prevents restart, independent remote ownership retained");
}
'''
(build/'stop_protocol.c').write_text(s)
idf=os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf');env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined','-isysroot','/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk','-Wno-deprecated-declarations','-I'+str(build),'-Itests/runtime/stubs','-Itests/certificate/stubs','-Icomponents/ts_automation/include','-Icomponents/ts_security/include','-I'+idf+'/components/json/cJSON',str(build/'stop_protocol.c'),'components/ts_automation/src/ts_rule_codec.c',idf+'/components/json/cJSON/cJSON.c','-lpthread','-lm','-o',str(build/'stop_protocol')],check=True,env=env)
subprocess.run([str(build/'stop_protocol')],check=True,env=env)
