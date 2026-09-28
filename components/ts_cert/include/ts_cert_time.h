#ifndef TS_CERT_TIME_H
#define TS_CERT_TIME_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
bool ts_cert_time_utc(int year, int month, int day, int hour, int minute, int second, int64_t *out);
int ts_cert_time_days(int64_t seconds);
bool ts_cert_time_ready(int64_t now, int minimum_year);
/* Returns true when the display was truncated. Always terminates if capacity > 0. */
bool ts_cert_hex(const unsigned char *bytes, size_t length, char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
