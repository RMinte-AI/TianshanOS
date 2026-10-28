#pragma once
#include "ts_automation_types.h"
#include "ts_ssh_commands_config.h"
#include "ts_ssh_hosts_config.h"
#include "cJSON.h"

typedef enum {
    TS_AUTO_CONFIG_RULE,
    TS_AUTO_CONFIG_ACTION,
    TS_AUTO_CONFIG_COMMAND,
    TS_AUTO_CONFIG_HOST,
    TS_AUTO_CONFIG_SOURCE,
    TS_AUTO_CONFIG_VARIABLE,
    TS_AUTO_CONFIG_KIND_COUNT
} ts_auto_config_kind_t;

typedef struct {
    const char *reason;
    bool saved; /* File committed, but a later explicit mode/pause change failed. */
    const char *collection;
    char id[TS_AUTO_NAME_MAX_LEN];
    char dependency[TS_AUTO_NAME_MAX_LEN];
} ts_auto_config_error_t;

/* A complete, prepared definition set. No live manager or remote state is read. */
typedef struct {
    bool enabled;
    uint32_t revision, saved_at, migration_version;
    char pause_reason[64];
    int rule_count, action_count, command_count, host_count, source_count, variable_count;
    ts_auto_rule_t *rules;
    cJSON **rule_packages; /* Import provenance only; never a verification authority. */
    ts_action_template_t *actions;
    ts_ssh_command_config_t *commands;
    ts_ssh_host_config_t *hosts;
    ts_auto_source_t *sources;
    ts_auto_variable_t *variables;
} ts_auto_document_t;

const char *ts_auto_config_collection(ts_auto_config_kind_t kind);
int ts_auto_config_limit(ts_auto_config_kind_t kind);
esp_err_t ts_auto_document_check_item(ts_auto_config_kind_t kind, const cJSON *json,
                                     ts_auto_config_error_t *error);
esp_err_t ts_auto_document_decode(const cJSON *json, ts_auto_document_t **out,
                                 ts_auto_config_error_t *error);
esp_err_t ts_auto_document_validate(const ts_auto_document_t *document,
                                   ts_auto_config_error_t *error);
cJSON *ts_auto_document_encode(const ts_auto_document_t *document);
void ts_auto_document_free(ts_auto_document_t *document);

typedef enum {
    TS_AUTO_DELETE_NORMAL,
    TS_AUTO_DELETE_REPLACE,
    TS_AUTO_DELETE_CASCADE,
    TS_AUTO_DELETE_FORCE
} ts_auto_delete_mode_t;

/* Mutations operate on an unpublished candidate. Discard it on any error.
 * A confirmed pause-and-force operation never requires the normal impact map. */
esp_err_t ts_auto_document_impact(const cJSON *json, ts_auto_config_kind_t kind,
                                 const char *id, cJSON **impact, ts_auto_config_error_t *error);
esp_err_t ts_auto_document_remove(cJSON *json, ts_auto_config_kind_t kind, const char *id,
                                 ts_auto_delete_mode_t mode, const char *replacement,
                                 bool pause_confirmed, cJSON **impact, ts_auto_config_error_t *error);
