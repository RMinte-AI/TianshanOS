#pragma once
#include "ts_automation_document.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { TS_AUTO_CONFIG_SD, TS_AUTO_CONFIG_INTERNAL } ts_auto_config_source_t;
typedef struct ts_auto_config_view ts_auto_config_view_t;
typedef struct {
    bool loaded, enabled, paused, uncertain;
    bool configured, sd_available, internal_exists, internal_valid;
    bool comparison_known, backup_matches, awaiting_sd_choice;
    uint32_t internal_saved_at;
    const char *internal_error;
    ts_auto_config_source_t source;
    char active_version[33];
    ts_auto_config_error_t error;
} ts_auto_config_status_t;

typedef enum {
    TS_AUTO_USE_SD,
    TS_AUTO_SAVE_TO_SD,
    TS_AUTO_RESTORE_INTERNAL
} ts_auto_config_operation_t;

/* After PKI, SDK variables and empty managers are ready. */
esp_err_t ts_auto_config_startup(ts_auto_config_error_t *error);
/* Explicit check: inspect the driver once, mount once if needed, and retain
 * internal mode when a card reappears. Periodic status reads do no I/O. */
esp_err_t ts_auto_config_refresh(ts_auto_config_error_t *error);
/* Completed mount/unmount notification: no probing or automatic remount. */
esp_err_t ts_auto_config_storage_changed(ts_auto_config_error_t *error);
esp_err_t ts_auto_config_reload(ts_auto_config_error_t *error);
void ts_auto_config_shutdown(void);
esp_err_t ts_auto_config_backup(const char *active_version,ts_auto_config_error_t *error);
esp_err_t ts_auto_config_preview(ts_auto_config_operation_t operation,cJSON **preview,ts_auto_config_error_t *error);
esp_err_t ts_auto_config_apply(ts_auto_config_operation_t operation,const char *active_version,
                               const char *target_version,ts_auto_config_error_t *error);

/* Boot orchestration calls open after storage, PKI, and the empty managers are
 * ready. Legacy conversion runs before this call, never inside ordinary CRUD. */
esp_err_t ts_auto_config_open(ts_auto_config_source_t source, ts_auto_config_error_t *error);
/* One-time migration/import: prepare every runtime view before saving and
 * publishing the complete set. Invalid definitions remain visible, paused. */
esp_err_t ts_auto_config_install(ts_auto_config_source_t source, const cJSON *json,
                                ts_auto_config_error_t *error);
void ts_auto_config_close(void);
void ts_auto_config_status(ts_auto_config_status_t *out);

/* Immutable snapshots may outlive publication and permit nested lookups. */
const ts_auto_config_view_t *ts_auto_config_acquire(void);
const ts_auto_config_view_t *ts_auto_config_acquire_status(ts_auto_config_status_t *status);
void ts_auto_config_release(const ts_auto_config_view_t *view);
uint64_t ts_auto_config_execution_epoch(void);
/** Configuration ownership for live service state; unchanged by user pause. */
uint64_t ts_auto_config_binding_epoch(void);
uint64_t ts_auto_config_template_instance(const char *id);
uint64_t ts_auto_config_host_instance(const char *id);
bool ts_auto_config_template_current(uint64_t instance);
bool ts_auto_config_command_current(const char *id, uint64_t instance);
const ts_auto_document_t *ts_auto_config_definitions(const ts_auto_config_view_t *view);
const cJSON *ts_auto_config_json(const ts_auto_config_view_t *view);
void ts_auto_config_rule_status(const ts_auto_config_view_t *view, const ts_auto_config_status_t *status,
                                const char *id, cJSON *out);

/* API handlers begin/end an edit using the displayed active_version.
 * Immediate CLI/internal mutations may pass NULL to the individual operations. */
esp_err_t ts_auto_config_edit_begin(const char *active_version, ts_auto_config_error_t *error);
void ts_auto_config_edit_end(void);
esp_err_t ts_auto_config_set(ts_auto_config_kind_t kind, const cJSON *value,
                             const char *active_version, ts_auto_config_error_t *error);
/* Only the verified package importer attaches provenance. It is not reused to
 * bypass signature verification on a later import. */
esp_err_t ts_auto_config_set_rule_from_pack(const cJSON *value, const cJSON *package,
                                            const char *active_version, ts_auto_config_error_t *error);
esp_err_t ts_auto_config_replace_collection(ts_auto_config_kind_t kind, const cJSON *array,
                                            const char *active_version, ts_auto_config_error_t *error);
/* Consumes the verified import candidate on every path. A command and its host
 * become visible together, without a second complete JSON allocation. */
esp_err_t ts_auto_config_import(cJSON *document,const char *active_version,ts_auto_config_error_t *error);
uint32_t ts_auto_config_command_time(const ts_auto_config_view_t *view, int index);
void ts_auto_config_command_executed(const char *id, uint64_t instance, uint32_t timestamp);
void ts_auto_config_template_stats(const ts_auto_config_view_t *view, int index, ts_action_template_t *out);
void ts_auto_config_template_used(const ts_auto_config_view_t *view, int index, int64_t timestamp);
void ts_auto_config_template_used_instance(uint64_t instance,int64_t timestamp);
esp_err_t ts_auto_config_delete(ts_auto_config_kind_t kind, const char *id,
                                ts_auto_delete_mode_t mode, const char *replacement,
                                bool pause_confirmed, const char *active_version,
                                cJSON **impact, ts_auto_config_error_t *error);
esp_err_t ts_auto_config_delete_preview(ts_auto_config_kind_t kind, const char *id,
                                        cJSON **impact, char active_version[33],
                                        ts_auto_config_error_t *error);
esp_err_t ts_auto_config_enable(bool enabled, const char *active_version,
                                ts_auto_config_error_t *error);

#ifdef __cplusplus
}
#endif
