#pragma once
#include "ts_ssh_commands_config.h"
#include "ts_ssh_hosts_config.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Only definition fields are encoded. Runtime state belongs to service control. */
cJSON *ts_ssh_command_encode(const ts_ssh_command_config_t *value);
esp_err_t ts_ssh_command_decode(const cJSON *json, ts_ssh_command_config_t *value);
cJSON *ts_ssh_host_encode(const ts_ssh_host_config_t *value);
esp_err_t ts_ssh_host_decode(const cJSON *json, ts_ssh_host_config_t *value);

#ifdef __cplusplus
}
#endif
