/* Exercise the actual WebSocket handler with the existing host adapter. */
#define OP_ADAPTER_NO_MAIN
#define CONFIG_TS_WEBUI_AUTH_REQUIRED 1
#include "test_operation_adapter.c"

static bool session_live = true;

esp_err_t ts_auth_validate_request(const char *header, uint32_t *session, ts_perm_level_t *level)
{
    if (!strcmp(header, "Bearer root-token")) {
        *session = 7; *level = TS_PERM_ROOT; return ESP_OK;
    }
    if (!strcmp(header, "Bearer admin-token")) {
        *session = 8; *level = TS_PERM_ADMIN; return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

bool ts_security_check_permission(uint32_t session, ts_perm_level_t required)
{
    return session_live && session == 7 && required == TS_PERM_ROOT;
}

static void send_message(int fd, const char *message)
{
    reqs[fd].method = 1;
    incoming = message;
    current = (void *)3;
    assert(ws_handler(&reqs[fd]) == ESP_OK);
    current = (void *)1;
}

int main(void)
{
    cJSON_Hooks hooks = {tracked_malloc, tracked_free};
    cJSON_InitHooks(&hooks);
    setup();
    send_message(1, "{\"type\":\"terminal_start\"}");
    assert(s_terminal_client_fd < 0 && strstr(payloads[1], "Unauthorized"));
    send_message(1, "{\"type\":\"terminal_start\",\"token\":\"admin-token\"}");
    assert(s_terminal_client_fd < 0 && strstr(payloads[1], "Unauthorized"));
    send_message(1, "{\"type\":\"terminal_start\",\"token\":\"root-token\"}");
    assert(s_terminal_client_fd == 1 && strstr(payloads[1], "connected"));
    send_message(2, "{\"type\":\"terminal_start\"}");
    assert(s_terminal_client_fd == 1 && strstr(payloads[2], "Unauthorized"));
    send_message(2, "{\"type\":\"terminal_input\",\"data\":\"cat /sdcard/config/automation/state.json\"}");
    assert(strstr(payloads[2], "Unauthorized"));
    session_live = false;
    send_message(1, "{\"type\":\"terminal_input\",\"data\":\"help\"}");
    assert(strstr(payloads[1], "Unauthorized"));
    send_message(1, "{\"type\":\"terminal_stop\"}");
    assert(s_terminal_client_fd < 0);
    finish_all();
    puts("PASS terminal starts only for root and checks the live session before commands");
}
