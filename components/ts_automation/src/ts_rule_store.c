#include "ts_rule_store.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef DIR_RULES
#define DIR_RULES "/sdcard/config/rules"
#endif
#define MAGIC 0x52554c32u
#define NS "rule_stage"
typedef struct {
    uint32_t magic, sequence, phase, source, bank, mirror, old_hash, new_hash;
    uint8_t had_old, deleting;
    char id[TS_AUTO_NAME_MAX_LEN];
} guard_t;
typedef struct {
    uint32_t count, hash;
} bank_meta_t;
static guard_t guard;
static bool recovery_error;
static uint32_t hash_bytes(uint32_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}
static uint32_t hash_text(const char *s) { return hash_bytes(2166136261u, s, strlen(s)); }
static esp_err_t guard_read(guard_t *g) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e != ESP_OK)
        return e;
    size_t n = sizeof(*g);
    e = nvs_get_blob(h, "guard", g, &n);
    nvs_close(h);
    return e == ESP_OK && n != sizeof(*g) ? ESP_ERR_INVALID_SIZE : e;
}
/* Readback is authoritative even if a commit call reports an uncertain outcome. */
static esp_err_t guard_write(const guard_t *g) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_blob(h, "guard", g, sizeof(*g));
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    guard_t actual = {0};
    esp_err_t read = guard_read(&actual);
    if (read == ESP_OK && !memcmp(&actual, g, sizeof(*g))) {
        guard = *g;
        return ESP_OK;
    }
    return e == ESP_OK ? ESP_FAIL : e;
}
static void paths(const char *id, char *target, char *temp, char *backup) {
    snprintf(target, 160, DIR_RULES "/%s.json", id);
    snprintf(temp, 160, DIR_RULES "/.%s.pending", id);
    snprintf(backup, 160, DIR_RULES "/.%s.previous", id);
}
static esp_err_t file_hash(const char *path, uint32_t *hash) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    char buf[256];
    size_t n;
    uint32_t h = 2166136261u;
    while ((n = fread(buf, 1, sizeof(buf), f)))
        h = hash_bytes(h, buf, n);
    bool ok = !ferror(f);
    if (fclose(f))
        ok = false;
    if (!ok)
        return ESP_FAIL;
    *hash = h;
    return ESP_OK;
}
static bool matches(const char *path, uint32_t expected) {
    uint32_t hash;
    return file_hash(path, &hash) == ESP_OK && hash == expected;
}
static esp_err_t remove_if_present(const char *path) {
    return unlink(path) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL;
}
static esp_err_t write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return ESP_FAIL;
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    if (fflush(f))
        ok = false;
    if (fsync(fileno(f)))
        ok = false;
    if (fclose(f))
        ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}
static esp_err_t rollback_sd(const guard_t *g) {
    char target[160], temp[160], bak[160];
    paths(g->id, target, temp, bak);
    if (g->had_old) {
        if (!matches(target, g->old_hash)) {
            if (!matches(bak, g->old_hash))
                return ESP_FAIL;
            if (remove_if_present(target) != ESP_OK || rename(bak, target))
                return ESP_FAIL;
        }
        if (!matches(target, g->old_hash))
            return ESP_FAIL;
    } else if (remove_if_present(target) != ESP_OK)
        return ESP_FAIL;
    if (remove_if_present(temp) != ESP_OK)
        return ESP_FAIL;
    guard_t clean = *g;
    clean.phase = 0;
    clean.mirror = 0;
    return guard_write(&clean);
}
static esp_err_t finish_sd(const guard_t *g) {
    char target[160], temp[160], bak[160];
    paths(g->id, target, temp, bak);
    if (g->deleting) {
        uint32_t unused;
        if (file_hash(target, &unused) != ESP_ERR_NOT_FOUND)
            return ESP_FAIL;
    } else if (!matches(target, g->new_hash))
        return ESP_FAIL;
    /* Keep a committed journal until the mirror is known current. */
    if (remove_if_present(temp) != ESP_OK || remove_if_present(bak) != ESP_OK)
        return ESP_FAIL;
    return ESP_OK;
}
esp_err_t ts_rule_store_recover(bool sd, bool *staging, bool *require_sd) {
    *staging = false;
    *require_sd = false;
    recovery_error = false;
    memset(&guard, 0, sizeof(guard));
    esp_err_t e = guard_read(&guard);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        memset(&guard, 0, sizeof(guard));
        return ESP_OK;
    }
    if (e != ESP_OK)
        goto bad;
    if (guard.magic != MAGIC || guard.bank > 1 || guard.phase > 2 || guard.source > 1)
        goto bad;
    if (guard.source == TS_RULE_SOURCE_SD) {
        *require_sd = true;
        if (!sd) {
            if (guard.phase == 0 && guard.mirror) {
                *staging = true;
                return ESP_OK;
            }
            goto bad;
        }
        if (guard.phase == 1) {
            if (!ts_rule_id_valid(guard.id) || rollback_sd(&guard) != ESP_OK)
                goto bad;
        } else if (guard.phase == 2) {
            if (!ts_rule_id_valid(guard.id) || finish_sd(&guard) != ESP_OK)
                goto bad;
        }
    } else
        *staging = true;
    return ESP_OK;
bad:
    recovery_error = true;
    return ESP_ERR_INVALID_STATE;
}
static char *bank_json(nvs_handle_t h, unsigned bank, unsigned i) {
    char key[16];
    snprintf(key, sizeof(key), "%c%u", bank ? 'b' : 'a', i);
    size_t n = 0;
    if (nvs_get_blob(h, key, NULL, &n) != ESP_OK || n < 2 || n > 262144)
        return NULL;
    char *s = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s)
        return NULL;
    if (nvs_get_blob(h, key, s, &n) != ESP_OK || s[n - 1] != 0 || strlen(s) != n - 1) {
        free(s);
        return NULL;
    }
    return s;
}
esp_err_t ts_rule_store_load_bank(ts_auto_rule_t *rules, int capacity, int *count) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e != ESP_OK)
        return e;
    bank_meta_t meta;
    size_t n = sizeof(meta);
    e = nvs_get_blob(h, guard.bank ? "meta_b" : "meta_a", &meta, &n);
    if (e != ESP_OK || n != sizeof(meta) || meta.count > (unsigned)capacity) {
        nvs_close(h);
        return ESP_FAIL;
    }
    uint32_t hash = 2166136261u;
    *count = 0;
    for (unsigned i = 0; i < meta.count; ++i) {
        char *s = bank_json(h, guard.bank, i);
        if (!s) {
            e = ESP_FAIL;
            break;
        }
        hash = hash_bytes(hash, s, strlen(s) + 1);
        cJSON *j = cJSON_Parse(s);
        free(s);
        e = ts_rule_decode(j, &rules[i]);
        cJSON_Delete(j);
        if (e != ESP_OK)
            break;
        for (unsigned k = 0; k < i; ++k)
            if (!strcmp(rules[i].id, rules[k].id))
                e = ESP_ERR_INVALID_ARG;
        ++*count;
        if (e != ESP_OK)
            break;
    }
    nvs_close(h);
    if (e == ESP_OK && hash != meta.hash)
        e = ESP_FAIL;
    if (e != ESP_OK) {
        for (int i = 0; i < *count; ++i)
            ts_rule_dispose(&rules[i]);
        *count = 0;
    }
    return e;
}
static esp_err_t stage_bank(const ts_auto_rule_t *rules, int count, const ts_auto_rule_t *candidate,
                            const char *id, unsigned bank) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    bank_meta_t meta = {.hash = 2166136261u};
    bool replaced = false;
    for (int i = 0; i <= count; ++i) {
        const ts_auto_rule_t *r = i < count ? &rules[i] : NULL;
        if (r && !strcmp(r->id, id)) {
            r = candidate;
            replaced = true;
        } else if (i == count && !replaced)
            r = candidate;
        if (!r)
            continue;
        cJSON *j = ts_rule_encode(r);
        char *s = j ? cJSON_PrintUnformatted(j) : NULL;
        cJSON_Delete(j);
        if (!s) {
            e = ESP_ERR_NO_MEM;
            break;
        }
        char key[16];
        snprintf(key, sizeof(key), "%c%u", bank ? 'b' : 'a', (unsigned)meta.count);
        e = nvs_set_blob(h, key, s, strlen(s) + 1);
        if (e == ESP_OK) {
            meta.hash = hash_bytes(meta.hash, s, strlen(s) + 1);
            ++meta.count;
        }
        free(s);
        if (e != ESP_OK)
            break;
    }
    if (e == ESP_OK)
        e = nvs_set_blob(h, bank ? "meta_b" : "meta_a", &meta, sizeof(meta));
    if (e == ESP_OK)
        e = nvs_commit(h);
    if (e == ESP_OK) {
        bank_meta_t readback;
        size_t size = sizeof(readback);
        if (nvs_get_blob(h, bank ? "meta_b" : "meta_a", &readback, &size) != ESP_OK ||
            size != sizeof(readback) || memcmp(&readback, &meta, size))
            e = ESP_FAIL;
    }
    if (e == ESP_OK) {
        uint32_t hash = 2166136261u;
        for (unsigned i = 0; i < meta.count; ++i) {
            char *s = bank_json(h, bank, i);
            if (!s) {
                e = ESP_FAIL;
                break;
            }
            hash = hash_bytes(hash, s, strlen(s) + 1);
            free(s);
        }
        if (hash != meta.hash)
            e = ESP_FAIL;
    }
    nvs_close(h);
    return e;
}
esp_err_t ts_rule_store_commit(const ts_auto_rule_t *rules, int count,
                               const ts_auto_rule_t *candidate, const char *id, int source,
                               ts_rule_commit_t *r) {
    *r = (ts_rule_commit_t){
        .applied = 0, .durable = 0, .mirror_synced = 0, .error_code = "storage_failed"};
    if (recovery_error) {
        r->error_code = "recovery_required";
        return ESP_ERR_INVALID_STATE;
    }
    if (source == TS_RULE_SOURCE_READONLY) {
        r->error_code = "source_read_only";
        return ESP_ERR_NOT_SUPPORTED;
    }
    guard_t next = guard;
    next.magic = MAGIC;
    next.sequence++;
    next.source = source;
    unsigned bank = guard.magic ? 1 - guard.bank : 0;
    if (source == TS_RULE_SOURCE_NVS) {
        esp_err_t e = stage_bank(rules, count, candidate, id, bank);
        if (e != ESP_OK)
            return e;
        next.bank = bank;
        next.phase = 0;
        next.mirror = 1;
        if (guard_write(&next) != ESP_OK) {
            guard_t actual;
            if (guard_read(&actual) != ESP_OK || memcmp(&actual, &guard, sizeof(guard))) {
                recovery_error = true;
                r->applied = r->durable = -1;
                r->error_code = "commit_unknown";
            }
            return ESP_FAIL;
        }
        r->applied = r->durable = r->mirror_synced = 1;
        r->error_code = "ok";
        return ESP_OK;
    }
    char target[160], temp[160], bak[160];
    paths(id, target, temp, bak);
    uint32_t oldhash = 0;
    esp_err_t prior = file_hash(target, &oldhash);
    if (prior != ESP_OK && prior != ESP_ERR_NOT_FOUND)
        return prior;
    cJSON *j = candidate ? ts_rule_encode(candidate) : NULL;
    char *text = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (candidate && !text)
        return ESP_ERR_NO_MEM;
    next.phase = 1;
    next.mirror = 0;
    next.had_old = prior == ESP_OK;
    next.old_hash = oldhash;
    next.new_hash = text ? hash_text(text) : 0;
    next.deleting = !candidate;
    strcpy(next.id, id);
    /* Persist SD authority before touching it: stale NVS cannot silently reappear. */
    if (guard_write(&next) != ESP_OK) {
        free(text);
        return ESP_FAIL;
    }
    bool ok = remove_if_present(temp) == ESP_OK && remove_if_present(bak) == ESP_OK;
    if (ok && text)
        ok = write_file(temp, text) == ESP_OK && matches(temp, next.new_hash);
    free(text);
    if (ok && next.had_old)
        ok = rename(target, bak) == 0;
    if (ok && candidate)
        ok = rename(temp, target) == 0;
    if (ok) {
        next.phase = 2;
        ok = guard_write(&next) == ESP_OK;
    }
    if (!ok) {
        if (rollback_sd(&next) != ESP_OK) {
            recovery_error = true;
            r->applied = r->durable = -1;
            r->error_code = "commit_unknown";
        }
        return ESP_FAIL;
    }
    r->applied = r->durable = 1;
    r->error_code = "mirror_failed";
    if (stage_bank(rules, count, candidate, id, bank) == ESP_OK) {
        next.bank = bank;
        next.mirror = 1;
        if (guard_write(&next) == ESP_OK) {
            r->mirror_synced = 1;
            r->error_code = "ok";
        }
    }
    if (finish_sd(&guard) != ESP_OK) {
        r->mirror_synced = 0;
        r->error_code = "recovery_cleanup_pending";
    } else if (r->mirror_synced) {
        next = guard;
        next.phase = 0;
        if (guard_write(&next) != ESP_OK) {
            r->mirror_synced = 0;
            r->error_code = "recovery_cleanup_pending";
        }
    }
    return ESP_OK;
}
