#pragma once
#include "ts_automation_types.h"
#include "ts_automation_document.h"

/* Prepare all allocations before persistence. Publication owns the prepared
 * arrays and does not allocate. Publication holds the shared execution admission gate. */
typedef struct ts_rule_config_view ts_rule_config_view_t;
esp_err_t ts_rule_config_prepare(const ts_auto_rule_t *rules, int count, bool keep_runtime, ts_rule_config_view_t **out);
void ts_rule_config_discard(ts_rule_config_view_t *view);
void ts_rule_config_publish(ts_rule_config_view_t *view, bool enabled,
    const ts_auto_document_t *previous,const ts_auto_document_t *document);
void ts_rule_config_pause(void);
void ts_rule_config_clear(void);

/* The prepared variable table reserves registration capacity until publish or
 * discard. Existing value updates remain available during file I/O. */
typedef struct ts_variable_config_view ts_variable_config_view_t;
typedef struct ts_source_config_view ts_source_config_view_t;
esp_err_t ts_variable_config_prepare(const ts_auto_document_t *document, const ts_source_config_view_t *sources, const ts_auto_document_t *previous,
                                     ts_variable_config_view_t **out);
void ts_variable_config_discard(ts_variable_config_view_t *view);
void ts_variable_config_publish(ts_variable_config_view_t *view);
void ts_variable_config_clear(const ts_auto_document_t *previous);

esp_err_t ts_source_config_prepare(ts_auto_document_t *document, bool keep_runtime, ts_source_config_view_t **out);
bool ts_source_config_keeps_variables(const ts_source_config_view_t *view, const char *source);
void ts_source_config_discard(ts_source_config_view_t *view);
void ts_source_config_publish(ts_source_config_view_t *view);
void ts_source_config_clear(void);
/* Starts transport work after the short publication locks are released. */
void ts_source_config_activate(void);
