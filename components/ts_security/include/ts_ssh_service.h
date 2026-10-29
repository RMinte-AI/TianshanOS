#pragma once
#include "ts_ssh_client.h"
#include "ts_ssh_commands_config.h"
#include "ts_ssh_hosts_config.h"
#include "ts_automation_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TS_SSH_RUNS_MAX 64
/* One pending direct start, separate from the durable run limit. */
#define TS_SSH_RUN_SLOTS (TS_SSH_RUNS_MAX + 1)
#define TS_SSH_RUN_ID_MAX 32

typedef enum { TS_SERVICE_START = 1, TS_SERVICE_VERIFY, TS_SERVICE_STOP } ts_service_operation_t;
typedef enum { TS_SSH_RUN_GROUP, TS_SSH_RUN_LEGACY_GROUP, TS_SSH_RUN_LEGACY_PID } ts_ssh_run_kind_t;
typedef struct {
    char state[16], last_known[16], source[16];
    uint32_t generation;
    int64_t confirmed_ms;
    bool busy;
    uint32_t operation_id;
    char operation_phase[12], operation_kind[8];
    esp_err_t operation_error;
    char operation_reason[48];
} ts_ssh_service_status_t;
/* Persistent control ownership, independent of configuration definitions. */
typedef struct {
    char run_id[TS_SSH_RUN_ID_MAX], command_id[TS_SSH_CMD_ID_MAX], name[TS_SSH_CMD_NAME_MAX];
    char rule[TS_AUTO_NAME_MAX_LEN], config_version[33];
    ts_ssh_host_config_t host;
    char variable[TS_SSH_CMD_VARNAME_MAX], locator[32], instance[96];
    ts_ssh_run_kind_t kind;
} ts_ssh_run_record_t;
/* In-memory publication permission. epoch is configuration binding ownership,
 * not action execution permission. Never restored from the persistent record. */
typedef struct {
    uint64_t epoch, command_instance, host_instance, variables[2];
    int64_t deadline_ms;
    char rule[TS_AUTO_NAME_MAX_LEN], config_version[33];
} ts_ssh_run_origin_t;

esp_err_t ts_ssh_service_init(void);
void ts_ssh_binding_lock(void);
bool ts_ssh_binding_try_lock(void);
void ts_ssh_binding_unlock(void);
/* Executor-only: verify retained runs once before allowing a new start. */
esp_err_t ts_ssh_service_check_start(const char *command_id, const ts_ssh_host_config_t *host,
    const char *exclude_run, int64_t deadline_ms, const char **reason);
/* Saves the planned locator before returning a launch reservation. */
esp_err_t ts_ssh_service_prepare(const ts_ssh_command_config_t *command,
    const ts_ssh_host_config_t *host, const ts_ssh_run_origin_t *origin,
    char run_id[TS_SSH_RUN_ID_MAX], uint32_t *operation_id, const char **reason);
/* HTTP admission creates only a queued plan. The executor persists it before launch. */
esp_err_t ts_ssh_service_plan(const ts_ssh_command_config_t *command,
    const ts_ssh_host_config_t *host, const ts_ssh_run_origin_t *origin,
    char run_id[TS_SSH_RUN_ID_MAX], uint32_t *operation_id, const char **reason);
esp_err_t ts_ssh_service_import_legacy(const ts_ssh_command_config_t *command,
    const ts_ssh_host_config_t *host);
esp_err_t ts_ssh_service_reserve_operation(const char *run_id,
    ts_service_operation_t kind, uint32_t *operation_id, const char **reason);
esp_err_t ts_ssh_service_operation_begin(const char *run_id, uint32_t operation_id);
void ts_ssh_service_set_operation_reason(const char *run_id, uint32_t operation_id, const char *reason);
bool ts_ssh_service_complete_operation(const char *run_id, uint32_t operation_id, esp_err_t error);
esp_err_t ts_ssh_service_begin_reserved(const char *run_id, ts_ssh_session_t session,
    uint32_t operation_id, uint32_t *generation, const char **reason);
esp_err_t ts_ssh_service_finish(const char *run_id, uint32_t generation,
    const char *receipt, ts_ssh_session_t session);
bool ts_ssh_service_launch_command(const char *run_id, const char *expanded, char *out, size_t capacity);
esp_err_t ts_ssh_service_execute_control(const char *run_id, uint32_t operation_id,
    ts_service_operation_t kind, ts_ssh_service_status_t *out);
esp_err_t ts_ssh_service_cached(const char *run_id, ts_ssh_service_status_t *out);
/* Cached run on a target. A nonzero definition instance scopes display state;
 * zero checks physical runs across definition changes for duplicate admission. */
bool ts_ssh_service_current(const char *command_id,const ts_ssh_host_config_t *host,
    uint64_t command_instance,char run_id[TS_SSH_RUN_ID_MAX],ts_ssh_service_status_t *status);
esp_err_t ts_ssh_service_get(unsigned slot, ts_ssh_run_record_t *record, ts_ssh_service_status_t *status);
esp_err_t ts_ssh_service_target(const char *run_id, uint32_t generation,
    ts_ssh_host_config_t *host, char log_file[256]);
esp_err_t ts_ssh_service_verify_instance(const char *run_id, uint32_t generation, ts_ssh_session_t session);
bool ts_ssh_service_observation_valid(const char *run_id, uint32_t generation);
void ts_ssh_service_observe(const char *run_id, uint32_t generation, const char *state);
void ts_ssh_service_cancel_observation(const char *run_id, uint32_t generation);
/* Connect using the frozen target and key reference. Caller owns session/key. */
esp_err_t ts_ssh_service_connect(const ts_ssh_host_config_t *host, ts_ssh_session_t *session,
    char **key, bool (*cancelled)(void *), void *context);

#ifdef __cplusplus
}
#endif
