#include "platform.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TS_SERVICE_PHASE_MAX 2
#define TS_EVENT_BASE_SERVICE "service"
#define TS_EVENT_SERVICE_STOPPED 1
typedef enum {TS_SERVICE_STATE_REGISTERED,TS_SERVICE_STATE_RUNNING,TS_SERVICE_STATE_STOPPING,TS_SERVICE_STATE_STOPPED} ts_service_state_t;
typedef struct svc {struct{const char*name;int phase;int(*stop)(void*,void*);void*user_data;}def;ts_service_state_t state;SemaphoreHandle_t state_sem;struct svc*next;} ts_service_instance_t;
typedef void* ts_service_handle_t;
typedef struct{const char*service_name;ts_service_state_t old_state,new_state;int error_code;} ts_service_event_data_t;
static struct{bool initialized,startup_complete;SemaphoreHandle_t mutex;ts_service_instance_t*services;unsigned service_count;}s_svc_ctx;
static void set_service_state(ts_service_instance_t*s,ts_service_state_t state){s->state=state;}
static int stop_service_internal(ts_service_instance_t*);
int ts_service_stop(ts_service_handle_t);
int ts_service_stop_all(void);
#include "reviewer_service.inc"
static unsigned ui_calls,net_calls;
static int ui_result=ESP_ERR_TIMEOUT;
static int busy_ui(void*h,void*u){(void)h;(void)u;ui_calls++;return ui_result;}
static int stop_net(void*h,void*u){(void)h;(void)u;net_calls++;return ESP_OK;}
int main(void){
 ts_service_instance_t*ui=calloc(1,sizeof(*ui)),*net=calloc(1,sizeof(*net));
 ui->def.name="webui";ui->def.phase=1;ui->def.stop=busy_ui;ui->state=TS_SERVICE_STATE_RUNNING;ui->next=net;
 net->def.name="network";net->def.phase=0;net->def.stop=stop_net;net->state=TS_SERVICE_STATE_RUNNING;
 s_svc_ctx.initialized=true;s_svc_ctx.startup_complete=true;s_svc_ctx.mutex=xSemaphoreCreateMutex();s_svc_ctx.services=ui;s_svc_ctx.service_count=2;
 assert(ts_service_stop_all()==ESP_ERR_TIMEOUT);assert(ui->state==TS_SERVICE_STATE_RUNNING && net_calls==0);
 assert(ts_service_deinit()==ESP_ERR_TIMEOUT && s_svc_ctx.initialized && s_svc_ctx.services==ui);
 ui_result=ESP_OK;assert(ts_service_deinit()==ESP_OK && !s_svc_ctx.initialized && !s_svc_ctx.services && net_calls==1);
 puts("PASS R4 service: stop errors retain registry/dependencies; retry drains then releases ownership");
}
