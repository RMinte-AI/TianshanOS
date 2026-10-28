#include "ts_automation_file.h"
#include "ts_config_partitions.h"
#include "ts_crypto.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_BYTES 0x200000U
static const char *TAG = "automation_file";

typedef struct {
    const char *current, *pending, *previous;
} paths_t;

static const paths_t s_paths[] = {
    {TS_CONFIG_AUTO_SD_DIR "/state.json", TS_CONFIG_AUTO_SD_DIR "/state.pending",
     TS_CONFIG_AUTO_SD_DIR "/state.previous"},
    {TS_CONFIG_AUTO_MOUNT "/state.json", TS_CONFIG_AUTO_MOUNT "/state.pending",
     TS_CONFIG_AUTO_MOUNT "/state.previous"}
};

static esp_err_t io_error(void)
{
    return errno == ENOSPC ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
}

static int exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

static bool remove_file(const char *path)
{
    return unlink(path) == 0 || errno == ENOENT;
}

static bool valid_document(const char *text, size_t size)
{
    cJSON *json = cJSON_ParseWithLengthOpts(text, size + 1, NULL, true);
    /* Business/schema checks belong to the configuration owner. A readable
     * object must remain available to an explicit pause-and-force-delete even
     * when some definitions or metadata are invalid. */
    bool valid = cJSON_IsObject(json);
    cJSON_Delete(json);
    return valid;
}

static esp_err_t read_bytes(const char *path, char **out, size_t *length)
{
    *out = NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : io_error();
    esp_err_t ret = ESP_OK;
    if (fseek(file, 0, SEEK_END) != 0) ret = io_error();
    long size = ret == ESP_OK ? ftell(file) : -1;
    if (ret == ESP_OK && size < 0) ret = io_error();
    if (ret == ESP_OK && fseek(file, 0, SEEK_SET) != 0) ret = io_error();
    char *text = ret == ESP_OK ?
        heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (ret == ESP_OK && !text) ret = ESP_ERR_NO_MEM;
    if (ret == ESP_OK) {
        if (fread(text, 1, size, file) != (size_t)size || ferror(file)) ret = io_error();
        else {
            text[size] = 0;
        }
    }
    if (fclose(file) != 0 && ret == ESP_OK) ret = io_error();
    if (ret == ESP_OK) { *out = text;if(length)*length=size; }
    else free(text);
    return ret;
}

static esp_err_t read_file(const char *path, char **out)
{
    size_t length=0;
    esp_err_t ret=read_bytes(path,out,&length);
    if(ret==ESP_OK && !valid_document(*out,length)){
        free(*out);*out=NULL;ret=ESP_ERR_INVALID_ARG;
    }
    return ret;
}

static esp_err_t write_pending(const char *path, const char *json)
{
    FILE *file = fopen(path, "wb");
    if (!file) return io_error();
    size_t size = strlen(json);
    esp_err_t ret = fwrite(json, 1, size, file) == size ? ESP_OK : io_error();
    if (fflush(file) != 0 && ret == ESP_OK) ret = io_error();
    if (fsync(fileno(file)) != 0 && ret == ESP_OK) ret = io_error();
    if (fclose(file) != 0 && ret == ESP_OK) ret = io_error();
    if (ret != ESP_OK) return ret;
    char *readback = NULL;
    ret = read_file(path, &readback);
    if (ret == ESP_OK && strcmp(readback, json)) ret = ESP_FAIL;
    free(readback);
    return ret;
}

/* Complete just one interrupted replacement. No search of legacy stores,
 * source switching, or persistent history is performed here. */
static esp_err_t recover(const paths_t *p, bool writing)
{
    int current = exists(p->current), pending = exists(p->pending), previous = exists(p->previous);
    if (current < 0 || pending < 0 || previous < 0) return ESP_ERR_NOT_FINISHED;
    if (pending) {
        if (previous) {
            if (current) return ESP_ERR_NOT_FINISHED;
            if (rename(p->previous, p->current) != 0) return ESP_ERR_NOT_FINISHED;
        }
        return remove_file(p->pending) ? ESP_OK : ESP_FAIL;
    }
    if (!previous) return ESP_OK;
    if (!current)
        return rename(p->previous, p->current) == 0 ? ESP_OK : ESP_ERR_NOT_FINISHED;
    char *committed = NULL;
    esp_err_t ret = read_file(p->current, &committed);
    free(committed);
    if (ret != ESP_OK) return ESP_ERR_NOT_FINISHED;
    if (!remove_file(p->previous)) {
        ESP_LOGW(TAG, "Configuration saved; previous-file cleanup is pending");
        return writing ? ESP_FAIL : ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t ensure_sd_directory(void)
{
    const char *paths[] = {TS_CONFIG_AUTO_SD_PARENT, TS_CONFIG_AUTO_SD_DIR};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (mkdir(paths[i], 0775) == 0) continue;
        if (errno != EEXIST) return io_error();
        struct stat st;
        if (stat(paths[i], &st) != 0 || !S_ISDIR(st.st_mode)) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ts_auto_file_load(ts_auto_file_location_t location, char **json)
{
    if (!json || (unsigned)location >= sizeof(s_paths) / sizeof(s_paths[0]))
        return ESP_ERR_INVALID_ARG;
    *json = NULL;
    const paths_t *p = &s_paths[location];
    esp_err_t ret = recover(p, false);
    return ret == ESP_OK ? read_file(p->current, json) : ret;
}

esp_err_t ts_auto_file_fingerprint(ts_auto_file_location_t location,char digest[65])
{
    if(!digest || (unsigned)location>=sizeof(s_paths)/sizeof(s_paths[0]))return ESP_ERR_INVALID_ARG;
    digest[0]=0;
    const paths_t *paths=&s_paths[location];
    esp_err_t ret=recover(paths,false);
    char *bytes=NULL;size_t length=0;unsigned char hash[32];
    if(ret==ESP_OK)ret=read_bytes(paths->current,&bytes,&length);
    if(ret==ESP_OK)ret=ts_crypto_hash(TS_HASH_SHA256,bytes,length,hash,sizeof(hash));
    if(ret==ESP_OK)ret=ts_crypto_hex_encode(hash,sizeof(hash),digest,65);
    free(bytes);return ret;
}

esp_err_t ts_auto_file_save(ts_auto_file_location_t location, const char *json)
{
    if ((unsigned)location >= sizeof(s_paths) / sizeof(s_paths[0]) || !json)
        return ESP_ERR_INVALID_ARG;
    size_t size = strlen(json);
    if (location == TS_AUTO_FILE_INTERNAL && size > MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    if (!valid_document(json, size)) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = location == TS_AUTO_FILE_SD ? ensure_sd_directory() : ESP_OK;
    const paths_t *p = &s_paths[location];
    if (ret == ESP_OK) ret = recover(p, true);
    if (ret != ESP_OK) return ret;
    ret = write_pending(p->pending, json);
    if (ret != ESP_OK) goto uncommitted;
    int current = exists(p->current);
    if (current < 0) { ret = ESP_FAIL; goto uncommitted; }
    if (current && rename(p->current, p->previous) != 0) {
        ret = io_error();
        goto uncommitted;
    }
    if (rename(p->pending, p->current) != 0) {
        ret = io_error();
        /* Some drivers report an error after rename took effect. Check the
         * actual installed bytes before saying the save failed. */
        char *installed = NULL;
        int pending = exists(p->pending);
        esp_err_t observed = read_file(p->current, &installed);
        bool committed = pending == 0 && observed == ESP_OK && !strcmp(installed, json);
        free(installed);
        if (!committed) {
            if (pending != 1 || (observed != ESP_ERR_NOT_FOUND && observed != ESP_OK))
                return ESP_ERR_NOT_FINISHED;
            goto uncommitted;
        }
    }
    /* The new complete document now has the target name: commit succeeded.
     * Cleanup failure cannot turn it into a reported failed save. */
    if (!remove_file(p->previous))
        ESP_LOGW(TAG, "Configuration saved; previous-file cleanup is pending");
    return ESP_OK;

uncommitted:
    return recover(p, false) == ESP_OK ? ret : ESP_ERR_NOT_FINISHED;
}
