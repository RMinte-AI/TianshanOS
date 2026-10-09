#!/usr/bin/env python3
"""A failed allocation must never turn a complete template into a partial save."""
from pathlib import Path
import os, subprocess, tempfile

root=Path(__file__).resolve().parents[2]
os.chdir(root)
source=root/'tests/runtime/test_input_repair.py'
namespace={'__file__':str(source)}
exec(source.read_text().split('with tempfile.TemporaryDirectory',1)[0],namespace)
code=namespace['code'].split('int main(void) {',1)[0]+r'''
static unsigned allocation, fail_at;
static void *allocate_json(size_t n){return ++allocation==fail_at?NULL:malloc(n);}
int main(void){
 cJSON_Hooks hooks={allocate_json,free};unsigned cases=0,partial=0,edits=0;
 pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;s_ctx->templates_mutex=&lock;s_ctx->template_count=1;
 const ts_auto_action_type_t kinds[]={TS_AUTO_ACT_CLI,TS_AUTO_ACT_LED,TS_AUTO_ACT_SSH_CMD_REF,TS_AUTO_ACT_LOG,TS_AUTO_ACT_SET_VAR,TS_AUTO_ACT_WEBHOOK};
 for(unsigned kind=0;kind<sizeof(kinds)/sizeof(kinds[0]);kind++){
  ts_action_template_t tpl={0};strcpy(tpl.id,"fixture");strcpy(tpl.name,"Complete name");strcpy(tpl.description,"Complete description");tpl.enabled=true;tpl.async=true;tpl.action.type=kinds[kind];
  switch(tpl.action.type){
   case TS_AUTO_ACT_CLI:strcpy(tpl.action.cli.command,"status");strcpy(tpl.action.cli.var_name,"result");tpl.action.cli.timeout_ms=5000;break;
   case TS_AUTO_ACT_LED:
    strcpy(tpl.action.led.device,"matrix");tpl.action.led.ctrl_type=TS_LED_CTRL_FILTER;strcpy(tpl.action.led.filter,"wave");
    strcpy(tpl.action.led.effect,"rainbow");strcpy(tpl.action.led.text,"Text");strcpy(tpl.action.led.font,"Font");strcpy(tpl.action.led.image_path,"/image");strcpy(tpl.action.led.qr_text,"QR");tpl.action.led.qr_ecc='M';strcpy(tpl.action.led.scroll,"left");strcpy(tpl.action.led.align,"center");
    {cJSON *params=cJSON_Parse("{\"angle\":0,\"speed\":25}");assert(ts_action_filter_decode(params,&tpl.action.led.filter_params)==ESP_OK);cJSON_Delete(params);}break;
   case TS_AUTO_ACT_SSH_CMD_REF:strcpy(tpl.action.ssh_ref.cmd_id,"command");break;
   case TS_AUTO_ACT_LOG:strcpy(tpl.action.log.message,"Message");break;
   case TS_AUTO_ACT_SET_VAR:strcpy(tpl.action.set_var.variable,"value");tpl.action.set_var.value.type=TS_AUTO_VAL_STRING;strcpy(tpl.action.set_var.value.str_val,"String");break;
   case TS_AUTO_ACT_WEBHOOK:strcpy(tpl.action.webhook.url,"http://fixture");strcpy(tpl.action.webhook.method,"POST");strcpy(tpl.action.webhook.body_template,"Body");break;
   default:assert(0);
  }
  allocation=fail_at=0;cJSON_InitHooks(&hooks);char *complete=template_to_json(&tpl);assert(complete);unsigned count=allocation;cJSON_InitHooks(NULL);
  for(unsigned point=1;point<=count+1;point++){
   allocation=0;fail_at=point;cJSON_InitHooks(&hooks);char *actual=template_to_json(&tpl);cJSON_InitHooks(NULL);
   if(actual&&strcmp(actual,complete)){if(partial++<3)fprintf(stderr,"partial template at type=%d allocation=%u: %s\n",tpl.action.type,point,actual);}
   free(actual);++cases;
  }
  ts_action_template_t next=tpl;strcpy(next.name,"Updated name");
  for(unsigned point=1;point<=count+1;point++){
   s_ctx->templates[0]=tpl;free(nv_saved);nv_saved=strdup(complete);
   allocation=0;fail_at=point;cJSON_InitHooks(&hooks);esp_err_t ret=template_update_impl("fixture",&next);cJSON_InitHooks(NULL);
   if(ret!=ESP_OK){assert(ret==ESP_ERR_NO_MEM);assert(!memcmp(&s_ctx->templates[0],&tpl,sizeof(tpl)));assert(!strcmp(nv_saved,complete));}
   else{assert(!strcmp(s_ctx->templates[0].name,"Updated name"));ts_action_template_t decoded;assert(json_to_template(nv_saved,&decoded)==ESP_OK&&!strcmp(decoded.name,"Updated name"));}
   ++edits;
  }
  free(complete);
 }
 assert(partial==0);
 free(nv_saved);
 printf("PASS production template serializer: %u serialization and %u update allocation boundaries across six stored action types; complete JSON or failure, failed edits preserve active and stored versions\n",cases,edits);
}
'''
with tempfile.TemporaryDirectory(prefix='ts-action-serialization-') as tmp:
 d=Path(tmp);(d/'actions').mkdir();f=d/'test.c';b=d/'test'
 f.write_text('#define ACTIONS_SDCARD_DIR "'+str(d/'actions')+'"\n'+code)
 idf=Path(os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf'))
 includes=['tests/runtime/stubs','tests/certificate/stubs','components/ts_api/include','components/ts_automation/include','components/ts_automation/src','components/ts_security/include','components/ts_led/include',str(idf/'components/json/cJSON')]
 env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
 subprocess.run(['cc','-std=gnu11','-g','-Wno-deprecated-declarations','-fsanitize=address,undefined',*['-I'+x for x in includes],'-DTS_ACTIONS_DIR="'+str(d/'actions')+'"',str(f),'components/ts_automation/src/ts_action_store.c','components/ts_automation/src/ts_action_filter.c','components/ts_automation/src/ts_rule_codec.c',str(idf/'components/json/cJSON/cJSON.c'),'-lpthread','-lm','-o',str(b)],check=True,env=env)
 subprocess.run([str(b)],check=True,env=env)
