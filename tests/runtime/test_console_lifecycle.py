"""Production console/service lifecycle with SDK linenoise and local RTOS/UART fixtures."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

IDF = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))


def body(path, name):
    source = Path(path).read_text()
    match = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.end()) + 2] + '\n'


RTOS = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include "esp_err.h"
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) (n)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define TS_LOGI(...) ((void)0)
#define TS_LOGD(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(e) assert((e)==ESP_OK)
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef struct task {pthread_t thread;void (*fn)(void *);void *arg;int held;} *TaskHandle_t;
typedef struct semaphore {pthread_mutex_t mutex;pthread_cond_t changed;int value;bool binary;TaskHandle_t owner;} *SemaphoreHandle_t;
static _Thread_local TaskHandle_t current_task;
static _Thread_local struct task main_task;
static TaskHandle_t tasks[128];static int task_count;
static bool fail_next_create;
static void (*after_binary_give)(SemaphoreHandle_t);
static int64_t esp_timer_get_time(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000000LL+t.tv_nsec/1000;}
static const char *esp_err_to_name(int e) {(void)e;return "fixture error";}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return current_task?current_task:&main_task;}
static TickType_t xTaskGetTickCount(void) {return esp_timer_get_time()/1000;}
static struct timespec deadline(unsigned ms) {struct timespec t;clock_gettime(CLOCK_REALTIME,&t);t.tv_sec+=ms/1000;t.tv_nsec+=(ms%1000)*1000000L;if(t.tv_nsec>=1000000000L){++t.tv_sec;t.tv_nsec-=1000000000L;}return t;}
static SemaphoreHandle_t semaphore(bool binary) {
    SemaphoreHandle_t s=calloc(1,sizeof(*s));assert(s);pthread_mutex_init(&s->mutex,NULL);pthread_cond_init(&s->changed,NULL);s->binary=binary;s->value=!binary;return s;
}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {return semaphore(false);}
static SemaphoreHandle_t xSemaphoreCreateBinary(void) {return semaphore(true);}
static int xSemaphoreTake(SemaphoreHandle_t s,unsigned timeout) {
    assert(s);pthread_mutex_lock(&s->mutex);struct timespec until=deadline(timeout==portMAX_DELAY?10000:timeout);
    while(!s->value){if(!timeout||pthread_cond_timedwait(&s->changed,&s->mutex,&until)){pthread_mutex_unlock(&s->mutex);return pdFALSE;}}
    s->value=0;if(!s->binary){assert(!s->owner);s->owner=xTaskGetCurrentTaskHandle();++s->owner->held;}
    pthread_mutex_unlock(&s->mutex);return pdTRUE;
}
static int xSemaphoreGive(SemaphoreHandle_t s) {
    assert(s);pthread_mutex_lock(&s->mutex);if(!s->binary){assert(s->owner==xTaskGetCurrentTaskHandle());--s->owner->held;s->owner=NULL;}
    s->value=1;pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&s->mutex);
    if(s->binary&&after_binary_give)after_binary_give(s);return pdTRUE;
}
static void vSemaphoreDelete(SemaphoreHandle_t s) {assert(s&&!s->owner);pthread_mutex_destroy(&s->mutex);pthread_cond_destroy(&s->changed);free(s);}
static void *task_entry(void *p) {current_task=p;current_task->fn(current_task->arg);assert(false);return NULL;}
static int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out) {
    (void)name;(void)stack;(void)priority;if(fail_next_create){fail_next_create=false;return pdFALSE;}
    TaskHandle_t t=calloc(1,sizeof(*t));assert(t&&task_count<128);t->fn=fn;t->arg=arg;tasks[task_count++]=t;*out=t;
    assert(!pthread_create(&t->thread,NULL,task_entry,t));return pdPASS;
}
static void vTaskDelete(TaskHandle_t handle) {assert(!handle);assert(current_task&&current_task->held==0);pthread_exit(NULL);}
static void vTaskDelay(unsigned ticks) {struct timespec t={ticks/1000,(ticks%1000)*1000000L};nanosleep(&t,NULL);}
static void vTaskDelayUntil(TickType_t *last,unsigned period) {TickType_t now=xTaskGetTickCount();*last+=period;if((int)(*last-now)>0)vTaskDelay(*last-now);}
static void *heap_caps_malloc(size_t n,unsigned caps) {(void)caps;return malloc(n);}
static void *heap_caps_calloc(size_t n,size_t z,unsigned caps) {(void)caps;return calloc(n,z);}
static void join_tasks(void) {for(int i=0;i<task_count;i++){assert(!pthread_join(tasks[i]->thread,NULL));free(tasks[i]);}task_count=0;}
#define WAIT_UNTIL(expression) do {int64_t end=esp_timer_get_time()+10000000;while(!(expression)){assert(esp_timer_get_time()<end);sched_yield();}} while(0)
#define TS_EVENT_BASE_SERVICE 1
#include "ts_service.h"
static atomic_int stopped_events;
static int ts_event_post(int base,int event,const void *data,size_t len,unsigned timeout) {(void)base;(void)data;(void)len;(void)timeout;if(event==TS_EVENT_SERVICE_STOPPED)++stopped_events;return ESP_OK;}
'''


def service_source():
    return re.sub(r'^#include[^\n]*\n', '', Path('components/ts_core/ts_service/src/ts_service.c').read_text(), flags=re.M)


def run_native(code, extra_sources=(), extra_headers=None):
    with tempfile.TemporaryDirectory(prefix='ts-local-lifecycle-') as directory:
        root = Path(directory)
        (root / 'test.c').write_text(code)
        (root / 'esp_err.h').write_text(Path('tests/runtime/rule_pack/stubs/esp_err.h').read_text() + '\n#define ESP_ERR_NOT_FINISHED 0x10c\n')
        for name, content in (extra_headers or {}).items():
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-Wno-deprecated-declarations', '-I' + str(root),
                        '-Itests/runtime/rule_pack/stubs', '-Icomponents/ts_core/ts_service/include',
                        '-Icomponents/ts_console/include', '-Icomponents/ts_drivers/include',
                        '-Icomponents/ts_api/include', '-I' + str(IDF / 'components/json/cJSON'),
                        '-I' + str(IDF / 'components/console/linenoise'),
                        str(root / 'test.c'), *map(str, extra_sources), '-pthread', '-lm', '-o', str(root / 'test')], check=True)
        subprocess.run([str(root / 'test')], cwd=root, check=True, timeout=45)


def main():
    prelude = RTOS + r'''
#include "ts_console.h"
#include "ts_service.h"
#include "linenoise.h"
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_SCLK_DEFAULT 0
#define ESP_LINE_ENDINGS_CR 1
#define ESP_LINE_ENDINGS_CRLF 2
typedef struct {int baud_rate,data_bits,parity,stop_bits,source_clk;} uart_config_t;
static atomic_int driver_installs,driver_deletes,uart_reads,consumed,commands;
static pthread_mutex_t input_lock=PTHREAD_MUTEX_INITIALIZER;
static char input[2048],last_command[128];static size_t input_read,input_write;
static SemaphoreHandle_t command_entered,command_release,line_entered,line_release;
static atomic_bool hold_line;
static int ts_i18n_init(void) {return ESP_OK;}
static int uart_driver_install(int n,int rx,int tx,int q,void *queue,int flags) {(void)n;(void)rx;(void)tx;(void)q;(void)queue;(void)flags;++driver_installs;return ESP_OK;}
static int uart_param_config(int n,const uart_config_t *c) {(void)n;(void)c;return ESP_OK;}
static int uart_driver_delete(int n) {(void)n;++driver_deletes;return ESP_OK;}
static void uart_vfs_dev_use_driver(int n) {(void)n;}
static void uart_vfs_dev_use_nonblocking(int n) {(void)n;}
static void uart_vfs_dev_port_set_rx_line_endings(int n,int e) {(void)n;(void)e;}
static void uart_vfs_dev_port_set_tx_line_endings(int n,int e) {(void)n;(void)e;}
static int uart_read_bytes(int n,void *out,size_t length,unsigned timeout) {
    (void)n;(void)length;++uart_reads;pthread_mutex_lock(&input_lock);
    if(input_read==input_write){pthread_mutex_unlock(&input_lock);vTaskDelay(timeout);return 0;}
    char c=input[input_read++];pthread_mutex_unlock(&input_lock);*(char *)out=c;++consumed;
    if(c=='\n'&&atomic_exchange(&hold_line,false)){xSemaphoreGive(line_entered);assert(xSemaphoreTake(line_release,portMAX_DELAY));}
    return 1;
}
static void feed(const char *text) {pthread_mutex_lock(&input_lock);size_t n=strlen(text);assert(input_write+n<sizeof(input));memcpy(input+input_write,text,n);input_write+=n;pthread_mutex_unlock(&input_lock);}
static void clear_input(void) {pthread_mutex_lock(&input_lock);input_read=input_write=0;pthread_mutex_unlock(&input_lock);}
int ts_console_printf(const char *format,...) {(void)format;return 0;}
int ts_console_error(const char *format,...) {(void)format;return 0;}
static int console_sdk_initializations,registrations;
static int esp_console_init(const esp_console_config_t *c) {(void)c;++console_sdk_initializations;return ESP_OK;}
static int esp_console_deinit(void) {return ESP_OK;}
static int esp_console_cmd_register(const esp_console_cmd_t *c) {(void)c;++registrations;return ESP_OK;}
static int esp_console_run(const char *line,int *result);
'''
    console = Path('components/ts_console/src/ts_console.c').read_text()
    console = console[:console.index('esp_err_t ts_console_exec(')]
    code = prelude + re.sub(r'^#include[^\n]*\n', '', console, flags=re.M)
    code += '\n#undef TAG\n#include "ts_service.h"\n' + service_source()
    code += r'''
static ts_service_handle_t console_service;
static int dummy_command(int argc,char **argv) {(void)argc;(void)argv;return 0;}
static int register_one(const char *name) {ts_console_cmd_t c={.command=name,.func=dummy_command};return ts_console_register_cmd(&c);}
int ts_console_register_builtin_cmds(void) {assert(register_one("help")==ESP_OK);return register_one("version");}
static int ts_cmd_register_all(void) {return register_one("service");}
static int esp_console_run(const char *line,int *result) {
    snprintf(last_command,sizeof(last_command),"%s",line);*result=0;++commands;
    if(!strcmp(line,"self-stop"))assert(ts_service_stop(console_service)==ESP_OK);
    else if(!strcmp(line,"self-restart"))assert(ts_service_restart(console_service)==ESP_OK);
    else if(!strcmp(line,"long")){xSemaphoreGive(command_entered);assert(xSemaphoreTake(command_release,portMAX_DELAY));}
    return ESP_OK;
}
'''
    for name in ['console_service_init', 'console_service_start', 'console_service_stop']:
        code += body('main/ts_services.c', name)
    code += r'''
#include "ts_api.h"
void ts_api_result_error(ts_api_result_t *r,ts_api_result_code_t code,const char *message) {r->code=code;r->message=(char *)message;}
void ts_api_result_ok(ts_api_result_t *r,cJSON *data) {r->code=TS_API_OK;r->data=data;}
'''
    for name in ['api_service_start', 'api_service_stop', 'api_service_restart']:
        code += body('components/ts_api/src/ts_api_service.c', name)
    code += r'''
static atomic_bool operation_done;static int operation_result;
static SemaphoreHandle_t tail_entered,tail_release;static atomic_bool hold_tail;
static void pause_tail(SemaphoreHandle_t signal) {if(signal==s_console.exit_done&&atomic_exchange(&hold_tail,false)){xSemaphoreGive(tail_entered);assert(xSemaphoreTake(tail_release,portMAX_DELAY));}}
static void *restart_thread(void *arg) {operation_result=arg?ts_console_deinit():ts_service_restart(console_service);operation_done=true;return NULL;}
static void *stop_thread(void *arg) {(void)arg;operation_result=ts_service_stop(console_service);operation_done=true;return NULL;}
static int callback_result;
static int controlled_callback(ts_service_handle_t h,void *u) {(void)h;(void)u;return callback_result;}
static bool command_finished(void) {xSemaphoreTake(s_console.mutex,portMAX_DELAY);bool done=!s_console.command_active;xSemaphoreGive(s_console.mutex);return done;}
static void command(const char *text) {int before=commands;feed(text);WAIT_UNTIL(commands>before);if(strcmp(text,"long\n"))WAIT_UNTIL(command_finished());}
int main(void) {
    command_entered=xSemaphoreCreateBinary();command_release=xSemaphoreCreateBinary();line_entered=xSemaphoreCreateBinary();line_release=xSemaphoreCreateBinary();
    tail_entered=xSemaphoreCreateBinary();tail_release=xSemaphoreCreateBinary();after_binary_give=pause_tail;
    assert(ts_service_init()==ESP_OK);
    for(int kind=0;kind<2;kind++) {
        ts_service_def_t probe={.name=kind?"busy-start":"busy-init"};
        if(kind)probe.start=controlled_callback;else probe.init=controlled_callback;
        ts_service_handle_t h;assert(ts_service_register(&probe,&h)==ESP_OK);
        callback_result=ESP_ERR_NOT_FINISHED;assert(ts_service_start(h)==ESP_ERR_NOT_FINISHED&&ts_service_get_state(h)==TS_SERVICE_STATE_REGISTERED);
        callback_result=ESP_FAIL;assert(ts_service_start(h)==ESP_FAIL&&ts_service_get_state(h)==TS_SERVICE_STATE_ERROR);
        callback_result=ESP_OK;assert(ts_service_start(h)==ESP_OK);assert(ts_service_stop(h)==ESP_OK);
    }
    ts_service_def_t definition={.name="console",.capabilities=TS_SERVICE_CAP_RESTARTABLE,.init=console_service_init,.start=console_service_start,.stop=console_service_stop};
    assert(ts_service_register(&definition,&console_service)==ESP_OK);assert(ts_service_start(console_service)==ESP_OK);
    command("help\r");assert(!strcmp(last_command,"help"));int count=ts_console_get_cmd_count();assert(count==3);
    for(int i=0;i<3;i++){assert(ts_service_restart(console_service)==ESP_OK);command("version\n");}
    assert(console_sdk_initializations==1&&registrations==3&&driver_installs==1&&ts_console_get_cmd_count()==count);
    int used=consumed;feed("he");WAIT_UNTIL(consumed>=used+2);int reads=uart_reads;WAIT_UNTIL(uart_reads>=reads+2);
    assert(ts_service_restart(console_service)==ESP_OK);command("help\n");assert(!strcmp(last_command,"help"));
    used=consumed;feed("versioX");WAIT_UNTIL(consumed>=used+7);reads=uart_reads;WAIT_UNTIL(uart_reads>=reads+2);command("\b n\b\bn\r\n");assert(!strcmp(last_command,"version"));
    assert(linenoiseHistorySave("history.txt")==0);FILE *history=fopen("history.txt","r");assert(history);char data[2048]={0};fread(data,1,sizeof(data)-1,history);fclose(history);assert(strstr(data,"help")&&strstr(data,"version"));
    command("self-restart\n");command("help\n");assert(ts_console_is_running());
    command("self-stop\n");WAIT_UNTIL(!ts_console_is_running());assert(ts_service_get_state(console_service)==TS_SERVICE_STATE_STOPPED);
    /* Join the self-stopping reader before starting another test round. */
    join_tasks();assert(ts_service_start(console_service)==ESP_OK);command("help\n");
    command("long\n");assert(xSemaphoreTake(command_entered,portMAX_DELAY));
    int events=stopped_events;assert(ts_service_restart(console_service)==ESP_ERR_NOT_FINISHED);
    assert(ts_service_get_state(console_service)==TS_SERVICE_STATE_RUNNING&&ts_console_is_running()&&stopped_events==events);
    xSemaphoreGive(command_release);command("help\n");
    /* A line arrives while an external stop owns the input loop. */
    int before=commands;hold_line=true;feed("version\n");assert(xSemaphoreTake(line_entered,portMAX_DELAY));
    pthread_t stop;operation_done=false;assert(!pthread_create(&stop,NULL,stop_thread,NULL));WAIT_UNTIL(!ts_console_is_running());
    assert(!operation_done);assert(ts_service_start(console_service)==ESP_ERR_NOT_FINISHED);assert(ts_service_stop(console_service)==ESP_ERR_NOT_FINISHED);
    cJSON *params=cJSON_Parse("{\"name\":\"console\"}");ts_api_result_t result={0};
    assert(api_service_start(params,&result)==ESP_ERR_NOT_FINISHED&&result.code==TS_API_ERR_BUSY);
    assert(api_service_stop(params,&result)==ESP_ERR_NOT_FINISHED&&result.code==TS_API_ERR_BUSY);
    assert(api_service_restart(params,&result)==ESP_ERR_NOT_FINISHED&&result.code==TS_API_ERR_BUSY);cJSON_Delete(params);
    xSemaphoreGive(line_release);
    assert(!pthread_join(stop,NULL));assert(operation_result==ESP_OK&&commands==before);clear_input();
    fail_next_create=true;assert(ts_service_start(console_service)==ESP_ERR_NO_MEM);assert(!ts_console_is_running());
    assert(ts_service_start(console_service)==ESP_OK);command("help\n");
    /* Deinit holds ownership until the old read returns and resources are released. */
    hold_line=true;feed("version\n");assert(xSemaphoreTake(line_entered,portMAX_DELAY));
    operation_done=false;hold_tail=true;assert(!pthread_create(&stop,NULL,restart_thread,(void *)1));WAIT_UNTIL(!ts_console_is_running());
    assert(ts_console_start()==ESP_ERR_NOT_FINISHED&&!operation_done);xSemaphoreGive(line_release);
    assert(xSemaphoreTake(tail_entered,portMAX_DELAY));assert(!operation_done);xSemaphoreGive(tail_release);
    assert(!pthread_join(stop,NULL));assert(operation_result==ESP_OK);
    assert(!ts_console_is_initialized()&&driver_deletes==1);join_tasks();
    assert(ts_console_init(NULL)==ESP_OK);assert(ts_console_deinit()==ESP_OK);
    vSemaphoreDelete(command_entered);vSemaphoreDelete(command_release);vSemaphoreDelete(line_entered);vSemaphoreDelete(line_release);
    vSemaphoreDelete(tail_entered);vSemaphoreDelete(tail_release);
    assert(ts_service_deinit()==ESP_OK);
    puts("PASS actual console/service/SDK linenoise: idle, slow/partial input, history, stop/restart, busy, failure, handoff");
}
'''
    headers = {
        'esp_console.h': '#pragma once\n#include <stddef.h>\ntypedef int (*esp_console_cmd_func_t)(int,char **);\ntypedef struct {int max_cmdline_args,max_cmdline_length,hint_color,hint_bold;} esp_console_config_t;\ntypedef struct {const char *command,*help,*hint;esp_console_cmd_func_t func;void *argtable;} esp_console_cmd_t;\n',
        'sdkconfig.h': '#define CONFIG_CONSOLE_UART_DEFAULT 1\n#define CONFIG_IDF_TARGET_LINUX 1\n',
    }
    run_native(code, [IDF / 'components/console/linenoise/linenoise.c', IDF / 'components/json/cJSON/cJSON.c'], headers)


if __name__ == '__main__':
    main()
