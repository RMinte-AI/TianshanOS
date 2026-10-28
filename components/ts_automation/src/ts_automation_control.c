#include "ts_automation_control.h"
#include "nvs.h"

esp_err_t ts_auto_control_load(ts_auto_control_t *control)
{
    *control = (ts_auto_control_t){.source=TS_AUTO_CONFIG_SD};
    nvs_handle_t handle;
    esp_err_t ret=nvs_open("ts_cfg_state",NVS_READONLY,&handle);
    if(ret==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    if(ret!=ESP_OK)return ret;
    uint8_t bytes[4];size_t size=sizeof(bytes);
    ret=nvs_get_blob(handle,"auto_control",bytes,&size);
    nvs_close(handle);
    if(ret==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    if(ret!=ESP_OK)return ret;
    if(size!=sizeof(bytes)||bytes[0]!=1||bytes[1]>TS_AUTO_CONFIG_INTERNAL||bytes[2]>1||bytes[3])
        return ESP_ERR_INVALID_STATE;
    control->source=bytes[1];control->paused=bytes[2];return ESP_OK;
}

esp_err_t ts_auto_control_save(const ts_auto_control_t *control)
{
    uint8_t bytes[]={1,control->source,control->paused,0};
    nvs_handle_t handle;
    esp_err_t ret=nvs_open("ts_cfg_state",NVS_READWRITE,&handle);
    if(ret!=ESP_OK)return ret;
    ret=nvs_set_blob(handle,"auto_control",bytes,sizeof(bytes));
    if(ret==ESP_OK)ret=nvs_commit(handle);
    nvs_close(handle);return ret;
}
