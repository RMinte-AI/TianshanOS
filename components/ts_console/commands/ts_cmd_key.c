/**
 * @file ts_cmd_key.c
 * @brief Key Management Console Commands
 * 
 * 实现 key 命令：密钥管理（与 SSH 解耦的独立命令）
 * - key --list                列出所有存储的密钥
 * - key --info --id <name>    查看密钥详情
 * - key --import --id <name> --file <path>  从文件导入密钥
 * - key --generate --id <name> --type <type> 生成新密钥
 * - key --delete --id <name>  删除密钥
 * - key --export --id <name> --output <path> 导出公钥
 * 
 * @author TianShanOS Team
 * @version 2.0.0
 * @date 2026-01-18
 */

#include "ts_storage.h"
#include "ts_console.h"
#include "ts_api.h"
#include "ts_log.h"
#include "ts_keystore.h"
#include "argtable3/argtable3.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TAG "cmd_key"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *list;       /**< 列出所有密钥 */
    struct arg_lit *info;       /**< 查看密钥详情 */
    struct arg_lit *import;     /**< 导入密钥 */
    struct arg_lit *generate;   /**< 生成密钥 */
    struct arg_lit *delete;     /**< 删除密钥 */
    struct arg_lit *export;     /**< 导出公钥 */
    struct arg_lit *export_priv; /**< 导出私钥（需 exportable） */
    struct arg_lit *use_for_https; /**< 将密钥部署到 HTTPS */
    struct arg_str *id;         /**< 密钥 ID */
    struct arg_str *file;       /**< 文件路径（导入用） */
    struct arg_str *output;     /**< 输出路径（导出用） */
    struct arg_str *type;       /**< 密钥类型 */
    struct arg_str *comment;    /**< 注释 */
    struct arg_str *cn;         /**< 证书 Common Name (用于 HTTPS) */
    struct arg_str *org;        /**< 证书 Organization (用于 HTTPS) */
    struct arg_str *country;    /**< 证书 Country (用于 HTTPS) */
    struct arg_int *days;       /**< 证书有效天数 (用于 HTTPS) */
    struct arg_lit *exportable; /**< 允许私钥导出 */
    struct arg_lit *json;       /**< JSON 格式输出 */
    struct arg_lit *help;
    struct arg_end *end;
} s_key_args;

/*===========================================================================*/
/*                          辅助函数                                          */
/*===========================================================================*/

/**
 * @brief 获取密钥类型描述
 */
static const char *get_key_type_desc(ts_keystore_key_type_t type)
{
    switch (type) {
        case TS_KEYSTORE_TYPE_RSA_2048:  return "RSA 2048-bit";
        case TS_KEYSTORE_TYPE_RSA_4096:  return "RSA 4096-bit";
        case TS_KEYSTORE_TYPE_ECDSA_P256: return "ECDSA P-256";
        case TS_KEYSTORE_TYPE_ECDSA_P384: return "ECDSA P-384";
        default: return "Unknown";
    }
}

/**
 * @brief 转换密钥类型：字符串 -> ts_keystore_key_type_t
 */
static bool parse_key_type(const char *type_str, ts_keystore_key_type_t *type)
{
    if (!type_str || !type) return false;
    
    if (strcmp(type_str, "rsa") == 0 || strcmp(type_str, "rsa2048") == 0) {
        *type = TS_KEYSTORE_TYPE_RSA_2048;
        return true;
    } else if (strcmp(type_str, "rsa4096") == 0) {
        *type = TS_KEYSTORE_TYPE_RSA_4096;
        return true;
    } else if (strcmp(type_str, "ec256") == 0 || strcmp(type_str, "ecdsa") == 0 || 
               strcmp(type_str, "ecdsa256") == 0) {
        *type = TS_KEYSTORE_TYPE_ECDSA_P256;
        return true;
    } else if (strcmp(type_str, "ec384") == 0 || strcmp(type_str, "ecdsa384") == 0) {
        *type = TS_KEYSTORE_TYPE_ECDSA_P384;
        return true;
    }
    return false;
}

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
 * @brief 列出所有存储的密钥
 */
static int do_key_list(bool json_output)
{
    /* JSON 模式使用 API */
    if (json_output) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        esp_err_t ret = ts_api_call("key.list", NULL, &result);
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
    
    /* 格式化输出 - 使用 PSRAM 分配避免栈溢出 */
    ts_keystore_key_info_t *keys = heap_caps_malloc(
        sizeof(ts_keystore_key_info_t) * TS_KEYSTORE_MAX_KEYS, 
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!keys) {
        keys = malloc(sizeof(ts_keystore_key_info_t) * TS_KEYSTORE_MAX_KEYS);
    }
    if (!keys) {
        ts_console_printf("Error: Memory allocation failed\n");
        return 1;
    }
    size_t count = TS_KEYSTORE_MAX_KEYS;
    
    esp_err_t ret = ts_keystore_list_keys(keys, &count);
    if (ret != ESP_OK) {
        free(keys);
        ts_console_printf("Error: Failed to list keys (%s)\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Secure Key Storage\n");
    ts_console_printf("══════════════════════════════════════════════════════════════════\n");
    
    if (count == 0) {
        ts_console_printf("  No keys stored.\n");
        ts_console_printf("\n  To import a key:   key --import --id <name> --file <path>\n");
        ts_console_printf("  To generate a key: key --generate --id <name> --type <type>\n");
    } else {
        ts_console_printf("  %-16s %-14s %-20s %s\n", "ID", "Type", "Created", "Comment");
        ts_console_printf("  ────────────────────────────────────────────────────────────────\n");
        
        for (size_t i = 0; i < count; i++) {
            char time_str[20];
            format_time(keys[i].created_at, time_str, sizeof(time_str));
            
            ts_console_printf("  %-16s %-14s %-20s %s\n",
                keys[i].id,
                get_key_type_desc(keys[i].type),
                time_str,
                keys[i].comment[0] ? keys[i].comment : "-");
        }
    }
    
    ts_console_printf("══════════════════════════════════════════════════════════════════\n");
    ts_console_printf("  Total: %zu / %d keys\n\n", count, TS_KEYSTORE_MAX_KEYS);
    
    free(keys);
    return 0;
}

/**
 * @brief 查看密钥详情
 */
static int do_key_info(const char *key_id, bool json_output)
{
    if (!key_id || strlen(key_id) == 0) {
        ts_console_printf("Error: --id is required\n");
        return 1;
    }
    
    /* JSON 模式使用 API */
    if (json_output) {
        ts_api_result_t result;
        ts_api_result_init(&result);
        
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "id", key_id);
        
        esp_err_t ret = ts_api_call("key.info", params, &result);
        cJSON_Delete(params);
        
        if (ret == ESP_OK && result.code == TS_API_OK && result.data) {
            char *json_str = cJSON_PrintUnformatted(result.data);
            if (json_str) {
                ts_console_printf("%s\n", json_str);
                free(json_str);
            }
        } else {
            ts_console_printf("{\"error\":\"%s\"}\n", result.message ? result.message : "Key not found");
        }
        ts_api_result_free(&result);
        return (ret == ESP_OK) ? 0 : 1;
    }
    
    /* 格式化输出 */
    ts_keystore_key_info_t info;
    esp_err_t ret = ts_keystore_get_key_info(key_id, &info);
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_console_printf("Error: Key '%s' not found\n", key_id);
        return 1;
    } else if (ret != ESP_OK) {
        ts_console_printf("Error: Failed to get key info (%s)\n", esp_err_to_name(ret));
        return 1;
    }
    
    char created_str[20], used_str[20];
    format_time(info.created_at, created_str, sizeof(created_str));
    format_time(info.last_used, used_str, sizeof(used_str));
    
    ts_console_printf("\n");
    ts_console_printf("Key Information\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  ID:          %s\n", info.id);
    ts_console_printf("  Type:        %s\n", get_key_type_desc(info.type));
    ts_console_printf("  Comment:     %s\n", info.comment[0] ? info.comment : "-");
    ts_console_printf("  Created:     %s\n", created_str);
    ts_console_printf("  Last Used:   %s\n", used_str);
    ts_console_printf("  Public Key:  %s\n", info.has_public_key ? "Yes" : "No");
    ts_console_printf("  Exportable:  %s\n", info.exportable ? "Yes (private key can be exported)" : "No");
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    return 0;
}

/**
 * @brief 导入密钥
 */
static int do_key_import(const char *key_id, const char *file_path, const char *comment)
{
    if (!key_id || strlen(key_id) == 0) {
        ts_console_printf("Error: --id is required\n");
        return 1;
    }
    
    if (!file_path || strlen(file_path) == 0) {
        ts_console_printf("Error: --file is required for import\n");
        return 1;
    }
    
    /* 检查 ID 长度 */
    if (strlen(key_id) >= TS_KEYSTORE_ID_MAX_LEN) {
        ts_console_printf("Error: Key ID too long (max %d chars)\n", TS_KEYSTORE_ID_MAX_LEN - 1);
        return 1;
    }
    
    /* 检查密钥是否已存在 */
    if (ts_keystore_key_exists(key_id)) {
        ts_console_printf("Error: Key '%s' already exists. Delete it first.\n", key_id);
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Import Key to Secure Storage\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  ID:      %s\n", key_id);
    ts_console_printf("  File:    %s\n", file_path);
    if (comment) {
        ts_console_printf("  Comment: %s\n", comment);
    }
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    ts_console_printf("Importing... ");
    
    esp_err_t ret = ts_keystore_import_from_file(key_id, file_path, comment);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("OK\n\n");
    ts_console_printf("✓ Key '%s' imported successfully\n", key_id);
    ts_console_printf("\n");
    ts_console_printf("Usage:\n");
    ts_console_printf("  ssh --host <ip> --user <name> --keyid %s --shell\n\n", key_id);
    
    return 0;
}

/**
 * @brief 生成密钥
 */
static int do_key_generate(const char *key_id, const char *type_str, const char *comment, bool exportable)
{
    if (!key_id || strlen(key_id) == 0) {
        ts_console_printf("Error: --id is required\n");
        return 1;
    }
    
    if (!type_str || strlen(type_str) == 0) {
        ts_console_printf("Error: --type is required for generate\n");
        ts_console_printf("Supported types: rsa, rsa2048, rsa4096, ecdsa, ec256, ec384\n");
        return 1;
    }
    
    ts_keystore_key_type_t key_type;
    if (!parse_key_type(type_str, &key_type)) {
        ts_console_printf("Error: Invalid key type '%s'\n", type_str);
        ts_console_printf("Supported types: rsa, rsa2048, rsa4096, ecdsa, ec256, ec384\n");
        return 1;
    }
    
    /* 检查 ID 长度 */
    if (strlen(key_id) >= TS_KEYSTORE_ID_MAX_LEN) {
        ts_console_printf("Error: Key ID too long (max %d chars)\n", TS_KEYSTORE_ID_MAX_LEN - 1);
        return 1;
    }
    
    /* 检查密钥是否已存在 */
    if (ts_keystore_key_exists(key_id)) {
        ts_console_printf("Error: Key '%s' already exists. Delete it first.\n", key_id);
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Generate Key in Secure Storage\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  ID:         %s\n", key_id);
    ts_console_printf("  Type:       %s\n", get_key_type_desc(key_type));
    ts_console_printf("  Exportable: %s\n", exportable ? "Yes" : "No");
    if (comment) {
        ts_console_printf("  Comment:    %s\n", comment);
    }
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    if (exportable) {
        ts_console_printf("⚠ WARNING: Private key will be exportable!\n");
        ts_console_printf("  Only use --exportable for backup purposes.\n\n");
    }
    
    if (key_type == TS_KEYSTORE_TYPE_RSA_4096) {
        ts_console_printf("Generating key pair (this may take 30-60 seconds)... ");
    } else {
        ts_console_printf("Generating key pair... ");
    }
    
    /* 使用扩展 API 生成密钥 */
    ts_keystore_gen_opts_t opts = {
        .exportable = exportable,
        .comment = comment,
    };
    
    esp_err_t ret = ts_keystore_generate_key_ex(key_id, key_type, &opts);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("OK\n\n");
    ts_console_printf("✓ Key '%s' generated and stored successfully\n", key_id);
    ts_console_printf("\n");
    ts_console_printf("Usage:\n");
    ts_console_printf("  key --export --id %s --output /sdcard/%s.pub  # Export public key\n", key_id, key_id);
    if (exportable) {
        ts_console_printf("  key --export-priv --id %s --output /sdcard/%s  # Export private key (backup)\n", key_id, key_id);
    }
    ts_console_printf("  ssh --host <ip> --user <name> --keyid %s --shell  # Use for SSH\n\n", key_id);
    
    return 0;
}

/**
 * @brief 删除密钥
 */
static int do_key_delete(const char *key_id)
{
    if (!key_id || strlen(key_id) == 0) {
        ts_console_printf("Error: --id is required\n");
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Delete Key from Secure Storage\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Key ID: %s\n", key_id);
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    ts_console_printf("Deleting... ");
    
    esp_err_t ret = ts_keystore_delete_key(key_id);
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: Key '%s' not found\n", key_id);
        return 1;
    } else if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("OK\n\n");
    ts_console_printf("✓ Key '%s' deleted from secure storage\n\n", key_id);
    
    return 0;
}

/**
 * @brief 导出公钥
 */
static int do_key_export(const char *key_id, const char *output_path)
{
    if (!key_id || strlen(key_id) == 0) {
        ts_console_printf("Error: --id is required\n");
        return 1;
    }
    
    if (!output_path || strlen(output_path) == 0) {
        ts_console_printf("Error: --output is required for export\n");
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("Export Public Key\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Key ID: %s\n", key_id);
    ts_console_printf("  Output: %s\n", output_path);
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    ts_console_printf("[1/2] Loading public key... ");
    
    /* 加载公钥 */
    char *public_key = NULL;
    size_t public_key_len = 0;
    esp_err_t ret = ts_keystore_load_public_key(key_id, &public_key, &public_key_len);
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: Key '%s' not found or has no public key\n", key_id);
        return 1;
    } else if (ret != ESP_OK || !public_key) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", esp_err_to_name(ret));
        return 1;
    }
    ts_console_printf("OK\n");
    
    ts_console_printf("[2/2] Writing to file... ");
    
    /* 写入文件 */
    ret = ts_storage_write_file(output_path, public_key, public_key_len);
    free(public_key);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf(ret == ESP_ERR_NOT_ALLOWED
            ? "  This file belongs to Automation. Choose another export path.\n"
            : "  Could not save the public key. Check the path and storage.\n");
        return 1;
    }

    ts_console_printf("OK\n\n");
    ts_console_printf("✓ Public key exported to: %s\n", output_path);
    ts_console_printf("\n");
    ts_console_printf("To deploy to remote server:\n");
    ts_console_printf("  ssh --copyid --host <ip> --user <name> --password <pwd> --key %s\n\n",
                      output_path);
    
    return 0;
}

/**
 * @brief 导出私钥（需要 exportable=true）
 */
static int do_key_export_private(const char *key_id, const char *output_path)
{
    if (!key_id || strlen(key_id) == 0) {
        ts_console_printf("Error: --id is required\n");
        return 1;
    }
    
    if (!output_path || strlen(output_path) == 0) {
        ts_console_printf("Error: --output is required for export\n");
        return 1;
    }
    
    esp_err_t ret = ts_storage_check_write_path(output_path, false);
    if (ret != ESP_OK) {
        ts_console_printf(ret == ESP_ERR_NOT_ALLOWED
            ? "This file belongs to Automation. Choose another export path.\n" : "Invalid export path.\n");
        return 1;
    }

    /* 检查密钥是否存在并获取 exportable 状态 */
    ts_keystore_key_info_t info;
    ret = ts_keystore_get_key_info(key_id, &info);
    if (ret == ESP_ERR_NOT_FOUND) {
        ts_console_printf("Error: Key '%s' not found\n", key_id);
        return 1;
    }
    
    if (!info.exportable) {
        ts_console_printf("\n");
        ts_console_printf("Error: Key '%s' is not exportable\n", key_id);
        ts_console_printf("\n");
        ts_console_printf("Security policy: Private keys cannot be exported unless\n");
        ts_console_printf("generated with --exportable flag.\n");
        ts_console_printf("\n");
        ts_console_printf("To create an exportable key:\n");
        ts_console_printf("  key --generate --id <name> --type <type> --exportable\n\n");
        return 1;
    }
    
    ts_console_printf("\n");
    ts_console_printf("⚠ Export Private Key (Security Sensitive)\n");
    ts_console_printf("═══════════════════════════════════════\n");
    ts_console_printf("  Key ID: %s\n", key_id);
    ts_console_printf("  Output: %s\n", output_path);
    ts_console_printf("═══════════════════════════════════════\n\n");
    
    ts_console_printf("WARNING: Private key will be written to file!\n");
    ts_console_printf("         Ensure secure handling and delete after use.\n\n");
    
    ts_console_printf("Exporting private key... ");
    
    ret = ts_keystore_export_private_key_to_file(key_id, output_path);
    if (ret != ESP_OK) {
        ts_console_printf("FAILED\n");
        ts_console_printf("  Error: %s\n", esp_err_to_name(ret));
        return 1;
    }
    
    ts_console_printf("OK\n\n");
    ts_console_printf("✓ Private key exported to: %s\n", output_path);
    ts_console_printf("\n");
    ts_console_printf("Security reminder:\n");
    ts_console_printf("  - Keep this file secure (set permissions to 600)\n");
    ts_console_printf("  - Delete the file after import/backup\n");
    ts_console_printf("  - Consider encrypting the backup\n\n");
    
    return 0;
}

/**
 * @brief 显示帮助
 */
static void show_help(void)
{
    ts_console_printf("\n");
    ts_console_printf("Key Management (Secure Storage)\n");
    ts_console_printf("════════════════════════════════════════════════════════════════\n");
    ts_console_printf("\n");
    ts_console_printf("Usage:\n");
    ts_console_printf("  key --list                              List all stored keys\n");
    ts_console_printf("  key --info --id <name>                  Show key details\n");
    ts_console_printf("  key --import --id <name> --file <path>  Import key from file\n");
    ts_console_printf("  key --generate --id <name> --type <t>   Generate and store new key\n");
    ts_console_printf("  key --delete --id <name>                Delete stored key\n");
    ts_console_printf("  key --export --id <name> --output <f>   Export public key to file\n");
    ts_console_printf("  key --export-priv --id <name> -o <f>    Export private key (if exportable)\n");
    ts_console_printf("\n");
    ts_console_printf("Options:\n");
    ts_console_printf("  --id <name>       Key identifier (max 31 chars)\n");
    ts_console_printf("  --file <path>     Private key file path (for import)\n");
    ts_console_printf("  --output <path>   Output file path (for export)\n");
    ts_console_printf("  --type <type>     Key type for generation\n");
    ts_console_printf("  --comment <text>  Optional comment/description\n");
    ts_console_printf("  --exportable      Allow private key export (for generate)\n");
    ts_console_printf("  --json            Output in JSON format\n");
    ts_console_printf("\n");
    ts_console_printf("Key Types (for --type):\n");
    ts_console_printf("  rsa, rsa2048      RSA 2048-bit (recommended for compatibility)\n");
    ts_console_printf("  rsa4096           RSA 4096-bit (slower generation, ~60s)\n");
    ts_console_printf("  ecdsa, ec256      ECDSA P-256 (fast, secure)\n");
    ts_console_printf("  ec384             ECDSA P-384 (high security)\n");
    ts_console_printf("\n");
    ts_console_printf("Examples:\n");
    ts_console_printf("  # Generate non-exportable key (recommended for production)\n");
    ts_console_printf("  key --generate --id agx --type ecdsa --comment \"AGX production\"\n");
    ts_console_printf("\n");
    ts_console_printf("  # Generate exportable key (for backup purposes)\n");
    ts_console_printf("  key --generate --id backup --type rsa4096 --exportable\n");
    ts_console_printf("\n");
    ts_console_printf("  # Import existing key\n");
    ts_console_printf("  key --import --id backup --file /sdcard/id_rsa\n");
    ts_console_printf("\n");
    ts_console_printf("  # Export public key for deployment\n");
    ts_console_printf("  key --export --id agx --output /sdcard/agx.pub\n");
    ts_console_printf("\n");
    ts_console_printf("  # Export private key (only if exportable=true)\n");
    ts_console_printf("  key --export-priv --id backup --output /sdcard/backup.pem\n");
    ts_console_printf("\n");
    ts_console_printf("  # Use with SSH\n");
    ts_console_printf("  ssh --host 10.10.99.100 --user nvidia --keyid agx --shell\n");
    ts_console_printf("\n");
    ts_console_printf("Security Notes:\n");
    ts_console_printf("  - Private keys stored in ESP32 NVS (encrypted when enabled)\n");
    ts_console_printf("  - Max %d keys supported\n", TS_KEYSTORE_MAX_KEYS);
    ts_console_printf("  - Keys persist across reboots\n");
    ts_console_printf("  - Non-exportable keys provide stronger security\n");
    ts_console_printf("════════════════════════════════════════════════════════════════\n\n");
}

/*===========================================================================*/
/*                          Command Handler                                   */
/*===========================================================================*/

static int key_cmd_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_key_args);
    
    /* 显示帮助 */
    if (s_key_args.help->count > 0) {
        show_help();
        return 0;
    }
    
    /* 检查解析错误 */
    if (nerrors > 0) {
        arg_print_errors(stderr, s_key_args.end, "key");
        ts_console_printf("Use 'key --help' for usage information\n");
        return 1;
    }
    
    /* 获取通用参数 */
    const char *key_id = (s_key_args.id->count > 0) ? s_key_args.id->sval[0] : NULL;
    const char *file_path = (s_key_args.file->count > 0) ? s_key_args.file->sval[0] : NULL;
    const char *output_path = (s_key_args.output->count > 0) ? s_key_args.output->sval[0] : NULL;
    const char *type_str = (s_key_args.type->count > 0) ? s_key_args.type->sval[0] : NULL;
    const char *comment = (s_key_args.comment->count > 0) ? s_key_args.comment->sval[0] : NULL;
    bool json_output = (s_key_args.json->count > 0);
    bool exportable = (s_key_args.exportable->count > 0);
    
    /* 处理命令 */
    if (s_key_args.list->count > 0) {
        return do_key_list(json_output);
    }
    
    if (s_key_args.info->count > 0) {
        return do_key_info(key_id, json_output);
    }
    
    if (s_key_args.import->count > 0) {
        return do_key_import(key_id, file_path, comment);
    }
    
    if (s_key_args.generate->count > 0) {
        return do_key_generate(key_id, type_str, comment, exportable);
    }
    
    if (s_key_args.delete->count > 0) {
        return do_key_delete(key_id);
    }
    
    if (s_key_args.export_priv->count > 0) {
        return do_key_export_private(key_id, output_path);
    }
    
    if (s_key_args.export->count > 0) {
        return do_key_export(key_id, output_path);
    }
    
    if (s_key_args.use_for_https->count > 0) {
        ts_console_printf("Error: HTTPS server not implemented\n");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    /* 无操作指定 - 默认显示列表 */
    return do_key_list(json_output);
}

/*===========================================================================*/
/*                          Command Registration                              */
/*===========================================================================*/

esp_err_t ts_cmd_key_register(void)
{
    /* 初始化参数表 */
    s_key_args.list = arg_lit0("l", "list", "List all stored keys");
    s_key_args.info = arg_lit0("i", "info", "Show key details");
    s_key_args.import = arg_lit0(NULL, "import", "Import key from file");
    s_key_args.generate = arg_lit0("g", "generate", "Generate and store new key");
    s_key_args.delete = arg_lit0("d", "delete", "Delete stored key");
    s_key_args.export = arg_lit0("e", "export", "Export public key to file");
    s_key_args.export_priv = arg_lit0(NULL, "export-priv", "Export private key (requires exportable)");
    s_key_args.use_for_https = arg_lit0(NULL, "use-for-https", "Deploy key for HTTPS (generates self-signed cert)");
    s_key_args.id = arg_str0(NULL, "id", "<name>", "Key identifier");
    s_key_args.file = arg_str0("f", "file", "<path>", "Private key file (for import)");
    s_key_args.output = arg_str0("o", "output", "<path>", "Output file (for export)");
    s_key_args.type = arg_str0("t", "type", "<type>", "Key type: rsa, ecdsa, ec256, ec384");
    s_key_args.comment = arg_str0("c", "comment", "<text>", "Comment/description");
    s_key_args.cn = arg_str0(NULL, "cn", "<name>", "Certificate Common Name (for HTTPS)");
    s_key_args.org = arg_str0(NULL, "org", "<organization>", "Certificate Organization (for HTTPS)");
    s_key_args.country = arg_str0(NULL, "country", "<code>", "Certificate Country Code (for HTTPS)");
    s_key_args.days = arg_int0(NULL, "days", "<num>", "Certificate validity days (for HTTPS)");
    s_key_args.exportable = arg_lit0(NULL, "exportable", "Allow private key export");
    s_key_args.json = arg_lit0("j", "json", "JSON format output");
    s_key_args.help = arg_lit0("h", "help", "Show help");
    s_key_args.end = arg_end(7);
    
    /* 检查参数分配 */
    void *argtable[] = {
        s_key_args.list,
        s_key_args.info,
        s_key_args.import,
        s_key_args.generate,
        s_key_args.delete,
        s_key_args.export,
        s_key_args.export_priv,
        s_key_args.use_for_https,
        s_key_args.id,
        s_key_args.file,
        s_key_args.output,
        s_key_args.type,
        s_key_args.comment,
        s_key_args.cn,
        s_key_args.org,
        s_key_args.country,
        s_key_args.days,
        s_key_args.exportable,
        s_key_args.json,
        s_key_args.help,
        s_key_args.end,
    };
    
    for (int i = 0; argtable[i] != s_key_args.end; i++) {
        if (argtable[i] == NULL) {
            TS_LOGE(TAG, "Failed to allocate argtable");
            return ESP_ERR_NO_MEM;
        }
    }
    
    /* 注册命令 */
    const esp_console_cmd_t cmd = {
        .command = "key",
        .help = "Manage cryptographic keys in secure storage",
        .hint = NULL,
        .func = &key_cmd_handler,
        .argtable = &s_key_args,
    };
    
    esp_err_t ret = esp_console_cmd_register(&cmd);
    if (ret == ESP_OK) {
        TS_LOGI(TAG, "Registered command: key");
    }
    
    return ret;
}
