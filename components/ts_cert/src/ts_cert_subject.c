#include "ts_cert_subject.h"
#include "ts_cert.h"
#include <stdbool.h>
#include <string.h>

static bool append(char *out, size_t capacity, size_t *used, const char *text, size_t len)
{
    if (*used >= capacity || len >= capacity - *used) return false;
    memcpy(out + *used, text, len);
    *used += len;
    out[*used] = '\0';
    return true;
}

static bool append_value(char *out, size_t capacity, size_t *used, const char *value)
{
    size_t len = strlen(value);
    bool encoded = value[0] == ' ' || value[0] == '#' || value[len - 1] == ' ';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 32 || c == 127 || strchr(",=+<>;\"\\", c)) encoded = true;
    }
    if (!encoded) return append(out, capacity, used, value, len);

    /* MbedTLS accepts RFC4514 hexstrings containing DER. This also handles a
     * trailing backslash correctly; its text-DN parser checks only the byte
     * before a comma, rather than the parity of preceding backslashes. */
    if (len > 255) return false;
    static const char hex[] = "0123456789ABCDEF";
    unsigned char header[] = {0x0c, (unsigned char)len}; /* UTF8String */
    if (!append(out, capacity, used, "#", 1)) return false;
    for (size_t i = 0; i < sizeof(header); i++) {
        char pair[] = {hex[header[i] >> 4], hex[header[i] & 15]};
        if (i == 1 && len >= 128 && !append(out, capacity, used, "81", 2)) return false;
        if (!append(out, capacity, used, pair, 2)) return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        char pair[] = {hex[c >> 4], hex[c & 15]};
        if (!append(out, capacity, used, pair, 2)) return false;
    }
    return true;
}

esp_err_t ts_cert_build_subject(const char *device_id, const char *organization,
                                const char *org_unit, char *out, size_t capacity)
{
    if (!out || !capacity || !device_id || !*device_id) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    if (strlen(device_id) >= TS_CERT_DEVICE_ID_MAX_LEN) return ESP_ERR_INVALID_SIZE;
    const char *names[] = {"CN=", ",O=", ",OU="};
    const char *values[] = {device_id, organization, org_unit};
    size_t used = 0;
    for (size_t i = 0; i < 3; i++) {
        if (!values[i] || !*values[i]) continue;
        if (!append(out, capacity, &used, names[i], strlen(names[i])) ||
            !append_value(out, capacity, &used, values[i])) {
            out[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}
