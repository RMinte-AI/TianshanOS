#pragma once
#include "ts_rule_codec.h"
#define TS_RULE_SOURCE_NVS 0
#define TS_RULE_SOURCE_SD 1
#define TS_RULE_SOURCE_READONLY 2
typedef struct {int applied,durable,mirror_synced;uint32_t revision;const char *error_code;} ts_rule_commit_t;
/* All calls serialized by engine configuration mutex, never evaluation mutex. */
esp_err_t ts_rule_store_recover(bool sd_mounted, bool *use_staging, bool *require_sd);
esp_err_t ts_rule_store_load_bank(ts_auto_rule_t *rules,int capacity,int *count);
esp_err_t ts_rule_store_commit(const ts_auto_rule_t *rules,int count,const ts_auto_rule_t *candidate,
                              const char *id,int source,ts_rule_commit_t *result);
