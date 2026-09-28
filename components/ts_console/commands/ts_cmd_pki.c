/**
 * @file ts_cmd_pki.c
 * @brief PKI Certificate Console Commands
 * 
 * 实现 pki 命令族：
 * - pki --status         显示 PKI 状态
 * - pki --generate       生成密钥对
 * - pki --csr            生成 CSR
 * - pki --install        安装证书
 * - pki --install-ca     安装 CA 链
 * - pki --export-csr     导出 CSR 到文件
 * - pki --info           显示证书信息
 * - pki --reset          删除所有 PKI 数据
 * 
 * @author TianShanOS Team
 * @version 1.0.0
 */

#include "ts_console.h"
#include "ts_cert.h"
#include "ts_log.h"
#include "argtable3/argtable3.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "cmd_pki"

/*===========================================================================*/
/*                          Argument Tables                                   */
/*===========================================================================*/

static struct {
    struct arg_lit *status;
    struct arg_lit *generate;
    struct arg_lit *csr;
    struct arg_lit *install;
    struct arg_lit *install_ca;
    struct arg_lit *export_csr;
    struct arg_lit *info;
    struct arg_lit *reset;
    struct arg_str *device_id;
    struct arg_str *ip;
    struct arg_str *file;
    struct arg_lit *force;
    struct arg_lit *json;
    struct arg_lit *help;
    struct arg_end *end;
} s_pki_args;

/*===========================================================================*/
/*                          Helper Functions                                  */
/*===========================================================================*/

static uint32_t parse_ip_address(const char *ip_str)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) == 1) {
        return ntohl(addr.s_addr);  /* Convert to host byte order */
    }
    return 0;
}

static void print_status_json(const ts_cert_pki_status_t *status)
{
    ts_console_printf("{\n");
    ts_console_printf("  \"status\": \"%s\",\n", ts_cert_status_to_str(status->status));
    ts_console_printf("  \"has_private_key\": %s,\n", status->has_private_key ? "true" : "false");
    ts_console_printf("  \"has_certificate\": %s,\n", status->has_certificate ? "true" : "false");
    ts_console_printf("  \"has_ca_chain\": %s", status->has_ca_chain ? "true" : "false");
    
    if (status->has_certificate) {
        ts_console_printf(",\n  \"certificate\": {\n");
        ts_console_printf("    \"subject\": \"%s\",\n", status->cert_info.subject_cn);
        ts_console_printf("    \"issuer\": \"%s\",\n", status->cert_info.issuer_cn);
        ts_console_printf("    \"serial\": \"%s\",\n", status->cert_info.serial);
        ts_console_printf("    \"valid\": %s,\n", status->cert_info.is_valid ? "true" : "false");
        ts_console_printf("    \"validity\": \"%s\",\n", ts_cert_validity_to_str(status->cert_info.validity));
        ts_console_printf("    \"days_until_expiry\": %d\n", status->cert_info.days_until_expiry);
        ts_console_printf("  }\n");
    } else {
        ts_console_printf("\n");
    }
    
    ts_console_printf("}\n");
}

static void print_status_text(const ts_cert_pki_status_t *status)
{
    ts_console_printf("\n");
    ts_console_printf("╔══════════════════════════════════════════╗\n");
    ts_console_printf("║           PKI Certificate Status         ║\n");
    ts_console_printf("╠══════════════════════════════════════════╣\n");
    
    const char *status_str;
    const char *status_color;
    
    switch (status->status) {
        case TS_CERT_STATUS_ACTIVATED:
            status_str = "TIME VALID";
            status_color = "\033[32m";  /* Green */
            break;
        case TS_CERT_STATUS_KEY_GENERATED:
            status_str = "KEY GENERATED";
            status_color = "\033[33m";  /* Yellow */
            break;
        case TS_CERT_STATUS_CSR_PENDING:
            status_str = "CSR PENDING";
            status_color = "\033[33m";  /* Yellow */
            break;
        case TS_CERT_STATUS_TIME_UNVERIFIED:
            status_str = "TIME UNVERIFIED"; status_color = "\033[33m"; break;
        case TS_CERT_STATUS_NOT_YET_VALID:
            status_str = "NOT YET VALID"; status_color = "\033[33m"; break;
        case TS_CERT_STATUS_ERROR:
            status_str = "MATERIAL ERROR"; status_color = "\033[31m"; break;
        case TS_CERT_STATUS_EXPIRED:
            status_str = "EXPIRED";
            status_color = "\033[31m";  /* Red */
            break;
        default:
            status_str = "NOT INITIALIZED";
            status_color = "\033[31m";  /* Red */
    }
    
    ts_console_printf("║ Status:      %s%-16s\033[0m        ║\n", status_color, status_str);
    ts_console_printf("║ Private Key: %-28s ║\n", status->has_private_key ? "✓ Present" : "✗ Missing");
    ts_console_printf("║ Certificate: %-28s ║\n", status->has_certificate ? "✓ Installed" : "✗ Not installed");
    ts_console_printf("║ CA Chain:    %-28s ║\n", status->has_ca_chain ? "✓ Installed" : "✗ Not installed");
    
    if (status->has_certificate) {
        ts_console_printf("╠══════════════════════════════════════════╣\n");
        ts_console_printf("║              Certificate Info            ║\n");
        ts_console_printf("╠══════════════════════════════════════════╣\n");
        ts_console_printf("║ Subject: %-32.32s ║\n", status->cert_info.subject_cn);
        ts_console_printf("║ Issuer:  %-32.32s ║\n", status->cert_info.issuer_cn);
        ts_console_printf("║ Serial:  %-32.32s ║\n", status->cert_info.serial);
        
        if (status->cert_info.is_valid) {
            ts_console_printf("║ Validity: \033[32m%-30s\033[0m ║\n", "Valid");
            if (status->cert_info.days_until_expiry < 30) {
                ts_console_printf("║ Expires:  \033[33m%d days\033[0m                         ║\n", 
                                 status->cert_info.days_until_expiry);
            } else {
                ts_console_printf("║ Expires:  %d days                         ║\n", 
                                 status->cert_info.days_until_expiry);
            }
        } else {
            ts_console_printf("║ Validity: %-30s ║\n", ts_cert_validity_to_str(status->cert_info.validity));
        }
    }
    
    ts_console_printf("╚══════════════════════════════════════════╝\n\n");
}

/*===========================================================================*/
/*                          Command Handlers                                  */
/*===========================================================================*/

static int cmd_pki_status(bool json)
{
    ts_cert_pki_status_t status;
    esp_err_t err = ts_cert_get_status(&status);
    
    if (err != ESP_OK) {
        ts_console_printf("Error: Failed to get PKI status: %s\n", esp_err_to_name(err));
        return 1;
    }
    
    if (json) {
        print_status_json(&status);
    } else {
        print_status_text(&status);
    }
    
    return 0;
}

static int cmd_pki_generate(bool force)
{
    if (ts_cert_has_keypair() && !force) {
        ts_console_printf("Error: Key pair already exists. Use --force to overwrite.\n");
        return 1;
    }
    
    ts_console_printf("Generating ECDSA P-256 key pair...\n");
    
    esp_err_t err = ts_cert_generate_keypair();
    if (err != ESP_OK) {
        ts_console_printf("Error: Key generation failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    
    ts_console_printf("\033[32m✓ Key pair generated successfully\033[0m\n");
    ts_console_printf("Next step: Run 'pki --csr' to generate a CSR\n");
    return 0;
}

static int cmd_pki_csr(const char *device_id, const char *ip_str, bool json)
{
    if (!ts_cert_has_keypair()) {
        ts_console_printf("Error: No key pair. Run 'pki --generate' first.\n");
        return 1;
    }
    
    /* Get current IP if not specified */
    uint32_t ip_addr = 0;
    if (ip_str) {
        ip_addr = parse_ip_address(ip_str);
        if (ip_addr == 0) {
            ts_console_printf("Error: Invalid IP address: %s\n", ip_str);
            return 1;
        }
    } else {
        /* Try to get IP from network interface */
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!netif) {
            netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        }
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                ip_addr = ntohl(ip_info.ip.addr);
            }
        }
    }
    
    /* Use default device ID if not specified */
    if (!device_id || strlen(device_id) == 0) {
        device_id = "TIANSHAN-DEVICE-001";  /* TODO: Get from config */
    }
    
    ts_cert_csr_opts_t opts = {
        .device_id = device_id,
        .organization = "TianShanOS",
        .org_unit = "Device",
        .ip_san_count = ip_addr ? 1 : 0,
        .dns_san_count = 0
    };
    opts.ip_sans[0] = ip_addr;
    
    /* Generate CSR */
    char *csr_buf = malloc(TS_CERT_CSR_MAX_LEN);
    if (!csr_buf) {
        ts_console_printf("Error: Out of memory\n");
        return 1;
    }
    
    size_t csr_len = TS_CERT_CSR_MAX_LEN;
    esp_err_t err = ts_cert_generate_csr(&opts, csr_buf, &csr_len);
    
    if (err != ESP_OK) {
        free(csr_buf);
        ts_console_printf("Error: CSR generation failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    
    if (json) {
        ts_console_printf("{\n");
        ts_console_printf("  \"device_id\": \"%s\",\n", device_id);
        if (ip_addr) {
            ts_console_printf("  \"ip_san\": \"%d.%d.%d.%d\",\n",
                             (ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF,
                             (ip_addr >> 8) & 0xFF, ip_addr & 0xFF);
        }
        ts_console_printf("  \"csr\": \"");
        /* Print CSR with escaped newlines */
        for (const char *p = csr_buf; *p; p++) {
            if (*p == '\n') ts_console_printf("\\n");
            else ts_console_printf("%c", *p);
        }
        ts_console_printf("\"\n}\n");
    } else {
        ts_console_printf("\n\033[32m✓ CSR generated successfully\033[0m\n\n");
        ts_console_printf("Device ID: %s\n", device_id);
        if (ip_addr) {
            ts_console_printf("IP SAN:    %d.%d.%d.%d\n",
                             (ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF,
                             (ip_addr >> 8) & 0xFF, ip_addr & 0xFF);
        }
        ts_console_printf("\n--- BEGIN CSR ---\n%s--- END CSR ---\n\n", csr_buf);
        ts_console_printf("Next steps:\n");
        ts_console_printf("  1. Copy the CSR above (or use 'pki --export-csr --file /sdcard/device.csr')\n");
        ts_console_printf("  2. Submit to CA: step ca sign device.csr device.crt\n");
        ts_console_printf("  3. Install certificate: pki --install --file /sdcard/device.crt\n");
    }
    
    free(csr_buf);
    return 0;
}

static int cmd_pki_export_csr(const char *filepath)
{
    if (!filepath || strlen(filepath) == 0) {
        ts_console_printf("Error: File path required (--file <path>)\n");
        return 1;
    }
    
    if (!ts_cert_has_keypair()) {
        ts_console_printf("Error: No key pair. Run 'pki --generate' first.\n");
        return 1;
    }
    
    /* Generate CSR */
    char *csr_buf = malloc(TS_CERT_CSR_MAX_LEN);
    if (!csr_buf) {
        ts_console_printf("Error: Out of memory\n");
        return 1;
    }
    
    size_t csr_len = TS_CERT_CSR_MAX_LEN;
    esp_err_t err = ts_cert_generate_csr_default(csr_buf, &csr_len);
    
    if (err != ESP_OK) {
        free(csr_buf);
        ts_console_printf("Error: CSR generation failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    
    /* Write to file */
    FILE *f = fopen(filepath, "w");
    if (!f) {
        free(csr_buf);
        ts_console_printf("Error: Cannot open file: %s\n", filepath);
        return 1;
    }
    
    size_t written = fwrite(csr_buf, 1, strlen(csr_buf), f);
    fclose(f);
    free(csr_buf);
    
    if (written == 0) {
        ts_console_printf("Error: Failed to write CSR\n");
        return 1;
    }
    
    ts_console_printf("\033[32m✓ CSR exported to: %s\033[0m\n", filepath);
    return 0;
}

static int cmd_pki_install(const char *filepath, bool is_ca)
{
    if (!filepath || strlen(filepath) == 0) {
        ts_console_printf("Error: File path required (--file <path>)\n");
        return 1;
    }
    
    /* Read certificate from file */
    FILE *f = fopen(filepath, "r");
    if (!f) {
        ts_console_printf("Error: Cannot open file: %s\n", filepath);
        return 1;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size >= TS_CERT_INSTALL_MAX_LEN) {
        fclose(f);
        ts_console_printf("Error: Invalid file size\n");
        return 1;
    }
    
    char *cert_buf = malloc(file_size + 1);
    if (!cert_buf) {
        fclose(f);
        ts_console_printf("Error: Out of memory\n");
        return 1;
    }
    
    size_t read_len = fread(cert_buf, 1, file_size, f);
    fclose(f);
    cert_buf[read_len] = '\0';
    
    esp_err_t err;
    if (is_ca) {
        err = ts_cert_install_ca_chain(cert_buf, read_len + 1);
    } else {
        err = ts_cert_install_certificate(cert_buf, read_len + 1);
    }
    
    free(cert_buf);
    
    if (err != ESP_OK) {
        ts_console_printf("Error: Installation failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    
    ts_console_printf("\033[32m✓ %s installed successfully\033[0m\n",
                     is_ca ? "CA chain" : "Certificate");
    return 0;
}

static int cmd_pki_reset(bool force)
{
    if (!force) {
        ts_console_printf("Warning: This will delete all PKI credentials!\n");
        ts_console_printf("Use 'pki --reset --force' to confirm.\n");
        return 1;
    }
    
    esp_err_t err = ts_cert_factory_reset();
    if (err != ESP_OK) {
        ts_console_printf("Error: Reset failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    
    ts_console_printf("\033[32m✓ PKI credentials deleted\033[0m\n");
    return 0;
}

/*===========================================================================*/
/*                          Main Command Handler                              */
/*===========================================================================*/

static int cmd_pki_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_pki_args);
    
    if (s_pki_args.help->count > 0) {
        ts_console_printf("Usage: pki [options]\n\n");
        ts_console_printf("PKI certificate management for mTLS authentication.\n\n");
        ts_console_printf("Options:\n");
        ts_console_printf("  --status            Show PKI status\n");
        ts_console_printf("  --generate          Generate ECDSA P-256 key pair\n");
        ts_console_printf("  --csr               Generate Certificate Signing Request\n");
        ts_console_printf("  --export-csr        Export CSR to file\n");
        ts_console_printf("  --install           Install signed certificate\n");
        ts_console_printf("  --install-ca        Install CA certificate chain\n");
        ts_console_printf("  --info              Show certificate info\n");
        ts_console_printf("  --reset             Delete all PKI credentials\n");
        ts_console_printf("\n");
        ts_console_printf("  --device-id <id>    Device ID for CSR (default: auto)\n");
        ts_console_printf("  --ip <addr>         IP address for SAN extension\n");
        ts_console_printf("  --file <path>       File path for import/export\n");
        ts_console_printf("  --force             Force overwrite/confirm dangerous operations\n");
        ts_console_printf("  --json              Output in JSON format\n");
        ts_console_printf("\n");
        ts_console_printf("Examples:\n");
        ts_console_printf("  pki --status                    # Show current status\n");
        ts_console_printf("  pki --generate                  # Generate key pair\n");
        ts_console_printf("  pki --csr --device-id RM01-001  # Generate CSR\n");
        ts_console_printf("  pki --export-csr --file /sdcard/device.csr\n");
        ts_console_printf("  pki --install --file /sdcard/device.crt\n");
        ts_console_printf("  pki --install-ca --file /sdcard/ca_chain.crt\n");
        return 0;
    }
    
    if (nerrors > 0) {
        arg_print_errors(stderr, s_pki_args.end, "pki");
        return 1;
    }
    
    bool json = s_pki_args.json->count > 0;
    bool force = s_pki_args.force->count > 0;
    const char *device_id = s_pki_args.device_id->count > 0 ? s_pki_args.device_id->sval[0] : NULL;
    const char *ip_str = s_pki_args.ip->count > 0 ? s_pki_args.ip->sval[0] : NULL;
    const char *filepath = s_pki_args.file->count > 0 ? s_pki_args.file->sval[0] : NULL;
    
    /* Handle commands */
    if (s_pki_args.generate->count > 0) {
        return cmd_pki_generate(force);
    }
    
    if (s_pki_args.csr->count > 0) {
        return cmd_pki_csr(device_id, ip_str, json);
    }
    
    if (s_pki_args.export_csr->count > 0) {
        return cmd_pki_export_csr(filepath);
    }
    
    if (s_pki_args.install->count > 0) {
        return cmd_pki_install(filepath, false);
    }
    
    if (s_pki_args.install_ca->count > 0) {
        return cmd_pki_install(filepath, true);
    }
    
    if (s_pki_args.info->count > 0) {
        ts_cert_info_t info;
        esp_err_t err = ts_cert_get_info(&info);
        if (err != ESP_OK) {
            ts_console_printf("Error: No certificate installed\n");
            return 1;
        }
        
        ts_console_printf("\nCertificate Information:\n");
        ts_console_printf("  Subject:  %s\n", info.subject_cn);
        ts_console_printf("  Issuer:   %s\n", info.issuer_cn);
        ts_console_printf("  Serial:   %s\n", info.serial);
        ts_console_printf("  Valid:    %s\n", info.is_valid ? "Yes" : "No");
        ts_console_printf("  Time validity: %s\n", ts_cert_validity_to_str(info.validity));
        if (info.time_ready && info.validity != TS_CERT_VALIDITY_INVALID)
            ts_console_printf("  Expires:  %d days\n", info.days_until_expiry);
        return 0;
    }
    
    if (s_pki_args.reset->count > 0) {
        return cmd_pki_reset(force);
    }
    
    /* Default: show status */
    return cmd_pki_status(json);
}

/*===========================================================================*/
/*                          Registration                                      */
/*===========================================================================*/

esp_err_t ts_cmd_pki_register(void)
{
    /* Initialize argument table */
    s_pki_args.status      = arg_lit0(NULL, "status", "Show PKI status");
    s_pki_args.generate    = arg_lit0("g", "generate", "Generate key pair");
    s_pki_args.csr         = arg_lit0("c", "csr", "Generate CSR");
    s_pki_args.install     = arg_lit0("i", "install", "Install certificate");
    s_pki_args.install_ca  = arg_lit0(NULL, "install-ca", "Install CA chain");
    s_pki_args.export_csr  = arg_lit0("e", "export-csr", "Export CSR to file");
    s_pki_args.info        = arg_lit0(NULL, "info", "Show certificate info");
    s_pki_args.reset       = arg_lit0(NULL, "reset", "Delete all PKI data");
    s_pki_args.device_id   = arg_str0("d", "device-id", "<id>", "Device ID");
    s_pki_args.ip          = arg_str0(NULL, "ip", "<addr>", "IP address for SAN");
    s_pki_args.file        = arg_str0("f", "file", "<path>", "File path");
    s_pki_args.force       = arg_lit0(NULL, "force", "Force operation");
    s_pki_args.json        = arg_lit0("j", "json", "JSON output");
    s_pki_args.help        = arg_lit0("h", "help", "Show help");
    s_pki_args.end         = arg_end(5);
    
    const esp_console_cmd_t cmd = {
        .command = "pki",
        .help = "PKI certificate management",
        .hint = NULL,
        .func = &cmd_pki_handler,
        .argtable = &s_pki_args
    };
    
    esp_err_t err = esp_console_cmd_register(&cmd);
    if (err == ESP_OK) {
        TS_LOGI(TAG, "Registered 'pki' command");
    }
    
    return err;
}
