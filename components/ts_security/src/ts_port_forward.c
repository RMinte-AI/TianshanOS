/**
 * @file ts_port_forward.c
 * @brief SSH Port Forwarding implementation using libssh2
 *
 * 实现本地端口转发（Local Forwarding）功能：
 * - 在本地监听 TCP 端口
 * - 通过 SSH 隧道将连接转发到远程目标
 * - 支持多个并发连接
 */

#include "ts_port_forward.h"
#include "ts_ssh_client.h"
#include "ts_core.h"  /* TS_CALLOC_PSRAM */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#include <libssh2.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/select.h>

static const char *TAG = "ts_forward";
#define FORWARD_TASK_STACK_SIZE 16384

/* ============================================================================
 * 内部数据结构
 * ============================================================================ */

/** 单个转发连接 */
typedef struct forward_conn_s {
    int client_sock;                /**< 客户端 socket */
    LIBSSH2_CHANNEL *channel;       /**< SSH 通道 */
    char client_addr[48];           /**< 客户端地址 */
    uint16_t client_port;           /**< 客户端端口 */
    uint64_t bytes_sent;            /**< 发送字节数 */
    uint64_t bytes_received;        /**< 接收字节数 */
    bool active;                    /**< 是否活跃 */
    struct forward_conn_s *next;    /**< 链表下一个 */
} forward_conn_t;

/** 端口转发内部结构 */
struct ts_port_forward_s {
    ts_ssh_session_t ssh_session;   /**< SSH 会话 */
    ts_forward_config_t config;     /**< 转发配置 */
    _Atomic(ts_forward_state_t) state; /**< 当前状态 */
    
    char *io_buffer; /* one bounded workspace, owned exclusively by forward_task */
    int listen_sock;                /**< 监听 socket */
    _Atomic(TaskHandle_t) task_handle; /**< 转发任务句柄，控制者完成交接后清除 */
    SemaphoreHandle_t mutex;        /**< 互斥锁 */
    SemaphoreHandle_t exit_done;    /**< 最后一次对象访问的交接信号 */
    atomic_bool stop_flag;          /**< 停止标志 */
    
    /* 连接管理 */
    forward_conn_t *connections;    /**< 连接链表 */
    uint32_t active_count;          /**< 活跃连接数 */
    uint32_t total_count;           /**< 总连接数 */
    
    /* 统计 */
    uint64_t total_bytes_sent;      /**< 总发送字节 */
    uint64_t total_bytes_received;  /**< 总接收字节 */
    
    /* 回调 */
    ts_forward_connect_cb_t connect_cb;
    void *connect_cb_data;
    ts_forward_disconnect_cb_t disconnect_cb;
    void *disconnect_cb_data;
    
    /* 配置副本 */
    char *local_host_copy;
    char *remote_host_copy;
};

/* 外部声明（来自 ts_ssh_client.c） */
extern LIBSSH2_SESSION *ts_ssh_get_libssh2_session(ts_ssh_session_t session);
extern int ts_ssh_get_socket(ts_ssh_session_t session);

/* ============================================================================
 * 内部辅助函数
 * ============================================================================ */

/**
 * @brief 设置 socket 为非阻塞模式
 */
static void set_nonblocking(int sock)
{
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief 等待 socket 就绪
 */
static int wait_socket_ready(int sock, LIBSSH2_SESSION *session, int timeout_ms)
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

/**
 * @brief 创建监听 socket
 */
static int create_listen_socket(const char *host, uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    /* 设置端口复用 */
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* 绑定地址 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (host && strcmp(host, "0.0.0.0") != 0) {
        addr.sin_addr.s_addr = inet_addr(host);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind: %s", strerror(errno));
        close(sock);
        return -1;
    }

    if (listen(sock, 5) < 0) {
        ESP_LOGE(TAG, "Failed to listen: %s", strerror(errno));
        close(sock);
        return -1;
    }

    set_nonblocking(sock);
    return sock;
}

/**
 * @brief 处理单个连接的数据转发
 */
static void process_connection(ts_port_forward_t forward, forward_conn_t *conn)
{
    if (forward->stop_flag) return;
    LIBSSH2_SESSION *session = ts_ssh_get_libssh2_session(forward->ssh_session);
    int ssh_sock = ts_ssh_get_socket(forward->ssh_session);
    char *buffer = forward->io_buffer;
    int64_t deadline=esp_timer_get_time()+(int64_t)forward->config.timeout_ms*1000;
    ssize_t n;
    int rc;

    /* 从客户端读取并写入 SSH 通道 */
    n = recv(conn->client_sock, buffer, forward->config.buffer_size, MSG_DONTWAIT);
    if (n > 0) {
        size_t written = 0;
        while (written < (size_t)n) {
            if (forward->stop_flag || esp_timer_get_time()>=deadline) { conn->active=false;return; }
            rc = libssh2_channel_write(conn->channel, buffer + written, n - written);
            if (rc > 0) {
                written += rc;
                xSemaphoreTake(forward->mutex, portMAX_DELAY);
                conn->bytes_sent += rc;
                xSemaphoreGive(forward->mutex);
            } else if (rc == LIBSSH2_ERROR_EAGAIN) {
                wait_socket_ready(ssh_sock, session, 100);
            } else {
                ESP_LOGD(TAG, "Channel write error: %d", rc);
                conn->active = false;
                return;
            }
        }
    } else if (n == 0) {
        /* 客户端关闭连接 */
        conn->active = false;
        return;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        conn->active = false;
        return;
    }

    /* 从 SSH 通道读取并写入客户端 */
    do {
        if (forward->stop_flag) { conn->active=false;return; }
        rc = libssh2_channel_read(conn->channel, buffer, forward->config.buffer_size);
        if (rc > 0) {
            ssize_t sent = 0;
            while (sent < rc) {
                if (forward->stop_flag || esp_timer_get_time()>=deadline) { conn->active=false;return; }
                ssize_t s = send(conn->client_sock, buffer + sent, rc - sent, 0);
                if (s > 0) {
                    sent += s;
                    xSemaphoreTake(forward->mutex, portMAX_DELAY);
                    conn->bytes_received += s;
                    xSemaphoreGive(forward->mutex);
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                } else {
                    conn->active = false;
                    return;
                }
            }
        }
    } while (rc > 0 && !forward->stop_flag);

    if (forward->stop_flag) return;

    if (rc < 0 && rc != LIBSSH2_ERROR_EAGAIN) {
        conn->active = false;
    }

    /* 检查通道是否已关闭 */
    if (libssh2_channel_eof(conn->channel)) {
        conn->active = false;
    }
}

/**
 * @brief 接受新连接
 */
static void accept_new_connection(ts_port_forward_t forward)
{
    if (forward->stop_flag) return;
    LIBSSH2_SESSION *session = ts_ssh_get_libssh2_session(forward->ssh_session);
    int ssh_sock = ts_ssh_get_socket(forward->ssh_session);

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_sock = accept(forward->listen_sock, (struct sockaddr *)&client_addr, &addr_len);
    if (client_sock < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "Accept failed: %s", strerror(errno));
        }
        return;
    }

    /* 检查连接数限制 */
    if ((int)forward->active_count >= forward->config.max_connections) {
        ESP_LOGW(TAG, "Max connections reached, rejecting");
        close(client_sock);
        return;
    }

    set_nonblocking(client_sock);

    /* 创建 SSH 通道到远程目标 */
    LIBSSH2_CHANNEL *channel = NULL;
    int64_t deadline=esp_timer_get_time()+(int64_t)forward->config.timeout_ms*1000;
    
    while (!forward->stop_flag && (channel = libssh2_channel_direct_tcpip(session,
                                                    forward->remote_host_copy,
                                                    forward->config.remote_port)) == NULL) {
        if (forward->stop_flag || esp_timer_get_time()>=deadline) { close(client_sock);return; }
        int err = libssh2_session_last_errno(session);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            wait_socket_ready(ssh_sock, session, 100);
        } else {
            char *err_msg = NULL;
            libssh2_session_last_error(session, &err_msg, NULL, 0);
            ESP_LOGE(TAG, "Failed to create tunnel: %s", err_msg ? err_msg : "unknown");
            close(client_sock);
            return;
        }
    }

    if (!channel || forward->stop_flag) {
        if (channel) {
            libssh2_channel_close(channel);
            libssh2_channel_free(channel);
        }
        close(client_sock);
        return;
    }

    /* 创建连接记录 */
    forward_conn_t *conn = TS_CALLOC_PSRAM(1, sizeof(forward_conn_t));
    if (!conn) {
        libssh2_channel_free(channel);
        close(client_sock);
        return;
    }

    conn->client_sock = client_sock;
    conn->channel = channel;
    conn->active = true;
    inet_ntop(AF_INET, &client_addr.sin_addr, conn->client_addr, sizeof(conn->client_addr));
    conn->client_port = ntohs(client_addr.sin_port);

    /* 添加到链表 */
    xSemaphoreTake(forward->mutex, portMAX_DELAY);
    conn->next = forward->connections;
    forward->connections = conn;
    forward->active_count++;
    forward->total_count++;
    xSemaphoreGive(forward->mutex);

    ESP_LOGI(TAG, "New connection from %s:%d -> %s:%d",
             conn->client_addr, conn->client_port,
             forward->remote_host_copy, forward->config.remote_port);

    /* 调用连接回调 */
    if (forward->connect_cb) {
        forward->connect_cb(forward, conn->client_addr, conn->client_port,
                           forward->connect_cb_data);
    }
}

/**
 * @brief 清理已关闭的连接
 */
static void cleanup_connections(ts_port_forward_t forward)
{
    xSemaphoreTake(forward->mutex, portMAX_DELAY);
    
    forward_conn_t **pp = &forward->connections;
    while (*pp) {
        forward_conn_t *conn = *pp;
        if (!conn->active) {
            /* 从链表移除 */
            *pp = conn->next;
            forward->active_count--;
            
            /* 更新统计 */
            forward->total_bytes_sent += conn->bytes_sent;
            forward->total_bytes_received += conn->bytes_received;
            
            uint64_t transferred = conn->bytes_sent + conn->bytes_received;
            
            ESP_LOGI(TAG, "Connection closed: %s:%d (transferred %llu bytes)",
                     conn->client_addr, conn->client_port, transferred);
            
            /* 调用断开回调 */
            if (forward->disconnect_cb) {
                forward->disconnect_cb(forward, transferred, forward->disconnect_cb_data);
            }
            
            /* 清理资源 */
            if (conn->channel) {
                libssh2_channel_close(conn->channel);
                libssh2_channel_free(conn->channel);
            }
            close(conn->client_sock);
            free(conn);
        } else {
            pp = &conn->next;
        }
    }
    
    xSemaphoreGive(forward->mutex);
}

/**
 * @brief 端口转发任务
 */
static void forward_task(void *arg)
{
    ts_port_forward_t forward = (ts_port_forward_t)arg;

    /* Creation can schedule this task before it returns to the controller. */
    xSemaphoreTake(forward->mutex, portMAX_DELAY);
    xSemaphoreGive(forward->mutex);
    
    ESP_LOGI(TAG, "Port forwarding started: %s:%d -> %s:%d",
             forward->config.local_host, forward->config.local_port,
             forward->remote_host_copy, forward->config.remote_port);
    
    while (!forward->stop_flag) {
        /* 接受新连接 */
        accept_new_connection(forward);
        if (forward->stop_flag) break;
        
        /* 处理现有连接 */
        /* Only this worker mutates the connection list. I/O must not hold the
         * stats mutex: the CLI must be able to read stats and then read Ctrl+C. */
        for (forward_conn_t *conn = forward->connections; conn; conn = conn->next) {
            if (conn->active) {
                process_connection(forward, conn);
            }
        }
        if (forward->stop_flag) break;
        
        /* 清理已关闭的连接 */
        cleanup_connections(forward);
        
        /* 短暂延迟避免 CPU 占用过高 */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    /* 清理所有连接 */
    xSemaphoreTake(forward->mutex, portMAX_DELAY);
    while (forward->connections) {
        forward_conn_t *conn = forward->connections;
        forward->connections = conn->next;
        
        if (conn->channel) {
            libssh2_channel_close(conn->channel);
            libssh2_channel_free(conn->channel);
        }
        close(conn->client_sock);
        free(conn);
    }
    forward->active_count = 0;
    if (forward->listen_sock >= 0) {
        close(forward->listen_sock);
        forward->listen_sock = -1;
    }
    forward->state = TS_FORWARD_STATE_STOPPED;
    ESP_LOGI(TAG, "Port forwarding stopped");
    xSemaphoreGive(forward->exit_done);
    xSemaphoreGive(forward->mutex); /* final access; stop crosses this mutex */
    vTaskDeleteWithCaps(NULL);
}

/* ============================================================================
 * 公开 API 实现
 * ============================================================================ */

esp_err_t ts_port_forward_create(ts_ssh_session_t session,
                                  const ts_forward_config_t *config,
                                  ts_port_forward_t *forward_out)
{
    if (!session || !config || !forward_out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ts_ssh_is_connected(session)) {
        ESP_LOGE(TAG, "SSH session not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (!config->remote_host || config->remote_port == 0) {
        ESP_LOGE(TAG, "Remote host and port are required");
        return ESP_ERR_INVALID_ARG;
    }

    if (config->local_port == 0) {
        ESP_LOGE(TAG, "Local port is required");
        return ESP_ERR_INVALID_ARG;
    }

    /* 目前只支持本地转发 */
    if (config->direction != TS_FORWARD_LOCAL) {
        ESP_LOGE(TAG, "Only local forwarding is currently supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (config->buffer_size > 16384) return ESP_ERR_INVALID_SIZE;

    ts_port_forward_t forward = TS_CALLOC_PSRAM(1, sizeof(struct ts_port_forward_s));
    if (!forward) {
        return ESP_ERR_NO_MEM;
    }

    forward->ssh_session = session;
    forward->config = *config;
    atomic_init(&forward->state, TS_FORWARD_STATE_IDLE);
    atomic_init(&forward->task_handle, NULL);
    atomic_init(&forward->stop_flag, false);
    forward->listen_sock = -1;
    
    /* 设置默认值 */
    if (forward->config.timeout_ms == 0) {
        forward->config.timeout_ms = 5000;
    }
    if (forward->config.buffer_size == 0) {
        forward->config.buffer_size = 4096;
    }
    if (forward->config.max_connections == 0) {
        forward->config.max_connections = 5;
    }
    if (!forward->config.local_host) {
        forward->config.local_host = "127.0.0.1";
    }

    /* 复制字符串 */
    forward->local_host_copy = TS_STRDUP_PSRAM(forward->config.local_host);
    forward->remote_host_copy = TS_STRDUP_PSRAM(config->remote_host);
    forward->config.local_host = forward->local_host_copy;
    
    if (!forward->local_host_copy || !forward->remote_host_copy) {
        free(forward->local_host_copy);
        free(forward->remote_host_copy);
        free(forward);
        return ESP_ERR_NO_MEM;
    }

    forward->mutex = xSemaphoreCreateMutex();
    if (!forward->mutex) {
        free(forward->local_host_copy);
        free(forward->remote_host_copy);
        free(forward);
        return ESP_ERR_NO_MEM;
    }
    forward->exit_done = xSemaphoreCreateBinary();
    if (!forward->exit_done) {
        vSemaphoreDelete(forward->mutex);
        free(forward->local_host_copy);
        free(forward->remote_host_copy);
        free(forward);
        return ESP_ERR_NO_MEM;
    }

    forward->io_buffer = TS_MALLOC_PSRAM(forward->config.buffer_size);
    if (!forward->io_buffer) { ts_port_forward_destroy(forward); return ESP_ERR_NO_MEM; }
    *forward_out = forward;
    ESP_LOGD(TAG, "Port forward created: L%d -> %s:%d",
             config->local_port, config->remote_host, config->remote_port);
    return ESP_OK;
}

esp_err_t ts_port_forward_start(ts_port_forward_t forward)
{
    if (!forward) {
        return ESP_ERR_INVALID_ARG;
    }
    if (forward->stop_flag && forward->task_handle) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(forward->mutex, portMAX_DELAY);
    if (forward->task_handle) {
        esp_err_t ret = forward->stop_flag ? ESP_ERR_INVALID_STATE : ESP_OK;
        xSemaphoreGive(forward->mutex);
        return ret;
    }

    /* 创建监听 socket */
    forward->listen_sock = create_listen_socket(forward->config.local_host,
                                                 forward->config.local_port);
    if (forward->listen_sock < 0) {
        forward->state = TS_FORWARD_STATE_ERROR;
        xSemaphoreGive(forward->mutex);
        return ESP_FAIL;
    }

    /* 创建转发任务
     * 使用 PSRAM 栈以减少 DRAM 压力（纯 libssh2 网络操作，不涉及 NVS/Flash）*/
    forward->stop_flag = false;
    xSemaphoreTake(forward->exit_done, 0);
    TaskHandle_t created = NULL;
    BaseType_t ret = xTaskCreateWithCaps(forward_task, "ssh_forward", FORWARD_TASK_STACK_SIZE,
                                          forward, 5, &created,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        close(forward->listen_sock);
        forward->listen_sock = -1;
        forward->state = TS_FORWARD_STATE_ERROR;
        xSemaphoreGive(forward->mutex);
        return ESP_ERR_NO_MEM;
    }
    forward->task_handle = created;
    forward->state = TS_FORWARD_STATE_RUNNING;
    xSemaphoreGive(forward->mutex);
    return ESP_OK;
}

esp_err_t ts_port_forward_stop(ts_port_forward_t forward)
{
    if (!forward) {
        return ESP_ERR_INVALID_ARG;
    }

    if (forward->task_handle == xTaskGetCurrentTaskHandle()) return ESP_ERR_INVALID_STATE;
    if (forward->stop_flag && forward->task_handle) return ESP_ERR_NOT_FINISHED;
    xSemaphoreTake(forward->mutex, portMAX_DELAY);
    if (!forward->task_handle) {
        xSemaphoreGive(forward->mutex);
        return ESP_OK;
    }
    if (forward->stop_flag) {
        xSemaphoreGive(forward->mutex);
        return ESP_ERR_NOT_FINISHED;
    }

    /* 设置停止标志 */
    forward->stop_flag = true;
    xSemaphoreGive(forward->mutex);
    /* The CLI releases SSH immediately after success, so this is a real join,
     * never a timed success while the old worker still owns that session. */
    while (xSemaphoreTake(forward->exit_done, portMAX_DELAY) != pdTRUE) {}
    xSemaphoreTake(forward->mutex, portMAX_DELAY);
    forward->task_handle = NULL;
    xSemaphoreGive(forward->mutex);

    return ESP_OK;
}

esp_err_t ts_port_forward_destroy(ts_port_forward_t forward)
{
    if (!forward) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先停止 */
    esp_err_t ret = ts_port_forward_stop(forward);
    if (ret != ESP_OK) return ret;

    /* 释放资源 */
    if (forward->mutex) {
        vSemaphoreDelete(forward->mutex);
    }
    vSemaphoreDelete(forward->exit_done);
    free(forward->io_buffer);
    free(forward->local_host_copy);
    free(forward->remote_host_copy);
    free(forward);

    return ESP_OK;
}

esp_err_t ts_port_forward_get_stats(ts_port_forward_t forward, 
                                     ts_forward_stats_t *stats)
{
    if (!forward || !stats) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(forward->mutex, portMAX_DELAY);
    
    /* 累加当前活跃连接的统计 */
    uint64_t active_sent = 0;
    uint64_t active_received = 0;
    for (forward_conn_t *conn = forward->connections; conn; conn = conn->next) {
        active_sent += conn->bytes_sent;
        active_received += conn->bytes_received;
    }
    
    stats->bytes_sent = forward->total_bytes_sent + active_sent;
    stats->bytes_received = forward->total_bytes_received + active_received;
    stats->active_connections = forward->active_count;
    stats->total_connections = forward->total_count;
    stats->state = forward->state;
    
    xSemaphoreGive(forward->mutex);
    
    return ESP_OK;
}

ts_forward_state_t ts_port_forward_get_state(ts_port_forward_t forward)
{
    return forward ? forward->state : TS_FORWARD_STATE_IDLE;
}

esp_err_t ts_port_forward_set_connect_cb(ts_port_forward_t forward,
                                          ts_forward_connect_cb_t cb,
                                          void *user_data)
{
    if (!forward) {
        return ESP_ERR_INVALID_ARG;
    }
    forward->connect_cb = cb;
    forward->connect_cb_data = user_data;
    return ESP_OK;
}

esp_err_t ts_port_forward_set_disconnect_cb(ts_port_forward_t forward,
                                             ts_forward_disconnect_cb_t cb,
                                             void *user_data)
{
    if (!forward) {
        return ESP_ERR_INVALID_ARG;
    }
    forward->disconnect_cb = cb;
    forward->disconnect_cb_data = user_data;
    return ESP_OK;
}

esp_err_t ts_port_forward_direct(ts_ssh_session_t session,
                                  const char *remote_host,
                                  uint16_t remote_port,
                                  int local_sock,
                                  uint32_t timeout_ms)
{
    if (!session || !remote_host || remote_port == 0 || local_sock < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ts_ssh_is_connected(session)) {
        return ESP_ERR_INVALID_STATE;
    }

    LIBSSH2_SESSION *ssh = ts_ssh_get_libssh2_session(session);
    int ssh_sock = ts_ssh_get_socket(session);

    /* 创建通道 */
    LIBSSH2_CHANNEL *channel = NULL;
    while ((channel = libssh2_channel_direct_tcpip(ssh, remote_host, remote_port)) == NULL) {
        int err = libssh2_session_last_errno(ssh);
        if (err == LIBSSH2_ERROR_EAGAIN) {
            wait_socket_ready(ssh_sock, ssh, 100);
        } else {
            ESP_LOGE(TAG, "Failed to create direct-tcpip channel");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Direct tunnel established to %s:%d", remote_host, remote_port);

    /* 设置非阻塞 */
    set_nonblocking(local_sock);

    /* 数据转发循环 */
    char buffer[4096];
    uint32_t idle_time = 0;
    const uint32_t idle_check_interval = 100;  /* 100ms */

    while (1) {
        bool activity = false;
        
        /* 从本地读取并写入 SSH */
        ssize_t n = recv(local_sock, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n > 0) {
            activity = true;
            size_t written = 0;
            while (written < (size_t)n) {
                int rc = libssh2_channel_write(channel, buffer + written, n - written);
                if (rc > 0) {
                    written += rc;
                } else if (rc == LIBSSH2_ERROR_EAGAIN) {
                    wait_socket_ready(ssh_sock, ssh, 100);
                } else {
                    goto cleanup;
                }
            }
        } else if (n == 0) {
            /* 本地关闭 */
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }

        /* 从 SSH 读取并写入本地 */
        int rc;
        do {
            rc = libssh2_channel_read(channel, buffer, sizeof(buffer));
            if (rc > 0) {
                activity = true;
                ssize_t sent = 0;
                while (sent < rc) {
                    ssize_t s = send(local_sock, buffer + sent, rc - sent, 0);
                    if (s > 0) {
                        sent += s;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    } else {
                        goto cleanup;
                    }
                }
            }
        } while (rc > 0);

        if (rc < 0 && rc != LIBSSH2_ERROR_EAGAIN) {
            break;
        }

        /* 检查通道是否关闭 */
        if (libssh2_channel_eof(channel)) {
            break;
        }

        /* 空闲超时检查 */
        if (!activity) {
            idle_time += idle_check_interval;
            if (timeout_ms > 0 && idle_time >= timeout_ms) {
                ESP_LOGW(TAG, "Idle timeout reached");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(idle_check_interval));
        } else {
            idle_time = 0;
        }
    }

cleanup:
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    ESP_LOGI(TAG, "Direct tunnel closed");
    
    return ESP_OK;
}
