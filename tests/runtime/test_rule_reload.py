"""Round trip production store/codec and extracted current directory loader."""
from pathlib import Path
import re,subprocess,os
root=Path.cwd();build=Path('/tmp/tianshan-runtime-tests')
s=Path('tests/runtime/test_store.c').read_text().split('int main(void)')[0]
s=s.replace('#include "../../components/ts_automation/src/ts_rule_store.c"',f'#include "{root}/components/ts_automation/src/ts_rule_store.c"')
s=re.sub(r'#define DIR_RULES "[^"]+"','#define DIR_RULES "/tmp/tianshan-runtime-tests/reload-rules"',s)
s+='''
#include <dirent.h>
#define RULES_SDCARD_DIR DIR_RULES
static struct {int capacity;} s_rule_ctx={2};
static esp_err_t load_one(const char *path,ts_auto_rule_t*r,bool*encrypted){char buf[8192];FILE*f=fopen(path,"r");assert(f);size_t n=fread(buf,1,sizeof(buf)-1,f);buf[n]=0;fclose(f);cJSON*j=cJSON_Parse(buf);esp_err_t e=ts_rule_decode(j,r);cJSON_Delete(j);*encrypted=false;return e;}
'''
engine=Path('components/ts_automation/src/ts_rule_engine.c').read_text();m=re.search(r'^static esp_err_t load_directory\([^;]+?\)\s*\{',engine,re.M);assert m
s+=engine[m.start():engine.index('\n}',m.start())+2]
s+='''
int main(void){
 mkdir(DIR_RULES,0700);
 const char *ids[]={"model",".model","..model",".json","a.pending"};
 FILE *tmp=fopen(DIR_RULES "/.system.pending","w");assert(tmp);fputs("not a rule",tmp);fclose(tmp);
 for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++){
  clear_all();ts_auto_rule_t r=rule("roundtrip");strcpy(r.id,ids[i]);assert(ts_rule_id_valid(r.id));
  ts_rule_commit_t result;assert(ts_rule_store_commit(NULL,0,&r,r.id,1,&result)==ESP_OK&&result.durable);
  ts_auto_rule_t loaded[2]={0};int n=0;bool readonly[2];
  assert(load_directory(loaded,&n,readonly)==ESP_OK&&n==1&&!readonly[0]&&!strcmp(loaded[0].id,ids[i]));
  ts_rule_dispose(&loaded[0]);char path[160];snprintf(path,sizeof(path),DIR_RULES "/%s.json",ids[i]);unlink(path);
 }
 clear_all();unlink(DIR_RULES "/.system.pending");
 puts("PASS actual store -> current loader: normal/dot IDs, temporary files ignored, reload retains saved rule");
}
'''
(build/'rule_reload.c').write_text(s)
idf=os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf')
env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined','-isysroot','/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk','-Wno-deprecated-declarations','-Itests/runtime/stubs','-Itests/certificate/stubs','-Icomponents/ts_automation/include','-I'+idf+'/components/json/cJSON',str(build/'rule_reload.c'),'components/ts_automation/src/ts_rule_codec.c',idf+'/components/json/cJSON/cJSON.c','-lpthread','-lm','-o',str(build/'rule_reload')],check=True,env=env)
subprocess.run([str(build/'rule_reload')],check=True,env=env)
