#pragma once
#include "ts_config_pack.h"
#include "ts_automation_types.h"
#include "ts_automation_document.h"

/* Strict envelope adapter; plain JSON continues to use ts_rule_decode directly. */
esp_err_t ts_rule_pack_decode(const char *content, size_t length, ts_auto_rule_t *rule);
const char *ts_rule_pack_error(ts_config_pack_result_t error);
/* Provenance is an annotation in installed configuration, not a trust grant. */
bool ts_rule_pack_definition_digest(const ts_auto_rule_t *rule, char out[65]);
cJSON *ts_rule_pack_provenance(const ts_auto_rule_t *rule, const ts_config_pack_acceptance_t *accepted);
esp_err_t ts_rule_pack_dependencies(const ts_auto_document_t *document, const ts_auto_rule_t *rule,
                                    const char **reason);
