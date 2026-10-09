#include "ts_action_store.h"
#include "ts_automation_types.h"
#include "cJSON.h"
#include "esp_log.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TS_ACTIONS_DIR
#define TS_ACTIONS_DIR "/sdcard/config/actions"
#endif
static const char *TAG = "action_store";
#define PATH_CAPACITY (sizeof(TS_ACTIONS_DIR) + TS_AUTO_NAME_MAX_LEN + sizeof(".tscfg.previous") + 2)

typedef struct {
    char json[PATH_CAPACITY], pack[PATH_CAPACITY], pending[PATH_CAPACITY];
    char old_json[PATH_CAPACITY], old_pack[PATH_CAPACITY];
} paths_t;

static bool paths(const char *id, paths_t *p)
{
    if (!id || !*id || strlen(id) >= TS_AUTO_NAME_MAX_LEN || strchr(id, '/') ||
        !strcmp(id, ".") || !strcmp(id, "..")) return false;
    snprintf(p->json, sizeof(p->json), TS_ACTIONS_DIR "/%s.json", id);
    snprintf(p->pack, sizeof(p->pack), TS_ACTIONS_DIR "/%s.tscfg", id);
    snprintf(p->pending, sizeof(p->pending), TS_ACTIONS_DIR "/.%s.pending", id);
    snprintf(p->old_json, sizeof(p->old_json), TS_ACTIONS_DIR "/.%s.json.previous", id);
    snprintf(p->old_pack, sizeof(p->old_pack), TS_ACTIONS_DIR "/.%s.tscfg.previous", id);
    return true;
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

static bool valid_json(const char *text, const char *id)
{
    cJSON *root = cJSON_Parse(text);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    bool ok = cJSON_IsObject(root) && cJSON_IsString(name) && !strcmp(name->valuestring, id) &&
              cJSON_IsString(type);
    cJSON_Delete(root);
    return ok;
}

static bool valid_file(const char *path, const char *id)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long size = ok ? ftell(file) : -1;
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) ok = false;
    char *text = ok ? malloc((size_t)size + 1) : NULL;
    if (!text) ok = false;
    if (ok) {
        ok = fread(text, 1, (size_t)size, file) == (size_t)size && !ferror(file);
        text[size] = 0;
    }
    if (fclose(file) != 0) ok = false;
    if (ok) ok = valid_json(text, id);
    free(text);
    return ok;
}

static bool write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t size = strlen(text);
    bool ok = fwrite(text, 1, size, file) == size;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool restore(const char *old, const char *target)
{
    int present = exists(old);
    if (present < 0) return false;
    return !present || (remove_file(target) && rename(old, target) == 0);
}

static esp_err_t recover_one(const char *id, const paths_t *p, bool for_write)
{
    int pending = exists(p->pending), old_json = exists(p->old_json), old_pack = exists(p->old_pack);
    if (pending < 0 || old_json < 0 || old_pack < 0) return ESP_FAIL;
    if (!pending && !old_json && !old_pack) return ESP_OK;

    /* Pending remains until the final rename, so it also records rollback
     * intent. Remove it only after BOTH original files have been restored. */
    if (pending) {
        if (!restore(p->old_json, p->json) || !restore(p->old_pack, p->pack) ||
            !remove_file(p->pending)) return ESP_FAIL;
        return ESP_OK;
    }

    int target = exists(p->json);
    if (target < 0) return ESP_FAIL;
    if (!target) {
        /* A missing new file cannot win over a complete backup.
         * Persist rollback intent before restoring either member. */
        if (!write_file(p->pending, "") || !restore(p->old_json, p->json) ||
            !restore(p->old_pack, p->pack) || !remove_file(p->pending)) return ESP_FAIL;
        return ESP_OK;
    }
    /* A read/allocation failure is not evidence that the commit failed.
     * Retain both versions until the committed file can be inspected. */
    if (!valid_file(p->json, id)) return ESP_FAIL;
    /* Final JSON is complete and pending is gone: the replacement committed.
     * Cleanup failure cannot undo it, but must block another replacement. */
    bool cleaned = remove_file(p->old_json) && remove_file(p->old_pack);
    if (!cleaned) {
        ESP_LOGW(TAG, "Committed template %s has backup cleanup pending", id);
        return for_write ? ESP_FAIL : ESP_OK;
    }
    return ESP_OK;
}

esp_err_t ts_action_store_save(const char *id, const char *json, bool replace_pack)
{
    paths_t p;
    if (!paths(id, &p) || !json || !valid_json(json, id)) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = recover_one(id, &p, true);
    if (ret != ESP_OK) return ret;
    if (!write_file(p.pending, json)) goto rollback;
    int had_json = exists(p.json), had_pack = replace_pack ? exists(p.pack) : 0;
    if (had_json < 0 || had_pack < 0) goto rollback;
    if (had_json && rename(p.json, p.old_json) != 0) goto rollback;
    if (had_pack && rename(p.pack, p.old_pack) != 0) goto rollback;
    if (rename(p.pending, p.json) != 0) {
        /* Some I/O errors report after the operation applied. Observe the
         * actual rename result before claiming the old version survived. */
        if (exists(p.pending) != 0 || !valid_file(p.json, id)) goto rollback;
    }
    (void)recover_one(id, &p, false);
    return ESP_OK;
rollback:
    if (recover_one(id, &p, false) != ESP_OK) ESP_LOGE(TAG, "Template %s needs storage recovery", id);
    return ESP_FAIL;
}

esp_err_t ts_action_store_recover(void)
{
    DIR *dir = opendir(TS_ACTIONS_DIR);
    if (!dir) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    esp_err_t ret = ESP_OK;
    struct dirent *entry;
    const char *suffixes[] = {".pending", ".json.previous", ".tscfg.previous"};
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] != '.') continue;
        size_t length = strlen(entry->d_name);
        for (size_t i = 0; i < 3; i++) {
            size_t suffix = strlen(suffixes[i]);
            if (length <= suffix + 1 || strcmp(entry->d_name + length - suffix, suffixes[i])) continue;
            size_t id_length = length - suffix - 1;
            if (id_length >= TS_AUTO_NAME_MAX_LEN) { ret = ESP_ERR_INVALID_ARG; break; }
            char id[TS_AUTO_NAME_MAX_LEN];
            memcpy(id, entry->d_name + 1, id_length); id[id_length] = 0;
            paths_t p;
            ret = paths(id, &p) ? recover_one(id, &p, false) : ESP_ERR_INVALID_ARG;
            break;
        }
        if (ret != ESP_OK) break;
    }
    if (closedir(dir) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    return ret;
}
