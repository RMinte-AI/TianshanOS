/**
 * @file ts_cmd_hosts.c
 * @brief SSH Hosts Management Commands
 * 
 * 实现 hosts 命令：管理 SSH 主机
 * 
 * 已知主机指纹管理（防止 MITM 攻击）：
 * - hosts --list                列出所有已知主机指纹
 * - hosts --remove --host <ip>  移除指定主机指纹
 * - hosts --clear               清空所有已知主机指纹
 * - hosts --info --host <ip>    显示主机指纹详情
 * 
 * 已部署主机管理（公钥部署凭证）：
 * - hosts --deployed            列出所有已部署主机
 * - hosts --deployed --remove --id <id>  移除已部署主机记录
 * - hosts --deployed --clear    清空所有已部署主机
 * 
 * @author TianShanOS Team
 * @version 3.0.0
 * @date 2026-01-27
 */

#include "ts_console.h"
#include "ts_api.h"
#include "ts_log.h"
#include "ts_known_hosts.h"
#include "ts_ssh_hosts_config.h"
#include "argtable3/argtable3.h"
#include <string.h>
#include <time.h>

#define TAG "cmd_hosts"

/* 最大主机数量 */
#define MAX_KNOWN_HOSTS 32

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *list;       /**< 列出所有主机 */
    struct arg_lit *remove;     /**< 移除主机 */
    struct arg_lit *clear;      /**< 清空所有主机 */
    struct arg_lit *info;       /**< 显示主机详情 */
    struct arg_lit *deployed;   /**< 操作已部署主机（而非指纹） */
    struct arg_str *host;       /**< 主机地址 */
    struct arg_str *id;         /**< 已部署主机 ID */
    struct arg_int *port;       /**< 端口（默认 22） */
    struct arg_lit *json;       /**< JSON 格式输出 */
    struct arg_lit *yes;        /**< 确认操作 */
    struct arg_lit *help;
    struct arg_end *end;
} s_hosts_args;

/* 最大已部署主机数量 */
#define MAX_DEPLOYED_HOSTS 16

/*===========================================================================*/
/*                          辅助函数                                          */
/*===========================================================================*/

/**
 * @brief 格式化时间戳
 */
static void format_time(uint32_t timestamp, char *buf, size_t buf_len)
{
    if (timestamp == 0) {
        strncpy(buf, "-", buf_len);
        return;
    }
    
    time_t t = (time_t)timestamp;
    struct tm *tm_info = localtime(&t);
    if (tm_info) {
        strftime(buf, buf_len, "%Y-%m-%d %H:%M", tm_info);
    } else {
        strncpy(buf, "Invalid", buf_len);
    }
}

/*===========================================================================*/
/*                          命令处理函数                                       */
/*===========================================================================*/

/**
 * @brief 列出所有已知主机
 */
static int do_hosts_list(bool json_output)
{
    /* JSON 模式使用 API */
    if (json_output) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("hosts.list", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"error\":\"%s\"}\n", result.message ? result.message : "Unknown error");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    ts_known_host_t hosts[MAX_KNOWN_HOSTS];
    size_t count = 0;
    
    esp_err_t ret = ts_known_hosts_list(hosts, MAX_KNOWN_HOSTS, &count);
    if (ret != ESP_OK) {
        ts_console_printf("Error: Failed to list known hosts (%s)\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("SSH Known Hosts\n");
    ts_console_printf("══════════════════════════════════════════════════════════════════════════\n");
    
    if (count == 0) {
        ts_console_printf("  No known hosts stored.\n");
        ts_console_printf("\n  Hosts are added automatically when connecting via SSH.\n");
    } else {
        ts_console_printf("  %-20s %-6s %-10s %-12s %s\n", 
                          "Host", "Port", "Type", "Added", "Fingerprint");
        ts_console_printf("  ──────────────────────────────────────────────────────────────────────────\n");
        
        for (size_t i = 0; i < count; i++) {
            char time_str[20];
            format_time(hosts[i].added_time, time_str, sizeof(time_str));
            
            /* 截断指纹显示 */
            char fp_short[20];
            snprintf(fp_short, sizeof(fp_short), "%.16s...", hosts[i].fingerprint);
            
            ts_console_printf("  %-20s %-6u %-10s %-12s %s\n",
                hosts[i].host,
                hosts[i].port,
                ts_host_key_type_str(hosts[i].type),
                time_str,
                fp_short);
        }
    }
    
    ts_console_printf("══════════════════════════════════════════════════════════════════════════\n");
    ts_console_printf("  Total: %zu hosts\n\n", count);
    
    return 0;
}

/**
 * @brief 显示主机详情
 */
static int do_hosts_info(const char *host, int port, bool json_output)
{
    if (!host || strlen(host) == 0) {
        ts_console_printf("Error: --host is required\n");
        return 1;
    }
    
    /* JSON 模式使用 API */
    if (json_output) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "host", host);
        cJSON_AddNumberToObject(params, "port", port);
        
        esp_err_t ret = ts_api_call("hosts.info", params, &result);
        cJSON_Delete(params);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"error\":\"%s\"}\n", result.message ? result.message : "Host not found");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    ts_known_host_t info;
    esp_err_t ret = ts_known_hosts_get(host, port, &info);
    
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_console_printf("Error: Host '%s:%d' not found in known hosts\n", host, port);
        return 1;
    } else if (ret != ESP_OK) {
        ts_console_printf("Error: Failed to get host info (%s)\n", esp_err_to_name(ret));
        return 1;
    }
    
    char time_str[20];
    format_time(info.added_time, time_str, sizeof(time_str));
    
    ts_console_printf("\n");
    ts_console_printf("Known Host Information\n");
    ts_console_printf("═══════════════════════════════════════════════════════════════\n");
    ts_console_printf("  Host:        %s\n", info.host);
    ts_console_printf("  Port:        %u\n", info.port);
    ts_console_printf("  Key Type:    %s\n", ts_host_key_type_str(info.type));
    ts_console_printf("  Fingerprint: SHA256:%s\n", info.fingerprint);
    ts_console_printf("  Added:       %s\n", time_str);
    ts_console_printf("═══════════════════════════════════════════════════════════════\n\n");
    
    return 0;
}

/**
 * @brief 移除指定主机
 */
static int do_hosts_remove(const char *host, int port)
{
    if (!host || strlen(host) == 0) {
        ts_console_printf("Error: --host is required\n");
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Remove Known Host\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Host: %s\n", host);
    ts_console_printf("  Port: %d\n", port);
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    ts_console_printf("Removing... ");
    
    esp_err_t ret = ts_known_hosts_remove(host, port);
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_console_printf("NOT FOUND\n");
        ts_console_printf("  Host '%s:%d' was not in known hosts\n", host, port);
        return 1;
    } else if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("OK\n\n");
    ts_console_printf("✓ Host '%s:%d' removed from known hosts\n\n", host, port);
    
    return 0;
}

/**
 * @brief 清空所有已知主机
 */
static int do_hosts_clear(bool confirmed)
{
    size_t count = ts_known_hosts_count();
    
    if (count == 0) {
        ts_console_printf("No known hosts to clear.\n");
        return 0;
    }
    
    if (!confirmed) {
        ts_console_printf("\n");
        ts_console_printf("⚠ WARNING: This will remove ALL %zu known hosts!\n", count);
        ts_console_printf("\n");
        ts_console_printf("To confirm, run: hosts --clear --yes\n\n");
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Clear All Known Hosts\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Hosts to remove: %zu\n", count);
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    ts_console_printf("Clearing... ");
    
    esp_err_t ret = ts_known_hosts_clear();
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("OK\n\n");
    ts_console_printf("✓ All %zu known hosts removed\n\n", count);
    
    return 0;
}

/*===========================================================================*/
/*                     已部署主机命令处理函数                                   */
/*===========================================================================*/

/**
 * @brief 列出所有已部署主机
 */
static int do_deployed_list(bool json_output)
{
    /* JSON 模式使用 API */
    if (json_output) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("ssh.hosts.list", NULL, &result);
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"error\":\"%s\"}\n", result.message ? result.message : "Unknown error");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    ts_ssh_host_config_t *hosts = heap_caps_malloc(sizeof(ts_ssh_host_config_t) * MAX_DEPLOYED_HOSTS, 
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hosts) {
        hosts = malloc(sizeof(ts_ssh_host_config_t) * MAX_DEPLOYED_HOSTS);
    }
    if (!hosts) {
        ts_console_printf("Error: Failed to allocate memory\n");
        return 1;
    }
    
    size_t count = 0;
    esp_err_t ret = ts_ssh_hosts_config_list(hosts, MAX_DEPLOYED_HOSTS, &count);
    if (ret != ESP_OK) {
        ts_console_printf("Error: Failed to list deployed hosts (%s)\n", esp_err_to_name(ret));
        free(hosts);
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Deployed SSH Hosts (Public Key Deployed)\n");
    ts_console_printf("══════════════════════════════════════════════════════════════════════════\n");
    
    if (count == 0) {
        ts_console_printf("  No deployed hosts recorded.\n");
        ts_console_printf("\n  Use 'ssh --deploy --host <ip>' to deploy public key to a host.\n");
    } else {
        ts_console_printf("  %-14s %-20s %-8s %-10s %s\n", 
                          "ID", "Host", "User", "Deployed", "Key");
        ts_console_printf("  ──────────────────────────────────────────────────────────────────────────\n");
        
        for (size_t i = 0; i < count; i++) {
            char time_str[20];
            format_time(hosts[i].created_time, time_str, sizeof(time_str));
            
            ts_console_printf("  %-14s %-20s %-8s %-10s %s\n",
                hosts[i].id,
                hosts[i].host,
                hosts[i].username,
                time_str,
                hosts[i].keyid);
        }
    }
    
    ts_console_printf("══════════════════════════════════════════════════════════════════════════\n");
    ts_console_printf("  Total: %zu deployed hosts\n\n", count);
    
    free(hosts);
    return 0;
}

/**
 * @brief 移除指定已部署主机记录
 */
static int do_deployed_remove(const char *id)
{
    if (!id || strlen(id) == 0) {
        ts_console_printf("Error: --id is required\n");
        ts_console_printf("Use 'hosts --deployed' to list all deployed hosts and their IDs\n");
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Remove Deployed Host Record\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  ID: %s\n", id);
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    ts_console_printf("⚠ Note: This only removes the LOCAL record.\n");
    ts_console_printf("  The public key remains on the remote host.\n");
    ts_console_printf("  Use 'ssh --revoke --id %s' to revoke the key first.\n\n", id);
    
    ts_console_printf("Removing... ");
    
    esp_err_t ret = ts_ssh_hosts_config_remove(id);
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_console_printf("NOT FOUND\n");
        ts_console_printf("  Deployed host '%s' not found\n", id);
        return 1;
    } else if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("OK\n\n");
    ts_console_printf("✓ Deployed host record '%s' removed\n\n", id);
    
    return 0;
}

/**
 * @brief 清空所有已部署主机记录
 */
static int do_deployed_clear(bool confirmed)
{
    size_t count = ts_ssh_hosts_config_count();
    
    if (count == 0) {
        ts_console_printf("No deployed hosts to clear.\n");
        return 0;
    }
    
    if (!confirmed) {
        ts_console_printf("\n");
        ts_console_printf("⚠ WARNING: This will remove ALL %zu deployed host records!\n", count);
        ts_console_printf("  The public keys will remain on remote hosts.\n");
        ts_console_printf("\n");
        ts_console_printf("To confirm, run: hosts --deployed --clear --yes\n\n");
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Clear All Deployed Host Records\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Records to remove: %zu\n", count);
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    ts_console_printf("Clearing... ");
    
    esp_err_t ret = ts_ssh_hosts_config_clear();
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("OK\n\n");
    ts_console_printf("✓ All %zu deployed host records removed\n\n", count);
    
    return 0;
}

/**
 * @brief 显示帮助
 */
static void show_help(void)
{
    ts_console_printf("\n");
    ts_console_printf("SSH Hosts Management\n");
    ts_console_printf("════════════════════════════════════════════════════════════════\n");
    ts_console_printf("\n");
    ts_console_printf("Known Hosts Fingerprints (MITM Protection):\n");
    ts_console_printf("  hosts --list                              List all known host fingerprints\n");
    ts_console_printf("  hosts --info --host <ip> [--port <n>]     Show fingerprint details\n");
    ts_console_printf("  hosts --remove --host <ip> [--port <n>]   Remove a fingerprint\n");
    ts_console_printf("  hosts --clear --yes                       Clear all fingerprints\n");
    ts_console_printf("\n");
    ts_console_printf("Deployed Hosts (Public Key Deployment Records):\n");
    ts_console_printf("  hosts --deployed                          List all deployed hosts\n");
    ts_console_printf("  hosts --deployed --remove --id <id>       Remove a deployment record\n");
    ts_console_printf("  hosts --deployed --clear --yes            Clear all deployment records\n");
    ts_console_printf("\n");
    ts_console_printf("Options:\n");
    ts_console_printf("  --host <ip>       Hostname or IP address\n");
    ts_console_printf("  --port <num>      Port number (default: 22)\n");
    ts_console_printf("  --id <id>         Deployed host ID\n");
    ts_console_printf("  --deployed        Operate on deployed hosts (not fingerprints)\n");
    ts_console_printf("  --yes             Confirm dangerous operations\n");
    ts_console_printf("  --json            Output in JSON format\n");
    ts_console_printf("\n");
    ts_console_printf("Security Notes:\n");
    ts_console_printf("  Known Hosts:\n");
    ts_console_printf("    - Stores SSH server fingerprints to prevent MITM attacks\n");
    ts_console_printf("    - Hosts are added automatically on first SSH connection\n");
    ts_console_printf("    - Remove only if server was legitimately reinstalled\n");
    ts_console_printf("\n");
    ts_console_printf("  Deployed Hosts:\n");
    ts_console_printf("    - Records which hosts have our public key deployed\n");
    ts_console_printf("    - Removing record does NOT revoke the key from remote host\n");
    ts_console_printf("    - Use 'ssh --revoke' to remove key from remote host first\n");
    ts_console_printf("════════════════════════════════════════════════════════════════\n\n");
}

/*===========================================================================*/
/*                          Command Handler                                   */
/*===========================================================================*/

static int hosts_cmd_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_hosts_args);
    
    /* 显示帮助 */
    if (s_hosts_args.help->count > 0) {
        show_help();
        return 0;
    }
    
    /* 检查解析错误 */
    if (nerrors > 0) {
        arg_print_errors(stderr, s_hosts_args.end, "hosts");
        ts_console_printf("Use 'hosts --help' for usage information\n");
        return 1;
    }
    
    /* 获取通用参数 */
    const char *host = (s_hosts_args.host->count > 0) ? s_hosts_args.host->sval[0] : NULL;
    const char *id = (s_hosts_args.id->count > 0) ? s_hosts_args.id->sval[0] : NULL;
    int port = (s_hosts_args.port->count > 0) ? s_hosts_args.port->ival[0] : 22;
    bool json_output = (s_hosts_args.json->count > 0);
    bool confirmed = (s_hosts_args.yes->count > 0);
    bool is_deployed = (s_hosts_args.deployed->count > 0);
    
    /* 已部署主机模式 */
    if (is_deployed) {
        if (s_hosts_args.clear->count > 0) {
            return do_deployed_clear(confirmed);
        }
        
        if (s_hosts_args.remove->count > 0) {
            return do_deployed_remove(id);
        }
        
        /* 默认列出已部署主机 */
        return do_deployed_list(json_output);
    }
    
    /* 已知主机指纹模式 */
    if (s_hosts_args.clear->count > 0) {
        return do_hosts_clear(confirmed);
    }
    
    if (s_hosts_args.remove->count > 0) {
        return do_hosts_remove(host, port);
    }
    
    if (s_hosts_args.info->count > 0) {
        return do_hosts_info(host, port, json_output);
    }
    
    /* 默认显示列表 */
    return do_hosts_list(json_output);
}

/*===========================================================================*/
/*                          Command Registration                              */
/*===========================================================================*/

esp_err_t ts_cmd_hosts_register(void)
{
    /* 初始化参数表 */
    s_hosts_args.list = arg_lit0("l", "list", "List all hosts");
    s_hosts_args.remove = arg_lit0("r", "remove", "Remove a host");
    s_hosts_args.clear = arg_lit0(NULL, "clear", "Clear all hosts");
    s_hosts_args.info = arg_lit0("i", "info", "Show host details");
    s_hosts_args.deployed = arg_lit0("d", "deployed", "Operate on deployed hosts (not fingerprints)");
    s_hosts_args.host = arg_str0("H", "host", "<ip>", "Hostname or IP address");
    s_hosts_args.id = arg_str0(NULL, "id", "<id>", "Deployed host ID");
    s_hosts_args.port = arg_int0("p", "port", "<num>", "Port number (default: 22)");
    s_hosts_args.json = arg_lit0("j", "json", "JSON format output");
    s_hosts_args.yes = arg_lit0("y", "yes", "Confirm dangerous operations");
    s_hosts_args.help = arg_lit0("h", "help", "Show help");
    s_hosts_args.end = arg_end(5);
    
    /* 注册命令 */
    const esp_console_cmd_t cmd = {
        .command = "hosts",
        .help = "Manage SSH hosts (fingerprints and deployment records)",
        .hint = NULL,
        .func = &hosts_cmd_handler,
        .argtable = &s_hosts_args,
    };
    
    esp_err_t ret = esp_console_cmd_register(&cmd);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Registered command: hosts");
    }
    
    return ret;
}
