#pragma once
#include "ts_automation_types.h"
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Decode owns arrays. Dispose decoded values; release engine reads separately. */
esp_err_t ts_rule_decode(const cJSON *json, ts_auto_rule_t *rule);
cJSON *ts_rule_encode(const ts_auto_rule_t *rule);
void ts_rule_dispose(ts_auto_rule_t *rule);
bool ts_rule_id_valid(const char *id);
/* A value object has "value" and optional "value_type" (preserves float 1.0). */
bool ts_auto_value_decode(const cJSON *object, ts_auto_value_t *value);
cJSON *ts_auto_value_encode(const ts_auto_value_t *value);
#ifdef __cplusplus
}
#endif
