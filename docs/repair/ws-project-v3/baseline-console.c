/**
 * @file ts_cmd_ssh.c
 * @brief SSH Console Commands
 * 
 * 实现 ssh 命令：
 * - ssh --host <ip> --user <user> --password <pwd> --exec <cmd>
 * - ssh --test --host <ip> --user <user> --password <pwd>
 * - ssh --keygen --type rsa2048 --output /sdcard/id_rsa
 * - ssh --keyid <id> 使用安全存储的密钥连接
 * 
 * 注意：密钥管理已移至独立的 key 命令
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 * @date 2026-01-18
 */

#include "ts_console.h"
#include "ts_i18n.h"
#include "ts_api.h"
#include "ts_log.h"
#include "ts_core.h"  /* TS_MALLOC_PSRAM, TS_STRDUP_PSRAM */
#include "ts_ssh_client.h"
#include "ts_ssh_shell.h"
#include "ts_port_forward.h"
#include "ts_crypto.h"
#include "ts_keystore.h"
#include "ts_known_hosts.h"
#include "argtable3/argtable3.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TAG "cmd_ssh"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_str *host;       /**< SSH 服务器地址 */
    struct arg_int *port;       /**< SSH 端口 (默认 22) */
    struct arg_str *user;       /**< 用户名 */
    struct arg_str *password;   /**< 密码 */
    struct arg_str *key;        /**< 私钥文件路径 (PEM 格式) */
    struct arg_str *keyid;      /**< 密钥 ID（从安全存储读取） */
    struct arg_str *exec;       /**< 要执行的命令 */
    struct arg_lit *test;       /**< 测试连接 */
    struct arg_lit *shell;      /**< 交互式 Shell */
    struct arg_str *forward;    /**< 端口转发 L<local>:<remote_host>:<remote_port> */
    struct arg_lit *keygen;     /**< 生成密钥对 */
    struct arg_lit *copyid;     /**< 部署公钥到远程服务器 */
    struct arg_lit *revoke;     /**< 从远程服务器撤销公钥 */
    struct arg_str *type;       /**< 密钥类型 (rsa2048, rsa4096, ec256, ec384) */
    struct arg_str *output;     /**< 密钥输出路径 */
    struct arg_str *comment;    /**< 密钥注释 */
    struct arg_int *timeout;    /**< 超时时间（秒） */
    struct arg_lit *verbose;    /**< 详细输出 */
    struct arg_lit *help;
    struct arg_end *end;
} s_ssh_args;

/*===========================================================================*/
/*                          认证信息结构                                      */
/*===========================================================================*/

/**
 * @brief SSH 认证信息（统一传递认证参数）
 * 
 * 支持三种认证方式：
 * 1. 密码认证：只设置 password
 * 2. 密钥文件认证：只设置 key_path  
 * 3. 内存密钥认证：设置 key_data 和 key_len（安全存储密钥）
 */
typedef struct {
    const char *password;       /**< 密码（密码认证） */
    const char *key_path;       /**< 私钥文件路径（文件认证） */
    const uint8_t *key_data;    /**< 私钥数据（内存认证，安全存储） */
    size_t key_len;             /**< 私钥数据长度 */
    const char *passphrase;     /**< 私钥密码（可选） */
} ssh_auth_info_t;

/**
 * @brief 根据认证信息配置 SSH config
 */
static void config_ssh_auth(ts_ssh_config_t *config, const ssh_auth_info_t *auth)
{
    if (auth->key_data && auth->key_len > 0) {
        /* 优先使用内存中的密钥（安全存储） */
        config->auth_method = TS_SSH_AUTH_PUBLICKEY;
        config->auth.key.private_key = auth->key_data;
        config->auth.key.private_key_len = auth->key_len;
        config->auth.key.private_key_path = NULL;
        config->auth.key.passphrase = auth->passphrase;
    } else if (auth->key_path) {
        /* 使用文件路径 */
        config->auth_method = TS_SSH_AUTH_PUBLICKEY;
        config->auth.key.private_key_path = auth->key_path;
        config->auth.key.private_key = NULL;
        config->auth.key.private_key_len = 0;
        config->auth.key.passphrase = auth->passphrase;
    } else {
        /* 密码认证 */
        config->auth_method = TS_SSH_AUTH_PASSWORD;
        config->auth.password = auth->password;
    }
}

/*===========================================================================*/
/*                          流式输出回调                                      */
/*===========================================================================*/

/** 用于传递给回调的上下文 */
typedef struct {
    ts_ssh_session_t session;
    bool verbose;
} stream_context_t;

/**
 * @brief Stream output callback for SSH
 * @note Reserved for real-time streaming output feature
 */
__attribute__((unused))
static void stream_output_callback(const char *data, size_t len, bool is_stderr, void *user_data)
{
    stream_context_t *ctx = (stream_context_t *)user_data;
    
    /* 检查中断请求 */
    if (ts_console_interrupted()) {
        ts_ssh_abort(ctx->session);
        return;
    }
    
    /* 打印输出 */
    if (is_stderr && ctx->verbose) {
        ts_console_printf("\033[31m");  /* 红色表示 stderr */
    }
    
    /* 直接输出数据 */
    for (size_t i = 0; i < len; i++) {
        ts_console_printf("%c", data[i]);
    }
    
    if (is_stderr && ctx->verbose) {
        ts_console_printf("\033[0m");   /* 重置颜色 */
    }
}

/*===========================================================================*/
/*                          主机密钥验证                                      */
/*===========================================================================*/

/**
 * @brief 主机密钥验证提示回调
 * 
 * 当主机未知或密钥变化时，提示用户确认
 */
static bool host_verify_prompt(const ts_known_host_t *host, 
                               ts_host_verify_result_t result,
                               void *user_data)
{
    (void)user_data;
    
    if (result == TS_HOST_VERIFY_NOT_FOUND) {
        ts_console_printf("\n");
        ts_console_printf("┌─────────────────────────────────────────────────────────────┐\n");
        ts_console_printf("│  The authenticity of host '%s' can't be established.        \n", host->host);
        ts_console_printf("│  %s key fingerprint is:                                      \n", ts_host_key_type_str(host->type));
        ts_console_printf("│    SHA256:%s\n", host->fingerprint);
        ts_console_printf("└─────────────────────────────────────────────────────────────┘\n");
        ts_console_printf("\nAre you sure you want to continue connecting? (yes/no): ");
        fflush(stdout);
        
    } else if (result == TS_HOST_VERIFY_MISMATCH) {
        ts_console_printf("\n");
        ts_console_printf("╔═══════════════════════════════════════════════════════════════╗\n");
        ts_console_printf("║  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  ║\n");
        ts_console_printf("║  @    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @  ║\n");
        ts_console_printf("║  @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  ║\n");
        ts_console_printf("╠═══════════════════════════════════════════════════════════════╣\n");
        ts_console_printf("║  IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!        ║\n");
        ts_console_printf("║  Someone could be eavesdropping on you right now              ║\n");
        ts_console_printf("║  (man-in-the-middle attack)!                                  ║\n");
        ts_console_printf("╠═══════════════════════════════════════════════════════════════╣\n");
        ts_console_printf("║  Host: %-50s     ║\n", host->host);
        ts_console_printf("║  New fingerprint: SHA256:%.40s...                             \n", host->fingerprint);
        ts_console_printf("╚═══════════════════════════════════════════════════════════════╝\n");
        ts_console_printf("\nAre you ABSOLUTELY sure you want to continue? (yes/no): ");
        fflush(stdout);
    } else {
        return false;
    }
    
    /* 读取用户输入 */
    char input[16] = {0};
    int idx = 0;
    
    while (idx < (int)sizeof(input) - 1) {
        uint8_t ch;
        int len = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(30000));
        if (len <= 0) {
            ts_console_printf("\nTimeout - aborting connection.\n");
            return false;
        }
        
        if (ch == '\r' || ch == '\n') {
            ts_console_printf("\n");
            break;
        } else if (ch == 0x03) {  /* Ctrl+C */
            ts_console_printf("\n^C\n");
            return false;
        } else if (ch == 0x7f || ch == 0x08) {  /* Backspace */
            if (idx > 0) {
                idx--;
                ts_console_printf("\b \b");
            }
        } else if (ch >= 32 && ch < 127) {
            input[idx++] = ch;
            ts_console_printf("%c", ch);
        }
    }
    input[idx] = '\0';
    
    if (strcmp(input, "yes") == 0) {
        ts_console_printf("Host key accepted and saved.\n\n");
        return true;
    } else {
        ts_console_printf("Host key verification failed.\n");
        return false;
    }
}

/**
 * @brief 验证主机密钥（连接后调用）
 * 
 * @return ESP_OK 验证通过或用户接受，其他表示拒绝
 */
static esp_err_t verify_host_key(ts_ssh_session_t session, bool verbose)
{
    if (verbose) {
        ts_console_printf("Verifying host key...\n");
    }
    
    esp_err_t ret = ts_known_hosts_verify_interactive(session, host_verify_prompt, NULL);
    
    if (ret == ESP_OK && verbose) {
        ts_console_printf("Host key verified.\n");
    }
    
    return ret;
}
static esp_err_t cli_verify_before_auth(ts_ssh_session_t session, void *unused)
{
    return verify_host_key(session, false);
}


/*===========================================================================*/
/*                          Ctrl+C 检测任务                                   */
/*===========================================================================*/

typedef struct {
    ts_ssh_session_t session;
    volatile bool running;
} interrupt_monitor_t;

/**
 * @brief Task to monitor Ctrl+C interrupts during SSH sessions
 * @note Reserved for async interrupt handling feature
 */
__attribute__((unused))
static void interrupt_monitor_task(void *arg)
{
    interrupt_monitor_t *mon = (interrupt_monitor_t *)arg;
    
    while (mon->running) {
        /* 非阻塞检查 UART 输入 */
        uint8_t ch;
        int len = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(50));
        if (len > 0) {
            if (ch == 0x03) {  /* Ctrl+C = ASCII 0x03 */
                ts_console_printf("\n^C\n");
                ts_console_request_interrupt();
                ts_ssh_abort(mon->session);
                break;
            } else if (ch == 0x1C) {  /* Ctrl+\ = ASCII 0x1C (SIGQUIT) */
                ts_console_printf("\n^\\\n");
                ts_console_request_interrupt();
                ts_ssh_abort(mon->session);
                break;
            }
        }
    }
    
    vTaskDelete(NULL);
}

/*===========================================================================*/
/*                          Command: ssh --exec                               */
/*===========================================================================*/

static int do_ssh_exec(const char *host, int port, const char *user, 
                       const ssh_auth_info_t *auth,
                       const char *command, int timeout_sec, bool verbose)
{
    /* 构建 API 参数 */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "host", host);
    cJSON_AddNumberToObject(params, "port", port);
    cJSON_AddStringToObject(params, "user", user);
    cJSON_AddStringToObject(params, "command", command);
    cJSON_AddNumberToObject(params, "timeout_ms", timeout_sec * 1000);
    
    /* 配置认证方式 */
    if (auth->password) {
        cJSON_AddStringToObject(params, "password", auth->password);
    } else if (auth->key_path) {
        cJSON_AddStringToObject(params, "keypath", auth->key_path);
    }
    /* 注意：keyid 认证需要 CLI 先加载密钥，API 不直接支持 keyid */
    
    if (verbose) {
        ts_console_printf("Connecting to %s@%s:%d...\n", user, host, port);
        ts_console_printf("Executing command: %s\n", command);
        ts_console_printf("(Press Ctrl+C to abort)\n\n");
    }
    
    /* 调用 API */
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("ssh.exec", params, &result);
    cJSON_Delete(params);
    
    if (ret != ESP_OK || result.code != TS_API_OK) {
        ts_console_printf("Error: %s\n", result.message ? result.message : "SSH command failed");
        ts_api_result_free(&result);
        return 1;
    }
    
    /* 解析结果 */
    if (result.data) {
        cJSON *stdout_obj = cJSON_GetObjectItem(result.data, "stdout");
        cJSON *stderr_obj = cJSON_GetObjectItem(result.data, "stderr");
        cJSON *exit_code = cJSON_GetObjectItem(result.data, "exit_code");
        
        /* 输出 stdout */
        if (stdout_obj && cJSON_IsString(stdout_obj) && stdout_obj->valuestring[0]) {
            ts_console_printf("%s", stdout_obj->valuestring);
            if (stdout_obj->valuestring[strlen(stdout_obj->valuestring)-1] != '\n') {
                ts_console_printf("\n");
            }
        }
        
        /* 输出 stderr（红色） */
        if (stderr_obj && cJSON_IsString(stderr_obj) && stderr_obj->valuestring[0]) {
            if (verbose) {
                ts_console_printf("\033[31m%s\033[0m", stderr_obj->valuestring);
            } else {
                ts_console_printf("%s", stderr_obj->valuestring);
            }
        }
        
        if (verbose && exit_code) {
            ts_console_printf("\n--- Command completed with exit code: %d ---\n", 
                              exit_code->valueint);
        }
        
        int code = exit_code ? exit_code->valueint : 0;
        ts_api_result_free(&result);
        return code;
    }
    
    ts_api_result_free(&result);
    return 0;
}

/*===========================================================================*/
/*                          Command: ssh --shell                              */
/*===========================================================================*/

/** Shell 输出回调 */
static void shell_output_callback(const char *data, size_t len, void *user_data)
{
    (void)user_data;
    /* 直接输出到 stdout，更高效 */
    fwrite(data, 1, len, stdout);
    fflush(stdout);  /* 立即刷新，确保显示 */
}

/** Shell 输入回调 */
static const char *shell_input_callback(size_t *out_len, void *user_data)
{
    static char input_buf[64];
    ts_ssh_shell_t shell = (ts_ssh_shell_t)user_data;
    
    *out_len = 0;
    
    /* 非阻塞读取 UART（0ms 超时 = 立即返回） */
    uint8_t ch;
    int len = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, 0);
    if (len > 0) {
        /* 检查特殊控制字符 */
        if (ch == 0x03) {  /* Ctrl+C */
            /* 发送 SIGINT 到远程 */
            ts_ssh_shell_send_signal(shell, "INT");
            return NULL;
        } else if (ch == 0x1C) {  /* Ctrl+\ - 退出 Shell */
            ts_console_printf("\n^\\  (Exit shell)\n");
            ts_console_request_interrupt();
            return NULL;
        }
        
        input_buf[0] = ch;
        *out_len = 1;
        return input_buf;
    }
    
    return NULL;
}

static int do_ssh_shell(const char *host, int port, const char *user,
                        const ssh_auth_info_t *auth,
                        int timeout_sec, bool verbose)
{
    ts_ssh_session_t session = NULL;
    ts_ssh_shell_t shell = NULL;
    esp_err_t ret;
    
    /* 配置 SSH 连接 */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    config.host = host;
    config.port = port;
    config.username = user;
    config.timeout_ms = timeout_sec * 1000;
    
    /* 配置认证方式 */
    config_ssh_auth(&config, auth);
    
    if (verbose) {
        if (auth->key_data) {
            ts_console_printf("Using public key authentication (secure storage)\n");
        } else if (auth->key_path) {
            ts_console_printf("Using public key authentication\n");
        }
    }
    
    ts_console_printf("Connecting to %s@%s:%d...\n", user, host, port);
    
    /* 创建会话 */
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        ts_console_printf("Error: Failed to create SSH session\n");
        return 1;
    }
    
    /* 连接 */
    ret = ts_ssh_connect_with_verifier(session, cli_verify_before_auth, NULL);
    if (ret != ESP_OK) {
        ts_console_printf("Error: %s\n", ts_ssh_get_error(session));
        ts_ssh_session_destroy(session);
        return 1;
    }
    
    /* 验证主机密钥 */
    ret = verify_host_key(session, verbose);
    if (ret != ESP_OK) {
        ts_console_printf("Error: Host key verification failed\n");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        return 1;
    }
    
    /* 打开交互式 Shell */
    ts_shell_config_t shell_config = TS_SHELL_DEFAULT_CONFIG();
    shell_config.term_width = 80;
    shell_config.term_height = 24;
    shell_config.read_timeout_ms = 50;
    
    ret = ts_ssh_shell_open(session, &shell_config, &shell);
    if (ret != ESP_OK) {
        ts_console_printf("Error: Failed to open shell\n");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        return 1;
    }
    
    ts_console_printf("Connected. Interactive shell started.\n");
    ts_console_printf("(Press Ctrl+\\ to exit, Ctrl+C to send SIGINT)\n\n");
    ts_console_clear_interrupt();
    
    /* 运行交互式 Shell 循环 */
    ret = ts_ssh_shell_run(shell, shell_output_callback, shell_input_callback, shell);
    
    int exit_code = ts_ssh_shell_get_exit_code(shell);
    
    /* 清理 */
    ts_ssh_shell_close(shell);
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    ts_console_clear_interrupt();
    
    ts_console_printf("\n--- Shell closed (exit code: %d) ---\n", exit_code);
    
    return (ret == ESP_OK) ? 0 : 1;
}

/*===========================================================================*/
/*                          Command: ssh --forward                            */
/*===========================================================================*/

static int do_ssh_forward(const char *host, int port, const char *user,
                          const ssh_auth_info_t *auth,
                          const char *forward_spec,
                          int timeout_sec, bool verbose)
{
    ts_ssh_session_t session = NULL;
    ts_port_forward_t forward = NULL;
    esp_err_t ret;
    
    (void)verbose;  /* 暂未使用 */
    
    /* 解析端口转发规格: L<local_port>:<remote_host>:<remote_port> */
    if (forward_spec[0] != 'L' && forward_spec[0] != 'l') {
        ts_console_printf("Error: Forward spec must start with 'L' (local forward)\n");
        ts_console_printf("Format: L<local_port>:<remote_host>:<remote_port>\n");
        ts_console_printf("Example: L8080:localhost:80\n");
        return 1;
    }
    
    int local_port = 0;
    char remote_host[128] = {0};
    int remote_port = 0;
    
    if (sscanf(forward_spec + 1, "%d:%127[^:]:%d", &local_port, remote_host, &remote_port) != 3) {
        ts_console_printf("Error: Invalid forward spec format\n");
        ts_console_printf("Format: L<local_port>:<remote_host>:<remote_port>\n");
        ts_console_printf("Example: L8080:localhost:80\n");
        return 1;
    }
    
    if (local_port <= 0 || local_port > 65535 || remote_port <= 0 || remote_port > 65535) {
        ts_console_printf("Error: Invalid port number\n");
        return 1;
    }
    
    /* 配置 SSH 连接 */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    config.host = host;
    config.port = port;
    config.username = user;
    config.timeout_ms = timeout_sec * 1000;
    
    /* 配置认证方式 */
    config_ssh_auth(&config, auth);
    
    ts_console_printf("Connecting to %s@%s:%d...\n", user, host, port);
    
    /* 创建会话 */
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        ts_console_printf("Error: Failed to create SSH session\n");
        return 1;
    }
    
    /* 连接 */
    ret = ts_ssh_connect_with_verifier(session, cli_verify_before_auth, NULL);
    if (ret != ESP_OK) {
        ts_console_printf("Error: %s\n", ts_ssh_get_error(session));
        ts_ssh_session_destroy(session);
        return 1;
    }
    
    /* 验证主机密钥 */
    ret = verify_host_key(session, verbose);
    if (ret != ESP_OK) {
        ts_console_printf("Error: Host key verification failed\n");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        return 1;
    }
    
    /* 创建端口转发 */
    ts_forward_config_t fwd_config = TS_FORWARD_DEFAULT_CONFIG();
    fwd_config.direction = TS_FORWARD_LOCAL;
    fwd_config.local_host = "0.0.0.0";  /* 允许所有接口访问 */
    fwd_config.local_port = local_port;
    fwd_config.remote_host = remote_host;
    fwd_config.remote_port = remote_port;
    
    ret = ts_port_forward_create(session, &fwd_config, &forward);
    if (ret != ESP_OK) {
        ts_console_printf("Error: Failed to create port forward\n");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        return 1;
    }
    
    /* 启动转发 */
    ret = ts_port_forward_start(forward);
    if (ret != ESP_OK) {
        ts_console_printf("Error: Failed to start port forward\n");
        ts_port_forward_destroy(forward);
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("╔════════════════════════════════════════════╗\n");
    ts_console_printf("║         SSH Port Forwarding Active         ║\n");
    ts_console_printf("╠════════════════════════════════════════════╣\n");
    ts_console_printf("║  Local:   0.0.0.0:%-5d                    ║\n", local_port);
    ts_console_printf("║  Remote:  %s:%-5d", remote_host, remote_port);
    /* 填充空格 */
    int len = strlen(remote_host) + 6;
    for (int i = len; i < 25; i++) ts_console_printf(" ");
    ts_console_printf("║\n");
    ts_console_printf("╠════════════════════════════════════════════╣\n");
    ts_console_printf("║  Press Ctrl+C to stop forwarding           ║\n");
    ts_console_printf("╚════════════════════════════════════════════╝\n\n");
    
    ts_console_clear_interrupt();
    
    /* 等待用户中断 */
    while (!ts_console_interrupted()) {
        /* 检查 UART 输入 */
        uint8_t ch;
        int len = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, pdMS_TO_TICKS(500));
        if (len > 0 && (ch == 0x03 || ch == 0x1C)) {
            ts_console_printf("\n^C\n");
            break;
        }
        
        /* 显示统计（每 5 秒） */
        if (verbose) {
            static int counter = 0;
            if (++counter >= 10) {
                counter = 0;
                ts_forward_stats_t stats;
                if (ts_port_forward_get_stats(forward, &stats) == ESP_OK) {
                    ts_console_printf("Stats: %u active, %u total, TX: %llu, RX: %llu\r",
                                      stats.active_connections, stats.total_connections,
                                      stats.bytes_sent, stats.bytes_received);
                }
            }
        }
    }
    
    /* 停止转发 */
    ts_console_printf("Stopping port forward...\n");
    ts_port_forward_stop(forward);
    ts_port_forward_destroy(forward);
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    ts_console_clear_interrupt();
    
    ts_console_printf("Port forwarding stopped.\n");
    return 0;
}

/*===========================================================================*/
/*                          Command: ssh --test                               */
/*===========================================================================*/

static int do_ssh_test(const char *host, int port, const char *user, 
                       const ssh_auth_info_t *auth, int timeout_sec)
{
    const char *auth_type = "Password";
    if (auth->key_data) {
        auth_type = "Public Key (secure storage)";
    } else if (auth->key_path) {
        auth_type = "Public Key";
    }
    
    ts_console_printf("\nSSH Connection Test\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Host:     %s\n", host);
    ts_console_printf("  Port:     %d\n", port);
    ts_console_printf("  User:     %s\n", user);
    ts_console_printf("  Auth:     %s\n", auth_type);
    ts_console_printf("  Timeout:  %d seconds\n", timeout_sec);
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    /* 构建 API 参数 */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "host", host);
    cJSON_AddNumberToObject(params, "port", port);
    cJSON_AddStringToObject(params, "user", user);
    
    /* 配置认证方式 */
    if (auth->password) {
        cJSON_AddStringToObject(params, "password", auth->password);
    } else if (auth->key_path) {
        cJSON_AddStringToObject(params, "keypath", auth->key_path);
    }
    
    ts_console_printf("[1/2] Testing connection... ");
    
    /* 调用 API */
    ts_api_result_t result;
    ts_api_result_init(&result);
    
    esp_err_t ret = ts_api_call("ssh.test", params, &result);
    cJSON_Delete(params);
    
    if (ret != ESP_OK || result.code != TS_API_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", result.message ? result.message : "Connection failed");
        ts_api_result_free(&result);
        return 1;
    }
    
    /* 解析结果 */
    bool success = false;
    if (result.data) {
        cJSON *success_obj = cJSON_GetObjectItem(result.data, "success");
        success = cJSON_IsTrue(success_obj);
        
        if (!success) {
            cJSON *error_obj = cJSON_GetObjectItem(result.data, "error");
            ts_console_printf("FAILED\n");
            if (error_obj && cJSON_IsString(error_obj)) {
                ts_console_printf("  Error: %s\n", error_obj->valuestring);
            }
        } else {
            ts_console_printf("OK\n");
        }
    }
    
    ts_api_result_free(&result);
    
    ts_console_printf("[2/2] Verifying response... ");
    if (success) {
        ts_console_printf("OK\n");
    } else {
        ts_console_printf("FAILED\n");
    }
    
    ts_console_printf("\n");
    if (success) {
        ts_console_printf("✓ SSH connection test PASSED\n");
        return 0;
    } else {
        ts_console_printf("✗ SSH connection test FAILED\n");
        return 1;
    }
}

/*===========================================================================*/
/*                          Command: ssh --copy-id                            */
/*===========================================================================*/

/**
 * @brief 读取公钥文件内容
 */
static esp_err_t load_public_key(const char *path, char **pubkey_data)
{
    if (!path || !pubkey_data) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 构建公钥文件路径 (.pub) */
    char pub_path[256];
    snprintf(pub_path, sizeof(pub_path), "%s.pub", path);

    FILE *fp = fopen(pub_path, "r");
    if (!fp) {
        ts_console_printf("Error: Cannot open public key file: %s\n", pub_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* 获取文件大小 */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 8192) {
        ts_console_printf("Error: Invalid public key file size: %ld\n", fsize);
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 分配内存并读取 */
    char *data = TS_MALLOC_PSRAM(fsize + 1);
    if (!data) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t read_len = fread(data, 1, fsize, fp);
    fclose(fp);

    if (read_len != (size_t)fsize) {
        free(data);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 去除末尾换行符 */
    data[fsize] = '\0';
    while (fsize > 0 && (data[fsize - 1] == '\n' || data[fsize - 1] == '\r')) {
        data[--fsize] = '\0';
    }

    *pubkey_data = data;
    return ESP_OK;
}

static int do_ssh_copy_id(const char *host, int port, const char *user, 
                          const char *password, const char *key_path, 
                          const char *keyid, int timeout_sec)
{
    ts_ssh_session_t session = NULL;
    esp_err_t ret;
    char *pubkey_data = NULL;
    char *privkey_data = NULL;
    size_t privkey_len = 0;
    bool use_keystore = (keyid != NULL);
    
    ts_console_printf("\nSSH Public Key Deployment\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Host:     %s\n", host);
    ts_console_printf("  Port:     %d\n", port);
    ts_console_printf("  User:     %s\n", user);
    if (use_keystore) {
        ts_console_printf("  Key:      [secure storage] %s\n", keyid);
    } else {
        ts_console_printf("  Key:      %s.pub\n", key_path);
    }
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    /* 读取公钥 */
    ts_console_printf("[1/4] Reading public key... ");
    if (use_keystore) {
        size_t pubkey_len = 0;
        ret = ts_keystore_load_public_key(keyid, &pubkey_data, &pubkey_len);
        if (ret != ESP_OK) {
            ts_console_printf("FAILED\n");
            ts_console_printf("  Error: Cannot load public key '%s' from secure storage\n", keyid);
            return 1;
        }
    } else {
        ret = load_public_key(key_path, &pubkey_data);
        if (ret != ESP_OK) {
            ts_console_printf("FAILED\n");
            return 1;
        }
    }
    ts_console_printf("OK\n");
    
    /* 配置 SSH 连接（使用密码认证） */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    config.host = host;
    config.port = port;
    config.username = user;
    config.timeout_ms = timeout_sec * 1000;
    config.auth_method = TS_SSH_AUTH_PASSWORD;
    config.auth.password = password;
    
    ts_console_printf("[2/4] Connecting with password... ");
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED (session create)\n");
        free(pubkey_data);
        return 1;
    }
    
    ret = ts_ssh_connect_with_verifier(session, cli_verify_before_auth, NULL);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", ts_ssh_get_error(session));
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        return 1;
    }
    
    /* 验证主机密钥 */
    ret = verify_host_key(session, false);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Host key verification failed\n");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        return 1;
    }
    ts_console_printf("OK\n");
    
    /* 构建部署命令 */
    ts_console_printf("[3/4] Deploying public key... ");
    
    /* 命令：创建 .ssh 目录并追加公钥到 authorized_keys */
    char *deploy_cmd = TS_MALLOC_PSRAM(strlen(pubkey_data) + 512);
    if (!deploy_cmd) {
        ts_console_printf("FAILED (out of memory)\n");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        return 1;
    }
    
    snprintf(deploy_cmd, strlen(pubkey_data) + 512,
             "mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
             "echo '%s' >> ~/.ssh/authorized_keys && "
             "chmod 600 ~/.ssh/authorized_keys && "
             "echo 'Key deployed successfully'",
             pubkey_data);
    
    ts_ssh_exec_result_t result;
    ret = ts_ssh_exec(session, deploy_cmd, &result);
    free(deploy_cmd);
    
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", ts_ssh_get_error(session));
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        return 1;
    }
    
    bool deploy_ok = (result.exit_code == 0);
    if (result.stderr_data && strlen(result.stderr_data) > 0) {
        ts_console_printf("WARNING\n");
        ts_console_printf("  stderr: %s\n", result.stderr_data);
    } else if (deploy_ok) {
        ts_console_printf("OK\n");
    } else {
        ts_console_printf("FAILED (exit code: %d)\n", result.exit_code);
    }
    ts_ssh_exec_result_free(&result);
    
    /* 断开密码连接 */
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    
    if (!deploy_ok) {
        free(pubkey_data);
        return 1;
    }
    
    /* 验证公钥认证 */
    ts_console_printf("[4/4] Verifying public key auth... ");
    
    /* 加载私钥用于验证 */
    if (use_keystore) {
        ret = ts_keystore_load_private_key(keyid, &privkey_data, &privkey_len);
        if (ret != ESP_OK) {
            ts_console_printf("SKIPPED\n");
            ts_console_printf("  Note: Cannot load private key for verification\n");
            free(pubkey_data);
            ts_console_printf("\n✓ Public key deployed successfully!\n");
            ts_console_printf("\nYou can now connect without password:\n");
            ts_console_printf("  ssh --host %s --user %s --keyid %s --shell\n", host, user, keyid);
            return 0;
        }
    }
    
    /* 使用公钥认证重新连接 */
    ts_ssh_config_t verify_config = TS_SSH_DEFAULT_CONFIG();
    verify_config.host = host;
    verify_config.port = port;
    verify_config.username = user;
    verify_config.timeout_ms = timeout_sec * 1000;
    verify_config.auth_method = TS_SSH_AUTH_PUBLICKEY;
    
    /* 配置认证方式 */
    if (use_keystore && privkey_data) {
        /* 使用内存中的密钥（安全，不写入临时文件） */
        verify_config.auth.key.private_key = (const uint8_t *)privkey_data;
        verify_config.auth.key.private_key_len = privkey_len;
        verify_config.auth.key.private_key_path = NULL;
    } else {
        /* 使用文件路径 */
        verify_config.auth.key.private_key_path = key_path;
        verify_config.auth.key.private_key = NULL;
        verify_config.auth.key.private_key_len = 0;
    }
    verify_config.auth.key.passphrase = NULL;
    
    ret = ts_ssh_session_create(&verify_config, &session);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED (session)\n");
        free(pubkey_data);
        if (privkey_data) {
            memset(privkey_data, 0, privkey_len);  /* 安全清零 */
            free(privkey_data);
        }
        return 1;
    }
    
    ret = ts_ssh_connect_with_verifier(session, cli_verify_before_auth, NULL);
    
    if (ret != ESP_OK) {
        const char *error_msg = ts_ssh_get_error(session);
        
        /* 检查是否是密钥类型不支持的错误 */
        if (error_msg && (strstr(error_msg, "Key type not supported") || 
                          strstr(error_msg, "Method unimplemented") ||
                          strstr(error_msg, "Method not supported"))) {
            ts_console_printf("SKIPPED\n");
            ts_console_printf("  Note: Key type may not be fully supported for verification\n");
            ts_ssh_session_destroy(session);
            free(pubkey_data);
            if (privkey_data) {
                memset(privkey_data, 0, privkey_len);
                free(privkey_data);
            }
            ts_console_printf("\n✓ Public key deployed successfully!\n");
            ts_console_printf("\n⚠ Verification skipped (may be ECDSA or memory auth limitation)\n");
            ts_console_printf("  The key has been added to authorized_keys.\n");
            ts_console_printf("  Try connecting with:\n");
            if (use_keystore) {
                ts_console_printf("    ssh --host %s --user %s --keyid %s --shell\n", host, user, keyid);
            } else {
                ts_console_printf("    ssh --host %s --user %s --key %s --shell\n", host, user, key_path);
            }
            return 0;
        }
        
        /* 检查是否是 ECDSA 不支持 */
        if (privkey_data && strstr(privkey_data, "BEGIN EC PRIVATE KEY")) {
            ts_console_printf("SKIPPED\n");
            ts_console_printf("  Note: ECDSA keys may not be fully supported by libssh2 mbedTLS backend\n");
            ts_ssh_session_destroy(session);
            free(pubkey_data);
            memset(privkey_data, 0, privkey_len);
            free(privkey_data);
            ts_console_printf("\n✓ Public key deployed successfully!\n");
            ts_console_printf("\nYou can now connect. If authentication fails, try using RSA keys:\n");
            ts_console_printf("    key --generate --id mykey --type rsa\n");
            return 0;
        }
        
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", error_msg ? error_msg : "Unknown error");
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        if (privkey_data) {
            memset(privkey_data, 0, privkey_len);
            free(privkey_data);
        }
        ts_console_printf("\n⚠ Key deployed but verification failed\n");
        return 1;
    }
    
    ts_console_printf("OK\n");
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    free(pubkey_data);
    if (privkey_data) {
        memset(privkey_data, 0, privkey_len);
        free(privkey_data);
    }
    
    ts_console_printf("\n✓ Public key authentication configured successfully!\n");
    ts_console_printf("\nYou can now connect without password:\n");
    if (use_keystore) {
        ts_console_printf("  ssh --host %s --user %s --keyid %s --shell\n", host, user, keyid);
    } else {
        ts_console_printf("  ssh --host %s --user %s --key %s --shell\n", host, user, key_path);
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: ssh --revoke                             */
/*===========================================================================*/

/**
 * @brief 从远程服务器撤销已部署的公钥
 * 
 * 使用密码认证连接到远程服务器，然后从 authorized_keys 中移除匹配的公钥。
 * 支持两种方式指定要撤销的密钥：
 * 1. --keyid：从安全存储获取公钥
 * 2. --key：从文件读取公钥
 * 
 * @param host 远程主机地址
 * @param port SSH 端口
 * @param user 用户名
 * @param password 密码（用于认证）
 * @param key_path 私钥文件路径（用于定位公钥）
 * @param keyid 密钥 ID（从安全存储）
 * @param timeout_sec 超时时间（秒）
 * @return 0 成功，非 0 失败
 */
static int do_ssh_revoke(const char *host, int port, const char *user, 
                         const char *password, const char *key_path, 
                         const char *keyid, int timeout_sec)
{
    ts_console_printf("\n══════════════════════════════════════════════════════════════════\n");
    ts_console_printf("  SSH Public Key Revocation\n");
    ts_console_printf("══════════════════════════════════════════════════════════════════\n\n");
    
    ts_ssh_session_t session = NULL;
    esp_err_t ret;
    char *pubkey_data = NULL;
    size_t pubkey_len = 0;
    bool use_keystore = (keyid != NULL && key_path == NULL);
    
    /* 获取公钥数据（用于匹配要删除的密钥） */
    if (use_keystore) {
        ts_console_printf("Loading public key '%s' from secure storage... ", keyid);
        ret = ts_keystore_load_public_key(keyid, &pubkey_data, &pubkey_len);
        if (ret != ESP_OK) {
            ts_console_printf("FAILED\n");
            ts_console_printf("Error: Failed to load public key: %s\n", esp_err_to_name(ret));
            return 1;
        }
        ts_console_printf("OK\n");
    } else {
        /* 从文件读取公钥 */
        char pubkey_path[256];
        snprintf(pubkey_path, sizeof(pubkey_path), "%s.pub", key_path);
        
        ts_console_printf("Reading public key from %s... ", pubkey_path);
        
        FILE *f = fopen(pubkey_path, "r");
        if (!f) {
            ts_console_printf("FAILED\n");
            ts_console_printf("Error: Cannot open public key file\n");
            return 1;
        }
        
        fseek(f, 0, SEEK_END);
        pubkey_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        pubkey_data = TS_MALLOC_PSRAM(pubkey_len + 1);
        if (!pubkey_data) {
            fclose(f);
            ts_console_printf("FAILED\n");
            ts_console_printf("Error: Memory allocation failed\n");
            return 1;
        }
        
        fread(pubkey_data, 1, pubkey_len, f);
        fclose(f);
        pubkey_data[pubkey_len] = '\0';
        ts_console_printf("OK\n");
    }
    
    /* 提取公钥的 key type 和 key data 部分（忽略 comment）
     * 格式: "ssh-rsa AAAAB3Nza... comment"
     * 我们需要 "ssh-rsa AAAAB3Nza..." 部分来匹配 */
    char *key_type = NULL;
    char *key_data = NULL;
    char *pubkey_copy = TS_STRDUP_PSRAM(pubkey_data);
    if (!pubkey_copy) {
        free(pubkey_data);
        ts_console_printf("Error: Memory allocation failed\n");
        return 1;
    }
    
    /* 解析公钥格式 */
    key_type = strtok(pubkey_copy, " ");
    key_data = strtok(NULL, " ");
    
    if (!key_type || !key_data) {
        ts_console_printf("Error: Invalid public key format\n");
        free(pubkey_data);
        free(pubkey_copy);
        return 1;
    }
    
    ts_console_printf("\nKey to revoke:\n");
    ts_console_printf("  Type: %s\n", key_type);
    ts_console_printf("  Data: %.40s...\n", key_data);
    
    /* 显示警告 */
    ts_console_printf("\n");
    ts_console_printf("┌─────────────────────────────────────────────────────────────┐\n");
    ts_console_printf("│  WARNING: This will remove the public key from the remote   │\n");
    ts_console_printf("│  server's authorized_keys file.                             │\n");
    ts_console_printf("└─────────────────────────────────────────────────────────────┘\n");
    ts_console_printf("\nTarget: %s@%s:%d\n\n", user, host, port);
    
    /* 配置 SSH 连接（使用密码认证） */
    ts_ssh_config_t config = TS_SSH_DEFAULT_CONFIG();
    config.host = host;
    config.port = port;
    config.username = user;
    config.timeout_ms = timeout_sec * 1000;
    config.auth_method = TS_SSH_AUTH_PASSWORD;
    config.auth.password = password;
    
    /* 创建会话 */
    ts_console_printf("Connecting with password authentication... ");
    ret = ts_ssh_session_create(&config, &session);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("Error: Failed to create SSH session\n");
        free(pubkey_data);
        free(pubkey_copy);
        return 1;
    }
    
    /* 连接 */
    ret = ts_ssh_connect_with_verifier(session, cli_verify_before_auth, NULL);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("Error: %s\n", ts_ssh_get_error(session));
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return 1;
    }
    ts_console_printf("OK\n");
    
    /* 验证主机密钥 */
    ret = verify_host_key(session, false);
    if (ret != ESP_OK) {
        ts_console_printf("  Host key verification failed\n");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return 1;
    }
    
    /* 构建检查命令 - 使用 grep -F 进行固定字符串匹配
     * 只取 key_data 的前 100 字符作为匹配特征，避免命令行过长
     * 同时需要检查完整密钥类型前缀以确保唯一性 */
    char key_signature[128];
    size_t sig_len = strlen(key_data) > 100 ? 100 : strlen(key_data);
    snprintf(key_signature, sizeof(key_signature), "%s %.*s", key_type, (int)sig_len, key_data);
    
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "if [ -f ~/.ssh/authorized_keys ]; then "
        "  grep -cF '%s' ~/.ssh/authorized_keys 2>/dev/null || echo '0'; "
        "else "
        "  echo '0'; "
        "fi",
        key_signature);
    
    /* 先检查密钥是否存在 */
    ts_console_printf("Checking if key exists on remote... ");
    
    ts_ssh_exec_result_t result;
    ret = ts_ssh_exec(session, cmd, &result);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("Error: Failed to check key: %s\n", ts_ssh_get_error(session));
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return 1;
    }
    
    int key_count = 0;
    if (result.stdout_data) {
        key_count = atoi(result.stdout_data);
    }
    ts_ssh_exec_result_free(&result);
    
    if (key_count == 0) {
        ts_console_printf("NOT FOUND\n");
        ts_console_printf("\n⚠ The specified public key was not found on the remote server.\n");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return 0;  /* 不视为错误 */
    }
    
    ts_console_printf("FOUND (%d match%s)\n", key_count, key_count > 1 ? "es" : "");
    
    /* 执行删除操作 - 使用 grep -vF 进行固定字符串排除 */
    ts_console_printf("Removing key from authorized_keys... ");
    
    snprintf(cmd, sizeof(cmd),
        "cp ~/.ssh/authorized_keys ~/.ssh/authorized_keys.bak 2>/dev/null; "
        "grep -vF '%s' ~/.ssh/authorized_keys > ~/.ssh/authorized_keys.tmp 2>/dev/null && "
        "mv ~/.ssh/authorized_keys.tmp ~/.ssh/authorized_keys && "
        "chmod 600 ~/.ssh/authorized_keys && "
        "echo 'OK'",
        key_signature);
    
    ret = ts_ssh_exec(session, cmd, &result);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("Error: Failed to remove key: %s\n", ts_ssh_get_error(session));
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return 1;
    }
    
    bool remove_ok = (result.stdout_data && strstr(result.stdout_data, "OK") != NULL);
    ts_ssh_exec_result_free(&result);
    
    if (!remove_ok) {
        ts_console_printf("FAILED\n");
        ts_console_printf("Error: Failed to remove key\n");
        ts_ssh_disconnect(session);
        ts_ssh_session_destroy(session);
        free(pubkey_data);
        free(pubkey_copy);
        return 1;
    }
    ts_console_printf("OK\n");
    
    /* 验证删除成功 */
    ts_console_printf("Verifying removal... ");
    
    snprintf(cmd, sizeof(cmd),
        "grep -cF '%s' ~/.ssh/authorized_keys 2>/dev/null || echo '0'",
        key_signature);
    
    ret = ts_ssh_exec(session, cmd, &result);
    if (ret == ESP_OK && result.stdout_data) {
        int remaining = atoi(result.stdout_data);
        if (remaining == 0) {
            ts_console_printf("OK\n");
        } else {
            ts_console_printf("WARNING (%d keys still match)\n", remaining);
        }
    } else {
        ts_console_printf("SKIPPED\n");
    }
    ts_ssh_exec_result_free(&result);
    
    ts_ssh_disconnect(session);
    ts_ssh_session_destroy(session);
    free(pubkey_data);
    free(pubkey_copy);
    
    ts_console_printf("\n✓ Public key revoked successfully!\n");
    ts_console_printf("\n  A backup was saved to ~/.ssh/authorized_keys.bak on the remote server.\n");
    
    /* 如果是 keystore 密钥，提示可以删除本地密钥 */
    if (use_keystore) {
        ts_console_printf("\n  To also delete the local key:\n");
        ts_console_printf("    key --delete --id %s\n", keyid);
    }
    
    return 0;
}

/*===========================================================================*/
/*                          Command: ssh --keygen                             */
/*===========================================================================*/

/**
 * @brief 解析密钥类型字符串
 */
static bool parse_key_type(const char *type_str, ts_crypto_key_type_t *type)
{
    if (!type_str || !type) return false;
    
    if (strcmp(type_str, "rsa2048") == 0 || strcmp(type_str, "rsa") == 0) {
        *type = TS_CRYPTO_KEY_RSA_2048;
        return true;
    } else if (strcmp(type_str, "rsa4096") == 0) {
        *type = TS_CRYPTO_KEY_RSA_4096;
        return true;
    } else if (strcmp(type_str, "ec256") == 0 || strcmp(type_str, "ecdsa") == 0) {
        *type = TS_CRYPTO_KEY_EC_P256;
        return true;
    } else if (strcmp(type_str, "ec384") == 0) {
        *type = TS_CRYPTO_KEY_EC_P384;
        return true;
    }
    return false;
}

/**
 * @brief 获取密钥类型的描述字符串
 */
static const char *get_key_type_desc(ts_crypto_key_type_t type)
{
    switch (type) {
        case TS_CRYPTO_KEY_RSA_2048: return "RSA 2048-bit";
        case TS_CRYPTO_KEY_RSA_4096: return "RSA 4096-bit";
        case TS_CRYPTO_KEY_EC_P256:  return "ECDSA P-256 (secp256r1)";
        case TS_CRYPTO_KEY_EC_P384:  return "ECDSA P-384 (secp384r1)";
        default: return "Unknown";
    }
}

/*===========================================================================*/
/*                          Command: ssh --keygen (file-based)                */
/*===========================================================================*/

/**
 * @brief 生成密钥对到文件（保留直接实现，因为需要写入文件系统）
 * 
 * 注意：如果要生成到安全存储，应使用 key 命令或 ssh.keygen API
 */
static int do_ssh_keygen(const char *type_str, const char *output_path, const char *comment)
{
    ts_crypto_key_type_t key_type;
    ts_keypair_t keypair = NULL;
    esp_err_t ret;
    
    /* 解析密钥类型 */
    if (!parse_key_type(type_str, &key_type)) {
        ts_console_printf("Error: Invalid key type '%s'\n", type_str);
        ts_console_printf("Supported types: rsa, rsa2048, rsa4096, ecdsa, ec256, ec384\n");
        return 1;
    }
    
    ts_console_printf("\nSSH Key Generation\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Type:     %s\n", get_key_type_desc(key_type));
    ts_console_printf("  Output:   %s\n", output_path);
    if (comment) {
        ts_console_printf("  Comment:  %s\n", comment);
    }
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    /* 生成密钥对 */
    ts_console_printf("[1/4] Generating key pair... ");
    
    /* RSA 4096 需要较长时间，给出提示 */
    if (key_type == TS_CRYPTO_KEY_RSA_4096) {
        ts_console_printf("(this may take 30-60 seconds)\n      ");
    }
    
    ret = ts_crypto_keypair_generate(key_type, &keypair);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: Failed to generate key pair (%s)\n", esp_err_to_name(ret));
        return 1;
    }
    ts_console_printf("OK\n");
    
    /* 导出私钥 (PEM 格式) */
    ts_console_printf("[2/4] Saving private key... ");
    
    char *private_pem = TS_MALLOC_PSRAM(8192);
    if (!private_pem) {
        ts_console_printf("FAILED (out of memory)\n");
        ts_crypto_keypair_free(keypair);
        return 1;
    }
    
    size_t pem_len = 8192;
    ret = ts_crypto_keypair_export_private(keypair, private_pem, &pem_len);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        free(private_pem);
        ts_crypto_keypair_free(keypair);
        return 1;
    }
    
    /* 写入私钥文件 */
    FILE *fp = fopen(output_path, "w");
    if (!fp) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: Cannot create file %s\n", output_path);
        free(private_pem);
        ts_crypto_keypair_free(keypair);
        return 1;
    }
    
    fprintf(fp, "%s", private_pem);
    fclose(fp);
    free(private_pem);
    ts_console_printf("OK\n");
    
    /* 导出公钥 (OpenSSH 格式) */
    ts_console_printf("[3/4] Saving public key... ");
    
    char *openssh_pub = TS_MALLOC_PSRAM(4096);
    if (!openssh_pub) {
        ts_console_printf("FAILED (out of memory)\n");
        ts_crypto_keypair_free(keypair);
        return 1;
    }
    
    size_t openssh_len = 4096;
    const char *key_comment = comment ? comment : "TianShanOS-generated-key";
    ret = ts_crypto_keypair_export_openssh(keypair, openssh_pub, &openssh_len, key_comment);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        free(openssh_pub);
        ts_crypto_keypair_free(keypair);
        return 1;
    }
    
    /* 写入公钥文件 (.pub) */
    char pub_path[256];
    snprintf(pub_path, sizeof(pub_path), "%s.pub", output_path);
    
    fp = fopen(pub_path, "w");
    if (!fp) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: Cannot create file %s\n", pub_path);
        free(openssh_pub);
        ts_crypto_keypair_free(keypair);
        return 1;
    }
    
    fprintf(fp, "%s", openssh_pub);
    fclose(fp);
    ts_console_printf("OK\n");
    
    /* 显示公钥指纹 */
    ts_console_printf("[4/4] Key generation complete!\n\n");
    
    ts_console_printf("Files created:\n");
    ts_console_printf("  Private key: %s\n", output_path);
    ts_console_printf("  Public key:  %s\n", pub_path);
    ts_console_printf("\nPublic key (for authorized_keys):\n");
    ts_console_printf("─────────────────────────────────────────\n");
    ts_console_printf("%s", openssh_pub);
    ts_console_printf("─────────────────────────────────────────\n");
    
    ts_console_printf("\nUsage:\n");
    ts_console_printf("  1. Copy the public key above to remote server's ~/.ssh/authorized_keys\n");
    ts_console_printf("  2. Use: ssh --host <ip> --user <user> --key %s --exec <cmd>\n", output_path);
    
    free(openssh_pub);
    ts_crypto_keypair_free(keypair);
    
    return 0;
}

/*===========================================================================*/
/*                          Command Handler                                   */
/*===========================================================================*/

static int ssh_cmd_handler(int argc, char **argv)
{
    /* 解析参数 */
    int nerrors = arg_parse(argc, argv, (void **)&s_ssh_args);
    
    /* 显示帮助 */
    if (s_ssh_args.help->count > 0) {
        ts_console_printf("\n%s\n\n", TS_STR(TS_STR_SSH_HELP_USAGE));
        ts_console_printf("%s\n\n", TS_STR(TS_STR_SSH_HELP_DESC));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_CONN_OPTS));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_HOST));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_PORT));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_USER));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_PASSWORD));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_KEY));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_KEYID));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EXEC));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_SHELL));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_FORWARD));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_TEST));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_TIMEOUT));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_VERBOSE));
        ts_console_printf("\n%s\n", TS_STR(TS_STR_SSH_HELP_KEY_MGMT));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_KEYGEN));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_COPYID));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_REVOKE));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_TYPE));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_OUTPUT));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_COMMENT));
        ts_console_printf("\n%s\n", TS_STR(TS_STR_SSH_HELP_GENERAL));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_HELP_OPT));
        ts_console_printf("\n%s\n", TS_STR(TS_STR_SSH_HELP_EXAMPLES));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_KEYGEN));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_KEYGEN_CMD));
        ts_console_printf("  \n");
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_STORED));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_KEY_LIST));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_KEY_GEN));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_SSH));
        ts_console_printf("  \n");
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_COPYID));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_COPYID_CMD));
        ts_console_printf("  \n");
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_REVOKE));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_REVOKE_CMD));
        ts_console_printf("  \n");
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_FILE));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_EX_FILE_CMD));
        ts_console_printf("\n%s\n", TS_STR(TS_STR_SSH_HELP_NOTE));
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_NOTE_HOSTS));
        return 0;
    }
    
    /* 参数错误检查 */
    if (nerrors > 0) {
        arg_print_errors(stderr, s_ssh_args.end, "ssh");
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_USE));
        return 1;
    }
    
    /* 检查是否是密钥生成模式 */
    if (s_ssh_args.keygen->count > 0) {
        /* --keygen 模式：必须指定 --type 和 --output */
        if (s_ssh_args.type->count == 0) {
            ts_console_printf("Error: --type is required for key generation\n");
            ts_console_printf("Supported types: rsa, rsa2048, rsa4096, ecdsa, ec256, ec384\n");
            return 1;
        }
        if (s_ssh_args.output->count == 0) {
            ts_console_printf("Error: --output is required for key generation\n");
            ts_console_printf("Example: --output /sdcard/id_rsa\n");
            return 1;
        }
        
        const char *comment = (s_ssh_args.comment->count > 0) ? s_ssh_args.comment->sval[0] : NULL;
        return do_ssh_keygen(s_ssh_args.type->sval[0], s_ssh_args.output->sval[0], comment);
    }
    
    /* 检查是否是公钥部署模式 */
    if (s_ssh_args.copyid->count > 0) {
        /* --copyid 模式：必须指定 --host, --user, --password, 以及 --key 或 --keyid */
        if (s_ssh_args.host->count == 0) {
            ts_console_printf("Error: --host is required for --copyid\n");
            return 1;
        }
        if (s_ssh_args.user->count == 0) {
            ts_console_printf("Error: --user is required for --copyid\n");
            return 1;
        }
        if (s_ssh_args.password->count == 0) {
            ts_console_printf("Error: --password is required for --copyid (initial auth)\n");
            return 1;
        }
        if (s_ssh_args.key->count == 0 && s_ssh_args.keyid->count == 0) {
            ts_console_printf("Error: --key or --keyid is required for --copyid\n");
            ts_console_printf("  --key <path>   Use key file (public key at <path>.pub)\n");
            ts_console_printf("  --keyid <id>   Use key from secure storage\n");
            return 1;
        }
        
        int port = (s_ssh_args.port->count > 0) ? s_ssh_args.port->ival[0] : 22;
        int timeout = (s_ssh_args.timeout->count > 0) ? s_ssh_args.timeout->ival[0] : 10;
        const char *key_path = (s_ssh_args.key->count > 0) ? s_ssh_args.key->sval[0] : NULL;
        const char *keyid = (s_ssh_args.keyid->count > 0) ? s_ssh_args.keyid->sval[0] : NULL;
        
        return do_ssh_copy_id(s_ssh_args.host->sval[0], port, 
                              s_ssh_args.user->sval[0], s_ssh_args.password->sval[0],
                              key_path, keyid, timeout);
    }
    
    /* 检查是否是公钥撤销模式 */
    if (s_ssh_args.revoke->count > 0) {
        /* --revoke 模式：必须指定 --host, --user, --password, 以及 --key 或 --keyid */
        if (s_ssh_args.host->count == 0) {
            ts_console_printf("Error: --host is required for --revoke\n");
            return 1;
        }
        if (s_ssh_args.user->count == 0) {
            ts_console_printf("Error: --user is required for --revoke\n");
            return 1;
        }
        if (s_ssh_args.password->count == 0) {
            ts_console_printf("Error: --password is required for --revoke (auth to remove key)\n");
            return 1;
        }
        if (s_ssh_args.key->count == 0 && s_ssh_args.keyid->count == 0) {
            ts_console_printf("Error: --key or --keyid is required for --revoke\n");
            ts_console_printf("  --key <path>   Revoke key file (public key at <path>.pub)\n");
            ts_console_printf("  --keyid <id>   Revoke key from secure storage\n");
            return 1;
        }
        
        int port = (s_ssh_args.port->count > 0) ? s_ssh_args.port->ival[0] : 22;
        int timeout = (s_ssh_args.timeout->count > 0) ? s_ssh_args.timeout->ival[0] : 10;
        const char *key_path = (s_ssh_args.key->count > 0) ? s_ssh_args.key->sval[0] : NULL;
        const char *keyid = (s_ssh_args.keyid->count > 0) ? s_ssh_args.keyid->sval[0] : NULL;
        
        return do_ssh_revoke(s_ssh_args.host->sval[0], port, 
                             s_ssh_args.user->sval[0], s_ssh_args.password->sval[0],
                             key_path, keyid, timeout);
    }
    
    /* 连接模式：必需参数检查 */
    if (s_ssh_args.host->count == 0) {
        ts_console_printf("Error: --host is required\n");
        return 1;;
    }
    
    if (s_ssh_args.user->count == 0) {
        ts_console_printf("Error: --user is required\n");
        return 1;
    }
    
    /* 认证方式检查：必须提供 password、key 或 keyid 之一 */
    if (s_ssh_args.password->count == 0 && s_ssh_args.key->count == 0 && s_ssh_args.keyid->count == 0) {
        ts_console_printf("Error: --password, --key, or --keyid is required\n");
        return 1;
    }
    
    /* 获取参数值 */
    const char *host = s_ssh_args.host->sval[0];
    int port = (s_ssh_args.port->count > 0) ? s_ssh_args.port->ival[0] : 22;
    const char *user = s_ssh_args.user->sval[0];
    const char *password = (s_ssh_args.password->count > 0) ? s_ssh_args.password->sval[0] : NULL;
    const char *key_path = (s_ssh_args.key->count > 0) ? s_ssh_args.key->sval[0] : NULL;
    const char *keyid = (s_ssh_args.keyid->count > 0) ? s_ssh_args.keyid->sval[0] : NULL;
    int timeout = (s_ssh_args.timeout->count > 0) ? s_ssh_args.timeout->ival[0] : 10;
    bool verbose = (s_ssh_args.verbose->count > 0);
    
    /* 构建认证信息 */
    ssh_auth_info_t auth = {0};
    char *loaded_key_data = NULL;
    size_t loaded_key_len = 0;
    
    if (keyid && !key_path) {
        /* 从安全存储加载密钥到内存（不写入临时文件） */
        if (verbose) {
            ts_console_printf("Loading key '%s' from secure storage...\n", keyid);
        }
        
        esp_err_t ret = ts_keystore_load_private_key(keyid, &loaded_key_data, &loaded_key_len);
        if (ret != ESP_OK) {
            ts_console_printf("Error: Failed to load key '%s' from secure storage (%s)\n", 
                              keyid, esp_err_to_name(ret));
            return 1;
        }
        
        /* 使用内存中的密钥（不暴露到文件系统） */
        auth.key_data = (const uint8_t *)loaded_key_data;
        auth.key_len = loaded_key_len;
        auth.passphrase = NULL;  /* TODO: 支持加密密钥 */
        
        if (verbose) {
            ts_console_printf("Key loaded: %zu bytes\n", loaded_key_len);
            /* 显示密钥类型 */
            if (strstr(loaded_key_data, "BEGIN RSA PRIVATE KEY")) {
                ts_console_printf("Key type: RSA (PKCS#1)\n");
            } else if (strstr(loaded_key_data, "BEGIN EC PRIVATE KEY")) {
                ts_console_printf("Key type: ECDSA\n");
                ts_console_printf("  Warning: ECDSA key authentication from memory may not work\n");
                ts_console_printf("           with libssh2 mbedTLS backend. Use RSA keys if possible.\n");
            }
        }
    } else if (key_path) {
        /* 使用文件路径 */
        auth.key_path = key_path;
        auth.passphrase = NULL;
    } else {
        /* 密码认证 */
        auth.password = password;
    }
    
    int result = 0;
    
    /* 执行操作（按优先级） */
    if (s_ssh_args.shell->count > 0) {
        /* 交互式 Shell */
        result = do_ssh_shell(host, port, user, &auth, timeout, verbose);
    } else if (s_ssh_args.forward->count > 0) {
        /* 端口转发 */
        result = do_ssh_forward(host, port, user, &auth,
                              s_ssh_args.forward->sval[0], timeout, verbose);
    } else if (s_ssh_args.exec->count > 0) {
        /* 执行命令 */
        const char *command = s_ssh_args.exec->sval[0];
        result = do_ssh_exec(host, port, user, &auth, command, timeout, verbose);
    } else if (s_ssh_args.test->count > 0) {
        /* 测试连接 */
        result = do_ssh_test(host, port, user, &auth, timeout);
    } else {
        ts_console_printf("Error: Specify --exec, --shell, --forward, --test, or --keygen\n");
        ts_console_printf("%s\n", TS_STR(TS_STR_SSH_HELP_USE));
        result = 1;
    }
    
    /* 清理内存中的密钥数据 */
    if (loaded_key_data) {
        /* 安全清零内存 */
        memset(loaded_key_data, 0, loaded_key_len);
        free(loaded_key_data);
    }
    
    return result;
}

/*===========================================================================*/
/*                          Command Registration                              */
/*===========================================================================*/

esp_err_t ts_cmd_ssh_register(void)
{
    /* 连接相关参数 */
    s_ssh_args.host = arg_str0(NULL, "host", "<ip>", "Remote host address");
    s_ssh_args.port = arg_int0(NULL, "port", "<num>", "SSH port (default: 22)");
    s_ssh_args.user = arg_str0(NULL, "user", "<name>", "Username");
    s_ssh_args.password = arg_str0(NULL, "password", "<pwd>", "Password");
    s_ssh_args.key = arg_str0(NULL, "key", "<path>", "Private key file (PEM)");
    s_ssh_args.keyid = arg_str0(NULL, "keyid", "<id>", "Key ID from secure storage");
    s_ssh_args.exec = arg_str0(NULL, "exec", "<cmd>", "Command to execute");
    s_ssh_args.test = arg_lit0(NULL, "test", "Test SSH connection");
    s_ssh_args.shell = arg_lit0(NULL, "shell", "Open interactive shell");
    s_ssh_args.forward = arg_str0(NULL, "forward", "<spec>", "Port forward: L<local>:<host>:<port>");
    s_ssh_args.timeout = arg_int0(NULL, "timeout", "<sec>", "Timeout in seconds");
    s_ssh_args.verbose = arg_lit0("v", "verbose", "Verbose output");
    
    /* 密钥生成参数 */
    s_ssh_args.keygen = arg_lit0(NULL, "keygen", "Generate SSH key pair");
    s_ssh_args.copyid = arg_lit0(NULL, "copyid", "Deploy public key to remote server");
    s_ssh_args.revoke = arg_lit0(NULL, "revoke", "Remove public key from remote server");
    s_ssh_args.type = arg_str0(NULL, "type", "<type>", "Key type: rsa, rsa2048, rsa4096, ecdsa, ec256, ec384");
    s_ssh_args.output = arg_str0(NULL, "output", "<path>", "Output file path for private key");
    s_ssh_args.comment = arg_str0(NULL, "comment", "<text>", "Comment for the public key");
    
    /* 通用参数 */
    s_ssh_args.help = arg_lit0("h", "help", "Show help");
    s_ssh_args.end = arg_end(10);
    
    /* 注册命令 */
    const esp_console_cmd_t cmd = {
        .command = "ssh",
        .help = "SSH client. Use 'ssh --help' for details. Key management: use 'key' command.",
        .hint = NULL,
        .func = ssh_cmd_handler,
        .argtable = &s_ssh_args,
    };
    
    esp_err_t ret = esp_console_cmd_register(&cmd);
    if (ret != ESP_OK) {
        TS_LOGE(TAG, "Failed to register ssh command: %s", esp_err_to_name(ret));
    } else {
        TS_LOGI(TAG, "Registered command: ssh");
    }
    
    return ret;
}
