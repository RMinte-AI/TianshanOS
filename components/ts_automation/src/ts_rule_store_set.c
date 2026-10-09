/* Versioned complete sets in the existing rule_stage namespace. Legacy store is
 * recovered first; its guard/banks are not modified by this format migration. */
#include "ts_rule_store_set.h"
#include "ts_rule_pack.h"
#include "ts_crypto.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#ifndef DIR_RULES
#define DIR_RULES "/sdcard/config/rules"
#endif
#ifndef CONFIG_TS_AUTOMATION_MAX_RULES
#define CONFIG_TS_AUTOMATION_MAX_RULES 32
#endif
#define SET_NAMESPACE "rule_stage"
#define SET_MAGIC 0x52534c33u
#define SET_VERSION 3
#define OBJECT_DIR DIR_RULES "/.rule_store"
#define RECORD_LIMIT 262144u
typedef struct {
    uint32_t magic, version, length, generation, bank, count, aggregate, checksum;
} set_selector_t;
static set_selector_t selected;
static cJSON *saved;
static bool set_loaded, set_blocked, legacy_readonly;
static bool set_sd;
static const char *load_error="config_loading";
static bool cleanup_objects(void);

static uint32_t bytes_hash(uint32_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}
static bool digest_text(const char *text, size_t length, char out[65]) {
    unsigned char digest[32];
    return ts_crypto_hash(TS_HASH_SHA256, text, length, digest, sizeof digest) == ESP_OK &&
           ts_crypto_hex_encode(digest, sizeof digest, out, 65) == ESP_OK;
}
static bool selector_valid(const set_selector_t *s) {
    return s->magic == SET_MAGIC && s->version == SET_VERSION && s->length == sizeof(*s) &&
        s->generation && s->bank <= 1 && s->count <= CONFIG_TS_AUTOMATION_MAX_RULES &&
        s->checksum == bytes_hash(2166136261u, s, offsetof(set_selector_t, checksum));
}
static esp_err_t read_blob(const char *key, void *out, size_t size) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(SET_NAMESPACE, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    size_t n = size;
    e = nvs_get_blob(h, key, out, &n);
    nvs_close(h);
    return e == ESP_OK && n != size ? ESP_ERR_INVALID_SIZE : e;
}
static esp_err_t write_blob(const char *key, const void *bytes, size_t length) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(SET_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, key, bytes, length);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
static char *read_record(unsigned bank, unsigned index) {
    char key[16]; snprintf(key, sizeof key, "set_%c%u", bank ? 'b' : 'a', index);
    nvs_handle_t h;
    if (nvs_open(SET_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t n = 0;
    char *text = NULL;
    if (nvs_get_blob(h, key, NULL, &n) == ESP_OK && n >= 2 && n <= RECORD_LIMIT)
        text = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text && (nvs_get_blob(h, key, text, &n) != ESP_OK || text[n-1] || strlen(text) != n-1)) {
        free(text); text = NULL;
    }
    nvs_close(h);
    return text;
}
static const char *string_field(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool number_field(const cJSON *j, const char *key, uint32_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > UINT32_MAX ||
        v->valuedouble != (uint32_t)v->valuedouble) return false;
    *out = (uint32_t)v->valuedouble; return true;
}
static cJSON *find_record(const cJSON *array, const char *id) {
    cJSON *r;
    cJSON_ArrayForEach(r, array) {
        const char *name = string_field(r, "id");
        if (name && !strcmp(name, id)) return r;
    }
    return NULL;
}
static bool record_info(const cJSON *r, ts_rule_saved_info_t *info) {
    const char *id = string_field(r, "id"), *digest = string_field(r, "digest");
    uint32_t source;
    if (!id || !ts_rule_id_valid(id) || !digest || strlen(digest) != 64 ||
        !number_field(r, "revision", &info->revision) || !info->revision ||
        !number_field(r, "source", &source) || source > TS_RULE_SET_SD_PACK ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(r, "readonly"))) return false;
    strcpy(info->id, id); strcpy(info->digest, digest);
    info->source = source; info->generation = selected.generation;
    info->readonly = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "readonly"));
    const char *name=string_field(r,"name"), *icon=string_field(r,"icon");
    if(!name||strlen(name)>=sizeof(info->name)||!icon||strlen(icon)>=sizeof(info->icon)||
       !number_field(r,"conditions_count",&info->conditions_count)||
       !number_field(r,"actions_count",&info->actions_count))return false;
    strcpy(info->name,name);strcpy(info->icon,icon);
    info->enabled=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"enabled"));
    return true;
}
static bool acceptance_read(const cJSON *r, ts_config_pack_acceptance_t *a) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(r, "acceptance");
    const char *package = string_field(j,"package"), *signer = string_field(j,"signer"),
               *root = string_field(j,"root"), *recipient = string_field(j,"recipient");
    const cJSON *when = cJSON_GetObjectItemCaseSensitive(j,"accepted_at");
    if (!number_field(j,"policy",&a->policy) || !a->policy || !cJSON_IsNumber(when) ||
        when->valuedouble <= 0 || when->valuedouble > 9007199254740991.0 ||
        when->valuedouble != (int64_t)when->valuedouble || !package || !signer || !root || !recipient ||
        strlen(package)!=64 || strlen(signer)!=64 || strlen(root)!=64 || strlen(recipient)!=64) return false;
    a->accepted_at=(int64_t)when->valuedouble;
    strcpy(a->package_sha256,package);strcpy(a->signer_sha256,signer);
    strcpy(a->root_sha256,root);strcpy(a->recipient,recipient);return true;
}
static cJSON *acceptance_json(const ts_config_pack_acceptance_t *a) {
    cJSON *j=cJSON_CreateObject();
    if (!j || !cJSON_AddNumberToObject(j,"policy",a->policy) ||
        !cJSON_AddNumberToObject(j,"accepted_at",a->accepted_at) ||
        !cJSON_AddStringToObject(j,"package",a->package_sha256) ||
        !cJSON_AddStringToObject(j,"signer",a->signer_sha256) ||
        !cJSON_AddStringToObject(j,"root",a->root_sha256) ||
        !cJSON_AddStringToObject(j,"recipient",a->recipient)) { cJSON_Delete(j);return NULL; }
    return j;
}
static esp_err_t object_read(const cJSON *r, char **text, size_t *length) {
    const char *path=string_field(r,"path"), *expected=string_field(r,"digest");
    uint32_t n;
    *text=NULL;
    if (!path || !expected || !number_field(r,"length",&n) || !n || n>RECORD_LIMIT)
        return ESP_ERR_INVALID_ARG;
    FILE *f=fopen(path,"rb"); if(!f)return ESP_ERR_NOT_FOUND;
    bool ok=fseek(f,0,SEEK_END)==0 && ftell(f)==n && fseek(f,0,SEEK_SET)==0;
    char *s=ok?heap_caps_malloc(n+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    ok=s && fread(s,1,n,f)==n && !ferror(f);
    if(fclose(f))ok=false;
    char digest[65];
    if(ok){s[n]=0;ok=digest_text(s,n,digest)&&!strcmp(digest,expected);}
    if(!ok){free(s);return ESP_FAIL;}
    *text=s;*length=n;return ESP_OK;
}
static esp_err_t decode_record(const cJSON *r, ts_auto_rule_t *rule, ts_rule_saved_info_t *info) {
    memset(info,0,sizeof(*info));
    if(!record_info(r,info))return ESP_ERR_INVALID_ARG;
    esp_err_t e;
    if(info->source==TS_RULE_SET_SD_PACK){
        char *text=NULL;size_t length=0;
        e=object_read(r,&text,&length);
        if(e!=ESP_OK)return e;
        ts_config_pack_acceptance_t a={0};ts_config_pack_t *pack=NULL;
        if(!acceptance_read(r,&a)||strcmp(a.package_sha256,info->digest)){free(text);return ESP_ERR_INVALID_ARG;}
        ts_config_pack_result_t got=ts_config_pack_load_verified_mem(text,length,&a,&pack,NULL,NULL);
        free(text);
        if(got!=TS_CONFIG_PACK_OK){load_error=ts_rule_pack_error(got);return ESP_ERR_INVALID_STATE;}
        e=ts_rule_pack_decode(pack->content,pack->content_len,rule);ts_config_pack_free(pack);
    }else{
        const cJSON *j=cJSON_GetObjectItemCaseSensitive(r,"json");
        char *text=cJSON_PrintUnformatted(j), digest[65];
        bool ok=text&&digest_text(text,strlen(text),digest)&&!strcmp(digest,info->digest);
        free(text);
        if(!ok)return ESP_FAIL;
        if(info->source==TS_RULE_SET_SD_JSON && set_sd && string_field(r,"path")) {
            char *bytes=NULL;size_t n=0;
            e=object_read(r,&bytes,&n);free(bytes);
            if(e!=ESP_OK)return e;
        }
        e=ts_rule_decode(j,rule);
    }
    if(e==ESP_OK&&strcmp(rule->id,info->id)){ts_rule_dispose(rule);return ESP_ERR_INVALID_ARG;}
    if(e==ESP_OK)rule->revision=info->revision;
    return e;
}
void ts_rule_store_set_reset(void){cJSON_Delete(saved);saved=NULL;memset(&selected,0,sizeof selected);set_loaded=set_blocked=legacy_readonly=set_sd=false;load_error="recovery_required";}
const char *ts_rule_store_set_error(void){return load_error;}
bool ts_rule_store_set_available(void){return set_loaded&&!set_blocked&&!legacy_readonly;}
bool ts_rule_store_set_present(void){return set_loaded||set_blocked;}
int ts_rule_store_set_count(void){return cJSON_GetArraySize(saved);}
uint32_t ts_rule_store_set_generation(void){return selected.generation;}
esp_err_t ts_rule_store_set_info(const char *id, ts_rule_saved_info_t *info){
    cJSON *r=find_record(saved,id);return !r?ESP_ERR_NOT_FOUND:record_info(r,info)?ESP_OK:ESP_FAIL;
}
esp_err_t ts_rule_store_set_info_at(int index,ts_rule_saved_info_t *info){
    const cJSON *r=cJSON_GetArrayItem(saved,index);
    return !r?ESP_ERR_NOT_FOUND:record_info(r,info)?ESP_OK:ESP_FAIL;
}
esp_err_t ts_rule_store_set_acceptance(const char *bytes,size_t length,ts_config_pack_acceptance_t *out){
    if(!ts_rule_store_set_available())return ESP_ERR_NOT_FOUND;
    char digest[65];if(!digest_text(bytes,length,digest))return ESP_FAIL;
    const cJSON *r;
    cJSON_ArrayForEach(r,saved){ts_rule_saved_info_t info={0};
        if(record_info(r,&info)&&info.source==TS_RULE_SET_SD_PACK&&!strcmp(info.digest,digest))
            return acceptance_read(r,out)?ESP_OK:ESP_FAIL;
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t ts_rule_store_set_get(int index,ts_auto_rule_t *rule,ts_rule_saved_info_t *info){
    const cJSON *r=cJSON_GetArrayItem(saved,index);return r?decode_record(r,rule,info):ESP_ERR_NOT_FOUND;
}
static esp_err_t load_saved_set(bool sd,ts_auto_rule_t *rules,bool *readonly,int capacity,int *count,bool metadata_only){
    ts_rule_store_set_reset();set_sd=sd;*count=0;
    esp_err_t e=read_blob("set_select",&selected,sizeof selected);
    if(e==ESP_ERR_NVS_NOT_FOUND)return ESP_ERR_NOT_FOUND;
    if(e!=ESP_OK||!selector_valid(&selected)){set_blocked=true;return ESP_ERR_INVALID_STATE;}
    char key[16];snprintf(key,sizeof key,"set_meta%u",(unsigned)selected.bank);
    set_selector_t meta;
    if(read_blob(key,&meta,sizeof meta)!=ESP_OK||memcmp(&meta,&selected,sizeof meta)||selected.count>(unsigned)capacity){set_blocked=true;return ESP_FAIL;}
    saved=cJSON_CreateArray();if(!saved)return ESP_ERR_NO_MEM;
    uint32_t aggregate=2166136261u;
    for(unsigned i=0;i<selected.count;++i){
        char *text=read_record(selected.bank,i);
        if(!text){e=ESP_FAIL;break;}
        aggregate=bytes_hash(aggregate,text,strlen(text)+1);
        cJSON *r=cJSON_Parse(text);free(text);
        ts_rule_saved_info_t info={0};
        if(!record_info(r,&info)||find_record(saved,info.id)||(!sd&&info.source==TS_RULE_SET_SD_PACK)){
            if(!sd&&info.source==TS_RULE_SET_SD_PACK)load_error="source_unavailable";
            cJSON_Delete(r);e=ESP_ERR_INVALID_STATE;break;
        }
        if(metadata_only){
            if(info.source==TS_RULE_SET_SD_PACK){
                ts_config_pack_acceptance_t a={0};
                if(!acceptance_read(r,&a)||strcmp(a.package_sha256,info.digest))e=ESP_ERR_INVALID_ARG;
            }else{
                char *text=cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(r,"json")),digest[65];
                uint32_t n=0;
                bool valid=text&&number_field(r,"length",&n)&&strlen(text)==n&&
                    digest_text(text,n,digest)&&!strcmp(digest,info.digest);
                free(text);if(!valid)e=ESP_ERR_INVALID_ARG;
            }
            if(e==ESP_OK&&(info.source==TS_RULE_SET_SD_PACK || (sd&&info.source==TS_RULE_SET_SD_JSON&&string_field(r,"path")))) {
                char *bytes=NULL;size_t n=0;e=object_read(r,&bytes,&n);free(bytes);
            }
        } else e=decode_record(r,&rules[*count],&info);
        if(e!=ESP_OK){cJSON_Delete(r);break;}
        if(readonly)readonly[*count]=info.readonly||(!sd&&info.source==TS_RULE_SET_SD_JSON);
        if(!cJSON_AddItemToArray(saved,r)){if(rules)ts_rule_dispose(&rules[*count]);cJSON_Delete(r);e=ESP_ERR_NO_MEM;break;}
        ++*count;
    }
    if(e==ESP_OK&&aggregate!=selected.aggregate)e=ESP_FAIL;
    if(e!=ESP_OK){if(rules)for(int i=0;i<*count;++i)ts_rule_dispose(&rules[i]);*count=0;set_blocked=true;return e;}
    set_loaded=true;legacy_readonly=!sd;
    if(!sd){legacy_readonly=false;const cJSON *r; cJSON_ArrayForEach(r,saved){ts_rule_saved_info_t info={0};if(!record_info(r,&info)||info.source!=TS_RULE_SET_NVS_JSON)legacy_readonly=true;}}
    load_error="ok";
    (void)cleanup_objects();
    return ESP_OK;
}
esp_err_t ts_rule_store_set_load(bool sd,ts_auto_rule_t *rules,bool *readonly,int capacity,int *count){
    return load_saved_set(sd,rules,readonly,capacity,count,false);
}
esp_err_t ts_rule_store_set_refresh(bool sd){
    int count=0;return load_saved_set(sd,NULL,NULL,CONFIG_TS_AUTOMATION_MAX_RULES,&count,true);
}
static cJSON *new_record(const ts_auto_rule_t *rule,ts_rule_set_source_t source,bool readonly,
                         const char *path,const char *bytes,size_t length,const ts_config_pack_acceptance_t *accepted){
    cJSON *r=cJSON_CreateObject(),*json=source==TS_RULE_SET_SD_PACK?NULL:ts_rule_encode(rule);
    char *text=json?cJSON_PrintUnformatted(json):NULL;
    char digest[65];
    if(source!=TS_RULE_SET_SD_PACK){bytes=text;length=text?strlen(text):0;}
    bool ok=r&&bytes&&length&&digest_text(bytes,length,digest)&&
        (source!=TS_RULE_SET_SD_PACK||(accepted&&!strcmp(digest,accepted->package_sha256)))&&
        cJSON_AddStringToObject(r,"id",rule->id)&&cJSON_AddNumberToObject(r,"revision",rule->revision)&&
        cJSON_AddNumberToObject(r,"source",source)&&cJSON_AddBoolToObject(r,"readonly",readonly)&&
        cJSON_AddNumberToObject(r,"length",length)&&cJSON_AddStringToObject(r,"digest",digest);
    ok=ok&&cJSON_AddStringToObject(r,"name",rule->name)&&cJSON_AddStringToObject(r,"icon",rule->icon)&&
        cJSON_AddBoolToObject(r,"enabled",rule->enabled)&&
        cJSON_AddNumberToObject(r,"conditions_count",rule->conditions.count)&&
        cJSON_AddNumberToObject(r,"actions_count",rule->action_count);
    if(ok&&json){ok=cJSON_AddItemToObject(r,"json",json);if(ok)json=NULL;}
    if(ok&&source==TS_RULE_SET_SD_PACK){
        cJSON *a=accepted?acceptance_json(accepted):NULL;
        ok=path&&a&&cJSON_AddStringToObject(r,"path",path)&&cJSON_AddItemToObject(r,"acceptance",a);
        if(!ok)cJSON_Delete(a);
    }
    free(text);cJSON_Delete(json);
    if(!ok){cJSON_Delete(r);return NULL;}return r;
}
esp_err_t ts_rule_store_set_seed(const ts_auto_rule_t *rule,const char *path,bool readonly,
                               const ts_config_pack_acceptance_t *accepted){
    if(!saved)saved=cJSON_CreateArray();
    if(!saved)return ESP_ERR_NO_MEM;
    char *text=NULL;size_t length=0;
    if(accepted){FILE *f=fopen(path,"rb");if(!f)return ESP_FAIL;
        bool ok=fseek(f,0,SEEK_END)==0;long n=ok?ftell(f):-1;ok=n>0&&n<=RECORD_LIMIT&&fseek(f,0,SEEK_SET)==0;
        text=ok?heap_caps_malloc(n+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
        ok=text&&fread(text,1,n,f)==(size_t)n&&!ferror(f);if(fclose(f))ok=false;
        if(!ok){free(text);return ESP_FAIL;}text[n]=0;length=n;
    }
    cJSON *r=new_record(rule,accepted?TS_RULE_SET_SD_PACK:TS_RULE_SET_SD_JSON,readonly,path,text,length,accepted);
    free(text);
    if(!r)return ESP_ERR_NO_MEM;
    if(find_record(saved,rule->id)||!cJSON_AddItemToArray(saved,r)){cJSON_Delete(r);return ESP_ERR_INVALID_ARG;}
    return ESP_OK;
}
esp_err_t ts_rule_store_set_adopt(const ts_auto_rule_t *rules,int count,int source,const bool *readonly){
    if(set_blocked)return ESP_ERR_INVALID_STATE;
    if(!saved)saved=cJSON_CreateArray();
    if(!saved)return ESP_ERR_NO_MEM;
    if(cJSON_GetArraySize(saved)&&cJSON_GetArraySize(saved)!=count)return ESP_ERR_INVALID_STATE;
    if(!cJSON_GetArraySize(saved))for(int i=0;i<count;++i){
        cJSON *r=new_record(&rules[i],source==TS_RULE_SOURCE_SD?TS_RULE_SET_SD_JSON:TS_RULE_SET_NVS_JSON,
                           (readonly&&readonly[i])||source==TS_RULE_SOURCE_READONLY,NULL,NULL,0,NULL);
        if(!r||!cJSON_AddItemToArray(saved,r)){cJSON_Delete(r);return ESP_ERR_NO_MEM;}
    }
    for(int i=0;i<count;++i){
        cJSON *r=find_record(saved,rules[i].id);
        if(!r)return ESP_ERR_INVALID_STATE;
        if(readonly&&readonly[i])
            cJSON_ReplaceItemInObjectCaseSensitive(r,"readonly",cJSON_CreateBool(true));
    }
    legacy_readonly=source==TS_RULE_SOURCE_READONLY;set_loaded=true;return ESP_OK;
}
static bool object_referenced(const cJSON *array,const char *path){const cJSON *r;cJSON_ArrayForEach(r,array){const char *p=string_field(r,"path");if(p&&!strcmp(p,path))return true;}return false;}
static bool cleanup_objects(void){
    DIR *dir=opendir(OBJECT_DIR);if(!dir)return errno==ENOENT;
    bool ok=true;struct dirent *entry;
    while((entry=readdir(dir))){size_t n=strlen(entry->d_name);if(n!=69||strcmp(entry->d_name+64,".pack"))continue;
        bool owned=true;for(unsigned i=0;i<64;++i)owned&=(entry->d_name[i]>='0'&&entry->d_name[i]<='9')||(entry->d_name[i]>='a'&&entry->d_name[i]<='f');
        if(!owned)continue;
        char path[sizeof(OBJECT_DIR)+72];snprintf(path,sizeof path,OBJECT_DIR "/%.69s",entry->d_name);
        if(!object_referenced(saved,path)&&unlink(path)&&errno!=ENOENT)ok=false;
    }
    closedir(dir);return ok;
}
static esp_err_t prepare_object(const char *bytes,size_t length,char *path,size_t cap){
    char digest[65];if(!digest_text(bytes,length,digest))return ESP_FAIL;
    char *parents=strdup(DIR_RULES);
    if(!parents)return ESP_ERR_NO_MEM;
    for(char *p=parents+1;*p;++p)if(*p=='/'){
        *p=0;bool ok=mkdir(parents,0755)==0||errno==EEXIST;*p='/';
        if(!ok){free(parents);return ESP_FAIL;}
    }
    free(parents);
    if(mkdir(DIR_RULES,0755)&&errno!=EEXIST)return ESP_FAIL;
    if(mkdir(OBJECT_DIR,0755)&&errno!=EEXIST)return ESP_FAIL;
    if(snprintf(path,cap,OBJECT_DIR "/%s.pack",digest)>=(int)cap)return ESP_ERR_INVALID_SIZE;
    struct stat st;
    if(stat(path,&st)==0){
        cJSON *r=cJSON_CreateObject();cJSON_AddStringToObject(r,"path",path);cJSON_AddStringToObject(r,"digest",digest);cJSON_AddNumberToObject(r,"length",length);
        char *old=NULL;size_t n=0;esp_err_t e=object_read(r,&old,&n);cJSON_Delete(r);free(old);
        if(e==ESP_OK)return ESP_OK;
        if(object_referenced(saved,path)||unlink(path))return ESP_FAIL;
    }else if(errno!=ENOENT)return ESP_FAIL;
    FILE *f=fopen(path,"wb");if(!f)return ESP_FAIL;
    bool ok=fwrite(bytes,1,length,f)==length&&!ferror(f);
    if(fflush(f))ok=false;
    if(fsync(fileno(f)))ok=false;
    if(fclose(f))ok=false;
    if(!ok)return ESP_FAIL;
    cJSON *r=cJSON_CreateObject();cJSON_AddStringToObject(r,"path",path);cJSON_AddStringToObject(r,"digest",digest);cJSON_AddNumberToObject(r,"length",length);
    char *check=NULL;size_t n=0;esp_err_t e=object_read(r,&check,&n);cJSON_Delete(r);free(check);return e;
}
static esp_err_t prepare_sources(cJSON *next) {
    cJSON *r;
    cJSON_ArrayForEach(r,next) {
        ts_rule_saved_info_t info={0};
        if(!record_info(r,&info))return ESP_ERR_INVALID_ARG;
        if(info.source==TS_RULE_SET_SD_JSON) {
            char *text=cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(r,"json"));
            if(!text)return ESP_ERR_NO_MEM;
            char path[sizeof(OBJECT_DIR)+72];
            esp_err_t e=prepare_object(text,strlen(text),path,sizeof path);free(text);
            if(e!=ESP_OK)return e;
            cJSON_DeleteItemFromObjectCaseSensitive(r,"path");
            if(!cJSON_AddStringToObject(r,"path",path))return ESP_ERR_NO_MEM;
        } else if(info.source==TS_RULE_SET_SD_PACK) {
            char *bytes=NULL;size_t n=0;
            esp_err_t e=object_read(r,&bytes,&n);
            char path[sizeof(OBJECT_DIR)+72];
            if(e==ESP_OK)e=prepare_object(bytes,n,path,sizeof path);
            free(bytes);
            if(e!=ESP_OK)return e;
            cJSON_DeleteItemFromObjectCaseSensitive(r,"path");
            if(!cJSON_AddStringToObject(r,"path",path))return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}
static esp_err_t commit_set(cJSON *next,uint32_t revision,ts_rule_commit_t *result){
    const cJSON *item;
    esp_err_t prepared=prepare_sources(next);
    if(prepared!=ESP_OK){cJSON_Delete(next);return prepared;}
    set_selector_t target={.magic=SET_MAGIC,.version=SET_VERSION,.length=sizeof target,
        .generation=selected.generation+1,.bank=selected.magic?1-selected.bank:0,
        .count=cJSON_GetArraySize(next),.aggregate=2166136261u};
    unsigned i=0;esp_err_t e=ESP_OK;
    cJSON_ArrayForEach(item,next){
        char *text=cJSON_PrintUnformatted(item);char key[16];snprintf(key,sizeof key,"set_%c%u",target.bank?'b':'a',i++);
        if(!text){e=ESP_ERR_NO_MEM;break;}
        size_t n=strlen(text)+1;
        if(n>RECORD_LIMIT||write_blob(key,text,n)!=ESP_OK){free(text);e=ESP_FAIL;break;}
        char *check=read_record(target.bank,i-1);bool ok=check&&!strcmp(check,text);
        target.aggregate=bytes_hash(target.aggregate,text,n);free(check);free(text);
        if(!ok){e=ESP_FAIL;break;}
    }
    target.checksum=bytes_hash(2166136261u,&target,offsetof(set_selector_t,checksum));
    char key[16];snprintf(key,sizeof key,"set_meta%u",(unsigned)target.bank);
    if(e==ESP_OK)e=write_blob(key,&target,sizeof target);
    set_selector_t check={0};
    if(e==ESP_OK&&(read_blob(key,&check,sizeof check)!=ESP_OK||memcmp(&check,&target,sizeof check)))e=ESP_FAIL;
    if(e!=ESP_OK){cJSON_Delete(next);return e;}
    esp_err_t write=write_blob("set_select",&target,sizeof target);
    esp_err_t read=read_blob("set_select",&check,sizeof check);
    if(read==ESP_OK&&!memcmp(&check,&target,sizeof check)){
        selected=target;cJSON_Delete(saved);saved=next;
        result->applied=result->durable=1;result->revision=revision;
        result->mirror_synced=cleanup_objects();result->error_code=result->mirror_synced?"ok":"cleanup_pending";
        return ESP_OK;
    }
    bool old=(read==ESP_OK&&!memcmp(&check,&selected,sizeof check)) ||
             (read==ESP_ERR_NVS_NOT_FOUND&&!selected.magic);
    cJSON_Delete(next);
    if(old)return write==ESP_OK?ESP_FAIL:write;
    set_blocked=true;result->applied=result->durable=-1;result->error_code="commit_unknown";
    return ESP_ERR_INVALID_STATE;
}

esp_err_t ts_rule_store_set_migrate(void){
    if(selected.magic||legacy_readonly)return ESP_OK;
    if(!ts_rule_store_set_available())return ESP_ERR_INVALID_STATE;
    cJSON *next=cJSON_Duplicate(saved,true);if(!next)return ESP_ERR_NO_MEM;
    ts_rule_commit_t result={.error_code="storage_failed"};
    return commit_set(next,0,&result);
}
esp_err_t ts_rule_store_set_commit(const ts_auto_rule_t *candidate,const char *id,uint32_t expected,
    uint32_t expected_generation,const char *pack,size_t pack_len,const ts_config_pack_acceptance_t *accepted,
    ts_rule_commit_t *result){
    *result=(ts_rule_commit_t){.error_code="storage_failed"};
    if(!ts_rule_store_set_available()){result->error_code=legacy_readonly?"source_read_only":"recovery_required";return ESP_ERR_INVALID_STATE;}
    ts_rule_saved_info_t prior={0};esp_err_t found=ts_rule_store_set_info(id,&prior);
    if(selected.generation!=expected_generation || (found==ESP_OK?prior.revision:0)!=expected){result->error_code="revision_conflict";return ESP_ERR_INVALID_STATE;}
    if(!candidate&&found!=ESP_OK)return ESP_ERR_NOT_FOUND;
    if(candidate&&found!=ESP_OK&&ts_rule_store_set_count()>=CONFIG_TS_AUTOMATION_MAX_RULES){result->error_code="capacity";return ESP_ERR_NO_MEM;}
    if(selected.generation==UINT32_MAX||prior.revision==UINT32_MAX){result->error_code="revision_exhausted";return ESP_ERR_INVALID_STATE;}
    char digest[65];
    if(pack&&(!candidate||!accepted||!digest_text(pack,pack_len,digest)||strcmp(digest,accepted->package_sha256)))return ESP_ERR_INVALID_ARG;
    if(pack&&found==ESP_OK&&prior.source==TS_RULE_SET_SD_PACK&&!strcmp(prior.digest,digest)){
        char *verified=NULL;size_t n=0;
        esp_err_t e=object_read(find_record(saved,id),&verified,&n);free(verified);
        if(e!=ESP_OK){result->error_code="source_unavailable";return e;}
        result->applied=result->durable=result->mirror_synced=1;result->revision=prior.revision;result->error_code="no_change";return ESP_OK;
    }
    if(!pack&&found==ESP_OK&&prior.readonly){result->error_code="source_read_only";return ESP_ERR_NOT_SUPPORTED;}
    cJSON *next=cJSON_Duplicate(saved,true);if(!next)return ESP_ERR_NO_MEM;
    char path[sizeof(OBJECT_DIR)+72];
    if(pack){esp_err_t e=prepare_object(pack,pack_len,path,sizeof path);if(e!=ESP_OK){cJSON_Delete(next);return e;}}
    ts_auto_rule_t local={0};if(candidate){local=*candidate;local.revision=found==ESP_OK?prior.revision+1:1;}
    ts_rule_set_source_t source=pack?TS_RULE_SET_SD_PACK:
        found==ESP_OK&&prior.source==TS_RULE_SET_SD_JSON?TS_RULE_SET_SD_JSON:TS_RULE_SET_NVS_JSON;
    cJSON *record=candidate?new_record(&local,source,pack!=NULL,
                                      pack?path:NULL,pack,pack_len,accepted):NULL;
    if(candidate&&!record){cJSON_Delete(next);return ESP_ERR_NO_MEM;}
    int index=0;const cJSON *item;cJSON_ArrayForEach(item,next){const char *name=string_field(item,"id");if(name&&!strcmp(name,id))break;++index;}
    if(found==ESP_OK){if(record){if(!cJSON_ReplaceItemInArray(next,index,record)){cJSON_Delete(record);cJSON_Delete(next);return ESP_ERR_NO_MEM;}}else cJSON_DeleteItemFromArray(next,index);}
    else if(!cJSON_AddItemToArray(next,record)){cJSON_Delete(record);cJSON_Delete(next);return ESP_ERR_NO_MEM;}
    return commit_set(next,local.revision,result);
}
