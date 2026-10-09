#pragma once
#include "ts_rule_store.h"
#include "ts_config_pack.h"

typedef enum { TS_RULE_SET_NVS_JSON, TS_RULE_SET_SD_JSON, TS_RULE_SET_SD_PACK } ts_rule_set_source_t;
typedef struct {
    char id[TS_AUTO_NAME_MAX_LEN], digest[65];
    uint32_t revision, generation;
    ts_rule_set_source_t source;
    bool readonly;
    char name[TS_AUTO_LABEL_MAX_LEN], icon[sizeof(((ts_auto_rule_t *)0)->icon)];
    bool enabled;
    uint32_t conditions_count, actions_count;
} ts_rule_saved_info_t;

/* Same engine configuration transaction as legacy store. One selector, complete saved set.
 * Legacy recovery/reading finishes before adopt; no write or migration during reads. */
void ts_rule_store_set_reset(void);
esp_err_t ts_rule_store_set_load(bool sd, ts_auto_rule_t *rules, bool *readonly, int capacity, int *count);
esp_err_t ts_rule_store_set_refresh(bool sd);
const char *ts_rule_store_set_error(void);
esp_err_t ts_rule_store_set_seed(const ts_auto_rule_t *rule, const char *path, bool readonly,
                               const ts_config_pack_acceptance_t *accepted);
esp_err_t ts_rule_store_set_adopt(const ts_auto_rule_t *rules, int count, int legacy_source,
                                const bool *readonly);
/* Called only after legacy recovery and complete dependency validation at boot. */
esp_err_t ts_rule_store_set_migrate(void);
bool ts_rule_store_set_available(void);
bool ts_rule_store_set_present(void);
int ts_rule_store_set_count(void);
uint32_t ts_rule_store_set_generation(void);
esp_err_t ts_rule_store_set_info(const char *id, ts_rule_saved_info_t *info);
esp_err_t ts_rule_store_set_info_at(int index, ts_rule_saved_info_t *info);
esp_err_t ts_rule_store_set_acceptance(const char *bytes,size_t length,ts_config_pack_acceptance_t *out);
esp_err_t ts_rule_store_set_get(int index, ts_auto_rule_t *rule, ts_rule_saved_info_t *info);
esp_err_t ts_rule_store_set_commit(const ts_auto_rule_t *candidate, const char *id,
    uint32_t expected_revision, uint32_t expected_generation, const char *pack, size_t pack_len,
    const ts_config_pack_acceptance_t *accepted, ts_rule_commit_t *result);
