#!/usr/bin/env python3
"""F60: real argtable command -> production API -> recorded RTOS reboot task. No reboot occurs."""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile
from test_console_lifecycle import body, IDF

root = Path(__file__).resolve().parents[2]
os.chdir(root)
normal = ['default', 'zero', 'negative', 'api-web', 'api-bounds']
defects = ['one', 'five', 'ten', 'twenty', 'large']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--group', choices=['normal', 'defect', 'all'], default='all')
parser.add_argument('--case', choices=normal + defects)
args = parser.parse_args()
cli = 'components/ts_console/commands/ts_cmd_system.c'
api = 'components/ts_api/src/ts_api_system.c'
core = Path('components/ts_api/src/ts_api.c').read_text()
core = re.sub(r'^#include[^\n]*\n', '', core[:core.index('\nesp_err_t ts_api_register_all(void)')], flags=re.M)
code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ts_api.h"
#include "freertos/semphr.h"
#include "argtable3.h"
#define TS_LOGD(...) ((void)0)
#define TS_LOGI(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define TS_STRDUP_PSRAM strdup
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdMS_TO_TICKS(n) (n)
#define TS_CMD_CAT_SYSTEM 0
static void *heap_caps_calloc(size_t n,size_t size,unsigned caps){(void)caps;return calloc(n,size);}
static cJSON *ts_config_pack_parse_import_request(const char *s,size_t n){(void)n;return cJSON_Parse(s);}
static char output[2048], request_json[256];
static unsigned tasks, sleeps[4], sleep_count, restarts, task_deletes;
static int scheduled;
static void (*recorded_task)(void *);static void *recorded_arg;
static int ts_console_printf(const char *fmt,...){va_list ap;va_start(ap,fmt);int r=vsnprintf(output+strlen(output),sizeof(output)-strlen(output),fmt,ap);va_end(ap);return r;}
static void vTaskDelay(unsigned ms){assert(sleep_count<4);sleeps[sleep_count++]=ms;}
static void esp_restart(void){++restarts;} /* Never calls a host/device restart. */
static void vTaskDelete(void *task){assert(!task);++task_deletes;}
static int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle){
 assert(!strcmp(name,"reboot")&&stack==2048&&priority==1&&!handle);
 ++tasks;scheduled=(int)(intptr_t)arg;recorded_task=fn;recorded_arg=arg;return 1;
}
typedef struct {const char *command,*help,*hint;int category;int (*func)(int,char **);void *argtable;} ts_console_cmd_t;
static int (*registered_command)(int,char **);
static esp_err_t ts_console_register_cmd(const ts_console_cmd_t *cmd){assert(!strcmp(cmd->command,"system"));registered_command=cmd->func;return ESP_OK;}
'''
code += core
code += body(api, 'reboot_task') + body(api, 'api_system_reboot')
# Observe the request at the handler boundary; the handler and scheduler are real.
code += r'''
static esp_err_t record_reboot(const cJSON *params,ts_api_result_t *result){
 char *json=cJSON_PrintUnformatted(params);snprintf(request_json,sizeof(request_json),"%s",json?json:"null");free(json);
 return api_system_reboot(params,result);
}
'''
text = Path(cli).read_text(); start = text.index('static struct {')
code += text[start:text.index('} s_system_args;', start) + len('} s_system_args;')]
for name in ['do_system_info', 'do_system_version', 'do_system_uptime', 'do_system_memory', 'do_system_cpu', 'do_system_tasks']:
    code += f'static int {name}(bool json){{(void)json;assert(false);return 1;}}\n'
for name in ['do_system_memory_detail', 'do_system_save']:
    code += f'static int {name}(void){{assert(false);return 1;}}\n'
code += body(cli, 'do_system_reboot') + body(cli, 'cmd_system') + body(cli, 'ts_cmd_system_register')
entry = re.search(r'\{\s*\.name = "system.reboot",[^{}]*\}', Path(api).read_text()).group()
code += 'static const ts_api_endpoint_t reboot_entry = ' + entry.replace('api_system_reboot', 'record_reboot') + ';\n'
code += r'''
static void reset(void){output[0]=request_json[0]=0;tasks=sleep_count=restarts=task_deletes=0;scheduled=-1;}
static void check_cli(const char *value,int expected){
 reset();char *argv[]={"system","--reboot","--delay",(char *)value};
 int rc=registered_command(value?4:2,argv);
 printf("CLI value=%s rc=%d request=%s scheduled_ms=%d prompt=%s",value?value:"<default>",rc,request_json,scheduled,output);fflush(stdout);
 assert(rc==0&&tasks==1&&sleep_count==1&&sleeps[0]==100&&!restarts);
 assert(scheduled==expected);
 if(value&&atoi(value)>0){char message[80];snprintf(message,sizeof(message),"System will reboot in %d seconds...",expected/1000);assert(strstr(output,message));
  cJSON *params=cJSON_Parse(request_json),*delay=cJSON_GetObjectItem(params,"delay");assert(cJSON_IsNumber(delay)&&delay->valueint==expected);cJSON_Delete(params);
 }else assert(strstr(output,"Rebooting..."));
 recorded_task(recorded_arg);assert(sleep_count==2&&sleeps[1]==(unsigned)expected&&restarts==1&&task_deletes==1);
}
static void check_api(int delay,int expected){
 reset();cJSON *params=cJSON_CreateObject();cJSON_AddNumberToObject(params,"delay",delay);ts_api_result_t result={0};
 assert(ts_api_call("system.reboot",params,&result)==ESP_OK&&result.code==TS_API_OK);
 printf("API request=%s scheduled_ms=%d response_ms=%d\n",request_json,scheduled,cJSON_GetObjectItem(result.data,"delay_ms")->valueint);fflush(stdout);
 assert(tasks==1&&scheduled==expected&&!sleep_count&&!restarts);
 assert(cJSON_GetObjectItem(result.data,"delay_ms")->valueint==expected);
 recorded_task(recorded_arg);assert(sleeps[0]==(unsigned)expected&&restarts==1);
 cJSON_Delete(params);ts_api_result_free(&result);
}
int main(int argc,char **argv){
 assert(argc==2);assert(ts_api_init()==ESP_OK);assert(ts_api_register(&reboot_entry)==ESP_OK);assert(ts_cmd_system_register()==ESP_OK);
 if(!strcmp(argv[1],"default"))check_cli(NULL,500);
 else if(!strcmp(argv[1],"zero"))check_cli("0",500);
 else if(!strcmp(argv[1],"negative"))check_cli("-1",500);
 else if(!strcmp(argv[1],"one"))check_cli("1",1000);
 else if(!strcmp(argv[1],"five"))check_cli("5",5000);
 else if(!strcmp(argv[1],"ten"))check_cli("10",10000);
 else if(!strcmp(argv[1],"twenty"))check_cli("20",10000);
 else if(!strcmp(argv[1],"large"))check_cli("2147483647",10000);
 else if(!strcmp(argv[1],"api-web"))check_api(500,500);
 else if(!strcmp(argv[1],"api-bounds")){check_api(0,100);check_api(20000,10000);}
 else assert(false);
 arg_freetable((void **)&s_system_args,sizeof(s_system_args)/sizeof(void *));assert(ts_api_deinit()==ESP_OK);printf("PASS F60 %s\n",argv[1]);
}
'''
cjson = IDF / 'components/json/cJSON'; argtable = IDF / 'components/console/argtable3'
with tempfile.TemporaryDirectory(prefix='ts-system-reboot-') as directory:
    build = Path(directory); (build / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-Wno-deprecated-declarations', '-Itests/runtime/storage_stubs', '-Icomponents/ts_api/include',
                    '-I' + str(cjson), '-I' + str(argtable), str(build / 'test.c'), str(cjson / 'cJSON.c'),
                    *map(str, argtable.glob('*.c')), '-lpthread', '-lm', '-o', str(build / 'test')], check=True)
    cases = [args.case] if args.case else normal if args.group == 'normal' else defects if args.group == 'defect' else normal + defects
    failed = []
    for case in cases:
        completed = subprocess.run([str(build / 'test'), case])
        print(f'F60 CASE {case} EXIT {completed.returncode}', flush=True)
        if completed.returncode: failed.append(case)
    raise SystemExit(bool(failed))
