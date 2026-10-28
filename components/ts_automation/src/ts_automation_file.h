#pragma once
#include "esp_err.h"

/* Only the configuration owner calls these while holding the writer/mount gate. */
typedef enum {
    TS_AUTO_FILE_SD,
    TS_AUTO_FILE_INTERNAL
} ts_auto_file_location_t;

/* The caller owns returned UTF-8 bytes. NOT_FOUND means no installed document.
 * INVALID_ARG means unreadable JSON; NOT_FINISHED means uncertain I/O.
 * The configuration owner validates schema and business references. */
esp_err_t ts_auto_file_load(ts_auto_file_location_t location, char **json);
esp_err_t ts_auto_file_save(ts_auto_file_location_t location, const char *json);
/* Preview tokens also cover a malformed destination which the user explicitly
 * chooses to replace. Missing files return NOT_FOUND, not an empty digest. */
esp_err_t ts_auto_file_fingerprint(ts_auto_file_location_t location, char digest[65]);
