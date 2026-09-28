#include "ts_ws_transport.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* Stable control storage: no caller can race destruction of a mutex. No I/O,
 * allocation, JSON operation or completion callback runs under this spinlock. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_sequence;
static struct {
    httpd_handle_t handle;
    uint64_t epoch;
    bool stopping, barrier_requested, quiesced, work_busy;
    unsigned outstanding;
    int64_t retry_at;
    TaskHandle_t owner;
    ts_ws_closed_t closed;
} s_server;
static ts_ws_peer_t s_peers[TS_WS_CONNECTIONS];
static int s_log_levels[TS_WS_CONNECTIONS];
static uint64_t s_log_revisions[TS_WS_CONNECTIONS];
typedef enum {MSG_ORDINARY, MSG_TOPIC, MSG_RESULT, MSG_POWER, MSG_POWER_TICK} message_kind_t;
struct ts_ws_message { unsigned refs; char *text; size_t len, capacity; message_kind_t kind; };
static char s_power_payloads[TS_WS_POWER_SLOTS][TS_WS_POWER_BYTES];
static unsigned s_power_bytes;
static ts_ws_transport_stats_t s_tx_stats;
static ts_ws_message_t s_messages[TS_WS_ALL_MESSAGE_SLOTS];
typedef enum {TX_FREE, TX_RESERVED, TX_PREPARED, TX_QUEUED, TX_EXECUTING, TX_DONE, TX_CANCELED} tx_state_t;
typedef struct {
    unsigned refs, retries;
    tx_state_t state;
    uint64_t order;
    ts_ws_peer_t peer;
    ts_ws_message_t *message;
    uint64_t revision, delivery;
    ts_ws_delivery_check_t check;
    ts_ws_delivery_done_t done;
} delivery_t;
static delivery_t s_deliveries[TS_WS_ALL_TX_SLOTS];
static uint64_t s_delivery_order;
extern void ts_ws_subscriptions_wake(void);

bool ts_ws_peer_equal(ts_ws_peer_t a, ts_ws_peer_t b)
{
    return a.server == b.server && a.epoch == b.epoch &&
           a.fd == b.fd && a.connection == b.connection;
}
static bool peer_valid(ts_ws_peer_t peer)
{
    if (s_server.handle != peer.server || s_server.epoch != peer.epoch)
        return false;
    for (unsigned i = 0; i < TS_WS_CONNECTIONS; ++i)
        if (s_peers[i].connection && ts_ws_peer_equal(s_peers[i], peer)) return true;
    return false;
}
bool ts_ws_transport_in_context(void)
{
    portENTER_CRITICAL(&s_lock);
    bool same = s_server.owner && s_server.owner == xTaskGetCurrentTaskHandle();
    portEXIT_CRITICAL(&s_lock);
    return same;
}
esp_err_t ts_ws_transport_start(httpd_handle_t server, ts_ws_closed_t closed)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!s_server.handle && s_sequence != UINT64_MAX) {
        s_server.handle = server;
        s_server.epoch = ++s_sequence;
        s_server.closed = closed;
        s_server.stopping = false;
        ret = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}
bool ts_ws_peer_get(httpd_handle_t server, int fd, ts_ws_peer_t *peer)
{
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < TS_WS_CONNECTIONS; ++i) {
        if (s_peers[i].connection && s_peers[i].server == server && s_peers[i].fd == fd) {
            *peer = s_peers[i]; found = true; break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}
void ts_ws_peer_close(ts_ws_peer_t peer)
{
    ts_ws_closed_t closed = NULL;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < TS_WS_CONNECTIONS; ++i) {
        if (s_peers[i].connection && ts_ws_peer_equal(s_peers[i], peer)) {
            s_peers[i].connection = 0;
            closed = s_server.closed;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (closed) closed(peer);
}
static void session_free(void *ctx)
{
    ts_ws_peer_t *peer = ctx;
    ts_ws_peer_close(*peer);
    free(peer);
}
esp_err_t ts_ws_peer_open(httpd_req_t *req, ts_ws_peer_t *peer)
{
    /* No authentication / TLS context is replaced. Current /ws owns no context. */
    if (req->sess_ctx) return ESP_ERR_INVALID_STATE;
    ts_ws_peer_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    esp_err_t ret = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);
    s_server.owner = xTaskGetCurrentTaskHandle();
    if (!s_server.stopping && req->handle == s_server.handle && s_sequence != UINT64_MAX) {
        for (unsigned i = 0; i < TS_WS_CONNECTIONS; ++i) if (!s_peers[i].connection) {
            *ctx = (ts_ws_peer_t){req->handle, s_server.epoch, ++s_sequence, httpd_req_to_sockfd(req)};
            s_peers[i] = *ctx; s_log_levels[i] = -1; s_log_revisions[i] = 0; *peer = *ctx; ret = ESP_OK; break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (ret != ESP_OK) { free(ctx); return ret; }
    req->sess_ctx = ctx;
    req->free_ctx = session_free; /* RST, LRU and httpd_stop also release this. */
    return ESP_OK;
}
static ts_ws_message_t *message_alloc(size_t capacity, message_kind_t kind)
{
    if(kind>=MSG_POWER && capacity>TS_WS_POWER_BYTES)return NULL;
    ts_ws_message_t *m = NULL;
    portENTER_CRITICAL(&s_lock);
    unsigned begin=kind>=MSG_POWER ? TS_WS_MESSAGE_SLOTS+2+(kind==MSG_POWER_TICK?TS_WS_POWER_TRANSITIONS:0) : kind==MSG_RESULT ? TS_WS_MESSAGE_SLOTS : kind==MSG_TOPIC ? 0 : TS_WS_MESSAGE_SLOTS-2;
    unsigned end=kind==MSG_POWER_TICK ? TS_WS_ALL_MESSAGE_SLOTS : kind==MSG_POWER ? TS_WS_ALL_MESSAGE_SLOTS-1 : kind==MSG_RESULT ? TS_WS_MESSAGE_SLOTS+2 : kind==MSG_TOPIC ? TS_WS_MESSAGE_SLOTS-2 : TS_WS_MESSAGE_SLOTS;
    size_t used=kind>=MSG_POWER ? s_power_bytes : s_tx_stats.bytes-s_power_bytes;
    size_t limit=kind>=MSG_POWER ? TS_WS_POWER_BUDGET : TS_WS_TOTAL_BYTES-TS_WS_POWER_BUDGET;
    for (unsigned i=begin; capacity <= limit-used && i<end; ++i) if (!s_messages[i].refs) {
        m = &s_messages[i]; m->refs = 1; m->capacity = capacity; m->kind=kind;
        if(kind>=MSG_POWER)s_power_bytes+=capacity;
        s_tx_stats.bytes += capacity;
        if(s_tx_stats.bytes > s_tx_stats.bytes_high) s_tx_stats.bytes_high=s_tx_stats.bytes;
        break;
    }
    portEXIT_CRITICAL(&s_lock);
    if (m) {
        m->text = kind>=MSG_POWER ? s_power_payloads[m-s_messages-TS_WS_MESSAGE_SLOTS-2] : malloc(capacity);
        if (!m->text) {
            portENTER_CRITICAL(&s_lock);s_tx_stats.rejected++;portEXIT_CRITICAL(&s_lock);
            ts_ws_message_release(m);return NULL;
        }
    } else {
        portENTER_CRITICAL(&s_lock);s_tx_stats.rejected++;portEXIT_CRITICAL(&s_lock);
    }
    return m;
}
void ts_ws_message_release(ts_ws_message_t *m)
{
    if (!m) return;
    char *text = NULL;
    bool reclaim = false;
    portENTER_CRITICAL(&s_lock);
    if (--m->refs == 0) {
        /* Keep this slot and its byte reservation until free has completed. */
        m->refs = 1;
        text = m->text;
        reclaim = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!reclaim) return;
    if(m->kind<MSG_POWER)free(text);
    portENTER_CRITICAL(&s_lock);
    if(m->kind>=MSG_POWER)s_power_bytes-=m->capacity;
    m->text = NULL;
    m->len = 0;
    s_tx_stats.bytes -= m->capacity;
    m->refs = 0;
    portEXIT_CRITICAL(&s_lock);
}

ts_ws_message_t *ts_ws_message_text(const char *text, size_t len)
{
    if (!text || len >= TS_WS_LEGACY_BYTES) return NULL;
    ts_ws_message_t *m = message_alloc(len + 1, false);
    if (m) { memcpy(m->text, text, len); m->text[len] = 0; m->len = len; }
    return m;
}
ts_ws_message_t *ts_ws_message_json(const char *topic, const cJSON *data, int64_t timestamp)
{
    ts_ws_message_t *m = message_alloc(TS_WS_FRAME_BYTES, true);
    if (!m) return NULL;
    cJSON *root = cJSON_CreateObject();
    /* Reference is local to this call; bounded print copies before return. */
    bool ok = root && cJSON_AddStringToObject(root, "type", "data") &&
        cJSON_AddStringToObject(root, "topic", topic) &&
        cJSON_AddNumberToObject(root, "timestamp", timestamp) &&
        ts_ws_json_reference(root, "data", (cJSON *)data) &&
        cJSON_PrintPreallocated(root, m->text, TS_WS_FRAME_BYTES, false);
    cJSON_Delete(root);
    if (!ok) { ts_ws_message_release(m); return NULL; }
    m->len = strlen(m->text);
    return m;
}
unsigned ts_ws_transport_capacity(void)
{
    unsigned n = 0;
    portENTER_CRITICAL(&s_lock);
    if (!s_server.stopping && s_server.handle)
        for (unsigned i = 0; i < TS_WS_TOPIC_TX_SLOTS; ++i) n += !s_deliveries[i].refs;
    portEXIT_CRITICAL(&s_lock);
    return n;
}
static void delivery_release(delivery_t *d)
{
    ts_ws_message_t *m = NULL;
    portENTER_CRITICAL(&s_lock);
    if (--d->refs == 0) {
        m = d->message;
        d->state=TX_FREE;
        s_server.outstanding--;
        s_tx_stats.jobs--;
    }
    portEXIT_CRITICAL(&s_lock);
    ts_ws_message_release(m);
}
/* Exactly one settlement for every accepted work reference, including stale
 * targets and queue rejection. Class is owned by the message, never by done. */
static void settle(delivery_t *d, esp_err_t result)
{
    portENTER_CRITICAL(&s_lock);
    s_tx_stats.settled++;
    bool power=d->message->kind>=MSG_POWER;
    if(power){s_tx_stats.power_settled++;if(result!=ESP_OK)s_tx_stats.power_failed++;}
    portEXIT_CRITICAL(&s_lock);
    if(d->message->kind>=MSG_RESULT && result!=ESP_OK)ESP_LOGW("ws_tx","Critical notification target failed: class=%d fd=%d result=%d",d->message->kind,d->peer.fd,result);
    if(d->done)d->done(d->revision,d->delivery,result);
}
static void deliver(void *arg)
{
    delivery_t *d = arg;
    portENTER_CRITICAL(&s_lock);
    s_server.owner = xTaskGetCurrentTaskHandle();
    d->state=TX_EXECUTING;
    bool valid = peer_valid(d->peer);
    portEXIT_CRITICAL(&s_lock);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    /* HTTPD session destruction/handshake/unsubscribe and this send are ordered
     * on this same task. The stop owner keeps the server alive until refs drain. */
    if (valid && (!d->check || d->check(d->revision, d->delivery)) &&
        httpd_ws_get_fd_info(d->peer.server, d->peer.fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)d->message->text, .len = d->message->len};
        int64_t start = esp_timer_get_time();
        ret = httpd_ws_send_frame_async(d->peer.server, d->peer.fd, &frame);
        int64_t elapsed = esp_timer_get_time() - start;
        portENTER_CRITICAL(&s_lock);
        s_tx_stats.attempts++;
        if(ret==ESP_OK) s_tx_stats.success++; else s_tx_stats.failed++;
        if(elapsed>s_tx_stats.max_send_us) s_tx_stats.max_send_us=elapsed;
        portEXIT_CRITICAL(&s_lock);
        if (ret != ESP_OK) {
            /* Owner context: invalidate this socket without taking over close()
             * ownership. HTTPD will observe EOF and invoke the session hook. */
            shutdown(d->peer.fd, SHUT_RDWR);
            ts_ws_peer_close(d->peer);
        }
    } else {
        portENTER_CRITICAL(&s_lock); s_tx_stats.stale++; portEXIT_CRITICAL(&s_lock);
    }
    TS_WS_TEST_POINT("sent");
    settle(d,ret);
    portENTER_CRITICAL(&s_lock);
    d->state=TX_DONE;
    s_server.work_busy=false;
    portEXIT_CRITICAL(&s_lock);
    delivery_release(d);
    ts_ws_subscriptions_wake();
}
/* One SDK control work item at a time. Accepted descriptors stay in our bounded
 * FIFO, not the six-entry lwIP UDP control mailbox. Completion only wakes the
 * telemetry worker; it never recursively queues another network operation. */
static esp_err_t publish_delivery(delivery_t *d)
{
    esp_err_t ret=httpd_queue_work(d->peer.server,deliver,d);
    portENTER_CRITICAL(&s_lock);
    bool retry=ret!=ESP_OK && d->message->kind>=MSG_RESULT && ++d->retries<TS_WS_QUEUE_RETRIES;
    if(ret==ESP_OK){s_tx_stats.queued++;s_server.retry_at=0;}
    else {
        s_tx_stats.rejected++;
        d->state=retry?TX_PREPARED:TX_CANCELED;
        if(retry)s_server.retry_at=esp_timer_get_time()+100000;
    }
    portEXIT_CRITICAL(&s_lock);
    if(ret!=ESP_OK){
        if(!retry) {
            settle(d,ret);
            delivery_release(d); /* failed ordinary work reference */
        }
        /* A failure callback may enqueue its reserved terminal. Keep the
         * executor busy until settlement ends to avoid recursive queue failures. */
        portENTER_CRITICAL(&s_lock);s_server.work_busy=false;portEXIT_CRITICAL(&s_lock);
        ts_ws_subscriptions_wake();
    }
    delivery_release(d); /* publication reference, may outlive the callback */
    return ret;
}
void ts_ws_transport_flush(void)
{
    delivery_t *next=NULL;
    portENTER_CRITICAL(&s_lock);
    if(s_server.handle && !s_server.work_busy && esp_timer_get_time()>=s_server.retry_at) {
        for(unsigned i=0;i<TS_WS_ALL_TX_SLOTS;i++)
            if(s_deliveries[i].state==TX_PREPARED && (!next || s_deliveries[i].order<next->order))next=&s_deliveries[i];
        if(next){next->state=TX_QUEUED;next->refs++;s_server.work_busy=true;}
    }
    portEXIT_CRITICAL(&s_lock);
    if(next)publish_delivery(next);
}
bool ts_ws_transport_needs_flush(void)
{
    bool ready=false;
    portENTER_CRITICAL(&s_lock);
    if(s_server.handle && !s_server.work_busy)
        for(unsigned i=0;i<TS_WS_ALL_TX_SLOTS;i++)if(s_deliveries[i].state==TX_PREPARED)ready=true;
    portEXIT_CRITICAL(&s_lock);
    return ready;
}
static void cancel_ready(bool topics_only)
{
    for(unsigned i=0;i<TS_WS_ALL_TX_SLOTS;i++){
        delivery_t *d=&s_deliveries[i];
        portENTER_CRITICAL(&s_lock);
        bool cancel=d->state==TX_PREPARED && (!topics_only || i<TS_WS_TOPIC_TX_SLOTS);
        if(cancel){d->state=TX_CANCELED;s_tx_stats.stale++;} /* take its existing work reference */
        portEXIT_CRITICAL(&s_lock);
        if(cancel){settle(d,ESP_ERR_INVALID_STATE);delivery_release(d);}
    }
}
void ts_ws_transport_cancel_topics(void){cancel_ready(true);}
esp_err_t ts_ws_transport_submit(ts_ws_peer_t peer, ts_ws_message_t *message,
    uint64_t revision, uint64_t delivery, ts_ws_delivery_check_t check, ts_ws_delivery_done_t done)
{
    if(!message || message->kind>MSG_TOPIC)return ESP_ERR_INVALID_ARG;
    delivery_t *d=NULL;
    portENTER_CRITICAL(&s_lock);
    if(!s_server.stopping && peer_valid(peer) && s_delivery_order!=UINT64_MAX)
        for(unsigned i=message->kind==MSG_TOPIC?0:TS_WS_TOPIC_TX_SLOTS;i<(message->kind==MSG_TOPIC?TS_WS_TOPIC_TX_SLOTS:TS_WS_TX_SLOTS);i++)if(!s_deliveries[i].refs){
            d=&s_deliveries[i];
            *d=(delivery_t){.refs=2,.state=TX_PREPARED,.order=++s_delivery_order,.peer=peer,
                .message=message,.revision=revision,.delivery=delivery,.check=check,.done=done};
            message->refs++;s_server.outstanding++;s_tx_stats.jobs++;
            if(s_tx_stats.jobs>s_tx_stats.jobs_high)s_tx_stats.jobs_high=s_tx_stats.jobs;
            break;
        }
    if(!d)s_tx_stats.rejected++;
    portEXIT_CRITICAL(&s_lock);
    if(!d)return ESP_ERR_NO_MEM;
    /* Accepted into the local FIFO. Queue/send failures are settled through done,
     * exactly once. The producer reference prevents early completion reuse. */
    ts_ws_transport_flush();
    TS_WS_TEST_POINT("B05"); /* done may have returned; producer ref still held */
    delivery_release(d);
    ts_ws_subscriptions_wake();
    return ESP_OK;
}
esp_err_t ts_ws_transport_send(httpd_handle_t server, int fd, const httpd_ws_frame_t *frame)
{
    ts_ws_peer_t peer;
    if (!ts_ws_peer_get(server, fd, &peer)) return ESP_ERR_INVALID_STATE;
    if(ts_ws_transport_in_context()) {
        /* Current HTTP request owns the session until it returns; HTTPD stop
         * waits for that request. Keep synchronous terminal takeover ordering. */
        return httpd_ws_send_frame_async(server,fd,(httpd_ws_frame_t *)frame);
    }
    ts_ws_message_t *m = ts_ws_message_text((const char *)frame->payload, frame->len);
    if (!m) return ESP_ERR_NO_MEM;
    esp_err_t ret = ts_ws_transport_submit(peer, m, 0, 0, NULL, NULL);
    ts_ws_message_release(m);
    return ret;
}
esp_err_t ts_ws_transport_stop(httpd_handle_t server, uint32_t timeout_ms)
{
    if (ts_ws_transport_in_context()) return ESP_ERR_INVALID_STATE;
    int64_t now = esp_timer_get_time(), delta = (int64_t)timeout_ms * 1000;
    int64_t deadline = now > INT64_MAX - delta ? INT64_MAX : now + delta;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        if (!s_server.handle) { portEXIT_CRITICAL(&s_lock); return ESP_OK; }
        if (server != s_server.handle) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_ARG; }
        s_server.stopping = true;
        portEXIT_CRITICAL(&s_lock);
        ts_ws_transport_flush(); /* drain accepted results; never cancel them on normal stop */
        portENTER_CRITICAL(&s_lock);
        unsigned outstanding=s_server.outstanding;
        portEXIT_CRITICAL(&s_lock);
        if (!outstanding) return ESP_OK;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}
/* Called only after actual HTTPD stop has freed all sessions. */
void ts_ws_transport_stopped(httpd_handle_t server)
{
    portENTER_CRITICAL(&s_lock);
    if (s_server.handle == server && s_server.stopping && !s_server.outstanding) {
        memset(&s_server, 0, sizeof(s_server));
        memset(s_peers, 0, sizeof(s_peers));
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool log_allowed(uint64_t revision, uint64_t level)
{
    bool allowed=false;
    portENTER_CRITICAL(&s_lock);
    for(unsigned i=0;i<TS_WS_CONNECTIONS;i++)
        if(s_peers[i].connection && s_log_revisions[i]==revision && s_log_levels[i]>=0 && level<=(unsigned)s_log_levels[i]) allowed=true;
    portEXIT_CRITICAL(&s_lock);
    return allowed;
}
static esp_err_t broadcast_filtered(const httpd_ws_frame_t *frame, int level)
{
    ts_ws_peer_t peers[TS_WS_CONNECTIONS];
    uint64_t revisions[TS_WS_CONNECTIONS];
    unsigned n=0;
    portENTER_CRITICAL(&s_lock);
    if(!s_server.stopping) for(unsigned i=0;i<TS_WS_CONNECTIONS;i++)
        if(s_peers[i].connection && (level < 0 || level <= s_log_levels[i])) {revisions[n]=s_log_revisions[i];peers[n++]=s_peers[i];}
    portEXIT_CRITICAL(&s_lock);
    if(!n) return ESP_OK;
    ts_ws_message_t *m=ts_ws_message_text((const char*)frame->payload,frame->len);
    if(!m) return ESP_ERR_NO_MEM;
    esp_err_t ret=ESP_OK;
    for(unsigned i=0;i<n;i++) {
        esp_err_t result=ts_ws_transport_submit(peers[i],m,revisions[i],level,level<0?NULL:log_allowed,NULL);
        if(result!=ESP_OK)ret=result;
    }
    ts_ws_message_release(m);
    return ret;
}

void ts_ws_peer_log_level(ts_ws_peer_t peer, int level)
{
    portENTER_CRITICAL(&s_lock);
    for(unsigned i=0;i<TS_WS_CONNECTIONS;i++)
        if(s_peers[i].connection && ts_ws_peer_equal(s_peers[i],peer)) {
            s_log_levels[i]=s_sequence==UINT64_MAX ? -1 : level;
            s_log_revisions[i]=s_sequence==UINT64_MAX ? 0 : ++s_sequence;
        }
    portEXIT_CRITICAL(&s_lock);
}
esp_err_t ts_ws_transport_broadcast(const httpd_ws_frame_t *frame) {return broadcast_filtered(frame,-1);}
esp_err_t ts_ws_transport_log(const httpd_ws_frame_t *frame, int level) {return broadcast_filtered(frame,level);}
unsigned ts_ws_message_capacity(void)
{
    unsigned n=0;
    portENTER_CRITICAL(&s_lock);
    if(s_tx_stats.bytes-s_power_bytes <= TS_WS_TOTAL_BYTES-TS_WS_POWER_BUDGET-TS_WS_FRAME_BYTES)
        for(unsigned i=0;i<TS_WS_MESSAGE_SLOTS-2;i++) n+=!s_messages[i].refs;
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void ts_ws_transport_get_stats(ts_ws_transport_stats_t *stats)
{
    if(!stats)return;
    portENTER_CRITICAL(&s_lock);*stats=s_tx_stats;portEXIT_CRITICAL(&s_lock);
}

/* Admission has already been closed by WebUI. Reaching this work item proves
 * the currently executing HTTP handler has returned; it does NOT drain TX. */
static void quiesce_work(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_lock);
    s_server.owner=xTaskGetCurrentTaskHandle();
    s_server.quiesced=true;
    s_server.outstanding--;
    portEXIT_CRITICAL(&s_lock);
}
esp_err_t ts_ws_transport_quiesce(httpd_handle_t server, uint32_t timeout_ms)
{
    if(ts_ws_transport_in_context())return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if(s_server.handle!=server){portEXIT_CRITICAL(&s_lock);return ESP_ERR_INVALID_STATE;}
    bool publish=!s_server.barrier_requested;
    if(publish){s_server.barrier_requested=true;s_server.outstanding+=2;}
    portEXIT_CRITICAL(&s_lock);
    if(publish){
        esp_err_t ret=httpd_queue_work(server,quiesce_work,NULL);
        portENTER_CRITICAL(&s_lock);
        if(ret!=ESP_OK){s_server.barrier_requested=false;s_server.outstanding--;}
        s_server.outstanding--;
        portEXIT_CRITICAL(&s_lock);
        if(ret!=ESP_OK)return ret;
    }
    int64_t start=esp_timer_get_time();
    for(;;){
        portENTER_CRITICAL(&s_lock);bool ready=s_server.quiesced;portEXIT_CRITICAL(&s_lock);
        if(ready)return ESP_OK;
        if(esp_timer_get_time()-start >= (int64_t)timeout_ms*1000)return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}

unsigned ts_ws_peer_snapshot(ts_ws_peer_t *peers, unsigned capacity)
{
    unsigned n=0;
    portENTER_CRITICAL(&s_lock);
    if(!s_server.stopping) for(unsigned i=0;i<TS_WS_CONNECTIONS && n<capacity;i++)
        if(s_peers[i].connection) peers[n++]=s_peers[i];
    portEXIT_CRITICAL(&s_lock);
    return n;
}
static esp_err_t reserve_kind(const ts_ws_peer_t *peers, unsigned count, message_kind_t kind,
                        size_t bytes, ts_ws_reservation_t *r)
{
    if(!r || (!peers && kind!=MSG_RESULT) || !count || count>TS_WS_RESERVATION_TARGETS || !bytes || bytes>TS_WS_LEGACY_BYTES) return ESP_ERR_INVALID_ARG;
    *r=(ts_ws_reservation_t){0};
    ts_ws_message_t *m=message_alloc(bytes,kind);
    if(!m) return ESP_ERR_NO_MEM;
    unsigned begin=kind>=MSG_POWER ? TS_WS_RESULT_TX_END+(kind==MSG_POWER_TICK?TS_WS_POWER_TRANSITIONS*TS_WS_CONNECTIONS:0) : kind==MSG_RESULT?TS_WS_TX_SLOTS:0;
    unsigned end=kind==MSG_POWER_TICK?TS_WS_ALL_TX_SLOTS:kind==MSG_POWER?TS_WS_ALL_TX_SLOTS-TS_WS_CONNECTIONS:kind==MSG_RESULT?TS_WS_RESULT_TX_END:TS_WS_TOPIC_TX_SLOTS;
    portENTER_CRITICAL(&s_lock);
    bool valid=!s_server.stopping && s_server.handle;
    for(unsigned i=0;peers && i<count;i++) if(!peer_valid(peers[i])) valid=false;
    unsigned available=0;
    for(unsigned i=begin;i<end;i++) if(!s_deliveries[i].refs) available++;
    if(valid && available>=count) {
        for(unsigned i=begin,j=0;i<end && j<count;i++) if(!s_deliveries[i].refs) {
            s_deliveries[i]=(delivery_t){.refs=1,.state=TX_RESERVED,.peer=peers?peers[j]:(ts_ws_peer_t){0},.message=m};
            r->slots[j++]=i;m->refs++;s_server.outstanding++;s_tx_stats.jobs++;
        }
        r->count=count;r->message=m;
        if(s_tx_stats.jobs>s_tx_stats.jobs_high)s_tx_stats.jobs_high=s_tx_stats.jobs;
    }
    portEXIT_CRITICAL(&s_lock);
    if(!r->message){ts_ws_message_release(m);return valid?ESP_ERR_NO_MEM:ESP_ERR_INVALID_STATE;}
    return ESP_OK;
}
esp_err_t ts_ws_reserve(const ts_ws_peer_t *peers,unsigned count,bool result,size_t bytes,ts_ws_reservation_t *r)
{
    return reserve_kind(peers,count,result?MSG_RESULT:MSG_TOPIC,bytes,r);
}
void ts_ws_reservation_release(ts_ws_reservation_t *r)
{
    if(!r || !r->message)return;
    for(unsigned i=0;i<r->count;i++)if(r->slots[i]>=0) delivery_release(&s_deliveries[r->slots[i]]);
    ts_ws_message_release(r->message);
    *r=(ts_ws_reservation_t){0};
    ts_ws_subscriptions_wake();
}
bool ts_ws_reserved_json(ts_ws_reservation_t *r,const char *topic,const cJSON *data,int64_t timestamp)
{
    if(!r->message)return false;
    cJSON *root=cJSON_CreateObject();
    bool ok=root && cJSON_AddStringToObject(root,"type","data") && cJSON_AddStringToObject(root,"topic",topic) &&
        cJSON_AddNumberToObject(root,"timestamp",timestamp) && ts_ws_json_reference(root,"data",data) &&
        cJSON_PrintPreallocated(root,r->message->text,r->message->capacity,false);
    cJSON_Delete(root);
    if(ok)r->message->len=strlen(r->message->text);
    return ok;
}
esp_err_t ts_ws_reserved_text(ts_ws_reservation_t *r,const char *text)
{
    if(!r || !r->message || !text)return ESP_ERR_INVALID_ARG;
    size_t n=strlen(text);
    if(n>=r->message->capacity)return ESP_ERR_INVALID_SIZE;
    memcpy(r->message->text,text,n+1);r->message->len=n;
    return ESP_OK;
}
esp_err_t ts_ws_reserved_submit(ts_ws_reservation_t *r,unsigned index,uint64_t revision,
    uint64_t delivery,ts_ws_delivery_check_t check,ts_ws_delivery_done_t done)
{
    if(!r || !r->message || index>=r->count || r->slots[index]<0)return ESP_ERR_INVALID_ARG;
    delivery_t *d=&s_deliveries[r->slots[index]];
    portENTER_CRITICAL(&s_lock);
    bool valid=!s_server.stopping && peer_valid(d->peer) && s_delivery_order!=UINT64_MAX;
    if(valid) {
        d->revision=revision;d->delivery=delivery;d->check=check;d->done=done;
        d->order=++s_delivery_order;d->state=TX_PREPARED;d->refs++; /* work + reservation */
    }
    portEXIT_CRITICAL(&s_lock);
    if(!valid)return ESP_ERR_INVALID_STATE;
    r->slots[index]=-1;
    ts_ws_transport_flush();delivery_release(d);ts_ws_subscriptions_wake();
    return ESP_OK;
}

esp_err_t ts_ws_result_reserve(size_t bytes,ts_ws_reservation_t *r)
{
    return ts_ws_reserve(NULL,TS_WS_CONNECTIONS,true,bytes,r);
}
esp_err_t ts_ws_result_broadcast(ts_ws_reservation_t *r,const char *text)
{
    esp_err_t ret=ts_ws_reserved_text(r,text);
    if(ret!=ESP_OK)return ret;
    ts_ws_peer_t peers[TS_WS_CONNECTIONS];
    unsigned n=ts_ws_peer_snapshot(peers,TS_WS_CONNECTIONS);
    return ts_ws_result_publish(r,peers,n);
}
esp_err_t ts_ws_result_publish(ts_ws_reservation_t *r,const ts_ws_peer_t *peers,unsigned n)
{
    esp_err_t ret=ESP_OK;
    for(unsigned i=0;i<n;i++) {
        if(i>=r->count || r->slots[i]<0)return ESP_ERR_INVALID_STATE;
        portENTER_CRITICAL(&s_lock);s_deliveries[r->slots[i]].peer=peers[i];portEXIT_CRITICAL(&s_lock);
        esp_err_t result=ts_ws_reserved_submit(r,i,0,0,NULL,NULL);
        if(result!=ESP_OK)ret=result;
    }
    return ret;
}

uint32_t ts_ws_transport_retry_ms(void)
{
    portENTER_CRITICAL(&s_lock);
    int64_t delta=s_server.retry_at-esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    return delta>0 ? (uint32_t)((delta+999)/1000) : 0;
}

esp_err_t ts_ws_power_reserve(bool tick,ts_ws_reservation_t *r)
{
    *r=(ts_ws_reservation_t){0};
    ts_ws_peer_t peers[TS_WS_CONNECTIONS];
    unsigned n=ts_ws_peer_snapshot(peers,TS_WS_CONNECTIONS);
    if(!n)return ESP_OK;
    esp_err_t ret=reserve_kind(peers,n,tick?MSG_POWER_TICK:MSG_POWER,TS_WS_POWER_BYTES,r);
    portENTER_CRITICAL(&s_lock);
    if(ret==ESP_OK)s_tx_stats.power_accepted++;else s_tx_stats.power_rejected++;
    portEXIT_CRITICAL(&s_lock);
    return ret;
}
esp_err_t ts_ws_power_publish(ts_ws_reservation_t *r,const char *text)
{
    if(!r->message)return ESP_OK; /* no recipients at admission */
    esp_err_t ret=ts_ws_reserved_text(r,text);
    if(ret!=ESP_OK)return ret;
    for(unsigned i=0;i<r->count;i++) {
        esp_err_t result=ts_ws_reserved_submit(r,i,0,0,NULL,NULL);
        if(result!=ESP_OK){ret=result;portENTER_CRITICAL(&s_lock);s_tx_stats.power_failed++;portEXIT_CRITICAL(&s_lock);}
    }
    return ret;
}

esp_err_t ts_ws_reserved_target(ts_ws_reservation_t *r,unsigned index,ts_ws_peer_t peer,
    uint64_t revision,uint64_t delivery,ts_ws_delivery_done_t done)
{
    if(!r || index>=r->count || r->slots[index]<0)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);s_deliveries[r->slots[index]].peer=peer;portEXIT_CRITICAL(&s_lock);
    return ts_ws_reserved_submit(r,index,revision,delivery,NULL,done);
}
