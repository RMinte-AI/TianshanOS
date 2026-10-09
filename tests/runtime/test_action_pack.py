#!/usr/bin/env python3
"""Actual action export payload -> SDK priority loader -> production directory loader."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[2];os.chdir(root)
p=root/'tests/runtime/test_input_repair.py';namespace={'__file__':str(p)}
exec(p.read_text().split('with tempfile.TemporaryDirectory',1)[0],namespace)
code=namespace['code'].split('int main(void) {',1)[0]
def extract(path,name):
 source=Path(path).read_text();m=re.search(r'^(?:static )?(?:\w+[ \t]+)+[ \t*]*'+name+r'\([^;]+?\)\s*\{',source,re.M);assert m,name
 return source[m.start():source.index('\n}',m.start())+2]+'\n'
code+=r'''
#include <dirent.h>
#define TS_CONFIG_PACK_OK 0
typedef int ts_config_pack_result_t;
typedef struct {const char *recipient_cert_pem;size_t recipient_cert_len;const char *description;} ts_config_pack_export_opts_t;
typedef struct {char *content;size_t content_len;} ts_config_pack_t;
static bool ts_config_pack_can_export(void){return true;}
static esp_err_t ts_cert_get_certificate(char *p,size_t *n){return ESP_FAIL;}
/* Cryptography is outside this test. Preserve the complete actual API payload. */
static int ts_config_pack_create(const char *id,const char *data,size_t n,const ts_config_pack_export_opts_t *o,char **out,size_t *len){*out=strdup(data);*len=n;return TS_CONFIG_PACK_OK;}
static int ts_config_pack_load(const char *path,ts_config_pack_t **out){
 FILE*f=fopen(path,"rb");if(!f)return ESP_FAIL;assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>=0);rewind(f);
 *out=calloc(1,sizeof(**out));(*out)->content=calloc(size+1,1);assert(fread((*out)->content,1,size,f)==(size_t)size);fclose(f);(*out)->content_len=size;return TS_CONFIG_PACK_OK;
}
static void ts_config_pack_free(ts_config_pack_t *p){free(p->content);free(p);}
'''
code+=extract('components/ts_api/src/ts_api_automation.c','api_automation_actions_export')
code+=extract('components/ts_config_pack/src/ts_config_pack.c','ts_config_pack_load_with_priority')
code+=extract('components/ts_automation/src/ts_action_manager.c','load_templates_from_dir')
code+=r'''
#define NVS_KEY_COUNT "count"
#define NVS_READONLY 0
static int fallbacks,backups;
esp_err_t ts_action_templates_load_from_file(const char *p){++fallbacks;return ESP_ERR_NOT_FOUND;}
static esp_err_t export_all_templates_to_dir(void){return ESP_OK;}
esp_err_t ts_action_templates_save(void){++backups;return ESP_OK;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *count){++fallbacks;*count=0;return ESP_ERR_NOT_FOUND;}
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *s,size_t *n){++fallbacks;return ESP_ERR_NOT_FOUND;}
'''
code+=extract('components/ts_automation/src/ts_action_manager.c','ts_action_templates_load')
code+=r'''
static void put(const char *p,const char *s){FILE*f=fopen(p,"wb");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));fclose(f);}
int main(void){
 pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;s_ctx->templates_mutex=&lock;mounted=true;
 cJSON *input=cJSON_Parse("{\"id\":\"fixture\",\"name\":\"Fixture\",\"type\":\"led\",\"enabled\":false,\"async\":true,\"led\":{\"device\":\"matrix\",\"ctrl_type\":\"filter\",\"filter\":\"wave\",\"filter_params\":{\"angle\":0,\"amplitude\":0}}}");
 ts_api_result_t r={0};assert(api_automation_actions_write(input,&r,false)==ESP_OK);ts_api_result_free(&r);ts_action_template_t original=s_ctx->templates[0];
 cJSON *params=cJSON_Parse("{\"id\":\"fixture\",\"recipient_cert\":\"synthetic certificate\"}");assert(api_automation_actions_export(params,&r)==ESP_OK&&r.code==TS_API_OK);
 const char *payload=cJSON_GetObjectItem(r.data,"tscfg")->valuestring;char pack[512],plain[512];snprintf(pack,sizeof(pack),"%s/fixture.tscfg",ACTIONS_SDCARD_DIR);snprintf(plain,sizeof(plain),"%s/fixture.json",ACTIONS_SDCARD_DIR);
 put(pack,payload);put(plain,"{\"id\":\"fixture\",\"type\":\"log\",\"name\":\"stale mirror\"}");
 s_ctx->template_count=0;assert(ts_action_templates_load()==ESP_OK&&s_ctx->template_count==1);same_filter(&original,&s_ctx->templates[0]);assert(!strcmp(s_ctx->templates[0].id,"fixture")&&!strcmp(s_ctx->templates[0].name,"Fixture"));assert(backups==1&&fallbacks==0);
 assert(!unlink(pack));char *legacy=template_to_json(&original);put(plain,legacy);s_ctx->template_count=0;assert(ts_action_templates_load()==ESP_OK&&s_ctx->template_count==1);same_filter(&original,&s_ctx->templates[0]);
 char pending[512],previous[512];snprintf(pending,sizeof(pending),"%s/.fixture.pending",ACTIONS_SDCARD_DIR);snprintf(previous,sizeof(previous),"%s/.fixture.json.previous",ACTIONS_SDCARD_DIR);
 assert(!rename(plain,previous));put(pending,"incomplete write");s_ctx->template_count=0;assert(ts_action_templates_load()==ESP_OK&&s_ctx->template_count==1);same_filter(&original,&s_ctx->templates[0]);assert(access(pending,F_OK)!=0&&access(previous,F_OK)!=0);
 /* Unresolved recovery must stop before trying older NVS/single-file sources. */
 put(previous,legacy);put(plain,"unreadable committed JSON");s_ctx->template_count=0;assert(ts_action_templates_load()!=ESP_OK&&s_ctx->template_count==0&&fallbacks==0);assert(access(previous,F_OK)==0);free(legacy);
 const char *bad[]={"[]","{}","{\"type\":\"action_template\"}","{\"type\":\"action_template\",\"template\":[]}","{\"type\":\"action_template\",\"template\":{\"id\":\"fixture\"}}","{\"id\":\"fixture\",\"type\":\"unknown\"}","{\"id\":\"fixture\",\"type\":\"led\"}","{\"id\":\"fixture\",\"type\":\"led\",\"led\":{\"filter_params\":{\"angle\":361}}}"};
 ts_action_template_t decoded;for(unsigned i=0;i<8;i++)assert(json_to_template(bad[i],&decoded)==ESP_ERR_INVALID_ARG);
 assert(!unlink(plain)&&!unlink(previous));
 /* IDs fit the declared 63-byte contract even when the extension pushes the
  * filename past the old arbitrary 60-byte directory-loader limit. */
 char limit_id[TS_AUTO_NAME_MAX_LEN];memset(limit_id,'x',sizeof(limit_id)-1);limit_id[sizeof(limit_id)-1]=0;
 const char *ids[]={limit_id,"中中中中中中中中中中中中中中中中中中中中中"};
 for(unsigned i=0;i<2;i++){
  assert(strlen(ids[i])==TS_AUTO_NAME_MAX_LEN-1);cJSON_ReplaceItemInObject(input,"id",cJSON_CreateString(ids[i]));cJSON_ReplaceItemInObject(params,"id",cJSON_CreateString(ids[i]));
  ts_api_result_free(&r);assert(api_automation_actions_write(input,&r,false)==ESP_OK);original=s_ctx->templates[0];ts_api_result_free(&r);assert(api_automation_actions_export(params,&r)==ESP_OK&&r.code==TS_API_OK);
  snprintf(pack,sizeof(pack),"%s/%s.tscfg",ACTIONS_SDCARD_DIR,ids[i]);snprintf(plain,sizeof(plain),"%s/%s.json",ACTIONS_SDCARD_DIR,ids[i]);put(pack,cJSON_GetObjectItem(r.data,"tscfg")->valuestring);
  s_ctx->template_count=0;assert(ts_action_templates_load()==ESP_OK&&s_ctx->template_count==1);assert(!strcmp(s_ctx->templates[0].id,ids[i]));same_filter(&original,&s_ctx->templates[0]);assert(!unlink(pack));
  legacy=template_to_json(&original);put(plain,legacy);free(legacy);s_ctx->template_count=0;assert(ts_action_templates_load()==ESP_OK&&s_ctx->template_count==1);assert(!strcmp(s_ctx->templates[0].id,ids[i]));assert(!unlink(plain));
 }
 ts_api_result_free(&r);cJSON_Delete(input);cJSON_Delete(params);
 puts("PASS actual action pack: complete export envelope -> SDK encrypted/plain priority -> real boot load entry and directory parser; identity/type/enabled/async/partial-zero parameters, flat legacy JSON, 63-byte ASCII/UTF-8 IDs, interrupted save recovery before load, unresolved recovery blocks stale fallback, malformed envelope rejection (crypto/NVS backup mocked)");
}
'''
# A short POSIX path keeps the fixture within the device's SD path budget on
# both macOS and Linux; macOS's default per-user temp path is much longer.
with tempfile.TemporaryDirectory(prefix='ts-action-pack-', dir='/tmp') as tmp:
 d=Path(tmp);(d/'actions').mkdir();f=d/'test.c';b=d/'test';f.write_text('#define ACTIONS_SDCARD_DIR "'+str(d/'actions')+'"\n'+code)
 idf=Path(os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf'))
 includes=['tests/runtime/stubs','tests/certificate/stubs','components/ts_api/include','components/ts_automation/include','components/ts_automation/src','components/ts_security/include','components/ts_led/include',str(idf/'components/json/cJSON')]
 subprocess.run(['cc','-std=gnu11','-g','-Wno-deprecated-declarations','-fsanitize=address,undefined',*['-I'+x for x in includes],'-DTS_ACTIONS_DIR="'+str(d/'actions')+'"',str(f),'components/ts_automation/src/ts_action_store.c','components/ts_automation/src/ts_action_filter.c','components/ts_automation/src/ts_rule_codec.c',str(idf/'components/json/cJSON/cJSON.c'),'-lpthread','-lm','-o',str(b)],check=True,env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'})
 subprocess.run([str(b)],check=True)
