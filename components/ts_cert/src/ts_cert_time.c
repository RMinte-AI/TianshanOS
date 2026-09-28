#include "ts_cert_time.h"
#include <limits.h>

bool ts_cert_time_utc(int y, int m, int d, int h, int min, int s, int64_t *out)
{
    static const int before[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!out || y < 1 || y > 9999 || m < 1 || m > 12 || h < 0 || h > 23 ||
        min < 0 || min > 59 || s < 0 || s > 59) return false;
    bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    if (d < 1 || d > days[m-1] + (m == 2 && leap)) return false;
    int64_t prev = y - 1;
    int64_t n = 365 * (int64_t)(y - 1970) + prev/4 - prev/100 + prev/400 - 477;
    n += before[m-1] + (leap && m > 2) + d - 1;
    *out = n * 86400 + h * 3600 + min * 60 + s;
    return true;
}

int ts_cert_time_days(int64_t seconds)
{
    int64_t days = seconds / 86400 - (seconds < 0 && seconds % 86400 != 0);
    return days > INT_MAX ? INT_MAX : days < INT_MIN ? INT_MIN : (int)days;
}

bool ts_cert_time_ready(int64_t now, int minimum_year)
{
    int64_t threshold;
    return now != -1 && ts_cert_time_utc(minimum_year, 1, 1, 0, 0, 0, &threshold) && now >= threshold;
}

bool ts_cert_hex(const unsigned char *bytes, size_t length, char *out, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!capacity) return length != 0;
    size_t n = length < (capacity-1)/2 ? length : (capacity-1)/2;
    for (size_t i = 0; i < n; ++i) {
        out[2*i] = hex[bytes[i] >> 4];
        out[2*i+1] = hex[bytes[i] & 15];
    }
    out[2*n] = 0;
    return n < length;
}
