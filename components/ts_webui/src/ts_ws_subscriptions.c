/** Demand-driven display telemetry. Hardware sampling/control is not scheduled here. */
#include "ts_ws_subscriptions.h"
#include "ts_ws_transport.h"
#include "ts_ws_operation.h"
#include "ts_api.h"
#include "ts_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>
#include <limits.h>

#define MAX_SUBSCRIPTIONS 32
#define EVENT_SLOTS 4
#define WORKER_STACK 8192
#define STOP_TIMEOUT_MS 6000
#define TAG "ws_subs"
static const struct { const char *name; uint32_t interval; int metric; } topics[] = {
    {"system.dashboard",1000,-1}, {"system.info",5000,2},
    {"system.memory",5000,1}, {"system.cpu",1000,0},
    {"network.status",5000,3}, {"power.status",5000,4},
    {"fan.status",5000,5}, {"service.list",5000,6},
    {"device.status",2000,-2}, {"ota.progress",1000,-2},
    {"config.pack.validated",0,-2}
};
#define TOPIC_COUNT (sizeof(topics)/sizeof(topics[0]))
static const char *metrics[] = {"system.cpu","system.memory","system.info",
    "network.status","power.status","fan.status","service.list"};
static const char *fields[] = {"cpu","memory","info","network","power","fan","services"};
typedef enum { OFF, STARTING, RUNNING, DRAINING, STOPPING } lifecycle_t;
typedef struct {
    ts_ws_peer_t peer;
    uint64_t revision, pending;
    int64_t last_success, next_attempt;
    uint32_t interval;
    unsigned topic;
    bool active;
} subscription_t;
typedef struct {
    ts_ws_peer_t peer;
    uint64_t revision, delivery;
    unsigned topic;
} target_t;
typedef struct {
    bool used, ready;
    uint64_t order;
    ts_ws_message_t *message;
    uint64_t targets[MAX_SUBSCRIPTIONS];
} event_item_t;
/* Stable storage / spinlock survive init/deinit. Only short value mutations
 * and task notifications under lock, never heap, API, JSON, queue or network. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lifecycle_t s_state;
static bool s_stop_owner;
static TaskHandle_t s_worker;
static unsigned s_entries, s_outstanding;
static uint64_t s_sequence;
static unsigned s_topic_cursor, s_subscription_cursor;
static int64_t s_allocation_retry_at;
static subscription_t s_subs[MAX_SUBSCRIPTIONS];
static event_item_t s_events[EVENT_SLOTS];
static ts_event_handler_handle_t s_handlers[3];
static struct {
    uint64_t api[7], batches[TOPIC_COUNT], serialized[TOPIC_COUNT];
    uint64_t queued, success, failure, stale, event_dropped, idle_wait, capacity_wait;
    uint32_t stack_low_water;
} s_stats;

static int64_t after_ms(int64_t now, uint32_t ms)
{
    int64_t delta = (int64_t)ms * 1000;
    return now > INT64_MAX - delta ? INT64_MAX : now + delta;
}
static void wake_locked(void) { if (s_worker) xTaskNotifyGive(s_worker); }
void ts_ws_subscriptions_wake(void)
{
    portENTER_CRITICAL(&s_lock);wake_locked();portEXIT_CRITICAL(&s_lock);
}
static int topic_id(const char *name)
{
    if (name) for (unsigned i=0;i<TOPIC_COUNT;i++) if (!strcmp(name,topics[i].name)) return i;
    return -1;
}
static bool enter(bool result_producer)
{
    portENTER_CRITICAL(&s_lock);
    bool ok = s_state == RUNNING || (result_producer && s_state == DRAINING);
    if (ok) s_entries++;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
static void leave(void)
{
    portENTER_CRITICAL(&s_lock); s_entries--; wake_locked(); portEXIT_CRITICAL(&s_lock);
}
static subscription_t *find_revision(uint64_t revision)
{
    for (unsigned i=0;i<MAX_SUBSCRIPTIONS;i++)
        if (s_subs[i].active && s_subs[i].revision == revision) return &s_subs[i];
    return NULL;
}
static bool delivery_valid(uint64_t revision, uint64_t delivery)
{
    portENTER_CRITICAL(&s_lock);
    subscription_t *s = find_revision(revision);
    bool ok = (s_state == RUNNING || s_state == DRAINING) && s && s->pending == delivery;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
static void delivery_done(uint64_t revision, uint64_t delivery, esp_err_t result)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    subscription_t *s = find_revision(revision);
    if (s && s->pending == delivery) {
        s->pending = 0;
        if (result == ESP_OK) s->last_success = now;
        /* Failure is not success. No catch-up burst and no retry of an old frame. */
        uint32_t delay = s->interval ? s->interval : (result == ESP_OK ? 0 : 1000);
        s->next_attempt = after_ms(now, delay);
    } else s_stats.stale++;
    if (result == ESP_OK) s_stats.success++; else s_stats.failure++;
    s_outstanding--;
    wake_locked();
    portEXIT_CRITICAL(&s_lock);
}
static bool reserve(subscription_t *s, target_t *t)
{
    if (s_sequence == UINT64_MAX) { s_state=STOPPING; wake_locked(); return false; } /* fail closed, never reuse IDs */
    s->pending = ++s_sequence;
    *t = (target_t){s->peer,s->revision,s->pending,s->topic};
    s_outstanding++;
    return true;
}
static void submit(target_t t, ts_ws_message_t *m)
{
    esp_err_t ret = ESP_ERR_NO_MEM;
    if (m && delivery_valid(t.revision,t.delivery))
        ret = ts_ws_transport_submit(t.peer,m,t.revision,t.delivery,delivery_valid,delivery_done);
    if (ret != ESP_OK) delivery_done(t.revision,t.delivery,ret);
    else { portENTER_CRITICAL(&s_lock); s_stats.queued++; portEXIT_CRITICAL(&s_lock); }
}
static bool metric_needed(target_t *targets, unsigned n, unsigned metric)
{
    for (unsigned i=0;i<n;i++) if ((topics[targets[i].topic].metric == -1 ||
        topics[targets[i].topic].metric == (int)metric) && delivery_valid(targets[i].revision,targets[i].delivery)) return true;
    return false;
}
static void collect_batch(target_t *targets, unsigned n, ts_ws_reservation_t *reserved)
{
    cJSON *data[7] = {0};
    for (unsigned i=0;i<7;i++) {
        if (!metric_needed(targets,n,i)) continue;
        ts_api_result_t r = {0};
        portENTER_CRITICAL(&s_lock); s_stats.api[i]++; portEXIT_CRITICAL(&s_lock);
        esp_err_t ret = ts_api_call(metrics[i],NULL,&r);
        if (ret == ESP_OK && r.code == 0 && r.data) { data[i]=r.data; r.data=NULL; }
        ts_api_result_free(&r);
    }
    for (unsigned topic=0;topic<8;topic++) {
        bool needed=false;
        for(unsigned i=0;i<n;i++) if(targets[i].topic==topic && delivery_valid(targets[i].revision,targets[i].delivery)) needed=true;
        ts_ws_message_t *m=NULL;
        cJSON *dashboard=NULL;
        if (needed) {
            const cJSON *payload=NULL;
            if (topic==0) {
                dashboard=cJSON_CreateObject();
                bool ok=dashboard!=NULL, any=false;
                for(unsigned i=0;i<7 && ok;i++) if(data[i]) {
                    any=true;
                    ok=ts_ws_json_reference(dashboard,fields[i],data[i]);
                }
                if(ok && any) payload=dashboard;
            } else payload=data[topics[topic].metric];
            if(payload && ts_ws_reserved_json(&reserved[topic],topics[topic].name,payload,esp_timer_get_time()/1000000)) m=reserved[topic].message;
            portENTER_CRITICAL(&s_lock);
            s_stats.batches[topic]++; if(m) s_stats.serialized[topic]++;
            portEXIT_CRITICAL(&s_lock);
        }
        unsigned destination=0;
        for(unsigned i=0;i<n;i++) if(targets[i].topic==topic) {
            target_t t=targets[i];
            esp_err_t ret=ESP_ERR_INVALID_STATE;
            if(m && delivery_valid(t.revision,t.delivery))
                ret=ts_ws_reserved_submit(&reserved[topic],destination,t.revision,t.delivery,delivery_valid,delivery_done);
            if(ret!=ESP_OK && t.delivery)delivery_done(t.revision,t.delivery,ret);
            else if(ret==ESP_OK){portENTER_CRITICAL(&s_lock);s_stats.queued++;portEXIT_CRITICAL(&s_lock);}
            destination++;
        }
        ts_ws_reservation_release(&reserved[topic]);
        cJSON_Delete(dashboard);
    }
    for(unsigned i=0;i<7;i++) cJSON_Delete(data[i]);
}
/* Called by the single worker; one bounded pass, no network wait under lock. */
static void run_batch(void)
{
    ts_ws_op_poll();
    ts_ws_transport_flush();
    target_t targets[MAX_SUBSCRIPTIONS];
    ts_ws_reservation_t reserved[8]={0};
    unsigned n=0,capacity;
    int64_t now=esp_timer_get_time();
    unsigned first=s_topic_cursor;
    for(unsigned turn=0;turn<8;turn++) {
        unsigned topic=(first+turn)%8;
        ts_ws_peer_t peers[TS_WS_CONNECTIONS];
        unsigned slots[TS_WS_CONNECTIONS], count=0;
        uint64_t revisions[TS_WS_CONNECTIONS];
        capacity=ts_ws_transport_capacity();
        portENTER_CRITICAL(&s_lock);
        if(s_state==RUNNING) for(unsigned j=0;j<MAX_SUBSCRIPTIONS && count<capacity && count<TS_WS_CONNECTIONS;j++) {
            unsigned i=(s_subscription_cursor+j)%MAX_SUBSCRIPTIONS;
            subscription_t *sub=&s_subs[i];
            if(sub->active && sub->topic==topic && !sub->pending && now>=sub->next_attempt) {
                peers[count]=sub->peer;slots[count]=i;revisions[count++]=sub->revision;
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if(!count || now<s_allocation_retry_at)continue;
        esp_err_t reserved_result=ts_ws_reserve(peers,count,false,TS_WS_FRAME_BYTES,&reserved[topic]);
        if(reserved_result!=ESP_OK) {
            /* A heap failure with available slots must not spin at tick rate. */
            if(reserved_result==ESP_ERR_NO_MEM && ts_ws_message_capacity())s_allocation_retry_at=after_ms(now,100);
            continue;
        }
        /* Resources are owned before pending/API work. Admission and revisions
         * may have changed during allocation; such targets retire normally. */
        portENTER_CRITICAL(&s_lock);
        for(unsigned j=0;j<count;j++) {
            subscription_t *sub=&s_subs[slots[j]];
            if(s_state==RUNNING && sub->active && sub->revision==revisions[j] && !sub->pending && reserve(sub,&targets[n])) n++;
            else targets[n++]=(target_t){.topic=topic};
        }
        s_subscription_cursor=(slots[count-1]+1)%MAX_SUBSCRIPTIONS;
        portEXIT_CRITICAL(&s_lock);
        s_topic_cursor=(topic+1)%8;
    }
    if(n) { TS_WS_TEST_POINT("snapshot"); collect_batch(targets,n,reserved); }
    /* Event FIFO per subscription. A slow target does not hold up other targets.
     * Different operation results never overwrite each other. */
    for(unsigned slot=0;slot<EVENT_SLOTS;slot++) {
        ts_ws_message_t *release=NULL;
        n=0; capacity=ts_ws_transport_capacity(); now=esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        event_item_t *e=&s_events[slot];
        if(e->ready) {
            for(unsigned j=0;j<MAX_SUBSCRIPTIONS;j++) if(e->targets[j]) {
                subscription_t *s=find_revision(e->targets[j]);
                if(!s || (s_state!=RUNNING && s_state!=DRAINING)) {e->targets[j]=0;continue;}
                bool earlier=false;
                for(unsigned k=0;k<EVENT_SLOTS;k++) if(s_events[k].used && s_events[k].order<e->order)
                    for(unsigned l=0;l<MAX_SUBSCRIPTIONS;l++) if(s_events[k].targets[l]==s->revision) earlier=true;
                if(!earlier && !s->pending && now>=s->next_attempt && n<capacity && reserve(s,&targets[n])) {
                    n++;e->targets[j]=0;
                }
            }
            bool remaining=false;
            for(unsigned j=0;j<MAX_SUBSCRIPTIONS;j++) if(e->targets[j]) remaining=true;
            if(!remaining) {release=e->message; e->ready=false; /* held until submits finish */}
        }
        ts_ws_message_t *m=e->message;
        portEXIT_CRITICAL(&s_lock);
        for(unsigned j=0;j<n;j++) submit(targets[j],m);
        if(release) {
            ts_ws_message_release(release);
            portENTER_CRITICAL(&s_lock); memset(e,0,sizeof(*e)); portEXIT_CRITICAL(&s_lock);
        }
    }
}
static TickType_t wait_ticks(int64_t delta)
{
    if(delta<=0) return 1;
    uint64_t tick_us=1000000/configTICK_RATE_HZ;
    uint64_t ticks=((uint64_t)delta+tick_us-1)/tick_us;
    if(ticks>=portMAX_DELAY) ticks=portMAX_DELAY-1;
    return (TickType_t)ticks;
}
static TickType_t next_wait(void)
{
    if(ts_ws_transport_needs_flush()) return wait_ticks((int64_t)ts_ws_transport_retry_ms()*1000);
    int64_t now=esp_timer_get_time(), nearest=ts_ws_op_needs_tick()?esp_timer_get_time()+100000:INT64_MAX;
    unsigned capacity=ts_ws_message_capacity() ? ts_ws_transport_capacity() : 0;
    portENTER_CRITICAL(&s_lock);
    for(unsigned i=0;i<MAX_SUBSCRIPTIONS;i++) {
        subscription_t *s=&s_subs[i];
        if(!s->active || s->pending) continue;
        bool wanted=s->topic<8;
        for(unsigned j=0;j<EVENT_SLOTS && !wanted;j++) if(s_events[j].ready)
            for(unsigned k=0;k<MAX_SUBSCRIPTIONS;k++) if(s_events[j].targets[k]==s->revision) wanted=true;
        if(wanted && s->next_attempt<nearest) nearest=s->next_attempt;
    }
    if(nearest==INT64_MAX) s_stats.idle_wait++;
    if(!capacity && nearest!=INT64_MAX) {nearest=after_ms(now,1000);s_stats.capacity_wait++;}
    portEXIT_CRITICAL(&s_lock);
    if(nearest!=INT64_MAX && nearest<s_allocation_retry_at)nearest=s_allocation_retry_at;
    return nearest==INT64_MAX ? portMAX_DELAY : wait_ticks(nearest-now);
}
static void telemetry_worker(void *arg)
{
    (void)arg;
    for(;;) {
        portENTER_CRITICAL(&s_lock); lifecycle_t state=s_state; portEXIT_CRITICAL(&s_lock);
        if(state==STOPPING) break;
        if(state==RUNNING || state==DRAINING) run_batch();
        uint32_t water=uxTaskGetStackHighWaterMark(NULL);
        portENTER_CRITICAL(&s_lock);s_stats.stack_low_water=water;portEXIT_CRITICAL(&s_lock);
        /* Notification arriving between deadline calculation and wait is retained. */
        ulTaskNotifyTake(pdTRUE,state==STARTING ? portMAX_DELAY : next_wait());
    }
    /* Handle is invalidated under same lock used by every notifier. */
    portENTER_CRITICAL(&s_lock); s_worker=NULL; portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}
static void event_handler(const ts_event_t *event, void *arg)
{
    unsigned topic=(unsigned)(uintptr_t)arg;
    bool demand=false;
    portENTER_CRITICAL(&s_lock);
    if(s_state==RUNNING) for(unsigned i=0;i<MAX_SUBSCRIPTIONS;i++)
        if(s_subs[i].active && (topic==0 ? s_subs[i].topic<8 : s_subs[i].topic==topic)) demand=true;
    if(topic==0 && demand) wake_locked();
    portEXIT_CRITICAL(&s_lock);
    if(topic==0 || !demand) return;
    if(!event || !event->data || !event->data_size) return;
    /* Parse bounded event bytes, not an assumed NUL terminated buffer. */
    cJSON *data=cJSON_ParseWithLength(event->data,event->data_size);
    if(data) {ts_ws_broadcast_to_topic(topics[topic].name,data);cJSON_Delete(data);}
}
esp_err_t ts_ws_subscriptions_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if(s_state!=OFF) {esp_err_t ret=s_state==RUNNING?ESP_OK:ESP_ERR_INVALID_STATE;portEXIT_CRITICAL(&s_lock);return ret;}
    s_state=STARTING;
    portEXIT_CRITICAL(&s_lock);
    const ts_event_base_t bases[]={TS_EVENT_BASE_SYSTEM,TS_EVENT_BASE_DEVICE_MON,TS_EVENT_BASE_OTA};
    const int ids[]={TS_EVENT_SYSTEM_INFO_CHANGED,TS_EVENT_DEVICE_STATUS_CHANGED,TS_EVENT_OTA_PROGRESS_UPDATE};
    const unsigned mapping[]={0,8,9};
    esp_err_t ret=ESP_OK;
    for(unsigned i=0;i<3;i++) {
        ret=ts_event_register(bases[i],ids[i],event_handler,(void*)(uintptr_t)mapping[i],&s_handlers[i]);
        if(ret!=ESP_OK) break;
    }
    if(ret==ESP_OK && xTaskCreate(telemetry_worker,"ws_telemetry",WORKER_STACK,NULL,2,&s_worker)!=pdPASS) ret=ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);s_state=ret==ESP_OK?RUNNING:STOPPING;wake_locked();portEXIT_CRITICAL(&s_lock);
    if(ret!=ESP_OK) {esp_err_t cleanup=ts_ws_subscriptions_deinit();if(cleanup!=ESP_OK)return cleanup;}
    return ret;
}
/* Stop telemetry/new subscriptions, but keep result subscriptions and the shared
 * sender alive until their producers have finished. Safe to retry. */
esp_err_t ts_ws_subscriptions_pause(void)
{
    portENTER_CRITICAL(&s_lock);
    esp_err_t ret=ESP_OK;
    if(s_state==RUNNING) {
        s_state=DRAINING;
        for(unsigned i=0;i<MAX_SUBSCRIPTIONS;i++) if(s_subs[i].topic<8) s_subs[i].active=false;
        wake_locked();
    } else if(s_state!=DRAINING && s_state!=OFF) ret=ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&s_lock);
    return ret;
}
/* The owner has closed business admission and waited for result producers. */
esp_err_t ts_ws_subscriptions_drain(void)
{
    if(ts_event_in_callback() || ts_ws_transport_in_context()) return ESP_ERR_INVALID_STATE;
    for(unsigned i=0;i<3;i++) if(s_handlers[i]) {
        esp_err_t ret=ts_event_unregister_sync(s_handlers[i],STOP_TIMEOUT_MS);
        if(ret!=ESP_OK && ret!=ESP_ERR_NOT_FOUND) return ret;
        s_handlers[i]=NULL;
    }
    int64_t deadline=after_ms(esp_timer_get_time(),STOP_TIMEOUT_MS);
    for(;;) {
        portENTER_CRITICAL(&s_lock);
        bool busy=s_entries || s_outstanding;
        for(unsigned i=0;i<EVENT_SLOTS;i++) busy |= s_events[i].used;
        wake_locked();
        portEXIT_CRITICAL(&s_lock);
        if(!busy) return ESP_OK;
        if(esp_timer_get_time()>=deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}
esp_err_t ts_ws_subscriptions_deinit(void)
{
    if(ts_event_in_callback() || ts_ws_transport_in_context()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if(s_state==OFF) {portEXIT_CRITICAL(&s_lock);return ESP_OK;}
    if(s_stop_owner || s_state==STARTING || s_worker==xTaskGetCurrentTaskHandle()) {portEXIT_CRITICAL(&s_lock);return ESP_ERR_INVALID_STATE;}
    s_stop_owner=true;s_state=STOPPING;
    for(unsigned i=0;i<MAX_SUBSCRIPTIONS;i++) s_subs[i].active=false;
    wake_locked();portEXIT_CRITICAL(&s_lock);
    esp_err_t ret=ESP_OK;
    for(unsigned i=0;i<3;i++) if(s_handlers[i]) {
        ret=ts_event_unregister_sync(s_handlers[i],STOP_TIMEOUT_MS);
        if(ret!=ESP_OK && ret!=ESP_ERR_NOT_FOUND) goto done;
        s_handlers[i]=NULL;
    }
    int64_t deadline=after_ms(esp_timer_get_time(),STOP_TIMEOUT_MS);
    for(;;) {
        ts_ws_transport_cancel_topics();
        ts_ws_transport_flush();
        portENTER_CRITICAL(&s_lock); bool drained=!s_entries && !s_worker && !s_outstanding;portEXIT_CRITICAL(&s_lock);
        if(drained) break;
        if(esp_timer_get_time()>=deadline) {ret=ESP_ERR_TIMEOUT;goto done;}
        vTaskDelay(1);
    }
    for(unsigned i=0;i<EVENT_SLOTS;i++) ts_ws_message_release(s_events[i].message);
    portENTER_CRITICAL(&s_lock);
    memset(s_events,0,sizeof(s_events));memset(s_subs,0,sizeof(s_subs));s_state=OFF;
    portEXIT_CRITICAL(&s_lock);
    ret=ESP_OK;
done:
    portENTER_CRITICAL(&s_lock);s_stop_owner=false;portEXIT_CRITICAL(&s_lock);
    return ret;
}
esp_err_t ts_ws_subscribe(ts_ws_peer_t peer,const char *topic,cJSON *params)
{
    int id=topic_id(topic);if(id<0)return ESP_ERR_NOT_FOUND;
    uint32_t interval=topics[id].interval;
    cJSON *p=params?cJSON_GetObjectItemCaseSensitive(params,"interval"):NULL;
    if(p) {
        double v=p->valuedouble;
        if(!cJSON_IsNumber(p)||!isfinite(v)||v<0||v>UINT32_MAX||floor(v)!=v||(v==0 && id<8)) return ESP_ERR_INVALID_ARG;
        interval=(uint32_t)v;
        if(id<8 && interval<topics[id].interval) interval=topics[id].interval;
    }
    ts_ws_peer_t current;
    if(!ts_ws_peer_get(peer.server,peer.fd,&current)||!ts_ws_peer_equal(peer,current))return ESP_ERR_INVALID_STATE;
    if(!enter(false)) return ESP_ERR_INVALID_STATE;
    int64_t now=esp_timer_get_time();
    esp_err_t ret=ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);
    subscription_t *slot=NULL;
    for(unsigned i=0;i<MAX_SUBSCRIPTIONS;i++) {
        subscription_t *s=&s_subs[i];
        if(s->active && s->topic==(unsigned)id && ts_ws_peer_equal(s->peer,peer)) {slot=s;break;}
        if(!s->active && !slot) slot=s;
    }
    if(s_state==RUNNING && slot && s_sequence!=UINT64_MAX) {
        if(!slot->active || slot->interval!=interval) {
            int64_t last=slot->active?slot->last_success:0;
            *slot=(subscription_t){.peer=peer,.revision=++s_sequence,.topic=id,.active=true,
                .interval=interval,.last_success=last,
                .next_attempt=id<8?after_ms(last?last:now,interval):now};
        }
        ret=ESP_OK;wake_locked();
    }
    portEXIT_CRITICAL(&s_lock);leave();return ret;
}
esp_err_t ts_ws_unsubscribe(ts_ws_peer_t peer,const char *topic)
{
    int id=topic_id(topic);if(id<0)return ESP_ERR_NOT_FOUND;
    if(!enter(true))return ESP_ERR_INVALID_STATE;
    esp_err_t ret=ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&s_lock);
    for(unsigned i=0;i<MAX_SUBSCRIPTIONS;i++) if(s_subs[i].active && s_subs[i].topic==(unsigned)id && ts_ws_peer_equal(s_subs[i].peer,peer)) {
        s_subs[i].active=false;ret=ESP_OK;
    }
    wake_locked();portEXIT_CRITICAL(&s_lock);leave();return ret;
}
void ts_ws_client_disconnected(ts_ws_peer_t peer)
{
    portENTER_CRITICAL(&s_lock);
    for(unsigned i=0;i<MAX_SUBSCRIPTIONS;i++) if(s_subs[i].active && ts_ws_peer_equal(s_subs[i].peer,peer))s_subs[i].active=false;
    wake_locked();portEXIT_CRITICAL(&s_lock);
}
void ts_ws_broadcast_to_topic(const char *topic,cJSON *data)
{
    int id=topic_id(topic);
    if(id<0 || !data || !enter(true))return;
    event_item_t *e=NULL;
    portENTER_CRITICAL(&s_lock);
    if(s_sequence!=UINT64_MAX) for(unsigned i=0;i<EVENT_SLOTS;i++) if(!s_events[i].used) {e=&s_events[i];break;}
    if(e) {
        memset(e,0,sizeof(*e));e->used=true;e->order=++s_sequence;
        bool any=false;
        for(unsigned i=0;i<MAX_SUBSCRIPTIONS;i++) if(s_subs[i].active && s_subs[i].topic==(unsigned)id) {e->targets[i]=s_subs[i].revision;any=true;}
        if(!any) {e->used=false;e=NULL;}
    } else s_stats.event_dropped++;
    portEXIT_CRITICAL(&s_lock);
    if(e) {
        ts_ws_message_t *m=ts_ws_message_json(topic,data,esp_timer_get_time()/1000000);
        portENTER_CRITICAL(&s_lock);
        if(m) {e->message=m;e->ready=true;s_stats.serialized[id]++;wake_locked();}
        else {memset(e,0,sizeof(*e));s_stats.event_dropped++;}
        portEXIT_CRITICAL(&s_lock);
        if(!m) TS_LOGW(TAG,"Topic event rejected: frame budget or allocation exhausted");
    }
    leave();
}

bool ts_ws_subscriptions_in_context(void)
{
    TaskHandle_t current=xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_lock);bool owner=s_worker && s_worker==current;portEXIT_CRITICAL(&s_lock);
    return owner;
}
