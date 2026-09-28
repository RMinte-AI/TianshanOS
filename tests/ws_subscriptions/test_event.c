#define TEST_REAL_EVENT
#include "platform.h"
#include <stdatomic.h>
#include "ts_event.c"
int64_t test_now;
TaskHandle_t xTaskGetCurrentTaskHandle(void) {return (void*)pthread_self();}
int xTaskCreate(void(*fn)(void*),const char*n,unsigned s,void*a,unsigned p,TaskHandle_t*out) {(void)fn;(void)n;(void)s;(void)a;(void)p;*out=(void*)1;return pdPASS;}
void vTaskDelete(TaskHandle_t t) {(void)t;}
static pthread_mutex_t barrier=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static bool entered, release_callback, drain_waiting;
static atomic_bool drained;
void vTaskDelay(TickType_t t) {(void)t;pthread_mutex_lock(&barrier);drain_waiting=true;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&barrier);sched_yield();}
static ts_event_handler_handle_t self_handle, blocked_handle, other_handle;
static int other_calls,self_calls;
static void self_cancel(const ts_event_t*e,void*u) {(void)e;(void)u;self_calls++;assert(ts_event_unregister(self_handle)==0);}
static void other(const ts_event_t*e,void*u) {(void)e;(void)u;other_calls++;}
static void blocked(const ts_event_t*e,void*u) {(void)e;(void)u;
#ifndef BASELINE_EVENT
 assert(ts_event_unregister_sync(blocked_handle,10)==ESP_ERR_INVALID_STATE);
#endif
 pthread_mutex_lock(&barrier);entered=true;pthread_cond_broadcast(&cv);while(!release_callback)pthread_cond_wait(&cv,&barrier);pthread_mutex_unlock(&barrier);
}
static void*post(void*p) {(void)p;ts_event_post_sync("test",1,NULL,0);return NULL;}
#ifndef BASELINE_EVENT
static void*drain(void*p) {(void)p;assert(ts_event_unregister_sync(blocked_handle,100)==0);drained=true;return NULL;}
#endif
int main(void) {
 s_event_ctx.initialized=true;s_event_ctx.running=true;s_event_ctx.mutex=xSemaphoreCreateMutex();
 assert(ts_event_register("test",1,self_cancel,NULL,&self_handle)==0);
 ts_event_post_sync("test",1,NULL,0);assert(self_calls==1);
 assert(ts_event_register("test",1,other,NULL,&other_handle)==0);
 assert(ts_event_register("test",1,blocked,NULL,&blocked_handle)==0);
 pthread_t dispatch;pthread_create(&dispatch,NULL,post,NULL);
 pthread_mutex_lock(&barrier);while(!entered)pthread_cond_wait(&cv,&barrier);pthread_mutex_unlock(&barrier);
 assert(ts_event_unregister(other_handle)==0); /* selected snapshot but not yet invoked */
#ifndef BASELINE_EVENT
 pthread_t waiter;pthread_create(&waiter,NULL,drain,NULL);
 pthread_mutex_lock(&barrier);while(!drain_waiting)pthread_cond_wait(&cv,&barrier);assert(!drained);release_callback=true;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&barrier);
 pthread_join(waiter,NULL);assert(drained);
#else
 ts_event_unregister(blocked_handle);
 pthread_mutex_lock(&barrier);release_callback=true;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&barrier);
#endif
 pthread_join(dispatch,NULL);assert(other_calls==0);assert(!s_event_ctx.handlers);
 assert(ts_event_register("test",1,other,NULL,&other_handle)==0);ts_event_post_sync("test",1,NULL,0);assert(other_calls==1);assert(ts_event_unregister(other_handle)==0);
#ifndef BASELINE_EVENT
 entered=false;release_callback=false;
 assert(ts_event_register("test",1,blocked,NULL,&blocked_handle)==0);
 pthread_create(&dispatch,NULL,post,NULL);
 pthread_mutex_lock(&barrier);while(!entered)pthread_cond_wait(&cv,&barrier);pthread_mutex_unlock(&barrier);
 assert(ts_event_unregister_sync(blocked_handle,0)==ESP_ERR_TIMEOUT);
 pthread_mutex_lock(&barrier);release_callback=true;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&barrier);
 pthread_join(dispatch,NULL);assert(s_event_ctx.handlers); /* retained for drain retry */
 assert(ts_event_unregister_sync(blocked_handle,0)==0);assert(!s_event_ctx.handlers);
#endif
 vSemaphoreDelete(s_event_ctx.mutex);
 puts("PASS T32: self unregister, selected-node retirement, in-flight synchronous drain, unrelated handler retained");
}
