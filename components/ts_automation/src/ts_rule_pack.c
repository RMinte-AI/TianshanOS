#include "ts_rule_pack.h"
#include "ts_rule_codec.h"
#include "ts_crypto.h"
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

bool ts_rule_pack_definition_digest(const ts_auto_rule_t *rule, char out[65])
{
    cJSON *json = ts_rule_encode(rule);
    if (!json) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(json, "revision");
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    unsigned char digest[32];
    bool ok = text && ts_crypto_hash(TS_HASH_SHA256, text, strlen(text), digest, sizeof(digest)) == ESP_OK &&
        ts_crypto_hex_encode(digest, sizeof(digest), out, 65) == ESP_OK;
    free(text);
    return ok;
}

cJSON *ts_rule_pack_provenance(const ts_auto_rule_t *rule, const ts_config_pack_acceptance_t *accepted)
{
    char digest[65];
    if (!ts_rule_pack_definition_digest(rule, digest)) return NULL;
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    if (!cJSON_AddStringToObject(json, "package", accepted->package_sha256) ||
        !cJSON_AddStringToObject(json, "definition", digest) ||
        !cJSON_AddStringToObject(json, "signer", accepted->signer_sha256) ||
        !cJSON_AddStringToObject(json, "root", accepted->root_sha256) ||
        !cJSON_AddStringToObject(json, "recipient", accepted->recipient) ||
        !cJSON_AddNumberToObject(json, "policy", accepted->policy) ||
        !cJSON_AddNumberToObject(json, "accepted_at", accepted->accepted_at)) {
        cJSON_Delete(json);return NULL;
    }
    return json;
}

esp_err_t ts_rule_pack_dependencies(const ts_auto_document_t *document, const ts_auto_rule_t *rule,
                                    const char **reason)
{
    for (unsigned i = 0; i < rule->action_count; ++i) {
        const ts_auto_action_t *action = &rule->actions[i];
        if (action->template_id[0]) {
            action = NULL;
            for (int n = 0; n < document->action_count; ++n)
                if (!strcmp(document->actions[n].id, rule->actions[i].template_id)) { action = &document->actions[n].action;break; }
            if (!action) { *reason = "dependency_missing";return ESP_ERR_NOT_FOUND; }
        }
        /* Preserve the signed-rule import policy. Ordinary local definitions
         * continue to support their existing action types. */
        if (action->type == TS_AUTO_ACT_WEBHOOK) { *reason = "action_unsupported";return ESP_ERR_NOT_SUPPORTED; }
    }
    ts_auto_config_error_t error = {0};
    esp_err_t ret = ts_auto_document_validate(document, &error);
    *reason = ret == ESP_OK ? "ok" : error.reason;
    return ret;
}
