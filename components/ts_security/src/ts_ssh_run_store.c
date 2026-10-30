#include "ts_ssh_run_store.h"
#include "nvs.h"
#include <stdio.h>
#include <stddef.h>
#include <string.h>

/* Byte fields keep the on-flash format independent of C padding and enums. */
typedef struct {
    uint8_t version, kind, auth, reserved, port[2];
    char run_id[32], command_id[32], host_id[32], name[64];
    char rule[TS_AUTO_NAME_MAX_LEN], config_version[33];
    char host[64], username[32], keyid[32], variable[32];
    char locator[32], instance[96];
    char command_object_id[25];
} run_wire_t;

static void slot_key(unsigned slot, char key[8]) { snprintf(key,8,"r%02u",slot); }

static bool terminated(const char *text,size_t size) { return memchr(text,0,size)!=NULL; }
#define COPY_FIELD(to,from,field) memcpy((to)->field,(from)->field,sizeof((to)->field))

esp_err_t ts_ssh_run_load(unsigned slot,ts_ssh_run_record_t *record)
{
    if(slot>=TS_SSH_RUNS_MAX||!record)return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;esp_err_t ret=nvs_open("ssh_runs",NVS_READONLY,&handle);
    if(ret!=ESP_OK)return ret;
    char key[8];slot_key(slot,key);run_wire_t wire={0};size_t size=sizeof(wire);
    ret=nvs_get_blob(handle,key,&wire,&size);nvs_close(handle);
    if(ret!=ESP_OK)return ret;
    if(!((wire.version==1&&size==offsetof(run_wire_t,command_object_id))||
         (wire.version==2&&size==sizeof(wire)))||wire.kind>TS_SSH_RUN_LEGACY_PID||
       wire.auth>TS_SSH_HOST_AUTH_PASSWORD||wire.reserved||!(wire.port[0]|wire.port[1]))return ESP_ERR_INVALID_STATE;
#define CHECK(field) if(!terminated(wire.field,sizeof(wire.field)))return ESP_ERR_INVALID_STATE
    CHECK(run_id);CHECK(command_id);CHECK(host_id);CHECK(name);CHECK(rule);CHECK(config_version);
    CHECK(host);CHECK(username);CHECK(keyid);CHECK(variable);CHECK(locator);CHECK(instance);
    CHECK(command_object_id);
#undef CHECK
    if(strlen(wire.run_id)!=24||strspn(wire.run_id,"0123456789abcdef")!=24||
       !wire.locator[0]||strspn(wire.locator,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=strlen(wire.locator)||
       !wire.host[0]||!wire.username[0]||
       strspn(wire.instance,"0123456789abcdefABCDEF-:")!=strlen(wire.instance)||
       (wire.command_object_id[0]&&(strlen(wire.command_object_id)!=24||
        strspn(wire.command_object_id,"0123456789abcdef")!=24)))return ESP_ERR_INVALID_STATE;
    memset(record,0,sizeof(*record));
    COPY_FIELD(record,&wire,run_id);COPY_FIELD(record,&wire,command_id);COPY_FIELD(record,&wire,name);
    COPY_FIELD(record,&wire,rule);COPY_FIELD(record,&wire,config_version);COPY_FIELD(record,&wire,variable);
    COPY_FIELD(record,&wire,locator);COPY_FIELD(record,&wire,instance);
    COPY_FIELD(record,&wire,command_object_id);
    memcpy(record->host.id,wire.host_id,sizeof(wire.host_id));
    COPY_FIELD(&record->host,&wire,host);COPY_FIELD(&record->host,&wire,username);COPY_FIELD(&record->host,&wire,keyid);
    record->host.port=(uint16_t)wire.port[0]|((uint16_t)wire.port[1]<<8);
    record->host.auth_type=wire.auth;record->kind=wire.kind;return ESP_OK;
}

esp_err_t ts_ssh_run_save(unsigned slot,const ts_ssh_run_record_t *record)
{
    if(slot>=TS_SSH_RUNS_MAX||!record)return ESP_ERR_INVALID_ARG;
    run_wire_t wire={.version=2,.kind=record->kind,.auth=record->host.auth_type,
        .port={record->host.port&255,record->host.port>>8}};
    COPY_FIELD(&wire,record,run_id);COPY_FIELD(&wire,record,command_id);COPY_FIELD(&wire,record,name);
    COPY_FIELD(&wire,record,rule);COPY_FIELD(&wire,record,config_version);COPY_FIELD(&wire,record,variable);
    COPY_FIELD(&wire,record,locator);COPY_FIELD(&wire,record,instance);
    COPY_FIELD(&wire,record,command_object_id);
    memcpy(wire.host_id,record->host.id,sizeof(wire.host_id));
    COPY_FIELD(&wire,&record->host,host);COPY_FIELD(&wire,&record->host,username);COPY_FIELD(&wire,&record->host,keyid);
    nvs_handle_t handle;esp_err_t ret=nvs_open("ssh_runs",NVS_READWRITE,&handle);
    if(ret!=ESP_OK)return ret;
    char key[8];slot_key(slot,key);ret=nvs_set_blob(handle,key,&wire,sizeof(wire));
    if(ret==ESP_OK)ret=nvs_commit(handle);
    nvs_close(handle);return ret;
}

esp_err_t ts_ssh_run_remove(unsigned slot)
{
    if(slot>=TS_SSH_RUNS_MAX)return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;esp_err_t ret=nvs_open("ssh_runs",NVS_READWRITE,&handle);
    if(ret!=ESP_OK)return ret;
    char key[8];slot_key(slot,key);ret=nvs_erase_key(handle,key);
    if(ret==ESP_ERR_NVS_NOT_FOUND)ret=ESP_OK;
    if(ret==ESP_OK)ret=nvs_commit(handle);
    nvs_close(handle);return ret;
}
