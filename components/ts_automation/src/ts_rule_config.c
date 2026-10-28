#include "ts_rule_engine.h"
#include "ts_rule_codec.h"
#include "ts_rule_pack.h"
#include "ts_automation_config.h"
#include "ts_cert.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const cJSON *find_rule(const cJSON *root, const char *id)
{
    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "rules"), *row;
    cJSON_ArrayForEach(row, rules) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(row, "id");
        if (cJSON_IsString(name) && !strcmp(name->valuestring, id)) return row;
    }
    return NULL;
}

static uint32_t revision(const cJSON *row)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, "revision");
    return cJSON_IsNumber(value) && value->valuedouble > 0 && value->valuedouble <= UINT32_MAX &&
        trunc(value->valuedouble) == value->valuedouble ? value->valuedouble : 0;
}

esp_err_t ts_rule_definition_get(const char *id,ts_auto_rule_t *out)
{
    if(!id||!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    const ts_auto_config_view_t *view=ts_auto_config_acquire();
    const cJSON *row=find_rule(ts_auto_config_json(view),id);
    esp_err_t ret=row?ts_rule_decode(row,out):ESP_ERR_NOT_FOUND;
    ts_auto_config_release(view);return ret;
}

bool ts_rule_edit_begin(void)
{
    ts_auto_config_status_t status;
    ts_auto_config_status(&status);
    return ts_auto_config_edit_begin(status.active_version, NULL) == ESP_OK;
}
void ts_rule_edit_end(void) { ts_auto_config_edit_end(); }

esp_err_t ts_rule_commit(const ts_auto_rule_t *input, const char *id, uint32_t expected,
                         ts_rule_commit_result_t *result)
{
    ts_rule_commit_result_t local = {0};
    if (!result) result = &local;
    *result = (ts_rule_commit_result_t){.error_code="invalid_argument"};
    if (!id || !ts_rule_id_valid(id) || (input && strcmp(input->id, id))) return ESP_ERR_INVALID_ARG;
    if (!ts_rule_edit_begin()) { result->error_code="configuration_unavailable";return ESP_ERR_INVALID_STATE; }
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const cJSON *old = find_rule(ts_auto_config_json(view), id);
    uint32_t current = revision(old);
    ts_auto_config_release(view);
    ts_auto_config_error_t error = {0};
    esp_err_t ret;
    if (expected != current || (old && !current)) {
        error.reason = "revision_conflict";ret = ESP_ERR_INVALID_STATE;
    } else if (input) {
        cJSON *json = ts_rule_encode(input);
        ret = json ? ts_auto_config_set(TS_AUTO_CONFIG_RULE, json, NULL, &error) : ESP_ERR_NO_MEM;
        cJSON_Delete(json);
    } else ret = ts_auto_config_delete(TS_AUTO_CONFIG_RULE, id, TS_AUTO_DELETE_NORMAL, NULL, false, NULL, NULL, &error);
    result->applied = result->durable = ret == ESP_OK ? 1 : ret == ESP_ERR_NOT_FINISHED ? -1 : 0;
    result->revision = ret == ESP_OK && input ? current + 1 : current;
    result->error_code = ret == ESP_OK ? "ok" : error.reason ? error.reason : ret == ESP_ERR_NO_MEM ? "out_of_memory" : "configuration_write_failed";
    if (error.collection && !strcmp(error.collection,"rules"))
        snprintf(result->missing_template_id,sizeof(result->missing_template_id),"%s",error.dependency);
    ts_rule_edit_end();
    return ret;
}

esp_err_t ts_rule_register(const ts_auto_rule_t *rule)
{
    return rule ? ts_rule_commit(rule, rule->id, 0, NULL) : ESP_ERR_INVALID_ARG;
}
esp_err_t ts_rule_unregister(const char *id)
{
    return ts_auto_config_delete(TS_AUTO_CONFIG_RULE, id, TS_AUTO_DELETE_NORMAL, NULL, false, NULL, NULL, NULL);
}

static esp_err_t set_enabled(const char *id, bool enabled)
{
    if (!id || !ts_rule_edit_begin()) return ESP_ERR_INVALID_STATE;
    const ts_auto_config_view_t *view = ts_auto_config_acquire();
    const cJSON *row = find_rule(ts_auto_config_json(view), id);
    cJSON *copy = row ? cJSON_Duplicate(row, true) : NULL;
    bool exists = row != NULL;
    ts_auto_config_release(view);
    esp_err_t ret = !exists ? ESP_ERR_NOT_FOUND : !copy ? ESP_ERR_NO_MEM : ESP_OK;
    if (ret == ESP_OK) {
        cJSON *value = cJSON_CreateBool(enabled);
        bool ok = value && (cJSON_HasObjectItem(copy,"enabled") ? cJSON_ReplaceItemInObjectCaseSensitive(copy,"enabled",value) :
                                                                   cJSON_AddItemToObject(copy,"enabled",value));
        if (!ok) { cJSON_Delete(value);ret=ESP_ERR_NO_MEM; }
        else ret = ts_auto_config_set(TS_AUTO_CONFIG_RULE, copy, NULL, NULL);
    }
    cJSON_Delete(copy);ts_rule_edit_end();return ret;
}
esp_err_t ts_rule_enable(const char *id) { return set_enabled(id,true); }
esp_err_t ts_rule_disable(const char *id) { return set_enabled(id,false); }

void ts_auto_config_rule_status(const ts_auto_config_view_t *view,const ts_auto_config_status_t *status,const char *id,cJSON *out)
{
    const char *fields[]={"saved_exists","saved_revision","active_revision","runtime_active","restart_required","readonly",
                          "pending_change","saved_source","saved_generation","package_digest","active_version"};
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i)cJSON_DeleteItemFromObjectCaseSensitive(out,fields[i]);
    const cJSON *root=ts_auto_config_json(view), *row=find_rule(root,id);
    const cJSON *package=cJSON_GetObjectItemCaseSensitive(row,"_package");
    cJSON_AddBoolToObject(out,"saved_exists",row!=NULL);
    cJSON_AddNumberToObject(out,"saved_revision",revision(row));
    cJSON_AddNumberToObject(out,"active_revision",revision(row));
    cJSON_AddBoolToObject(out,"runtime_active",row && !status->error.reason && ts_auto_config_definitions(view)!=NULL);
    cJSON_AddStringToObject(out,"saved_source",status->source==TS_AUTO_CONFIG_SD?"sd":"internal");
    const cJSON *generation=cJSON_GetObjectItemCaseSensitive(root,"revision");
    cJSON_AddNumberToObject(out,"saved_generation",cJSON_IsNumber(generation)?generation->valuedouble:0);
    const cJSON *digest=cJSON_GetObjectItemCaseSensitive(package,"package");
    if(cJSON_IsString(digest))cJSON_AddStringToObject(out,"package_digest",digest->valuestring);
    else cJSON_AddNullToObject(out,"package_digest");
    cJSON_AddStringToObject(out,"active_version",status->active_version);
}

esp_err_t ts_rule_import_pack(const char *bytes,size_t length,bool preview,bool overwrite,
    uint32_t expected_revision,uint32_t expected_generation,uint32_t expected_credential,
    const char *expected_digest,const char *expected_version,cJSON **data,const char **reason)
{
    if(!data||!reason)return ESP_ERR_INVALID_ARG;
    *data=NULL;*reason="invalid_pack";
    ts_config_pack_t *pack=NULL;ts_config_pack_acceptance_t accepted={0};uint32_t credential=0;
    /* Installed definitions never load their old package again. A new import
     * verifies against current credentials/trust; SD provenance grants no trust. */
    ts_config_pack_result_t got=ts_config_pack_load_verified_mem(bytes,length,NULL,&pack,&accepted,&credential);
    if(got!=TS_CONFIG_PACK_OK){*reason=ts_rule_pack_error(got);return ESP_ERR_INVALID_STATE;}
    if(!preview&&(credential!=expected_credential||!expected_digest||strcmp(expected_digest,accepted.package_sha256))){
        ts_config_pack_free(pack);*reason="credential_changed";return ESP_ERR_INVALID_STATE;
    }
    ts_auto_rule_t rule={0};esp_err_t ret=ts_rule_pack_decode(pack->content,pack->content_len,&rule);
    ts_config_pack_free(pack);
    if(ret!=ESP_OK){*reason="invalid_rule_pack";return ret;}
    ts_auto_config_status_t status;ts_auto_config_status(&status);
    ts_auto_config_error_t error={0};
    ret=ts_auto_config_edit_begin(preview?status.active_version:expected_version,&error);
    if(ret!=ESP_OK){*reason=error.reason;ts_rule_dispose(&rule);return ret;}
    ts_auto_config_status(&status);
    const ts_auto_config_view_t *view=ts_auto_config_acquire();
    const cJSON *root=ts_auto_config_json(view),*old=find_rule(root,rule.id);
    uint32_t current=revision(old),set_revision=revision(root);
    bool exists=old!=NULL;
    cJSON *candidate=cJSON_Duplicate(root,true),*previous=old?cJSON_Duplicate(old,true):NULL,
          *json=ts_rule_encode(&rule),*provenance=ts_rule_pack_provenance(&rule,&accepted);
    ts_auto_config_release(view);
    ts_auto_document_t *document=NULL;
    if(!candidate||!json||!provenance||(exists&&!previous)){ret=ESP_ERR_NO_MEM;*reason="out_of_memory";goto done;}
    cJSON *rules=cJSON_GetObjectItemCaseSensitive(candidate,"rules");
    if(!cJSON_IsArray(rules)){ret=ESP_ERR_INVALID_STATE;*reason="configuration_invalid";goto done;}
    int index=-1;
    for(int i=0;i<cJSON_GetArraySize(rules);++i){
        const cJSON *id=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rules,i),"id");
        if(cJSON_IsString(id)&&!strcmp(id->valuestring,rule.id)){index=i;break;}
    }
    cJSON *copy=cJSON_Duplicate(json,true);
    bool inserted=copy&&(index<0?cJSON_AddItemToArray(rules,copy):cJSON_ReplaceItemInArray(rules,index,copy));
    if(!inserted){cJSON_Delete(copy);ret=ESP_ERR_NO_MEM;*reason="out_of_memory";goto done;}
    ret=ts_auto_document_decode(candidate,&document,&error);
    if(ret!=ESP_OK){*reason=error.reason;goto done;}
    ret=ts_rule_pack_dependencies(document,&rule,reason);
    if(ret!=ESP_OK)goto done;
    for(int i=0;i<document->rule_count;++i){
        if(strcmp(document->rules[i].id,rule.id))continue;
        cJSON *normalized=ts_rule_encode(&document->rules[i]);
        cJSON *origin=ts_rule_pack_provenance(&document->rules[i],&accepted);
        if(!normalized||!origin){cJSON_Delete(normalized);cJSON_Delete(origin);ret=ESP_ERR_NO_MEM;*reason="out_of_memory";goto done;}
        cJSON_Delete(json);json=normalized;cJSON_Delete(provenance);provenance=origin;break;
    }
    const cJSON *old_package=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(previous,"_package"),"package");
    bool unchanged=cJSON_IsString(old_package)&&!strcmp(old_package->valuestring,accepted.package_sha256);
    cJSON_DeleteItemFromObjectCaseSensitive(previous,"_package");
    cJSON *old_revision=cJSON_GetObjectItemCaseSensitive(previous,"revision");
    const cJSON *new_revision=cJSON_GetObjectItemCaseSensitive(json,"revision");
    if(cJSON_IsNumber(old_revision)&&cJSON_IsNumber(new_revision))cJSON_SetNumberValue(old_revision,new_revision->valuedouble);
    else unchanged=false;
    unchanged=unchanged&&cJSON_Compare(previous,json,true);
    *data=cJSON_CreateObject();
    cJSON *reply_rule=cJSON_Duplicate(json,true);
    bool reply=*data&&reply_rule&&cJSON_AddItemToObject(*data,"rule",reply_rule);
    if(!reply){cJSON_Delete(reply_rule);ret=ESP_ERR_NO_MEM;*reason="out_of_memory";goto done;}
    reply=cJSON_AddStringToObject(*data,"id",rule.id)&&cJSON_AddStringToObject(*data,"name",rule.name)&&
        cJSON_AddStringToObject(*data,"type","automation_rule")&&cJSON_AddBoolToObject(*data,"valid",true)&&
        cJSON_AddBoolToObject(*data,"trusted",true)&&cJSON_AddBoolToObject(*data,"target_matches",true)&&
        cJSON_AddBoolToObject(*data,"official",true)&&cJSON_AddBoolToObject(*data,"exists",exists)&&
        cJSON_AddBoolToObject(*data,"unchanged",unchanged)&&
        cJSON_AddStringToObject(*data,"signer",accepted.signer_sha256)&&
        cJSON_AddStringToObject(*data,"package_digest",accepted.package_sha256)&&
        cJSON_AddNumberToObject(*data,"expected_revision",current)&&
        cJSON_AddNumberToObject(*data,"expected_generation",set_revision)&&
        cJSON_AddNumberToObject(*data,"credential_generation",credential)&&
        cJSON_AddStringToObject(*data,"active_version",status.active_version)&&
        cJSON_AddBoolToObject(*data,"imported",false)&&cJSON_AddBoolToObject(*data,"saved",false)&&
        cJSON_AddBoolToObject(*data,"durable",false)&&cJSON_AddBoolToObject(*data,"runtime_applied",false)&&
        cJSON_AddBoolToObject(*data,"restart_required",false)&&cJSON_AddNumberToObject(*data,"saved_revision",current+(unchanged?0:1));
    if(!reply){ret=ESP_ERR_NO_MEM;*reason="out_of_memory";goto done;}
    if(preview){*reason="ok";goto done;}
    if(current!=expected_revision||set_revision!=expected_generation){ret=ESP_ERR_INVALID_STATE;*reason="revision_conflict";goto done;}
    if(exists&&!overwrite&&!unchanged){ret=ESP_ERR_INVALID_STATE;*reason="overwrite_required";goto done;}
    ts_auto_document_free(document);document=NULL;cJSON_Delete(candidate);candidate=NULL;
    if(!ts_cert_material_begin(credential)){ret=ESP_ERR_INVALID_STATE;*reason="credential_changed";goto done;}
    ret=unchanged?ESP_OK:ts_auto_config_set_rule_from_pack(json,provenance,expected_version,&error);
    ts_cert_material_end();
    *reason=ret==ESP_OK?(unchanged?"no_change":"ok"):error.reason;
    if(ret==ESP_OK){
        ts_auto_config_status(&status);
        cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(*data,"imported"),true);
        cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(*data,"saved"),true);
        cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(*data,"durable"),true);
        cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(*data,"runtime_applied"),!status.error.reason);
        memcpy(cJSON_GetObjectItemCaseSensitive(*data,"active_version")->valuestring,status.active_version,33);
    }
done:
    ts_auto_document_free(document);cJSON_Delete(candidate);cJSON_Delete(previous);cJSON_Delete(json);cJSON_Delete(provenance);
    ts_rule_dispose(&rule);ts_auto_config_edit_end();
    if(ret!=ESP_OK&&*data){cJSON_Delete(*data);*data=NULL;}
    return ret;
}
