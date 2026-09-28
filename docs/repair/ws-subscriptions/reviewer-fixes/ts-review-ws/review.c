#define main original_manager_main
#include "/Users/massif/TianshanOS/tests/ws_subscriptions/test_manager.c"
#undef main
static bool s_ws_stopping, s_exec_running, s_ssh_poll_alive;
static unsigned s_exec_creators;
static ts_event_handler_handle_t s_power_event_handle;
static ts_ws_peer_t s_ssh_peer;
static int s_ssh_client_fd;
static httpd_handle_t s_server=(void*)11;
static void ssh_cleanup(void) {}
esp_err_t ts_webui_ws_stop(httpd_handle_t server)
{
    if (ts_ws_transport_in_context() || ts_event_in_callback()) return ESP_ERR_INVALID_STATE;
    s_ws_stopping = true;
    esp_err_t ret = ts_ws_subscriptions_deinit();
    if (ret != ESP_OK) return ret;
    ret = ts_ws_transport_quiesce(server, 6000);
    if (ret != ESP_OK) return ret;
    if (s_power_event_handle) {
        ret = ts_event_unregister_sync(s_power_event_handle, 6000);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) return ret;
        s_power_event_handle = NULL;
    }
    /* Do not destroy a running remote operation or replay it. Keep HTTPD alive
     * and return busy; the control caller can retry after its normal completion. */
    if (s_exec_creators || s_exec_running) return ESP_ERR_INVALID_STATE;
    ssh_cleanup();
    if (s_ssh_poll_alive) return ESP_ERR_TIMEOUT;
    return ts_ws_transport_stop(server, 6000);
}
#define free tracked_free
static void ssh_send_status(const char *status, const char *message)
{
    if (s_ssh_client_fd < 0 || !s_server) return;

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "ssh_status");
    cJSON_AddStringToObject(msg, "status", status);
    if (message) {
        cJSON_AddStringToObject(msg, "message", message);
    }
    
    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    
    if (json) {
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len = strlen(json)
        };
        ts_ws_message_t *owned = ts_ws_message_text((const char *)ws_pkt.payload, ws_pkt.len);
        if (owned) {
            ts_ws_transport_submit(s_ssh_peer, owned, 0, 0, NULL, NULL);
            ts_ws_message_release(owned);
        }
        free(json);
    }
}

#undef free

#define MAX_WS_CLIENTS TS_WS_CONNECTIONS
enum { WS_CLIENT_TYPE_EVENT, WS_CLIENT_TYPE_TERMINAL, WS_CLIENT_TYPE_SSH_SHELL, WS_CLIENT_TYPE_LOG };
static struct {bool active;int fd;httpd_handle_t hd;int type;} s_clients[MAX_WS_CLIENTS];
static int s_terminal_client_fd=-1;
static void ts_console_clear_output_cb(void){}
static void terminal_output_cb(const char *d,size_t n,void *u){(void)d;(void)n;(void)u;}
static void ts_console_set_output_cb(void(*fn)(const char*,size_t,void*),void*u){(void)fn;(void)u;}
static void cleanup_disconnected_client(int fd){(void)fd;}
static int httpd_ws_send_frame(httpd_req_t*r,httpd_ws_frame_t*f){return httpd_ws_send_frame_async(r->handle,r->fd,f);}
#define free tracked_free
static void start_terminal_session(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    
    // 检查是否有其他终端会话
    if (s_terminal_client_fd >= 0 && s_terminal_client_fd != fd) {
        // 检查旧的终端 fd 是否还在活跃客户端列表中
        bool old_fd_active = false;
        httpd_handle_t old_hd = NULL;
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_clients[i].active && s_clients[i].fd == s_terminal_client_fd) {
                old_fd_active = true;
                old_hd = s_clients[i].hd;
                break;
            }
        }
        
        // 如果旧的 fd 已经不活跃（例如页面刷新），强制清理
        if (!old_fd_active) {
            TS_LOGW(TAG, "Terminal session orphaned (fd=%d), cleaning up", s_terminal_client_fd);
            ts_console_clear_output_cb();
            s_terminal_client_fd = -1;
        } else {
            // 旧会话仍在列表中
            // 采用宽松策略：新的终端请求优先，主动关闭旧会话
            // 原因：通常是用户刷新页面或网络重连，旧会话应该让位
            TS_LOGI(TAG, "Terminal takeover: closing old session (fd=%d) for new request (fd=%d)", 
                    s_terminal_client_fd, fd);
            
            // 向旧会话发送关闭通知
            cJSON *close_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(close_msg, "type", "session_closed");
            cJSON_AddStringToObject(close_msg, "reason", "Another terminal session requested");
            char *close_json = cJSON_PrintUnformatted(close_msg);
            cJSON_Delete(close_msg);
            
            if (close_json) {
                httpd_ws_frame_t ws_pkt = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)close_json,
                    .len = strlen(close_json)
                };
                ts_ws_transport_send(old_hd, s_terminal_client_fd, &ws_pkt);
                free(close_json);
            }
            
            // 清理旧会话
            cleanup_disconnected_client(s_terminal_client_fd);
            s_terminal_client_fd = -1;
            
            // 短暂延迟确保关闭消息发送
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    
    // 设置为终端客户端
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            s_clients[i].type = WS_CLIENT_TYPE_TERMINAL;
            break;
        }
    }
    
    // 设置输出回调
    s_terminal_client_fd = fd;
    ts_console_set_output_cb(terminal_output_cb, NULL);
    
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
        httpd_ws_send_frame(req, &ws_pkt);
        free(json);
    }
    
    TS_LOGD(TAG, "Terminal session started (fd=%d)", fd);
}
#undef free

static void sdk_one(void) {
 if(!queued)return;
 current=(void*)3;typeof(queue[0]) q=queue[0];memmove(queue,queue+1,--queued*sizeof(queue[0]));q.fn(q.arg);current=(void*)1;
}
static void stop_progress(void){worker_exit();sdk_one();}
int main(void) {
 cJSON_Hooks hooks={tracked_malloc,tracked_free};cJSON_InitHooks(&hooks);
 start();s_ssh_peer=open_peer(1);s_ssh_client_fd=1;open_peer(2);
 s_exec_running=true;delay_hook=stop_progress;
 int ret=ts_webui_ws_stop((void*)11);delay_hook=NULL;
 assert(ret==ESP_ERR_INVALID_STATE && !s_worker && s_state==OFF);
 httpd_ws_frame_t f={.payload=(uint8_t*)"{}",.len=2};
 assert(ts_ws_transport_broadcast(&f)==ESP_OK);
 sdk_one();
 assert(queued==0 && ts_ws_transport_needs_flush() && frames[1]==1 && frames[2]==0);
 printf("CONFIRMED R1: stop returned busy; worker absent; accepted two-recipient message sent to only one; pending jobs=%u\n",s_tx_stats.jobs);
 pump();s_exec_running=false;stop();
 start();s_ssh_peer=open_peer(1);s_ssh_client_fd=1;
 unsigned before=frames[1];
 current=(void*)3;
 ssh_send_status("connecting", "Connecting to SSH server...");
 ssh_send_status("connected", "SSH shell ready");
 ssh_send_status("error", "Failed to create SSH session");
 current=(void*)1;pump();
 assert(frames[1]==before+2 && strstr(payloads[1],"connected"));
 printf("CONFIRMED R2: three real ssh_send_status calls produce two frames; final frame=%s\n",payloads[1]);
 stop();assert(live_allocations==0);
 start();ts_ws_peer_t peer=open_peer(1);
 s_clients[0].active=true;s_clients[0].fd=1;s_clients[0].hd=peer.server;s_clients[0].type=WS_CLIENT_TYPE_LOG;
 ts_ws_peer_log_level(peer,5);
 httpd_ws_frame_t log={.payload=(uint8_t*)"old-log",.len=7};assert(ts_ws_transport_log(&log,3)==0);
 current=(void*)3;start_terminal_session(&reqs[1]);current=(void*)1;
 assert(s_clients[0].type==WS_CLIENT_TYPE_TERMINAL);
 pump();assert(!strcmp(payloads[1],"old-log"));
 assert(ts_ws_transport_log(&log,3)==0);pump();assert(!strcmp(payloads[1],"old-log"));
 puts("CONFIRMED R3: actual terminal_start changes client role, but both queued and new log frames still reach this peer");
 stop();assert(live_allocations==0);
 return 0;
}
