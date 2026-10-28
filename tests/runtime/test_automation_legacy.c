/* Production upgrade reader. NVS and pack verification are boundary stubs;
 * actual wire records, selector checksums, SHA-256 and files are exercised. */
#include "nvs.h"
#include "ts_config_pack.h"
#include "ts_crypto.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char test_root[] = "/tmp/tianshan-auto-legacy-XXXXXX";
static FILE *test_fopen(const char *, const char *);
static DIR *test_opendir(const char *);
static int test_stat(const char *, struct stat *);
#define fopen(path,mode) test_fopen(path,mode)
#define opendir(path) test_opendir(path)
#define stat(path,out) test_stat(path,out)
#include "../../components/ts_automation/src/ts_automation_legacy.c"
#undef fopen
#undef opendir
#undef stat

typedef struct { char ns[16], key[16]; nvs_type_t type; void *data; size_t length; } record_t;
static record_t records[128];
static size_t record_count;
static const char *handles[128];
static unsigned handle_count, pack_calls, raw_pack_calls;
static bool reject_pack = true, accepted_seen, missing_signing_root;

static record_t *find_record(const char *ns, const char *key)
{
    for (size_t i=0; i<record_count; ++i)
        if (!strcmp(records[i].ns,ns) && !strcmp(records[i].key,key)) return &records[i];
    return NULL;
}
static void put(const char *ns,const char *key,nvs_type_t type,const void *data,size_t length)
{
    record_t *r=find_record(ns,key);
    if (!r) { assert(record_count<128);r=&records[record_count++];strcpy(r->ns,ns);strcpy(r->key,key); }
    free(r->data);r->data=malloc(length);assert(r->data);memcpy(r->data,data,length);r->length=length;r->type=type;
}
esp_err_t nvs_open_from_partition(const char *partition,const char *ns,nvs_open_mode_t mode,nvs_handle_t *out)
{
    assert(!strcmp(partition,"keys_nvs") && mode==NVS_READONLY);
    assert(strcmp(ns,"ts_keystore") && strcmp(ns,"ts_security") && strcmp(ns,"ts_pki"));
    for (size_t i=0;i<record_count;++i) if (!strcmp(records[i].ns,ns)) {
        assert(++handle_count<128);handles[handle_count]=records[i].ns;*out=handle_count;return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
void nvs_close(nvs_handle_t h) { assert(h && h<=handle_count); }
static esp_err_t get(nvs_handle_t h,const char *key,nvs_type_t type,void *out,size_t *length)
{
    record_t *r=find_record(handles[h],key);
    if (!r) return ESP_ERR_NVS_NOT_FOUND;
    if (r->type!=type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (out && *length<r->length) return ESP_ERR_NVS_INVALID_LENGTH;
    if (out) memcpy(out,r->data,r->length);
    *length=r->length;return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v) {size_t n=1;return get(h,k,NVS_TYPE_U8,v,&n);}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *v,size_t *n) {return get(h,k,NVS_TYPE_BLOB,v,n);}
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *v,size_t *n) {return get(h,k,NVS_TYPE_STR,v,n);}

cJSON *ts_config_pack_parse_json(const char *text,size_t length)
{
    return text ? cJSON_ParseWithLengthOpts(text,length+1,NULL,true) : NULL;
}
static ts_config_pack_result_t decrypted_fixture(const char *text,size_t length,ts_config_pack_t **out)
{
    if(reject_pack)return TS_CONFIG_PACK_ERR_SIGNATURE;
    *out=calloc(1,sizeof(**out));assert(*out);
    (*out)->content=strndup(text,length);(*out)->content_len=length;return TS_CONFIG_PACK_OK;
}
ts_config_pack_result_t ts_config_pack_load_verified_mem(const char *text,size_t length,
    const ts_config_pack_acceptance_t *accepted,ts_config_pack_t **out,
    ts_config_pack_acceptance_t *unused,uint32_t *generation)
{
    (void)unused;(void)generation;++pack_calls;
    if(accepted)accepted_seen=true;
    if(missing_signing_root)return TS_CONFIG_PACK_ERR_TRUST_NOT_CONFIGURED;
    return decrypted_fixture(text,length,out);
}
ts_config_pack_result_t ts_config_pack_load_mem(const char *text,size_t length,ts_config_pack_t **out)
{
    ++pack_calls;++raw_pack_calls;
    return decrypted_fixture(text,length,out);
}
void ts_config_pack_free(ts_config_pack_t *pack) { free(pack->content);free(pack); }

static const char *mapped(const char *path,char buffer[512])
{
    if(strncmp(path,"/sdcard/config",14))return path;
    snprintf(buffer,512,"%s%s",test_root,path+14);return buffer;
}
static FILE *test_fopen(const char *path,const char *mode) {char b[512];return fopen(mapped(path,b),mode);}
static DIR *test_opendir(const char *path) {char b[512];return opendir(mapped(path,b));}
static int test_stat(const char *path,struct stat *out) {char b[512];return stat(mapped(path,b),out);}
static void write_file(const char *name,const char *text)
{
    char path[512];snprintf(path,sizeof(path),"%s/%s",test_root,name);
    FILE *f=fopen(path,"wb");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));
}
static void remove_tree(const char *path)
{
    DIR *dir=opendir(path);assert(dir);struct dirent *entry;
    while((entry=readdir(dir))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        char child[1024];snprintf(child,sizeof(child),"%s/%s",path,entry->d_name);
        struct stat st;assert(!stat(child,&st));
        if(S_ISDIR(st.st_mode))remove_tree(child);else assert(!unlink(child));
    }
    assert(!closedir(dir));assert(!rmdir(path));
}
static void reset(void)
{
    for(size_t i=0;i<record_count;++i)free(records[i].data);
    memset(records,0,sizeof(records));record_count=handle_count=pack_calls=raw_pack_calls=0;reject_pack=true;missing_signing_root=false;
    remove_tree(test_root);assert(!mkdir(test_root,0775));
}
static cJSON *read_config(bool sd)
{
    cJSON *json=NULL;ts_auto_config_error_t error={0};assert(ts_auto_legacy_read(sd,&json,&error)==ESP_OK);return json;
}
static cJSON *list(cJSON *json,const char *key) {return cJSON_GetObjectItemCaseSensitive(json,key);}
static const char *one_rule="{\"id\":\"saved\",\"name\":\"Saved change\",\"enabled\":false,\"actions\":[]}";
static void selected(const char *rule)
{
    legacy_selector_t s={.magic=0x52534c33u,.version=3,.length=sizeof(s),.generation=3,.count=rule?1:0,.aggregate=FNV_OFFSET};
    if(rule) {
        unsigned char digest[32];char hex[65];ts_crypto_hash(TS_HASH_SHA256,rule,strlen(rule),digest,32);ts_crypto_hex_encode(digest,32,hex,65);
        cJSON *r=cJSON_CreateObject();assert(r);
        cJSON_AddStringToObject(r,"id","saved");cJSON_AddNumberToObject(r,"revision",7);cJSON_AddNumberToObject(r,"source",0);
        cJSON_AddNumberToObject(r,"length",strlen(rule));cJSON_AddStringToObject(r,"digest",hex);
        cJSON_AddItemToObject(r,"json",cJSON_Parse(rule));char *text=cJSON_PrintUnformatted(r);assert(text);
        put("rule_stage","set_a0",NVS_TYPE_BLOB,text,strlen(text)+1);s.aggregate=hash_bytes(FNV_OFFSET,text,strlen(text)+1);
        free(text);cJSON_Delete(r);
    }
    s.checksum=hash_bytes(FNV_OFFSET,&s,offsetof(legacy_selector_t,checksum));
    put("rule_stage","set_select",NVS_TYPE_BLOB,&s,sizeof(s));put("rule_stage","set_meta0",NVS_TYPE_BLOB,&s,sizeof(s));
}
static void selected_pack(void)
{
    char bytes[512];snprintf(bytes,sizeof(bytes),"{\"type\":\"automation_rule\",\"rule\":%s}",one_rule);
    write_file("rules/saved.tscfg",bytes);
    unsigned char hash[32];char digest[65];
    assert(ts_crypto_hash(TS_HASH_SHA256,bytes,strlen(bytes),hash,32)==ESP_OK);
    assert(ts_crypto_hex_encode(hash,32,digest,65)==ESP_OK);
    cJSON *row=cJSON_CreateObject();assert(row);
    cJSON_AddStringToObject(row,"id","saved");cJSON_AddNumberToObject(row,"revision",7);
    cJSON_AddNumberToObject(row,"source",2);cJSON_AddNumberToObject(row,"length",strlen(bytes));
    cJSON_AddStringToObject(row,"digest",digest);cJSON_AddStringToObject(row,"path","/sdcard/config/rules/saved.tscfg");
    cJSON *proof=cJSON_AddObjectToObject(row,"acceptance");assert(proof);
    cJSON_AddNumberToObject(proof,"policy",1);cJSON_AddNumberToObject(proof,"accepted_at",1);
    const char *keys[]={"package","signer","root","recipient"};
    for(unsigned i=0;i<4;++i)cJSON_AddStringToObject(proof,keys[i],digest);
    char *text=cJSON_PrintUnformatted(row);assert(text);
    legacy_selector_t selector={.magic=0x52534c33u,.version=3,.length=sizeof(selector),.generation=3,.count=1};
    selector.aggregate=hash_bytes(FNV_OFFSET,text,strlen(text)+1);
    selector.checksum=hash_bytes(FNV_OFFSET,&selector,offsetof(legacy_selector_t,checksum));
    put("rule_stage","set_a0",NVS_TYPE_BLOB,text,strlen(text)+1);
    put("rule_stage","set_select",NVS_TYPE_BLOB,&selector,sizeof(selector));
    put("rule_stage","set_meta0",NVS_TYPE_BLOB,&selector,sizeof(selector));free(text);cJSON_Delete(row);
}
int main(void)
{
    assert(mkdtemp(test_root));cJSON *json=read_config(false);assert(!cJSON_GetArraySize(list(json,"rules")));cJSON_Delete(json);
    legacy_command_t cmd={.enabled=1,.timeout_sec=20,.nohup=1,.created_time=42};
    strcpy(cmd.id,"command");strcpy(cmd.host_id,"host");strcpy(cmd.name,"Saved command");strcpy(cmd.command,"old run");
    put("ts_ssh_cmd","c_0",NVS_TYPE_BLOB,&cmd,offsetof(legacy_command_t,service_mode));
    legacy_host_t host={.port=22,.enabled=1};strcpy(host.id,"host");strcpy(host.host,"192.0.2.1");strcpy(host.username,"operator");
    put("ts_ssh_cfg","h_0",NVS_TYPE_BLOB,&host,sizeof(host));
    json=read_config(false);assert(!strcmp(string(cJSON_GetArrayItem(list(json,"commands"),0),"command"),"old run"));cJSON_Delete(json);
    char path[512];snprintf(path,sizeof(path),"%s/ssh_commands",test_root);assert(!mkdir(path,0775));
    write_file("ssh_commands/card.json","{\"id\":\"card\",\"host_id\":\"host\",\"name\":\"Card\",\"command\":\"card run\"}");
    json=read_config(true);assert(cJSON_GetArraySize(list(json,"commands"))==1);assert(!strcmp(string(cJSON_GetArrayItem(list(json,"commands"),0),"id"),"card"));cJSON_Delete(json);
    write_file("ssh_commands/card.tscfg","rejected package");
    ts_auto_config_error_t error={0};assert(ts_auto_legacy_read(true,&json,&error)!=ESP_OK && !json && pack_calls==1);
    assert(!strcmp(error.reason,"signature_invalid"));
    reset();snprintf(path,sizeof(path),"%s/sources",test_root);assert(!mkdir(path,0775));reject_pack=false;missing_signing_root=true;
    write_file("sources/card.tscfg","{\"type\":\"automation_source\",\"source\":{\"id\":\"card\",\"type\":\"rest\",\"enabled\":true,\"url\":\"http://192.0.2.10/status\",\"method\":\"GET\",\"auth_header\":\"synthetic-token\",\"json_path\":\"data.power\",\"mappings\":[{\"json_path\":\"data.power\",\"var_name\":\"card.power\"}]}}");
    json=read_config(true);assert(raw_pack_calls==1);
    ts_auto_document_t *document=NULL;assert(ts_auto_document_decode(json,&document,&error)==ESP_OK);
    assert(document->source_count==1 && document->sources[0].type==TS_AUTO_SRC_REST &&
        !strcmp(document->sources[0].rest.url,"http://192.0.2.10/status") &&
        !strcmp(document->sources[0].rest.path,"data.power") && !strcmp(document->sources[0].rest.auth_header,"synthetic-token") &&
        document->sources[0].mapping_count==1 && !strcmp(document->sources[0].mappings[0].json_path,"data.power") &&
        !strcmp(document->sources[0].mappings[0].var_name,"card.power"));
    ts_auto_document_free(document);cJSON_Delete(json);
    reset();snprintf(path,sizeof(path),"%s/rules",test_root);assert(!mkdir(path,0775));
    write_file("rules/stale.json","{\"id\":\"stale\",\"enabled\":false,\"actions\":[]}");
    selected(one_rule);json=read_config(true);assert(cJSON_GetArraySize(list(json,"rules"))==1);
    assert(!strcmp(string(cJSON_GetArrayItem(list(json,"rules"),0),"id"),"saved"));
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(list(json,"rules"),0),"revision")->valueint==7);cJSON_Delete(json);
    selected_pack();reject_pack=false;accepted_seen=false;json=read_config(true);
    const cJSON *saved=cJSON_GetArrayItem(list(json,"rules"),0),*proof=cJSON_GetObjectItem(saved,"_package");
    assert(accepted_seen && cJSON_IsObject(proof) && strlen(string(proof,"package"))==64);
    assert(cJSON_GetObjectItem(saved,"revision")->valueint==7);cJSON_Delete(json);
    reject_pack=true;
    assert(ts_auto_legacy_read(true,&json,&error)!=ESP_OK && !json);
    selected(NULL);json=read_config(true);assert(!cJSON_GetArraySize(list(json,"rules")));cJSON_Delete(json);
    selected(one_rule);record_t *r=find_record("rule_stage","set_a0");strcpy(r->key,"missing_record");
    assert(ts_auto_legacy_read(true,&json,&error)!=ESP_OK && !json); /* Never load stale.json on a broken selected set. */
    reset();snprintf(path,sizeof(path),"%s/rules",test_root);assert(!mkdir(path,0775));
    legacy_guard_t guard={.magic=0x52554c32u,.phase=1,.source=1,.had_old=1};strcpy(guard.id,"saved");
    guard.old_hash=hash_bytes(FNV_OFFSET,one_rule,strlen(one_rule));
    put("rule_stage","guard",NVS_TYPE_BLOB,&guard,sizeof(guard));
    write_file("rules/saved.json","{\"id\":\"saved\",\"name\":\"Uncommitted\",\"enabled\":false,\"actions\":[]}");
    write_file("rules/.saved.previous",one_rule);
    json=read_config(true);assert(!strcmp(string(cJSON_GetArrayItem(list(json,"rules"),0),"name"),"Saved change"));cJSON_Delete(json);
    assert(ts_auto_legacy_read(false,&json,&error)!=ESP_OK && !json);
    guard.phase=0;guard.mirror=1;legacy_bank_t bank={.count=1,.hash=hash_bytes(FNV_OFFSET,one_rule,strlen(one_rule)+1)};
    put("rule_stage","guard",NVS_TYPE_BLOB,&guard,sizeof(guard));put("rule_stage","meta_a",NVS_TYPE_BLOB,&bank,sizeof(bank));
    put("rule_stage","a0",NVS_TYPE_BLOB,one_rule,strlen(one_rule)+1);
    json=read_config(false);assert(cJSON_GetArraySize(list(json,"rules"))==1);cJSON_Delete(json);
    reset();remove_tree(test_root);
    puts("PASS legacy automation: read-only upgrade, deployed layouts, committed set, empty set, interrupted edit and pack priority");
    return 0;
}
