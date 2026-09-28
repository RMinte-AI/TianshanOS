#include "ts_cert_time.h"
/**
 * @file ts_https_auth.c
 * @brief mTLS Authentication and Role Extraction
 * 
 * Handles client certificate validation and role extraction from OU field.
 */

#include "ts_https.h"
#include "ts_https_internal.h"
#include "esp_log.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ssl.h"
#include <string.h>

static const char *TAG = "ts_https_auth";

/*===========================================================================*/
/*                         Role Conversion                                    */
/*===========================================================================*/

const char *ts_https_role_to_str(ts_https_role_t role)
{
    switch (role) {
        case TS_HTTPS_ROLE_ANONYMOUS: return "anonymous";
        case TS_HTTPS_ROLE_VIEWER:    return "viewer";
        case TS_HTTPS_ROLE_OPERATOR:  return "operator";
        case TS_HTTPS_ROLE_ADMIN:     return "admin";
        case TS_HTTPS_ROLE_DEVICE:    return "device";
        default:                      return "unknown";
    }
}

ts_https_role_t ts_https_str_to_role(const char *str)
{
    if (!str) return TS_HTTPS_ROLE_ANONYMOUS;
    
    if (strcasecmp(str, "admin") == 0) return TS_HTTPS_ROLE_ADMIN;
    if (strcasecmp(str, "operator") == 0) return TS_HTTPS_ROLE_OPERATOR;
    if (strcasecmp(str, "viewer") == 0) return TS_HTTPS_ROLE_VIEWER;
    if (strcasecmp(str, "device") == 0) return TS_HTTPS_ROLE_DEVICE;
    
    return TS_HTTPS_ROLE_ANONYMOUS;
}

bool ts_https_role_has_permission(ts_https_role_t user_role, ts_https_role_t required_role)
{
    // Admin > Operator > Viewer > Anonymous
    // Device is special - only equals Device
    if (required_role == TS_HTTPS_ROLE_DEVICE) {
        return user_role == TS_HTTPS_ROLE_DEVICE;
    }
    
    // For other roles, higher numeric value = more permission
    return (int)user_role >= (int)required_role;
}

/*===========================================================================*/
/*                         Certificate Parsing                                */
/*===========================================================================*/

/**
 * @brief Extract a specific RDN (Relative Distinguished Name) from X.509 name
 * 
 * @param name X.509 name structure
 * @param oid OID to look for (e.g., MBEDTLS_OID_AT_CN for Common Name)
 * @param oid_len OID length
 * @param value Output buffer
 * @param value_len Buffer size
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND otherwise
 */
static esp_err_t extract_rdn(const mbedtls_x509_name *name, 
                             const char *oid, size_t oid_len,
                             char *value, size_t value_len)
{
    const mbedtls_x509_name *cur = name;
    
    while (cur != NULL) {
        if (cur->oid.len == oid_len && 
            memcmp(cur->oid.p, oid, oid_len) == 0) {
            size_t copy_len = cur->val.len < value_len - 1 ? 
                              cur->val.len : value_len - 1;
            memcpy(value, cur->val.p, copy_len);
            value[copy_len] = '\0';
            return ESP_OK;
        }
        cur = cur->next;
    }
    
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Extract authentication info from client certificate
 * 
 * @param cert Client certificate
 * @param auth Output authentication info
 * @return ESP_OK on success
 */
esp_err_t ts_https_auth_from_cert(const mbedtls_x509_crt *cert, ts_https_auth_t *auth)
{
    if (!cert || !auth) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(auth, 0, sizeof(*auth));
    auth->authenticated = true;
    
    // Extract Common Name (CN) → username
    // OID for CN: 2.5.4.3
    static const char oid_cn[] = { 0x55, 0x04, 0x03 };
    if (extract_rdn(&cert->subject, oid_cn, sizeof(oid_cn), 
                    auth->username, sizeof(auth->username)) != ESP_OK) {
        strncpy(auth->username, "unknown", sizeof(auth->username) - 1);
    }
    
    // Extract Organization (O)
    // OID for O: 2.5.4.10
    static const char oid_o[] = { 0x55, 0x04, 0x0A };
    extract_rdn(&cert->subject, oid_o, sizeof(oid_o),
                auth->organization, sizeof(auth->organization));
    
    // Extract Organizational Unit (OU) → role
    // OID for OU: 2.5.4.11
    static const char oid_ou[] = { 0x55, 0x04, 0x0B };
    char ou_value[32] = {0};
    if (extract_rdn(&cert->subject, oid_ou, sizeof(oid_ou),
                    ou_value, sizeof(ou_value)) == ESP_OK) {
        auth->role = ts_https_str_to_role(ou_value);
        ESP_LOGI(TAG, "Client role from OU: %s → %s", 
                 ou_value, ts_https_role_to_str(auth->role));
    } else {
        // No OU field - default to viewer
        auth->role = TS_HTTPS_ROLE_VIEWER;
        ESP_LOGW(TAG, "No OU field in certificate, defaulting to viewer");
    }
    
    int64_t expiry;
    time_t now = time(NULL);
    auth->cert_days_remaining = 0;
    if (now != (time_t)-1 && ts_cert_time_utc(cert->valid_to.year, cert->valid_to.mon,
            cert->valid_to.day, cert->valid_to.hour, cert->valid_to.min, cert->valid_to.sec, &expiry))
        auth->cert_days_remaining = ts_cert_time_days(expiry - (int64_t)now);

    ESP_LOGI(TAG, "Authenticated: %s (%s), role=%s, expires in %d days",
             auth->username, auth->organization,
             ts_https_role_to_str(auth->role),
             auth->cert_days_remaining);
    
    return ESP_OK;
}

/**
 * @brief Get client certificate from SSL session
 * 
 * This is called during request handling to extract auth info.
 * 
 * @param ssl_ctx SSL context from esp_tls
 * @param auth Output authentication info
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no client cert
 */
esp_err_t ts_https_get_client_auth(void *ssl_ctx, ts_https_auth_t *auth)
{
    if (!ssl_ctx || !auth) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(auth, 0, sizeof(*auth));
    
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)ssl_ctx;
    
    // Get peer certificate
    const mbedtls_x509_crt *peer_cert = mbedtls_ssl_get_peer_cert(ssl);
    if (!peer_cert) {
        ESP_LOGD(TAG, "No client certificate provided");
        auth->authenticated = false;
        auth->role = TS_HTTPS_ROLE_ANONYMOUS;
        return ESP_ERR_NOT_FOUND;
    }
    
    return ts_https_auth_from_cert(peer_cert, auth);
}

/*===========================================================================*/
/*                         Permission Checking                                */
/*===========================================================================*/

/**
 * @brief Check if request has sufficient permissions
 * 
 * @param auth Authentication info
 * @param min_role Minimum required role
 * @return true if permitted
 */
bool ts_https_check_permission(const ts_https_auth_t *auth, ts_https_role_t min_role)
{
    if (!auth) {
        return min_role == TS_HTTPS_ROLE_ANONYMOUS;
    }
    
    // If mTLS is required but not authenticated, deny
    if (!auth->authenticated && min_role > TS_HTTPS_ROLE_ANONYMOUS) {
        ESP_LOGW(TAG, "Permission denied: not authenticated");
        return false;
    }
    
    bool permitted = ts_https_role_has_permission(auth->role, min_role);
    
    if (!permitted) {
        ESP_LOGW(TAG, "Permission denied: %s requires %s, user has %s",
                 ts_https_role_to_str(min_role),
                 ts_https_role_to_str(min_role),
                 ts_https_role_to_str(auth->role));
    }
    
    return permitted;
}
