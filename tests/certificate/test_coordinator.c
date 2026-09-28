#define main lifecycle_suite
#include "test_lifecycle.c"
#undef main
#include <stdatomic.h>
#include <unistd.h>
#include "../../main/ts_https_retry.h"
typedef void *ts_service_handle_t;
typedef void *ts_event_handler_handle_t;
typedef struct {int id;} ts_event_t;
typedef pthread_t *TaskHandle_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define TS_EVENT_BASE_TIME "time"
#define TS_EVENT_TIME_SYNCED 1
static pthread_t worker;
static _Atomic int64_t simulated_ms;
static _Atomic bool allowed;
static _Atomic unsigned created_tasks,subscribed;
static int64_t esp_timer_get_time(void) {return atomic_load(&simulated_ms)*1000;}
static void vTaskDelay(unsigned ticks) {(void)ticks;usleep(1000);}
static unsigned ulTaskNotifyTake(int clear,unsigned ticks) {(void)clear;(void)ticks;usleep(1000);return 0;}
static void xTaskNotifyGive(TaskHandle_t t) {(void)t;}
static void (*task_fn)(void *);
static void *task_entry(void *arg) {(void)arg;task_fn(NULL);return NULL;}
static BaseType_t xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out) {
    (void)name;(void)stack;(void)arg;(void)priority;task_fn=fn;*out=&worker;created_tasks++;assert(!pthread_create(&worker,NULL,task_entry,NULL));return pdPASS;
}
static int ts_event_register(const char *base,int id,void (*handler)(const ts_event_t *,void *),void *data,ts_event_handler_handle_t *out) {
    (void)base;(void)id;(void)handler;(void)data;*out=(void *)1;subscribed++;return 0;
}
static int ts_event_unregister(ts_event_handler_handle_t h) {(void)h;subscribed--;return 0;}
static int default_error;
esp_err_t ts_https_register_default_api(void) {if(default_error)return default_error;s_endpoint_count=1;s_endpoints[0]=(ts_https_endpoint_t){.uri="/api/auth/whoami"};return 0;}
esp_err_t ts_cert_get_status(ts_cert_pki_status_t *out) {memset(out,0,sizeof(*out));out->generation=stored_generation;out->time_ready=atomic_load(&allowed);return 0;}
bool ts_cert_prerequisites(const ts_cert_pki_status_t *s,bool ca_required) {(void)ca_required;return s->time_ready;}
#include "coordinator.inc"
static void settle(void) {usleep(30000);}
static void wait_running(bool running) {for(int i=0;i<1000;++i){if(ts_https_is_running()==running)return;usleep(1000);}assert(!"coordinator timeout");}
static void *stop_request(void *unused) {(void)unused;assert(https_service_stop(NULL,NULL)==0);return NULL;}
int main(void) {
    assert(https_service_init(NULL,NULL)==0);assert(https_service_init(NULL,NULL)==0);assert(created_tasks==1&&subscribed==2);
    assert(https_service_start(NULL,NULL)==0);settle();assert(!ts_https_is_running());
    // No event: low-frequency fallback notices the final prerequisite.
    atomic_store(&allowed,true);simulated_ms=5000;wait_running(true);
    unsigned count=starts;
    for(int i=0;i<100;++i)https_conditions_changed(NULL,NULL);settle();assert(starts==count);
    stored_generation=2;settle();ts_https_runtime_t r;ts_https_get_runtime(&r);assert(r.loaded_generation==1);
    assert(https_service_stop(NULL,NULL)==0);assert(!ts_https_is_running());
    for(int i=0;i<100;++i)https_conditions_changed(NULL,NULL);simulated_ms=100000;settle();assert(starts==count);
    start_error=ESP_FAIL;assert(https_service_start(NULL,NULL)==0);settle();assert(starts==count+1);
    simulated_ms=101000;settle();assert(starts==count+2);
    simulated_ms=106000;settle();assert(starts==count+3);
    simulated_ms=121000;settle();assert(starts==count+4);
    simulated_ms=999999;for(int i=0;i<100;++i)https_conditions_changed(NULL,NULL);settle();assert(starts==count+4);
    assert(https_service_stop(NULL,NULL)==0);start_error=0;
    assert(https_service_start(NULL,NULL)==0);wait_running(true);ts_https_get_runtime(&r);assert(r.loaded_generation==2);
    // Stop failure is reported, buffers remain; a subsequent explicit stop recovers.
    stop_error=ESP_FAIL;assert(https_service_stop(NULL,NULL)==ESP_FAIL);assert(ts_https_is_running());stop_error=0;
    assert(https_service_stop(NULL,NULL)==0);assert(!ts_https_is_running());
    // Initialization and default endpoint failures share the bounded retry policy.
    init_error=ESP_ERR_NO_MEM;count=starts;assert(https_service_start(NULL,NULL)==0);settle();ts_https_get_runtime(&r);assert(r.last_error==ESP_ERR_NO_MEM&&!strcmp(r.last_error_stage,"init"));assert(starts==count);
    assert(https_service_stop(NULL,NULL)==0);init_error=0;default_error=ESP_FAIL;
    assert(https_service_start(NULL,NULL)==0);settle();ts_https_get_runtime(&r);assert(!strcmp(r.last_error_stage,"default_endpoints"));assert(!allocations);
    assert(https_service_stop(NULL,NULL)==0);default_error=0;
    block_start=true;
    assert(https_service_start(NULL,NULL)==0);
    for(int i=0;!inside_start&&i<1000;++i)usleep(1000);
    assert(inside_start);
    pthread_t stopping;pthread_create(&stopping,NULL,stop_request,NULL);settle();
    stored_generation=3;https_conditions_changed(NULL,NULL);block_start=false;
    pthread_join(stopping,NULL);assert(!ts_https_is_running());settle();assert(!ts_https_is_running());
    puts("PASS actual coordinator with simulated scheduler: fallback, duplicate events, bounded retries, stop intent, explicit restart, stage errors");
}
