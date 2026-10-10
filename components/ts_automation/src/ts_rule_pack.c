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
/* One proposed object overlays the read-only configuration view. The actual
 * executor/snapshot path is never called by static validation. */
typedef struct {
    ts_rule_dependency_t kind;
    const char *id;
    const void *next;
} dependency_view_t;
static bool view_targets(const dependency_view_t *view, ts_rule_dependency_t kind, const char *id) {
    return view && view->kind==kind && (!view->id || !strcmp(view->id,id));
}
static esp_err_t dependency_error(esp_err_t error, const char **reason) {
    *reason=error==ESP_ERR_NO_MEM?"no_memory":error==ESP_ERR_NOT_FOUND||error==ESP_ERR_INVALID_ARG?
        "dependency_missing":"dependency_unavailable";
    return error;
}
static esp_err_t dependency_ready(esp_err_t state,const char **reason) {
    if(state==ESP_OK)return ESP_OK;
    *reason=state==ESP_ERR_INVALID_STATE?"dependency_loading":"dependency_unavailable";
    return ESP_ERR_INVALID_STATE;
}
static esp_err_t read_command(const char *id,const dependency_view_t *view,
                              ts_ssh_command_config_t *out,const char **reason) {
    esp_err_t ret=dependency_ready(ts_ssh_commands_config_load_state(),reason);
    if(ret!=ESP_OK)return ret;
    if(view_targets(view,TS_RULE_DEP_COMMAND,id)) {
        if(!view->next)return dependency_error(ESP_ERR_NOT_FOUND,reason);
        *out=*(const ts_ssh_command_config_t *)view->next;
        return ESP_OK;
    }
    ret=ts_ssh_commands_config_get(id,out);
    return ret==ESP_OK?ret:dependency_error(ret,reason);
}
static esp_err_t read_config_host(const char *id,const dependency_view_t *view,
                                  ts_action_ssh_host_t *out,const char **reason) {
    esp_err_t ret=dependency_ready(ts_ssh_hosts_config_load_state(),reason);
    if(ret!=ESP_OK)return ret;
    ts_ssh_host_config_t *config=heap_caps_calloc(1,sizeof(*config),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!config)return dependency_error(ESP_ERR_NO_MEM,reason);
    if(view_targets(view,TS_RULE_DEP_HOST,id)) {
        if(view->next)*config=*(const ts_ssh_host_config_t *)view->next;
        else ret=ESP_ERR_NOT_FOUND;
    }else ret=ts_ssh_hosts_config_get(id,config);
    if(ret==ESP_OK){
        /* Static checks use only fields the selected runtime host requires. */
        memset(out,0,sizeof(*out));
        memcpy(out->host,config->host,sizeof(out->host));
        memcpy(out->username,config->username,sizeof(out->username));out->port=config->port;
    }
    free(config);return ret==ESP_OK?ret:dependency_error(ret,reason);
}
static esp_err_t read_host(const char *id,const dependency_view_t *view,
                           ts_action_ssh_host_t *out,const char **reason) {
    if(view_targets(view,TS_RULE_DEP_ACTION_HOST,id)) {
        if(view->next){*out=*(const ts_action_ssh_host_t *)view->next;return ESP_OK;}
        /* The removed internal host cannot satisfy its own fallback lookup. */
        return read_config_host(id,NULL,out,reason);
    }
    bool internal=false;
    esp_err_t ret=ts_action_get_ssh_host_ex(id,out,&internal);
    if(internal)return ret==ESP_OK?ret:dependency_error(ret,reason);
    if(view_targets(view,TS_RULE_DEP_HOST,id))return read_config_host(id,view,out,reason);
    esp_err_t ready=dependency_ready(ts_ssh_hosts_config_load_state(),reason);
    if(ready!=ESP_OK)return ready;
    return ret==ESP_OK?ret:dependency_error(ret,reason);
}
static esp_err_t validate_dependencies(const ts_auto_rule_t *rule,const dependency_view_t *view,
                                        const char **reason,cJSON **warnings) {
    *reason="invalid_configuration";if(warnings)*warnings=NULL;
    bool disabled_warning=false,dynamic_warning=rule->conditions.count!=0;
    for(unsigned i=0;i<rule->action_count;++i){
        const ts_auto_action_t *source=&rule->actions[i],*a=source;
        dynamic_warning|=source->condition.has_condition;
        bool template_changed=source->template_id[0]&&view_targets(view,TS_RULE_DEP_TEMPLATE,source->template_id);
        if(view&&view->kind==TS_RULE_DEP_TEMPLATE&&!template_changed)continue;
        ts_action_template_t *owned=NULL;const ts_action_template_t *tpl=NULL;
        ts_ssh_command_config_t *cmd=NULL;esp_err_t ret=ESP_OK;
        if(source->template_id[0]){
            if(template_changed){
                tpl=view->next;
                if(!tpl)return dependency_error(ESP_ERR_NOT_FOUND,reason);
            }else{
                ret=dependency_ready(ts_action_templates_load_state(),reason);if(ret!=ESP_OK)return ret;
                owned=heap_caps_calloc(1,sizeof(*owned),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
                if(!owned)return dependency_error(ESP_ERR_NO_MEM,reason);
                ret=ts_action_template_get(source->template_id,owned);
                if(ret!=ESP_OK){free(owned);return dependency_error(ret,reason);}tpl=owned;
            }
            a=&tpl->action;
        }
        /* Establish the effective reference before testing its proposed state.
         * Known-unaffected references do not freeze unrelated configuration. */
        if(view&&!template_changed){
            if(view->kind==TS_RULE_DEP_COMMAND){
                if(a->type!=TS_AUTO_ACT_SSH_CMD_REF||!view_targets(view,TS_RULE_DEP_COMMAND,a->ssh_ref.cmd_id)){
                    free(owned);continue;
                }
            }else if(view->kind==TS_RULE_DEP_HOST||view->kind==TS_RULE_DEP_ACTION_HOST){
                const char *host_id=NULL;
                if(a->type==TS_AUTO_ACT_SSH_CMD)host_id=a->ssh.host_ref;
                else if(a->type==TS_AUTO_ACT_SSH_CMD_REF){
                    cmd=heap_caps_calloc(1,sizeof(*cmd),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
                    if(!cmd){free(owned);return dependency_error(ESP_ERR_NO_MEM,reason);}
                    ret=read_command(a->ssh_ref.cmd_id,NULL,cmd,reason);
                    if(ret!=ESP_OK){free(cmd);free(owned);return ret;}host_id=cmd->host_id;
                }else {free(owned);continue;}
                if(view->id&&strcmp(view->id,host_id)){free(cmd);free(owned);continue;}
                ts_action_ssh_host_t actual;bool internal=false;
                ret=ts_action_get_ssh_host_ex(host_id,&actual,&internal);
                if(view->kind==TS_RULE_DEP_HOST&&ret==ESP_OK&&internal){free(cmd);free(owned);continue;}
                if(view->kind==TS_RULE_DEP_ACTION_HOST&&!view->next&&ret==ESP_OK&&!internal){free(cmd);free(owned);continue;}
            }
        }
        if(tpl&&rule->enabled&&!tpl->enabled){*reason="dependency_disabled";ret=ESP_ERR_INVALID_STATE;goto action_done;}
        if(tpl)disabled_warning|=!tpl->enabled;
        if(a->type==TS_AUTO_ACT_WEBHOOK){*reason="action_unsupported";ret=ESP_ERR_NOT_SUPPORTED;goto action_done;}
        if(a->type==TS_AUTO_ACT_SSH_CMD||a->type==TS_AUTO_ACT_SSH_CMD_REF){
            const char *host_id=a->ssh.host_ref;
            if(a->type==TS_AUTO_ACT_SSH_CMD_REF){
                if(!cmd){
                    cmd=heap_caps_calloc(1,sizeof(*cmd),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
                    if(!cmd){ret=dependency_error(ESP_ERR_NO_MEM,reason);goto action_done;}
                    ret=read_command(a->ssh_ref.cmd_id,view,cmd,reason);
                    if(ret!=ESP_OK)goto action_done;
                }
                if(rule->enabled&&!cmd->enabled){*reason="dependency_disabled";ret=ESP_ERR_INVALID_STATE;goto action_done;}
                disabled_warning|=!cmd->enabled;host_id=cmd->host_id;
            }
            ts_action_ssh_host_t host;
            ret=read_host(host_id,view,&host,reason);
            if(ret==ESP_OK&&(!host.host[0]||!host.username[0]||!host.port))ret=dependency_error(ESP_ERR_INVALID_ARG,reason);
        }
action_done:
        free(cmd);free(owned);if(ret!=ESP_OK)return ret;
    }
    if(warnings&&(dynamic_warning||disabled_warning)){
        *warnings=cJSON_CreateArray();
        if(!*warnings||(dynamic_warning&&!cJSON_AddItemToArray(*warnings,cJSON_CreateString("dynamic_inputs")))||
            (disabled_warning&&!cJSON_AddItemToArray(*warnings,cJSON_CreateString("dependency_disabled")))){
            cJSON_Delete(*warnings);*warnings=NULL;return dependency_error(ESP_ERR_NO_MEM,reason);
        }
    }
    *reason="ok";return ESP_OK;
}
esp_err_t ts_rule_pack_dependencies(const ts_auto_rule_t *rule,const char **reason,cJSON **warnings) {
    return validate_dependencies(rule,NULL,reason,warnings);
}
esp_err_t ts_rule_pack_dependency_change(const ts_auto_rule_t *rule,ts_rule_dependency_t kind,
                                        const char *id,const void *next,const char **reason) {
    dependency_view_t view={kind,id,next};
    return validate_dependencies(rule,&view,reason,NULL);
}
