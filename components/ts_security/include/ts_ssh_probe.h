#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Shell words are always single-quoted; inputs must be NUL terminated. */
bool ts_ssh_shell_quote(const char *input, char *out, size_t capacity);
bool ts_ssh_log_probe_command(const char *path, const char *ready, const char *fail,
                              char *out, size_t capacity);
/* Exact protocol token, one optional LF/CRLF; no substring matching. */
bool ts_ssh_probe_token(const char *output, const char *token);

#ifdef __cplusplus
}
#endif
