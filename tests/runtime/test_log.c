#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ts_log_preview.h"
#define TS_LOG_TAG_MAX_LEN 16
#define TS_LOG_MSG_MAX_LEN 256
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
#define TS_LOG_ERROR 1
#define TS_LOG_WARN 2
#define TS_LOG_INFO 3
#define TS_LOG_DEBUG 4
#define TS_LOG_VERBOSE 5
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
typedef void *TaskHandle_t;
typedef int (*vprintf_like_t)(const char *,va_list);
typedef struct {uint32_t timestamp_ms;int level;char tag[16],message[256],task_name[16];} ts_log_entry_t;
static void *xTaskGetCurrentTaskHandle(void) {return (void *)(uintptr_t)pthread_self();}
static const char *pcTaskGetName(void *p) {(void)p;return "test";}
static int64_t esp_timer_get_time(void) {return 123000;}
static int xSemaphoreTake(pthread_mutex_t *p,unsigned t) {return (t?pthread_mutex_lock(p):pthread_mutex_trylock(p))==0;}
static void xSemaphoreGive(pthread_mutex_t *p) {pthread_mutex_unlock(p);}
typedef int ts_log_level_t;
typedef int esp_log_level_t;
#define TS_LOG_MAX 6
#define ESP_LOG_ERROR TS_LOG_ERROR
#define ESP_LOG_WARN TS_LOG_WARN
#define ESP_LOG_INFO TS_LOG_INFO
#define ESP_LOG_DEBUG TS_LOG_DEBUG
#define ESP_LOG_VERBOSE TS_LOG_VERBOSE
#define TS_LOG_OUTPUT_CONSOLE 1
#define TS_LOG_OUTPUT_FILE 2
#define TS_LOG_OUTPUT_BUFFER 4
#define LOG_RESET_COLOR ""
typedef struct ts_log_tag_level {char tag[16];int level;struct ts_log_tag_level *next;} ts_log_tag_level_t;
typedef struct ts_log_callback_node {unsigned readers;void (*callback)(const ts_log_entry_t*,void*);void *user_data;int min_level;struct ts_log_callback_node *next;} ts_log_callback_node_t;
static TaskHandle_t callback_tasks[8];
static pthread_mutex_t io = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *s_log_io_mutex = &io;
static const char *ts_log_level_color(int level) {return "";}
static void log_output_file(const ts_log_entry_t *entry) {assert(0);}
static vprintf_like_t installed_hook;
static vprintf_like_t esp_log_set_vprintf(vprintf_like_t next) {vprintf_like_t old=installed_hook; installed_hook=next;return old;}
static void esp_log_writev(int l,const char*t,const char*f,va_list a) {installed_hook(f,a);}
static struct {
 _Atomic bool initialized,esp_log_capture_enabled;
 _Atomic(vprintf_like_t) original_vprintf;
 pthread_mutex_t *mutex;
 unsigned active_writers,output_mask;
 bool task_name_enabled,timestamp_enabled,colors_enabled;
 int global_level;
 ts_log_tag_level_t *tag_levels;
 ts_log_callback_node_t *callbacks;
 struct {ts_log_entry_t *entries;size_t head,count,capacity;} buffer;
 unsigned logs_dropped,total_logs_captured;
 _Atomic unsigned capture_dropped,capture_truncated;
} s_log_ctx;
static pthread_mutex_t s_capture_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { bool busy;char text[512];ts_log_entry_t entry;} s_capture[2];
static char console_output[16384];
static size_t console_size;
static int sink_vprintf(const char *format,va_list args) {
 assert(pthread_mutex_trylock(s_log_ctx.mutex)==0);pthread_mutex_unlock(s_log_ctx.mutex);
 int n=vsnprintf(console_output+console_size,sizeof(console_output)-console_size,format,args);
 assert(n>=0 && (size_t)n<sizeof(console_output)-console_size);console_size+=n;return n;
}
static int sink_printf(const char *format,...) {va_list a;va_start(a,format);int n=sink_vprintf(format,a);va_end(a);return n;}
#define printf sink_printf
#define vprintf sink_vprintf
#include "log_functions.inc"
#undef printf
#undef vprintf
static void own_emit(const char *format,...) {va_list a;va_start(a,format);ts_log_v(TS_LOG_INFO,"own",format,a);va_end(a);}
static unsigned callback_count;
static void recursive_callback(const ts_log_entry_t *entry,void *unused) {++callback_count;own_emit("callback record");}

static _Atomic unsigned output_calls;
static int output(const char *fmt,va_list args) {
 char text[8192];int n=vsnprintf(text,sizeof(text),fmt,args);
 assert(n>0 && (size_t)n<sizeof(text));assert(strlen(text)==(size_t)n);
 atomic_fetch_add(&output_calls,1);return n;
}
static int emit(const char *fmt,...) {va_list a;va_start(a,fmt);int n=ts_log_vprintf_hook(fmt,a);va_end(a);return n;}
static void *thread(void *arg) {(void)arg;for(int i=0;i<1000;++i)emit("I (1) tag: worker=%d m\n",i);return NULL;}
int main(void) {
 pthread_mutex_t ring=PTHREAD_MUTEX_INITIALIZER;ts_log_entry_t entries[16]={0};
 s_log_ctx.mutex=&ring;s_log_ctx.initialized=true;s_log_ctx.esp_log_capture_enabled=true;
 s_log_ctx.original_vprintf=output;s_log_ctx.buffer.entries=entries;s_log_ctx.buffer.capacity=16;
 const size_t lengths[]={510,511,512,1024,4096};char text[4097];
 for(unsigned i=0;i<5;++i){memset(text,'x',lengths[i]);text[lengths[i]]=0;assert(emit("%s",text)==(int)lengths[i]);assert(strlen(entries[i].message)>0);assert(strstr(entries[i].message," ..."));}
 emit("\033[32mI (1) tag: room\033[0m\n");assert(!strcmp(entries[5].message,"room"));
 char utf8[8];assert(ts_log_preview(utf8,sizeof(utf8),"你好你好",false));assert(!strcmp(utf8,"你 ..."));
 emit("\n");
 s_capture[0].busy=s_capture[1].busy=true;unsigned before=s_log_ctx.capture_dropped;
 emit("slot exhausted\n");assert(s_log_ctx.capture_dropped==before+1);s_capture[0].busy=s_capture[1].busy=false;
 pthread_t threads[4];for(int i=0;i<4;++i)pthread_create(&threads[i],NULL,thread,NULL);
 for(int i=0;i<4;++i)pthread_join(threads[i],NULL);
 assert(output_calls==4008);assert(s_log_ctx.capture_truncated==5);
 s_log_ctx.global_level=TS_LOG_VERBOSE;s_log_ctx.output_mask=TS_LOG_OUTPUT_CONSOLE|TS_LOG_OUTPUT_BUFFER;
 memset(text,'z',4096);text[4096]=0;unsigned head=s_log_ctx.buffer.head;
 own_emit("%s",text);assert(strlen(console_output)==strlen("I own: ")+4096+1);assert(s_log_ctx.buffer.head==(head+1)%16);
 assert(strstr(entries[head].message," ..."));
 console_size=0;console_output[0]=0;ts_log_callback_node_t callback={.callback=recursive_callback,.min_level=TS_LOG_VERBOSE};s_log_ctx.callbacks=&callback;
 own_emit("outer record");assert(callback_count==1&&callback.readers==0&&s_log_ctx.active_writers==0);s_log_ctx.callbacks=NULL;
 installed_hook=ts_log_vprintf_hook;head=s_log_ctx.buffer.head;ts_log_enable_esp_capture(false);emit("disabled capture\n");assert(s_log_ctx.buffer.head==head);ts_log_enable_esp_capture(true);emit("enabled capture\n");assert(s_log_ctx.buffer.head==(head+1)%16);
 puts("PASS actual log hook/parser: full 510..4096 output, bounded UTF-8 preview, ANSI, concurrent capture/drop, full own output, callback recursion, capture toggle");
}
