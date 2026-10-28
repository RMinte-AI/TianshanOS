#include "ts_api_auto_config.h"
#include "ts_action_codec.h"
#include "ts_ssh_config_codec.h"
#include "ts_rule_codec.h"
#include "ts_source_manager.h"
#include "ts_variable.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static cJSON *config_reply_buffer(cJSON *object,const char *key,size_t capacity)
{
    char value[128];
    if(capacity>sizeof(value))return NULL;
    memset(value,' ',capacity-1);value[capacity-1]=0;
    return cJSON_AddStringToObject(object,key,value);
}
bool ts_api_auto_reply_prepare(ts_api_result_t *result)
{
    result->data=cJSON_CreateObject();result->message=malloc(96);
    cJSON *data=result->data;
    const char *flags[]={"loaded","configured","enabled","paused","uncertain","sd_available","internal_exists",
        "internal_valid","comparison_known","backup_matches","awaiting_sd_choice","saved"};
    bool ok=data&&result->message;
    for(unsigned i=0;ok&&i<sizeof(flags)/sizeof(flags[0]);++i)ok=cJSON_AddBoolToObject(data,flags[i],false)!=NULL;
    const char *strings[]={"source","active_version","load_error","internal_error","collection","id","dependency"};
    for(unsigned i=0;ok&&i<sizeof(strings)/sizeof(strings[0]);++i)ok=config_reply_buffer(data,strings[i],96)!=NULL;
    ok=ok&&cJSON_AddNumberToObject(data,"internal_saved_at",0);
    if(!ok){cJSON_Delete(data);free(result->message);result->data=NULL;result->message=NULL;result->code=TS_API_ERR_NO_MEM;}
    return ok;
}
static void config_reply_string(cJSON *data,const char *key,const char *text)
{
    snprintf(cJSON_GetObjectItemCaseSensitive(data,key)->valuestring,96,"%s",text?text:"");
}
void ts_api_auto_reply_finish(ts_api_result_t *result,esp_err_t ret,const ts_auto_config_error_t *error)
{
    ts_auto_config_status_t status;ts_auto_config_status(&status);cJSON *data=result->data;
#define CONFIG_BOOL(field) cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(data,#field),status.field)
    CONFIG_BOOL(loaded);CONFIG_BOOL(configured);CONFIG_BOOL(enabled);CONFIG_BOOL(paused);CONFIG_BOOL(uncertain);
    CONFIG_BOOL(sd_available);CONFIG_BOOL(internal_exists);CONFIG_BOOL(internal_valid);CONFIG_BOOL(comparison_known);
    CONFIG_BOOL(backup_matches);CONFIG_BOOL(awaiting_sd_choice);
#undef CONFIG_BOOL
    cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(data,"saved"),error&&error->saved);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(data,"internal_saved_at"),status.internal_saved_at);
    config_reply_string(data,"source",status.source==TS_AUTO_CONFIG_SD?"sd":"internal");
    config_reply_string(data,"active_version",status.active_version);
    config_reply_string(data,"load_error",status.uncertain?"save_outcome_unknown":status.error.reason);
    config_reply_string(data,"internal_error",status.internal_error);
    config_reply_string(data,"collection",error?error->collection:NULL);
    config_reply_string(data,"id",error?error->id:NULL);
    config_reply_string(data,"dependency",error?error->dependency:NULL);
    result->code=ret==ESP_OK?TS_API_OK:ret==ESP_ERR_NO_MEM?TS_API_ERR_NO_MEM:TS_API_ERR_INVALID_ARG;
    snprintf(result->message,96,"%s",ret==ESP_OK?"ok":error&&error->reason?error->reason:"configuration_unavailable");
}

static const char *text(const cJSON *json,const char *key)
{ return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json,key)); }
static const char *id_key(ts_auto_config_kind_t kind)
{ return kind==TS_AUTO_CONFIG_VARIABLE?"name":"id"; }
static const cJSON *find_row(const cJSON *document,ts_auto_config_kind_t kind,const char *id)
{
    const cJSON *rows=cJSON_GetObjectItemCaseSensitive(document,ts_auto_config_collection(kind)),*row;
    cJSON_ArrayForEach(row,rows){const char *name=text(row,id_key(kind));if(name&&id&&!strcmp(name,id))return row;}
    return NULL;
}
static bool put(cJSON *json,const char *key,cJSON *value)
{
    if(!value)return false;
    cJSON_DeleteItemFromObjectCaseSensitive(json,key);
    if(cJSON_AddItemToObject(json,key,value))return true;
    cJSON_Delete(value);return false;
}
static bool default_field(cJSON *json,const char *key,cJSON *value)
{
    if(cJSON_HasObjectItem(json,key)){cJSON_Delete(value);return true;}
    return put(json,key,value);
}
static bool edit_begin(const cJSON *params,ts_api_result_t *result,ts_auto_config_error_t *error)
{
    if(!ts_api_auto_reply_prepare(result))return false;
    esp_err_t ret=ts_auto_config_edit_begin(text(params,"active_version"),error);
    if(ret==ESP_OK)return true;
    ts_api_auto_reply_finish(result,ret,error);return false;
}
static void edit_finish(ts_api_result_t *result,esp_err_t ret,ts_auto_config_error_t *error)
{
    if(ret==ESP_OK)error->saved=true;
    ts_api_auto_reply_finish(result,ret,error);ts_auto_config_edit_end();
}
static esp_err_t invalid(ts_auto_config_error_t *error,const char *reason,esp_err_t ret)
{ error->reason=reason;return ret; }

/* Source API aliases/defaults are converted here; the owner stores one format. */
static esp_err_t source_defaults(cJSON *input)
{
    const char *type=text(input,"type");
    if(type&&!strcmp(type,"websocket")&&!cJSON_HasObjectItem(input,"uri")&&text(input,"url")&&
       !put(input,"uri",cJSON_CreateString(text(input,"url"))))return ESP_ERR_NO_MEM;
    if(type&&!strcmp(type,"socketio")&&!cJSON_HasObjectItem(input,"url")&&text(input,"uri")&&
       !put(input,"url",cJSON_CreateString(text(input,"uri"))))return ESP_ERR_NO_MEM;
    bool stream=type&&(!strcmp(type,"websocket")||!strcmp(type,"socketio"));
    const cJSON *numeric=cJSON_GetObjectItemCaseSensitive(input,"type");
    stream|=cJSON_IsNumber(numeric)&&(numeric->valueint==TS_AUTO_SRC_WEBSOCKET||numeric->valueint==TS_AUTO_SRC_SOCKETIO);
    if(!default_field(input,"enabled",cJSON_CreateBool(true))||
       !default_field(input,"auto_discover",cJSON_CreateBool(true))||
       !default_field(input,"poll_interval_ms",cJSON_CreateNumber(stream?0:1000))||
       !default_field(input,"reconnect_ms",cJSON_CreateNumber(5000))||
       !default_field(input,"method",cJSON_CreateString("GET")))return ESP_ERR_NO_MEM;
    if(cJSON_IsNumber(numeric)&&numeric->valueint==TS_AUTO_SRC_REST&&!cJSON_HasObjectItem(input,"rest_method")&&
       !put(input,"rest_method",cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(input,"method"),true)))return ESP_ERR_NO_MEM;
    /* The existing single-field selection creates the same explicit mapping. */
    const char *path=text(input,"json_path"),*id=text(input,"id");
    if(path&&path[0]&&id&&(!type||strcmp(type,"variable"))){
        cJSON *mappings=cJSON_GetObjectItemCaseSensitive(input,"mappings");
        if(!mappings){mappings=cJSON_AddArrayToObject(input,"mappings");if(!mappings)return ESP_ERR_NO_MEM;}
        if(!cJSON_IsArray(mappings))return ESP_ERR_INVALID_ARG;
        const cJSON *row;cJSON_ArrayForEach(row,mappings)if(text(row,"json_path")&&!strcmp(text(row,"json_path"),path))return ESP_OK;
        char name[TS_AUTO_NAME_MAX_LEN];int length=snprintf(name,sizeof(name),"%s.%s",id,path);
        if(length<0||(size_t)length>=sizeof(name))return ESP_ERR_INVALID_ARG;
        for(char *p=name;*p;++p)if(*p=='['||*p==']')*p='_';
        cJSON *mapping=cJSON_CreateObject();
        if(!mapping||!cJSON_AddStringToObject(mapping,"json_path",path)||!cJSON_AddStringToObject(mapping,"var_name",name)||
           !cJSON_AddItemToArray(mappings,mapping)){cJSON_Delete(mapping);return ESP_ERR_NO_MEM;}
    }
    return ESP_OK;
}
static cJSON *canonical(ts_auto_config_kind_t kind,const cJSON *params,esp_err_t *ret)
{
    cJSON *input=cJSON_Duplicate(params,true),*output=NULL;*ret=input?ESP_OK:ESP_ERR_NO_MEM;
    if(!input)return NULL;
    if(kind==TS_AUTO_CONFIG_SOURCE)*ret=source_defaults(input);
    if(kind==TS_AUTO_CONFIG_COMMAND&&!default_field(input,"icon",cJSON_CreateString("🚀")))*ret=ESP_ERR_NO_MEM;
    if(*ret==ESP_OK&&kind==TS_AUTO_CONFIG_VARIABLE){
        const cJSON *value=cJSON_GetObjectItemCaseSensitive(input,"value");
        int type=cJSON_IsBool(value)?TS_AUTO_VAL_BOOL:cJSON_IsString(value)?TS_AUTO_VAL_STRING:
            cJSON_IsNumber(value)?(isfinite(value->valuedouble)&&value->valuedouble>=INT32_MIN&&value->valuedouble<=INT32_MAX&&
                                  trunc(value->valuedouble)==value->valuedouble?TS_AUTO_VAL_INT:TS_AUTO_VAL_FLOAT):TS_AUTO_VAL_NULL;
        if(!value)*ret=ESP_ERR_INVALID_ARG;
        else if(!default_field(input,"value_type",cJSON_CreateNumber(type))||
                !default_field(input,"source_id",cJSON_CreateString("user")))*ret=ESP_ERR_NO_MEM;
    }
    if(*ret==ESP_OK&&(kind==TS_AUTO_CONFIG_COMMAND||kind==TS_AUTO_CONFIG_HOST)){
        if(kind==TS_AUTO_CONFIG_COMMAND){ts_ssh_command_config_t command;*ret=ts_ssh_command_decode(input,&command);if(*ret==ESP_OK)output=ts_ssh_command_encode(&command);}
        else {ts_ssh_host_config_t host;*ret=ts_ssh_host_decode(input,&host);if(*ret==ESP_OK)output=ts_ssh_host_encode(&host);}
    }else if(*ret==ESP_OK&&(kind==TS_AUTO_CONFIG_ACTION||kind==TS_AUTO_CONFIG_SOURCE)){
        char *json=cJSON_PrintUnformatted(input),*encoded=NULL;
        void *value=heap_caps_calloc(1,kind==TS_AUTO_CONFIG_ACTION?sizeof(ts_action_template_t):sizeof(ts_auto_source_t),
                                   MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        *ret=!json||!value?ESP_ERR_NO_MEM:kind==TS_AUTO_CONFIG_ACTION?ts_action_template_decode(json,value):ts_source_config_decode(json,value);
        if(*ret==ESP_OK)encoded=kind==TS_AUTO_CONFIG_ACTION?ts_action_template_encode(value):ts_source_config_encode(value);
        if(*ret==ESP_OK)output=encoded?cJSON_Parse(encoded):NULL;
        free(encoded);free(value);free(json);
    }else if(*ret==ESP_OK){output=input;input=NULL;}
    cJSON_Delete(input);if(*ret==ESP_OK&&!output)*ret=ESP_ERR_NO_MEM;
    return output;
}

esp_err_t ts_api_auto_write(ts_auto_config_kind_t kind,const cJSON *params,ts_api_auto_write_t mode,ts_api_result_t *result)
{
    ts_auto_config_error_t error={0};if(!edit_begin(params,result,&error))return ESP_OK;
    const char *id=text(params,id_key(kind));const ts_auto_config_view_t *view=ts_auto_config_acquire();
    const cJSON *old=find_row(ts_auto_config_json(view),kind,id);bool existed=old!=NULL;esp_err_t ret=ESP_OK;
    if(!id||!id[0])ret=invalid(&error,"invalid_request",ESP_ERR_INVALID_ARG);
    else if(kind==TS_AUTO_CONFIG_VARIABLE&&!cJSON_HasObjectItem(params,"value"))ret=invalid(&error,"invalid_request",ESP_ERR_INVALID_ARG);
    else if(mode==TS_API_AUTO_CREATE&&old)ret=invalid(&error,"object_exists",ESP_ERR_INVALID_STATE);
    else if(mode==TS_API_AUTO_UPDATE&&!old)ret=invalid(&error,"not_found",ESP_ERR_NOT_FOUND);
    const cJSON *expected=cJSON_GetObjectItemCaseSensitive(params,"expected_revision");
    if(ret==ESP_OK&&kind==TS_AUTO_CONFIG_RULE&&expected){
        const cJSON *revision=cJSON_GetObjectItemCaseSensitive(old,"revision");
        double current=cJSON_IsNumber(revision)?revision->valuedouble:0;
        if(!cJSON_IsNumber(expected)||expected->valuedouble!=current)ret=invalid(&error,"configuration_changed",ESP_ERR_INVALID_STATE);
    }
    if(ret==ESP_OK&&kind==TS_AUTO_CONFIG_VARIABLE&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,"create_only"))){
        ts_auto_value_t existing;
        if(ts_variable_get(id,&existing)==ESP_OK){
            bool response=cJSON_AddStringToObject(result->data,"name",id)&&cJSON_AddBoolToObject(result->data,"created",false);
            cJSON *current=ts_auto_value_encode(&existing);
            if(!response||!current||!cJSON_AddItemToObject(result->data,"value",current)){cJSON_Delete(current);ret=ESP_ERR_NO_MEM;error.reason="out_of_memory";}
            ts_auto_config_release(view);ts_api_auto_reply_finish(result,ret,&error);ts_auto_config_edit_end();return ESP_OK;
        }
    }
    cJSON *value=ret==ESP_OK?canonical(kind,params,&ret):NULL;
    if(ret==ESP_OK&&kind==TS_AUTO_CONFIG_RULE&&old){
        cJSON *merged=cJSON_Duplicate(old,true);
        if(!merged)ret=ESP_ERR_NO_MEM;
        const cJSON *field;
        cJSON_ArrayForEach(field,value){
            if(ret!=ESP_OK)break;
            if(!put(merged,field->string,cJSON_Duplicate(field,true)))ret=ESP_ERR_NO_MEM;
        }
        cJSON_Delete(value);value=merged;
    }
    if(ret==ESP_OK&&(kind==TS_AUTO_CONFIG_ACTION||kind==TS_AUTO_CONFIG_COMMAND||kind==TS_AUTO_CONFIG_HOST)){
        const char *key=kind==TS_AUTO_CONFIG_ACTION?"created_at":"created_time";
        const cJSON *created=cJSON_GetObjectItemCaseSensitive(old,key);
        double now=(double)time(NULL);if(now<0)now=0;
        if(!put(value,key,created?cJSON_Duplicate(created,true):cJSON_CreateNumber(kind==TS_AUTO_CONFIG_ACTION?now*1000:now)))ret=ESP_ERR_NO_MEM;
    }
    if(ret==ESP_OK&&kind==TS_AUTO_CONFIG_VARIABLE){
        cJSON *reported=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(value,"value"),true);
        bool metadata=cJSON_AddStringToObject(result->data,"name",id)&&cJSON_AddBoolToObject(result->data,"created",false);
        if(!metadata||!reported||!cJSON_AddItemToObject(result->data,"value",reported)){cJSON_Delete(reported);ret=ESP_ERR_NO_MEM;}
    }
    if(ret==ESP_OK&&kind==TS_AUTO_CONFIG_COMMAND&&!cJSON_AddStringToObject(result->data,"name",text(value,"name")?text(value,"name"):""))ret=ESP_ERR_NO_MEM;
    if(ret==ESP_OK&&kind==TS_AUTO_CONFIG_HOST&&!cJSON_AddBoolToObject(result->data,"added",false))ret=ESP_ERR_NO_MEM;
    ts_auto_config_release(view);
    if(ret==ESP_OK)ret=ts_auto_config_set(kind,value,NULL,&error);
    if(ret==ESP_OK&&kind==TS_AUTO_CONFIG_VARIABLE)cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(result->data,"created"),!existed);
    if(ret==ESP_OK&&kind==TS_AUTO_CONFIG_HOST)cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(result->data,"added"),true);
    if(ret!=ESP_OK&&!error.reason)error.reason=ret==ESP_ERR_NO_MEM?"out_of_memory":"configuration_invalid";
    if(id)snprintf(error.id,sizeof(error.id),"%s",id);
    cJSON_Delete(value);edit_finish(result,ret,&error);return ESP_OK;
}

esp_err_t ts_api_auto_delete(ts_auto_config_kind_t kind,const cJSON *params,ts_api_result_t *result)
{
    ts_auto_config_error_t error={0};if(!edit_begin(params,result,&error))return ESP_OK;
    const char *id=text(params,id_key(kind));cJSON *impact=NULL;esp_err_t ret;
    bool preview=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,"preview"));
    if(!id||!id[0])ret=invalid(&error,"invalid_request",ESP_ERR_INVALID_ARG);
    else if(preview)ret=ts_auto_config_delete_preview(kind,id,&impact,NULL,&error);
    else {
        const char *replacement=text(params,"replacement_id"),*mode=text(params,"mode");
        bool force=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,"force"));
        ts_auto_delete_mode_t action=force?TS_AUTO_DELETE_FORCE:replacement?TS_AUTO_DELETE_REPLACE:
            mode&&!strcmp(mode,"cascade")?TS_AUTO_DELETE_CASCADE:TS_AUTO_DELETE_NORMAL;
        ret=ts_auto_config_delete(kind,id,action,replacement,
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,"pause_automation")),NULL,&impact,&error);
    }
    if(ret==ESP_OK&&!preview)error.saved=true;
    ts_api_auto_reply_finish(result,ret,&error);
    if(impact)cJSON_AddItemToObjectCS(result->data,"impact",impact);
    ts_auto_config_edit_end();return ESP_OK;
}

static void alias(cJSON *object,const char *from,const char *to)
{
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(object,from);
    if(value)put(object,to,cJSON_Duplicate(value,true));
}
static cJSON *public_row(const ts_auto_config_view_t *view,ts_auto_config_kind_t kind,const cJSON *row,int index)
{
    cJSON *item=cJSON_Duplicate(row,true);if(!item)return NULL;
    const ts_auto_document_t *definitions=ts_auto_config_definitions(view);
    if(!definitions)cJSON_AddBoolToObject(item,"invalid",true);
    if(kind==TS_AUTO_CONFIG_COMMAND){
        const char *const fields[][2]={{"expect_pattern","expectPattern"},{"fail_pattern","failPattern"},
            {"extract_pattern","extractPattern"},{"var_name","varName"},{"timeout_sec","timeout"},
            {"stop_on_match","stopOnMatch"},{"service_mode","serviceMode"},{"ready_pattern","readyPattern"},
            {"service_fail_pattern","serviceFailPattern"},{"ready_timeout_sec","readyTimeout"},
            {"ready_check_interval_ms","readyInterval"},{"created_time","created"}};
        for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i)alias(item,fields[i][0],fields[i][1]);
        const char *host=text(row,"host_id");bool orphan=!host||!find_row(ts_auto_config_json(view),TS_AUTO_CONFIG_HOST,host);
        cJSON_AddBoolToObject(item,"orphan",orphan);
        cJSON_AddNumberToObject(item,"lastExec",definitions?ts_auto_config_command_time(view,index):0);
    }else if(kind==TS_AUTO_CONFIG_HOST){alias(item,"created_time","created");cJSON_AddNumberToObject(item,"last_used",0);}
    else if(kind==TS_AUTO_CONFIG_ACTION){
        ts_action_template_t stats={0};if(definitions)ts_auto_config_template_stats(view,index,&stats);
        cJSON_AddNumberToObject(item,"use_count",stats.use_count);cJSON_AddNumberToObject(item,"last_used_at",stats.last_used_at);
    }else if(kind==TS_AUTO_CONFIG_SOURCE){
        const cJSON *type=cJSON_GetObjectItemCaseSensitive(row,"type");
        const char *names[]={"builtin","websocket","socketio","rest","variable"};
        if(cJSON_IsNumber(type)&&type->valuedouble==type->valueint&&type->valueint>=0&&type->valueint<=TS_AUTO_SRC_VARIABLE)put(item,"type",cJSON_CreateString(names[type->valueint]));
        ts_auto_source_t runtime;const char *id=text(row,"id");bool current=definitions&&id&&ts_source_get_copy(id,&runtime)==ESP_OK&&runtime.instance==definitions->sources[index].instance;
        cJSON_AddBoolToObject(item,"connected",current&&runtime.connected);
        cJSON_AddNumberToObject(item,"last_update_ms",current?runtime.last_update_ms:0);
        if(current)cJSON_AddItemToObject(item,"last_value",ts_auto_value_encode(&runtime.last_value));
        cJSON_AddNumberToObject(item,"mapping_count",cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(row,"mappings")));
        bool stream=cJSON_IsNumber(type)&&(type->valueint==TS_AUTO_SRC_WEBSOCKET||type->valueint==TS_AUTO_SRC_SOCKETIO);
        cJSON_AddStringToObject(item,"update_mode",stream?"realtime":"polling");
    }
    return item;
}

esp_err_t ts_api_auto_list(ts_auto_config_kind_t kind,const cJSON *params,const char *key,ts_api_result_t *result)
{
    ts_auto_config_status_t status;const ts_auto_config_view_t *view=ts_auto_config_acquire_status(&status);
    const cJSON *rows=cJSON_GetObjectItemCaseSensitive(ts_auto_config_json(view),ts_auto_config_collection(kind));
    if(!cJSON_IsArray(rows)){ts_auto_config_release(view);ts_api_result_error(result,TS_API_ERR_INVALID_ARG,status.error.reason?status.error.reason:"configuration_unavailable");return ESP_OK;}
    cJSON *data=cJSON_CreateObject(),*items=cJSON_CreateArray();
    if(!data||!items){cJSON_Delete(data);cJSON_Delete(items);ts_auto_config_release(view);return ESP_ERR_NO_MEM;}
    cJSON_AddItemToObject(data,key,items);const char *host=text(params,"host_id");
    const cJSON *offset=cJSON_GetObjectItemCaseSensitive(params,"offset"),*limit=cJSON_GetObjectItemCaseSensitive(params,"limit");
    int skip=cJSON_IsNumber(offset)&&offset->valueint>0?offset->valueint:0;
    int maximum=cJSON_IsNumber(limit)&&limit->valueint>0?limit->valueint:INT32_MAX,total=0,index=0;
    const cJSON *row;cJSON_ArrayForEach(row,rows){
        int position=index++;
        if(host&&kind==TS_AUTO_CONFIG_COMMAND&&(!text(row,"host_id")||strcmp(host,text(row,"host_id"))))continue;
        if(total++<skip||cJSON_GetArraySize(items)>=maximum)continue;
        cJSON *item=public_row(view,kind,row,position);
        if(!item){cJSON_Delete(data);ts_auto_config_release(view);return ESP_ERR_NO_MEM;}
        cJSON_AddItemToArray(items,item);
    }
    cJSON_AddNumberToObject(data,"count",cJSON_GetArraySize(items));cJSON_AddNumberToObject(data,"total",total);
    cJSON_AddStringToObject(data,"active_version",status.active_version);
    cJSON_AddStringToObject(data,"load_error",status.error.reason?status.error.reason:"");
    cJSON_AddBoolToObject(data,"paused",status.paused);
    ts_auto_config_release(view);ts_api_result_ok(result,data);return ESP_OK;
}
esp_err_t ts_api_auto_get(ts_auto_config_kind_t kind,const cJSON *params,ts_api_result_t *result)
{
    ts_auto_config_status_t status;const ts_auto_config_view_t *view=ts_auto_config_acquire_status(&status);
    const char *id=text(params,id_key(kind));const cJSON *rows=cJSON_GetObjectItemCaseSensitive(ts_auto_config_json(view),ts_auto_config_collection(kind)),*row;
    cJSON *data=NULL;int index=0;
    cJSON_ArrayForEach(row,rows){const char *name=text(row,id_key(kind));if(name&&id&&!strcmp(name,id)){data=public_row(view,kind,row,index);break;}++index;}
    bool found=row!=NULL;ts_auto_config_release(view);
    if(!status.loaded)ts_api_result_error(result,TS_API_ERR_INTERNAL,status.error.reason?status.error.reason:"configuration_unavailable");
    else if(!found)ts_api_result_error(result,TS_API_ERR_NOT_FOUND,"not_found");
    else if(!data)return ESP_ERR_NO_MEM;
    else{cJSON_AddStringToObject(data,"active_version",status.active_version);ts_api_result_ok(result,data);}
    return ESP_OK;
}

esp_err_t ts_api_auto_enabled(ts_auto_config_kind_t kind,const cJSON *params,bool enabled,ts_api_result_t *result)
{
    ts_auto_config_error_t error={0};if(!edit_begin(params,result,&error))return ESP_OK;
    const ts_auto_config_view_t *view=ts_auto_config_acquire();const cJSON *row=find_row(ts_auto_config_json(view),kind,text(params,"id"));
    cJSON *copy=row?cJSON_Duplicate(row,true):NULL;esp_err_t ret=!row?ESP_ERR_NOT_FOUND:copy?ESP_OK:ESP_ERR_NO_MEM;
    const cJSON *expected=cJSON_GetObjectItemCaseSensitive(params,"expected_revision");
    if(ret==ESP_OK&&kind==TS_AUTO_CONFIG_RULE&&expected){
        const cJSON *revision=cJSON_GetObjectItemCaseSensitive(row,"revision");
        if(!cJSON_IsNumber(expected)||!revision||expected->valuedouble!=revision->valuedouble)ret=invalid(&error,"configuration_changed",ESP_ERR_INVALID_STATE);
    }
    ts_auto_config_release(view);
    if(ret==ESP_OK&&!put(copy,"enabled",cJSON_CreateBool(enabled)))ret=ESP_ERR_NO_MEM;
    if(ret==ESP_OK)ret=ts_auto_config_set(kind,copy,NULL,&error);
    if(ret!=ESP_OK&&!error.reason)error.reason=ret==ESP_ERR_NOT_FOUND?"not_found":"out_of_memory";
    cJSON_Delete(copy);edit_finish(result,ret,&error);return ESP_OK;
}
esp_err_t ts_api_auto_mapping(const cJSON *params,bool remove,ts_api_result_t *result)
{
    ts_auto_config_error_t error={0};if(!edit_begin(params,result,&error))return ESP_OK;
    const ts_auto_config_view_t *view=ts_auto_config_acquire();const cJSON *row=find_row(ts_auto_config_json(view),TS_AUTO_CONFIG_SOURCE,text(params,"id"));
    cJSON *copy=row?cJSON_Duplicate(row,true):NULL;esp_err_t ret=!row?ESP_ERR_NOT_FOUND:copy?ESP_OK:ESP_ERR_NO_MEM;
    ts_auto_config_release(view);
    cJSON *mappings=cJSON_GetObjectItemCaseSensitive(copy,"mappings");
    if(ret==ESP_OK&&!mappings)mappings=cJSON_AddArrayToObject(copy,"mappings");
    if(ret==ESP_OK&&!cJSON_IsArray(mappings))ret=ESP_ERR_INVALID_ARG;
    const char *path=text(params,"json_path"),*variable=text(params,"var_name");
    const cJSON *number=cJSON_GetObjectItemCaseSensitive(params,"index");int found=-1,index=0;const cJSON *mapping;
    cJSON_ArrayForEach(mapping,mappings){
        if((path&&text(mapping,"json_path")&&!strcmp(path,text(mapping,"json_path")))||
           (variable&&text(mapping,"var_name")&&!strcmp(variable,text(mapping,"var_name")))||
           (remove&&cJSON_IsNumber(number)&&number->valuedouble==index))found=index;
        ++index;
    }
    if(ret==ESP_OK&&remove){if(found<0)ret=ESP_ERR_NOT_FOUND;else cJSON_DeleteItemFromArray(mappings,found);}
    else if(ret==ESP_OK){
        if(!path||!variable)ret=ESP_ERR_INVALID_ARG;
        else if(found>=0)ret=ESP_ERR_INVALID_STATE;
        else {
            cJSON *value=cJSON_CreateObject();
            if(!value||!cJSON_AddStringToObject(value,"json_path",path)||!cJSON_AddStringToObject(value,"var_name",variable)||
               (text(params,"transform")&&!cJSON_AddStringToObject(value,"transform",text(params,"transform")))||
               !cJSON_AddItemToArray(mappings,value)){cJSON_Delete(value);ret=ESP_ERR_NO_MEM;}
        }
    }
    if(ret==ESP_OK){
        bool response=cJSON_AddStringToObject(result->data,"source_id",text(params,"id"))&&
            cJSON_AddNumberToObject(result->data,"mapping_count",cJSON_GetArraySize(mappings));
        if(!remove)response=response&&cJSON_AddNumberToObject(result->data,"mapping_index",cJSON_GetArraySize(mappings)-1);
        if(!response)ret=ESP_ERR_NO_MEM;
    }
    if(ret==ESP_OK)ret=ts_auto_config_set(TS_AUTO_CONFIG_SOURCE,copy,NULL,&error);
    if(ret!=ESP_OK&&!error.reason)error.reason=ret==ESP_ERR_NO_MEM?"out_of_memory":ret==ESP_ERR_NOT_FOUND?"not_found":"configuration_invalid";
    cJSON_Delete(copy);edit_finish(result,ret,&error);return ESP_OK;
}

/* Config packs are import inputs. They are never saved as a second live store. */
#include "ts_config_pack.h"
#include "ts_rule_pack.h"
#include "ts_cert.h"

static const cJSON *pack_object(ts_auto_config_kind_t kind,const cJSON *root)
{
    const char *type=text(root,"type");
    if(type&&kind==TS_AUTO_CONFIG_ACTION&&!strcmp(type,"action_template"))return cJSON_GetObjectItemCaseSensitive(root,"template");
    if(type&&kind==TS_AUTO_CONFIG_SOURCE&&!strcmp(type,"automation_source"))return cJSON_GetObjectItemCaseSensitive(root,"source");
    if(type&&kind==TS_AUTO_CONFIG_COMMAND&&!strcmp(type,"ssh_command"))return cJSON_GetObjectItemCaseSensitive(root,"command");
    return root;
}
static esp_err_t pack_item(cJSON *candidate,cJSON *changes,ts_auto_config_kind_t kind,const cJSON *input,
    bool *exists,bool *changed,ts_auto_config_error_t *error)
{
    esp_err_t ret=ESP_OK;cJSON *value=canonical(kind,input,&ret);
    if(ret!=ESP_OK)return invalid(error,ret==ESP_ERR_NO_MEM?"out_of_memory":"invalid_pack",ret);
    ret=ts_auto_document_check_item(kind,value,error);
    const char *id=text(value,id_key(kind));
    cJSON *rows=cJSON_GetObjectItemCaseSensitive(candidate,ts_auto_config_collection(kind));int index=-1;
    if(ret==ESP_OK&&!cJSON_IsArray(rows))ret=invalid(error,"configuration_invalid",ESP_ERR_INVALID_STATE);
    for(int i=0;ret==ESP_OK&&i<cJSON_GetArraySize(rows);++i){const char *name=text(cJSON_GetArrayItem(rows,i),id_key(kind));if(name&&!strcmp(name,id)){index=i;break;}}
    const cJSON *old=index<0?NULL:cJSON_GetArrayItem(rows,index);
    const char *created=kind==TS_AUTO_CONFIG_ACTION?"created_at":kind==TS_AUTO_CONFIG_COMMAND||kind==TS_AUTO_CONFIG_HOST?"created_time":NULL;
    const cJSON *stamp=created?cJSON_GetObjectItemCaseSensitive(old,created):NULL;
    if(ret==ESP_OK&&stamp&&!put(value,created,cJSON_Duplicate(stamp,true)))ret=ESP_ERR_NO_MEM;
    bool differs=index<0||!cJSON_Compare(old,value,true);
    if(ret==ESP_OK){
        cJSON *item=cJSON_CreateObject();
        bool ok=item&&cJSON_AddStringToObject(item,"collection",ts_auto_config_collection(kind))&&cJSON_AddStringToObject(item,"id",id)&&
            cJSON_AddBoolToObject(item,"exists",index>=0)&&cJSON_AddBoolToObject(item,"changed",differs)&&cJSON_AddItemToArray(changes,item);
        if(!ok){cJSON_Delete(item);ret=ESP_ERR_NO_MEM;}
    }
    if(ret==ESP_OK){
        bool inserted=index<0?cJSON_AddItemToArray(rows,value):cJSON_ReplaceItemInArray(rows,index,value);
        if(inserted){value=NULL;*exists|=index>=0;*changed|=differs;}else ret=ESP_ERR_NO_MEM;
    }
    cJSON_Delete(value);if(ret!=ESP_OK&&!error->reason)error->reason="out_of_memory";return ret;
}
esp_err_t ts_api_auto_pack_import(ts_auto_config_kind_t kind,const cJSON *params,ts_api_result_t *result)
{
    if(!ts_api_auto_reply_prepare(result))return ESP_OK;
    ts_auto_config_error_t error={0};esp_err_t ret=ESP_OK;
    const char *bytes=text(params,"tscfg");bool preview=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,"preview"));
    ts_config_pack_t *pack=NULL;ts_config_pack_acceptance_t accepted={0};uint32_t credential=0;
    cJSON *content=NULL,*candidate=NULL,*changes=NULL;bool editing=false,exists=false,changed=false;char imported_id[TS_AUTO_NAME_MAX_LEN]={0};
    if(!bytes||!bytes[0]){ret=invalid(&error,"invalid_request",ESP_ERR_INVALID_ARG);goto done;}
    ts_config_pack_result_t verified=ts_config_pack_load_verified_mem(bytes,strlen(bytes),NULL,&pack,&accepted,&credential);
    if(verified!=TS_CONFIG_PACK_OK){ret=invalid(&error,ts_rule_pack_error(verified),ESP_ERR_INVALID_STATE);goto done;}
    const cJSON *expected=cJSON_GetObjectItemCaseSensitive(params,"expected_credential");const char *digest=text(params,"expected_digest");
    if(!preview&&(!cJSON_IsNumber(expected)||expected->valuedouble!=credential||!digest||strcmp(digest,accepted.package_sha256))){
        ret=invalid(&error,"credential_changed",ESP_ERR_INVALID_STATE);goto done;
    }
    content=ts_config_pack_parse_json(pack->content,pack->content_len);
    if(!content){ret=invalid(&error,"invalid_pack",ESP_ERR_INVALID_ARG);goto done;}
    ts_auto_config_status_t status;ts_auto_config_status(&status);
    ret=ts_auto_config_edit_begin(preview&&!text(params,"active_version")?status.active_version:text(params,"active_version"),&error);
    if(ret!=ESP_OK)goto done;
    editing=true;
    const ts_auto_config_view_t *view=ts_auto_config_acquire();candidate=cJSON_Duplicate(ts_auto_config_json(view),true);ts_auto_config_release(view);
    changes=cJSON_AddArrayToObject(result->data,"items");
    if(!candidate||!changes){ret=invalid(&error,"out_of_memory",ESP_ERR_NO_MEM);goto done;}
    /* The existing command export may include one dependent host. */
    const cJSON *host=kind==TS_AUTO_CONFIG_COMMAND?cJSON_GetObjectItemCaseSensitive(content,"host"):NULL;
    if(cJSON_IsObject(host))ret=pack_item(candidate,changes,TS_AUTO_CONFIG_HOST,host,&exists,&changed,&error);
    const cJSON *object=pack_object(kind,content);
    if(ret==ESP_OK)ret=pack_item(candidate,changes,kind,object,&exists,&changed,&error);
    if(ret!=ESP_OK)goto done;
    const char *id=text(object,id_key(kind));if(id)snprintf(imported_id,sizeof(imported_id),"%s",id);
    ts_auto_document_t *document=NULL;ret=ts_auto_document_decode(candidate,&document,&error);
    if(ret==ESP_OK&&document->enabled)ret=ts_auto_document_validate(document,&error);
    ts_auto_document_free(document);
    if(ret!=ESP_OK)goto done;
    const char *type=kind==TS_AUTO_CONFIG_COMMAND?"ssh_command":kind==TS_AUTO_CONFIG_HOST?"ssh_host":
        kind==TS_AUTO_CONFIG_ACTION?"action_template":"automation_source";
    bool reply=cJSON_AddStringToObject(result->data,"type",type)&&cJSON_AddBoolToObject(result->data,"valid",true)&&
        cJSON_AddBoolToObject(result->data,"exists",exists)&&cJSON_AddBoolToObject(result->data,"unchanged",!changed)&&
        cJSON_AddStringToObject(result->data,"signer",pack->sig_info.signer_cn)&&cJSON_AddBoolToObject(result->data,"official",pack->sig_info.is_official)&&
        cJSON_AddStringToObject(result->data,"package_digest",accepted.package_sha256)&&cJSON_AddNumberToObject(result->data,"credential_generation",credential)&&
        cJSON_AddBoolToObject(result->data,"imported",false)&&cJSON_AddBoolToObject(result->data,"overwritten",false)&&
        cJSON_AddBoolToObject(result->data,"restart_required",false);
    if(!reply){ret=invalid(&error,"out_of_memory",ESP_ERR_NO_MEM);goto done;}
    if(preview)goto done;
    if(exists&&changed&&!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,"overwrite"))){ret=invalid(&error,"overwrite_required",ESP_ERR_INVALID_STATE);goto done;}
    if(!ts_cert_material_begin(credential)){ret=invalid(&error,"credential_changed",ESP_ERR_INVALID_STATE);goto done;}
    if(changed){cJSON *submitted=candidate;candidate=NULL;ret=ts_auto_config_import(submitted,text(params,"active_version"),&error);}
    else ret=ESP_OK;
    ts_cert_material_end();
    if(ret==ESP_OK){
        error.saved=changed;cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(result->data,"imported"),true);
        cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(result->data,"overwritten"),exists&&changed);
    }
done:
    if(ret==ESP_OK)snprintf(error.id,sizeof(error.id),"%s",imported_id);
    ts_api_auto_reply_finish(result,ret,&error);
    if(editing)ts_auto_config_edit_end();
    cJSON_Delete(candidate);cJSON_Delete(content);ts_config_pack_free(pack);return ESP_OK;
}
