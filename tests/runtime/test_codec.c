#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_rule_codec.h"
#include "ts_action_manager.h"
static int fail_after=-1, json_fail=-1, json_calls;
static void *json_alloc(size_t n){json_calls++;if(json_fail==0)return NULL;if(json_fail>0)--json_fail;return malloc(n);}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps){(void)caps;if(fail_after==0)return NULL;if(fail_after>0)--fail_after;return calloc(n,size);}
esp_err_t ts_action_template_get(const char *id,ts_action_template_t *tpl){(void)id;(void)tpl;return ESP_ERR_NOT_FOUND;}
int main(void){
 const char *fixture="{\"id\":\"model\",\"name\":\"Model\",\"enabled\":true,\"manual_trigger\":true,\"show_on_dashboard\":false,\"allow_manual_trigger\":false,\"conditions\":[{\"variable\":\"upstream.status\",\"operator\":\"eq\",\"value\":\"ready\"}],\"actions\":[{\"type\":\"ssh_cmd_ref\",\"cmd_id\":\"model-command\",\"template_id\":\"start-template\",\"repeat_mode\":\"count\",\"repeat_count\":3,\"repeat_interval_ms\":500,\"delay_ms\":123,\"condition\":{\"variable\":\"host.ready\",\"operator\":\"eq\",\"value\":true}}]}";
 cJSON *j=cJSON_Parse(fixture);ts_auto_rule_t a,b;assert(ts_rule_decode(j,&a)==ESP_OK);assert(a.manual_trigger&&a.conditions.count==1&&!a.show_on_dashboard&&!a.allow_manual_trigger&&a.presentation_fields==3);
 cJSON *out=ts_rule_encode(&a);assert(out);assert(ts_rule_decode(out,&b)==ESP_OK);assert(!memcmp(a.actions,b.actions,sizeof(*a.actions)));assert(!memcmp(a.conditions.conditions,b.conditions.conditions,sizeof(*a.conditions.conditions)));ts_rule_dispose(&b);cJSON_Delete(out);
 for(int type=0;type<=TS_AUTO_ACT_CLI;++type){memset(a.actions,0,sizeof(*a.actions));a.actions[0].type=type;a.actions[0].repeat_mode=TS_AUTO_REPEAT_COUNT;a.actions[0].repeat_count=3;a.actions[0].delay_ms=65535;
  if(type==TS_AUTO_ACT_LED){strcpy(a.actions[0].led.text,"synthetic text");a.actions[0].led.ctrl_type=TS_LED_CTRL_TEXT;a.actions[0].led.x=-123;a.actions[0].led.center=true;}
  if(type==TS_AUTO_ACT_SSH_CMD){strcpy(a.actions[0].ssh.host_ref,"test-host");strcpy(a.actions[0].ssh.command,"true");a.actions[0].ssh.timeout_ms=30000;a.actions[0].ssh.async=true;}
  if(type==TS_AUTO_ACT_SET_VAR){strcpy(a.actions[0].set_var.variable,"test");a.actions[0].set_var.value.type=TS_AUTO_VAL_STRING;strcpy(a.actions[0].set_var.value.str_val,"value");}
  out=ts_rule_encode(&a);assert(out);assert(ts_rule_decode(out,&b)==ESP_OK);assert(!memcmp(a.actions,b.actions,sizeof(*a.actions)));ts_rule_dispose(&b);cJSON_Delete(out);
 }
 /* Every JSON allocation in the encoder must fail cleanly. */
 cJSON_Hooks hooks={json_alloc,free};cJSON_InitHooks(&hooks);json_calls=0;out=ts_rule_encode(&a);assert(out);int allocations=json_calls;cJSON_Delete(out);
 for(int i=0;i<allocations;i++){json_fail=i;out=ts_rule_encode(&a);assert(!out);}json_fail=-1;cJSON_InitHooks(NULL);
 a.actions[0].type=TS_AUTO_ACT_SET_VAR;strcpy(a.actions[0].set_var.variable,"float");a.actions[0].set_var.value.type=TS_AUTO_VAL_FLOAT;a.actions[0].set_var.value.float_val=3.0;
 out=ts_rule_encode(&a);assert(out&&ts_rule_decode(out,&b)==ESP_OK);assert(b.actions[0].set_var.value.type==TS_AUTO_VAL_FLOAT);ts_rule_dispose(&b);cJSON_Delete(out);
 a.conditions.logic=99;assert(!ts_rule_encode(&a));a.conditions.logic=TS_AUTO_LOGIC_AND;
 ts_rule_dispose(&a);
 cJSON *unresolved=cJSON_Parse("{\"id\":\"missing\",\"name\":\"Missing\",\"actions\":[{\"template_id\":\"unavailable\"}]}");assert(ts_rule_decode(unresolved,&a)==ESP_OK);assert(a.actions[0].type==TS_AUTO_ACT_TEMPLATE_REF);out=ts_rule_encode(&a);assert(out);cJSON_Delete(out);cJSON_Delete(unresolved);ts_rule_dispose(&a);
 fail_after=0;assert(ts_rule_decode(j,&a)==ESP_ERR_NO_MEM);assert(!a.actions&&!a.conditions.conditions);fail_after=1;assert(ts_rule_decode(j,&a)==ESP_ERR_NO_MEM);fail_after=-1;
 cJSON_ReplaceItemInObject(cJSON_GetArrayItem(cJSON_GetObjectItem(j,"conditions"),0),"operator",cJSON_CreateString("matches"));assert(ts_rule_decode(j,&a)==ESP_ERR_INVALID_ARG);
 cJSON_Delete(j);assert(!ts_rule_id_valid("../model")&&ts_rule_id_valid("Qwen3.6-35B"));
 printf("PASS rule codec: nine action types, conditions/repeats/explicit false, allocation failures, unsupported operator; rule=%zu action=%zu\n",sizeof(ts_auto_rule_t),sizeof(ts_auto_action_t));
}
