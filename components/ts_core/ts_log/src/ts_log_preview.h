#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
/* Fixed UTF-8 prefix; strip complete CSI only. No allocation, no logging. */
static bool ts_log_preview(char *dst, size_t cap, const char *src, bool clipped) {
    size_t n = 0;
    while (*src) {
        const unsigned char *p = (const unsigned char *)src;
        if (p[0] == 27 && p[1] == '[') {
            size_t k = 2;
            while (p[k] >= 0x20 && p[k] <= 0x3f)
                ++k;
            if (p[k] >= 0x40 && p[k] <= 0x7e) {
                src += k + 1;
                continue;
            }
        }
        size_t bytes =
            p[0] < 0x80
                ? 1
                : (p[0] >= 0xc2 && p[0] <= 0xdf
                       ? 2
                       : (p[0] >= 0xe0 && p[0] <= 0xef ? 3
                                                       : (p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0)));
        bool valid = bytes != 0;
        for (size_t i = 1; valid && i < bytes; ++i)
            valid = p[i] >= 0x80 && p[i] <= 0xbf;
        if (valid && bytes == 3)
            valid = !(p[0] == 0xe0 && p[1] < 0xa0) && !(p[0] == 0xed && p[1] >= 0xa0);
        if (valid && bytes == 4)
            valid = !(p[0] == 0xf0 && p[1] < 0x90) && !(p[0] == 0xf4 && p[1] >= 0x90);
        if (!valid) {
            clipped = true;
            break;
        }
        if (n + bytes + 1 > cap) {
            clipped = true;
            break;
        }
        memcpy(dst + n, src, bytes);
        n += bytes;
        src += bytes;
    }
    while (n && (dst[n - 1] == '\r' || dst[n - 1] == '\n'))
        --n;
    if (clipped && cap >= 5) {
        while (n > cap - 5) {
            --n;
            while (n && ((unsigned char)dst[n] & 0xc0) == 0x80)
                --n;
        }
        memcpy(dst + n, " ...", 4);
        n += 4;
    }
    if (cap)
        dst[n] = 0;
    return clipped;
}
