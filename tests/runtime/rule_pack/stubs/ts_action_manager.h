#pragma once
#include "ts_automation_types.h"
typedef struct {
 char id[TS_AUTO_NAME_MAX_LEN], name[TS_AUTO_LABEL_MAX_LEN], description[64];
 ts_auto_action_t action; bool enabled,async;
 int64_t created_at,last_used_at; uint32_t use_count;
} ts_action_template_t;
esp_err_t ts_action_template_get(const char *, ts_action_template_t *);
bool ts_action_templates_ready(void);
esp_err_t ts_action_templates_load_state(void);
typedef struct {
 char id[TS_AUTO_NAME_MAX_LEN],host[64];uint16_t port;char username[32],password[64],key_path[96];bool use_key_auth;
} ts_action_ssh_host_t;
esp_err_t ts_action_get_ssh_host(const char *,ts_action_ssh_host_t *);
esp_err_t ts_action_get_ssh_host_ex(const char *,ts_action_ssh_host_t *,bool *);

/* Side-effect boundaries counted by the integration fixture; preflight must not use them. */
typedef struct {int status;} ts_action_result_t;
typedef void (*ts_action_callback_t)(const ts_auto_action_t *,const ts_action_result_t *,void *);
esp_err_t ts_action_queue(const ts_auto_action_t *,ts_action_callback_t,void *,uint8_t);
