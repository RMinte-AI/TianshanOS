/**
 * @file ts_ssh_shell.c
 * @brief SSH Interactive Shell implementation using libssh2
 *
 * 实现交互式 Shell 功能：
 * - PTY（伪终端）分配
 * - 远程 Shell 启动
 * - 双向数据传输
 * - 终端窗口大小调整
 */

#include "ts_ssh_shell.h"
#include "ts_ssh_client.h"
#include "ts_core.h"  /* TS_CALLOC_PSRAM */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <libssh2.h>

#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>

static const char *TAG = "ts_shell";

/* ============================================================================
 * 内部数据结构
 * ============================================================================ */

/** Shell 内部结构 */
struct ts_ssh_shell_s {
    ts_ssh_session_t ssh_session;       /**< SSH 会话 */
    LIBSSH2_CHANNEL *channel;           /**< SSH 通道 */
    ts_shell_config_t config;           /**< Shell 配置 */
    ts_shell_state_t state;             /**< 当前状态 */
    bool close_notified;
    int exit_code;                      /**< 退出码 */
    
    /* 回调 */
    ts_shell_output_cb_t output_cb;
    void *output_cb_data;
    ts_shell_close_cb_t close_cb;
    void *close_cb_data;
    
    /* 读取缓冲区 */
    char read_buffer[1024];
};

/* 外部声明（来自 ts_ssh_client.c） */
extern LIBSSH2_SESSION *ts_ssh_get_libssh2_session(ts_ssh_session_t session);
extern int ts_ssh_get_socket(ts_ssh_session_t session);

/* ============================================================================
 * 内部辅助函数
 * ============================================================================ */

/**
 * @brief 获取终端类型字符串
 */
static const char *get_term_string(ts_term_type_t type)
{
    switch (type) {
        case TS_TERM_XTERM:  return "xterm";
        case TS_TERM_VT100:  return "vt100";
        case TS_TERM_VT220:  return "vt220";
        case TS_TERM_ANSI:   return "ansi";
        case TS_TERM_DUMB:   return "dumb";
        default:             return "xterm";
    }
}

/**
 * @brief 等待 socket 就绪
 */
static int wait_socket(int sock, LIBSSH2_SESSION *session, int timeout_ms)
{
    struct timeval timeout;
    fd_set fd;
    fd_set *readfd = NULL;
    fd_set *writefd = NULL;
    int dir;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    FD_ZERO(&fd);
    FD_SET(sock, &fd);

    dir = libssh2_session_block_directions(session);

    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) {
        readfd = &fd;
    }
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) {
        writefd = &fd;
    }

    return select(sock + 1, readfd, writefd, NULL, &timeout);
}

/* A libssh2 request left in EAGAIN cannot be replaced by a different request.
 * Bound the wait and retire the channel on uncertainty instead of replaying. */
static bool shell_wait(int sock,LIBSSH2_SESSION *ssh,int64_t deadline)
{
    int64_t left=deadline-esp_timer_get_time();
    if(left<=0)return false;
    int ms=(int)((left+999)/1000);if(ms>100)ms=100;
    return wait_socket(sock,ssh,ms)>=0 && esp_timer_get_time()<deadline;
}
static void shell_release_channel(ts_ssh_shell_t shell)
{
    if(!shell->channel)return;
    /* libssh2 retains ownership only on EAGAIN. Its session owns that channel
     * and the client's local-disconnect callbacks make destruction nonwaiting. */
    if(libssh2_channel_free(shell->channel)==LIBSSH2_ERROR_EAGAIN)
        ts_ssh_disconnect(shell->ssh_session);
    shell->channel=NULL;
}
static void shell_notify_close(ts_ssh_shell_t shell)
{
    if(!shell->close_notified){shell->close_notified=true;
        if(shell->close_cb)shell->close_cb(shell->exit_code,shell->close_cb_data);}
}

/* ============================================================================
 * 公开 API 实现
 * ============================================================================ */

esp_err_t ts_ssh_shell_open(ts_ssh_session_t session,
                             const ts_shell_config_t *config,
                             ts_ssh_shell_t *shell_out)
{
    if (!session || !shell_out) {
        return ESP_ERR_INVALID_ARG;
    }

    *shell_out=NULL;
    if (!ts_ssh_is_connected(session)) {
        ESP_LOGE(TAG, "SSH session not connected");
        return ESP_ERR_INVALID_STATE;
    }

    LIBSSH2_SESSION *ssh = ts_ssh_get_libssh2_session(session);
    int sock = ts_ssh_get_socket(session);

    /* 分配 Shell 结构 */
    ts_ssh_shell_t shell = TS_CALLOC_PSRAM(1, sizeof(struct ts_ssh_shell_s));
    if (!shell) {
        return ESP_ERR_NO_MEM;
    }

    shell->ssh_session = session;
    shell->state = TS_SHELL_STATE_IDLE;
    shell->exit_code = -1;

    /* 使用默认配置或用户配置 */
    if (config) {
        shell->config = *config;
    } else {
        ts_shell_config_t default_config = TS_SHELL_DEFAULT_CONFIG();
        shell->config = default_config;
    }

    /* 设置默认值 */
    if (shell->config.term_width == 0) {
        shell->config.term_width = 80;
    }
    if (shell->config.term_height == 0) {
        shell->config.term_height = 24;
    }
    if (shell->config.read_timeout_ms == 0) {
        shell->config.read_timeout_ms = 100;
    }

    /* 打开通道 */
    LIBSSH2_CHANNEL *channel = NULL;
    int rc;
    int64_t deadline=esp_timer_get_time()+10000000;

    while ((channel = libssh2_channel_open_session(ssh)) == NULL &&
           libssh2_session_last_errno(ssh) == LIBSSH2_ERROR_EAGAIN) {
        if(!shell_wait(sock,ssh,deadline))goto setup_timeout;
    }

    if (!channel) {
        char *err_msg = NULL;
        libssh2_session_last_error(ssh, &err_msg, NULL, 0);
        ESP_LOGE(TAG, "Failed to open channel: %s", err_msg ? err_msg : "unknown");
        free(shell);
        return ESP_FAIL;
    }

    shell->channel=channel;
    /* 请求 PTY */
    const char *term = get_term_string(shell->config.term_type);
    
    while ((rc = libssh2_channel_request_pty_ex(channel, 
                                                 term, strlen(term),
                                                 NULL, 0,  /* 终端模式（使用默认） */
                                                 shell->config.term_width,
                                                 shell->config.term_height,
                                                 0, 0)) == LIBSSH2_ERROR_EAGAIN) {
        if(!shell_wait(sock,ssh,deadline))goto setup_timeout;
    }

    if (rc != 0) {
        char *err_msg = NULL;
        libssh2_session_last_error(ssh, &err_msg, NULL, 0);
        ESP_LOGE(TAG, "Failed to request PTY: %s", err_msg ? err_msg : "unknown");
        shell_release_channel(shell);
        free(shell);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "PTY allocated: %s %dx%d", term, 
             shell->config.term_width, shell->config.term_height);

    /* 启动 Shell */
    while ((rc = libssh2_channel_shell(channel)) == LIBSSH2_ERROR_EAGAIN) {
        if(!shell_wait(sock,ssh,deadline))goto setup_timeout;
    }

    if (rc != 0) {
        char *err_msg = NULL;
        libssh2_session_last_error(ssh, &err_msg, NULL, 0);
        ESP_LOGE(TAG, "Failed to start shell: %s", err_msg ? err_msg : "unknown");
        shell_release_channel(shell);
        free(shell);
        return ESP_FAIL;
    }

    shell->channel = channel;
    shell->state = TS_SHELL_STATE_RUNNING;

    *shell_out = shell;
    ESP_LOGI(TAG, "Interactive shell opened");
    return ESP_OK;
setup_timeout:
    shell_release_channel(shell);
    ts_ssh_disconnect(session); /* pending open/setup must not survive as a usable session */
    free(shell);
    return ESP_ERR_TIMEOUT;
}

esp_err_t ts_ssh_shell_request_close(ts_ssh_shell_t shell)
{
    if (!shell) return ESP_ERR_INVALID_ARG;
    if (shell->state == TS_SHELL_STATE_RUNNING) shell->state = TS_SHELL_STATE_CLOSED;
    return ESP_OK;
}

esp_err_t ts_ssh_shell_close(ts_ssh_shell_t shell)
{
    if (!shell) {
        return ESP_ERR_INVALID_ARG;
    }

    /* EOF/ERROR changes state, not ownership. All allocated handles are
     * consumed here; callers clear their handle after this call. */
    if(shell->channel && shell->state==TS_SHELL_STATE_RUNNING)
        shell->exit_code=libssh2_channel_get_exit_status(shell->channel);
    shell->state=TS_SHELL_STATE_CLOSED;
    shell_release_channel(shell);
    shell_notify_close(shell);
    free(shell);
    return ESP_OK;
}

esp_err_t ts_ssh_shell_write(ts_ssh_shell_t shell,const char *data,size_t len,size_t *written)
{
    if(written)*written=0;
    if(!shell || !data || !len)return ESP_ERR_INVALID_ARG;
    if(shell->state!=TS_SHELL_STATE_RUNNING)return ESP_ERR_INVALID_STATE;
    LIBSSH2_SESSION *ssh=ts_ssh_get_libssh2_session(shell->ssh_session);
    int sock=ts_ssh_get_socket(shell->ssh_session);
    int64_t deadline=esp_timer_get_time()+1000000;
    size_t total=0;
    while(total<len){
        int rc=libssh2_channel_write(shell->channel,data+total,len-total);
        if(rc>0){total+=rc;if(written)*written=total;}
        else if(rc==LIBSSH2_ERROR_EAGAIN && shell_wait(sock,ssh,deadline))continue;
        else {shell->state=TS_SHELL_STATE_ERROR;return rc==LIBSSH2_ERROR_EAGAIN?ESP_ERR_TIMEOUT:ESP_FAIL;}
        if(total<len && esp_timer_get_time()>=deadline){shell->state=TS_SHELL_STATE_ERROR;return ESP_ERR_TIMEOUT;}
    }
    return ESP_OK;
}

esp_err_t ts_ssh_shell_read(ts_ssh_shell_t shell,char *buffer,size_t size,size_t *read_len)
{
    if(read_len)*read_len=0;
    if(!shell || !buffer || !size)return ESP_ERR_INVALID_ARG;
    if(shell->state!=TS_SHELL_STATE_RUNNING)return ESP_ERR_INVALID_STATE;
    int rc=libssh2_channel_read(shell->channel,buffer,size);
    if(rc>0){if(read_len)*read_len=rc;return ESP_OK;}
    if((rc==0 || rc==LIBSSH2_ERROR_EAGAIN) && libssh2_channel_eof(shell->channel)){
        shell->state=TS_SHELL_STATE_CLOSED;
        shell->exit_code=libssh2_channel_get_exit_status(shell->channel);
        return ESP_ERR_INVALID_STATE;
    }
    if(rc==0 || rc==LIBSSH2_ERROR_EAGAIN)return ESP_ERR_TIMEOUT; /* no data, not EOF */
    shell->state=TS_SHELL_STATE_ERROR;
    return ESP_FAIL;
}

esp_err_t ts_ssh_shell_set_output_cb(ts_ssh_shell_t shell,
                                      ts_shell_output_cb_t cb,
                                      void *user_data)
{
    if (!shell) {
        return ESP_ERR_INVALID_ARG;
    }
    shell->output_cb = cb;
    shell->output_cb_data = user_data;
    return ESP_OK;
}

esp_err_t ts_ssh_shell_set_close_cb(ts_ssh_shell_t shell,
                                     ts_shell_close_cb_t cb,
                                     void *user_data)
{
    if (!shell) {
        return ESP_ERR_INVALID_ARG;
    }
    shell->close_cb = cb;
    shell->close_cb_data = user_data;
    return ESP_OK;
}

esp_err_t ts_ssh_shell_poll(ts_ssh_shell_t shell)
{
    if(!shell)return ESP_ERR_INVALID_ARG;
    size_t n=0;esp_err_t ret=ts_ssh_shell_read(shell,shell->read_buffer,sizeof(shell->read_buffer),&n);
    if(n && shell->output_cb)shell->output_cb(shell->read_buffer,n,shell->output_cb_data);
    if(shell->state==TS_SHELL_STATE_CLOSED || shell->state==TS_SHELL_STATE_ERROR)shell_notify_close(shell);
    return ret;
}

esp_err_t ts_ssh_shell_run(ts_ssh_shell_t shell,ts_shell_output_cb_t output_cb,
                         ts_shell_input_cb_t input_cb,void *data)
{
    if(!shell || !output_cb || !input_cb)return ESP_ERR_INVALID_ARG;
    if(shell->state!=TS_SHELL_STATE_RUNNING)return ESP_ERR_INVALID_STATE;
    while(shell->state==TS_SHELL_STATE_RUNNING){
        size_t len=0;const char *input=input_cb(&len,data);
        if(shell->state!=TS_SHELL_STATE_RUNNING)break;
        if(input && len){esp_err_t ret=ts_ssh_shell_write(shell,input,len,NULL);if(ret!=ESP_OK)return ret;}
        bool activity = len != 0;
        size_t n;
        do {
            n = 0;
            esp_err_t ret = ts_ssh_shell_read(shell, shell->read_buffer, sizeof(shell->read_buffer), &n);
            if (n) { output_cb(shell->read_buffer, n, data); activity = true; }
            if (shell->state == TS_SHELL_STATE_CLOSED) return ESP_OK;
            if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) return ret;
        } while (n); /* preserve CLI's existing drain-until-no-data cadence */
        if(!activity)wait_socket(ts_ssh_get_socket(shell->ssh_session),ts_ssh_get_libssh2_session(shell->ssh_session),10);
    }
    return shell->state==TS_SHELL_STATE_CLOSED?ESP_OK:ESP_FAIL;
}

esp_err_t ts_ssh_shell_resize(ts_ssh_shell_t shell,
                               uint16_t width, uint16_t height)
{
    if (!shell || !width || !height) {
        return ESP_ERR_INVALID_ARG;
    }

    if (shell->state != TS_SHELL_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    LIBSSH2_SESSION *ssh = ts_ssh_get_libssh2_session(shell->ssh_session);
    int sock = ts_ssh_get_socket(shell->ssh_session);

    int rc;
    int64_t deadline=esp_timer_get_time()+1000000;
    while ((rc = libssh2_channel_request_pty_size(shell->channel, width, height)) 
           == LIBSSH2_ERROR_EAGAIN) {
        if(!shell_wait(sock,ssh,deadline)){shell->state=TS_SHELL_STATE_ERROR;return ESP_ERR_TIMEOUT;}
    }

    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to resize PTY: %d", rc);
        if(rc!=LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED)shell->state=TS_SHELL_STATE_ERROR;
        return ESP_FAIL;
    }

    shell->config.term_width = width;
    shell->config.term_height = height;
    ESP_LOGD(TAG, "Terminal resized to %dx%d", width, height);
    
    return ESP_OK;
}

esp_err_t ts_ssh_shell_send_signal(ts_ssh_shell_t shell, const char *signal_name)
{
    if (!shell || !signal_name) {
        return ESP_ERR_INVALID_ARG;
    }

    if (shell->state != TS_SHELL_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 对于常用信号，直接发送对应的控制字符 */
    if (strcmp(signal_name, "INT") == 0) {
        /* Ctrl+C */
        return ts_ssh_shell_write(shell, "\x03", 1, NULL);
    } else if (strcmp(signal_name, "QUIT") == 0) {
        /* Ctrl+\ */
        return ts_ssh_shell_write(shell, "\x1c", 1, NULL);
    } else if (strcmp(signal_name, "TSTP") == 0) {
        /* Ctrl+Z */
        return ts_ssh_shell_write(shell, "\x1a", 1, NULL);
    } else if (strcmp(signal_name, "EOF") == 0) {
        /* Ctrl+D */
        return ts_ssh_shell_write(shell, "\x04", 1, NULL);
    }

    /* 其他信号：尝试使用 libssh2 的信号功能 */
    /* 注意：并非所有服务器都支持 */
    ESP_LOGW(TAG, "Signal %s may not be supported", signal_name);
    return ESP_ERR_NOT_SUPPORTED;
}

ts_shell_state_t ts_ssh_shell_get_state(ts_ssh_shell_t shell)
{
    return shell ? shell->state : TS_SHELL_STATE_IDLE;
}

bool ts_ssh_shell_is_active(ts_ssh_shell_t shell)
{
    return shell && shell->state == TS_SHELL_STATE_RUNNING;
}

int ts_ssh_shell_get_exit_code(ts_ssh_shell_t shell)
{
    return shell ? shell->exit_code : -1;
}
