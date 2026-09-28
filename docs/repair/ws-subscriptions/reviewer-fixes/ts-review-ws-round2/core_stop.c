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
static bool s_core_initialized=true,s_core_started=false,log_freed,config_freed;
static int ts_core_stop(void){return 0;}
static int ts_service_deinit(void){return 0;}
static int ts_log_deinit(void){log_freed=true;return 0;}
static int ts_config_deinit(void){config_freed=true;return 0;}
esp_err_t ts_core_deinit(void)
{
    if (!s_core_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing TianShanOS Core...");

    // 如果已启动，先停止
    if (s_core_started) {
        ts_core_stop();
    }

    // 按相反顺序反初始化
    ts_service_deinit();
    ts_event_deinit();
    ts_log_deinit();
    ts_config_deinit();

    s_core_initialized = false;
    ESP_LOGI(TAG, "TianShanOS Core deinitialized");

    return ESP_OK;
}
int main(void){
 s_event_ctx.initialized=true;s_event_ctx.running=true;s_event_ctx.mutex=xSemaphoreCreateMutex();s_event_ctx.event_queue=xQueueCreate(4,1);
 ts_event_handler_handle_t h;assert(ts_event_register("test",1,blocked,NULL,&h)==0);
 pthread_t thread;pthread_create(&thread,NULL,post,NULL);
 pthread_mutex_lock(&barrier);while(!entered)pthread_cond_wait(&cv,&barrier);pthread_mutex_unlock(&barrier);
 int result=ts_core_deinit();assert(result==ESP_OK && !s_core_initialized && log_freed && config_freed && s_event_ctx.initialized && s_dispatch_scopes);
 printf("CONFIRMED R4: actual event drain timed out at %lld us, but actual core_deinit returned OK and deinitialized log/config while callback was still running\n",(long long)test_now);
 pthread_mutex_lock(&barrier);release_callback=true;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&barrier);pthread_join(thread,NULL);
 assert(ts_event_unregister_sync(h,0)==0);assert(ts_event_deinit()==0);
}
