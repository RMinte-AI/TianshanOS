"""Actual SD OTA/abort lifecycle, synthetic local image and non-flashing SDK boundaries."""
from pathlib import Path
import argparse
from test_console_lifecycle import RTOS, IDF, body, run_native

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root',type=Path,default=Path('.'))
args=parser.parse_args()
CORE='components/ts_ota/src/ts_ota.c'
SD='components/ts_ota/src/ts_ota_sdcard.c'


def function(path,name):
    original=args.source_root/path
    return body(original if original.exists() else path,name)


rtos=RTOS.replace('#define TS_EVENT_BASE_SERVICE 1',r'''
#define TS_EVENT_BASE_SERVICE 1
#define TS_EVENT_BASE_OTA 9
static atomic_int ota_events[9];static void (*ota_event_hook)(int);
''').replace('if(event==TS_EVENT_SERVICE_STOPPED)++stopped_events;',
'''if(base==TS_EVENT_BASE_OTA){assert(event>=0&&event<9);++ota_events[event];if(ota_event_hook)ota_event_hook(event);}else if(event==TS_EVENT_SERVICE_STOPPED)++stopped_events;''')
code=rtos+r'''
#include <ctype.h>
#include <sys/stat.h>
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "ts_ota.h"
#include "ts_api.h"
#define TAG "local-ota"
#define OTA_MALLOC heap_alloc
#define TS_STRDUP_PSRAM strdup
#define CONFIG_TS_OTA_BUFFER_SIZE 512
#define CONFIG_TS_OTA_TASK_STACK_SIZE 4096
#define CONFIG_TS_OTA_TASK_PRIORITY 5
#define CONFIG_TS_OTA_VERSION_CHECK 1
#define OTA_WITH_SEQUENTIAL_WRITES 0
#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503
#define ESP_ERR_INVALID_VERSION 0x1504
typedef uint32_t esp_ota_handle_t;
static void *heap_alloc(size_t n) {return malloc(n);}
static void sdcard_ota_task(void *);
static SemaphoreHandle_t paused,resume_worker;
enum {PAUSE_NONE,PAUSE_QUEUED,PAUSE_READ,PAUSE_WRITE,PAUSE_END,PAUSE_BOOT,PAUSE_COMPLETED};
static int pause_stage;static atomic_bool pause_once;
static bool finish_before_create_returns,https_running;static int https_aborts,flash_aborts,restarts,boot_choices,begin_calls,end_calls;
static uint8_t written_bytes[2048];static size_t written_size;
static const esp_partition_t target_partition={.label="ota_fixture",.address=0x360000,.size=0x300000};
static esp_app_desc_t running_app={.magic_word=ESP_APP_DESC_MAGIC_WORD,.version="0.6.3",.project_name="TianShanOS"};
static void maybe_pause(int stage) {if(pause_stage==stage&&atomic_exchange(&pause_once,false)){xSemaphoreGive(paused);assert(xSemaphoreTake(resume_worker,portMAX_DELAY));}}
static FILE *local_open(const char *path,const char *mode) {FILE *f=fopen(path,mode);if(current_task)maybe_pause(PAUSE_READ);return f;}
static const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) {(void)p;return &target_partition;}
const esp_app_desc_t *esp_app_get_description(void) {return &running_app;}
static int esp_ota_begin(const esp_partition_t *p,size_t n,esp_ota_handle_t *out) {assert(p==&target_partition&&n==OTA_WITH_SEQUENTIAL_WRITES);*out=1;++begin_calls;return ESP_OK;}
static int esp_ota_write(esp_ota_handle_t h,const void *data,size_t n) {assert(h==1&&written_size+n<=sizeof(written_bytes));maybe_pause(PAUSE_WRITE);memcpy(written_bytes+written_size,data,n);written_size+=n;return ESP_OK;}
static int esp_ota_end(esp_ota_handle_t h) {assert(h==1);maybe_pause(PAUSE_END);++end_calls;return ESP_OK;}
static int esp_ota_set_boot_partition(const esp_partition_t *p) {assert(p==&target_partition);++boot_choices;maybe_pause(PAUSE_BOOT);return ESP_OK;}
static int esp_ota_abort(esp_ota_handle_t h) {assert(h);++flash_aborts;return ESP_OK;}
static void esp_restart(void) {++restarts;}
bool ts_ota_https_is_running(void) {return https_running;}
void ts_ota_abort_https(void) {++https_aborts;}
esp_err_t ts_ota_start_https(const ts_ota_config_t *config) {(void)config;assert(false);return ESP_FAIL;}
esp_err_t ts_ota_download_firmware(const char *url,bool skip,ts_ota_progress_cb_t cb,void *ctx,bool reboot) {(void)url;(void)skip;(void)cb;(void)ctx;(void)reboot;assert(false);return ESP_FAIL;}
static void event_pause(int event) {if(event==TS_EVENT_OTA_COMPLETED)maybe_pause(PAUSE_COMPLETED);}
struct sd_job {void (*fn)(void *);void *arg;};
static void sd_proxy(void *p) {struct sd_job job=*(struct sd_job*)p;free(p);maybe_pause(PAUSE_QUEUED);job.fn(job.arg);}
static int create_sd_task(void (*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out) {struct sd_job *job=malloc(sizeof(*job));*job=(struct sd_job){fn,arg};int result=xTaskCreate(sd_proxy,name,stack,job,priority,out);if(result!=pdPASS)free(job);else if(finish_before_create_returns)WAIT_UNTIL(ota_events[TS_EVENT_OTA_COMPLETED]>0&&!ts_ota_sdcard_is_running());return result;}
'''
core=Path(CORE).read_text()
code+=core[core.index('static bool s_initialized'):core.index('// Rollback timer')]
for name in ['ota_set_state','ts_ota_get_progress','ota_notify_progress','ts_ota_update_progress','ts_ota_set_error','ts_ota_set_completed','ts_ota_abort']:
    code+=function(CORE,name)
sd=Path(SD).read_text()
code+=sd[sd.index('static TaskHandle_t s_ota_task_handle'):sd.index('// Forward declarations')]
code+='#define xTaskCreate create_sd_task\n'+function(SD,'ts_ota_start_sdcard')+'\n#undef xTaskCreate\n'
code+=function(SD,'ts_ota_sdcard_is_running')
for name in ['parse_version','ts_ota_compare_versions','ts_ota_start']:
    code+=function('components/ts_ota/src/ts_ota_version.c',name)
code+='#define fopen local_open\n'+function(SD,'sdcard_ota_task')+'\n#undef fopen\n'
for name in ['ts_api_result_init','ts_api_result_free','ts_api_result_ok','ts_api_result_error']:
    code+=function('components/ts_api/src/ts_api.c',name)
for name in ['api_ota_start_file','api_ota_abort']:
    code+=function('components/ts_api/src/ts_api_ota.c',name)
code+=r'''
static uint8_t firmware[1536];
static void make_firmware(void) {
    memset(firmware,0x5a,sizeof(firmware));esp_image_header_t header={.magic=ESP_IMAGE_HEADER_MAGIC,.segment_count=1};esp_image_segment_header_t segment={.data_len=sizeof(firmware)-sizeof(header)-sizeof(segment)};esp_app_desc_t app=running_app;
    memcpy(firmware,&header,sizeof(header));memcpy(firmware+sizeof(header),&segment,sizeof(segment));memcpy(firmware+sizeof(header)+sizeof(segment),&app,sizeof(app));FILE *f=fopen("normal.bin","wb");assert(f);assert(fwrite(firmware,1,sizeof(firmware),f)==sizeof(firmware));assert(!fclose(f));
}
static void reset_fixture(void) {
    assert(!s_ota_running&&!s_ota_task_handle&&!s_ota_url);s_state=TS_OTA_STATE_IDLE;s_error=TS_OTA_ERR_NONE;s_total_size=s_received_size=0;strcpy(s_status_msg,"idle");s_upload_handle=0;s_upload_in_progress=false;https_running=false;https_aborts=flash_aborts=restarts=boot_choices=begin_calls=end_calls=0;written_size=0;pause_stage=PAUSE_NONE;pause_once=false;finish_before_create_returns=false;
    for(int i=0;i<9;i++)ota_events[i]=0;
}
static void refused_without_mutation(void) {
    ts_ota_progress_t before,after;assert(ts_ota_get_progress(&before)==ESP_OK);char message[64];strcpy(message,before.status_msg);int events[9];for(int i=0;i<9;i++)events[i]=ota_events[i];
    ts_api_result_t r={0};assert(api_ota_abort(NULL,&r)==ESP_ERR_NOT_SUPPORTED&&r.code!=TS_API_OK&&!cJSON_HasObjectItem(r.data,"aborted"));ts_api_result_free(&r);assert(ts_ota_get_progress(&after)==ESP_OK);
    assert(after.state==before.state&&after.error==before.error&&after.total_size==before.total_size&&after.received_size==before.received_size&&!strcmp(after.status_msg,message));for(int i=0;i<9;i++)assert(ota_events[i]==events[i]);assert(!flash_aborts&&!https_aborts);
}
static void start_from_api(void) {cJSON *params=cJSON_Parse("{\"file\":\"normal.bin\",\"no_reboot\":true}");ts_api_result_t r={0};assert(api_ota_start_file(params,&r)==ESP_OK&&r.code==TS_API_OK&&cJSON_IsTrue(cJSON_GetObjectItem(r.data,"started")));cJSON_Delete(params);ts_api_result_free(&r);}
static void complete_worker(void) {join_tasks();assert(!s_ota_running&&!s_ota_task_handle&&!s_ota_url&&s_ota_config.url==NULL);assert(s_state==TS_OTA_STATE_PENDING_REBOOT&&s_error==TS_OTA_ERR_NONE);assert(begin_calls==1&&end_calls==1&&boot_choices==1&&!flash_aborts&&written_size==sizeof(firmware)&&!memcmp(written_bytes,firmware,sizeof(firmware)));assert(!ota_events[TS_EVENT_OTA_ABORTED]);}
int main(void) {
    s_ota_mutex=xSemaphoreCreateMutex();paused=xSemaphoreCreateBinary();resume_worker=xSemaphoreCreateBinary();ota_event_hook=event_pause;make_firmware();
    for(int stage=PAUSE_QUEUED;stage<=PAUSE_COMPLETED;stage++) {
        reset_fixture();pause_stage=stage;pause_once=true;start_from_api();assert(xSemaphoreTake(paused,portMAX_DELAY));assert(ts_ota_sdcard_is_running());refused_without_mutation();xSemaphoreGive(resume_worker);complete_worker();assert(!restarts);refused_without_mutation();
    }
    reset_fixture();fail_next_create=true;ts_ota_config_t config={.source=TS_OTA_SOURCE_SDCARD,.url="normal.bin",.auto_reboot=false};assert(ts_ota_start(&config)==ESP_ERR_NO_MEM&&!s_ota_running&&!s_ota_url&&!s_ota_config.url);start_from_api();complete_worker();assert(!restarts);
    /* A fast worker may finish before xTaskCreate returns. It must stay stopped. */
    reset_fixture();finish_before_create_returns=true;start_from_api();complete_worker();assert(!restarts);
    /* The original CLI-style auto_reboot choice is unchanged after rejection. */
    reset_fixture();pause_stage=PAUSE_WRITE;pause_once=true;config.auto_reboot=true;assert(ts_ota_start(&config)==ESP_OK);assert(xSemaphoreTake(paused,portMAX_DELAY));refused_without_mutation();xSemaphoreGive(resume_worker);complete_worker();assert(restarts==1);
    reset_fixture();assert(ts_ota_abort()==ESP_OK&&s_state==TS_OTA_STATE_IDLE&&!ota_events[TS_EVENT_OTA_ABORTED]);
    s_state=TS_OTA_STATE_ERROR;s_error=TS_OTA_ERR_WRITE_FAILED;assert(ts_ota_abort()==ESP_OK&&s_state==TS_OTA_STATE_IDLE&&s_error==TS_OTA_ERR_ABORTED&&ota_events[TS_EVENT_OTA_ABORTED]==1);
    reset_fixture();s_state=TS_OTA_STATE_WRITING;https_running=true;assert(ts_ota_abort()==ESP_OK&&https_aborts==1&&s_state==TS_OTA_STATE_IDLE&&ota_events[TS_EVENT_OTA_ABORTED]==1);
    reset_fixture();s_state=TS_OTA_STATE_WRITING;s_upload_handle=99;s_upload_in_progress=true;ts_api_result_t r={0};assert(api_ota_abort(NULL,&r)==ESP_OK&&r.code==TS_API_OK&&cJSON_IsTrue(cJSON_GetObjectItem(r.data,"aborted")));ts_api_result_free(&r);assert(!s_upload_handle&&!s_upload_in_progress&&flash_aborts==1&&s_state==TS_OTA_STATE_IDLE&&ota_events[TS_EVENT_OTA_ABORTED]==1);
    vSemaphoreDelete(s_ota_mutex);vSemaphoreDelete(paused);vSemaphoreDelete(resume_worker);
    puts("PASS actual SD task/API abort: queued/read/write/verify/boot/completed refuse unchanged; local bytes/boot result, no_reboot, create failure/fast exit, original idle/error/HTTPS/upload paths");
}
'''
headers={'ts_ota.h':Path('components/ts_ota/include/ts_ota.h').read_text(),
 'esp_partition.h':'#pragma once\n#include <stdint.h>\ntypedef struct {char label[17];uint32_t address,size;} esp_partition_t;\n',
 'esp_app_format.h':(IDF/'components/bootloader_support/include/esp_app_format.h').read_text(),
 'esp_app_desc.h':(IDF/'components/esp_app_format/include/esp_app_desc.h').read_text(),
 'esp_assert.h':'#pragma once\n#define ESP_STATIC_ASSERT _Static_assert\n',
 'esp_attr.h':'#pragma once\n#define FORCE_INLINE_ATTR static inline\n'}
run_native(code,[IDF/'components/json/cJSON/cJSON.c'],headers)
