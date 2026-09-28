#pragma once
#include "ts_ssh_client.h"
#include "ts_ssh_commands_config.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char state[16], last_known[16], source[16];
    uint32_t generation;
    int64_t confirmed_ms;
    bool busy;
} ts_ssh_service_status_t;
esp_err_t ts_ssh_service_init(void);
void ts_ssh_service_safe_name(const ts_ssh_command_config_t *cmd, char out[32]);
/* reserve -> probe -> execute -> finish; network calls never hold registry lock */
esp_err_t ts_ssh_service_begin(const ts_ssh_command_config_t *cmd, ts_ssh_session_t session,
                               uint32_t *generation);
bool ts_ssh_service_finish(const char *id, uint32_t generation, const char *receipt, ts_ssh_session_t session);
void ts_ssh_service_observe(const char *id, uint32_t generation, const char *state, const char *var_name);
esp_err_t ts_ssh_service_query(const char *id, ts_ssh_service_status_t *out);
esp_err_t ts_ssh_service_stop(const char *id, ts_ssh_service_status_t *out);
/* Conservative guard for mutation; unknown/active command bindings are protected. */
bool ts_ssh_service_binding_busy(const char *id);
bool ts_ssh_service_launch_command(const ts_ssh_command_config_t *cmd, const char *expanded,
                                   char *out, size_t capacity);

void ts_ssh_binding_lock(void);
void ts_ssh_binding_unlock(void);
esp_err_t ts_ssh_service_pin(const ts_ssh_command_config_t *cmd, const char *host, uint16_t port, uint32_t *registration);
void ts_ssh_service_unpin(const char *id, uint32_t registration);
bool ts_ssh_service_start_admissible(const char *id);

bool ts_ssh_service_command_protected(const char *id);
bool ts_ssh_service_host_protected(const char *host_id);

void ts_ssh_service_cancel_observation(const char *id, uint32_t generation);
bool ts_ssh_service_any_in_use(void);
void ts_ssh_service_set_owner(const char *id, uint32_t registration, const char *rule);
bool ts_ssh_service_rule_protected(const char *rule);
/* Call deletion/forget while holding binding_gate; no network under that gate. */
esp_err_t ts_ssh_service_delete_begin(const char *id, uint32_t *credential);
void ts_ssh_service_delete_finish(const char *id, uint32_t credential, bool removed);
void ts_ssh_service_forget_stopped(void);
esp_err_t ts_ssh_service_cached(const char *id, ts_ssh_service_status_t *out);
esp_err_t ts_ssh_service_verify_instance(const char *id, uint32_t generation, ts_ssh_session_t session);
bool ts_ssh_service_observation_valid(const char *id, uint32_t generation);
void ts_ssh_service_recovery_kick(const char *id);
bool ts_ssh_service_recover(const char *id, uint32_t generation);
void ts_ssh_service_recovery_done(const char *id, uint32_t generation);

#ifdef __cplusplus
}
#endif
