#include "platform.h"
#include <sys/time.h>
#include <stdatomic.h>
/* Use the production time information type, bypassing the platform stub header. */
#include "../../components/ts_net/include/ts_time_sync.h"
#define TS_EVENT_BASE_TIME "ts_time"
#define TS_EVENT_TIME_SYNCED 1
#define pdMS_TO_TICKS(x) (x)
static struct {ts_time_sync_info_t info;char timezone[32];} s_time_sync;
static time_t clock_value;
static int set_error,event_error;
static unsigned notifications;
static ts_time_sync_info_t delivered;
static time_t read_clock(time_t *p) {if(p)*p=clock_value;return clock_value;}
static int set_clock(const struct timeval *tv,const struct timezone *tz) {(void)tz;if(set_error)return -1;clock_value=tv->tv_sec;return 0;}
int ts_event_post(const char *base,int id,const void *data,size_t length,unsigned wait) {
    (void)base;(void)id;(void)wait;assert(length==sizeof(delivered));delivered=*(const ts_time_sync_info_t *)data;++notifications;return event_error;
}
static struct {_Atomic bool enroll_running,stop_requested;} s_client;
static unsigned delays;
static bool finish_on_delay;
static void vTaskDelay(unsigned t) {(void)t;++delays;if(finish_on_delay)s_client.enroll_running=false;}
#define time read_clock
#define settimeofday set_clock
#include "time_cancel.inc"
#undef time
#undef settimeofday
int main(void) {
    set_error=1;assert(ts_time_sync_set_time(1790121600000LL,TS_TIME_SOURCE_HTTP)!=0);assert(notifications==0);
    set_error=0;assert(ts_time_sync_set_time(1790121600000LL,TS_TIME_SOURCE_HTTP)==0);assert(notifications==1&&delivered.source==TS_TIME_SOURCE_HTTP);assert(!ts_time_sync_needs_sync());
    assert(ts_time_sync_set_time(0,TS_TIME_SOURCE_MANUAL)==0);assert(notifications==2&&delivered.source==TS_TIME_SOURCE_MANUAL);assert(ts_time_sync_needs_sync());
    struct timeval tv={.tv_sec=1790121600};time_sync_notification_cb(&tv);assert(notifications==3&&delivered.source==TS_TIME_SOURCE_NTP);
    event_error=ESP_FAIL;assert(ts_time_sync_set_time(1790121600000LL,TS_TIME_SOURCE_HTTP)==0);assert(!ts_time_sync_needs_sync());
    for(unsigned i=0;i<3;++i){const char *tz[]={"UTC","CST-8","PST8PDT"};setenv("TZ",tz[i],1);tzset();clock_value=1735689599;assert(ts_time_sync_needs_sync());clock_value=1735689600;assert(!ts_time_sync_needs_sync());}
    s_client.enroll_running=true;ts_pki_client_stop_auto_enroll();assert(s_client.stop_requested&&s_client.enroll_running&&delays==50);
    finish_on_delay=true;ts_pki_client_stop_auto_enroll();assert(!s_client.enroll_running);
    puts("PASS actual time notifications/UTC readiness and cooperative enrollment stop (syscalls/tasks mocked)");
    return 0;
}
