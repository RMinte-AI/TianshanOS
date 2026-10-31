"""Actual memory-detail API with local SDK snapshot/heap/NVS fixtures."""
from test_console_lifecycle import body, run_native, IDF

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_api.h"
typedef unsigned UBaseType_t;
typedef void *TaskHandle_t;
typedef uint8_t StackType_t; /* Locked SDK Xtensa portmacro.h:88. */
typedef enum {eRunning,eReady,eBlocked,eSuspended,eDeleted} eTaskState;
typedef struct {
 void *xHandle;const char *pcTaskName;UBaseType_t uxCurrentPriority;
 uint32_t ulRunTimeCounter;StackType_t *pxStackBase;unsigned usStackHighWaterMark;
 int xCoreID;eTaskState eCurrentState;
} TaskStatus_t;
typedef struct {size_t allocated_blocks,free_blocks,total_blocks;} multi_heap_info_t;
typedef struct {size_t used_entries,free_entries,total_entries,namespace_count;} nvs_stats_t;
enum {MALLOC_CAP_INTERNAL=1,MALLOC_CAP_SPIRAM=2,MALLOC_CAP_DMA=4,MALLOC_CAP_32BIT=8,MALLOC_CAP_EXEC=16,MALLOC_CAP_8BIT=32,MALLOC_CAP_DEFAULT=64};
#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 4096
#define CONFIG_ESP_TIMER_TASK_STACK_SIZE 4096
#define CONFIG_ESP_IPC_TASK_STACK_SIZE 2048
#define CONFIG_FREERTOS_IDLE_TASK_STACKSIZE 1536
#define CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH 2048
#define configGENERATE_RUN_TIME_STATS 1
static uint32_t sections[32];
#define _data_start sections[0]
#define _data_end sections[4]
#define _bss_start sections[4]
#define _bss_end sections[8]
#define _rodata_start sections[8]
#define _rodata_end sections[16]
#define _iram_text_start sections[16]
#define _iram_text_end sections[24]
#define _rtc_data_start sections[24]
#define _rtc_data_end sections[28]
#define _rtc_bss_start sections[28]
#define _rtc_bss_end sections[32]
static size_t heap_caps_get_total_size(unsigned caps){return caps&MALLOC_CAP_SPIRAM?8000:4000;}
static size_t heap_caps_get_free_size(unsigned caps){return caps&MALLOC_CAP_SPIRAM?6000:1000;}
static size_t heap_caps_get_largest_free_block(unsigned caps){return caps&MALLOC_CAP_SPIRAM?5000:750;}
static size_t heap_caps_get_minimum_free_size(unsigned caps){return caps&MALLOC_CAP_SPIRAM?5500:900;}
static void heap_caps_get_info(multi_heap_info_t *out,unsigned caps){(void)caps;*out=(multi_heap_info_t){3,4,7};}
static size_t esp_get_minimum_free_heap_size(void){return 555;}
static int nvs_get_stats(const char *name,nvs_stats_t *out){(void)name;*out=(nvs_stats_t){10,90,100,2};return ESP_OK;}
static unsigned announced,actual,snapshots,allocations,releases;
static TaskStatus_t fixture[4];
static void *task_allocation;
static UBaseType_t uxTaskGetNumberOfTasks(void){return announced;}
static UBaseType_t uxTaskGetSystemState(TaskStatus_t *out,UBaseType_t capacity,uint32_t *runtime){
 ++snapshots;assert(capacity==announced);
 if(!actual)return 0; /* Capacity-short SDK path need not write runtime/array. */
 assert(actual<=capacity);memcpy(out,fixture,actual*sizeof(*out));*runtime=100;return actual;
}
static void *task_malloc(size_t size){assert(!task_allocation&&size==announced*sizeof(TaskStatus_t));++allocations;return task_allocation=malloc(size);}
static void task_free(void *p){assert(p==task_allocation);++releases;free(p);task_allocation=NULL;}
void ts_api_result_ok(ts_api_result_t *result,cJSON *data){result->code=TS_API_OK;result->data=data;}
void ts_api_result_error(ts_api_result_t *result,ts_api_result_code_t code,const char *message){(void)message;result->code=code;}
#define TS_MALLOC_PSRAM task_malloc
#define free task_free
'''
code += body('components/ts_api/src/ts_api_system.c', 'api_system_memory_detail')
code += r'''
#undef free
static cJSON *get(cJSON *json,const char *name){cJSON *value=cJSON_GetObjectItemCaseSensitive(json,name);assert(value);return value;}
static void number(cJSON *json,const char *key,double expected){assert(cJSON_GetNumberValue(get(json,key))==expected);}
static void request(unsigned count,unsigned returned,const char **expected){
 announced=count;actual=returned;snapshots=allocations=releases=0;ts_api_result_t result={0};
 assert(api_system_memory_detail(NULL,&result)==ESP_OK&&result.code==TS_API_OK&&result.data);
 /* Assert the serialized API payload, not just intermediate task structs. */
 char *wire=cJSON_PrintUnformatted(result.data);assert(wire);cJSON *data=cJSON_Parse(wire);assert(data);free(wire);cJSON_Delete(result.data);
 number(get(data,"dram"),"total",4000);number(get(data,"dram"),"free",1000);
 number(get(data,"dram"),"used",3000);number(get(data,"psram"),"total",8000);number(get(data,"psram"),"free",6000);
 number(get(data,"history"),"min_free_heap_ever",555);number(get(data,"nvs"),"used_entries",10);
 assert(cJSON_IsArray(get(data,"tips")));assert(!task_allocation);
 if(!count){
  assert(!snapshots&&!allocations&&!releases&&!cJSON_GetObjectItemCaseSensitive(data,"tasks"));
 }else{
  assert(snapshots==1&&allocations==1&&releases==1);
  cJSON *tasks=get(data,"tasks");assert(cJSON_IsArray(tasks)&&cJSON_GetArraySize(tasks)==(int)returned);
  number(data,"task_count",returned);
  if(!returned)number(data,"total_stack_allocated",0);
  for(unsigned i=0;i<returned;i++){
   cJSON *task=cJSON_GetArrayItem(tasks,i);assert(!strcmp(cJSON_GetStringValue(get(task,"name")),expected[i]));
   TaskStatus_t *original=NULL;
   for(unsigned j=0;j<returned;j++)if(!strcmp(fixture[j].pcTaskName,expected[i]))original=&fixture[j];
   assert(original);number(task,"stack_hwm",original->usStackHighWaterMark);number(task,"priority",original->uxCurrentPriority);
   number(task,"core",original->xCoreID);number(task,"cpu_percent",original->ulRunTimeCounter);
   assert(!strcmp(cJSON_GetStringValue(get(task,"state")),"Ready"));
  }
 }
 cJSON_Delete(data);
}
int main(void){
 static StackType_t stacks[4][4096];
 const char *names[]={"main","low","equal","middle"};unsigned water[]={600,100,100,400};
 for(unsigned i=0;i<4;i++)fixture[i]=(TaskStatus_t){(void *)(uintptr_t)(i+1),names[i],i+1,(i+1)*10,stacks[i],water[i],(int)(i%2),eReady};
 request(0,0,NULL);
 request(2,0,NULL); /* New tasks may make the counted capacity insufficient. */
 const char *one[]={"main"};request(2,1,one);
 const char *many[]={"low","equal","middle","main"};request(4,4,many);
 request(2,0,NULL);request(4,4,many);request(1,1,one);
 puts("PASS actual memory-detail JSON: zero/one/multiple snapshots, stable stack sorting and original metadata/heap fields");
 puts("PASS capacity-short empty snapshot reports zero/empty tasks, then ordinary refresh works without retries or locks");
}
'''
run_native(code, [IDF / 'components/json/cJSON/cJSON.c'])
