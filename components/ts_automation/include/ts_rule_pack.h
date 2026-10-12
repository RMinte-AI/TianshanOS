#pragma once
#include "ts_config_pack.h"
#include "ts_automation_types.h"
#include "ts_rule_engine.h"

/* Strict envelope adapter; plain JSON continues to use ts_rule_decode directly. */
esp_err_t ts_rule_pack_decode(const char *content, size_t length, ts_auto_rule_t *rule);
const char *ts_rule_pack_error(ts_config_pack_result_t error);
/* Caller holds the SSH configuration binding lock. No execution, connection or pin. */
esp_err_t ts_rule_pack_dependencies(const ts_auto_rule_t *rule, const char **reason, cJSON **warnings);
esp_err_t ts_rule_pack_dependency_change(const ts_auto_rule_t *rule, ts_rule_dependency_t kind,
    const char *id, const void *next, const char **reason);
