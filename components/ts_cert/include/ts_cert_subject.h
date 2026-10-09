#pragma once
#include <stddef.h>
#include "esp_err.h"

#define TS_CERT_SUBJECT_CAPACITY 256
#define TS_CERT_DEFAULT_DEVICE_ID "TIANSHAN-DEVICE-001"

/* Build the MbedTLS DN without letting field contents become DN syntax. */
esp_err_t ts_cert_build_subject(const char *device_id, const char *organization,
                                const char *org_unit, char *out, size_t capacity);
