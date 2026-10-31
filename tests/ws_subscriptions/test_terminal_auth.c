/* Actual WS handler, controller and transport; only credentials/remote I/O fake. */
#define OP_ADAPTER_NO_MAIN
#define CONFIG_TS_WEBUI_AUTH_REQUIRED 1
#include "test_operation_adapter.c"

static bool session_live = true;
static ts_perm_level_t admin_level = TS_PERM_ADMIN;
esp_err_t ts_security_validate_session(uint32_t sid, ts_session_t *out)
{
    if (!session_live || (sid != 7 && sid != 8)) return ESP_ERR_NOT_FOUND;
    if (out) *out = (ts_session_t){.session_id=sid, .level=sid==7?TS_PERM_ROOT:admin_level};
    return ESP_OK;
}
esp_err_t ts_auth_validate_request(const char *header, uint32_t *sid, ts_perm_level_t *level)
{
    if (!strcmp(header,"Bearer root-token")) *sid=7;
    else if (!strcmp(header,"Bearer admin-token")) *sid=8;
    else return ESP_ERR_NOT_FOUND;
    ts_session_t session;
    if (ts_security_validate_session(*sid,&session)!=ESP_OK) return ESP_ERR_NOT_FOUND;
    *level=session.level;return ESP_OK;
}
static void send_message(int fd, const char *message)
{
    reqs[fd].method=1;incoming=message;current=(void *)3;
    assert(ws_handler(&reqs[fd])==ESP_OK);current=(void *)1;incoming=NULL;
}
static ts_ws_peer_t peer(int fd) {return *(ts_ws_peer_t *)reqs[fd].sess_ctx;}
static unsigned private_frames,auth_errors,private_by_fd[32];
static void count_private(httpd_ws_frame_t *frame)
{
    cJSON *msg=cJSON_ParseWithLength((char *)frame->payload,frame->len);
    cJSON *type=cJSON_GetObjectItem(msg,"type");
    if(cJSON_IsString(type)) {
        if(!strcmp(type->valuestring,"auth_error"))auth_errors++;
        if(!strcmp(type->valuestring,"output")||!strcmp(type->valuestring,"done")||
           !strcmp(type->valuestring,"ssh_output")||!strncmp(type->valuestring,"ssh_exec_",9)){private_frames++;private_by_fd[sent_fd]++;}
    }
    cJSON_Delete(msg);
}
static void revoke(void) {session_live=false;}
static void revoke_after_output(httpd_ws_frame_t *frame) {
    count_private(frame);
    if(strstr((char *)frame->payload,"\"type\":\"output\""))session_live=false;
}
static void close_exec(ts_ws_operation_t *op) {
    assert(ts_ws_op_close_begin(op));ts_ws_op_close_finish(op,"{\"type\":\"ssh_exec_done\"}");
    ts_ws_op_executor_done(op);finish_all();
}
static void identity_and_delivery(void)
{
    setup();sent_hook=count_private;
    send_message(1,"{\"type\":\"auth\",\"token\":null}");
    assert(strstr(payloads[1],"auth_error")&&!ts_ws_peer_authorized(peer(1),TS_PERM_READ));
    send_message(1,"{\"type\":\"auth\",\"token\":\"admin-token\",\"level\":4}");
    assert(strstr(payloads[1],"auth_ok") && !ts_ws_peer_authorized(peer(1),TS_PERM_ROOT));
    send_message(1,"{\"type\":\"auth\",\"token\":\"admin-token\"}");assert(strstr(payloads[1],"auth_ok"));
    send_message(1,"{\"type\":\"auth\",\"token\":\"root-token\"}");assert(strstr(payloads[1],"auth_error"));
    send_message(2,"{\"type\":\"auth\",\"token\":\"root-token\"}");
    esp_err_t error;ts_ws_operation_t *op=ts_ws_op_create(TS_WS_OP_EXEC,(ts_ws_peer_t){0},&error);
    assert(op&&ts_ws_op_open(op));ts_ws_output_ticket_t ticket;
    assert(ts_ws_op_output_begin(op,&ticket)&&ticket.count==2);
    unsigned before=private_frames,errors=auth_errors;
    ts_ws_transport_stats_t tx_before,tx_after;ts_ws_transport_get_stats(&tx_before);
    ts_ws_op_output_finish(&ticket,"{\"type\":\"ssh_exec_output\",\"data\":\"private\"}");
    session_live=false;pump();
    ts_ws_op_stats_t stats;ts_ws_op_stats(TS_WS_OP_EXEC,&stats);
    assert(!stats.failed&&!stats.unsettled&&private_frames==before&&auth_errors==errors+2);
    ts_ws_transport_get_stats(&tx_after);assert(tx_after.attempts==tx_before.attempts&&tx_after.success==tx_before.success);
    current=(void*)3;assert(!ts_ws_peer_authorized(peer(1),TS_PERM_READ));current=(void*)1;
    assert(auth_errors==errors+2); /* only one expiry notice per previous binding */
    session_live=true;
    send_message(1,"{\"type\":\"auth\",\"token\":\"root-token\"}");assert(strstr(payloads[1],"auth_error"));
    /* Public traffic survives expiry; a recycled private pool slot resets NONE. */
    httpd_ws_frame_t public_frame={.type=1,.payload=(uint8_t*)"{\"type\":\"data\"}",.len=15};
    assert(ts_ws_transport_send(peer(1).server,1,&public_frame)==ESP_OK);pump();assert(strstr(payloads[1],"data"));
    close_exec(op);

    setup();send_message(1,"{\"type\":\"auth\",\"token\":\"admin-token\"}");
    admin_level=TS_PERM_READ;assert(ts_ws_peer_authorized(peer(1),TS_PERM_READ));assert(!ts_ws_peer_authorized(peer(1),TS_PERM_ADMIN));admin_level=TS_PERM_ADMIN;
    ts_ws_peer_t old=peer(1);ts_ws_message_t *m=ts_ws_message_text("private",7);ts_ws_message_require(m,TS_PERM_READ);
    assert(ts_ws_transport_submit(old,m,0,0,NULL,NULL)==ESP_OK);ts_ws_message_release(m);
    close_peer(1);ts_ws_peer_t fresh=open_peer(1);before=frames[1];pump();assert(frames[1]==before);
    assert(!ts_ws_peer_authorized(fresh,TS_PERM_READ));assert(ts_ws_peer_bind_session(old,7)!=ESP_OK);
    assert(ts_ws_peer_bind_session(fresh,7)==ESP_OK);finish_all();
    setup();send_message(1,"{\"type\":\"auth\",\"token\":\"admin-token\"}");
    send_message(2,"{\"type\":\"auth\",\"token\":\"root-token\"}");
    op=ts_ws_op_create(TS_WS_OP_EXEC,(ts_ws_peer_t){0},&error);assert(op&&ts_ws_op_open(op));
    assert(ts_ws_op_output_begin(op,&ticket)&&ticket.count==2);
    send_fail_fd=1;ts_ws_op_output_finish(&ticket,"{\"type\":\"ssh_exec_output\"}");pump();send_fail_fd=-1;
    ts_ws_op_stats(TS_WS_OP_EXEC,&stats);assert(stats.failed);
    ts_ws_op_executor_done(op);finish_all();
    puts("PASS WS token binding/roles, idempotence, one-binding history, queued revocation, public reuse and complete-peer fd reuse");
}
static void heartbeat_expiry(void)
{
    setup();sent_hook=count_private;open_peer(3);
    send_message(1,"{\"type\":\"auth\",\"token\":\"root-token\"}");
    send_message(2,"{\"type\":\"auth\",\"token\":\"admin-token\"}");
    unsigned errors=auth_errors;
    admin_level=TS_PERM_READ;send_message(2,"{\"type\":\"ping\"}");
    assert(auth_errors==errors&&strstr(payloads[2],"pong")&&ts_ws_peer_authorized(peer(2),TS_PERM_READ));admin_level=TS_PERM_ADMIN;
    esp_err_t error;ts_ws_operation_t *op=ts_ws_op_create(TS_WS_OP_EXEC,(ts_ws_peer_t){0},&error);assert(op&&ts_ws_op_open(op));
    session_live=false;unsigned before=frames[1];ts_ws_output_ticket_t ticket;
    assert(ts_ws_op_output_begin(op,&ticket)&&ticket.count==0);ts_ws_op_output_finish(&ticket,NULL);
    assert(frames[1]==before&&auth_errors==errors); /* no empty private message */
    send_message(1,"{\"type\":\"ping\"}");assert(frames[1]==before+2&&strstr(payloads[1],"pong")&&auth_errors==errors+1);
    send_message(1,"{\"type\":\"ping\"}");send_message(3,"{\"type\":\"ping\"}");
    assert(auth_errors==errors+1&&strstr(payloads[1],"pong")&&strstr(payloads[3],"pong"));
    send_message(1,"{\"type\":\"subscribe\",\"topic\":\"system.cpu\",\"params\":{\"interval\":1000}}");
    assert(strstr(payloads[1],"subscribed")&&strstr(payloads[1],"true"));
    close_exec(op);session_live=true;
    puts("PASS existing public heartbeat emits one revoked-binding notice after zero-recipient filtering; repeat/anonymous pong and valid READ/public subscription preserved");
}
static void terminal_and_shell(void)
{
    setup();sent_hook=count_private;
    send_message(1,"{\"type\":\"terminal_start\"}");assert(s_terminal_client_fd<0);
    send_message(1,"{\"type\":\"terminal_start\",\"token\":\"admin-token\"}");assert(s_terminal_client_fd<0);
    send_message(1,"{\"type\":\"terminal_start\",\"token\":\"root-token\"}");assert(s_terminal_client_fd==1);
    s_output_mutex=xSemaphoreCreateMutex();s_terminal_output_buf=tracked_malloc(TERMINAL_OUTPUT_BUF_SIZE);
    unsigned before=private_frames;
    send_message(1,"{\"type\":\"terminal_input\",\"data\":\"help\"}");assert(private_frames==before+2);
    console_hook=revoke;before=private_frames;
    send_message(1,"{\"type\":\"terminal_input\",\"data\":\"help\"}");assert(private_frames==before);console_hook=NULL;
    send_message(1,"{\"type\":\"terminal_stop\"}");assert(s_terminal_client_fd<0);
    session_live=true;
    /* New connection: expiry cleared the old binding and forbids rebinding it. */
    close_peer(1);current=(void*)3;reqs[1]=(httpd_req_t){.handle=(void*)11,.fd=1};assert(add_client(&reqs[1],WS_CLIENT_TYPE_EVENT)==ESP_OK);current=(void*)1;
    send_message(1,"{\"type\":\"terminal_start\",\"token\":\"root-token\"}");sent_hook=revoke_after_output;before=private_frames;
    send_message(1,"{\"type\":\"terminal_input\",\"data\":\"help\"}");assert(private_frames==before+1); /* done rechecks after output */
    send_message(1,"{\"type\":\"terminal_stop\"}");session_live=true;
    tracked_free(s_terminal_output_buf);s_terminal_output_buf=NULL;vSemaphoreDelete(s_output_mutex);s_output_mutex=NULL;finish_all();

    setup();sent_hook=count_private;
    send_message(1,"{\"type\":\"ssh_connect\",\"token\":\"admin-token\",\"host\":\"fake\",\"user\":\"u\"}");assert(!ts_ws_op_busy());
    send_message(1,"{\"type\":\"ssh_connect\",\"token\":\"root-token\",\"host\":\"fake\",\"user\":\"u\"}");assert(ts_ws_op_busy());
    unsigned writes=control_writes;
    send_message(2,"{\"type\":\"auth\",\"token\":\"root-token\"}");send_message(2,"{\"type\":\"ssh_input\",\"data\":\"no\"}");assert(control_writes==writes);
    send_message(1,"{\"type\":\"ssh_input\",\"data\":\"yes\"}");assert(control_writes==writes+1);
    ssh_shell_context_t *ctx=shell_task.arg;
    current=(void*)3;session_live=false;before=private_frames;
    assert(ssh_start_status(ctx,"connected","private")==ESP_ERR_INVALID_STATE); /* already OPEN */
    httpd_ws_frame_t f={.type=1,.payload=(uint8_t*)"private",.len=7};
    assert(ts_ws_transport_send_private(peer(1),&f,TS_PERM_ROOT)==ESP_ERR_NOT_ALLOWED);assert(private_frames==before);
    current=(void*)1;shell_task.fn(shell_task.arg);session_live=true;finish_all();
    puts("PASS Root terminal normal output/done and expiry at both actual sends; Shell token/complete-peer owner and sync private send");
}
static void zero_observers(void)
{
    for(unsigned observers=0;observers<2;observers++) {
        setup();sent_hook=count_private;open_peer(3);
        if(observers) {
            send_message(1,"{\"type\":\"auth\",\"token\":\"admin-token\"}");
            send_message(2,"{\"type\":\"auth\",\"token\":\"root-token\"}");
        }
        if(observers)admin_level=TS_PERM_READ;
        ts_webui_ssh_options_t options={.collect_output=true,.extract_pattern="value=(.*)",.var_name="fake"};
        unsigned writes=variable_writes,submitted=commands_submitted,before=private_frames,old_aborts=aborts;
        unsigned anon=private_by_fd[3],admin=private_by_fd[1],root=private_by_fd[2];uint32_t id;stored_extract[0]=0;
        assert(ts_webui_ssh_exec_start_ex("fake",22,"u",NULL,"p","cmd",&options,&id)==ESP_OK);
        exec_task.fn(exec_task.arg);finish_all();
        assert(variable_writes>writes&&commands_submitted==submitted+1&&aborts==old_aborts&&!strcmp(stored_extract,"42"));
        assert(private_by_fd[3]==anon);
        if(observers)assert(private_by_fd[1]>admin&&private_by_fd[2]>root);
        else assert(private_frames==before);
        admin_level=TS_PERM_ADMIN;
    }
    puts("PASS anonymous receives no private frames; normal READ/Admin+Root exec and zero observers both match/store 42 and finish without abort");
}

int main(void)
{
    cJSON_Hooks hooks={tracked_malloc,tracked_free};cJSON_InitHooks(&hooks);
    identity_and_delivery();heartbeat_expiry();terminal_and_shell();zero_observers();
}
