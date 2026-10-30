#include "ts_ssh_client.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool ts_ssh_pattern_match(const char *text, const char *pattern, char **extracted)
{
    if (!text || !pattern) return false;

    /* 简化实现：支持 * 通配符和 () 捕获组 */
    /* 对于复杂正则，推荐在前端处理 */

    /* 检查是否包含捕获组 */
    const char *cap_start = strchr(pattern, '(');
    const char *cap_end = cap_start ? strchr(cap_start, ')') : NULL;

    if (cap_start && cap_end && extracted) {
        /* 有捕获组，提取模式 */
        /* 格式：prefix(capture)suffix */
        size_t prefix_len = cap_start - pattern;
        char *prefix = NULL;
        if (prefix_len > 0) {
            prefix = strndup(pattern, prefix_len);
        }

        size_t capture_len = cap_end - cap_start - 1;
        char *capture_pattern = strndup(cap_start + 1, capture_len);

        const char *suffix = cap_end + 1;

        /* 查找前缀位置 - 找最后一个匹配 */
        const char *match_pos = NULL;
        const char *search_pos = text;
        if (prefix && prefix[0]) {
            while ((search_pos = strstr(search_pos, prefix)) != NULL) {
                match_pos = search_pos + prefix_len;
                search_pos++;  /* 继续搜索下一个 */
            }
        } else {
            /* 没有前缀，从最后一行开始 */
            const char *last_newline = strrchr(text, '\n');
            match_pos = last_newline ? last_newline + 1 : text;
        }

        if (match_pos) {
            /* 查找后缀位置 */
            const char *end_pos = NULL;
            if (suffix && suffix[0]) {
                end_pos = strstr(match_pos, suffix);
            } else {
                /* 没有后缀，取到行尾或字符串尾 */
                end_pos = strchr(match_pos, '\n');
                if (!end_pos) end_pos = match_pos + strlen(match_pos);
            }

            if (end_pos && end_pos > match_pos) {
                /* 提取捕获的内容 */
                size_t ext_len = end_pos - match_pos;
                *extracted = strndup(match_pos, ext_len);
                /* 去除首尾空白 */
                if (*extracted) {
                    char *p = *extracted;
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (p != *extracted) memmove(*extracted, p, strlen(p) + 1);
                    size_t len = strlen(*extracted);
                    while (len > 0 && isspace((unsigned char)(*extracted)[len - 1])) {
                        (*extracted)[--len] = '\0';
                    }
                }
            }
        }

        free(prefix);
        free(capture_pattern);
        return (match_pos != NULL);
    } else {
        /* 简单包含匹配 */
        return strstr(text, pattern) != NULL;
    }
}
