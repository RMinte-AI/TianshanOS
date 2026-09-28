#include "platform.h"
#include <stdatomic.h>
#include "ts_ws_transport.h"
#define MAX_WS_CLIENTS 8
#define TERMINAL_OUTPUT_BUF_SIZE 32768
#define TS_EVENT_BASE_POWER "power"
#define TS_EVENT_ANY_ID -1
#define HTTP_GET 0
static struct {int active;} s_clients[MAX_WS_CLIENTS];
static httpd_handle_t s_server;
static bool s_ws_stopping;
static int s_terminal_client_fd;
static SemaphoreHandle_t s_terminal_mutex,s_output_mutex;
static char*s_terminal_output_buf;
static size_t s_terminal_output_len;
static ts_event_handler_handle_t s_power_event_handle;
static int fail_stage,mutex_calls;
static httpd_handle_t ts_http_server_get_handle(void){return fail_stage==1?NULL:(void*)11;}
static void peer_closed(ts_ws_peer_t peer){(void)peer;}
static void ts_ws_op_enable(void){}
esp_err_t ts_ws_transport_start(httpd_handle_t server,ts_ws_closed_t fn){(void)server;(void)fn;return fail_stage==2?ESP_ERR_NO_MEM:ESP_OK;}
static int ts_ws_subscriptions_init(void){return fail_stage==3?ESP_ERR_NO_MEM:ESP_OK;}
static esp_err_t ts_webui_ws_stop(httpd_handle_t server){(void)server;return ESP_OK;}
static int manager_stop_result,barrier_result,tx_stop_result,tx_stop_calls;
static bool owner_context,s_exec_running,s_ssh_poll_alive,cleanup_finishes;
static atomic_bool s_exec_result_active,s_exec_output_failed,s_exec_terminal_ready;
static atomic_uint s_ssh_output_pending,s_ssh_result_pending;
static atomic_bool s_exec_terminal_claimed;
static atomic_uint s_exec_creators;
bool ts_ws_transport_in_context(void){return owner_context;}
static esp_err_t ts_ws_subscriptions_deinit(void){return manager_stop_result;}
static esp_err_t ts_ws_subscriptions_pause(void){return manager_stop_result;}
static esp_err_t ts_ws_subscriptions_drain(void){return manager_stop_result;}
static esp_err_t ts_webui_log_stream_enable(bool enable){(void)enable;return ESP_OK;}
static ts_ws_reservation_t s_exec_terminal;
esp_err_t ts_ws_result_reserve(size_t bytes,ts_ws_reservation_t*r){(void)bytes;(void)r;return 0;}
void ts_ws_reservation_release(ts_ws_reservation_t*r){(void)r;}
esp_err_t ts_ws_transport_quiesce(httpd_handle_t server,uint32_t timeout){(void)server;(void)timeout;return barrier_result;}
esp_err_t ts_ws_transport_stop(httpd_handle_t server,uint32_t timeout){(void)server;(void)timeout;tx_stop_calls++;return tx_stop_result;}
static void ssh_cleanup(void){if(cleanup_finishes)s_ssh_poll_alive=false;}
static void ts_webui_ws_stopped(httpd_handle_t server){(void)server;}
static void ts_http_server_set_stop_hooks(esp_err_t(*a)(httpd_handle_t),void(*b)(httpd_handle_t)){(void)a;(void)b;}
static SemaphoreHandle_t create_mutex(void){mutex_calls++;if(fail_stage==3+mutex_calls)return NULL;return xSemaphoreCreateMutex();}
static void*buffer_alloc(size_t n){if(fail_stage==6)return NULL;return malloc(n);}
static void*heap_alloc(size_t n,unsigned caps){(void)caps;return buffer_alloc(n);}
static int ws_handler(httpd_req_t*r){(void)r;return 0;}
static void power_policy_event_handler(const ts_event_t*e,void*u){(void)e;(void)u;}
typedef struct {const char*uri;int method;int(*handler)(httpd_req_t*);void*user_ctx;bool is_websocket,handle_ws_control_frames;} httpd_uri_t;
static int httpd_register_uri_handler(httpd_handle_t h,const httpd_uri_t*u){(void)h;(void)u;return fail_stage==7?ESP_FAIL:0;}
static int register_event(const char*b,int id,void(*fn)(const ts_event_t*,void*),void*u,void**out){(void)b;(void)id;(void)fn;(void)u;if(fail_stage==8)return ESP_ERR_NO_MEM;*out=(void*)1;return 0;}
#define xSemaphoreCreateMutex create_mutex
#define malloc buffer_alloc
#define heap_caps_malloc heap_alloc
#define ts_event_register register_event
#include "ws_init.inc"
#undef xSemaphoreCreateMutex
#undef malloc
#undef heap_caps_malloc
#undef ts_event_register
static void reset(void){
 if(s_terminal_mutex)vSemaphoreDelete(s_terminal_mutex);if(s_output_mutex)vSemaphoreDelete(s_output_mutex);free(s_terminal_output_buf);
 s_terminal_mutex=s_output_mutex=NULL;s_terminal_output_buf=NULL;s_server=NULL;s_power_event_handle=NULL;mutex_calls=0;s_ws_stopping=false;
}
int main(void){
 for(fail_stage=1;fail_stage<=8;fail_stage++){assert(ts_webui_ws_init()!=ESP_OK);reset();}
 fail_stage=0;assert(ts_webui_ws_init()==0);assert(ts_webui_ws_init()==0);s_ws_stopping=true;assert(ts_webui_ws_init()==ESP_ERR_INVALID_STATE);reset();
 puts("PASS actual WebSocket initializer: server/transport/manager/two mutexes/buffer/URI/power-handler failures are not reported as success; duplicate/retry-state guards");
}
