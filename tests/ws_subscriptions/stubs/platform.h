#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x106
#define ESP_ERR_NOT_SUPPORTED 0x107
#define TS_LOGD(...) ((void)0)
#define TS_LOGI(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define portTICK_PERIOD_MS 1
#define configTICK_RATE_HZ 1000
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { SemaphoreHandle_t p=malloc(sizeof(*p)); pthread_mutex_init(p,0); return p; }
static inline int xSemaphoreTake(SemaphoreHandle_t p,unsigned t) { (void)t; return !pthread_mutex_lock(p); }
static inline int xSemaphoreGive(SemaphoreHandle_t p) { return !pthread_mutex_unlock(p); }
static inline void vSemaphoreDelete(SemaphoreHandle_t p) { pthread_mutex_destroy(p);free(p); }
static inline const char *esp_err_to_name(int e) {(void)e;return "error";}
extern int64_t test_now;
static inline int64_t esp_timer_get_time(void) {return test_now;}
typedef struct test_timer {void (*callback)(void*); bool active;} *esp_timer_handle_t;
typedef struct {void (*callback)(void*);const char*name;} esp_timer_create_args_t;
static inline int esp_timer_create(const esp_timer_create_args_t*a,esp_timer_handle_t*p) {*p=calloc(1,sizeof(**p));(*p)->callback=a->callback;return 0;}
static inline int esp_timer_start_periodic(esp_timer_handle_t p,uint64_t us) {(void)us;p->active=true;return 0;}
static inline int esp_timer_stop(esp_timer_handle_t p) {p->active=false;return 0;}
static inline int esp_timer_delete(esp_timer_handle_t p) {free(p);return 0;}
static inline bool esp_timer_is_active(esp_timer_handle_t p) {return p->active;}
#ifndef TEST_REAL_EVENT
typedef const char *ts_event_base_t;
typedef void *ts_event_handler_handle_t;
typedef struct {void*data;size_t data_size;int id;} ts_event_t;
#define TS_EVENT_BASE_SYSTEM "system"
#define TS_EVENT_BASE_DEVICE_MON "device"
#define TS_EVENT_BASE_OTA "ota"
#define TS_EVENT_SYSTEM_INFO_CHANGED 1
#define TS_EVENT_DEVICE_STATUS_CHANGED 2
#define TS_EVENT_OTA_PROGRESS_UPDATE 3
#ifdef TEST_FAIL_EVENTS
extern int test_event_fail_at, test_event_registers;
#endif
static inline int ts_event_register(const char*b,int i,void(*h)(const ts_event_t*,void*),void*u,void**out) {(void)b;(void)i;(void)h;(void)u;
#ifdef TEST_FAIL_EVENTS
if(++test_event_registers==test_event_fail_at)return ESP_ERR_NO_MEM;
#endif
*out=(void*)1;return 0;}
static inline int ts_event_unregister(void*h) {(void)h;return 0;}
static inline int ts_event_post(const char*b,int i,const void*d,size_t s,unsigned t) {(void)b;(void)i;(void)d;(void)s;(void)t;return 0;}
#endif
typedef struct {int code;cJSON*data;char*message;} ts_api_result_t;
int ts_api_call(const char*,const cJSON*,ts_api_result_t*);
#ifdef TEST_ALLOC
extern int test_result_frees;
#endif
static inline void ts_api_result_free(ts_api_result_t*r) {
#ifdef TEST_ALLOC
test_result_frees++;
#endif
cJSON_Delete(r->data);free(r->message);memset(r,0,sizeof(*r));}
int ts_webui_broadcast(const char*);
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
int xTaskCreate(void(*fn)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(int,TickType_t);
void vTaskDelete(TaskHandle_t);
void vTaskDelay(TickType_t);
#ifndef TEST_REAL_EVENT
static inline bool ts_event_in_callback(void) {return false;}
static inline int ts_event_unregister_sync(void*h,unsigned t) {(void)h;(void)t;return 0;}
#endif
typedef void *httpd_handle_t;
typedef struct {httpd_handle_t handle;void*sess_ctx;void(*free_ctx)(void*);int fd;int method;void*user_ctx;const char*uri;size_t content_len;} httpd_req_t;
typedef struct {int type;uint8_t*payload;size_t len;} httpd_ws_frame_t;
#define HTTPD_WS_TYPE_TEXT 1
#define HTTPD_WS_CLIENT_WEBSOCKET 2
static inline int httpd_req_to_sockfd(httpd_req_t*r) {return r->fd;}
int httpd_ws_get_fd_info(httpd_handle_t,int);
int httpd_ws_send_frame_async(httpd_handle_t,int,httpd_ws_frame_t*);
int httpd_queue_work(httpd_handle_t,void(*)(void*),void*);
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void*heap_caps_calloc(size_t n,size_t s,unsigned c) {(void)c;return calloc(n,s);}
typedef void*QueueHandle_t;
static inline QueueHandle_t xQueueCreate(unsigned n,unsigned s) {(void)n;(void)s;return malloc(1);}
static inline void vQueueDelete(void*p) {free(p);}
static inline int xQueueSend(void*q,const void*d,unsigned t) {(void)q;(void)d;(void)t;return pdTRUE;}
static inline int xQueueSendFromISR(void*q,const void*d,int*t) {(void)q;(void)d;(void)t;return pdTRUE;}
static inline int xQueueReceive(void*q,void*d,unsigned t) {(void)q;(void)d;(void)t;return 0;}
static inline unsigned uxQueueMessagesWaiting(void*q) {(void)q;return 0;}

static inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t){(void)t;return 8192;}
