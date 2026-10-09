#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include "ts_fan.h"
#include "ts_temp_source.h"
#include "cJSON.h"
typedef void *ts_pwm_handle_t;
typedef void *ts_gpio_handle_t;
typedef int nvs_handle_t;
#define FAN_NVS_NAMESPACE "fan_config"
#define FAN_CONFIG_VERSION 2
#ifndef CONFIG_TS_DRIVERS_FAN_TEMP_UPDATE_MS
#define CONFIG_TS_DRIVERS_FAN_TEMP_UPDATE_MS 1000
#endif
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define ESP_FAIL (-1)
#define ESP_ERR_NOT_FOUND 0x105
#define TS_LOGD(...) ((void)0)
#define TS_LOGI(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define portTICK_PERIOD_MS 1
#define TS_PWM_TIMER_AUTO 0
#define TS_GPIO_DIR_INPUT 0
#define TS_GPIO_PULL_UP 0
#define TS_GPIO_INTR_NEGEDGE 0
#define TS_GPIO_DRIVE_2 0
typedef struct {int frequency,resolution_bits,timer;bool invert;float initial_duty;} ts_pwm_config_t;
typedef struct {int direction,pull_mode,intr_type,drive,initial_level;bool invert;} ts_gpio_config_t;
#include "fan_types.inc"
static fan_instance_t s_fans[TS_FAN_MAX];
static bool s_auto_temp_enabled = true;
static ts_temp_data_t input;
static int64_t now_ms;
static float output[TS_FAN_MAX];
static esp_err_t hal_result;
static fan_nvs_config_t saved;
static bool have_saved, check;
static int64_t esp_timer_get_time(void) {return now_ms*1000;}
static uint32_t xTaskGetTickCount(void) {return now_ms;}
int16_t ts_temp_get_effective(ts_temp_data_t *data) {*data=input;return input.value;}
int16_t ts_temp_get_effective_nonblocking(ts_temp_data_t *data) {return ts_temp_get_effective(data);}
static esp_err_t ts_pwm_set_duty(ts_pwm_handle_t pwm,float duty) {
    if(hal_result==ESP_OK)output[(uintptr_t)pwm-1]=duty;
    return hal_result;
}
static ts_pwm_handle_t ts_pwm_create_raw(int pin,const char *name) {return (void *)(uintptr_t)(pin+1);}
static esp_err_t ts_pwm_configure(ts_pwm_handle_t pwm,const ts_pwm_config_t *cfg) {output[(uintptr_t)pwm-1]=cfg->initial_duty;return ESP_OK;}
static void ts_pwm_destroy(ts_pwm_handle_t pwm) {}
static ts_gpio_handle_t ts_gpio_create_raw(int pin,const char *name) {return NULL;}
static void ts_gpio_configure(ts_gpio_handle_t g,const ts_gpio_config_t *cfg) {}
static void ts_gpio_set_isr_callback(ts_gpio_handle_t g,void (*fn)(ts_gpio_handle_t,void*),void *arg) {}
static void ts_gpio_intr_enable(ts_gpio_handle_t g) {}
static void tach_isr_callback(ts_gpio_handle_t g,void *arg) {}
static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *nvs) {*nvs=1;return ESP_OK;}
static esp_err_t nvs_set_blob(nvs_handle_t nvs,const char *key,const void *data,size_t len) {saved=*(const fan_nvs_config_t *)data;have_saved=true;return ESP_OK;}
static esp_err_t nvs_get_blob(nvs_handle_t nvs,const char *key,void *data,size_t *len) {
    if(!have_saved||strcmp(key,"fan0"))return ESP_ERR_NOT_FOUND;
    memcpy(data,&saved,sizeof(saved));return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t nvs) {return ESP_OK;}
static void nvs_close(nvs_handle_t nvs) {}
static esp_err_t update_pwm(fan_instance_t *f,uint8_t duty);
#include "fan_core.inc"
#define TS_API_ERR_INVALID_ARG 1
#define TS_API_ERR_HARDWARE 9
typedef struct {int code;cJSON *data;const char *message;} ts_api_result_t;
static void ts_api_result_error(ts_api_result_t *r,int code,const char *message) {r->code=code;}
static void ts_api_result_ok(ts_api_result_t *r,cJSON *data) {r->code=0;r->data=data;}
#include "api_core.inc"
#include "device_core.inc"
#define TS_API_OK 0
static char console_text[4096];
static void ts_console_printf(const char *fmt,...) {
    va_list args;va_start(args,fmt);size_t len=strlen(console_text);
    vsnprintf(console_text+len,sizeof(console_text)-len,fmt,args);va_end(args);
}
#define ts_console_error ts_console_printf
static const char *esp_err_to_name(esp_err_t ret){return "mock error";}
static esp_err_t ts_api_call(const char *name,const cJSON *params,ts_api_result_t *result){return ESP_FAIL;}
static void ts_api_result_free(ts_api_result_t *result){}
#include "console_core.inc"

static void setup(int duty) {
    memset(s_fans,0,sizeof(s_fans));now_ms=100000;hal_result=ESP_OK;have_saved=false;
    fan_instance_t *f=&s_fans[0];f->initialized=f->enabled=true;f->pwm=(void *)1;f->mode=TS_FAN_MODE_AUTO;
    f->current_duty=duty;output[0]=duty;f->temperature=488;
    f->config.gpio_pwm=0;f->config.gpio_tach=-1;f->config.min_duty=10;f->config.max_duty=100;
    f->config.hysteresis=20;f->config.min_interval=5000;
    const ts_fan_curve_point_t curve[]={{300,0},{400,10},{500,15},{550,25},{600,35},{650,40},{700,45},{800,50},{900,60}};
    f->config.curve_points=9;memcpy(f->config.curve,curve,sizeof(curve));reset_adaptive_auto_state(f);
    update_pwm(f,duty); /* Establish successful output through the real writer. */
    input=(ts_temp_data_t){.value=488,.guard_value=488,.valid=true,.guard_valid=true,
        .source=TS_TEMP_SOURCE_VARIABLE,.timestamp_ms=now_ms,.bound_valid_count=1,.bound_total_count=1};
}
static void tick(int64_t time,int temperature,bool fresh) {
    now_ms=time;input.value=input.guard_value=temperature;
    if(fresh)input.timestamp_ms=now_ms;
    fan_update_callback(NULL);
}
static void record(const char *name,int result) {
    ts_fan_status_t status={0};assert(ts_fan_get_status(0,&status)==ESP_OK);
    cJSON *obj=cJSON_CreateObject();cJSON_AddStringToObject(obj,"case",name);
    cJSON_AddNumberToObject(obj,"result",result);cJSON_AddNumberToObject(obj,"hal_output",output[0]);
    cJSON_AddNumberToObject(obj,"credit",s_fans[0].auto_fall_credit);
    cJSON_AddItemToObject(obj,"fan",status_to_json(0,&status));
    ts_api_result_t device={0};assert(api_device_fan_status(NULL,&device)==ESP_OK);
    cJSON *device_fan=cJSON_GetArrayItem(cJSON_GetObjectItem(device.data,"fans"),0);
#if REPAIRED
    if(check) {
        assert(cJSON_IsTrue(cJSON_GetObjectItem(device_fan,"duty_valid"))==status.duty_valid);
        if(status.duty_valid)assert(cJSON_GetObjectItem(device_fan,"duty")->valueint==status.duty_percent);
        else assert(cJSON_IsNull(cJSON_GetObjectItem(device_fan,"duty")));
    }
#endif
    console_text[0]=0;assert(do_fan_status(0,false)==0);
#if REPAIRED
    if(check) {
        if(!status.duty_valid)assert(strstr(console_text,"Duty:         Unknown"));
        else {char expected[64];snprintf(expected,sizeof(expected),"Duty:         %d%%",status.duty_percent);assert(strstr(console_text,expected));}
    }
#endif
    cJSON_AddStringToObject(obj,"console",console_text);
    console_text[0]=0;assert(do_fan_status(-1,false)==0);
#if REPAIRED
    if(check&&!status.duty_valid)assert(strstr(console_text,"--"));
#endif
    cJSON_AddItemToObject(obj,"device_fan",cJSON_Duplicate(device_fan,true));cJSON_Delete(device.data);
    char *str=cJSON_PrintUnformatted(obj);
    puts(str);free(str);cJSON_Delete(obj);
}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"cadence")) {
        check=true;setup(37);input.source=TS_TEMP_SOURCE_MANUAL;tick(100000,950,true);tick(101000,890,true);
        int period=CONFIG_TS_DRIVERS_FAN_TEMP_UPDATE_MS;
        if(period<=10000) {
            for(int elapsed=period;elapsed<=30000;elapsed+=period) {
                tick(101000+elapsed,890,elapsed%5000==0||period>=5000);
                if(elapsed<30000)assert(s_fans[0].guard_active);
            }
            assert(!s_fans[0].guard_active);
        } else {
            tick(101000+period,890,false);
            assert(s_fans[0].guard_active&&s_fans[0].guard_release_since_ms==101000+period);
        }
        record("R1_configured_cadence",0);return 0;
    }
    check=argc>1&&!strcmp(argv[1],"check");
    setup(37);tick(100000,950,true);tick(101000,890,true);tick(161000,890,true);
    record("R1_gap60",0);if(check)assert(s_fans[0].guard_active&&output[0]==100);
    /* Cached reads are valid observations, not fictitious new sensor reports. */
    setup(37);tick(100000,950,true);tick(101000,890,true);
    for(int t=102;t<=131;t++) {tick(t*1000,890,t%5==1);if(check&&t<131)assert(s_fans[0].guard_active);}
    record("R1_cached5s_release30",0);if(check)assert(!s_fans[0].guard_active);
    for(int kind=0;kind<5;kind++) {
        setup(37);tick(100000,950,true);tick(101000,890,true);
        for(int t=102;t<=120;t++)tick(t*1000,890,true);
        if(kind==0)input.source=TS_TEMP_SOURCE_AGX_AUTO;
#if REPAIRED
        if(kind==1)input.identity_revision++;
#endif
        int64_t resume=kind==2?119000:kind==3?120000:kind==4?123001:121000;
        tick(resume,890,true);
        record(kind==0?"R1_source_change":kind==1?"R1_binding_change":kind==2?"R1_clock_rollback":kind==3?"R1_duplicate_clock":"R1_missed_tick",0);
#if REPAIRED
        if(check)assert(s_fans[0].guard_active&&s_fans[0].guard_release_since_ms==resume);
#endif
        for(int t=1;t<=30;t++){tick(resume+t*1000,890,t%5==0);if(check&&t<30)assert(s_fans[0].guard_active);}
        if(check)assert(!s_fans[0].guard_active);
    }
    setup(37);tick(100000,950,true);tick(101000,890,true);input.valid=false;
    tick(120000,890,false);record("R1_stale25",0);if(check)assert(output[0]==25&&s_fans[0].temp_stale);
    input.valid=true;tick(121000,950,true);record("R1_stale_reheat_guard100",0);
    if(check)assert(output[0]==100&&s_fans[0].guard_active);
    setup(37);s_fans[0].mode=TS_FAN_MODE_MANUAL;assert(ts_fan_set_duty(0,37)==ESP_OK);
    hal_result=ESP_ERR_INVALID_STATE;int ret=ts_fan_set_duty(0,70);record("R2_manual_fail",ret);
    if(check)assert(s_fans[0].current_duty==37&&s_fans[0].target_duty==70&&output[0]==37);
    tick(101000,488,true);record("R2_manual_tick_fail",0);if(check)assert(s_fans[0].current_duty==37);
    ts_fan_save_full_config(0);if(check)assert(saved.duty==70);
    hal_result=ESP_OK;tick(102000,488,true);record("R2_manual_recovered",0);
    if(check)assert(s_fans[0].current_duty==70&&!s_fans[0].fault);
    setup(37);hal_result=ESP_ERR_INVALID_STATE;ret=ts_fan_emergency_full();record("R2_emergency_fail",ret);
    if(check)assert(ret==ESP_ERR_INVALID_STATE&&s_fans[0].current_duty==37&&s_fans[0].target_duty==100);
    setup(37);ts_fan_set_duty(0,70);ts_fan_save_full_config(0);ts_fan_set_duty(0,37);
    hal_result=ESP_ERR_INVALID_STATE;ret=ts_fan_load_config();record("R2_restore_fail",ret);
    if(check)assert(ret==ESP_ERR_INVALID_STATE&&s_fans[0].current_duty==37&&s_fans[0].target_duty==70);
    setup(37);ts_fan_config_t cfg=s_fans[0].config;cfg.min_duty=20;
    ret=ts_fan_configure(0,&cfg);record("R2_configure",ret);if(check)assert(output[0]==20&&s_fans[0].current_duty==20);
    setup(37);hal_result=ESP_ERR_INVALID_STATE;ret=ts_fan_configure(0,&cfg);record("R2_configure_fail",ret);
#if REPAIRED
    if(check)assert(ret==ESP_ERR_INVALID_STATE&&!s_fans[0].duty_valid);
#endif
    setup(37);ts_fan_enable(0,false);
    cJSON *params=cJSON_CreateObject();cJSON_AddNumberToObject(params,"id",0);cJSON_AddNumberToObject(params,"duty",70);
    ts_api_result_t api={0};ret=api_fan_set(params,&api);record("R2_api_disabled",ret);
    if(check){assert(ret==ESP_OK&&api.code==0);assert(cJSON_GetObjectItem(api.data,"duty")->valueint==0);assert(cJSON_GetObjectItem(api.data,"target_duty")->valueint==70);}
    cJSON_Delete(api.data);cJSON_Delete(params);
    setup(37);ts_fan_set_duty(0,70);record("R2_manual_success",0);
    if(check)assert(s_fans[0].current_duty==70&&output[0]==70);
    ts_fan_enable(0,false);if(check)assert(s_fans[0].current_duty==0&&s_fans[0].target_duty==70);
    ts_fan_save_config();if(check)assert(saved.duty==70);
    ts_fan_enable(0,true);tick(101000,488,true);record("R2_reenable",0);if(check)assert(output[0]==70);
    setup(37);s_fans[0].config.invert_pwm=true;ts_fan_set_duty(0,70);
    record("R2_inverted",0);if(check)assert(output[0]==30&&s_fans[0].current_duty==70);
    setup(37);hal_result=ESP_ERR_INVALID_STATE;ret=ts_fan_set_mode(0,TS_FAN_MODE_OFF);record("R2_off_fail",ret);
    if(check)assert(ret!=ESP_OK&&s_fans[0].current_duty==37&&s_fans[0].target_duty==0);
    hal_result=ESP_OK;tick(101000,488,true);if(check)assert(s_fans[0].current_duty==0&&!s_fans[0].fault);
    setup(37);s_fans[0].mode=TS_FAN_MODE_CURVE;tick(101000,488,true);record("R2_curve_success",0);
    if(check)assert(s_fans[0].current_duty==output[0]);
    setup(87);s_fans[0].last_auto_update_ms=100000;hal_result=ESP_ERR_INVALID_STATE;
    tick(140000,488,true);record("R3_failed_reach",0);
    if(check)assert(s_fans[0].current_duty==87&&s_fans[0].auto_fall_credit==0);
    hal_result=ESP_OK;tick(141000,488,true);record("R3_failed_retry",0);
    if(check)assert(s_fans[0].current_duty==85&&s_fans[0].auto_fall_credit<1);
    setup(15);s_fans[0].auto_fall_credit=0.9f;s_fans[0].last_auto_update_ms=100000;
    tick(101000,488,true);record("R3_fraction_reached",0);
    if(check)assert(s_fans[0].current_duty==14&&s_fans[0].auto_fall_credit==0);
    tick(102000,400,true);if(check)assert(s_fans[0].current_duty==14);
    tick(103000,900,true);record("R3_reheat",0);
    if(check)assert(s_fans[0].current_duty>=26&&s_fans[0].auto_fall_credit==0);
    setup(87);s_fans[0].last_auto_update_ms=100000;
    tick(105000,488,true);record("R3_long_not_reached",0);
    if(check)assert(s_fans[0].current_duty==75&&s_fans[0].auto_fall_credit<1);
    tick(106000,400,true);record("R3_long_next",0);if(check)assert(s_fans[0].current_duty==73&&s_fans[0].auto_fall_credit<1);
    setup(14);s_fans[0].auto_fall_credit=0.9f;tick(101000,488,true);record("R3_equal_reset",0);
    if(check)assert(s_fans[0].current_duty==14&&s_fans[0].auto_fall_credit==0);
    setup(87);s_fans[0].auto_fall_credit=500;ts_fan_set_mode(0,TS_FAN_MODE_MANUAL);ts_fan_set_mode(0,TS_FAN_MODE_AUTO);
    record("R3_mode_reset",0);if(check)assert(s_fans[0].current_duty==85&&s_fans[0].auto_fall_credit<1);
    setup(87);s_fans[0].auto_fall_credit=500;input.valid=false;tick(101000,488,false);record("R3_stale_reset",0);
    if(check)assert(s_fans[0].current_duty==25&&s_fans[0].auto_fall_credit==0);
    input.valid=true;tick(102000,950,true);record("R3_stale_guard",0);
    if(check)assert(s_fans[0].current_duty==100&&s_fans[0].auto_fall_credit==0);
    const int intervals[]={40,200,300};
    for(unsigned index=0;index<sizeof(intervals)/sizeof(intervals[0]);index++) {
        int interval=intervals[index];
        char label[40];
        setup(87);s_fans[0].last_auto_update_ms=100000;
        tick(100000+interval*1000,488,true);snprintf(label,sizeof(label),"R3_dt%d_reached",interval);record(label,0);
        if(check)assert(s_fans[0].current_duty==14&&s_fans[0].auto_fall_credit==0);
        tick(101000+interval*1000,400,true);snprintf(label,sizeof(label),"R3_dt%d_next",interval);record(label,0);
        if(check)assert(s_fans[0].current_duty==14&&s_fans[0].auto_fall_credit>0&&s_fans[0].auto_fall_credit<1);
    }
    return 0;
}
