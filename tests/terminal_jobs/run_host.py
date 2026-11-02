#!/usr/bin/env python3
from pathlib import Path
import tempfile,subprocess,re
R=Path(__file__).resolve().parents[2]
def fun(text,name,signature=None):
 m=re.search(r'^(?:esp_err_t|bool|void|const ts_console_exec_context_t \*)\s*'+re.escape(name)+r'\([^;]*?\)\s*\{',text,re.M);assert m,name
 a=m.start();return text[a:text.index('\n}',a)+2]

console=(R/'components/ts_console/src/ts_console.c').read_text();fwd=(R/'components/ts_security/src/ts_port_forward.c').read_text()
pre=r'''#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
static TaskHandle_t current=(void*)1,mutex_owner;
static unsigned depth,exec_calls;
static TaskHandle_t xTaskGetCurrentTaskHandle(void){return current;}
static int xSemaphoreTakeRecursive(void *m,int t){(void)m;(void)t;if(mutex_owner&&mutex_owner!=current)return 0;mutex_owner=current;depth++;return 1;}
static void xSemaphoreGiveRecursive(void *m){(void)m;assert(mutex_owner==current&&depth);if(!--depth)mutex_owner=NULL;}
typedef struct {int code;const char *message;void *data;size_t data_size;}ts_cmd_result_t;
typedef void(*ts_console_output_cb_t)(const char*,size_t,void*);
typedef struct {bool remote;bool(*cancelled)(void*);ts_console_output_cb_t output;void *user_data;}ts_console_exec_context_t;
static char s_raw_cmdline[512];
static SemaphoreHandle_t s_exec_mutex=(void*)1;
static _Atomic(TaskHandle_t) s_exec_owner;
static const ts_console_exec_context_t *s_exec_context;
static struct {bool interrupt_requested;}s_console;
static bool cancelled;
static bool is_cancelled(void *p){(void)p;return cancelled;}
esp_err_t ts_console_exec(const char*,ts_cmd_result_t*);
const ts_console_exec_context_t *ts_console_current_context(void);
static int esp_console_run(const char *cmd,int *code){
 exec_calls++;assert(ts_console_current_context()||!strcmp(cmd,"uart"));
 if(!strcmp(cmd,"parent")){
  assert(!strcmp(s_raw_cmdline,"parent"));ts_cmd_result_t result;
  assert(ts_console_exec("child",&result)==0&&result.code==7);assert(!strcmp(s_raw_cmdline,"parent"));
  current=(void*)2;assert(!ts_console_current_context());assert(ts_console_exec("uart",&result)==ESP_ERR_INVALID_STATE);current=(void*)1;
 }
 *code=7;return 0;
}
'''
body='\n'.join(fun(console,n) for n in ['ts_console_current_context','ts_console_interrupted','ts_console_clear_interrupt','ts_console_exec_with_context','ts_console_exec'])
main=r'''
int main(void){
 ts_cmd_result_t r;ts_console_exec_context_t ctx={.remote=true,.cancelled=is_cancelled};
 cancelled=true;assert(ts_console_exec_with_context("parent",&r,&ctx)==ESP_ERR_TIMEOUT&&exec_calls==0);
 cancelled=false;assert(ts_console_exec_with_context("parent",&r,&ctx)==0&&r.code==7&&exec_calls==2);
 assert(!s_exec_owner&&!depth&&!s_raw_cmdline[0]);
 s_exec_context=&ctx;s_exec_owner=current;cancelled=true;ts_console_clear_interrupt();assert(ts_console_interrupted());s_exec_owner=NULL;s_exec_context=NULL;
 puts("PASS actual console: early cancellation, sticky stop, nested commands, unrelated UART exclusion, raw-command restoration");
}
'''
with tempfile.TemporaryDirectory() as td:
 for name,text in [('console',pre+body+main)]:
  p=Path(td)/name;p.with_suffix('.c').write_text(text);subprocess.run(['cc','-std=gnu11','-g','-fsanitize=address,undefined',str(p.with_suffix('.c')),'-o',str(p)],check=True);subprocess.run([str(p)],check=True)

# Compile the actual known-host getter with a bounded fake NVS record.
known=(R/'components/ts_security/src/ts_known_hosts.c').read_text()
kpre=r"""#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_SIZE 4
#define portMAX_DELAY 0
typedef int esp_err_t;typedef int ts_host_key_type_t;
typedef struct {char host[128];uint16_t port;int type;char fingerprint[65];uint32_t added_time;}ts_known_host_t;
typedef struct {int type;char fingerprint[65];uint32_t added_time;}stored_host_t;
static struct {int initialized,mutex,nvs;}s_state={1,0,0};
static int short_record;
static void make_nvs_key(const char*h,int p,char*out,size_t n){(void)h;(void)p;snprintf(out,n,"fixture");}
static void xSemaphoreTake(int m,int t){(void)m;(void)t;}
static void xSemaphoreGive(int m){(void)m;}
static int nvs_get_blob(int nvs,const char*k,stored_host_t*out,size_t*n){(void)nvs;(void)k;memset(out,0,sizeof(*out));memset(out->fingerprint,'a',64);out->type=2;out->added_time=123;*n=sizeof(*out)-(short_record?1:0);return 0;}
"""
kmain=r"""
int main(void){ts_known_host_t out;memset(&out,0xa5,sizeof(out));assert(ts_known_hosts_get("fixture",22,&out)==0);assert(strlen(out.fingerprint)==64&&out.fingerprint[64]==0&&!strcmp(out.host,"fixture")&&out.added_time==123);char host[200];memset(host,'b',199);host[199]=0;memset(&out,0xa5,sizeof(out));assert(ts_known_hosts_get(host,22,&out)==0&&out.host[127]==0&&out.fingerprint[64]==0);short_record=1;assert(ts_known_hosts_get("fixture",22,&out)==ESP_ERR_INVALID_SIZE);puts("PASS actual known-host getter: dirty destination, full fingerprint, bounded hostname, malformed record rejected");}
"""
with tempfile.TemporaryDirectory() as td:
 p=Path(td)/'known';p.with_suffix('.c').write_text(kpre+fun(known,'ts_known_hosts_get')+kmain);subprocess.run(['cc','-std=gnu11','-g','-fsanitize=address,undefined',str(p.with_suffix('.c')),'-o',str(p)],check=True);subprocess.run([str(p)],check=True)
