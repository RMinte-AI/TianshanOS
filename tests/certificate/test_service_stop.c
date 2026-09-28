#include "platform.h"
#define TS_SERVICE_STATE_STOPPED 0
#define TS_SERVICE_STATE_REGISTERED 1
#define TS_SERVICE_STATE_RUNNING 2
#define TS_SERVICE_STATE_STOPPING 3
#define TS_EVENT_BASE_SERVICE "service"
#define TS_EVENT_SERVICE_STOPPED 1
typedef int ts_service_state_t;
typedef struct {const char *service_name;int old_state,new_state,error_code;} ts_service_event_data_t;
typedef struct {int state;struct {const char *name;unsigned capabilities;int (*stop)(void *,void *);void *user_data;}def;} ts_service_instance_t;
static void set_service_state(ts_service_instance_t *s,int state) {s->state=state;}
static unsigned stopped_events;
int ts_event_post(const char *base,int id,const void *data,size_t len,unsigned timeout) {(void)base;(void)id;(void)data;(void)len;(void)timeout;++stopped_events;return 0;}
#include "service_stop.inc"
typedef ts_service_instance_t *ts_service_handle_t;
#define TS_SERVICE_CAP_RESTARTABLE 1
static unsigned starts;
static int ts_service_stop(ts_service_handle_t s) {return stop_service_internal(s);}
static int ts_service_start(ts_service_handle_t s) {++starts;s->state=TS_SERVICE_STATE_RUNNING;return ESP_OK;}
#include "service_restart.inc"
static int result;
static int stop_callback(void *s,void *u) {(void)s;(void)u;return result;}
int main(void) {
    ts_service_instance_t s={.state=TS_SERVICE_STATE_RUNNING,.def={.name="https",.stop=stop_callback}};
    result=ESP_FAIL;int ret=stop_service_internal(&s);
    if(ret!=ESP_FAIL||s.state!=TS_SERVICE_STATE_RUNNING||stopped_events) {
        puts("REPRODUCED manager stop bug: callback failure becomes success/stopped");return 2;
    }
    result=ESP_OK;assert(stop_service_internal(&s)==ESP_OK);assert(s.state==TS_SERVICE_STATE_STOPPED&&stopped_events==1);
    assert(stop_service_internal(&s)==ESP_OK&&stopped_events==1);
    s=(ts_service_instance_t){.state=TS_SERVICE_STATE_RUNNING,.def={.name="other",.stop=stop_callback}};
    assert(stop_service_internal(&s)==ESP_OK&&s.state==TS_SERVICE_STATE_STOPPED);
    s.def.capabilities=TS_SERVICE_CAP_RESTARTABLE;
    const int failures[]={ESP_FAIL,ESP_ERR_TIMEOUT,ESP_ERR_INVALID_STATE};
    for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);++i) {
        s.state=TS_SERVICE_STATE_RUNNING;result=failures[i];
        unsigned events=stopped_events;
        assert(ts_service_restart(&s)==result);
        assert(s.state==TS_SERVICE_STATE_RUNNING&&starts==0&&stopped_events==events);
    }
    result=ESP_OK;
    assert(ts_service_restart(&s)==ESP_OK&&starts==1&&s.state==TS_SERVICE_STATE_RUNNING);
    puts("PASS manager stop/restart: failed stop retained, restart aborted, successful/duplicate stop unchanged");return 0;
}
