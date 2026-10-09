#define main crypto_checks
#include "test_crypto.c"
#undef main
#include <setjmp.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "ts_rule_codec.h"
#include "ts_action_manager.h"
#include "ts_ssh_commands_config.h"
#include "ts_ssh_hosts_config.h"
#include "nvs.h"
static bool templates_ready=true, template_exists=true, template_enabled=true;
static bool command_exists,command_enabled=true,host_exists,internal_host;
static esp_err_t command_state=ESP_OK,host_state=ESP_OK;
static ts_auto_action_t template_action={.type=TS_AUTO_ACT_LOG};
esp_err_t ts_action_template_get(const char *id,ts_action_template_t *t){
 if(!template_exists||strcmp(id,"template"))return ESP_ERR_NOT_FOUND;
 memset(t,0,sizeof *t);strcpy(t->id,id);t->enabled=template_enabled;t->action=template_action;return ESP_OK;
}
bool ts_action_templates_ready(void){return templates_ready;}
bool ts_ssh_commands_config_is_initialized(void){return true;}
bool ts_ssh_hosts_config_is_initialized(void){return true;}
esp_err_t ts_ssh_commands_config_load_state(void){return command_state;}
esp_err_t ts_ssh_hosts_config_load_state(void){return host_state;}
esp_err_t ts_ssh_commands_config_get(const char *id,ts_ssh_command_config_t *c){if(!command_exists||strcmp(id,"command"))return ESP_ERR_NOT_FOUND;memset(c,0,sizeof *c);strcpy(c->id,id);strcpy(c->host_id,"host");c->enabled=command_enabled;return ESP_OK;}
esp_err_t ts_ssh_hosts_config_get(const char *id,ts_ssh_host_config_t *h){if(!host_exists||strcmp(id,"host"))return ESP_ERR_NOT_FOUND;memset(h,0,sizeof *h);strcpy(h->id,id);strcpy(h->host,"192.0.2.1");strcpy(h->username,"test");h->port=22;h->enabled=false;return ESP_OK;}
esp_err_t ts_action_get_ssh_host_ex(const char *id,ts_action_ssh_host_t *h,bool *internal){*internal=internal_host;if(!host_exists||strcmp(id,"host"))return ESP_ERR_NOT_FOUND;memset(h,0,sizeof *h);strcpy(h->id,id);strcpy(h->host,"192.0.2.1");strcpy(h->username,"test");h->port=22;return ESP_OK;}
esp_err_t ts_action_get_ssh_host(const char *id,ts_action_ssh_host_t *h){bool internal;return ts_action_get_ssh_host_ex(id,h,&internal);}
typedef struct {char key[24];void *data;size_t size;} value_t;
static value_t values[160];
static unsigned operation, fault, mode;
static bool fail_selector_read;
static jmp_buf crash;
static bool before(void){
 ++operation;
 if(operation!=fault)return false;
 if(mode==3)longjmp(crash,1);
 return mode==1;
}
static bool after(void){
 if(operation!=fault)return false;
 if(mode==4)longjmp(crash,1);
 return mode==2;
}
esp_err_t nvs_open(const char *ns,int access,nvs_handle_t *h){*h=1;return before()||after()?ESP_FAIL:ESP_OK;}
void nvs_close(nvs_handle_t h){}
esp_err_t nvs_commit(nvs_handle_t h){return before()||after()?ESP_FAIL:ESP_OK;}
static value_t *value(const char *key,bool add){
 for(unsigned i=0;i<160;++i)if(!strcmp(values[i].key,key))return &values[i];
 if(add)for(unsigned i=0;i<160;++i)if(!values[i].key[0]){strcpy(values[i].key,key);return &values[i];}
 return NULL;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n){
 if(fail_selector_read&&!strcmp(key,"set_select"))return ESP_FAIL;
 if(before())return ESP_FAIL;value_t *v=value(key,false);if(!v)return ESP_ERR_NVS_NOT_FOUND;
 if(out){if(*n<v->size)return ESP_ERR_INVALID_SIZE;memcpy(out,v->data,v->size);}*n=v->size;
 return after()?ESP_FAIL:ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *bytes,size_t n){
 if(before())return ESP_FAIL;value_t *v=value(key,true);if(!v)return ESP_ERR_NO_MEM;
 free(v->data);v->data=malloc(n);assert(v->data);memcpy(v->data,bytes,n);v->size=n;
 return after()?ESP_FAIL:ESP_OK;
}
static size_t write_fault(const void *p,size_t s,size_t n,FILE *f){
 if(before())return 0;
 if(operation==fault&&mode==5){size_t part=n/2;fwrite(p,s,part,f);return part;}
 size_t result=fwrite(p,s,n,f);return after()?0:result;
}
static FILE *open_fault(const char *path,const char *access){
 if(before())return NULL;FILE *f=fopen(path,access);if(after()){if(f)fclose(f);return NULL;}return f;
}
static size_t read_fault(void *p,size_t s,size_t n,FILE *f){
 if(before())return 0;if(operation==fault&&mode==5)return fread(p,s,n/2,f);
 size_t r=fread(p,s,n,f);return after()?0:r;
}
static int seek_fault(FILE *f,long offset,int origin){if(before())return -1;int r=fseek(f,offset,origin);return after()?-1:r;}
static long tell_fault(FILE *f){if(before())return -1;long r=ftell(f);return after()?-1:r;}
static int flush_fault(FILE *f){if(before())return -1;int r=fflush(f);return after()?-1:r;}
static int sync_fault(int fd){if(before())return -1;int r=fsync(fd);return after()?-1:r;}
static int close_fault(FILE *f){bool fail=before();int r=fclose(f);return fail||after()?-1:r;}
static int unlink_fault(const char *p){if(before()){errno=EIO;return -1;}int r=unlink(p);if(after()){errno=EIO;return -1;}return r;}
#define fwrite write_fault
#define fopen open_fault
#define fread read_fault
#define fseek seek_fault
#define ftell tell_fault
#define fflush flush_fault
#define fsync sync_fault
#define fclose close_fault
#define unlink unlink_fault
#define DIR_RULES "/tmp/tianshan-rule-pack-tests/rules"
#include "../../../components/ts_automation/src/ts_rule_store_set.c"
#undef fwrite
#undef fopen
#undef fread
#undef fseek
#undef ftell
#undef fflush
#undef fsync
#undef fclose
#undef unlink
static void reset_disk(void){
 fail_selector_read=false;
 fault=mode=operation=0;ts_rule_store_set_reset();
 for(unsigned i=0;i<160;++i){free(values[i].data);memset(&values[i],0,sizeof values[i]);}
 DIR *d=opendir(OBJECT_DIR);struct dirent *e;
 if(d){while((e=readdir(d)))if(e->d_name[0]!='.'){char p[512];snprintf(p,sizeof p,OBJECT_DIR "/%s",e->d_name);assert(!unlink(p));}closedir(d);}
}
static ts_auto_rule_t ordinary(const char *id,const char *name){
 static ts_auto_action_t action={.type=TS_AUTO_ACT_LOG};
 ts_auto_rule_t r={.actions=&action,.action_count=1,.enabled=true,.revision=1};
 strcpy(r.id,id);strcpy(r.name,name);return r;
}
static void boot_expect(bool has_pack){
 fault=mode=operation=0;ts_rule_store_set_reset();
 ts_auto_rule_t rules[4]={0};bool ro[4]={0};int count=0;
 assert(ts_rule_store_set_load(true,rules,ro,4,&count)==ESP_OK);
 assert(count==(has_pack?3:2));
 bool a=false,b=false,c=false;
 for(int i=0;i<count;++i){
  ts_rule_saved_info_t info={0};assert(ts_rule_store_set_info(rules[i].id,&info)==ESP_OK);
  if(!strcmp(rules[i].id,"a")){a=true;assert(!strcmp(rules[i].name,"A")&&info.source==TS_RULE_SET_NVS_JSON&&!ro[i]&&rules[i].revision==1);}
  else if(!strcmp(rules[i].id,"b")){b=true;assert(!strcmp(rules[i].name,"B")&&info.source==TS_RULE_SET_NVS_JSON&&!ro[i]&&rules[i].revision==1);}
  else if(!strcmp(rules[i].id,"night")){c=true;assert(has_pack&&!strcmp(rules[i].name,"Night")&&info.source==TS_RULE_SET_SD_PACK&&ro[i]&&rules[i].revision==1);}
  else assert(!"unexpected rule");
  ts_rule_dispose(&rules[i]);
 }
 assert(a&&b&&c==has_pack);
}
#ifndef RULE_ENGINE_INTEGRATION
int main(int argc,char **argv){
 assert(argc==2);const char *pki=argv[1];
 device_key=read_file(pki,"developer.key");device_cert=read_file(pki,"developer.pem");ca=read_file(pki,"root.pem");
 assert(ts_config_pack_init()==ESP_OK);
 char *recipient=read_file(pki,"recipient.pem");
 ts_auto_rule_t imported=ordinary("night","Night");
 cJSON *envelope=cJSON_CreateObject();cJSON_AddStringToObject(envelope,"type","automation_rule");
 cJSON_AddItemToObject(envelope,"rule",ts_rule_encode(&imported));char *plain=cJSON_PrintUnformatted(envelope);cJSON_Delete(envelope);
 ts_config_pack_export_opts_t opts={.recipient_cert_pem=recipient,.recipient_cert_len=strlen(recipient)};
 char *raw=NULL;size_t length=0;assert(ts_config_pack_create("different metadata ID",plain,strlen(plain),&opts,&raw,&length)==TS_CONFIG_PACK_OK);free(plain);
 free(device_key);free(device_cert);device_key=read_file(pki,"recipient.key");device_cert=strdup(recipient);
 ts_config_pack_t *decoded=NULL;ts_config_pack_acceptance_t accepted={0};
 assert(ts_config_pack_load_verified_mem(raw,length,NULL,&decoded,&accepted,NULL)==TS_CONFIG_PACK_OK);
 ts_auto_rule_t actual={0};assert(ts_rule_pack_decode(decoded->content,decoded->content_len,&actual)==ESP_OK);
 ts_config_pack_free(decoded);
 /* Legacy seeding re-reads the original file after verification. A changed
  * file must not receive an acceptance record for different bytes. */
 char *legacy_changed=malloc(length+2);memcpy(legacy_changed,raw,length);legacy_changed[length]=' ';legacy_changed[length+1]=0;
 assert(!new_record(&actual,TS_RULE_SET_SD_PACK,true,"legacy.tscfg",legacy_changed,length+1,&accepted));
 free(legacy_changed);
 ts_auto_rule_t initial[]={ordinary("a","A"),ordinary("b","B")};bool readonly[]={false,false};
 unsigned cases=0;
 for(unsigned m=1;m<=5;++m)for(unsigned step=1;step<160;++step){
  reset_disk();assert(ts_rule_store_set_adopt(initial,2,TS_RULE_SOURCE_NVS,readonly)==ESP_OK);
  ts_rule_commit_t result;ts_auto_rule_t a=initial[0];
  /* Establish complete old set before injecting the import. */
  assert(ts_rule_store_set_commit(&a,"a",1,0,NULL,0,NULL,&result)==ESP_OK);
  initial[0].revision=2;
  /* Use a fresh A/B at revision 1 for assertions; establish via seed without mutation increment. */
  reset_disk();initial[0].revision=1;
  assert(ts_rule_store_set_adopt(NULL,0,TS_RULE_SOURCE_NVS,NULL)==ESP_OK);
  assert(ts_rule_store_set_commit(&initial[0],"a",0,0,NULL,0,NULL,&result)==ESP_OK);
  assert(ts_rule_store_set_commit(&initial[1],"b",0,1,NULL,0,NULL,&result)==ESP_OK);
  operation=0;fault=step;mode=m;
  esp_err_t ret=ESP_FAIL;
  if(!setjmp(crash))ret=ts_rule_store_set_commit(&actual,actual.id,0,2,raw,length,&accepted,&result);
  fault=mode=0;
  value_t *selector=value("set_select",false);assert(selector&&selector->size==sizeof(set_selector_t));
  set_selector_t durable;memcpy(&durable,selector->data,sizeof durable);
  assert(selector_valid(&durable));bool committed=durable.generation==3;assert(committed||durable.generation==2);
  if(m!=3&&m!=4&&ret==ESP_OK)assert(committed&&result.durable==1);
  trusted_time=false;boot_expect(committed);boot_expect(committed);trusted_time=true;
  ++cases;
 }
 reset_disk();
 assert(ts_rule_store_set_adopt(initial,2,TS_RULE_SOURCE_NVS,readonly)==ESP_OK);
 ts_rule_commit_t result;assert(ts_rule_store_set_commit(&actual,actual.id,0,0,raw,length,&accepted,&result)==ESP_OK);
 assert(ts_rule_store_set_count()==3);
 uint32_t gen=ts_rule_store_set_generation();
 assert(ts_rule_store_set_commit(&actual,actual.id,1,gen,raw,length,&accepted,&result)==ESP_OK);
 assert(ts_rule_store_set_generation()==gen&&!strcmp(result.error_code,"no_change"));
 ts_auto_rule_t edited=ordinary("a","Changed A");
 assert(ts_rule_store_set_commit(&edited,"a",1,gen,NULL,0,NULL,&result)==ESP_OK);
 ts_auto_rule_t restored[4]={0};bool ro[4];int n;
 ts_rule_store_set_reset();assert(ts_rule_store_set_load(true,restored,ro,4,&n)==ESP_OK&&n==3);
 for(int i=0;i<n;++i){if(!strcmp(restored[i].id,"a"))assert(!strcmp(restored[i].name,"Changed A"));ts_rule_dispose(&restored[i]);}
 ts_rule_store_set_reset();assert(ts_rule_store_set_load(false,restored,ro,4,&n)!=ESP_OK&&n==0);
 reset_disk();ts_rule_dispose(&actual);
 /* First migration has no selector. Failed preparation must leave legacy bytes
  * untouched; after a known selector it must recover that complete generation. */
 actual=ordinary("night","Night");
 const char *legacy=DIR_RULES "/legacy.tscfg";
 unsigned migrations=0;
 for(unsigned m=1;m<=5;++m)for(unsigned step=1;step<125;++step){
  reset_disk();FILE *f=fopen(legacy,"wb");assert(f&&fwrite(raw,1,length,f)==length&&!fclose(f));
  assert(ts_rule_store_set_seed(&actual,legacy,true,&accepted)==ESP_OK);
  bool ro[]={true};assert(ts_rule_store_set_adopt(&actual,1,TS_RULE_SOURCE_SD,ro)==ESP_OK);
  operation=0;fault=step;mode=m;
  if(!setjmp(crash))(void)ts_rule_store_set_migrate();
  fault=mode=0;
  char *original=read_file(DIR_RULES,"legacy.tscfg");assert(strlen(original)==length&&!memcmp(original,raw,length));free(original);
  bool committed=value("set_select",false)!=NULL;
  ts_rule_store_set_reset();ts_auto_rule_t loaded[4]={0};bool readonly[4];int count=0;
  esp_err_t e=ts_rule_store_set_load(true,loaded,readonly,4,&count);
  if(!committed){
   assert(e==ESP_ERR_NOT_FOUND&&count==0);
   assert(ts_rule_store_set_seed(&actual,legacy,true,&accepted)==ESP_OK);
   assert(ts_rule_store_set_adopt(&actual,1,TS_RULE_SOURCE_SD,ro)==ESP_OK);
   assert(ts_rule_store_set_migrate()==ESP_OK);
  }else {assert(e==ESP_OK&&count==1&&!strcmp(loaded[0].id,"night")&&readonly[0]);ts_rule_dispose(&loaded[0]);}
  trusted_time=false;ts_rule_store_set_reset();
  assert(ts_rule_store_set_load(true,loaded,readonly,4,&count)==ESP_OK&&count==1&&readonly[0]&&loaded[0].revision==1);
  ts_rule_dispose(&loaded[0]);trusted_time=true;
  assert(!cJSON_GetObjectItem(cJSON_GetArrayItem(saved,0),"json"));
  ++migrations;
 }
 unlink(legacy);
 /* Ordinary SD JSON retains its source and the exact same-generation mirror. */
 reset_disk();assert(ts_rule_store_set_adopt(initial,2,TS_RULE_SOURCE_SD,readonly)==ESP_OK);
 assert(ts_rule_store_set_migrate()==ESP_OK);gen=ts_rule_store_set_generation();
 edited=ordinary("a","New SD A");assert(ts_rule_store_set_commit(&edited,"a",1,gen,NULL,0,NULL,&result)==ESP_OK);
 ts_rule_store_set_reset();assert(ts_rule_store_set_load(false,restored,ro,4,&n)==ESP_OK&&n==2&&!ts_rule_store_set_available());
 for(int i=0;i<n;++i){ts_rule_saved_info_t info;assert(ts_rule_store_set_info(restored[i].id,&info)==ESP_OK&&info.source==TS_RULE_SET_SD_JSON&&ro[i]);if(!strcmp(restored[i].id,"a"))assert(!strcmp(restored[i].name,"New SD A")&&restored[i].revision==2);ts_rule_dispose(&restored[i]);}
 reset_disk();
 unsigned recovery_cases=0;
 for(unsigned m=1;m<=4;++m)for(unsigned step=1;step<110;++step){
  reset_disk();assert(ts_rule_store_set_adopt(initial,2,TS_RULE_SOURCE_NVS,NULL)==ESP_OK);
  assert(ts_rule_store_set_commit(&actual,actual.id,0,0,raw,length,&accepted,&result)==ESP_OK);
  operation=0;fault=step;mode=m;ts_rule_store_set_reset();
  if(!setjmp(crash)){
   n=0;esp_err_t e=ts_rule_store_set_load(true,restored,ro,4,&n);
   if(e==ESP_OK)for(int i=0;i<n;++i)ts_rule_dispose(&restored[i]);
  }
  fault=mode=0;trusted_time=false;boot_expect(true);boot_expect(true);trusted_time=true;++recovery_cases;
 }
 reset_disk();
 free(raw);free(recipient);free(device_key);free(device_cert);free(ca);
 printf("PASS production codec -> real pack -> complete-set store -> offline boot: %u save, %u migration and %u recovery fault cases; full sources/revisions/readonly, idempotence, unrelated edit, SD absence and same-generation JSON mirror\n",cases,migrations,recovery_cases);
}
#endif
