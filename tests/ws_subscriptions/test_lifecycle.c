#include "platform.h"
#include <stdatomic.h>
static httpd_handle_t s_server=(void*)11;
static bool s_initialized=true;
static atomic_bool s_stopping;
static atomic_flag s_stop_busy=ATOMIC_FLAG_INIT;
static _Atomic(TaskHandle_t) s_http_task;
static int s_route_count=4;
static struct {void *owned_copy;} s_registered_routes[64];
static int hook_result,stop_result,stop_calls,after_calls;
static esp_err_t before(httpd_handle_t h){assert(h==(void*)11);return hook_result;}
static void after(httpd_handle_t h){assert(h==(void*)11);after_calls++;}
static esp_err_t(*s_before_stop)(httpd_handle_t)=before;
static void(*s_after_stop)(httpd_handle_t)=after;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void*)1;}
static int httpd_stop(httpd_handle_t h){assert(h==(void*)11);stop_calls++;return stop_result;}
#include "http_stop.inc"
static httpd_handle_t ts_http_server_get_handle(void){return s_server;}
static bool ts_http_server_is_stopping(void){return s_stopping;}
#define s_initialized webui_initialized
static bool s_initialized=true;
static bool s_running=true;
esp_err_t ts_webui_stop(void);
#include "webui_stop.inc"
#undef s_initialized
int main(void){
 s_registered_routes[0].owned_copy=malloc(16);
 hook_result=ESP_ERR_TIMEOUT;assert(ts_webui_stop()==ESP_ERR_TIMEOUT);assert(stop_calls==0&&s_server&&s_running&&s_registered_routes[0].owned_copy);assert(!ts_webui_is_running());
 assert(ts_webui_deinit()==ESP_ERR_TIMEOUT);assert(s_initialized&&webui_initialized);
 hook_result=0;stop_result=ESP_FAIL;assert(ts_webui_stop()==ESP_FAIL);assert(s_server&&s_stopping&&after_calls==0);
 stop_result=0;s_http_task=(void*)1;assert(ts_webui_stop()==ESP_ERR_INVALID_STATE);s_http_task=NULL;
 assert(ts_webui_stop()==0);assert(!s_server&&!s_stopping&&!s_running&&after_calls==1&&!s_registered_routes[0].owned_copy);
 assert(ts_webui_deinit()==0);assert(!s_initialized&&!webui_initialized);assert(ts_webui_stop()==0);
 puts("PASS actual HTTP/WebUI stop/deinit: hook timeout, HTTPD failure, self-stop rejection, truthful state, retained ownership and retry");
}
