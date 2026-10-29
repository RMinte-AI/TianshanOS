#include "ts_action_codec.h"
#include "ts_ssh_config_codec.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These production files link without managers, SSH, NVS or the variable store. */
int main(void)
{
    ts_action_template_t template={0}, decoded={0};
    strcpy(template.id,"display"); strcpy(template.name,"Display");
    template.enabled=true;template.action.type=TS_AUTO_ACT_LED;
    template.action.led.ctrl_type=TS_LED_CTRL_FILL;
    strcpy(template.action.led.device,"matrix");
    template.action.led.r=0x12;template.action.led.g=0x34;template.action.led.b=0x56;
    template.use_count=123;template.last_used_at=456;
    char *raw=ts_action_template_encode(&template);
    assert(raw && !strstr(raw,"use_count") && !strstr(raw,"last_used"));
    assert(ts_action_template_decode(raw,&decoded)==ESP_OK);
    assert(!strcmp(decoded.id,template.id) && decoded.action.led.r==0x12 && !decoded.use_count);
    free(raw);

    const char *colors[]={"red","rgb(255,0,0)","#FF0000","FF0000"};
    for(unsigned i=0;i<4;++i){
        char json[192];snprintf(json,sizeof(json),"{\"id\":\"red\",\"type\":\"led\",\"led\":{\"color\":\"%s\"}}",colors[i]);
        assert(ts_action_template_decode(json,&decoded)==ESP_OK);
        assert(decoded.action.led.r==255&&decoded.action.led.g==0&&decoded.action.led.b==0);
    }

    /* GPIO parameters must survive the API's decode/encode/save path. */
    assert(ts_action_template_decode("{\"id\":\"gpio\",\"type\":\"gpio\",\"gpio\":{\"pin\":48,\"level\":true,\"pulse_ms\":250}}",&template)==ESP_OK);
    raw=ts_action_template_encode(&template);assert(raw);
    assert(ts_action_template_decode(raw,&decoded)==ESP_OK&&decoded.action.gpio.pin==48&&decoded.action.gpio.level&&decoded.action.gpio.pulse_ms==250);
    free(raw);

    ts_auto_source_t source={0}, source_read={0};
    strcpy(source.id,"sensor");source.type=TS_AUTO_SRC_REST;source.enabled=true;
    strcpy(source.rest.url,"http://192.0.2.3/status");
    source.poll_interval_ms=1000;source.connected=true;
    raw=ts_source_config_encode(&source);
    assert(raw && !strstr(raw,"connected"));
    assert(ts_source_config_decode(raw,&source_read)==ESP_OK && !source_read.connected);
    assert(!strcmp(source_read.rest.url,source.rest.url) && source_read.poll_interval_ms==1000);
    free(raw);

    cJSON *json=cJSON_Parse("{\"id\":\"service\",\"host_id\":\"agx\",\"name\":\"Model\",\"command\":\"run-model\",\"serviceMode\":true,\"nohup\":true,\"varName\":\"model\",\"readyTimeout\":60,\"readyInterval\":3000}");
    ts_ssh_command_config_t command={0}, command_read={0};
    assert(ts_ssh_command_decode(json,&command)==ESP_OK);
    cJSON_Delete(json);
    assert(command.service_mode && command.ready_check_interval_ms==3000 && command.ready_timeout_sec==60);
    assert(!strcmp(command.var_name,"model"));
    command.last_exec_time=123;
    json=ts_ssh_command_encode(&command);
    assert(json && !cJSON_HasObjectItem(json,"last_exec_time"));
    assert(ts_ssh_command_decode(json,&command_read)==ESP_OK);
    assert(command_read.service_mode && !command_read.last_exec_time);
    cJSON_Delete(json);

    ts_ssh_host_config_t host={0}, host_read={0};
    strcpy(host.id,"agx");strcpy(host.host,"192.0.2.4");strcpy(host.username,"operator");
    host.port=22;host.auth_type=TS_SSH_HOST_AUTH_KEY;strcpy(host.keyid,"device-key");
    host.enabled=true;host.last_used_time=123;
    json=ts_ssh_host_encode(&host);
    assert(json && !cJSON_HasObjectItem(json,"last_used_time") && !cJSON_HasObjectItem(json,"privkey"));
    assert(ts_ssh_host_decode(json,&host_read)==ESP_OK);
    assert(!strcmp(host_read.keyid,"device-key") && host_read.port==22 && !host_read.last_used_time);
    cJSON_Delete(json);
    template.action.type=TS_AUTO_ACT_SSH_CMD;
    strcpy(template.action.ssh.host_ref,"agx");
    strcpy(template.action.ssh.command,"run-with-arguments");
    template.action.ssh.timeout_ms=45000;template.action.ssh.async=true;
    raw=ts_action_template_encode(&template);
    assert(raw && ts_action_template_decode(raw,&decoded)==ESP_OK);
    assert(!strcmp(decoded.action.ssh.command,"run-with-arguments") &&
           decoded.action.ssh.timeout_ms==45000 && decoded.action.ssh.async);free(raw);
    template.action.type=TS_AUTO_ACT_SET_VAR;
    strcpy(template.action.set_var.variable,"threshold");
    template.action.set_var.value.type=TS_AUTO_VAL_FLOAT;template.action.set_var.value.float_val=1.0;
    raw=ts_action_template_encode(&template);
    assert(raw && ts_action_template_decode(raw,&decoded)==ESP_OK);
    assert(decoded.action.set_var.value.type==TS_AUTO_VAL_FLOAT &&
           decoded.action.set_var.value.float_val==1.0);free(raw);
    source.type=TS_AUTO_SRC_BUILTIN;source.builtin.type=TS_AUTO_BUILTIN_ADC;
    source.builtin.adc.channel=3;source.builtin.adc.atten=12;
    raw=ts_source_config_encode(&source);
    assert(raw && ts_source_config_decode(raw,&source_read)==ESP_OK);
    assert(source_read.builtin.type==TS_AUTO_BUILTIN_ADC &&
           source_read.builtin.adc.channel==3 && source_read.builtin.adc.atten==12);free(raw);

    json=cJSON_Parse("{\"id\":\"bad\",\"timeout\":-1}");
    assert(ts_ssh_command_decode(json,&command)==ESP_ERR_INVALID_ARG);cJSON_Delete(json);
    json=cJSON_Parse("{\"id\":\"bad\",\"readyInterval\":65536}");
    assert(ts_ssh_command_decode(json,&command)==ESP_ERR_INVALID_ARG);cJSON_Delete(json);
    assert(ts_source_config_decode("{\"id\":\"bad\",\"type\":1,\"reconnect_ms\":-1}",&source_read)==ESP_ERR_INVALID_ARG);
    assert(ts_source_config_decode("{\"id\":\"bad\",\"type\":3,\"rest_url\":42}",&source_read)==ESP_ERR_INVALID_ARG);
    puts("PASS configuration codecs: pure conversion, SSH API aliases and no runtime backup");
    return 0;
}
