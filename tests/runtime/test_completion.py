import sys
"""Compile production completion lifetime and synchronous admission; mock only queue/RTOS I/O."""
from pathlib import Path
import re, subprocess, os
source=Path('components/ts_automation/src/ts_action_manager.c').read_text()
def function(name):
 m=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',source,re.M)
 assert m,name
 return source[m.start():source.index('\n}',m.start())+2]
build=Path('/tmp/tianshan-runtime-tests');build.mkdir(exist_ok=True)
code=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include "ts_action_manager.h"
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define xSemaphoreTake completion_take
#define xSemaphoreGive completion_give
static int sem_live, mode, binding_refs;static bool has_queue;
typedef unsigned UBaseType_t;
static int64_t esp_timer_get_time(void){return 1000;}
static void *xSemaphoreCreateBinary(void){++sem_live;return malloc(1);}
static void vSemaphoreDelete(void *p){--sem_live;free(p);}
static void *heap_caps_calloc(size_t n,size_t s,int caps){return calloc(n,s);}
static ts_action_queue_entry_t queued;
static int xQueueSend(void*q,void*p,unsigned timeout){if(mode==2)return 0;assert(!has_queue);queued=*(ts_action_queue_entry_t*)p;has_queue=true;return 1;}
static int xQueueReceive(void *queue,void *out,unsigned timeout){(void)queue;assert(!timeout);if(!has_queue)return 0;*(ts_action_queue_entry_t*)out=queued;has_queue=false;return 1;}
static unsigned uxQueueMessagesWaiting(void *queue){(void)queue;return has_queue;}
static int xSemaphoreGive(void*s){return 1;}
static int xSemaphoreTake(void*s,unsigned timeout){if(s==(void*)2)return 1;if(mode==1){queued.result_ptr->status=TS_ACTION_STATUS_SUCCESS;return 1;}return 0;}
void ts_ssh_binding_lock(void){}
void ts_ssh_binding_unlock(void){}
esp_err_t ts_action_snapshot(const ts_auto_action_t*a,ts_auto_action_t*b){*b=*a;return ESP_OK;}
void ts_action_snapshot_retain(const ts_auto_action_t*a){if(a->runtime_binding)++binding_refs;}
void ts_action_snapshot_release(ts_auto_action_t*a){if(a->runtime_binding){assert(binding_refs>1);--binding_refs;a->runtime_binding=NULL;}}
bool ts_ssh_service_complete_operation(const char *id,uint32_t operation,esp_err_t error){(void)id;(void)operation;(void)error;assert(false);return false;}
static void callback(const ts_auto_action_t *action,const ts_action_result_t *result,void *context){(void)action;(void)result;(void)context;assert(false);}
static struct {int running,accepting;unsigned pending,direct_pending;ts_action_stats_t stats;void*executor_task;void*action_queue;void*stats_mutex;} context={.running=1,.accepting=1,.executor_task=(void*)1,.action_queue=(void*)1,.stats_mutex=(void*)2},*s_ctx=&context;
typedef struct {atomic_uint refs;SemaphoreHandle_t semaphore;ts_action_result_t result;} action_completion_t;
'''+'\n'.join(function(n) for n in ['completion_release','action_admit','action_finished','direct_service_finished','entry_finished','ts_action_manager_quiesce','ts_action_manager_resume','prepare_action_entry','action_manager_execute','ts_action_manager_execute','action_queue','ts_action_queue','ts_action_submit_prepared','ts_action_cancel_all'])+r'''
int main(void){
 ts_auto_action_t a={.type=TS_AUTO_ACT_LOG};ts_action_result_t result;
 assert(ts_action_manager_execute(&a,&result)==ESP_ERR_TIMEOUT&&result.status==TS_ACTION_STATUS_TIMEOUT&&sem_live==1);
 /* Caller has already returned: executor still owns this storage. */
 assert(s_ctx->pending==1);assert(ts_action_manager_quiesce()==ESP_ERR_TIMEOUT);assert(ts_action_manager_resume()!=ESP_OK);
 queued.result_ptr->status=TS_ACTION_STATUS_SUCCESS;strcpy(queued.result_ptr->output,"late completion");completion_release(queued.completion);action_finished();has_queue=false;assert(!sem_live&&!s_ctx->pending);assert(ts_action_manager_resume()==ESP_OK);
 mode=1;assert(ts_action_manager_execute(&a,&result)==ESP_OK&&sem_live==1);completion_release(queued.completion);action_finished();has_queue=false;assert(!sem_live&&!s_ctx->pending);assert(ts_action_manager_resume()==ESP_OK);
 mode=2;assert(ts_action_manager_execute(&a,&result)==ESP_ERR_NO_MEM&&!sem_live&&!s_ctx->pending);
 assert(action_admit()&&s_ctx->pending==2);
 assert(ts_action_manager_quiesce()==ESP_ERR_TIMEOUT&&!action_admit());
 action_finished();assert(ts_action_manager_resume()!=ESP_OK);action_finished();
 assert(ts_action_manager_quiesce()==ESP_OK&&ts_action_manager_resume()==ESP_OK);
 mode=0;assert(ts_action_manager_execute(&a,&result)==ESP_ERR_TIMEOUT&&sem_live==1&&has_queue);
 assert(ts_action_cancel_all()==ESP_OK&&!sem_live&&!s_ctx->pending&&!has_queue&&result.status==TS_ACTION_STATUS_TIMEOUT);
 binding_refs=1;a.runtime_snapshot=true;a.runtime_binding=(void*)3;a.async=true;a.delay_ms=75;
 assert(ts_action_submit_prepared(&a)==ESP_OK&&binding_refs==2&&s_ctx->pending==1&&queued.action.delay_ms==0);
 assert(ts_action_cancel_all()==ESP_OK&&binding_refs==1&&!s_ctx->pending);
 assert(ts_action_queue(&a,callback,(void*)4,3)==ESP_OK&&binding_refs==2&&queued.callback==callback&&queued.user_data==(void*)4&&queued.priority==3&&queued.action.delay_ms==75);
 assert(ts_action_cancel_all()==ESP_OK&&binding_refs==1&&!has_queue&&!s_ctx->pending);
 mode=2;assert(ts_action_submit_prepared(&a)==ESP_ERR_NO_MEM&&binding_refs==1&&!s_ctx->pending);
 puts("PASS production sync/async action completion: prepared leases, metadata, cancellation without callbacks, timeout/late results and queue rejection;  timeout then late result, success, queue rejection; semaphore lifetime balanced");
}
'''
(build/'completion.c').write_text(code)
env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined',*(['-isysroot','/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'] if sys.platform == 'darwin' else []),'-Itests/runtime/stubs','-Itests/certificate/stubs','-Icomponents/ts_automation/include','-Icomponents/ts_security/include',str(build/'completion.c'),'-o',str(build/'completion')],check=True,env=env)
subprocess.run([str(build/'completion')],check=True,env=env)
