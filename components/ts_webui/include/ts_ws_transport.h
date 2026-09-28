#pragma once
/* Internal bounded delivery path. Session mutation and send execute in HTTPD. */
#include "esp_http_server.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#define TS_WS_TOPIC_TX_SLOTS 32
#define TS_WS_TX_SLOTS (TS_WS_TOPIC_TX_SLOTS + TS_WS_CONNECTIONS)
#define TS_WS_MESSAGE_SLOTS 8
#define TS_WS_POWER_TRANSITIONS 4
#define TS_WS_POWER_SLOTS (TS_WS_POWER_TRANSITIONS + 1)
#define TS_WS_POWER_BYTES 1024
#define TS_WS_POWER_BUDGET (TS_WS_POWER_SLOTS * TS_WS_POWER_BYTES)
#define TS_WS_QUEUE_RETRIES 300 /* 100 ms backoff, bounded SDK admission retries */
#define TS_WS_ALL_MESSAGE_SLOTS (TS_WS_MESSAGE_SLOTS + 2 + TS_WS_POWER_SLOTS)
#define TS_WS_RESULT_TX_END (TS_WS_TX_SLOTS + 2 * TS_WS_CONNECTIONS)
#define TS_WS_ALL_TX_SLOTS (TS_WS_RESULT_TX_END + TS_WS_POWER_SLOTS * TS_WS_CONNECTIONS)
#define TS_WS_FRAME_BYTES 32768
#define TS_WS_LEGACY_BYTES (512 * 1024)
#define TS_WS_TOTAL_BYTES (1024 * 1024)
#ifdef CONFIG_TS_WEBUI_WS_MAX_CLIENTS
#define TS_WS_CONNECTIONS CONFIG_TS_WEBUI_WS_MAX_CLIENTS
#else
#define TS_WS_CONNECTIONS 8
#endif

typedef struct {
    httpd_handle_t server;
    uint64_t epoch, connection;
    int fd;
} ts_ws_peer_t;
typedef struct ts_ws_message ts_ws_message_t;
typedef bool (*ts_ws_delivery_check_t)(uint64_t revision, uint64_t delivery);
typedef void (*ts_ws_delivery_done_t)(uint64_t revision, uint64_t delivery, esp_err_t result);
typedef void (*ts_ws_closed_t)(ts_ws_peer_t peer);
esp_err_t ts_ws_transport_start(httpd_handle_t server, ts_ws_closed_t closed);
esp_err_t ts_ws_transport_stop(httpd_handle_t server, uint32_t timeout_ms);
bool ts_ws_transport_in_context(void);
esp_err_t ts_ws_peer_open(httpd_req_t *req, ts_ws_peer_t *peer);
/* HTTPD owner only. Values, not bare fd, cross task boundaries. */
void ts_ws_peer_close(ts_ws_peer_t peer);
bool ts_ws_peer_get(httpd_handle_t server, int fd, ts_ws_peer_t *peer);
bool ts_ws_peer_equal(ts_ws_peer_t a, ts_ws_peer_t b);
unsigned ts_ws_transport_capacity(void);
ts_ws_message_t *ts_ws_message_json(const char *topic, const cJSON *data, int64_t timestamp);
ts_ws_message_t *ts_ws_message_text(const char *text, size_t len);
void ts_ws_message_release(ts_ws_message_t *message);
/* ESP_OK means local acceptance. Each accepted target settles once via done,
 * including later queue failure, stale/canceled target and socket failure.
 * A synchronous rejection never calls done. Message origin selects its pool. */
esp_err_t ts_ws_transport_submit(ts_ws_peer_t peer, ts_ws_message_t *message,
    uint64_t revision, uint64_t delivery, ts_ws_delivery_check_t check, ts_ws_delivery_done_t done);
/* Compatibility bridge for existing global/stream senders; never coalesces. */
esp_err_t ts_ws_transport_send(httpd_handle_t server, int fd, const httpd_ws_frame_t *frame);

void ts_ws_transport_stopped(httpd_handle_t server);
esp_err_t ts_ws_transport_broadcast(const httpd_ws_frame_t *frame);

void ts_ws_peer_log_level(ts_ws_peer_t peer, int level);
esp_err_t ts_ws_transport_log(const httpd_ws_frame_t *frame, int level);
unsigned ts_ws_message_capacity(void);

/* cJSON 1.7's AddItemReferenceToObject loses its newly allocated reference if
 * key duplication fails. Own that reference explicitly; keys here are static. */
static inline bool ts_ws_json_reference(cJSON *object, const char *key, const cJSON *data)
{
    if (!object || !data) return false;
    cJSON *ref = cJSON_CreateNull();
    if (!ref) return false;
    *ref = *data;
    ref->next = ref->prev = NULL;
    ref->string = NULL;
    ref->type = (ref->type & ~cJSON_StringIsConst) | cJSON_IsReference;
    if (cJSON_AddItemToObjectCS(object, key, ref)) return true;
    cJSON_Delete(ref);
    return false;
}

typedef struct {
    uint64_t queued, attempts, success, failed, stale, rejected;
    uint64_t settled, power_accepted, power_rejected, power_settled, power_failed;
    uint32_t bytes, bytes_high, jobs, jobs_high;
    int64_t max_send_us;
} ts_ws_transport_stats_t;
void ts_ws_transport_get_stats(ts_ws_transport_stats_t *stats);

/* Deterministic host barriers; compiled out in firmware. */
#ifndef TS_WS_TEST_POINT
#define TS_WS_TEST_POINT(name) ((void)0)
#endif

esp_err_t ts_ws_transport_quiesce(httpd_handle_t server, uint32_t timeout_ms);

void ts_ws_transport_flush(void);
bool ts_ws_transport_needs_flush(void);
void ts_ws_transport_cancel_topics(void);

/* A bounded reservation owns actual payload bytes and destination descriptors.
 * No API collection / accepted result producer may start before it succeeds. */
#define TS_WS_RESERVATION_TARGETS ((TS_WS_CONNECTIONS > TS_WS_TOPIC_TX_SLOTS) ? TS_WS_CONNECTIONS : TS_WS_TOPIC_TX_SLOTS)
typedef struct {
    ts_ws_message_t *message;
    unsigned count;
    int slots[TS_WS_RESERVATION_TARGETS];
} ts_ws_reservation_t;
esp_err_t ts_ws_reserve(const ts_ws_peer_t *peers, unsigned count, bool result,
                        size_t bytes, ts_ws_reservation_t *reservation);
bool ts_ws_reserved_json(ts_ws_reservation_t *r, const char *topic, const cJSON *data, int64_t timestamp);
esp_err_t ts_ws_reserved_text(ts_ws_reservation_t *r, const char *text);
esp_err_t ts_ws_reserved_submit(ts_ws_reservation_t *r, unsigned index,
    uint64_t revision, uint64_t delivery, ts_ws_delivery_check_t check, ts_ws_delivery_done_t done);
void ts_ws_reservation_release(ts_ws_reservation_t *r);
unsigned ts_ws_peer_snapshot(ts_ws_peer_t *peers, unsigned capacity);

/* Preserve global result recipients at completion, with capacity owned at admission. */
esp_err_t ts_ws_result_reserve(size_t bytes, ts_ws_reservation_t *r);
esp_err_t ts_ws_result_broadcast(ts_ws_reservation_t *r, const char *text);

uint32_t ts_ws_transport_retry_ms(void);

/* Fixed power payloads and exclusive descriptors/byte budget. Tick traffic has
 * its own slot; neither ticks nor ordinary logs consume transition capacity.
 * Snapshot recipients at admission; no coalescing or re-broadcast on retry. */
esp_err_t ts_ws_power_reserve(bool tick, ts_ws_reservation_t *r);
esp_err_t ts_ws_power_publish(ts_ws_reservation_t *r, const char *text);

esp_err_t ts_ws_result_publish(ts_ws_reservation_t *r,const ts_ws_peer_t *peers,unsigned count);

/* Bind an original terminal recipient before transferring its reserved target. */
esp_err_t ts_ws_reserved_target(ts_ws_reservation_t *r, unsigned index, ts_ws_peer_t peer,
    uint64_t revision, uint64_t delivery, ts_ws_delivery_done_t done);
