#include "nvs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include "ts_led_color_correction.h"
#include "ts_storage.h"
#include "ts_temp_source.h"
#include "ts_power_policy.h"

FILE *legacy_file(const char *path, const char *mode);
int legacy_stat(const char *path, struct stat *out);
#define stat(path, out) legacy_stat(path, out)
#define fopen legacy_file
#include "../../components/ts_core/ts_config/src/ts_config_migrate.c"
#undef fopen
#undef stat

typedef struct {
    bool live;
    char partition[16], ns[16], key[16];
    nvs_type_t type;
    unsigned char *data;
    size_t length;
} record_t;
static record_t records[128];
static size_t record_count;
static struct { char partition[16], ns[16]; } handles[128];
static unsigned handle_count, file_reads, private_reads, erased;
static bool fail_auth_commit, encrypted_net, fail_pack;
static unsigned pack_reads;
static const char *color_file;
static bool color_sd=true, fail_color_write, fail_power_write;
struct nvs_opaque_iterator_t { size_t index; };

static record_t *find(const char *partition, const char *ns, const char *key)
{
    for (size_t i = 0; i < record_count; ++i) {
        record_t *r = &records[i];
        if (r->live && !strcmp(r->partition, partition) &&
            !strcmp(r->ns, ns) && !strcmp(r->key, key)) return r;
    }
    return NULL;
}
static void put(const char *partition, const char *ns, const char *key,
                nvs_type_t type, const void *data, size_t length)
{
    record_t *r = find(partition, ns, key);
    if (!r) {
        assert(record_count < 128);
        r = &records[record_count++];
        strcpy(r->partition, partition); strcpy(r->ns, ns); strcpy(r->key, key);
    }
    free(r->data);
    r->data = malloc(length ? length : 1); assert(r->data);
    memcpy(r->data, data, length); r->length = length; r->type = type; r->live = true;
}
esp_err_t nvs_open_from_partition(const char *partition, const char *ns,
                                  nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (mode == NVS_READONLY) {
        bool found = false;
        for (size_t i = 0; i < record_count; ++i)
            if (records[i].live && !strcmp(records[i].partition, partition) &&
                !strcmp(records[i].ns, ns)) found = true;
        if (!found) return ESP_ERR_NVS_NOT_FOUND;
    }
    assert(++handle_count < 128);
    strcpy(handles[handle_count].partition, partition);
    strcpy(handles[handle_count].ns, ns); *out = handle_count; return ESP_OK;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open_from_partition("nvs", ns, mode, out);
}
void nvs_close(nvs_handle_t handle) { assert(handle > 0 && handle <= handle_count); }
static esp_err_t get(nvs_handle_t h, const char *key, nvs_type_t type, void *out, size_t *size)
{
    if (!strcmp(handles[h].ns, "ts_keystore") ||
        (!strcmp(handles[h].ns, "ts_pki") && !strcmp(key, "privkey"))) ++private_reads;
    record_t *r = find(handles[h].partition, handles[h].ns, key);
    if (!r) return ESP_ERR_NVS_NOT_FOUND;
    if (r->type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (out && *size < r->length) return ESP_ERR_NVS_INVALID_LENGTH;
    if (out) memcpy(out, r->data, r->length);
    *size = r->length; return ESP_OK;
}
static esp_err_t set(nvs_handle_t h, const char *key, nvs_type_t type, const void *data, size_t size)
{
    if(fail_power_write && !strcmp(handles[h].ns,"ts_power"))return ESP_FAIL;
    if(fail_color_write && !strcmp(handles[h].ns,"led_color"))return ESP_FAIL;
    put(handles[h].partition, handles[h].ns, key, type, data, size); return ESP_OK;
}
#define INTEGER(kind, suffix, ctype) \
esp_err_t nvs_get_##suffix(nvs_handle_t h,const char *k,ctype *v) { \
    size_t n = sizeof(*v); return get(h,k,NVS_TYPE_##kind,v,&n); } \
esp_err_t nvs_set_##suffix(nvs_handle_t h,const char *k,ctype v) { \
    return set(h,k,NVS_TYPE_##kind,&v,sizeof(v)); }
INTEGER(U8,u8,uint8_t)
INTEGER(I8,i8,int8_t)
INTEGER(U16,u16,uint16_t)
INTEGER(I16,i16,int16_t)
INTEGER(U32,u32,uint32_t)
INTEGER(I32,i32,int32_t)
INTEGER(U64,u64,uint64_t)
INTEGER(I64,i64,int64_t)
#undef INTEGER
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *v,size_t *n) {return get(h,k,NVS_TYPE_STR,v,n);}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *v,size_t *n) {return get(h,k,NVS_TYPE_BLOB,v,n);}
esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *v) {return set(h,k,NVS_TYPE_STR,v,strlen(v)+1);}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *v,size_t n) {return set(h,k,NVS_TYPE_BLOB,v,n);}
esp_err_t nvs_commit(nvs_handle_t h) {return fail_auth_commit && !strcmp(handles[h].ns,"ts_auth") ? ESP_FAIL : ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key)
{
    record_t *marker = find("nvs","ts_cfg_state","system_v");
    assert(marker && marker->data[0] == 1); /* Completion precedes any cleanup. */
    record_t *r = find(handles[h].partition, handles[h].ns, key);
    if (!r) return ESP_ERR_NVS_NOT_FOUND;
    r->live = false; ++erased; return ESP_OK;
}
esp_err_t nvs_entry_find(const char *partition,const char *ns,nvs_type_t type,nvs_iterator_t *it)
{
    assert(!strcmp(partition,"keys_nvs") && !ns && type == NVS_TYPE_ANY);
    *it = malloc(sizeof(**it)); assert(*it); (*it)->index = (size_t)-1;
    return nvs_entry_next(it);
}
esp_err_t nvs_entry_next(nvs_iterator_t *it)
{
    while (++(*it)->index < record_count)
        if (records[(*it)->index].live && !strcmp(records[(*it)->index].partition,"keys_nvs"))
            return ESP_OK;
    free(*it); *it = NULL; return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_entry_info(const nvs_iterator_t it,nvs_entry_info_t *out)
{
    record_t *r = &records[it->index];
    strcpy(out->namespace_name,r->ns); strcpy(out->key,r->key); out->type=r->type; return ESP_OK;
}
void nvs_release_iterator(nvs_iterator_t it) { free(it); }

int legacy_stat(const char *path, struct stat *out)
{
    if(color_file && strstr(path,"/led_color_correction.json")){memset(out,0,sizeof(*out));out->st_size=strlen(color_file);return 0;}
    if (encrypted_net && strstr(path, "/net.tscfg")) { memset(out,0,sizeof(*out)); return 0; }
    errno=ENOENT; return -1;
}
esp_err_t ts_cert_init(void)
{
    /* Public identity has moved before a reader needs the original private key. */
    assert(find("nvs","ts_pki","cert") && find("keys_nvs","ts_pki","privkey"));
    return ESP_OK;
}
esp_err_t ts_config_pack_init(void) { return ESP_OK; }
ts_config_pack_result_t ts_config_pack_load(const char *path, ts_config_pack_t **out)
{
    assert(strstr(path,"/net.tscfg")); ++pack_reads;
    if (fail_pack) return TS_CONFIG_PACK_ERR_DECRYPT;
    *out=calloc(1,sizeof(**out)); assert(*out);
    (*out)->content=strdup("{\"eth.ip\":\"192.0.2.8\"}");
    (*out)->content_len=strlen((*out)->content);
    return TS_CONFIG_PACK_OK;
}
void ts_config_pack_free(ts_config_pack_t *pack)
{
    if (pack) { free(pack->content); free(pack); }
}
FILE *legacy_file(const char *path, const char *mode)
{
    (void)mode; ++file_reads;
    const char *raw = strstr(path,"/led_color_correction.json") ? color_file : strstr(path,"/net.json") ? "{\"eth.ip\":\"192.0.2.9\"}" :
                      strstr(path,"/fan.json") ? "{\"speed\":99}" :
                      strstr(path,"/power_policy.json") ? "{\"low_voltage_threshold\":12.9,\"enabled\":false}" :
                      strstr(path,"/ui_widgets.json") ? "{\"widgets\":[],\"refresh_interval\":1234}" : NULL;
    if (!raw) { errno=ENOENT; return NULL; }
    FILE *f=tmpfile(); assert(f); assert(fputs(raw,f)>=0); rewind(f); return f;
}
static void fixture(void)
{
    for (size_t i=0;i<record_count;++i) free(records[i].data);
    memset(records,0,sizeof(records)); record_count=handle_count=0;
    file_reads=private_reads=erased=pack_reads=0; fail_auth_commit=encrypted_net=fail_pack=fail_color_write=false;
    color_file=NULL;color_sd=true;
    put("keys_nvs","ts_keystore","index",NVS_TYPE_STR,"synthetic-key",14);
    put("keys_nvs","ts_pki","privkey",NVS_TYPE_STR,"synthetic-private",18);
    put("keys_nvs","ts_security","key_sample",NVS_TYPE_BLOB,"synthetic-aes",14);
    put("keys_nvs","ts_pki","cert",NVS_TYPE_STR,"synthetic-public",17);
    put("keys_nvs","ts_auth","cred_root",NVS_TYPE_BLOB,"root-credential",15);
    put("keys_nvs","ts_ssh_cmd","c_0",NVS_TYPE_BLOB,"old-command",12);
    put("keys_nvs","ts_net","config",NVS_TYPE_BLOB,"{\"eth.ip\":\"192.0.2.1\"}",22);
    put("keys_nvs","ts_fan","config",NVS_TYPE_BLOB,"{\"speed\":40}",12);
    put("keys_nvs","ts_ui","widgets",NVS_TYPE_BLOB,"{\"widgets\":[]}",14);
    uint8_t pending=1U<<5;
    put("keys_nvs","ts_meta","pending_sync",NVS_TYPE_U8,&pending,sizeof(pending));
    int64_t signed_value=-123456789123LL;
    put("keys_nvs","phy","cal",NVS_TYPE_I64,&signed_value,sizeof(signed_value));
}
static record_t *system_record(const char *ns,const char *key) {return find("nvs",ns,key);}

bool ts_storage_sd_mounted(void) {return color_sd;}
esp_err_t ts_storage_write_file(const char *path,const void *data,size_t size)
{(void)path;(void)data;(void)size;return ESP_ERR_NOT_SUPPORTED;}

static void color_storage(void)
{
    fixture();
    color_file="{\"enabled\":true,\"gamma\":{\"enabled\":true,\"gamma\":1.9}}";
    assert(ts_led_cc_init()==ESP_OK);
    ts_led_cc_config_t config,defaults;
    assert(ts_led_cc_get_config(&config)==ESP_OK && fabsf(config.gamma.gamma-1.9f)<0.001f);
    unsigned reads=file_reads;
    color_file="{\"gamma\":{\"enabled\":true,\"gamma\":1.1}}";
    assert(ts_led_cc_set_gamma(true,2.4f)==ESP_OK);
    assert(ts_led_cc_deinit()==ESP_OK && ts_led_cc_init()==ESP_OK);
    assert(file_reads==reads && ts_led_cc_get_config(&config)==ESP_OK && fabsf(config.gamma.gamma-2.4f)<0.001f);
    fail_color_write=true;
    assert(ts_led_cc_set_gamma(true,3.1f)==ESP_FAIL);
    assert(ts_led_cc_get_config(&config)==ESP_OK && fabsf(config.gamma.gamma-2.4f)<0.001f);
    fail_color_write=false;
    assert(ts_led_cc_reset_config()==ESP_OK);
    assert(ts_led_cc_deinit()==ESP_OK && ts_led_cc_init()==ESP_OK && file_reads==reads);
    assert(ts_led_cc_get_config(&config)==ESP_OK && ts_led_cc_get_default_config(&defaults)==ESP_OK);
    assert(config.gamma.gamma==defaults.gamma.gamma && config.enabled==defaults.enabled);
    assert(ts_led_cc_deinit()==ESP_OK);
    fixture();color_sd=false;
    color_file="{\"gamma\":{\"enabled\":true,\"gamma\":1.7}}";
    assert(ts_led_cc_init()==ESP_OK && !file_reads);
    color_sd=true;
    assert(ts_led_cc_deinit()==ESP_OK && ts_led_cc_init()==ESP_OK && !file_reads);
    assert(ts_led_cc_load_from_sdcard(TS_LED_CC_SDCARD_JSON_PATH)==ESP_OK);
    assert(ts_led_cc_get_config(&config)==ESP_OK && fabsf(config.gamma.gamma-1.7f)<0.001f);
    color_file="{\"gamma\":{\"enabled\":true,\"gamma\":1.1}}";reads=file_reads;
    assert(ts_led_cc_deinit()==ESP_OK && ts_led_cc_init()==ESP_OK && file_reads==reads);
    assert(ts_led_cc_get_config(&config)==ESP_OK && fabsf(config.gamma.gamma-1.7f)<0.001f);
    assert(ts_led_cc_deinit()==ESP_OK);
    puts("PASS system LED storage: one-time SD import, NVS edits, truthful failure, persistent reset and explicit import");
}

/* Exercise the actual owning driver conversion functions with the same NVS
 * fixture used by the system migration; hardware/task functions are excluded. */
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0
static void xSemaphoreTake(SemaphoreHandle_t s,int wait) {(void)s;(void)wait;}
static void xSemaphoreGive(SemaphoreHandle_t s) {(void)s;}
#define TS_LOGD(...) ((void)0)
#define TS_LOGI(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#include "system_driver_persistence.inc"
static void driver_storage(void)
{
    fixture();assert(ts_config_migrate_system(true)==ESP_OK);
    assert(system_record("ts_power","config"));
    assert(load_config_values("config")==ESP_OK && fabsf(s_pp.config.low_voltage_threshold-12.9f)<0.001f && !s_pp.enabled_on_boot);
    assert(save_config_values(false)==ESP_OK && !system_record("ts_power","config"));
    s_pp.config.low_voltage_threshold=0;
    assert(load_config_values("policy")==ESP_OK && fabsf(s_pp.config.low_voltage_threshold-12.9f)<0.001f);
    fail_power_write=true;assert(save_config_values(false)==ESP_FAIL);fail_power_write=false;
    const char *json="{\"preferred_source\":\"variable\",\"bound_variables\":[{\"name\":\"agx_temp\",\"weight\":0.7}]}";
    put("nvs","ts_temp","config",NVS_TYPE_BLOB,json,strlen(json));
    memset(&s_state,0,sizeof(s_state));s_state.mutex=(void *)1;
    assert(load_preferred_source_from_nvs()==ESP_OK);
    assert(!system_record("ts_temp","config") && s_state.preferred_source==TS_TEMP_SOURCE_VARIABLE && s_state.bound_var_count==1);
    assert(!strcmp(s_state.bound_vars[0].name,"agx_temp") && fabsf(s_state.bound_vars[0].weight-0.7f)<0.001f);
    assert(save_preferred_source_to_nvs(TS_TEMP_SOURCE_SENSOR_LOCAL)==ESP_OK);
    unsigned reads=file_reads;memset(&s_state,0,sizeof(s_state));s_state.mutex=(void *)1;
    assert(load_preferred_source_from_nvs()==ESP_OK && s_state.preferred_source==TS_TEMP_SOURCE_SENSOR_LOCAL && s_state.bound_var_count==1 && reads==file_reads);
    s_state.bound_var_count=0;sync_bound_variable_compat();assert(save_bound_vars_to_nvs()==ESP_OK);
    memset(&s_state,0,sizeof(s_state));s_state.mutex=(void *)1;assert(load_preferred_source_from_nvs()==ESP_OK && !s_state.bound_var_count);
    puts("PASS system driver storage: selected power migration, actual save failure, one-time temperature conversion, NVS edits and cleared bindings survive restart");
}

int main(void)
{
    fixture();
    fail_auth_commit=true;
    assert(ts_config_migrate_system(true)==ESP_FAIL);
    assert(!system_record("ts_cfg_state","system_v") && !erased && !private_reads);
    assert(find("keys_nvs","ts_auth","cred_root")); /* Failed writes cannot destroy old data. */
    fail_auth_commit=false;
    assert(ts_config_migrate_system(true)==ESP_OK);
    assert(system_record("ts_auth","cred_root") && system_record("ts_pki","cert") && system_record("phy","cal"));
    assert(!system_record("ts_pki","privkey") && !system_record("ts_keystore","index") && !system_record("ts_ssh_cmd","c_0"));
    assert(find("keys_nvs","ts_pki","privkey") && find("keys_nvs","ts_ssh_cmd","c_0") && !private_reads);
    assert(find("keys_nvs","ts_security","key_sample") && !system_record("ts_security","key_sample"));
    record_t *net=system_record("ts_net","config"), *fan=system_record("ts_fan","config");
    assert(net && net->length == 22 && !memcmp(net->data,"{\"eth.ip\":\"192.0.2.9\"}",22));
    assert(fan && fan->length==12 && !memcmp(fan->data,"{\"speed\":40}",12));
    record_t *widgets=system_record("ts_ui","widgets");
    assert(widgets && widgets->length==strlen("{\"widgets\":[],\"refresh_interval\":1234}") &&
           !memcmp(widgets->data,"{\"widgets\":[],\"refresh_interval\":1234}",widgets->length));
    put("nvs","ts_auth","cred_root",NVS_TYPE_BLOB,"new",4);
    put("keys_nvs","ts_auth","cred_root",NVS_TYPE_BLOB,"stale",6);
    assert(ts_config_migrate_system(true)==ESP_OK);
    assert(!memcmp(system_record("ts_auth","cred_root")->data,"new",4)); /* Never replay after completion. */
    fixture();
    assert(ts_config_migrate_system(false)==ESP_OK && !file_reads && !private_reads);
    assert(system_record("ts_auth","cred_root") && system_record("ts_net","config"));
    fixture(); encrypted_net=fail_pack=true;
    assert(ts_config_migrate_system(true)==ESP_FAIL && pack_reads==1 && !erased);
    assert(!system_record("ts_cfg_state","system_v") && find("keys_nvs","ts_auth","cred_root"));
    fail_pack=false;
    assert(ts_config_migrate_system(true)==ESP_OK && pack_reads==2);
    assert(!memcmp(system_record("ts_net","config")->data,"{\"eth.ip\":\"192.0.2.8\"}",22));
    put("nvs","ts_ui","widgets",NVS_TYPE_BLOB,"{\"widgets\":[1]}",15);
    unsigned reads=file_reads;
    assert(ts_config_migrate_system(true)==ESP_OK && reads==file_reads && pack_reads==2);
    assert(!memcmp(system_record("ts_ui","widgets")->data,"{\"widgets\":[1]}",15));
    color_storage();
    driver_storage();
    fixture();
    for(size_t i=0;i<record_count;++i) free(records[i].data);
    puts("PASS system migration: key isolation, committed migration, SD priority and no replay");
    return 0;
}
