/**
 * @file ts_webui_ws.c
 * @brief WebSocket Implementation with Terminal Support (including SSH Shell)
 */

#include "ts_webui.h"
#include "ts_http_server.h"
#include "ts_log.h"
#include "ts_event.h"
#include "ts_console.h"
#ifdef CONFIG_TS_WEBUI_AUTH_REQUIRED
#include "ts_security.h"
#endif
#include "ts_power_policy.h"
#include "ts_ssh_client.h"
#include "ts_ssh_shell.h"
#include "ts_keystore.h"
#include "ts_ssh_service.h"
#include "ts_automation_config.h"
#include "ts_ws_subscriptions.h"
#include "ts_ws_transport.h"
#include "ts_ws_operation.h"
#include "freertos/timers.h"
#include <stdatomic.h>
#include <assert.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "ts_exec_output.h"
// ts_var.h 已废弃，统一使用 ts_variable.h（ts_automation 变量系统）
#include "ts_variable.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <errno.h>
#include <math.h>

#define TAG "webui_ws"

#ifdef CONFIG_TS_WEBUI_WS_MAX_CLIENTS
#define MAX_WS_CLIENTS CONFIG_TS_WEBUI_WS_MAX_CLIENTS
#else
#define MAX_WS_CLIENTS 8
#endif

/* 终端输出缓冲区大小（可由 Kconfig 配置） */
#ifdef CONFIG_TS_WEBUI_TERMINAL_OUTPUT_BUF_SIZE
#define TERMINAL_OUTPUT_BUF_SIZE CONFIG_TS_WEBUI_TERMINAL_OUTPUT_BUF_SIZE
#else
#define TERMINAL_OUTPUT_BUF_SIZE 32768
#endif

/* SSH Shell 输出缓冲区大小 */
#define SSH_OUTPUT_BUF_SIZE 2048
#define SSH_POLL_STACK_SIZE 8192

typedef enum {
    WS_CLIENT_TYPE_EVENT,      // 普通事件订阅客户端
    WS_CLIENT_TYPE_TERMINAL,   // 终端会话客户端
    WS_CLIENT_TYPE_SSH_SHELL,  // SSH Shell 会话客户端
    WS_CLIENT_TYPE_LOG         // 日志订阅客户端
} ws_client_type_t;

typedef struct {
    atomic_bool active;
    ts_ws_peer_t peer; /* HTTPD owner writes the connection registry */
    atomic_int fd;
    _Atomic(httpd_handle_t) hd;
    _Atomic(ws_client_type_t) type;
    _Atomic(ts_log_level_t) log_min_level;  // 日志客户端的最小级别过滤
} ws_client_t;

static ws_client_t s_clients[MAX_WS_CLIENTS];
static _Atomic(httpd_handle_t) s_server = NULL;
static atomic_bool s_ws_stopping;
static SemaphoreHandle_t s_terminal_mutex = NULL;
static int s_terminal_client_fd = -1;  // 当前终端会话的 fd

/* 终端输出缓冲区 */
static char *s_terminal_output_buf = NULL;
static size_t s_terminal_output_len = 0;
static SemaphoreHandle_t s_output_mutex = NULL;

/* Business resources belong to the operation held by task args and borrowers. */
typedef struct {
    ts_ws_operation_t *op;
    ts_ssh_session_t session;
    ts_ssh_shell_t shell;
    SemaphoreHandle_t io;
    atomic_bool disconnect_requested;
} ssh_shell_context_t;

/* 电压保护事件处理器句柄 */
static ts_event_handler_handle_t s_power_event_handle = NULL;

/* 日志回调句柄 */
static ts_log_callback_handle_t s_log_callback_handle = NULL;
static atomic_bool s_log_streaming_enabled = false;

/* 日志级别名称映射 */
static const char *s_level_names[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};

/* 前向声明 */
static void exec_retry_timer_drain(void);
static void update_log_stream_state(void);
esp_err_t ts_webui_log_stream_enable(bool enable);
esp_err_t ts_webui_ws_stop(httpd_handle_t server);
static void exec_cache_clear(void);
void ts_webui_ws_stopped(httpd_handle_t server);
static bool has_log_clients(void);

/* HTTPD owner updates role and log generation together. Old queued logs cannot
 * regain eligibility after a later subscribe on the same connection. */
static void set_client_role(ts_ws_peer_t peer, ws_client_type_t type, ts_log_level_t level)
{
    ts_ws_peer_t current;
    if (!ts_ws_peer_get(peer.server, peer.fd, &current) || !ts_ws_peer_equal(peer,current)) return;
    for (int i=0;i<MAX_WS_CLIENTS;i++) if(s_clients[i].active && s_clients[i].fd==peer.fd) {
        s_clients[i].type=type;
        s_clients[i].log_min_level=level;
        ts_ws_peer_log_level(peer,type==WS_CLIENT_TYPE_LOG ? (int)level : -1);
        break;
    }
    update_log_stream_state();
}

/* 电压保护状态字符串 */
static const char *power_state_to_string(ts_power_policy_state_t state)
{
    switch (state) {
        case TS_POWER_POLICY_STATE_NORMAL:      return "NORMAL";
        case TS_POWER_POLICY_STATE_LOW_VOLTAGE: return "LOW_VOLTAGE";
        case TS_POWER_POLICY_STATE_SHUTDOWN:    return "SHUTDOWN";
        case TS_POWER_POLICY_STATE_PROTECTED:   return "PROTECTED";
        case TS_POWER_POLICY_STATE_RECOVERY:    return "RECOVERY";
        default: return "UNKNOWN";
    }
}

/* 电压保护事件处理回调 */
static void power_policy_event_handler(const ts_event_t *event, void *user_data)
{
    if (!event || !event->data) return;
    
    ts_power_policy_status_t *status = (ts_power_policy_status_t *)event->data;
    
    bool tick=event->id==TS_POWER_POLICY_EVENT_COUNTDOWN_TICK || event->id==TS_POWER_POLICY_EVENT_DEBUG_TICK;
    ts_ws_reservation_t reservation;
    esp_err_t ret=ts_ws_power_reserve(tick,&reservation);
    if(ret!=ESP_OK) {
        TS_LOGW(TAG,"Power notification admission failed: event=%ld result=%d",(long)event->id,ret);
        return; /* bounded overload; never wait on protection's event callback */
    }
    if(!reservation.message)return;
    const char *event_name="unknown";
    switch(event->id) {
        case TS_POWER_POLICY_EVENT_STATE_CHANGED:event_name="state_changed";break;
        case TS_POWER_POLICY_EVENT_LOW_VOLTAGE:event_name="low_voltage";break;
        case TS_POWER_POLICY_EVENT_COUNTDOWN_TICK:event_name="countdown_tick";break;
        case TS_POWER_POLICY_EVENT_SHUTDOWN_START:event_name="shutdown_start";break;
        case TS_POWER_POLICY_EVENT_PROTECTED:event_name="protected";break;
        case TS_POWER_POLICY_EVENT_RECOVERY_START:event_name="recovery_start";break;
        case TS_POWER_POLICY_EVENT_RECOVERY_COMPLETE:event_name="recovery_complete";break;
        case TS_POWER_POLICY_EVENT_DEBUG_TICK:event_name="debug_tick";break;
    }
    /* Fixed schema, numeric values and constant strings; no heap JSON needed.
     * Preserve cJSON's non-finite-number encoding as null. */
    char voltage[32],json[512];
    if(isfinite(status->current_voltage))snprintf(voltage,sizeof(voltage),"%.17g",(double)status->current_voltage);
    else strcpy(voltage,"null");
    int n=snprintf(json,sizeof(json),"{\"type\":\"power_event\",\"state\":\"%s\",\"voltage\":%s,\"countdown\":%lu,\"protection_count\":%lu,\"event\":\"%s\"}",
        power_state_to_string(status->state),voltage,(unsigned long)status->countdown_remaining_sec,(unsigned long)status->protection_count,event_name);
    ret=n<0 || n>=(int)sizeof(json)?ESP_ERR_INVALID_SIZE:ts_ws_power_publish(&reservation,json);
    ts_ws_reservation_release(&reservation);
    if(ret!=ESP_OK)TS_LOGW(TAG,"Power notification submission failed: event=%ld result=%d",(long)event->id,ret);

}

/*===========================================================================*/
/*                          SSH Shell Functions                               */
/*===========================================================================*/

/* Request errors must go to the requester, never mutate an existing session. */
static void ssh_request_error(httpd_req_t *req, const char *message)
{
    TS_WS_TEST_POINT("request_error_encode");
    cJSON *msg=cJSON_CreateObject();
    bool ok=msg && cJSON_AddStringToObject(msg,"type","error") && cJSON_AddStringToObject(msg,"message",message);
    char *json=ok?cJSON_PrintUnformatted(msg):NULL;cJSON_Delete(msg);
    const char *text=json?json:"{\"type\":\"error\",\"message\":\"SSH request rejected\"}";
    httpd_ws_frame_t frame={.type=HTTPD_WS_TYPE_TEXT,.payload=(uint8_t*)text,.len=strlen(text)};
    esp_err_t ret=httpd_ws_send_frame(req,&frame);
    if(ret!=ESP_OK)TS_LOGW(TAG,"SSH request error could not be sent: %s",esp_err_to_name(ret));
    free(json);
}

static bool bind_request_session(httpd_req_t *req, const cJSON *msg, ts_perm_level_t required)
{
#ifdef CONFIG_TS_WEBUI_AUTH_REQUIRED
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(msg, "token");
    char authorization[136];
    uint32_t session_id = 0;
    ts_perm_level_t level = TS_PERM_NONE;
    int n = cJSON_IsString(token) ? snprintf(authorization, sizeof(authorization), "Bearer %s", token->valuestring) : -1;
    if (n < 0 || n >= sizeof(authorization) ||
        ts_auth_validate_request(authorization, &session_id, &level) != ESP_OK || level < required)
        return false;
    ts_ws_peer_t peer = *(ts_ws_peer_t *)req->sess_ctx;
    return ts_ws_peer_bind_session(peer, session_id) == ESP_OK && ts_ws_peer_authorized(peer, required);
#else
    (void)req;
    (void)msg;
    (void)required;
    return true;
#endif
}
static void handle_auth(httpd_req_t *req, const cJSON *msg)
{
    const char *text = bind_request_session(req, msg, TS_PERM_READ) ?
        "{\"type\":\"auth_ok\"}" : "{\"type\":\"auth_error\",\"message\":\"Unauthorized\"}";
    httpd_ws_frame_t frame = {.type=HTTPD_WS_TYPE_TEXT, .payload=(uint8_t *)text, .len=strlen(text)};
    httpd_ws_send_frame(req, &frame);
}

static void shell_resources_close(ssh_shell_context_t *ctx)
{
    xSemaphoreTake(ctx->io,portMAX_DELAY);
    if(ctx->shell){ts_ssh_shell_close(ctx->shell);ctx->shell=NULL;}
    if(ctx->session){ts_ssh_disconnect(ctx->session);ts_ssh_session_destroy(ctx->session);ctx->session=NULL;}
    xSemaphoreGive(ctx->io);
}
static void shell_destroy(void *data)
{
    ssh_shell_context_t *ctx=data;
    if(!ctx)return;
    /* Network cleanup belongs to the creator/poller, never the TX worker. */
    assert(!ctx->session && !ctx->shell);
    vSemaphoreDelete(ctx->io);free(ctx);
}
static void ssh_close(ts_ws_operation_t *op,const char *status,const char *message)
{
    if(!ts_ws_op_close_begin(op))return;
    cJSON *msg=cJSON_CreateObject();
    bool ok=msg && cJSON_AddStringToObject(msg,"type","ssh_status") &&
        cJSON_AddStringToObject(msg,"status",status) && cJSON_AddStringToObject(msg,"message",message);
    char *json=ok?cJSON_PrintUnformatted(msg):NULL;cJSON_Delete(msg);
    ts_ws_op_close_finish(op,json);free(json);
}
static void ssh_send_output(ssh_shell_context_t *ctx,const char *data,size_t len)
{
    if(!data || !len)return;
    ts_ws_output_ticket_t ticket;
    if(!ts_ws_op_output_begin(ctx->op,&ticket))return;
    char *buf=NULL,*json=NULL;cJSON *msg=NULL;
    if(ticket.count) {
        buf=heap_caps_malloc(len+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!buf)buf=malloc(len+1);
        if(buf){memcpy(buf,data,len);buf[len]=0;msg=cJSON_CreateObject();
            if(msg && cJSON_AddStringToObject(msg,"type","ssh_output") && cJSON_AddStringToObject(msg,"data",buf))json=cJSON_PrintUnformatted(msg);}
    }
    free(buf);cJSON_Delete(msg);ts_ws_op_output_finish(&ticket,json);free(json);
}
/* Startup controls are synchronous on the HTTPD owner. Creator + poller refs
 * pin STARTING; OPEN is published only after connected has returned successfully.
 * They cannot be overtaken by ordinary output or an asynchronous close callback. */
static esp_err_t ssh_start_status(ssh_shell_context_t *ctx,const char *status,const char *message)
{
    if(!ts_ws_op_starting(ctx->op))return ESP_ERR_INVALID_STATE;
    cJSON *msg=cJSON_CreateObject();
    bool ok=msg && cJSON_AddStringToObject(msg,"type","ssh_status") &&
        cJSON_AddStringToObject(msg,"status",status) && cJSON_AddStringToObject(msg,"message",message);
    char *json=ok?cJSON_PrintUnformatted(msg):NULL;cJSON_Delete(msg);
    esp_err_t ret=ESP_ERR_NO_MEM;
    if(json){ts_ws_peer_t peer=ts_ws_op_peer(ctx->op);httpd_ws_frame_t f={.type=HTTPD_WS_TYPE_TEXT,.payload=(uint8_t*)json,.len=strlen(json)};
        ret=ts_ws_transport_send_private(peer,&f,TS_PERM_ROOT);}
    free(json);return ret==ESP_ERR_NOT_ALLOWED ? ESP_OK : ret;
}
/* Poller remains the only ordinary observation producer. User disconnect is
 * handed to that producer, preserving the disconnecting/closed wire messages. */
static void ssh_disconnecting(ssh_shell_context_t *ctx)
{
    ts_ws_output_ticket_t ticket;
    if(ts_ws_op_output_begin(ctx->op,&ticket))
        ts_ws_op_output_finish(&ticket,"{\"type\":\"ssh_status\",\"status\":\"disconnecting\",\"message\":\"Closing SSH session...\"}");
    ssh_close(ctx->op,"closed","SSH session closed");
}
static void ssh_poll_task(void *arg)
{
    ssh_shell_context_t *ctx=arg;ts_ws_operation_t *op=ctx->op;
    char buf[SSH_OUTPUT_BUF_SIZE];
    while(ts_ws_op_starting(op))vTaskDelay(1);
    while(ts_ws_op_is_open(op)) {
        if(ctx->disconnect_requested){ssh_disconnecting(ctx);break;}
        size_t n=0;
        xSemaphoreTake(ctx->io,portMAX_DELAY);
        esp_err_t ret=ts_ssh_shell_read(ctx->shell,buf,sizeof(buf)-1,&n);
        bool active=ts_ssh_shell_is_active(ctx->shell);
        bool failed=ts_ssh_shell_get_state(ctx->shell)==TS_SHELL_STATE_ERROR;
        xSemaphoreGive(ctx->io);
        if(ret==ESP_OK && n)ssh_send_output(ctx,buf,n);
        if(!active){ssh_close(op,failed?"error":"closed",failed?"SSH channel failed; remote result is not confirmed":"SSH session closed");break;}
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    shell_resources_close(ctx);
    ts_ws_op_executor_done(op);
    vTaskDeleteWithCaps(NULL);
}
static void ssh_cleanup(void)
{
    ts_ws_operation_t *op=ts_ws_op_shell_owner();
    if(!op)return;
    ssh_close(op,"closed","SSH session closed");
    ts_ws_op_release(op);
}
static void handle_ssh_connect(httpd_req_t *req,cJSON *params)
{
    if (!bind_request_session(req, params, TS_PERM_ROOT)) {
        ssh_request_error(req, "Unauthorized");
        return;
    }

    cJSON *host=cJSON_GetObjectItem(params,"host"),*user=cJSON_GetObjectItem(params,"user");
    cJSON *port=cJSON_GetObjectItem(params,"port"),*password=cJSON_GetObjectItem(params,"password");
    if(!cJSON_IsString(host) || !cJSON_IsString(user)){ssh_request_error(req,"Missing host or user");return;}
    ts_ws_peer_t peer=*(ts_ws_peer_t*)req->sess_ctx;
    esp_err_t admission;
    ts_ws_operation_t *op=ts_ws_op_create(TS_WS_OP_SHELL,peer,&admission);
    if(!op){ssh_request_error(req,admission==ESP_ERR_NO_MEM?"SSH result delivery capacity unavailable":"Another SSH session is active");return;}
    ssh_shell_context_t *ctx=calloc(1,sizeof(*ctx));
    if(!ctx || !(ctx->io=xSemaphoreCreateMutex())) {
        free(ctx);ts_ws_op_discard(op);ts_ws_op_executor_done(op);ssh_request_error(req,"Failed to create SSH session");return;
    }
    ctx->op=op;ts_ws_op_bind(op,ctx,shell_destroy);
    set_client_role(peer,WS_CLIENT_TYPE_SSH_SHELL,TS_LOG_NONE);
    const char *error="SSH connection status delivery failed";bool poller=false;
    if(ssh_start_status(ctx,"connecting","Connecting to SSH server...")!=ESP_OK)goto failed;
    ts_ssh_config_t config=TS_SSH_DEFAULT_CONFIG();
    config.host=host->valuestring;config.username=user->valuestring;
    config.port=cJSON_IsNumber(port)?port->valueint:22;
    config.auth_method=TS_SSH_AUTH_PASSWORD;config.auth.password=cJSON_IsString(password)?password->valuestring:"";config.timeout_ms=10000;
    error="Failed to create SSH session";
    if(ts_ssh_session_create(&config,&ctx->session)!=ESP_OK)goto failed;
    if(ts_ssh_connect(ctx->session)!=ESP_OK){error=ts_ssh_get_error(ctx->session);goto failed;}
    ts_shell_config_t shell_config=TS_SHELL_DEFAULT_CONFIG();shell_config.term_width=80;shell_config.term_height=24;shell_config.read_timeout_ms=50;
    error="Failed to open shell";
    if(ts_ssh_shell_open(ctx->session,&shell_config,&ctx->shell)!=ESP_OK)goto failed;
    ts_ws_op_executor_take(op);
    if(xTaskCreateWithCaps(ssh_poll_task,"ssh_poll",SSH_POLL_STACK_SIZE,ctx,5,NULL,MALLOC_CAP_SPIRAM)!=pdPASS){ts_ws_op_executor_done(op);error="Failed to create SSH session";goto failed;}
    poller=true;error="SSH shell closed during startup";
    if(!ts_ssh_shell_is_active(ctx->shell))goto failed;
    error="SSH readiness delivery failed";
    if(ssh_start_status(ctx,"connected","SSH shell ready")!=ESP_OK)goto failed;
    ts_ws_op_open(op);ts_ws_op_executor_done(op);return;
failed:
    ssh_close(op,"error",error?error:"SSH connection failed");
    if(!poller)shell_resources_close(ctx);
    set_client_role(peer,WS_CLIENT_TYPE_TERMINAL,TS_LOG_NONE);
    ts_ws_op_executor_done(op);
}
/* Page control has one ownership gate. The acquired original context remains
 * pinned across resource I/O; no handler can retarget the current Shell. */
static void handle_ssh_control(httpd_req_t *req,ts_ws_peer_t peer,const char *type,cJSON *msg)
{
    if (!ts_ws_peer_authorized(peer, TS_PERM_ROOT)) {
        ssh_request_error(req, "Unauthorized");
        return;
    }

    ts_ws_operation_t *op=ts_ws_op_shell_control(peer);
    if(!op){ssh_request_error(req,"This connection does not own an active SSH session");return;}
    ssh_shell_context_t *ctx=ts_ws_op_data(op);
    if(!strcmp(type,"ssh_disconnect"))ctx->disconnect_requested=true;
    else {
        esp_err_t ret=ESP_ERR_INVALID_ARG;bool active=false;
        xSemaphoreTake(ctx->io,portMAX_DELAY);
        if(ctx->shell && ts_ws_op_is_open(op)) {
            active=true;
            if(!strcmp(type,"ssh_input")) {
                cJSON *data=cJSON_GetObjectItem(msg,"data");
                if(cJSON_IsString(data))ret=ts_ssh_shell_write(ctx->shell,data->valuestring,strlen(data->valuestring),NULL);
            } else if(!strcmp(type,"ssh_signal")) {
                cJSON *signal=cJSON_GetObjectItem(msg,"signal");
                if(cJSON_IsString(signal))ret=ts_ssh_shell_send_signal(ctx->shell,signal->valuestring);
            } else {
                cJSON *w=cJSON_GetObjectItem(msg,"width"),*h=cJSON_GetObjectItem(msg,"height");
                if(cJSON_IsNumber(w) && cJSON_IsNumber(h) && w->valuedouble>=1 && w->valuedouble<=UINT16_MAX && h->valuedouble>=1 && h->valuedouble<=UINT16_MAX &&
                   w->valuedouble==w->valueint && h->valuedouble==h->valueint)
                    ret=ts_ssh_shell_resize(ctx->shell,w->valueint,h->valueint);
            }
        }
        if(ctx->shell)active=ts_ssh_shell_is_active(ctx->shell);
        xSemaphoreGive(ctx->io);
        if(ret!=ESP_OK){
            if(!active)ssh_close(op,"error","SSH channel failed; remote result is not confirmed");
            else ssh_request_error(req,ret==ESP_ERR_NOT_SUPPORTED?"Unsupported SSH signal":
                ret==ESP_ERR_INVALID_ARG?"Invalid SSH control parameters":"SSH control request failed; input was not replayed");
        }
    }
    ts_ws_op_release(op);
}

/*===========================================================================*/
/*                          Terminal Functions                                */
/*===========================================================================*/

typedef struct {
    ts_ws_operation_t *op;
    ts_ws_peer_t peer;
    char *command;
    atomic_bool cancel, detached, finished, truncated;
    int code;
    int64_t retry_deadline;
} terminal_job_t;

static bool terminal_cancelled(void *data) {
    terminal_job_t *job=data;
#ifdef CONFIG_TS_WEBUI_AUTH_REQUIRED
    if(!ts_ws_peer_authorized(job->peer,TS_PERM_ROOT))job->cancel=true;
#endif
    return job->cancel;
}
static void terminal_output_cb(const char *data, size_t len, void *user_data)
{
    terminal_job_t *job = user_data;
    if (!data || !len || job->detached) return;
    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    size_t space = TERMINAL_OUTPUT_BUF_SIZE - s_terminal_output_len - 1;
    size_t count = len < space ? len : space;
    memcpy(s_terminal_output_buf + s_terminal_output_len, data, count);
    s_terminal_output_len += count;
    s_terminal_output_buf[s_terminal_output_len] = 0;
    if (count != len) job->truncated = true;
    xSemaphoreGive(s_output_mutex);
    ts_ws_subscriptions_wake();
}

/* Do not split a valid UTF-8 character at a JSON/WebSocket frame boundary. */
static size_t terminal_utf8_prefix(const char *data, size_t n)
{
    if (!n) return 0;
    size_t start = n - 1;
    while (start && (((unsigned char)data[start] & 0xc0) == 0x80)) --start;
    unsigned char lead = data[start];
    size_t width = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    return width > n - start ? start : n;
}
static void terminal_job_destroy(void *data)
{
    terminal_job_t *job = data;
    assert(job->finished && !job->command);
    free(job);
}
static bool terminal_job_maintain(void *data)
{
    terminal_job_t *job = data;
    bool pending;
    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    if (job->detached) s_terminal_output_len = 0;
    size_t n = s_terminal_output_len < 4096 ? s_terminal_output_len : 4096;
    n = terminal_utf8_prefix(s_terminal_output_buf, n);
    if (!n && s_terminal_output_len && job->finished) {
        s_terminal_output_len = 0; job->truncated = true;
    }
    if (n) {
        char saved = s_terminal_output_buf[n]; s_terminal_output_buf[n] = 0;
        cJSON *msg = cJSON_CreateObject();
        bool ok = msg && cJSON_AddStringToObject(msg,"type","output") &&
            cJSON_AddStringToObject(msg,"data",s_terminal_output_buf);
        s_terminal_output_buf[n] = saved;
        char *json = ok ? cJSON_PrintUnformatted(msg) : NULL; cJSON_Delete(msg);
        ts_ws_output_ticket_t ticket;
        bool accepted = false;
        if (json && ts_ws_op_output_begin(job->op,&ticket)) accepted = ts_ws_op_output_try(&ticket,json);
        free(json);
        if (accepted) {
            memmove(s_terminal_output_buf,s_terminal_output_buf+n,s_terminal_output_len-n);
            s_terminal_output_len -= n; job->retry_deadline = 0;
        } else {
            if (!job->retry_deadline) job->retry_deadline = esp_timer_get_time()+30000000;
            if (esp_timer_get_time() >= job->retry_deadline) { s_terminal_output_len=0; job->truncated=true; }
        }
    }
    pending = s_terminal_output_len != 0;
    xSemaphoreGive(s_output_mutex);
    if (!pending && job->finished && ts_ws_op_close_begin(job->op)) {
        char done[128]; ts_ws_op_stats_t stats; ts_ws_op_stats(TS_WS_OP_TERMINAL,&stats);
        snprintf(done,sizeof(done),"{\"type\":\"done\",\"code\":%d,\"output_incomplete\":%s,\"stopped\":%s}",
            job->code, job->truncated || stats.failed ? "true":"false",job->cancel?"true":"false");
        ts_ws_op_close_finish(job->op,done);
    }
    return pending;
}
static void terminal_job_task(void *data)
{
    terminal_job_t *job = data;
    ts_console_exec_context_t context = {.remote=true,.cancelled=terminal_cancelled,
        .output=terminal_output_cb,.user_data=job};
    ts_cmd_result_t result = {.code=-1};
    esp_err_t ret = ts_console_exec_with_context(job->command,&result,&context);
    if (ret == ESP_ERR_INVALID_STATE) { const char busy[]="Console is busy. Retry after the current command ends.\n";terminal_output_cb(busy,sizeof(busy)-1,job); }
    job->code = ret == ESP_OK ? result.code : -1;
    memset(job->command,0,strlen(job->command));free(job->command);job->command=NULL;
    ts_ws_op_executor_done(job->op);
    job->finished = true; /* release: all producer access precedes retirement */
    ts_ws_subscriptions_wake();
    vTaskDelete(NULL);
}
static bool terminal_job_cancel(ts_ws_peer_t peer, bool detach)
{
    ts_ws_operation_t *op = ts_ws_op_acquire(TS_WS_OP_TERMINAL,0);
    if (!op) return false;
    terminal_job_t *job = ts_ws_op_data(op);
    bool matches = job && ts_ws_peer_equal(job->peer,peer);
    if (matches) { job->cancel=true; if(detach)job->detached=true; }
    ts_ws_op_release(op); return matches;
}

/* 前向声明 */
static void cleanup_disconnected_client(ts_ws_peer_t peer);

static esp_err_t add_client(httpd_req_t *req, ws_client_type_t type)
{
    ts_ws_peer_t peer;
    esp_err_t ret = ts_ws_peer_open(req, &peer);
    if (ret != ESP_OK) return ret;
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) if (!s_clients[i].active) {
        s_clients[i].peer = peer;
        s_clients[i].fd = peer.fd;
        s_clients[i].hd = peer.server;
        s_clients[i].type = type;
        s_clients[i].log_min_level = TS_LOG_NONE;
        s_clients[i].active = true;
        return ESP_OK;
    }
    ts_ws_peer_close(peer);
    return ESP_ERR_NO_MEM;
}

/* 处理终端命令执行 */
static void handle_terminal_command(httpd_req_t *req, const char *command)
{
    int fd = httpd_req_to_sockfd(req);
    
    // 检查是否是当前终端会话
    if (s_terminal_client_fd != fd) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "type", "error");
        cJSON_AddStringToObject(err, "message", "Not a terminal session");
        char *json = cJSON_PrintUnformatted(err);
        cJSON_Delete(err);
        
        if (json) {
            httpd_ws_frame_t ws_pkt = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)json,
                .len = strlen(json)
            };
            httpd_ws_send_frame(req, &ws_pkt);
            free(json);
        }
        return;
    }
    
    if (strlen(command) >= TS_CONSOLE_MAX_LINE_LENGTH) { ssh_request_error(req,"Command is too long"); return; }
    ts_ws_peer_t peer = *(ts_ws_peer_t *)req->sess_ctx;
    esp_err_t error;
    ts_ws_operation_t *op = ts_ws_op_create(TS_WS_OP_TERMINAL,peer,&error);
    if (!op) { ssh_request_error(req,"A terminal command is already running"); return; }
    terminal_job_t *job = calloc(1,sizeof(*job));
    if (job) job->command=strdup(command);
    if (!job || !job->command) {
        free(job);ts_ws_op_discard(op);ts_ws_op_executor_done(op);ssh_request_error(req,"No memory for terminal command");return;
    }
    job->op=op;job->peer=peer;
    ts_ws_op_bind(op,job,terminal_job_destroy);
    ts_ws_op_set_maintenance(op,terminal_job_maintain);
    xSemaphoreTake(s_output_mutex,portMAX_DELAY);s_terminal_output_len=0;s_terminal_output_buf[0]=0;xSemaphoreGive(s_output_mutex);
    ts_ws_op_open(op);
    /* Arbitrary console commands may access NVS/flash: stack must be internal. */
    if (xTaskCreate(terminal_job_task,"terminal_cmd",8192,job,5,NULL)!=pdPASS) {
        free(job->command);job->command=NULL;job->finished=true;
        ts_ws_op_discard(op);ts_ws_op_executor_done(op);ssh_request_error(req,"Failed to start terminal command");return;
    }
    const char *started="{\"type\":\"command_started\"}";
    httpd_ws_frame_t frame={.type=HTTPD_WS_TYPE_TEXT,.payload=(uint8_t*)started,.len=strlen(started)};
    httpd_ws_send_frame(req,&frame);
}

/* 启动终端会话 */
static esp_err_t start_terminal_session(httpd_req_t *req, const cJSON *msg)
{
    int fd = httpd_req_to_sockfd(req);
    if (!bind_request_session(req, msg, TS_PERM_ROOT)) {
        ssh_request_error(req, "Unauthorized");
        return ESP_OK;
    }
    ts_ws_operation_t *running = ts_ws_op_acquire(TS_WS_OP_TERMINAL,0);
    if (running) {
        terminal_job_t *job = ts_ws_op_data(running);
        bool other = job && !ts_ws_peer_equal(job->peer,*(ts_ws_peer_t *)req->sess_ctx);
        ts_ws_op_release(running);
        if (other) { ssh_request_error(req,"Previous terminal command is still stopping"); return ESP_FAIL; }
    }
    // 检查是否有其他终端会话
    if (s_terminal_client_fd >= 0 && s_terminal_client_fd != fd) {
        // 检查旧的终端 fd 是否还在活跃客户端列表中
        bool old_fd_active = false;
        ts_ws_peer_t old_peer={0};
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_clients[i].active && s_clients[i].fd == s_terminal_client_fd) {
                old_fd_active = true;
                old_peer=s_clients[i].peer;
                break;
            }
        }
        
#ifdef CONFIG_TS_WEBUI_AUTH_REQUIRED
        if (old_fd_active && !ts_ws_peer_authorized(old_peer, TS_PERM_ROOT)) {
            cleanup_disconnected_client(old_peer);
            httpd_sess_trigger_close(old_peer.server, old_peer.fd);
            old_fd_active = false;
        }
#endif
        // 如果旧的 fd 已经不活跃（例如页面刷新），强制清理
        if (!old_fd_active) {
            TS_LOGW(TAG, "Terminal session orphaned (fd=%d), cleaning up", s_terminal_client_fd);
            ts_console_clear_output_cb();
            s_terminal_client_fd = -1;
        } else {
            /* A background retry must not take over a live terminal. Reject
             * only the requesting connection; its handler closes it below. */
            static const char busy[] = "{\"type\":\"terminal_busy\",\"retry_after\":5}";
            httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)busy, .len = sizeof(busy) - 1};
            httpd_ws_send_frame(req, &frame);
            uint8_t reason[] = {0x03, 0xf5};
            frame = (httpd_ws_frame_t){.type = HTTPD_WS_TYPE_CLOSE, .payload = reason, .len = sizeof(reason)};
            httpd_ws_send_frame(req, &frame);
            return ESP_FAIL;
        }
    }
    
    // 设置为终端客户端
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            set_client_role(*(ts_ws_peer_t *)req->sess_ctx, WS_CLIENT_TYPE_TERMINAL, TS_LOG_NONE);
            break;
        }
    }
    
    // 设置输出回调
    s_terminal_client_fd = fd;
    /* Command output is bound to its execution context, not a global callback. */
    
    // 发送确认消息和欢迎信息
    cJSON *welcome = cJSON_CreateObject();
    cJSON_AddStringToObject(welcome, "type", "connected");
    cJSON_AddStringToObject(welcome, "message", "Terminal session started");
    cJSON_AddStringToObject(welcome, "prompt", "tianshan> ");
    char *json = cJSON_PrintUnformatted(welcome);
    cJSON_Delete(welcome);
    
    if (json) {
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = strlen(json)
        };
        if (ts_ws_peer_authorized(*(ts_ws_peer_t *)req->sess_ctx, TS_PERM_ROOT))
            httpd_ws_send_frame(req, &ws_pkt);
        free(json);
    }
    
    TS_LOGD(TAG, "Terminal session started (fd=%d)", fd);
    return ESP_OK;
}

/* 清理断开的客户端 */
static void cleanup_disconnected_client(ts_ws_peer_t peer)
{
    terminal_job_cancel(peer,true);
    bool was_log_client = false;
    int fd=peer.fd;
    ts_ws_peer_t live;
    if(ts_ws_peer_get(peer.server,peer.fd,&live) && ts_ws_peer_equal(live,peer)) {
        ts_ws_peer_close(peer);
        return;
    }

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_clients[i].active && ts_ws_peer_equal(s_clients[i].peer,peer)) {
            // 如果是终端客户端，清理输出回调
            if (s_clients[i].type == WS_CLIENT_TYPE_TERMINAL && s_terminal_client_fd == fd) {
                ts_console_clear_output_cb();
                s_terminal_client_fd = -1;
                if (s_terminal_mutex) {
                    xSemaphoreGive(s_terminal_mutex);
                }
            }
            // 检查是否是日志客户端
            if (s_clients[i].type == WS_CLIENT_TYPE_LOG) {
                was_log_client = true;
            }
            s_clients[i].active = false;
            TS_LOGD(TAG, "WebSocket client disconnected (fd=%d)", fd);
            break;
        }
    }
    
    // 如果是日志客户端断开，更新日志流状态
    if (was_log_client) {
        update_log_stream_state();
    }
}

static void peer_closed(ts_ws_peer_t peer)
{
    ts_ws_client_disconnected(peer);
    ts_ws_operation_t *op=ts_ws_op_shell_connection(peer);
    if(op){ssh_close(op,"closed","SSH session closed");ts_ws_op_release(op);}
    cleanup_disconnected_client(peer);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        TS_LOGD(TAG, "WebSocket handshake");
        // 初次连接时添加为事件客户端
        if (s_ws_stopping) return ESP_ERR_INVALID_STATE;
        if (ts_http_server_admit_ws(req, MAX_WS_CLIENTS) != ESP_OK) return ESP_FAIL;
        return add_client(req, WS_CLIENT_TYPE_EVENT);
    }
    
    ts_ws_peer_t peer;
    if (!req->sess_ctx || !ts_ws_peer_get(req->handle, httpd_req_to_sockfd(req), &peer) ||
        !ts_ws_peer_equal(peer, *(ts_ws_peer_t *)req->sess_ctx)) return ESP_ERR_INVALID_STATE;

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // Get frame info
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        TS_LOGD(TAG, "ws_recv_frame error: %s", esp_err_to_name(ret));
        cleanup_disconnected_client(peer);
        return ret;
    }
    
    // 处理关闭帧
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        TS_LOGD(TAG, "WebSocket close frame received");
        cleanup_disconnected_client(peer);
        return ESP_OK;
    }
    
    if (ws_pkt.len == 0) {
        return ESP_OK;
    }
    
    // Allocate buffer for payload (PSRAM first)
    uint8_t *buf = heap_caps_malloc(ws_pkt.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(ws_pkt.len + 1);
    }
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        cleanup_disconnected_client(peer);
        free(buf);
        return ret;
    }
    buf[ws_pkt.len] = '\0';
    
    // Parse message
    cJSON *msg = cJSON_Parse((char *)buf);
    free(buf);
    
    if (msg) {
        cJSON *type = cJSON_GetObjectItem(msg, "type");
        if (type && cJSON_IsString(type)) {
            /* Keep existing connections (and heartbeat) alive while results drain.
             * Reject new work with a protocol error, not an HTTPD session error. */
            if(s_ws_stopping && strcmp(type->valuestring,"ping") &&
               strcmp(type->valuestring,"unsubscribe")) {
                const char *error="{\"type\":\"error\",\"message\":\"Service is stopping\"}";
                httpd_ws_frame_t frame={.type=HTTPD_WS_TYPE_TEXT,.payload=(uint8_t*)error,.len=strlen(error)};
                esp_err_t result=httpd_ws_send_frame(req,&frame);
                cJSON_Delete(msg);
                return result;
            }

            TS_LOGD(TAG, "WS msg type=%s from fd=%d", type->valuestring, httpd_req_to_sockfd(req));
            
            if (strcmp(type->valuestring, "auth") == 0) {
                handle_auth(req, msg);
            }
            else if (strcmp(type->valuestring, "ping") == 0) {
                /* Existing heartbeat retires a revoked binding even when the
                 * producer has already filtered every private recipient. Pong
                 * remains public, including for anonymous connections. */
                (void)ts_ws_peer_authorized(peer, TS_PERM_READ);
                // Respond to ping
                cJSON *pong = cJSON_CreateObject();
                cJSON_AddStringToObject(pong, "type", "pong");
                char *pong_str = cJSON_PrintUnformatted(pong);
                cJSON_Delete(pong);
                
                ws_pkt.payload = (uint8_t *)pong_str;
                ws_pkt.len = strlen(pong_str);
                httpd_ws_send_frame(req, &ws_pkt);
                free(pong_str);
            }
            else if (strcmp(type->valuestring, "subscribe") == 0) {
                // 处理订阅请求（新增：topic 订阅）
                cJSON *topic = cJSON_GetObjectItem(msg, "topic");
                cJSON *params = cJSON_GetObjectItem(msg, "params");
                
                if (topic && cJSON_IsString(topic)) {
                    esp_err_t ret = ts_ws_subscribe(*(ts_ws_peer_t *)req->sess_ctx, topic->valuestring, params);
                    
                    // 发送确认消息
                    cJSON *ack = cJSON_CreateObject();
                    cJSON_AddStringToObject(ack, "type", "subscribed");
                    cJSON_AddStringToObject(ack, "topic", topic->valuestring);
                    cJSON_AddBoolToObject(ack, "success", ret == ESP_OK);
                    if (ret != ESP_OK) {
                        cJSON_AddStringToObject(ack, "error", esp_err_to_name(ret));
                    }
                    
                    char *ack_str = cJSON_PrintUnformatted(ack);
                    cJSON_Delete(ack);
                    if (ack_str) {
                        ws_pkt.payload = (uint8_t *)ack_str;
                        ws_pkt.len = strlen(ack_str);
                        httpd_ws_send_frame(req, &ws_pkt);
                        free(ack_str);
                    }
                } else {
                    // 保持为事件订阅客户端（已在握手时添加）
                    TS_LOGD(TAG, "Client subscribed to events");
                }
            }
            else if (strcmp(type->valuestring, "unsubscribe") == 0) {
                // 处理取消订阅请求（新增）
                cJSON *topic = cJSON_GetObjectItem(msg, "topic");
                
                if (topic && cJSON_IsString(topic)) {
                    esp_err_t ret = ts_ws_unsubscribe(*(ts_ws_peer_t *)req->sess_ctx, topic->valuestring);
                    
                    // 发送确认消息
                    cJSON *ack = cJSON_CreateObject();
                    cJSON_AddStringToObject(ack, "type", "unsubscribed");
                    cJSON_AddStringToObject(ack, "topic", topic->valuestring);
                    cJSON_AddBoolToObject(ack, "success", ret == ESP_OK);
                    
                    char *ack_str = cJSON_PrintUnformatted(ack);
                    cJSON_Delete(ack);
                    if (ack_str) {
                        ws_pkt.payload = (uint8_t *)ack_str;
                        ws_pkt.len = strlen(ack_str);
                        httpd_ws_send_frame(req, &ws_pkt);
                        free(ack_str);
                    }
                }
            }
            else if (strcmp(type->valuestring, "terminal_start") == 0) {
                // 启动终端会话
                ret = start_terminal_session(req, msg);
            }
            else if (strcmp(type->valuestring, "terminal_input") == 0) {
                // 终端命令输入
#ifdef CONFIG_TS_WEBUI_AUTH_REQUIRED
                if (s_terminal_client_fd != httpd_req_to_sockfd(req) ||
                    !ts_ws_peer_authorized(peer, TS_PERM_ROOT)) {
                    ssh_request_error(req, "Unauthorized");
                } else
#endif
                {
                cJSON *data = cJSON_GetObjectItem(msg, "data");
                if (data && cJSON_IsString(data)) {
                    handle_terminal_command(req, data->valuestring);
                }
                }
            }
            else if (strcmp(type->valuestring, "terminal_interrupt") == 0) {
                // 发送中断信号 (Ctrl+C)
#ifdef CONFIG_TS_WEBUI_AUTH_REQUIRED
                if (s_terminal_client_fd != httpd_req_to_sockfd(req) ||
                    !ts_ws_peer_authorized(peer, TS_PERM_ROOT)) {
                    ssh_request_error(req, "Unauthorized");
                } else
#endif
                {
                if (terminal_job_cancel(peer,false)) {
                    const char *text="{\"type\":\"command_stopping\"}";
                    httpd_ws_frame_t frame={.type=HTTPD_WS_TYPE_TEXT,.payload=(uint8_t*)text,.len=strlen(text)};
                    httpd_ws_send_frame(req,&frame);
                }
                TS_LOGD(TAG, "Terminal interrupt requested");
                }
            }
            else if (strcmp(type->valuestring, "terminal_stop") == 0) {
                // 停止终端会话
                int fd = httpd_req_to_sockfd(req);
                if (s_terminal_client_fd == fd) {
                    terminal_job_cancel(*(ts_ws_peer_t *)req->sess_ctx,true);
                    ts_console_clear_output_cb();
                    s_terminal_client_fd = -1;
                    // 恢复为普通事件客户端
                    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                        if (s_clients[i].active && s_clients[i].fd == fd) {
                            set_client_role(*(ts_ws_peer_t *)req->sess_ctx, WS_CLIENT_TYPE_EVENT, TS_LOG_NONE);
                            break;
                        }
                    }
                    TS_LOGD(TAG, "Terminal session stopped");
                }
            }
            /* SSH Shell 消息处理 */
            else if (strcmp(type->valuestring, "ssh_connect") == 0) {
                // SSH 连接请求
                handle_ssh_connect(req, msg);
            }
            else if (!strcmp(type->valuestring,"ssh_input") || !strcmp(type->valuestring,"ssh_disconnect") ||
                     !strcmp(type->valuestring,"ssh_signal") || !strcmp(type->valuestring,"ssh_resize")) {
                handle_ssh_control(req,peer,type->valuestring,msg);
            }
            /* 日志流订阅 */
            else if (strcmp(type->valuestring, "log_subscribe") == 0) {
                // 订阅日志流
                int fd = httpd_req_to_sockfd(req);
                cJSON *level = cJSON_GetObjectItem(msg, "minLevel");
                ts_log_level_t min_level = TS_LOG_VERBOSE;  // 默认接收所有级别
                if (level && cJSON_IsNumber(level)) {
                    min_level = (ts_log_level_t)level->valueint;
                }
                
                bool client_found = false;
                for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                    if (s_clients[i].active && s_clients[i].fd == fd) {
                        set_client_role(*(ts_ws_peer_t *)req->sess_ctx, WS_CLIENT_TYPE_LOG, min_level);
                        client_found = true;
                        TS_LOGI(TAG, "Client %d subscribed to logs (minLevel=%d)", i, min_level);
                        break;
                    }
                }
                
                if (!client_found) {
                    TS_LOGW(TAG, "log_subscribe: Client fd=%d not found in active clients", fd);
                }
                
                // 更新日志流状态
                update_log_stream_state();
                
                // 发送确认
                cJSON *ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "type", "log_subscribed");
                cJSON_AddNumberToObject(ack, "minLevel", min_level);
                char *ack_str = cJSON_PrintUnformatted(ack);
                cJSON_Delete(ack);
                if (ack_str) {
                    ws_pkt.payload = (uint8_t *)ack_str;
                    ws_pkt.len = strlen(ack_str);
                    httpd_ws_send_frame(req, &ws_pkt);
                    free(ack_str);
                }
            }
            else if (strcmp(type->valuestring, "log_unsubscribe") == 0) {
                // 取消订阅日志流
                int fd = httpd_req_to_sockfd(req);
                for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                    if (s_clients[i].active && s_clients[i].fd == fd) {
                        set_client_role(*(ts_ws_peer_t *)req->sess_ctx, WS_CLIENT_TYPE_EVENT, TS_LOG_NONE);
                        break;
                    }
                }
                // 更新日志流状态
                update_log_stream_state();
            }
            else if (strcmp(type->valuestring, "log_set_level") == 0) {
                // 更新日志级别过滤
                int fd = httpd_req_to_sockfd(req);
                cJSON *level = cJSON_GetObjectItem(msg, "minLevel");
                if (level && cJSON_IsNumber(level)) {
                    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                        if (s_clients[i].active && s_clients[i].fd == fd && 
                            s_clients[i].type == WS_CLIENT_TYPE_LOG) {
                            set_client_role(*(ts_ws_peer_t *)req->sess_ctx, WS_CLIENT_TYPE_LOG, (ts_log_level_t)level->valueint);
                            break;
                        }
                    }
                }
            }
            /* 获取历史日志 */
            else if (strcmp(type->valuestring, "log_get_history") == 0) {
                // 获取历史日志
                cJSON *j_limit = cJSON_GetObjectItem(msg, "limit");
                cJSON *j_min = cJSON_GetObjectItem(msg, "minLevel");
                cJSON *j_max = cJSON_GetObjectItem(msg, "maxLevel");
                
                size_t limit = 500;
                ts_log_level_t min_level = TS_LOG_ERROR;
                ts_log_level_t max_level = TS_LOG_VERBOSE;
                
                if (j_limit && cJSON_IsNumber(j_limit)) limit = (size_t)j_limit->valueint;
                if (j_min && cJSON_IsNumber(j_min)) min_level = (ts_log_level_t)j_min->valueint;
                if (j_max && cJSON_IsNumber(j_max)) max_level = (ts_log_level_t)j_max->valueint;
                if (limit > CONFIG_TS_LOG_BUFFER_SIZE) limit = CONFIG_TS_LOG_BUFFER_SIZE;
                
                // 分配缓冲区（优先使用 PSRAM）
                ts_log_entry_t *entries = heap_caps_malloc(limit * sizeof(ts_log_entry_t),
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!entries) {
                    entries = malloc(limit * sizeof(ts_log_entry_t));  // Fallback to DRAM
                }
                if (entries) {
                    size_t count = ts_log_buffer_search(entries, limit, min_level, max_level, NULL, NULL);
                    
                    // 构建响应
                    cJSON *resp = cJSON_CreateObject();
                    cJSON_AddStringToObject(resp, "type", "log_history");
                    cJSON *logs = cJSON_AddArrayToObject(resp, "logs");
                    
                    for (size_t i = 0; i < count; i++) {
                        cJSON *entry = cJSON_CreateObject();
                        cJSON_AddNumberToObject(entry, "timestamp", entries[i].timestamp_ms);
                        cJSON_AddNumberToObject(entry, "level", entries[i].level);
                        cJSON_AddStringToObject(entry, "levelName", 
                                               (entries[i].level < 6) ? s_level_names[entries[i].level] : "UNKNOWN");
                        cJSON_AddStringToObject(entry, "tag", entries[i].tag);
                        cJSON_AddStringToObject(entry, "message", entries[i].message);
                        cJSON_AddStringToObject(entry, "task", entries[i].task_name);
                        cJSON_AddItemToArray(logs, entry);
                    }
                    cJSON_AddNumberToObject(resp, "total", count);
                    
                    char *resp_str = cJSON_PrintUnformatted(resp);
                    cJSON_Delete(resp);
                    free(entries);
                    
                    if (resp_str) {
                        ws_pkt.payload = (uint8_t *)resp_str;
                        ws_pkt.len = strlen(resp_str);
                        httpd_ws_send_frame(req, &ws_pkt);
                        free(resp_str);
                    }
                }
            }
        }
        cJSON_Delete(msg);
    }
    
    return ret;
}

esp_err_t ts_webui_ws_init(void)
{
    TS_LOGI(TAG, "Initializing WebSocket with Terminal support");
    
    httpd_handle_t server = ts_http_server_get_handle();
    if (!server) return ESP_ERR_INVALID_STATE;
    if (s_server) return s_ws_stopping ? ESP_ERR_INVALID_STATE : ESP_OK;
    esp_err_t transport_ret = ts_ws_transport_start(server, peer_closed);
    if (transport_ret != ESP_OK) return transport_ret;
    s_server = server;
    s_ws_stopping = false;
    ts_http_server_set_stop_hooks(ts_webui_ws_stop, ts_webui_ws_stopped);
    memset(s_clients, 0, sizeof(s_clients));
    s_terminal_client_fd = -1;
    
    // 初始化订阅管理器（新增）
    esp_err_t ret = ts_ws_subscriptions_init();
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to initialize subscriptions: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 创建终端会话互斥锁
    if (!s_terminal_mutex) {
        s_terminal_mutex = xSemaphoreCreateMutex();
    }
    
    // 创建输出缓冲区互斥锁
    if (!s_output_mutex) {
        s_output_mutex = xSemaphoreCreateMutex();
    }
    
    if (!s_terminal_mutex || !s_output_mutex) return ESP_ERR_NO_MEM;

    // 分配终端输出缓冲区（优先使用 PSRAM）
    if (!s_terminal_output_buf) {
        s_terminal_output_buf = heap_caps_malloc(TERMINAL_OUTPUT_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_terminal_output_buf) {
            // Fallback 到 DRAM
            TS_LOGW(TAG, "PSRAM not available, using DRAM for terminal buffer");
            s_terminal_output_buf = malloc(TERMINAL_OUTPUT_BUF_SIZE);
            if (!s_terminal_output_buf) {
                TS_LOGE(TAG, "Failed to allocate terminal output buffer");
                return ESP_ERR_NO_MEM;
            }
        } else {
            TS_LOGI(TAG, "Terminal buffer allocated in PSRAM (%d bytes)", TERMINAL_OUTPUT_BUF_SIZE);
        }
        s_terminal_output_buf[0] = '\0';
        s_terminal_output_len = 0;
    }
    
    // Get HTTP server handle
    if (!server) {
        TS_LOGE(TAG, "HTTP server not started");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_server = server;
    
    // Register WebSocket URI handler
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true
    };
    
    esp_err_t reg_ret = httpd_register_uri_handler(server, &ws_uri);
    if (reg_ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register WebSocket handler: %s", esp_err_to_name(reg_ret));
        return reg_ret;
    }
    
    // 注册电压保护事件处理器
    if (!s_power_event_handle) {
        ret = ts_event_register(TS_EVENT_BASE_POWER, TS_EVENT_ANY_ID, 
                                 power_policy_event_handler, NULL, &s_power_event_handle);
        if (ret == ESP_OK) {
            TS_LOGI(TAG, "Power policy event handler registered");
        } else {
            TS_LOGW(TAG, "Failed to register power event handler: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    ts_ws_op_enable();
    TS_LOGI(TAG, "WebSocket handler registered at /ws");
    return ESP_OK;
}

esp_err_t ts_webui_ws_stop(httpd_handle_t server)
{
    if (ts_ws_transport_in_context() || ts_event_in_callback() || ts_ws_subscriptions_in_context()) return ESP_ERR_INVALID_STATE;
    ts_ws_operation_t *terminal = ts_ws_op_acquire(TS_WS_OP_TERMINAL,0);
    if (terminal) {
        terminal_job_t *job=ts_ws_op_data(terminal);if(job){job->cancel=true;job->detached=true;}
        ts_ws_op_release(terminal);
        return ESP_ERR_INVALID_STATE; /* sender/worker remain alive until retirement */
    }
    ts_ws_op_stop_admission();
    exec_retry_timer_drain();
    s_ws_stopping = true;
    esp_err_t ret = ts_ws_subscriptions_pause();
    if (ret != ESP_OK) return ret;
    ret = ts_ws_transport_quiesce(server, 6000);
    if (ret != ESP_OK) return ret;
    /* The worker still sends results even when this attempt returns busy. */
    ssh_cleanup();
    ts_ws_operation_t *exec=ts_ws_op_acquire(TS_WS_OP_EXEC,0);
    bool executing=exec && ts_ws_op_executing(exec);
    if(exec)ts_ws_op_release(exec);
    if(executing)return ESP_ERR_INVALID_STATE; /* never cancel the remote job to stop WebUI */
    int64_t deadline=esp_timer_get_time()+2000000;
    while(ts_ws_op_busy()) {
        if(esp_timer_get_time()>=deadline)return ESP_ERR_TIMEOUT;
        vTaskDelay(1); /* producer, callbacks, timer daemon and sender remain alive */
    }
    if (s_power_event_handle) {
        ret = ts_event_unregister_sync(s_power_event_handle, 6000);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) return ret;
        s_power_event_handle = NULL;
    }
    ret = ts_webui_log_stream_enable(false);
    if (ret != ESP_OK) return ret;
    ret = ts_ws_subscriptions_drain();
    if (ret != ESP_OK) return ret;
    if(ts_ws_op_busy())return ESP_ERR_INVALID_STATE;
    ret = ts_ws_transport_stop(server, 6000);
    if (ret != ESP_OK) return ret;
    return ts_ws_subscriptions_deinit();
}

void ts_webui_ws_stopped(httpd_handle_t server)
{
    ts_ws_transport_stopped(server);
    exec_cache_clear();
    if (s_server == server) s_server = NULL;
    /* Existing terminal buffer/mutex are stable reusable singleton resources. */
}

esp_err_t ts_webui_broadcast(const char *message)
{
    if (!message) return ESP_ERR_INVALID_ARG;
    
    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)message,
        .len = strlen(message)
    };
    
    return ts_ws_transport_broadcast(&ws_pkt);
}

esp_err_t ts_webui_broadcast_event(const char *event_type, const char *data)
{
    if (!event_type) return ESP_ERR_INVALID_ARG;
    
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "event");
    cJSON_AddStringToObject(msg, "event", event_type);
    if (data) {
        cJSON *data_obj = cJSON_Parse(data);
        if (data_obj) {
            cJSON_AddItemToObject(msg, "data", data_obj);
        } else {
            cJSON_AddStringToObject(msg, "data", data);
        }
    }
    
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    
    if (json) {
        esp_err_t ret = ts_webui_broadcast(json);
        free(json);
        return ret;
    }
    
    return ESP_ERR_NO_MEM;
}

/*===========================================================================*/
/*                      Log Streaming via WebSocket                           */
/*===========================================================================*/

/**
 * @brief 日志回调 - 将日志推送到所有订阅的 WebSocket 客户端
 */
static void log_ws_callback(const ts_log_entry_t *entry, void *user_data)
{
    (void)user_data;
    
    if (!entry || !s_server || !s_log_streaming_enabled) {
        // Debug: log why callback is skipped
        if (!entry) TS_LOGD(TAG, "log_ws_callback: entry is NULL");
        if (!s_server) TS_LOGD(TAG, "log_ws_callback: s_server is NULL");
        if (!s_log_streaming_enabled) TS_LOGD(TAG, "log_ws_callback: streaming disabled");
        return;
    }
    
    // 构造日志消息
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "log");
    cJSON_AddNumberToObject(msg, "timestamp", entry->timestamp_ms);
    cJSON_AddNumberToObject(msg, "level", entry->level);
    cJSON_AddStringToObject(msg, "levelName", 
                           (entry->level < 6) ? s_level_names[entry->level] : "UNKNOWN");
    cJSON_AddStringToObject(msg, "tag", entry->tag);
    cJSON_AddStringToObject(msg, "message", entry->message);
    cJSON_AddStringToObject(msg, "task", entry->task_name);
    
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    
    if (!json) return;
    
    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json)
    };
    
    ts_ws_transport_log(&ws_pkt, entry->level);

    free(json);
}

/**
 * @brief 启用/禁用日志 WebSocket 流
 */
esp_err_t ts_webui_log_stream_enable(bool enable)
{
    if(enable) {
        if(!s_log_callback_handle) {
            esp_err_t ret=ts_log_add_callback(log_ws_callback,TS_LOG_VERBOSE,NULL,&s_log_callback_handle);
            if(ret!=ESP_OK)return ret;
        }
        /* A failed remove retains the handle; resubscribe reuses it. */
        s_log_streaming_enabled=true;
    } else {
        s_log_streaming_enabled=false;
        if(s_log_callback_handle) {
            esp_err_t ret=ts_log_remove_callback(s_log_callback_handle);
            if(ret!=ESP_OK)return ret;
            s_log_callback_handle=NULL;
        }
    }
    return ESP_OK;
}

/**
 * @brief 检查是否有日志订阅客户端
 */
static bool has_log_clients(void)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].type == WS_CLIENT_TYPE_LOG) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 更新日志流状态（根据是否有订阅客户端）
 */
static void update_log_stream_state(void)
{
    bool need_streaming = has_log_clients();
    TS_LOGI(TAG, "update_log_stream_state: need_streaming=%d, current=%d", 
            need_streaming, s_log_streaming_enabled);
    if (need_streaming != s_log_streaming_enabled) {
        ts_webui_log_stream_enable(need_streaming);
    }
}

/*===========================================================================*/
/*                     SSH Exec Stream Functions                              */
/*===========================================================================*/

/* Only the active result and the latest completed result are retained. No
 * credentials or log history. Records are pinned while parsed, never freed or
 * allocated under the critical section. Published final JSON is immutable. */
typedef struct exec_record {
    unsigned refs; uint32_t id; bool done;
    ts_webui_ssh_receipt_t receipt;
    char json[TS_WS_LEGACY_BYTES];
} exec_record_t;
static portMUX_TYPE s_exec_cache_lock=portMUX_INITIALIZER_UNLOCKED;
static exec_record_t *s_exec_active,*s_exec_latest;
static char s_exec_boot[33];
static void exec_hex_random(char out[33]) {
    unsigned char bytes[16];esp_fill_random(bytes,sizeof(bytes));
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;i++){out[2*i]=hex[bytes[i]>>4];out[2*i+1]=hex[bytes[i]&15];}out[32]=0;
}
static void exec_record_release(exec_record_t *r) {
    if(!r)return;
    portENTER_CRITICAL(&s_exec_cache_lock);assert(r->refs);bool last=!--r->refs;portEXIT_CRITICAL(&s_exec_cache_lock);
    if(last)free(r);
}
static void exec_record_complete(exec_record_t *r) {
    portENTER_CRITICAL(&s_exec_cache_lock);
    r->done=true;r->refs++;exec_record_t *old=s_exec_latest;s_exec_latest=r;
    if(s_exec_active==r)s_exec_active=NULL; /* active is a borrowed params reference */
    portEXIT_CRITICAL(&s_exec_cache_lock);exec_record_release(old);
}
static void exec_cache_clear(void) {
    portENTER_CRITICAL(&s_exec_cache_lock);exec_record_t *old=s_exec_latest;s_exec_latest=NULL;portEXIT_CRITICAL(&s_exec_cache_lock);
    exec_record_release(old);
}
char *ts_webui_ssh_exec_status(uint32_t id,const char *boot,const char *key,esp_err_t *error) {
    if(error)*error=ESP_ERR_NOT_FOUND;
    if(!id || !boot || !key || strlen(boot)!=32 || strlen(key)!=32)return NULL;
    portENTER_CRITICAL(&s_exec_cache_lock);
    exec_record_t *r=s_exec_active && s_exec_active->id==id?s_exec_active:s_exec_latest;
    if(r && (r->id!=id || strcmp(r->receipt.boot_id,boot) || strcmp(r->receipt.result_key,key)))r=NULL;
    bool done=r && r->done;if(r)r->refs++;
    portEXIT_CRITICAL(&s_exec_cache_lock);
    if(!r)return NULL;
    char *json=done?strdup(r->json):NULL;
    if(!done){cJSON *obj=cJSON_CreateObject();cJSON_AddStringToObject(obj,"type","ssh_exec_running");cJSON_AddNumberToObject(obj,"session_id",id);json=cJSON_PrintUnformatted(obj);cJSON_Delete(obj);}
    if(error)*error=json?ESP_OK:ESP_ERR_NO_MEM;
    exec_record_release(r);return json;
}

/* SSH Exec 任务参数 */
typedef struct {
    ts_ws_operation_t *op;
    exec_record_t *record;
    SemaphoreHandle_t output_lock;
    exec_output_t *stream;
    char *send_bytes, *send_base64;
    bool collection_truncated, pending_final, conditions_incomplete;
    int64_t final_flush_until;
    ts_ssh_session_t session;
    SemaphoreHandle_t io;
    TimerHandle_t timer;
    atomic_bool timer_closing;
    atomic_uint timer_phase; /* 0 none, 1 live, 2 delete, 3 barrier, 4 queued, 5 drained */
    atomic_uint timer_retries;
    int64_t timer_retry_at; /* maintenance worker owns pacing */
    char *password_owned;
    ts_ssh_config_t config;
    char *command;           /* Task-owned full command, including caller wrapping. */
    uint32_t session_id;
    char keyid[64];           /* 存储 keyid 以便获取私钥 */
    char var_name[64];        /* 变量名（用于存储结果） */
    char command_id[TS_SSH_CMD_ID_MAX];
    uint64_t command_instance;
    uint64_t variable_instances[7];
    bool variables_bound;
    /* 可选参数 */
    char *expect_pattern;     /* 期望匹配的模式 */
    char *fail_pattern;       /* 失败模式 */
    char *extract_pattern;    /* 提取值的模式 */
    uint32_t timeout_ms;      /* 超时时间 */
    bool collect_output;      /* 是否收集输出 */
    uint32_t max_output_size; /* 最大输出大小 */
    bool stop_on_match;       /* 匹配成功后是否停止命令 */
    /* 运行时状态 */
    char *output_buffer;      /* 输出缓冲区 */
    size_t output_len;        /* 当前输出长度 */
    size_t output_capacity;   /* 缓冲区容量 */
    ts_ssh_match_window_t matching;
    /* 实时匹配状态 */
    bool match_found;         /* 是否已找到匹配 */
    bool expect_matched;      /* 期望模式是否匹配 */
    bool fail_matched;        /* 失败模式是否匹配 */
    char *extracted_value;    /* 提取的值 */
} ssh_exec_task_params_t;

static const char *const exec_variable_suffixes[] = {
    "status", "exit_code", "extracted", "expect_matched", "fail_matched", "host", "timestamp"
};

/* Configuration publication uses the same registration/binding gates. */
static bool exec_variables_begin(const ssh_exec_task_params_t *params)
{
    if (!params->variables_bound || !ts_variable_is_initialized()) return false;
    ts_variable_registration_begin();
    ts_ssh_binding_lock();
    bool current = !params->command_id[0] ||
        ts_auto_config_command_current(params->command_id, params->command_instance);
    for (unsigned i = 0; current && i < sizeof(exec_variable_suffixes) / sizeof(exec_variable_suffixes[0]); ++i) {
        char name[96];
        snprintf(name, sizeof(name), "%s.%s", params->var_name, exec_variable_suffixes[i]);
        current = ts_variable_instance(name) == params->variable_instances[i];
    }
    if (!current) {
        ts_ssh_binding_unlock();
        ts_variable_registration_end();
    }
    return current;
}

static void exec_variables_end(void)
{
    ts_ssh_binding_unlock();
    ts_variable_registration_end();
}

/* 简单的正则表达式匹配（支持基本模式）*/

static void exec_abort(ssh_exec_task_params_t *params)
{
    xSemaphoreTake(params->io,portMAX_DELAY);
    if(params->session)ts_ssh_abort(params->session);
    xSemaphoreGive(params->io);
}
/* SSH Exec 输出回调 - 广播到所有 WebSocket 客户端并收集输出 */
static void ssh_exec_output_callback(const char *data, size_t len, bool is_stderr, void *user_data)
{
    if (!data || len == 0) return;
    
    ssh_exec_task_params_t *params=user_data;
    ts_ws_op_retain(params->op); /* synchronous SSH callback borrows executor-owned params */
    uint32_t session_id=params->session_id;
    
    /* 收集输出到缓冲区 */
    if (params && params->collect_output && params->output_buffer) {
        size_t space = params->output_capacity - params->output_len - 1;
        size_t copy_len = (len < space) ? len : space;
        params->collection_truncated |= copy_len != len || memchr(data,0,len)!=NULL;
        if (copy_len > 0) {
            memcpy(params->output_buffer + params->output_len, data, copy_len);
            params->output_len += copy_len;
            params->output_buffer[params->output_len] = '\0';
        }
    }
    
    xSemaphoreTake(params->output_lock,portMAX_DELAY);
    exec_output_put(params->stream,data,len,is_stderr);
    xSemaphoreGive(params->output_lock);
    ts_ws_subscriptions_wake();

    /* 实时模式匹配 */
    if (params->matching.data) {
        ts_ssh_match_window_append(&params->matching,data,len);
        bool should_send_match = false;
        bool pattern_matched = false;  /* expect/fail 模式是否匹配 */
        bool is_first_extract = false;  /* 是否为首次提取 */
        
        /* 检查失败模式（仅在尚未匹配时） */
        if (!params->match_found && params->fail_pattern && params->fail_pattern[0]) {
            if (ts_ssh_match_window_match(&params->matching, params->fail_pattern, NULL)) {
                params->fail_matched = true;
                params->match_found = true;
                should_send_match = true;
                pattern_matched = true;
                TS_LOGW(TAG, "Realtime fail pattern matched");
            }
        }
        
        /* 检查期望模式（仅在尚未匹配时，且失败模式没有匹配） */
        if (!params->match_found && !params->fail_matched &&
            params->expect_pattern && params->expect_pattern[0]) {
            if (ts_ssh_match_window_match(&params->matching, params->expect_pattern, NULL)) {
                params->expect_matched = true;
                params->match_found = true;
                should_send_match = true;
                pattern_matched = true;
                TS_LOGI(TAG, "Realtime expect pattern matched");
            }
        }
        
        /* 提取值 */
        if (params->extract_pattern && params->extract_pattern[0]) {
            char *extracted = NULL;
            if (ts_ssh_match_window_match(&params->matching, params->extract_pattern, &extracted)) {
                /* 检查是否为首次提取 */
                if (!params->extracted_value) {
                    is_first_extract = true;
                }
                
                /* 检查是否与上次提取的值不同 */
                bool is_new_value = false;
                if (is_first_extract) {
                    is_new_value = true;
                } else if (extracted && strcmp(extracted, params->extracted_value) != 0) {
                    is_new_value = true;
                }
                
                if (is_new_value) {
                    /* 释放旧值，保存新值 */
                    if (params->extracted_value) {
                        free(params->extracted_value);
                    }
                    params->extracted_value = extracted;
                    should_send_match = true;
                    TS_LOGI(TAG, "Realtime extract: %s (first=%d)", extracted ? extracted : "(null)", is_first_extract);
                } else {
                    /* 值相同，释放本次提取的 */
                    free(extracted);
                }
            }
        }
        
        /* 判断是否应该停止 */
        bool should_stop = false;
        if (params->stop_on_match) {
            /* 勾选了"匹配后停止"：expect/fail 匹配 或 首次提取成功 都应该停止 */
            if (pattern_matched || is_first_extract) {
                should_stop = true;
            }
        }
        
        /* 发送实时匹配消息 */
        if (should_send_match) {
            ts_ws_output_ticket_t match_ticket;
            if(ts_ws_op_output_begin(params->op,&match_ticket)) {
            if(match_ticket.count) {
            cJSON *match_msg = cJSON_CreateObject();
            bool match_encoded=match_msg!=NULL;
            match_encoded = (cJSON_AddStringToObject(match_msg, "type", "ssh_exec_match"))!=NULL && match_encoded;
            match_encoded = (cJSON_AddNumberToObject(match_msg, "session_id", session_id))!=NULL && match_encoded;
            match_encoded = (cJSON_AddBoolToObject(match_msg, "expect_matched", params->expect_matched))!=NULL && match_encoded;
            match_encoded = (cJSON_AddBoolToObject(match_msg, "fail_matched", params->fail_matched))!=NULL && match_encoded;
            if (params->extracted_value) {
                match_encoded = (cJSON_AddStringToObject(match_msg, "extracted", params->extracted_value))!=NULL && match_encoded;
            }
            /* 标记是否为终止匹配 */
            match_encoded = (cJSON_AddBoolToObject(match_msg, "is_final", should_stop))!=NULL && match_encoded;
            
            char *match_json = match_encoded?cJSON_PrintUnformatted(match_msg):NULL;
            cJSON_Delete(match_msg);
            ts_ws_op_output_finish(&match_ticket,match_json);
            free(match_json);
            
            } else ts_ws_op_output_finish(&match_ticket,NULL);
            }
            /* ========== 根据命令类型决定变量更新时机 ========== */
            /* 
             * 持续监控型：只有 extract_pattern，无 expect/fail/stop_on_match
             *   → 每次提取到值就实时更新变量（适合持续运行的命令如 ping）
             * 
             * 结果导向型：有 expect/fail 或 stop_on_match
             *   → 等命令结束时才更新变量（在 ssh_exec_task 完成处理）
             */
            bool is_continuous_mode = params->extract_pattern &&
                                      !params->expect_pattern &&
                                      !params->fail_pattern &&
                                      !params->stop_on_match;
            
            TS_LOGI(TAG, "Variable update check: var_name='%s', continuous=%d, extract=%p, expect=%p, fail=%p, stop=%d",
                    params->var_name,
                    is_continuous_mode,
                    params->extract_pattern,
                    params->expect_pattern,
                    params->fail_pattern,
                    params->stop_on_match);
            
            if (is_continuous_mode && params->var_name[0] && exec_variables_begin(params)) {
                /* 持续监控模式：实时更新变量 */
                char full_var_name[96];
                ts_auto_variable_t var = {0};
                strncpy(var.source_id, params->var_name, sizeof(var.source_id) - 1);
                var.flags = 0;
                
                /* 更新 extracted 变量 */
                if (params->extracted_value) {
                    snprintf(full_var_name, sizeof(full_var_name), "%s.extracted", params->var_name);
                    strncpy(var.name, full_var_name, sizeof(var.name) - 1);
                    var.value.type = TS_AUTO_VAL_STRING;
                    strncpy(var.value.str_val, params->extracted_value, sizeof(var.value.str_val) - 1);
                    esp_err_t ret = ts_variable_upsert(&var);
                    TS_LOGI(TAG, "Registered %s = %s (ret=%d)", full_var_name, params->extracted_value, ret);
                }
                
                /* 更新 status 为 running */
                snprintf(full_var_name, sizeof(full_var_name), "%s.status", params->var_name);
                strncpy(var.name, full_var_name, sizeof(var.name) - 1);
                var.value.type = TS_AUTO_VAL_STRING;
                strncpy(var.value.str_val, "running", sizeof(var.value.str_val) - 1);
                ts_variable_upsert(&var);
                
                /* 更新 host */
                snprintf(full_var_name, sizeof(full_var_name), "%s.host", params->var_name);
                strncpy(var.name, full_var_name, sizeof(var.name) - 1);
                var.value.type = TS_AUTO_VAL_STRING;
                strncpy(var.value.str_val, params->config.host ? params->config.host : "", sizeof(var.value.str_val) - 1);
                ts_variable_upsert(&var);
                
                /* 更新 timestamp */
                snprintf(full_var_name, sizeof(full_var_name), "%s.timestamp", params->var_name);
                strncpy(var.name, full_var_name, sizeof(var.name) - 1);
                var.value.type = TS_AUTO_VAL_INT;
                var.value.int_val = (int32_t)(esp_timer_get_time() / 1000000);
                ts_variable_upsert(&var);
                
                TS_LOGD(TAG, "Continuous mode: realtime update %s", params->var_name);
                exec_variables_end();
            }
            /* 结果导向模式：变量在命令完成时更新（见下方 ssh_exec_task 完成处理） */
            
            /* 如果应该停止，中断 SSH 执行 */
            if (should_stop) {
                TS_LOGI(TAG, "Aborting SSH execution (pattern=%d, first_extract=%d)",
                        pattern_matched, is_first_extract);
                if(ts_ws_op_request_stop(params->op,OP_STOP_MATCH))exec_abort(params);
            }
        }
    }
    ts_ws_op_release(params->op);
}

/* Timer's original context is pinned from creation through the daemon barrier.
 * The existing WS worker retries queue admission; it never waits on the daemon. */
static void ssh_exec_timeout_callback(TimerHandle_t timer)
{
    ssh_exec_task_params_t *params=pvTimerGetTimerID(timer);
    xSemaphoreTake(params->io,portMAX_DELAY);
    if(!params->timer_closing) {
        if(ts_ws_op_request_stop(params->op,OP_STOP_TIMEOUT) && params->session)ts_ssh_abort(params->session);
    }
    xSemaphoreGive(params->io);
}
static void exec_timer_drained(void *arg,uint32_t unused)
{
    (void)unused;ssh_exec_task_params_t *params=arg;
    params->timer_phase=5;
    ts_ws_op_release(params->op);
}
static void exec_stage_result(ssh_exec_task_params_t *p,const char *json) {
    /* The worker owns this record until it publishes pending_final. */
    if(!json || strlen(json)>=sizeof(p->record->json))
        snprintf(p->record->json,sizeof(p->record->json),"{\"type\":\"ssh_exec_error\",\"session_id\":%lu,\"error\":\"ssh_result_unknown\"}",(unsigned long)p->session_id);
    else strcpy(p->record->json,json);
    xSemaphoreTake(p->output_lock,portMAX_DELAY);
    p->final_flush_until=esp_timer_get_time()+2000000;p->pending_final=true;
    xSemaphoreGive(p->output_lock);ts_ws_subscriptions_wake();
}
static bool exec_stream_maintenance(ssh_exec_task_params_t *p) {
    xSemaphoreTake(p->output_lock,portMAX_DELAY);
    exec_span_t span=exec_output_peek(p->stream,p->send_bytes);
    bool pending=p->pending_final;
    bool expired=pending && esp_timer_get_time()>=p->final_flush_until;
    xSemaphoreGive(p->output_lock);
    if(span.len && !expired){
        exec_output_base64((unsigned char*)p->send_bytes,span.len,p->send_base64);
        cJSON *msg=cJSON_CreateObject();
        bool ok=msg && cJSON_AddStringToObject(msg,"type","ssh_exec_output") &&
            cJSON_AddNumberToObject(msg,"session_id",p->session_id) &&
            cJSON_AddNumberToObject(msg,"output_offset",(double)span.offset) &&
            cJSON_AddNumberToObject(msg,"output_length",span.len) &&
            cJSON_AddBoolToObject(msg,"is_stderr",span.is_stderr) &&
            cJSON_AddStringToObject(msg,"data_b64",p->send_base64);
        char *json=ok?cJSON_PrintUnformatted(msg):NULL;cJSON_Delete(msg);
        bool sent=false;ts_ws_output_ticket_t ticket;
        if(json && ts_ws_op_output_begin(p->op,&ticket))sent=ts_ws_op_output_try(&ticket,json);
        free(json);
        if(sent){xSemaphoreTake(p->output_lock,portMAX_DELAY);exec_output_pop(p->stream,span.len);xSemaphoreGive(p->output_lock);}
    }
    xSemaphoreTake(p->output_lock,portMAX_DELAY);
    bool finish=p->pending_final && (!p->stream->used || expired);
    bool tick=p->stream->used || p->pending_final;
    if(finish){p->pending_final=false;p->stream->incomplete|=p->stream->used!=0;}
    uint64_t total=p->stream->total;bool incomplete=p->stream->incomplete;
    xSemaphoreGive(p->output_lock);
    if(finish){
        cJSON *result=cJSON_Parse(p->record->json);
        if(result){
            cJSON_AddNumberToObject(result,"output_bytes",(double)total);
            cJSON_AddBoolToObject(result,"output_incomplete",incomplete);
            cJSON_AddBoolToObject(result,"collection_truncated",p->collection_truncated);
            cJSON_AddBoolToObject(result,"conditions_complete",!p->conditions_incomplete);
            bool encoded=cJSON_PrintPreallocated(result,p->record->json,sizeof(p->record->json),false);
            cJSON_Delete(result);
            if(!encoded)snprintf(p->record->json,sizeof(p->record->json),"{\"type\":\"ssh_exec_error\",\"session_id\":%lu,\"error\":\"ssh_result_unknown\"}",(unsigned long)p->session_id);
        }
        exec_record_complete(p->record);
        if(ts_ws_op_close_begin(p->op))ts_ws_op_close_finish(p->op,p->record->json);
        tick=false;
    }
    return tick;
}
static bool exec_timer_maintenance(void *data)
{
    ssh_exec_task_params_t *params=data;unsigned phase=params->timer_phase;
    if(phase<2 || phase>=5)return false;
    if(phase==4)return true;
    if(esp_timer_get_time()<params->timer_retry_at)return true;
    BaseType_t ret=pdFAIL;
    if(phase==2) {
        ret=xTimerDelete(params->timer,0);
        if(ret==pdPASS){params->timer=NULL;params->timer_phase=3;}
    } else {
        params->timer_phase=4; /* callback can finish before submit returns */
        ret=xTimerPendFunctionCall(exec_timer_drained,params,0,0);
        if(ret!=pdPASS)params->timer_phase=3;
    }
    if(ret!=pdPASS)params->timer_retry_at=esp_timer_get_time()+100000;
    if(ret!=pdPASS && ++params->timer_retries==TS_WS_QUEUE_RETRIES) {
        params->timer_phase=phase==2?6:7;
        TS_LOGE(TAG,"SSH timer drain retry limit; context retained until stop retry");
        return false;
    }
    if(ret==pdPASS)params->timer_retries=0;
    return params->timer_phase!=5;
}
static bool exec_maintenance(void *data) {
    ssh_exec_task_params_t *p=data;
    bool output=exec_stream_maintenance(p);
    return exec_timer_maintenance(p) || output;
}
static void exec_retry_timer_drain(void)
{
    ts_ws_operation_t *op=ts_ws_op_acquire(TS_WS_OP_EXEC,0);
    if(!op)return;
    ssh_exec_task_params_t *params=ts_ws_op_data(op);
    if(params){unsigned phase=params->timer_phase;
        if(phase>=6){params->timer_retries=0;atomic_compare_exchange_strong(&params->timer_phase,&phase,phase==6?2:3);}}
    ts_ws_op_release(op);
}
static void exec_destroy(void *data)
{
    ssh_exec_task_params_t *params=data;if(!params)return;
    assert(!params->session && (!params->timer_phase || params->timer_phase==5));
    free((void*)params->config.host);free((void*)params->config.username);
    free(params->command);
    if(params->password_owned){memset(params->password_owned,0,strlen(params->password_owned));free(params->password_owned);}
    free(params->expect_pattern);free(params->fail_pattern);free(params->extract_pattern);
    free(params->output_buffer);free(params->extracted_value);
    free(params->matching.data);
    portENTER_CRITICAL(&s_exec_cache_lock);
    if(s_exec_active==params->record)s_exec_active=NULL;
    portEXIT_CRITICAL(&s_exec_cache_lock);
    exec_record_release(params->record);
    free(params->stream);free(params->send_bytes);free(params->send_base64);
    if(params->output_lock)vSemaphoreDelete(params->output_lock);
    vSemaphoreDelete(params->io);free(params);
}

/* SSH Exec 任务 */
static void ssh_exec_task(void *arg)
{
    ssh_exec_task_params_t *params = (ssh_exec_task_params_t *)arg;
    uint32_t session_id = params->session_id;
    int exit_code = -1;
    esp_err_t ret;
    char *key_buf = NULL;
    size_t key_len = 0;
    
    ts_ws_stop_reason_t stop_reason=OP_STOP_NONE;
    if(ts_ws_op_cancelled(params->op)){ret=ESP_ERR_TIMEOUT;goto execution_finished;}
    
    TS_LOGI(TAG, "SSH exec task started: session_id=%lu, cmd=%s", 
            (unsigned long)session_id, params->command);
    
    /* 如果使用密钥认证，加载私钥 */
    if (params->keyid[0] != '\0') {
        ret = ts_keystore_load_private_key(params->keyid, &key_buf, &key_len);
        if (ret == ESP_OK && key_buf) {
            params->config.auth_method = TS_SSH_AUTH_PUBLICKEY;
            params->config.auth.key.private_key = (const uint8_t *)key_buf;
            params->config.auth.key.private_key_len = key_len;
            params->config.auth.key.private_key_path = NULL;
            params->config.auth.key.passphrase = NULL;
        } else {
            stop_reason=ts_ws_op_business_end(params->op,false);
            if(stop_reason){ret=ESP_ERR_TIMEOUT;goto execution_finished;}
            TS_LOGE(TAG, "Failed to load key '%s': %s", params->keyid, esp_err_to_name(ret));
            /* 发送错误消息 */
            if(!params->pending_final) {
        cJSON *msg = cJSON_CreateObject();
        bool encoded=msg!=NULL;
            encoded = (cJSON_AddStringToObject(msg, "type", "ssh_exec_error"))!=NULL && encoded;
            encoded = (cJSON_AddNumberToObject(msg, "session_id", session_id))!=NULL && encoded;
            encoded = (cJSON_AddStringToObject(msg, "error", "key_not_found"))!=NULL && encoded;
            char *json = encoded?cJSON_PrintUnformatted(msg):NULL;
            cJSON_Delete(msg);
            exec_stage_result(params,json); free(json);
        }
            goto cleanup;
        }
    }
    
    if(ts_ws_op_cancelled(params->op)){ret=ESP_ERR_TIMEOUT;goto execution_finished;}

    /* 创建 SSH 会话 */
    ts_ssh_session_t created=NULL;
    ret = ts_ssh_session_create(&params->config, &created);
    xSemaphoreTake(params->io,portMAX_DELAY);params->session=created;xSemaphoreGive(params->io);
    if (ret != ESP_OK) {
        stop_reason=ts_ws_op_business_end(params->op,false);
        if(stop_reason){ret=ESP_ERR_TIMEOUT;goto execution_finished;}
        TS_LOGE(TAG, "Failed to create SSH session: %s", esp_err_to_name(ret));
        if(!params->pending_final) {
        cJSON *msg = cJSON_CreateObject();
        bool encoded=msg!=NULL;
        encoded = (cJSON_AddStringToObject(msg, "type", "ssh_exec_error"))!=NULL && encoded;
        encoded = (cJSON_AddNumberToObject(msg, "session_id", session_id))!=NULL && encoded;
        encoded = (cJSON_AddStringToObject(msg, "error", "no_memory"))!=NULL && encoded;
        char *json = encoded?cJSON_PrintUnformatted(msg):NULL;
        cJSON_Delete(msg);
        exec_stage_result(params,json); free(json);
        }
        goto cleanup;
    }
    
    /* 连接 */
    ret = ts_ssh_connect(params->session);
    if (ret != ESP_OK) {
        stop_reason=ts_ws_op_business_end(params->op,ret==ESP_ERR_TIMEOUT);
        if(stop_reason)goto execution_finished;
        const char *err = ts_ssh_get_error(params->session);
        TS_LOGE(TAG, "SSH connect failed: %s", err ? err : "unknown");
        if(!params->pending_final) {
        cJSON *msg = cJSON_CreateObject();
        bool encoded=msg!=NULL;
        encoded = (cJSON_AddStringToObject(msg, "type", "ssh_exec_error"))!=NULL && encoded;
        encoded = (cJSON_AddNumberToObject(msg, "session_id", session_id))!=NULL && encoded;
        const char *stage = "connect";
        const char *reason = ts_ssh_connect_reason(params->session, &stage);
        encoded = (cJSON_AddStringToObject(msg, "error", reason))!=NULL && encoded;
        encoded = (cJSON_AddStringToObject(msg, "stage", stage))!=NULL && encoded;
        if (err && err[0]) encoded = (cJSON_AddStringToObject(msg, "detail", err))!=NULL && encoded;
        char *json = encoded?cJSON_PrintUnformatted(msg):NULL;
        cJSON_Delete(msg);
        exec_stage_result(params,json); free(json);
        }
        goto cleanup;
    }
    
    if(ts_ws_op_cancelled(params->op)){ret=ESP_ERR_TIMEOUT;goto execution_finished;}

    /* 发送开始消息 */
    {
        ts_ws_output_ticket_t start_ticket;
        if(ts_ws_op_output_begin(params->op,&start_ticket)) {
        if(start_ticket.count) {
        cJSON *msg = cJSON_CreateObject();
        bool encoded=msg!=NULL;
        encoded = (cJSON_AddStringToObject(msg, "type", "ssh_exec_start"))!=NULL && encoded;
        encoded = (cJSON_AddNumberToObject(msg, "session_id", session_id))!=NULL && encoded;
        encoded = (cJSON_AddStringToObject(msg, "command", params->command))!=NULL && encoded;
        /* 添加配置信息 */
        if (params->expect_pattern) {
            encoded = (cJSON_AddStringToObject(msg, "expect_pattern", params->expect_pattern))!=NULL && encoded;
        }
        if (params->fail_pattern) {
            encoded = (cJSON_AddStringToObject(msg, "fail_pattern", params->fail_pattern))!=NULL && encoded;
        }
        if (params->extract_pattern) {
            encoded = (cJSON_AddStringToObject(msg, "extract_pattern", params->extract_pattern))!=NULL && encoded;
        }
        char *json = encoded?cJSON_PrintUnformatted(msg):NULL;
        cJSON_Delete(msg);
        ts_ws_op_output_finish(&start_ticket,json); free(json);
        } else ts_ws_op_output_finish(&start_ticket,NULL);
        }
    }
    
    /* 判断是否需要启动超时定时器 */
    /* 超时在以下情况有效：
     * 1. 设定了超时时间（前端可能为 nohup 命令设置短超时）
     * 2. 设定了成功/失败条件
     * 3. 勾选了匹配后停止 
     */
    bool need_timeout = (params->timeout_ms > 0) ||
                        params->stop_on_match || 
                        (params->expect_pattern && params->expect_pattern[0]) ||
                        (params->fail_pattern && params->fail_pattern[0]);
    
    TS_LOGI(TAG, "Timeout check: timeout_ms=%lu, need_timeout=%d, stop_on_match=%d",
            (unsigned long)params->timeout_ms, need_timeout, params->stop_on_match);
    
    if (need_timeout && params->timeout_ms > 0) {
        ts_ws_op_retain(params->op);
        params->timer=xTimerCreate("ssh_timeout",pdMS_TO_TICKS(params->timeout_ms),pdFALSE,params,ssh_exec_timeout_callback);
        if(params->timer){params->timer_phase=1;
            if(xTimerStart(params->timer,0)!=pdPASS)TS_LOGW(TAG,"SSH timeout timer could not start");
        } else ts_ws_op_release(params->op);
    }

    /* This lock-ordered claim is the conservative irreversible boundary.
     * After it, cancellation is cooperative: remote execution may have begun. */
    if(!ts_ws_op_claim_submit(params->op)){ret=ESP_ERR_TIMEOUT;goto execution_finished;}
    ret = ts_ssh_exec_stream(params->session, params->command,
                              ssh_exec_output_callback, 
                              params,
                              &exit_code);
    
execution_finished:
    TS_WS_TEST_POINT("C04");
    stop_reason=ts_ws_op_business_end(params->op,ret==ESP_ERR_TIMEOUT);
    TS_WS_TEST_POINT("C05");
    xSemaphoreTake(params->io,portMAX_DELAY);params->timer_closing=true;xSemaphoreGive(params->io);
    if(params->timer_phase==1){params->timer_phase=2;ts_ws_subscriptions_wake();}

    /* 处理模式匹配和状态判断 */
    ts_webui_ssh_status_t status = TS_WEBUI_SSH_STATUS_SUCCESS;
    
    /* 检查是否因超时被取消 */
    bool was_timeout = stop_reason==OP_STOP_TIMEOUT;
    
    /* 优先使用实时匹配结果 */
    bool expect_matched = params->expect_matched;
    bool fail_matched = params->fail_matched;
    char *extracted_value = params->extracted_value;
    params->extracted_value = NULL;  /* 转移所有权，避免重复释放 */
    
    if(stop_reason==OP_STOP_USER) {
        status=TS_WEBUI_SSH_STATUS_CANCELLED;
    } else if(stop_reason==OP_STOP_MATCH) {
        status=params->fail_matched?TS_WEBUI_SSH_STATUS_MATCH_FAILED:TS_WEBUI_SSH_STATUS_MATCH_SUCCESS;
    } else if (was_timeout && !params->match_found) {
        /* 超时且未匹配成功 */
        status = TS_WEBUI_SSH_STATUS_TIMEOUT;
        TS_LOGW(TAG, "SSH exec timed out without match");
    } else if (ret == ESP_ERR_TIMEOUT && params->match_found) {
        /* 被中断但已有匹配结果（stop_on_match 触发） */
        if (params->fail_matched) {
            status = TS_WEBUI_SSH_STATUS_MATCH_FAILED;
        } else {
            status = TS_WEBUI_SSH_STATUS_MATCH_SUCCESS;
        }
    } else if (ret == ESP_ERR_TIMEOUT) {
        status = TS_WEBUI_SSH_STATUS_CANCELLED;
    } else if (ret != ESP_OK) {
        status = TS_WEBUI_SSH_STATUS_FAILED;
    } else if (exit_code != 0 && !params->match_found) {
        /* 命令失败且没有实时匹配结果 */
        status = TS_WEBUI_SSH_STATUS_FAILED;
    } else {
        /* 命令完成，确定最终状态 */
        
        /* 如果实时匹配没有找到，再做一次完整匹配 */
        if (!params->match_found) {
            /* 检查失败模式 */
            if (params->fail_pattern && params->fail_pattern[0]) {
                fail_matched = ts_ssh_match_window_match(&params->matching, params->fail_pattern, NULL);
            }
            
            /* 检查期望模式 */
            if (!fail_matched && params->expect_pattern && params->expect_pattern[0]) {
                expect_matched = ts_ssh_match_window_match(&params->matching, params->expect_pattern, NULL);
            }
            
            /* 提取值 */
            if (!extracted_value && params->extract_pattern && params->extract_pattern[0]) {
                ts_ssh_match_window_match(&params->matching, params->extract_pattern, &extracted_value);
            }
        }
        
        /* 确定最终状态 */
        if (fail_matched) {
            status = TS_WEBUI_SSH_STATUS_MATCH_FAILED;
        } else if (params->expect_pattern && params->expect_pattern[0]) {
            status = expect_matched ? TS_WEBUI_SSH_STATUS_MATCH_SUCCESS : TS_WEBUI_SSH_STATUS_MATCH_FAILED;
        } else if (exit_code == 0) {
            status = TS_WEBUI_SSH_STATUS_SUCCESS;
        } else {
            status = TS_WEBUI_SSH_STATUS_FAILED;
        }
    }
    
    /* Retained-display truncation does not invalidate a match/capture already
     * observed by the independent streaming matcher. */
    params->conditions_incomplete = params->collection_truncated &&
        (params->expect_pattern || params->fail_pattern || params->extract_pattern) &&
        !params->match_found && !extracted_value;
    /* 诊断日志：检查变量写入条件 */
    TS_LOGI(TAG, "SSH exec finished: var_name='%s' (has_var=%d), status=%d (is_cancelled=%d), exit_code=%d",
            params->var_name,
            params->var_name[0] ? 1 : 0,
            status,
            status == TS_WEBUI_SSH_STATUS_CANCELLED ? 1 : 0,
            exit_code);
    
    /* 存储结果到变量系统 - 包括取消状态也要更新 */
    if (params->var_name[0] && exec_variables_begin(params)) {
        const char *status_str = "unknown";
        switch (status) {
            case TS_WEBUI_SSH_STATUS_SUCCESS:       status_str = "success"; break;
            case TS_WEBUI_SSH_STATUS_FAILED:        status_str = "failed"; break;
            case TS_WEBUI_SSH_STATUS_TIMEOUT:       status_str = "timeout"; break;
            case TS_WEBUI_SSH_STATUS_MATCH_SUCCESS: status_str = "match_success"; break;
            case TS_WEBUI_SSH_STATUS_MATCH_FAILED:  status_str = "match_failed"; break;
            case TS_WEBUI_SSH_STATUS_CANCELLED:     status_str = "cancelled"; break;
            default: break;
        }
        
        /* 注意：不再调用 ts_var_store_ssh_result()，
         * 因为它使用旧的 ts_var 系统且会把所有值转成字符串。
         * 现在只使用 ts_automation 变量系统 */
        
        /* 同步更新到 ts_automation 变量系统 - 使用 register 确保变量存在并更新 */
        if(params->conditions_incomplete)status_str="unknown";
        TS_LOGI(TAG, "Writing SSH result to automation variables: var_name=%s, status=%s, exit_code=%d",
                params->var_name, status_str, exit_code);
        
        char full_var_name[96];  /* 足够容纳 source_id + suffix */
        ts_auto_variable_t var = {0};
        strncpy(var.source_id, params->var_name, sizeof(var.source_id) - 1);
        var.flags = 0;
        
        /* status (string) */
        snprintf(full_var_name, sizeof(full_var_name), "%s.status", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_STRING;
        strncpy(var.value.str_val, status_str, sizeof(var.value.str_val) - 1);
        ts_variable_upsert(&var);
        
        /* exit_code (int) */
        snprintf(full_var_name, sizeof(full_var_name), "%s.exit_code", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_INT;
        var.value.int_val = exit_code;
        ts_variable_upsert(&var);
        
        /* extracted (string) */
        snprintf(full_var_name, sizeof(full_var_name), "%s.extracted", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_STRING;
        strncpy(var.value.str_val, extracted_value ? extracted_value : "", sizeof(var.value.str_val) - 1);
        ts_variable_upsert(&var);
        
        /* expect_matched (bool) */
        snprintf(full_var_name, sizeof(full_var_name), "%s.expect_matched", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_BOOL;
        var.value.bool_val = expect_matched;
        ts_variable_upsert(&var);
        
        /* fail_matched (bool) */
        snprintf(full_var_name, sizeof(full_var_name), "%s.fail_matched", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_BOOL;
        var.value.bool_val = fail_matched;
        ts_variable_upsert(&var);
        
        /* host (string) */
        snprintf(full_var_name, sizeof(full_var_name), "%s.host", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_STRING;
        strncpy(var.value.str_val, params->config.host ? params->config.host : "", sizeof(var.value.str_val) - 1);
        ts_variable_upsert(&var);
        
        /* timestamp (int) */
        snprintf(full_var_name, sizeof(full_var_name), "%s.timestamp", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_INT;
        var.value.int_val = (int32_t)(esp_timer_get_time() / 1000000);  /* 秒级时间戳 */
        ts_variable_upsert(&var);
        
        TS_LOGI(TAG, "Synced SSH result to automation variables: %s (7 vars)", params->var_name);
        exec_variables_end();
    }
    
    /* 发送完成消息 */
    {
        if(!params->pending_final) {
        cJSON *msg = cJSON_CreateObject();
        bool encoded=msg!=NULL;
        encoded = (cJSON_AddStringToObject(msg, "type",
            status == TS_WEBUI_SSH_STATUS_CANCELLED ? "ssh_exec_cancelled" : "ssh_exec_done"))!=NULL && encoded;
        encoded = (cJSON_AddNumberToObject(msg, "session_id", session_id))!=NULL && encoded;
        
        encoded = (cJSON_AddStringToObject(msg,"execution_status",
            ret==ESP_OK && exit_code>=0 ? (exit_code==0?"success":"failed") : "unknown"))!=NULL && encoded;
        if (status != TS_WEBUI_SSH_STATUS_CANCELLED) {
            encoded = (cJSON_AddNumberToObject(msg, "exit_code", exit_code))!=NULL && encoded;
            
            /* 状态字符串 */
            const char *status_str = "unknown";
            switch (status) {
                case TS_WEBUI_SSH_STATUS_SUCCESS:       status_str = "success"; break;
                case TS_WEBUI_SSH_STATUS_FAILED:        status_str = "failed"; break;
                case TS_WEBUI_SSH_STATUS_TIMEOUT:       status_str = "timeout"; break;
                case TS_WEBUI_SSH_STATUS_MATCH_SUCCESS: status_str = "match_success"; break;
                case TS_WEBUI_SSH_STATUS_MATCH_FAILED:  status_str = "match_failed"; break;
                default: break;
            }
            if(params->conditions_incomplete)status_str="unknown";
            encoded = (cJSON_AddStringToObject(msg, "status", status_str))!=NULL && encoded;
            
            /* 简单的成功标志 */
            bool is_success = !params->conditions_incomplete && (status == TS_WEBUI_SSH_STATUS_SUCCESS ||
                              status == TS_WEBUI_SSH_STATUS_MATCH_SUCCESS);
            encoded = (cJSON_AddBoolToObject(msg, "success", is_success))!=NULL && encoded;
            
            /* 模式匹配结果 */
            if (params->expect_pattern) {
                encoded = (cJSON_AddBoolToObject(msg, "expect_matched", expect_matched))!=NULL && encoded;
            }
            if (params->fail_pattern) {
                encoded = (cJSON_AddBoolToObject(msg, "fail_matched", fail_matched))!=NULL && encoded;
            }
            
            /* 提取的值 */
            if (extracted_value) {
                encoded = (cJSON_AddStringToObject(msg, "extracted", extracted_value))!=NULL && encoded;
            }
            
            /* 错误信息 */
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
                encoded = (cJSON_AddStringToObject(msg, "error", ts_ssh_get_error(params->session)))!=NULL && encoded;
            }
        }
        
        char *json = encoded?cJSON_PrintUnformatted(msg):NULL;
        cJSON_Delete(msg);
        exec_stage_result(params,json); free(json);
        }
    }
    
    /* 释放提取的值 */
    if (extracted_value) {
        free(extracted_value);
    }

cleanup:
    ts_ws_op_business_end(params->op,false);
    /* 清理密钥缓冲区 */
    if (key_buf) {
        memset(key_buf, 0, key_len);
        free(key_buf);
    }
    
    xSemaphoreTake(params->io,portMAX_DELAY);
    params->timer_closing=true;
    if(params->session){ts_ssh_disconnect(params->session);ts_ssh_session_destroy(params->session);params->session=NULL;}
    xSemaphoreGive(params->io);
    if(params->timer_phase==1)params->timer_phase=2;
    ts_ws_op_executor_done(params->op);

    TS_LOGI(TAG, "SSH exec task ended: session_id=%lu", (unsigned long)session_id);
    vTaskDelete(NULL);
}

/* 带选项的扩展版本 */
static esp_err_t ts_webui_ssh_exec_start_ex_impl(ts_ws_operation_t *op, const char *host, uint16_t port,
                                      const char *user, const char *keyid,
                                      const char *password, const char *command,
                                      const ts_webui_ssh_options_t *options,
                                      uint32_t *session_id)
{
    if (!host || !user || !command) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 分配任务参数 */
    ssh_exec_task_params_t *params = heap_caps_calloc(1, sizeof(ssh_exec_task_params_t), 
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!params) params = calloc(1, sizeof(ssh_exec_task_params_t));
    if (!params) {
        return ESP_ERR_NO_MEM;
    }
    
    params->op=op;params->session_id=ts_ws_op_id(op);
    params->io=xSemaphoreCreateMutex();
    if(!params->io){free(params);return ESP_ERR_NO_MEM;}
    ts_ws_op_bind(op,params,exec_destroy);

    params->output_lock=xSemaphoreCreateMutex();
    params->stream=heap_caps_calloc(1,sizeof(exec_output_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    params->send_bytes=heap_caps_malloc(EXEC_OUTPUT_CHUNK,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    params->send_base64=heap_caps_malloc(4*((EXEC_OUTPUT_CHUNK+2)/3)+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    params->record=heap_caps_calloc(1,sizeof(exec_record_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(params->record)params->record->refs=1;
    if(!params->output_lock || !params->stream || !params->send_bytes || !params->send_base64 || !params->record)return ESP_ERR_NO_MEM;
    exec_record_t *record=params->record;record->id=params->session_id;
    if(!s_exec_boot[0])exec_hex_random(s_exec_boot); /* only one exec creator admitted */
    strcpy(record->receipt.boot_id,s_exec_boot);exec_hex_random(record->receipt.result_key);
    if(options && options->receipt)*options->receipt=record->receipt;

    /* 配置 SSH */
    params->config = (ts_ssh_config_t)TS_SSH_DEFAULT_CONFIG();
    params->config.cancelled=ts_ws_op_cancelled;
    params->config.cancel_context=op;
    params->config.host = strdup(host);
    params->config.port = port;
    params->config.username = strdup(user);
    params->config.timeout_ms = (options && options->timeout_ms > 0) ? options->timeout_ms : 30000;

    
    /* The request body is released before the async task runs. Own the full
     * command; truncation can remove shell quoting or change its meaning. */
    size_t command_size = strlen(command) + 1;
    params->command = heap_caps_malloc(command_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!params->command) params->command = malloc(command_size);
    if (!params->command) return ESP_ERR_NO_MEM;
    memcpy(params->command, command, command_size);
    
    /* 配置认证方式 */
    if (keyid && keyid[0]) {
        strncpy(params->keyid, keyid, sizeof(params->keyid) - 1);
    } else if (password && password[0]) {
        params->config.auth_method = TS_SSH_AUTH_PASSWORD;
        params->password_owned=strdup(password);
        params->config.auth.password=params->password_owned;
    } else {
        return ESP_ERR_INVALID_ARG; /* operation destructor owns partially built params */
    }
    if(!params->config.host || !params->config.username ||
       (password && password[0] && !params->keyid[0] && !params->password_owned))return ESP_ERR_NO_MEM;

    /* 配置可选参数 */
    if (options) {
        if (options->command_id) {
            if (strlen(options->command_id) >= sizeof(params->command_id) || !options->command_instance)
                return ESP_ERR_INVALID_ARG;
            strcpy(params->command_id, options->command_id);
            params->command_instance = options->command_instance;
        }
        if (options->expect_pattern && options->expect_pattern[0]) {
            params->expect_pattern = strdup(options->expect_pattern);
        }
        if (options->fail_pattern && options->fail_pattern[0]) {
            params->fail_pattern = strdup(options->fail_pattern);
        }
        if (options->extract_pattern && options->extract_pattern[0]) {
            params->extract_pattern = strdup(options->extract_pattern);
        }
        if (options->var_name && options->var_name[0]) {
            strncpy(params->var_name, options->var_name, sizeof(params->var_name) - 1);
            params->var_name[sizeof(params->var_name) - 1] = '\0';
        }
        params->timeout_ms = options->timeout_ms;
        params->collect_output = options->collect_output;
        params->max_output_size = options->max_output_size > 0 ? options->max_output_size : 64 * 1024;
        params->stop_on_match = options->stop_on_match;
        
        TS_LOGI(TAG, "SSH exec options: timeout_ms=%lu, expect=%s, fail=%s, extract=%s, stop_on_match=%d, var=%s",
                (unsigned long)params->timeout_ms,
                params->expect_pattern ? params->expect_pattern : "(null)",
                params->fail_pattern ? params->fail_pattern : "(null)",
                params->extract_pattern ? params->extract_pattern : "(null)",
                params->stop_on_match,
                params->var_name[0] ? params->var_name : "(none)");
    } else {
        params->collect_output = true;
        params->max_output_size = 64 * 1024;
        params->stop_on_match = false;
    }
    
    if(params->expect_pattern || params->fail_pattern || params->extract_pattern)params->collect_output=true;
    if(params->max_output_size>TS_WS_LEGACY_BYTES || params->max_output_size<2)return ESP_ERR_INVALID_ARG;

    /* 初始化实时匹配状态 */
    params->match_found = false;
    params->expect_matched = false;
    params->fail_matched = false;
    params->extracted_value = NULL;
    
    /* 分配输出缓冲区 */
    if (params->collect_output) {
        params->output_capacity = params->max_output_size;
        params->output_buffer = heap_caps_malloc(params->output_capacity, 
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!params->output_buffer) {
            params->output_buffer = malloc(params->output_capacity);
        }
        if (params->output_buffer) {
            params->output_buffer[0] = '\0';
            params->output_len = 0;
        }
    }
    

    
    if (params->collect_output &&
        (params->expect_pattern || params->fail_pattern || params->extract_pattern)) {
        params->matching.capacity = params->max_output_size;
        params->matching.data = heap_caps_malloc(params->matching.capacity,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!params->matching.data) params->matching.data = malloc(params->matching.capacity);
        if (!params->matching.data) return ESP_ERR_NO_MEM;
        params->matching.data[0] = '\0';
    }
    if(params->collect_output && !params->output_buffer)return ESP_ERR_NO_MEM;

    /* Bind the saved command and result variables atomically with publication. */
    if ((params->command_id[0] || params->var_name[0]) && !ts_variable_is_initialized()) {
        if (params->command_id[0]) return ESP_ERR_NOT_FOUND;
    } else if (params->command_id[0] || params->var_name[0]) {
    ts_variable_registration_begin();
    ts_ssh_binding_lock();
    if (params->command_id[0] && !ts_auto_config_command_current(params->command_id, params->command_instance)) {
        ts_ssh_binding_unlock();
        ts_variable_registration_end();
        return ESP_ERR_NOT_FOUND;
    }
    /* 命令开始时初始化所有变量（重置旧值） */
    TS_LOGI(TAG, "Checking var_name for init: '%s' (len=%d)", 
            params->var_name, (int)strlen(params->var_name));
    if (params->var_name[0]) {
        ts_auto_variable_t var = {0};
        char full_var_name[96];
        
        /* 设置 source_id（所有变量共用） */
        strncpy(var.source_id, params->var_name, sizeof(var.source_id) - 1);
        var.flags = 0;
        
        TS_LOGI(TAG, "Initializing 7 variables for source: %s", params->var_name);
        
        /* 初始化 status 为 "running" */
        snprintf(full_var_name, sizeof(full_var_name), "%s.status", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_STRING;
        strncpy(var.value.str_val, "running", sizeof(var.value.str_val) - 1);
        ts_variable_upsert(&var);
        
        /* 初始化 exit_code 为 -1（未完成） */
        snprintf(full_var_name, sizeof(full_var_name), "%s.exit_code", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_INT;
        var.value.int_val = -1;
        ts_variable_upsert(&var);
        
        /* 初始化 extracted 为空 */
        snprintf(full_var_name, sizeof(full_var_name), "%s.extracted", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_STRING;
        var.value.str_val[0] = '\0';
        ts_variable_upsert(&var);
        
        /* 初始化 expect_matched 为 false */
        snprintf(full_var_name, sizeof(full_var_name), "%s.expect_matched", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_BOOL;
        var.value.bool_val = false;
        ts_variable_upsert(&var);
        
        /* 初始化 fail_matched 为 false */
        snprintf(full_var_name, sizeof(full_var_name), "%s.fail_matched", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_BOOL;
        var.value.bool_val = false;
        ts_variable_upsert(&var);
        
        /* 初始化 host */
        snprintf(full_var_name, sizeof(full_var_name), "%s.host", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_STRING;
        strncpy(var.value.str_val, params->config.host ? params->config.host : "", sizeof(var.value.str_val) - 1);
        ts_variable_upsert(&var);
        
        /* 初始化 timestamp */
        snprintf(full_var_name, sizeof(full_var_name), "%s.timestamp", params->var_name);
        strncpy(var.name, full_var_name, sizeof(var.name) - 1);
        var.value.type = TS_AUTO_VAL_INT;
        var.value.int_val = (int32_t)(esp_timer_get_time() / 1000000);  /* 秒级时间戳 */
        ts_variable_upsert(&var);
        
        TS_LOGI(TAG, "Initialized variables for %s (status=running)", params->var_name);
        for (unsigned i = 0; i < sizeof(exec_variable_suffixes) / sizeof(exec_variable_suffixes[0]); ++i) {
            snprintf(full_var_name, sizeof(full_var_name), "%s.%s", params->var_name, exec_variable_suffixes[i]);
            params->variable_instances[i] = ts_variable_instance(full_var_name);
        }
        params->variables_bound = true;
    }
    ts_ssh_binding_unlock();
    ts_variable_registration_end();
    }
    
    /* 创建任务 - MUST use DRAM stack because keystore access requires NVS.
     * SPI Flash operations disable cache, and PSRAM access requires cache.
     */
    /* Task may finish before task-create returns: creator still owns params. */
    ts_ws_op_set_maintenance(op,exec_maintenance);
    portENTER_CRITICAL(&s_exec_cache_lock);s_exec_active=record;portEXIT_CRITICAL(&s_exec_cache_lock);
    ts_ws_op_executor_take(op);
    ts_ws_op_open(op);
    BaseType_t ret=xTaskCreate(ssh_exec_task,"ssh_exec",8192,params,5,NULL);
    if(ret!=pdPASS){
        ts_ws_op_executor_done(op);
        ts_ws_op_discard(op);
        return ESP_FAIL;
    }

    if (session_id) {
        *session_id = params->session_id;
    }
    
    TS_LOGI(TAG, "SSH exec started (extended): session_id=%lu, host=%s, cmd=%s, expect=%s", 
            (unsigned long)params->session_id, host, command,
            params->expect_pattern ? params->expect_pattern : "(none)");
    
    return ESP_OK;
}

esp_err_t ts_webui_ssh_exec_start_ex(const char *host, uint16_t port,
                                      const char *user, const char *keyid,
                                      const char *password, const char *command,
                                      const ts_webui_ssh_options_t *options,
                                      uint32_t *session_id)
{
    esp_err_t admission;
    ts_ws_operation_t *op=ts_ws_op_create(TS_WS_OP_EXEC,(ts_ws_peer_t){0},&admission);
    if(!op)return admission;
    esp_err_t ret=ts_webui_ssh_exec_start_ex_impl(op,host,port,user,keyid,password,command,options,session_id);
    if(ret!=ESP_OK && ts_ws_op_starting(op))ts_ws_op_discard(op);
    ts_ws_op_executor_done(op);
    return ret;
}

esp_err_t ts_webui_ssh_exec_start(const char *host,uint16_t port,const char *user,
    const char *keyid,const char *password,const char *command,uint32_t *session_id)
{
    return ts_webui_ssh_exec_start_ex(host,port,user,keyid,password,command,NULL,session_id);
}

/* 释放结果结构 */
void ts_webui_ssh_result_free(ts_webui_ssh_result_t *result)
{
    if (!result) return;
    free(result->extracted_value);
    free(result->output);
    free(result->error_msg);
    memset(result, 0, sizeof(*result));
}

esp_err_t ts_webui_ssh_exec_cancel(uint32_t session_id)
{
    ts_ws_operation_t *op=ts_ws_op_cancel_exec(session_id);
    if(!op)return ESP_ERR_INVALID_STATE;
    ssh_exec_task_params_t *params=ts_ws_op_data(op);
    exec_abort(params); /* intent is already persistent, even without a session */
    ts_ws_op_release(op);
    return ESP_OK; /* accepted request, not remote termination confirmation */
}

bool ts_webui_ssh_exec_is_running(uint32_t session_id)
{
    ts_ws_operation_t *op=ts_ws_op_acquire(TS_WS_OP_EXEC,session_id);
    if(!op)return false;
    bool running=ts_ws_op_business_running(op);ts_ws_op_release(op);return running;
}
