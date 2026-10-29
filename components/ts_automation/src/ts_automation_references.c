#include "ts_automation_document.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static esp_err_t fail(ts_auto_config_error_t *error, const char *reason, esp_err_t code)
{
    if (error) error->reason = reason;
    return code;
}

static const char *string(const cJSON *object, const char *key, bool required, bool *known)
{
    if (!cJSON_IsObject(object)) { *known = false; return ""; }
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value && !required) return "";
    if (!cJSON_IsString(value)) { *known = false; return ""; }
    return value->valuestring;
}

static const cJSON *find(const cJSON *json, ts_auto_config_kind_t kind,
                         const char *id, bool *known)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(json, ts_auto_config_collection(kind));
    if (!cJSON_IsArray(array)) { *known = false; return NULL; }
    const cJSON *found = NULL, *item;
    cJSON_ArrayForEach(item, (cJSON_IsArray(array) ? array : NULL)) {
        const char *name = string(item, kind == TS_AUTO_CONFIG_VARIABLE ? "name" : "id", true, known);
        if (strcmp(name, id)) continue;
        if (found) *known = false;
        found = item;
    }
    return found;
}

static bool command_depends(const cJSON *command, ts_auto_config_kind_t kind,
                             const char *id, bool *known)
{
    return !strcmp(string(command, kind == TS_AUTO_CONFIG_HOST ? "host_id" : "id", true, known), id);
}

static bool set(cJSON *json, const char *name, cJSON *value)
{
    if (!value) return false;
    bool ok = cJSON_HasObjectItem(json, name) ?
        cJSON_ReplaceItemInObjectCaseSensitive(json, name, value) :
        cJSON_AddItemToObject(json, name, value);
    if (!ok) cJSON_Delete(value);
    return ok;
}

static bool row(cJSON *array, const cJSON *object)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
    if (!cJSON_IsString(id)) return true;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(object, "name");
    cJSON *item = cJSON_CreateObject();
    if (!item) return false;
    if (!cJSON_AddStringToObject(item, "id", id->valuestring) ||
        !cJSON_AddStringToObject(item, "name", cJSON_IsString(name) ? name->valuestring : id->valuestring) ||
        !cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static int targets(const cJSON *json, ts_auto_config_kind_t kind, const char *id, bool *known)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(json, ts_auto_config_collection(kind));
    if (!cJSON_IsArray(array)) { *known = false; return 0; }
    int count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, (cJSON_IsArray(array) ? array : NULL))
        count += !strcmp(string(item, kind == TS_AUTO_CONFIG_VARIABLE ? "name" : "id", true, known), id);
    if (count > 1) *known = false;
    return count;
}

/* The same plan drives preview, ordinary deletion, replacement and force.
 * Only real configuration fields participate; runtime values are not owners. */
typedef enum { KEEP, UPDATE, DISABLE, REMOVE } reference_effect_t;
typedef struct {
    const cJSON *json;
    ts_auto_config_kind_t kind;
    const char *id, *replacement;
    cJSON *plan;
    bool known;
} references_t;

static const char *effect_name(reference_effect_t effect)
{
    return effect == REMOVE ? "delete" : effect == DISABLE ? "disable" : "update";
}
static reference_effect_t planned(const references_t *r, const char *collection, const char *id)
{
    const cJSON *item, *rows = cJSON_GetObjectItemCaseSensitive(r->plan, collection);
    cJSON_ArrayForEach(item, rows) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
        const char *effect = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "effect"));
        if (name && !strcmp(name, id))
            return effect && !strcmp(effect, "delete") ? REMOVE : effect && !strcmp(effect, "disable") ? DISABLE : UPDATE;
    }
    return KEEP;
}
static bool plan_item(references_t *r, const char *collection, const cJSON *object,
                      reference_effect_t effect, bool *changed)
{
    const char *id = string(object, "id", true, &r->known);
    if (!id[0] || effect == KEEP || effect <= planned(r, collection, id)) return true;
    cJSON *rows = cJSON_GetObjectItemCaseSensitive(r->plan, collection), *item;
    cJSON_ArrayForEach(item, rows)
        if (!strcmp(string(item, "id", true, &r->known), id)) {
            *changed = true;
            return set(item, "effect", cJSON_CreateString(effect_name(effect)));
        }
    if (!row(rows, object)) return false;
    *changed = true;
    return cJSON_AddStringToObject(cJSON_GetArrayItem(rows, cJSON_GetArraySize(rows)-1), "effect", effect_name(effect)) != NULL;
}
static reference_effect_t maximum(reference_effect_t a, reference_effect_t b) { return a > b ? a : b; }
static bool target(references_t *r, ts_auto_config_kind_t kind, const cJSON *object)
{
    return r->kind == kind && !strcmp(string(object, kind == TS_AUTO_CONFIG_VARIABLE ? "name" : "id", true, &r->known), r->id);
}
static bool under(const char *name, const char *prefix)
{
    size_t n = strlen(prefix);
    return n && !strncmp(name, prefix, n) && name[n] == '.';
}
static const char *const observer_fields[] = {
    "status", "exit_code", "extracted", "expect_matched", "fail_matched", "host", "timestamp", "output"
};
static bool variable_name(references_t *r, const char *base, size_t length, const char *field,
                          char out[TS_AUTO_NAME_MAX_LEN])
{
    int size = snprintf(out, TS_AUTO_NAME_MAX_LEN, "%.*s.%s", (int)length, base, field);
    if (size < 0 || size >= TS_AUTO_NAME_MAX_LEN) { r->known = false; return false; }
    return true;
}
static bool source_input_name(references_t *r, const char *prefix, const char *field,
                              char out[TS_AUTO_NAME_MAX_LEN])
{
    size_t length=strlen(prefix);
    if(length&&prefix[length-1]=='.')--length;
    return variable_name(r,prefix,length,field,out);
}
static bool command_produces(references_t *r, const cJSON *command, const char *name)
{
    const char *prefix = string(command, "var_name", false, &r->known);
    if (!under(name, prefix)) return false;
    const char *suffix = name + strlen(prefix) + 1;
    const char *fields[] = {"status", "exit_code", "extracted", "expect_matched", "fail_matched", "host", "timestamp"};
    for (unsigned i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i)
        if (!strcmp(suffix, fields[i])) return true;
    return false;
}
static bool source_produces(references_t *r, const cJSON *source, const char *name)
{
    const char *id = string(source, "id", true, &r->known);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(source, "type");
    if (!cJSON_IsNumber(type)) { r->known = false; return false; }
    if (type->valueint == TS_AUTO_SRC_VARIABLE) {
        const char *prefix = string(source, "var_prefix", false, &r->known);
        size_t length = strlen(prefix);
        if (length && prefix[length-1] == '.') --length;
        /* A same-namespace observer does not supply the original data. */
        if (!length || (strlen(id) == length && !strncmp(id, prefix, length))) return false;
        if (!under(name, id)) return false;
        const char *suffix = name + strlen(id) + 1;
        for (unsigned i=0;i<sizeof(observer_fields)/sizeof(observer_fields[0]);++i)
            if (!strcmp(suffix, observer_fields[i])) return true;
        return false;
    }
    const cJSON *mapping, *mappings = cJSON_GetObjectItemCaseSensitive(source, "mappings");
    cJSON_ArrayForEach(mapping, mappings) {
        const char *variable = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(mapping, "var"));
        if (!variable) variable = string(mapping, "var_name", true, &r->known);
        if (!strcmp(name, variable) || under(name, variable)) return true;
    }
    return under(name, id) && (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(source, "auto_discover")) ||
        !cJSON_GetArraySize(mappings));
}
/* A variable source copies an input; it is not an independent data owner. */
static bool supplied(references_t *r, const char *name, bool after, bool replacement_inputs);

/* Only command replacement can relocate a whole group. A surviving independent
 * owner of the old group keeps observers on that original group. */
static const char *replacement_prefix(references_t *r, const char *prefix)
{
    if(r->kind!=TS_AUTO_CONFIG_COMMAND || !r->replacement)return NULL;
    const cJSON *old=find(r->json,TS_AUTO_CONFIG_COMMAND,r->id,&r->known);
    const char *before=string(old,"var_name",false,&r->known);
    size_t n=strlen(prefix);if(n&&prefix[n-1]=='.')--n;
    if(!n||strlen(before)!=n||strncmp(prefix,before,n))return NULL;
    char status[TS_AUTO_NAME_MAX_LEN];
    if(!source_input_name(r,prefix,"status",status)||supplied(r,status,true,false))return NULL;
    const cJSON *next=find(r->json,TS_AUTO_CONFIG_COMMAND,r->replacement,&r->known);
    const char *after=string(next,"var_name",false,&r->known);
    return after[0]?after:NULL;
}
static bool supplied(references_t *r, const char *name, bool after, bool replacement_inputs)
{
    const cJSON *sources=cJSON_GetObjectItemCaseSensitive(r->json,"sources");
    const char *current=name;
    char inputs[2][TS_AUTO_NAME_MAX_LEN];
    /* For a standard output field, unique source IDs identify at most one
     * copier. More hops than sources means a cycle, not another data owner.
     * Iteration also keeps stack usage constant on the controller. */
    for(int hop=0;hop<=cJSON_GetArraySize(sources);++hop) {
        if(after&&r->kind==TS_AUTO_CONFIG_VARIABLE&&!strcmp(current,r->id))return false;
        if(find(r->json,TS_AUTO_CONFIG_VARIABLE,current,&r->known))return true;
        const cJSON *object;
        cJSON_ArrayForEach(object,cJSON_GetObjectItemCaseSensitive(r->json,"commands")) {
            if(after&&(target(r,TS_AUTO_CONFIG_COMMAND,object)||
                planned(r,"commands",string(object,"id",true,&r->known))>=DISABLE))continue;
            if(command_produces(r,object,current))return true;
        }
        const cJSON *copier=NULL;
        cJSON_ArrayForEach(object,sources) {
            if(after&&(target(r,TS_AUTO_CONFIG_SOURCE,object)||
                planned(r,"sources",string(object,"id",true,&r->known))>=DISABLE))continue;
            if(!source_produces(r,object,current))continue;
            if(cJSON_GetObjectItemCaseSensitive(object,"type")->valueint!=TS_AUTO_SRC_VARIABLE)return true;
            if(copier){r->known=false;return false;}
            copier=object;
        }
        if(!copier)return false;
        const char *prefix=string(copier,"var_prefix",false,&r->known);
        const char *next=after&&replacement_inputs?replacement_prefix(r,prefix):NULL;
        const char *id=string(copier,"id",true,&r->known);
        const char *field=current+strlen(id)+1;
        bool named=next?variable_name(r,next,strlen(next),field,inputs[hop%2]):
                        source_input_name(r,prefix,field,inputs[hop%2]);
        if(!named)return false;
        current=inputs[hop%2];
    }
    return false;
}
/* Compare real ownership before and after the operation. Pre-existing unknown
 * inputs stay unknown; copies without a remaining root cannot keep a rule live. */
static reference_effect_t variable_change(references_t *r, const char *name, char replacement[TS_AUTO_NAME_MAX_LEN])
{
    replacement[0]=0;
    if(!name[0])return KEEP;
    if(r->kind==TS_AUTO_CONFIG_VARIABLE&&!strcmp(name,r->id)) {
        if(!r->replacement)return DISABLE;
        snprintf(replacement,TS_AUTO_NAME_MAX_LEN,"%s",r->replacement);return UPDATE;
    }
    if(supplied(r,name,true,true)||!supplied(r,name,false,false))return KEEP;
    if(r->kind==TS_AUTO_CONFIG_COMMAND&&r->replacement) {
        const cJSON *old=find(r->json,TS_AUTO_CONFIG_COMMAND,r->id,&r->known);
        const cJSON *next=find(r->json,TS_AUTO_CONFIG_COMMAND,r->replacement,&r->known);
        const char *before=string(old,"var_name",false,&r->known),*after=string(next,"var_name",false,&r->known);
        if(command_produces(r,old,name)&&after[0]&&
           variable_name(r,after,strlen(after),name+strlen(before)+1,replacement))return UPDATE;
    }
    return DISABLE;
}
static reference_effect_t variable_field(references_t *r, const cJSON *object, const char *key)
{
    char replacement[TS_AUTO_NAME_MAX_LEN];
    return variable_change(r, string(object, key, false, &r->known), replacement);
}
static reference_effect_t expanded_field(references_t *r, const cJSON *object, const char *key)
{
    const char *text = string(object, key, false, &r->known);
    reference_effect_t effect = KEEP;
    for (const char *p = text; (p = strstr(p, "${")); ) {
        const char *end = strchr(p+2, '}'); if (!end) break;
        size_t size = end-p-2;
        if (size < TS_AUTO_NAME_MAX_LEN) {
            char name[TS_AUTO_NAME_MAX_LEN], replacement[TS_AUTO_NAME_MAX_LEN];
            memcpy(name,p+2,size);name[size]=0;
            effect = maximum(effect,variable_change(r,name,replacement));
        }
        p=end+1;
    }
    return effect;
}
static reference_effect_t source_effect(references_t *r, const cJSON *source)
{
    reference_effect_t effect = KEEP;
    if (r->kind == TS_AUTO_CONFIG_HOST && !strcmp(string(source,"ssh_host_id",false,&r->known),r->id))
        effect = r->replacement ? UPDATE : DISABLE;
    /* Mapping destinations write variables just as set-variable actions do. */
    const cJSON *mapping;
    cJSON_ArrayForEach(mapping,cJSON_GetObjectItemCaseSensitive(source,"mappings"))
        effect=maximum(effect,variable_field(r,mapping,cJSON_HasObjectItem(mapping,"var")?"var":"var_name"));
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(source,"type");
    if (!cJSON_IsNumber(type)) {r->known=false;return effect;}
    if (type->valueint != TS_AUTO_SRC_VARIABLE) return effect;
    const char *prefix=string(source,"var_prefix",false,&r->known);
    if(prefix[0]) {
        for(unsigned i=0;i<sizeof(observer_fields)/sizeof(observer_fields[0]);++i) {
            char name[TS_AUTO_NAME_MAX_LEN],replacement[TS_AUTO_NAME_MAX_LEN];
            if(!source_input_name(r,prefix,observer_fields[i],name))continue;
            reference_effect_t change=variable_change(r,name,replacement);
            /* A single replacement cannot rewrite one field of a group reader. */
            if(change==UPDATE&&r->kind==TS_AUTO_CONFIG_VARIABLE)change=DISABLE;
            effect=maximum(effect,change);
        }
    } else effect=maximum(effect,variable_field(r,source,"var_name"));
    return effect;
}
static reference_effect_t command_effect(references_t *r, const cJSON *command)
{
    reference_effect_t effect=expanded_field(r,command,"command");
    if(r->kind==TS_AUTO_CONFIG_HOST && command_depends(command,r->kind,r->id,&r->known))
        effect=maximum(effect,r->replacement?UPDATE:DISABLE);
    return effect;
}
static reference_effect_t action_effect(references_t *r, const cJSON *action, bool template)
{
    reference_effect_t effect=KEEP;
    const char *ref=string(action,"template_id",false,&r->known);
    if(template&&ref[0]) {r->known=false;return KEEP;} /* Templates contain actions, never template references. */
    if(ref[0]) {
        if(r->kind==TS_AUTO_CONFIG_ACTION&&!strcmp(ref,r->id))effect=r->replacement?UPDATE:REMOVE;
        effect=maximum(effect,planned(r,"actions",ref));
        if(!r->plan) {
            const cJSON *definition=find(r->json,TS_AUTO_CONFIG_ACTION,ref,&r->known);
            if(definition)effect=maximum(effect,action_effect(r,definition,true));else r->known=false;
        }
    } else {
        const char *type=string(action,"type",true,&r->known);
        const cJSON *p=template?cJSON_GetObjectItemCaseSensitive(action,!strcmp(type,"ssh_cmd_ref")?"ssh_ref":!strcmp(type,"ssh_cmd")?"ssh":type):action;
        if(!strcmp(type,"ssh_cmd_ref")) {
            const char *cmd=string(p,"cmd_id",true,&r->known);
            if(r->kind==TS_AUTO_CONFIG_COMMAND&&!strcmp(cmd,r->id))effect=r->replacement?UPDATE:REMOVE;
            effect=maximum(effect,planned(r,"commands",cmd));
            if(!r->plan) {
                const cJSON *definition=find(r->json,TS_AUTO_CONFIG_COMMAND,cmd,&r->known);
                if(definition)effect=maximum(effect,command_effect(r,definition));else r->known=false;
            }
        } else if(!strcmp(type,"ssh")||!strcmp(type,"ssh_cmd")) {
            if(r->kind==TS_AUTO_CONFIG_HOST&&!strcmp(string(p,"host_ref",true,&r->known),r->id))effect=r->replacement?UPDATE:DISABLE;
            effect=maximum(effect,expanded_field(r,p,"command"));
        } else if(!strcmp(type,"set_var")) effect=variable_field(r,p,"variable");
        else if(!strcmp(type,"log")) effect=expanded_field(r,p,"message");
        else if(!strcmp(type,"led")) {
            const char *fields[]={"text","image_path","qr_text","filter","effect"};
            for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i)effect=maximum(effect,expanded_field(r,p,fields[i]));
        } else if(strcmp(type,"cli")&&strcmp(type,"gpio")&&strcmp(type,"webhook")&&strcmp(type,"device_ctrl")&&strcmp(type,"device"))r->known=false;
    }
    const cJSON *condition=cJSON_GetObjectItemCaseSensitive(action,"condition");
    if(cJSON_IsObject(condition))effect=maximum(effect,variable_field(r,condition,"variable"));
    return effect;
}
static reference_effect_t rule_effect(references_t *r, const cJSON *rule)
{
    reference_effect_t effect=KEEP;
    const cJSON *conditions=cJSON_GetObjectItemCaseSensitive(rule,"conditions"),*item;
    if(cJSON_IsObject(conditions))conditions=cJSON_GetObjectItemCaseSensitive(conditions,"items");
    if(conditions&&!cJSON_IsArray(conditions))r->known=false;
    cJSON_ArrayForEach(item,(cJSON_IsArray(conditions)?conditions:NULL))
        effect=maximum(effect,variable_field(r,item,"variable"));
    const cJSON *steps=cJSON_GetObjectItemCaseSensitive(rule,"actions");
    if(!cJSON_IsArray(steps))r->known=false;
    cJSON_ArrayForEach(item,(cJSON_IsArray(steps)?steps:NULL))effect=maximum(effect,action_effect(r,item,false));
    return effect>=DISABLE?DISABLE:effect;
}
static esp_err_t make_plan(references_t *r,cJSON **out,ts_auto_config_error_t *error)
{
    *out=NULL;
    if(error) {
        memset(error,0,sizeof(*error));error->collection=ts_auto_config_collection(r->kind);
        snprintf(error->id,sizeof(error->id),"%s",r->id);
    }
    int count=targets(r->json,r->kind,r->id,&r->known);
    if(!count)return fail(error,"not_found",ESP_ERR_NOT_FOUND);
    r->plan=cJSON_CreateObject();if(!r->plan)return ESP_ERR_NO_MEM;
    const char *collections[]={"rules","actions","commands","sources"};
    for(unsigned i=0;i<4;++i) {
        if(!cJSON_AddArrayToObject(r->plan,collections[i]))goto oom;
        if(!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(r->json,collections[i])))r->known=false;
    }
    if(!cJSON_AddNumberToObject(r->plan,"target_count",count))goto oom;
    /* Follow the finite command/source data flow until no more inputs are lost. */
    bool changed;
    do {
        changed=false;const cJSON *object;
        cJSON_ArrayForEach(object,cJSON_GetObjectItemCaseSensitive(r->json,"commands"))
            if(!target(r,TS_AUTO_CONFIG_COMMAND,object)&&!plan_item(r,"commands",object,command_effect(r,object),&changed))goto oom;
        cJSON_ArrayForEach(object,cJSON_GetObjectItemCaseSensitive(r->json,"sources"))
            if(!target(r,TS_AUTO_CONFIG_SOURCE,object)&&!plan_item(r,"sources",object,source_effect(r,object),&changed))goto oom;
    } while(changed);
    const cJSON *object;
    cJSON_ArrayForEach(object,cJSON_GetObjectItemCaseSensitive(r->json,"actions"))
        if(!target(r,TS_AUTO_CONFIG_ACTION,object)&&!plan_item(r,"actions",object,action_effect(r,object,true),&changed))goto oom;
    cJSON_ArrayForEach(object,cJSON_GetObjectItemCaseSensitive(r->json,"rules"))
        if(!target(r,TS_AUTO_CONFIG_RULE,object)&&!plan_item(r,"rules",object,rule_effect(r,object),&changed))goto oom;
    if(!cJSON_AddBoolToObject(r->plan,"references_confirmed",r->known)||
       !cJSON_AddBoolToObject(r->plan,"requires_pause",!r->known))goto oom;
    *out=r->plan;return ESP_OK;
oom:
    cJSON_Delete(r->plan);r->plan=NULL;return fail(error,"out_of_memory",ESP_ERR_NO_MEM);
}

esp_err_t ts_auto_document_impact(const cJSON *json,ts_auto_config_kind_t kind,const char *id,
                                 cJSON **impact,ts_auto_config_error_t *error)
{
    if(!impact||!cJSON_IsObject(json)||(unsigned)kind>=TS_AUTO_CONFIG_KIND_COUNT||!id||!id[0])return ESP_ERR_INVALID_ARG;
    references_t r={.json=json,.kind=kind,.id=id,.known=true};
    return make_plan(&r,impact,error);
}

static bool rename_variable(references_t *r,cJSON *object,const char *key)
{
    char next[TS_AUTO_NAME_MAX_LEN];
    return variable_change(r,string(object,key,false,&r->known),next)!=UPDATE || set(object,key,cJSON_CreateString(next));
}
static bool rewrite_expanded(references_t *r,cJSON *object,const char *key)
{
    const char *value=string(object,key,false,&r->known);
    if(expanded_field(r,object,key)==KEEP)return true;
    size_t capacity=strlen(value)+1;
    for(const char *p=value;(p=strstr(p,"${"));p+=2)capacity+=TS_AUTO_NAME_MAX_LEN;
    char *buffer=malloc(capacity);if(!buffer)return false;
    size_t used=0;
    for(const char *p=value;*p;) {
        const char *end=p[0]=='$'&&p[1]=='{'?strchr(p+2,'}'):NULL;
        if(end&&(size_t)(end-p-2)<TS_AUTO_NAME_MAX_LEN) {
            char name[TS_AUTO_NAME_MAX_LEN],next[TS_AUTO_NAME_MAX_LEN];size_t n=end-p-2;
            memcpy(name,p+2,n);name[n]=0;
            if(variable_change(r,name,next)==UPDATE) {used+=(size_t)snprintf(buffer+used,capacity-used,"${%s}",next);p=end+1;continue;}
        }
        buffer[used++]=*p++;
    }
    buffer[used]=0;bool ok=set(object,key,cJSON_CreateString(buffer));free(buffer);return ok;
}
static bool rewrite_action(references_t *r,cJSON *action,bool template)
{
    const char *ref=string(action,"template_id",false,&r->known);
    if(ref[0]) {
        if(r->kind==TS_AUTO_CONFIG_ACTION&&!strcmp(ref,r->id)&&r->replacement&&!set(action,"template_id",cJSON_CreateString(r->replacement)))return false;
    } else {
        const char *type=string(action,"type",true,&r->known);
        cJSON *p=template?cJSON_GetObjectItemCaseSensitive(action,!strcmp(type,"ssh_cmd_ref")?"ssh_ref":!strcmp(type,"ssh_cmd")?"ssh":type):action;
        if(!strcmp(type,"ssh_cmd_ref")) {
            if(r->kind==TS_AUTO_CONFIG_COMMAND&&!strcmp(string(p,"cmd_id",true,&r->known),r->id)&&r->replacement&&!set(p,"cmd_id",cJSON_CreateString(r->replacement)))return false;
        } else if(!strcmp(type,"ssh")||!strcmp(type,"ssh_cmd")) {
            if(r->kind==TS_AUTO_CONFIG_HOST&&!strcmp(string(p,"host_ref",true,&r->known),r->id)&&!set(p,"host_ref",cJSON_CreateString(r->replacement?r->replacement:"")))return false;
            if(!rewrite_expanded(r,p,"command"))return false;
        } else if(!strcmp(type,"set_var")){if(!rename_variable(r,p,"variable"))return false;}
        else if(!strcmp(type,"log")){if(!rewrite_expanded(r,p,"message"))return false;}
        else if(!strcmp(type,"led")) {
            const char *fields[]={"text","image_path","qr_text","filter","effect"};
            for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i)if(!rewrite_expanded(r,p,fields[i]))return false;
        }
    }
    cJSON *condition=cJSON_GetObjectItemCaseSensitive(action,"condition");
    return !cJSON_IsObject(condition)||rename_variable(r,condition,"variable");
}
static esp_err_t apply_plan(cJSON *json,references_t *r)
{
    cJSON *object,*rows=cJSON_GetObjectItemCaseSensitive(json,"rules");
    cJSON_ArrayForEach(object,(cJSON_IsArray(rows)?rows:NULL)) {
        reference_effect_t effect=r->plan?planned(r,"rules",string(object,"id",true,&r->known)):rule_effect(r,object);
        if(effect==KEEP)continue;
        cJSON_DeleteItemFromObjectCaseSensitive(object,"_package");
        if(effect>=DISABLE&&!set(object,"enabled",cJSON_CreateFalse()))return ESP_ERR_NO_MEM;
        cJSON *conditions=cJSON_GetObjectItemCaseSensitive(object,"conditions"),*condition;
        if(cJSON_IsObject(conditions))conditions=cJSON_GetObjectItemCaseSensitive(conditions,"items");
        cJSON_ArrayForEach(condition,(cJSON_IsArray(conditions)?conditions:NULL))if(!rename_variable(r,condition,"variable"))return ESP_ERR_NO_MEM;
        cJSON *steps=cJSON_GetObjectItemCaseSensitive(object,"actions");
        for(int i=cJSON_GetArraySize(steps)-1;cJSON_IsArray(steps)&&i>=0;--i) {
            cJSON *step=cJSON_GetArrayItem(steps,i);
            if(action_effect(r,step,false)==REMOVE)cJSON_DeleteItemFromArray(steps,i);
            else if(!rewrite_action(r,step,false))return ESP_ERR_NO_MEM;
        }
    }
    rows=cJSON_GetObjectItemCaseSensitive(json,"actions");
    for(int i=cJSON_GetArraySize(rows)-1;cJSON_IsArray(rows)&&i>=0;--i) {
        object=cJSON_GetArrayItem(rows,i);
        reference_effect_t effect=r->plan?planned(r,"actions",string(object,"id",true,&r->known)):action_effect(r,object,true);
        if(effect==REMOVE)cJSON_DeleteItemFromArray(rows,i);
        else if(effect!=KEEP) {
            if(effect==DISABLE&&!set(object,"enabled",cJSON_CreateFalse()))return ESP_ERR_NO_MEM;
            if(!rewrite_action(r,object,true))return ESP_ERR_NO_MEM;
        }
    }
    rows=cJSON_GetObjectItemCaseSensitive(json,"sources");
    cJSON_ArrayForEach(object,(cJSON_IsArray(rows)?rows:NULL)) {
        reference_effect_t effect=r->plan?planned(r,"sources",string(object,"id",true,&r->known)):source_effect(r,object);
        if(effect==KEEP)continue;
        if(effect==DISABLE&&!set(object,"enabled",cJSON_CreateFalse()))return ESP_ERR_NO_MEM;
        if(r->kind==TS_AUTO_CONFIG_HOST&&!strcmp(string(object,"ssh_host_id",false,&r->known),r->id)&&!set(object,"ssh_host_id",cJSON_CreateString(r->replacement?r->replacement:"")))return ESP_ERR_NO_MEM;
        if(!rename_variable(r,object,"var_name"))return ESP_ERR_NO_MEM;
        cJSON *mapping;
        cJSON_ArrayForEach(mapping,cJSON_GetObjectItemCaseSensitive(object,"mappings"))
            if(!rename_variable(r,mapping,cJSON_HasObjectItem(mapping,"var")?"var":"var_name"))return ESP_ERR_NO_MEM;
        const char *prefix=string(object,"var_prefix",false,&r->known);
        const char *next=replacement_prefix(r,prefix);
        if(next) {
            char group[TS_AUTO_NAME_MAX_LEN];
            if(!variable_name(r,next,strlen(next),"",group)||!set(object,"var_prefix",cJSON_CreateString(group)))return ESP_ERR_NO_MEM;
            if(string(object,"ssh_host_id",false,&r->known)[0]) {
                const cJSON *command=find(r->json,TS_AUTO_CONFIG_COMMAND,r->replacement,&r->known);
                if(!set(object,"ssh_host_id",cJSON_CreateString(string(command,"host_id",true,&r->known))))return ESP_ERR_NO_MEM;
            }
        }
    }
    rows=cJSON_GetObjectItemCaseSensitive(json,"commands");
    cJSON_ArrayForEach(object,(cJSON_IsArray(rows)?rows:NULL)) {
        reference_effect_t effect=r->plan?planned(r,"commands",string(object,"id",true,&r->known)):command_effect(r,object);
        if(effect==KEEP)continue;
        if(effect==DISABLE&&!set(object,"enabled",cJSON_CreateFalse()))return ESP_ERR_NO_MEM;
        if(!rewrite_expanded(r,object,"command"))return ESP_ERR_NO_MEM;
        if(r->kind==TS_AUTO_CONFIG_HOST&&command_depends(object,r->kind,r->id,&r->known)&&!set(object,"host_id",cJSON_CreateString(r->replacement?r->replacement:"")))return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ts_auto_document_remove(cJSON *json,ts_auto_config_kind_t kind,const char *id,
    ts_auto_delete_mode_t mode,const char *replacement,bool pause_confirmed,cJSON **impact,ts_auto_config_error_t *error)
{
    if(impact)*impact=NULL;
    if(!cJSON_IsObject(json)||!id||!id[0]||(unsigned)kind>=TS_AUTO_CONFIG_KIND_COUNT||(unsigned)mode>TS_AUTO_DELETE_FORCE)return ESP_ERR_INVALID_ARG;
    references_t r={.json=json,.kind=kind,.id=id,.replacement=mode==TS_AUTO_DELETE_REPLACE?replacement:NULL,.known=true};
    if(!targets(json,kind,id,&r.known))return fail(error,"not_found",ESP_ERR_NOT_FOUND);
    if(mode==TS_AUTO_DELETE_REPLACE && (!replacement||!strcmp(replacement,id)||!find(json,kind,replacement,&r.known)))
        return fail(error,"replacement_missing",ESP_ERR_NOT_FOUND);
    bool forced_pause=mode==TS_AUTO_DELETE_FORCE&&pause_confirmed;
    cJSON *plan=NULL;esp_err_t ret=ESP_OK;
    if(!forced_pause) {
        ret=make_plan(&r,&plan,error);
        if(ret!=ESP_OK)return mode==TS_AUTO_DELETE_FORCE?fail(error,"pause_required",ESP_ERR_INVALID_STATE):ret;
        if(impact)*impact=plan;
        if(!r.known)ret=fail(error,mode==TS_AUTO_DELETE_FORCE?"pause_required":"reference_check_failed",ESP_ERR_INVALID_STATE);
        if(ret==ESP_OK&&mode==TS_AUTO_DELETE_NORMAL) {
            const char *collections[]={"rules","actions","commands","sources"};
            for(unsigned i=0;i<4;++i)if(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(plan,collections[i])))ret=fail(error,"references_exist",ESP_ERR_INVALID_STATE);
        }
    }
    /* Replacement decisions keep referring to the original names throughout
     * the edit, including same-namespace observers that become mirrors. */
    cJSON *original = ret==ESP_OK && r.replacement ? cJSON_Duplicate(json,true) : NULL;
    if(ret==ESP_OK && r.replacement && !original)ret=ESP_ERR_NO_MEM;
    if(original)r.json=original;
    if(ret==ESP_OK)ret=apply_plan(json,&r);
    cJSON_Delete(original);
    if(ret==ESP_OK) {
        cJSON *rows=cJSON_GetObjectItemCaseSensitive(json,ts_auto_config_collection(kind));
        for(int i=cJSON_GetArraySize(rows)-1;i>=0;--i)
            if(!strcmp(string(cJSON_GetArrayItem(rows,i),kind==TS_AUTO_CONFIG_VARIABLE?"name":"id",true,&r.known),id))cJSON_DeleteItemFromArray(rows,i);
        if(forced_pause&&(!set(json,"enabled",cJSON_CreateFalse())||!set(json,"pause_reason",cJSON_CreateString("references_unavailable"))))ret=ESP_ERR_NO_MEM;
    }
    if(!impact)cJSON_Delete(plan);
    if(ret==ESP_ERR_NO_MEM)fail(error,"out_of_memory",ret);
    return ret;
}
