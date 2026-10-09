#include "ts_rule_pack.h"
#include "ts_rule_codec.h"
#include "ts_action_manager.h"
#include "ts_ssh_commands_config.h"
#include "ts_ssh_hosts_config.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

const char *ts_rule_pack_error(ts_config_pack_result_t error) {
    switch(error){
    case TS_CONFIG_PACK_ERR_TIME_UNVERIFIED:return "time_unverified";
    case TS_CONFIG_PACK_ERR_TRUST_NOT_CONFIGURED:return "signing_trust_unconfigured";
    case TS_CONFIG_PACK_ERR_CERT_CHAIN:return "signer_untrusted";
    case TS_CONFIG_PACK_ERR_SIGNER_ROLE:return "signer_unauthorized";
    case TS_CONFIG_PACK_ERR_RECIPIENT:return "wrong_device";
    case TS_CONFIG_PACK_ERR_SIGNATURE:return "signature_invalid";
    case TS_CONFIG_PACK_ERR_CREDENTIAL_CHANGED:return "credential_changed";
    case TS_CONFIG_PACK_ERR_NO_MEM:return "no_memory";
    default:return "invalid_pack";
    }
}

esp_err_t ts_rule_pack_decode(const char *text, size_t length, ts_auto_rule_t *rule) {
    cJSON *j = ts_config_pack_parse_json(text, length);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "type");
    esp_err_t ret = cJSON_IsString(type) && !strcmp(type->valuestring, "automation_rule")
        ? ts_rule_decode(cJSON_GetObjectItemCaseSensitive(j, "rule"), rule) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    return ret;
}
esp_err_t ts_rule_pack_dependencies(const ts_auto_rule_t *rule, const char **reason, cJSON **warnings) {
    *reason = "invalid_configuration";
    if (warnings) *warnings = NULL;
    bool disabled_warning=false, dynamic_warning=rule->conditions.count!=0;
    for (unsigned i = 0; i < rule->action_count; ++i) {
        const ts_auto_action_t *a = &rule->actions[i];
        dynamic_warning |= a->condition.has_condition;
        ts_action_template_t *tpl = NULL;
        if (a->template_id[0]) {
            if (!ts_action_templates_ready()) { *reason = "dependency_loading"; return ESP_ERR_INVALID_STATE; }
            tpl = heap_caps_calloc(1, sizeof(*tpl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!tpl) { *reason = "no_memory"; return ESP_ERR_NO_MEM; }
            esp_err_t got = ts_action_template_get(a->template_id, tpl);
            if (got != ESP_OK) { free(tpl); *reason = "dependency_missing"; return got; }
            if (rule->enabled && !tpl->enabled) { free(tpl); *reason = "dependency_disabled"; return ESP_ERR_INVALID_STATE; }
            disabled_warning |= !tpl->enabled;
            a = &tpl->action;
        }
        if (a->type == TS_AUTO_ACT_SSH_CMD_REF) {
            esp_err_t commands=ts_ssh_commands_config_load_state();
            if (commands!=ESP_OK) {
                free(tpl); *reason=commands==ESP_ERR_INVALID_STATE?"dependency_loading":"dependency_unavailable";
                return ESP_ERR_INVALID_STATE;
            }
            ts_ssh_command_config_t *cmd = heap_caps_calloc(1, sizeof(*cmd), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            ts_action_ssh_host_t host;
            if (!cmd) { free(tpl); *reason = "no_memory"; return ESP_ERR_NO_MEM; }
            esp_err_t got = ts_ssh_commands_config_get(a->ssh_ref.cmd_id, cmd);
            bool internal=false;
            if (got == ESP_OK) got = ts_action_get_ssh_host_ex(cmd->host_id, &host,&internal);
            esp_err_t hosts=ts_ssh_hosts_config_load_state();
            if (!internal && hosts!=ESP_OK) {
                free(cmd);free(tpl);*reason=hosts==ESP_ERR_INVALID_STATE?"dependency_loading":"dependency_unavailable";
                return ESP_ERR_INVALID_STATE;
            }
            if (got == ESP_OK && (!host.host[0] || !host.username[0] || !host.port)) got=ESP_ERR_INVALID_ARG;
            bool disabled = got == ESP_OK && !cmd->enabled;
            disabled_warning |= disabled;
            free(cmd); free(tpl);
            if (got != ESP_OK) { *reason = "dependency_missing"; return got; }
            if (rule->enabled && disabled) { *reason = "dependency_disabled"; return ESP_ERR_INVALID_STATE; }
        } else free(tpl);
    }
    /* Dynamic variables are not a static existence proof. Their values remain runtime inputs. */
    if (warnings && (dynamic_warning || disabled_warning)) {
        *warnings = cJSON_CreateArray();
        if (!*warnings || (dynamic_warning && !cJSON_AddItemToArray(*warnings,cJSON_CreateString("dynamic_inputs"))) ||
            (disabled_warning && !cJSON_AddItemToArray(*warnings,cJSON_CreateString("dependency_disabled")))) {
            cJSON_Delete(*warnings); *warnings = NULL; *reason = "no_memory"; return ESP_ERR_NO_MEM;
        }
    }
    *reason = "ok";
    return ESP_OK;
}
