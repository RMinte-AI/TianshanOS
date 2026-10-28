#include "ts_storage.h"
#include "ts_config_partitions.h"
#include <string.h>
#include <strings.h>

/* FatFs resolves its own short names and Unicode case equivalents. */
extern bool ts_storage_sd_name_is(const char *path, const char *name);

static bool directory_is(const char *path, const char *expected)
{
    const char *name = strrchr(expected, '/') + 1;
    const char *component = strrchr(path, '/') + 1;
    return !strcasecmp(component, name) || ts_storage_sd_name_is(path, name);
}

esp_err_t ts_storage_check_write_path(const char *path, bool include_children)
{
    if (!path || path[0] != '/' || strlen(path) >= TS_STORAGE_MAX_PATH) return ESP_ERR_INVALID_ARG;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        /* FatFs treats a control character as the end of the path. */
        if (*p < ' ' || *p == 0x7f) return ESP_ERR_INVALID_ARG;
    }
    const char *at = path;
    while (*at == '/') ++at;
    const char *end = strchr(at, '/');
    size_t mount = end ? (size_t)(end - at) : strlen(at);
    if (!mount) return include_children ? ESP_ERR_NOT_ALLOWED : ESP_OK;
    if (mount == strlen(&TS_CONFIG_AUTO_MOUNT[1]) && !strncmp(at, &TS_CONFIG_AUTO_MOUNT[1], mount)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (mount != 6 || strncmp(at, "sdcard", 6)) return ESP_OK;

    /* Normalize within the VFS mount. Both separators and trailing dots/spaces
     * have FAT semantics; '..' cannot change which VFS mount owns this path. */
    char normalized[TS_STORAGE_MAX_PATH] = "/sdcard";
    size_t used = 7;
    at = end ? end : at + mount;
    while (*at) {
        while (*at == '/' || *at == '\\') ++at;
        if (!*at) break;
        end = at;
        while (*end && *end != '/' && *end != '\\') ++end;
        size_t length = (size_t)(end - at);
        while (length && at[length - 1] == ' ') --length;
        if (length == 2 && !strncmp(at, "..", 2)) {
            if (used > 7) {
                used = (size_t)(strrchr(normalized, '/') - normalized);
                normalized[used] = 0;
            }
        } else if (length != 1 || at[0] != '.') {
            while (length && (at[length - 1] == '.' || at[length - 1] == ' ')) --length;
            if (!length) return ESP_ERR_INVALID_ARG;
            normalized[used++] = '/';
            memcpy(normalized + used, at, length);
            used += length;
            normalized[used] = 0;
        }
        at = end;
    }
    if (used == 7) return include_children ? ESP_ERR_NOT_ALLOWED : ESP_OK;
    char *parent_end = strchr(normalized + 8, '/');
    if (parent_end) *parent_end = 0;
    bool config_parent = directory_is(normalized, TS_CONFIG_AUTO_SD_PARENT);
    if (parent_end) *parent_end = '/';
    if (!config_parent) return ESP_OK;
    if (!parent_end) return include_children ? ESP_ERR_NOT_ALLOWED : ESP_OK;
    char *directory_end = strchr(parent_end + 1, '/');
    if (directory_end) *directory_end = 0;
    return directory_is(normalized, TS_CONFIG_AUTO_SD_DIR) ? ESP_ERR_NOT_ALLOWED : ESP_OK;
}
