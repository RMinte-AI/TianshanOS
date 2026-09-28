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
static int sem_live, mode;
static void *xSemaphoreCreateBinary(void){++sem_live;return malloc(1);}
static void vSemaphoreDelete(void *p){--sem_live;free(p);}
static void *heap_caps_calloc(size_t n,size_t s,int caps){return calloc(n,s);}
static ts_action_queue_entry_t queued;
static int xQueueSend(void*q,void*p,unsigned timeout){if(mode==2)return 0;queued=*(ts_action_queue_entry_t*)p;return 1;}
static int xSemaphoreGive(void*s){return 1;}
static int xSemaphoreTake(void*s,unsigned timeout){if(s==(void*)2)return 1;if(mode==1){queued.result_ptr->status=TS_ACTION_STATUS_SUCCESS;return 1;}return 0;}
void ts_ssh_binding_lock(void){}
void ts_ssh_binding_unlock(void){}
esp_err_t ts_action_snapshot(const ts_auto_action_t*a,ts_auto_action_t*b){*b=*a;return ESP_OK;}
void ts_action_snapshot_retain(const ts_auto_action_t*a){}
void ts_action_snapshot_release(ts_auto_action_t*a){}
static struct {int running,accepting;unsigned pending;void*executor_task;void*action_queue;void*stats_mutex;} context={.running=1,.accepting=1,.executor_task=(void*)1,.action_queue=(void*)1,.stats_mutex=(void*)2},*s_ctx=&context;
typedef struct {atomic_uint refs;SemaphoreHandle_t semaphore;ts_action_result_t result;} action_completion_t;
'''+'\n'.join(function(n) for n in ['completion_release','action_admit','action_finished','ts_action_manager_quiesce','ts_action_manager_resume','ts_action_manager_execute'])+r'''
int main(void){
 ts_auto_action_t a={.type=TS_AUTO_ACT_LOG};ts_action_result_t result;
 assert(ts_action_manager_execute(&a,&result)==ESP_ERR_TIMEOUT&&result.status==TS_ACTION_STATUS_TIMEOUT&&sem_live==1);
 /* Caller has already returned: executor still owns this storage. */
 assert(s_ctx->pending==1);assert(ts_action_manager_quiesce()==ESP_ERR_TIMEOUT);assert(ts_action_manager_resume()!=ESP_OK);
 queued.result_ptr->status=TS_ACTION_STATUS_SUCCESS;strcpy(queued.result_ptr->output,"late completion");completion_release(queued.completion);action_finished();assert(!sem_live&&!s_ctx->pending);assert(ts_action_manager_resume()==ESP_OK);
 mode=1;assert(ts_action_manager_execute(&a,&result)==ESP_OK&&sem_live==1);completion_release(queued.completion);action_finished();assert(!sem_live&&!s_ctx->pending);assert(ts_action_manager_resume()==ESP_OK);
 mode=2;assert(ts_action_manager_execute(&a,&result)==ESP_ERR_NO_MEM&&!sem_live&&!s_ctx->pending);
 assert(action_admit()&&s_ctx->pending==2);
 assert(ts_action_manager_quiesce()==ESP_ERR_TIMEOUT&&!action_admit());
 action_finished();assert(ts_action_manager_resume()!=ESP_OK);action_finished();
 assert(ts_action_manager_quiesce()==ESP_OK&&ts_action_manager_resume()==ESP_OK);
 puts("PASS production synchronous action completion: timeout then late result, success, queue rejection; semaphore lifetime balanced");
}
'''
(build/'completion.c').write_text(code)
env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined','-isysroot','/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk','-Itests/runtime/stubs','-Itests/certificate/stubs','-Icomponents/ts_automation/include',str(build/'completion.c'),'-o',str(build/'completion')],check=True,env=env)
subprocess.run([str(build/'completion')],check=True,env=env)
