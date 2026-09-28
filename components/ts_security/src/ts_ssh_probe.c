#include "ts_ssh_probe.h"
#include <stdio.h>
#include <string.h>

bool ts_ssh_shell_quote(const char *input, char *out, size_t cap) {
    if (!input || !out || cap < 3)
        return false;
    size_t n = 0;
    out[n++] = '\'';
    for (; *input; input++) {
        const char *part = *input == '\'' ? "'\\''" : input;
        size_t len = *input == '\'' ? 4 : 1;
        if (len + 2 >= cap - n)
            return false;
        memcpy(out + n, part, len);
        n += len;
    }
    out[n++] = '\'';
    out[n] = 0;
    return true;
}

bool ts_ssh_log_probe_command(const char *path, const char *ready, const char *fail, char *out,
                              size_t cap) {
    /* Build each quoted value directly in the bounded output, without scratch
     * arrays on the 8 KiB SSH worker stack. */
    if (!path || !ready || !fail || cap < 32)
        return false;
    size_t n = 0;
    const char *keys[] = {"p=", "; r=", "; f="};
    const char *values[] = {path, ready, fail};
    for (unsigned i = 0; i < 3; i++) {
        size_t len = strlen(keys[i]);
        if (len >= cap - n)
            return false;
        memcpy(out + n, keys[i], len);
        n += len;
        if (!ts_ssh_shell_quote(values[i], out + n, cap - n))
            return false;
        n += strlen(out + n);
    }
    const char *script = "; if [ ! -f \"$p\" ]; then echo NOTFOUND; "
                         "elif [ ! -r \"$p\" ]; then exit 2; "
                         "else if [ -n \"$f\" ]; then grep -qF -- \"$f\" \"$p\"; x=$?; "
                         "if [ $x -eq 0 ]; then echo FAIL; exit 0; fi; [ $x -eq 1 ] || exit 2; fi; "
                         "grep -qF -- \"$r\" \"$p\"; x=$?; "
                         "if [ $x -eq 0 ]; then echo READY; elif [ $x -eq 1 ]; then echo WAITING; "
                         "else exit 2; fi; fi";
    size_t len = strlen(script);
    if (len >= cap - n)
        return false;
    memcpy(out + n, script, len + 1);
    return true;
}

bool ts_ssh_probe_token(const char *out, const char *token) {
    if (!out || !token)
        return false;
    size_t n = strlen(token);
    if (strncmp(out, token, n))
        return false;
    return !out[n] || !strcmp(out + n, "\n") || !strcmp(out + n, "\r\n");
}
