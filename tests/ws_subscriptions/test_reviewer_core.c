#define TEST_REAL_EVENT
#include "platform.h"
#include <stdatomic.h>
#include "ts_event.c"
int64_t test_now;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void*)pthread_self();}
int xTaskCreate(void(*fn)(void*),const char*n,unsigned s,void*a,unsigned p,TaskHandle_t*out){(void)fn;(void)n;(void)s;(void)a;(void)p;*out=(void*)1;return pdPASS;}
void vTaskDelete(TaskHandle_t t){(void)t;}
void vTaskDelay(TickType_t t){test_now+=(int64_t)t*1000;}
static pthread_mutex_t barrier=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static bool entered,release_callback;
static void blocked(const ts_event_t*e,void*u){(void)e;(void)u;pthread_mutex_lock(&barrier);entered=true;pthread_cond_broadcast(&cv);while(!release_callback)pthread_cond_wait(&cv,&barrier);pthread_mutex_unlock(&barrier);}
static void*post(void*p){(void)p;ts_event_post_sync("test",1,NULL,0);return NULL;}

static bool s_core_cleanup_pending,s_core_stopping;
static int ts_service_start_all(void){return 0;}
static bool s_core_initialized=true,s_core_started=true,service_live=true,log_live=true,config_live=true;
static int service_stop_result,save_result,log_result,config_result;
static unsigned save_calls;
static bool ts_service_is_initialized(void){return service_live;}
static bool ts_log_is_initialized(void){return log_live;}
static bool ts_config_is_initialized(void){return config_live;}
static int ts_service_stop_all(void){return service_stop_result;}
static int ts_config_save(void){save_calls++;return save_result;}
static int ts_service_deinit(void){service_live=false;return 0;}
static int ts_log_deinit(void){if(!log_result)log_live=false;return log_result;}
static int ts_config_deinit(void){if(!config_result)config_live=false;return config_result;}
#include "reviewer_core.inc"
static unsigned shutdown_calls;
static void shutdown_event(const ts_event_t*e,void*u){(void)e;(void)u;shutdown_calls++;}
int main(void){
 s_event_ctx.initialized=true;s_event_ctx.running=true;s_event_ctx.mutex=xSemaphoreCreateMutex();s_event_ctx.event_queue=xQueueCreate(4,1);
 ts_event_handler_handle_t shutdown_handle;assert(ts_event_register(TS_EVENT_BASE_SYSTEM,TS_EVENT_SYSTEM_SHUTDOWN,shutdown_event,NULL,&shutdown_handle)==0);
 service_stop_result=ESP_ERR_TIMEOUT;assert(ts_core_stop()==ESP_ERR_TIMEOUT && s_core_started && save_calls==0);
 assert(ts_core_start()==ESP_ERR_INVALID_STATE);
 service_stop_result=0;save_result=ESP_FAIL;assert(ts_core_stop()==ESP_FAIL && s_core_started);
 save_result=0;assert(ts_core_stop()==0 && !s_core_started && shutdown_calls==1);
 ts_event_handler_handle_t h;assert(ts_event_register("test",1,blocked,NULL,&h)==0);
 pthread_t thread;pthread_create(&thread,NULL,post,NULL);
 pthread_mutex_lock(&barrier);while(!entered)pthread_cond_wait(&cv,&barrier);pthread_mutex_unlock(&barrier);
 assert(ts_core_deinit()==ESP_ERR_TIMEOUT && s_core_initialized && log_live && config_live && s_event_ctx.initialized && s_dispatch_scopes);
 pthread_mutex_lock(&barrier);release_callback=true;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&barrier);pthread_join(thread,NULL);
 log_result=ESP_FAIL;assert(ts_core_deinit()==ESP_FAIL && s_core_initialized && config_live && !s_event_ctx.initialized);
 log_result=0;config_result=ESP_FAIL;assert(ts_core_deinit()==ESP_FAIL && s_core_initialized && !log_live);
 config_result=0;assert(ts_core_deinit()==0 && !s_core_initialized && !config_live);
 s_core_cleanup_pending=false;log_live=true;config_live=true;log_result=ESP_FAIL;
 assert(core_init_rollback(ESP_ERR_NO_MEM)==ESP_FAIL && s_core_cleanup_pending && config_live);
 log_result=0;assert(ts_core_deinit()==0 && !s_core_cleanup_pending && !config_live);
 puts("PASS R4 core: real event drain timeout retains downstream resources; service/config/log errors propagate; partial deinit retries succeed");
}
