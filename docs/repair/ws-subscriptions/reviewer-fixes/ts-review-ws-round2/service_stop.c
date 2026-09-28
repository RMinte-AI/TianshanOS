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
static esp_err_t stop_service_internal(ts_service_instance_t *service)
{
    if (service == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 已经停止
    if (service->state == TS_SERVICE_STATE_STOPPED ||
        service->state == TS_SERVICE_STATE_REGISTERED) {
        return ESP_OK;
    }

    // 不能停止非运行状态的服务
    if (service->state != TS_SERVICE_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping service: %s", service->def.name);

    ts_service_state_t old_state = service->state;
    set_service_state(service, TS_SERVICE_STATE_STOPPING);

    esp_err_t ret = ESP_OK;

    // 调用停止回调
    if (service->def.stop != NULL) {
        ret = service->def.stop(service, service->def.user_data);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Service '%s' stop returned error: %s", 
                     service->def.name, esp_err_to_name(ret));
            /* Failed stop retains the service and its resources for a later retry. */
            set_service_state(service, old_state);
            return ret;
        }
    }

    set_service_state(service, TS_SERVICE_STATE_STOPPED);

    ESP_LOGI(TAG, "Service '%s' stopped", service->def.name);

    // 发送服务停止事件
    ts_service_event_data_t event_data = {
        .service_name = service->def.name,
        .old_state = old_state,
        .new_state = TS_SERVICE_STATE_STOPPED,
        .error_code = ret,
    };
    ts_event_post(TS_EVENT_BASE_SERVICE, TS_EVENT_SERVICE_STOPPED,
                  &event_data, sizeof(event_data), 100);

    return ESP_OK;
}
esp_err_t ts_service_stop(ts_service_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ts_service_instance_t *service = (ts_service_instance_t *)handle;
    return stop_service_internal(service);
}
esp_err_t ts_service_stop_all(void)
{
    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping all services...");

    // 按相反顺序停止（从 UI 到 PLATFORM）
    for (int phase = TS_SERVICE_PHASE_MAX - 1; phase >= 0; phase--) {
        ESP_LOGI(TAG, "Stopping phase: %s", ts_service_phase_to_string(phase));

        xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

        ts_service_instance_t *service = s_svc_ctx.services;
        while (service != NULL) {
            if (service->def.phase == phase && 
                service->state == TS_SERVICE_STATE_RUNNING) {
                xSemaphoreGive(s_svc_ctx.mutex);
                ts_service_stop(service);
                xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);
            }
            service = service->next;
        }

        xSemaphoreGive(s_svc_ctx.mutex);
    }

    s_svc_ctx.startup_complete = false;
    ESP_LOGI(TAG, "All services stopped");

    return ESP_OK;
}
esp_err_t ts_service_deinit(void)
{
    if (!s_svc_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing service management...");

    // 停止所有服务
    ts_service_stop_all();

    xSemaphoreTake(s_svc_ctx.mutex, portMAX_DELAY);

    // 释放所有服务
    ts_service_instance_t *service = s_svc_ctx.services;
    while (service != NULL) {
        ts_service_instance_t *next = service->next;
        if (service->state_sem != NULL) {
            vSemaphoreDelete(service->state_sem);
        }
        free(service);
        service = next;
    }
    s_svc_ctx.services = NULL;
    s_svc_ctx.service_count = 0;

    xSemaphoreGive(s_svc_ctx.mutex);

    vSemaphoreDelete(s_svc_ctx.mutex);
    s_svc_ctx.mutex = NULL;

    s_svc_ctx.initialized = false;
    ESP_LOGI(TAG, "Service management deinitialized");

    return ESP_OK;
}
static unsigned ui_calls,net_calls;
static int busy_ui(void*h,void*u){(void)h;(void)u;ui_calls++;return ESP_ERR_TIMEOUT;}
static int stop_net(void*h,void*u){(void)h;(void)u;net_calls++;return ESP_OK;}
int main(void){
 ts_service_instance_t*ui=calloc(1,sizeof(*ui)),*net=calloc(1,sizeof(*net));
 ui->def.name="webui";ui->def.phase=1;ui->def.stop=busy_ui;ui->state=TS_SERVICE_STATE_RUNNING;ui->next=net;
 net->def.name="network";net->def.phase=0;net->def.stop=stop_net;net->state=TS_SERVICE_STATE_RUNNING;
 s_svc_ctx.initialized=true;s_svc_ctx.startup_complete=true;s_svc_ctx.mutex=xSemaphoreCreateMutex();s_svc_ctx.services=ui;s_svc_ctx.service_count=2;
 assert(ts_service_stop_all()==ESP_OK);assert(ui->state==TS_SERVICE_STATE_RUNNING && net_calls==1);
 assert(ts_service_deinit()==ESP_OK && !s_svc_ctx.initialized && !s_svc_ctx.services && ui_calls==2);
 puts("CONFIRMED R4 aggregate: failed WebUI stop was ignored; lower-phase dependency stopped; service_deinit released registry and returned OK despite repeated failure");
}
