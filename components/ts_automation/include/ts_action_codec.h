#pragma once
#include "ts_automation_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure configuration conversions: no manager, NVS, SSH or variable lookups.
 * Encoded strings belong to the caller. */
esp_err_t ts_action_parse_color(const char *color, uint8_t *r, uint8_t *g, uint8_t *b);
char *ts_action_template_encode(const ts_action_template_t *value);
esp_err_t ts_action_template_decode(const char *json, ts_action_template_t *value);
char *ts_source_config_encode(const ts_auto_source_t *value);
esp_err_t ts_source_config_decode(const char *json, ts_auto_source_t *value);
bool ts_source_config_same(const ts_auto_source_t *a, const ts_auto_source_t *b);

#ifdef __cplusplus
}
#endif
