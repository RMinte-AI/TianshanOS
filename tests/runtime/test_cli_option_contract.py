"""Whole production temp/net commands with selected SDK's real argtable parser."""
from pathlib import Path
import re
import subprocess
import tempfile
from test_console_lifecycle import RTOS, IDF, run_native

code=RTOS+r'''
#include "ts_api.h"
#include "ts_temp_source.h"
#include "ts_net_manager.h"
#include "argtable3/argtable3.h"
typedef struct {const char *command,*help,*hint;int (*func)(int,char**);void *argtable;int category;} ts_console_cmd_t;
typedef ts_console_cmd_t esp_console_cmd_t;
#define TS_CMD_CAT_NETWORK 1
#define TS_CONFIG_MODULE_NET 1
static int (*net_handler)(int,char**),(*temp_handler)(int,char**);
static int ts_console_register_cmd(const ts_console_cmd_t *c){net_handler=c->func;return ESP_OK;}
static int esp_console_cmd_register(const esp_console_cmd_t *c){temp_handler=c->func;return ESP_OK;}
#define ts_console_success(...) ((void)0)
static int ts_console_printf(const char *f,...){(void)f;return 0;}
static void ts_console_error(const char *f,...){(void)f;}
static int writes,reads,saves,restarts,loads,mode_writes;
static ts_net_if_t last_iface;
static ts_net_if_config_t config,configs[TS_NET_IF_MAX];
static char api_name[40];static cJSON *api_params;
esp_err_t ts_api_call(const char *name,const cJSON *params,ts_api_result_t *r){
 strcpy(api_name,name);cJSON_Delete(api_params);api_params=cJSON_Duplicate(params,true);
 if(!strcmp(name,"temp.manual")||!strcmp(name,"temp.select"))++writes;else ++reads;
 memset(r,0,sizeof(*r));r->code=TS_API_OK;r->data=cJSON_CreateObject();return ESP_OK;
}
void ts_api_result_init(ts_api_result_t *r){memset(r,0,sizeof(*r));}
esp_err_t ts_temp_get_by_source(ts_temp_source_type_t t,ts_temp_data_t *d){(void)t;memset(d,0,sizeof(*d));return ESP_OK;}
void ts_api_result_free(ts_api_result_t *r){cJSON_Delete(r->data);r->data=NULL;}
int16_t ts_temp_get_effective(ts_temp_data_t *d){++reads;memset(d,0,sizeof(*d));return 0;}
bool ts_temp_source_is_initialized(void){return true;}
bool ts_temp_is_manual_mode(void){return false;}
esp_err_t ts_temp_get_status(ts_temp_status_t *s){++reads;memset(s,0,sizeof(*s));return ESP_OK;}
esp_err_t ts_net_manager_get_status(ts_net_manager_status_t *s){++reads;memset(s,0,sizeof(*s));return ESP_OK;}
esp_err_t ts_net_manager_get_config(ts_net_if_t i,ts_net_if_config_t *c){++reads;last_iface=i;*c=i==TS_NET_IF_ETH?config:configs[i];return ESP_OK;}
esp_err_t ts_net_manager_set_config(ts_net_if_t i,const ts_net_if_config_t *c){
 if(i==TS_NET_IF_ETH&&c->ip_mode!=TS_NET_IP_MODE_STATIC)return ESP_ERR_NOT_SUPPORTED;
 ++writes;last_iface=i;if(i==TS_NET_IF_ETH)config=*c;else configs[i]=*c;return ESP_OK;
}
esp_err_t ts_net_manager_set_static_ip(ts_net_if_t i,const ts_net_ip_info_str_t *p){++writes;last_iface=i;(i==TS_NET_IF_ETH?&config:&configs[i])->static_ip=*p;return ESP_OK;}
esp_err_t ts_net_manager_set_ip_mode(ts_net_if_t i,ts_net_ip_mode_t m){++writes;++mode_writes;last_iface=i;(i==TS_NET_IF_ETH?&config:&configs[i])->ip_mode=m;return ESP_OK;}
esp_err_t ts_net_manager_set_hostname(const char *s){(void)s;++writes;return ESP_OK;}
const char *ts_net_state_to_str(ts_net_state_t s){(void)s;return "fixture";}
const char *ts_net_if_to_str(ts_net_if_t i){(void)i;return "fixture";}
esp_err_t ts_net_manager_start(ts_net_if_t i){last_iface=i;++writes;return ESP_OK;}
esp_err_t ts_net_manager_stop(ts_net_if_t i){last_iface=i;++writes;return ESP_OK;}
esp_err_t ts_net_manager_restart(ts_net_if_t i){last_iface=i;++writes;++restarts;return ESP_OK;}
esp_err_t ts_net_manager_save_config(void){++writes;++saves;return ESP_OK;}
static int ts_config_module_persist(int module){assert(module==TS_CONFIG_MODULE_NET);return ESP_OK;}
esp_err_t ts_net_manager_load_config(void){++writes;++loads;return ESP_OK;}
esp_err_t ts_net_manager_reset_config(void){++writes;return ESP_OK;}
'''
for file in ['ts_cmd_temp.c','ts_cmd_net.c']:
    source=Path('components/ts_console/commands',file).read_text()
    code+='\n#undef TAG\n'+re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
code+=r'''
static int run(const char *command){
 char text[1024];strcpy(text,command);char *argv[64];int argc=0;
 /* Split these literal argv fixtures only; actual option parsing is SDK arg_parse. */
 char *save,*word=strtok_r(text," ",&save);while(word){argv[argc++]=word;word=strtok_r(NULL," ",&save);}argv[argc]=NULL;
 return !strcmp(argv[0],"net")?net_handler(argc,argv):temp_handler(argc,argv);
}
static void readonly(const char *cmd){int before=writes;assert(run(cmd)==0&&writes==before);}
static void invalid(const char *cmd){int before=writes;assert(run(cmd)!=0&&writes==before);}
int main(void){
 config.ip_mode=TS_NET_IP_MODE_STATIC;for(unsigned i=0;i<TS_NET_IF_MAX;i++)configs[i].ip_mode=TS_NET_IP_MODE_STATIC;
 assert(ts_cmd_temp_register()==ESP_OK&&ts_cmd_net_register()==ESP_OK);
 if(getenv("TS_CLI_CONTRACT_TEMP_FIRST"))assert(run("temp --mode --value auto")==0&&!strcmp(api_name,"temp.select"));
 for(int round=0;round<3;round++){
  readonly("net --ip");readonly("net --ip --iface eth --json");readonly("net --iface wifi --ip -j");
  int before=writes;assert(run("net --set --iface eth --ip 10.10.99.98")==0&&writes==before+1);assert(!strcmp(config.static_ip.ip,"10.10.99.98")&&last_iface==TS_NET_IF_ETH);
  readonly("net --ip");
  assert(run("net --ip=10.10.99.99 --iface eth --set")==0&&!strcmp(config.static_ip.ip,"10.10.99.99"));
  readonly("net --ip");
  assert(run("net --set --mode static --ip 10.10.99.98 --netmask 255.255.255.0 --gateway 10.10.99.1")==0);
  assert(!strcmp(config.static_ip.ip,"10.10.99.98")&&!strcmp(config.static_ip.netmask,"255.255.255.0")&&!strcmp(config.static_ip.gateway,"10.10.99.1")&&config.ip_mode==TS_NET_IP_MODE_STATIC);
  invalid("net --set --mode dhcp");assert(config.ip_mode==TS_NET_IP_MODE_STATIC);
  assert(run("net --set --iface wifi --mode dhcp")==0&&configs[TS_NET_IF_WIFI_STA].ip_mode==TS_NET_IP_MODE_DHCP);
  assert(run("net --set --dns 8.8.8.8")==0&&!strcmp(config.static_ip.dns1,"8.8.8.8"));
  readonly("net --config");readonly("net -c --iface wifi -j");readonly("net --status");readonly("net -s -j");readonly("net --help");readonly("net -h");readonly("net");
  readonly("temp --status");readonly("temp --providers --json");readonly("temp -h");
  const char *numbers[]={"45.5","-2.5","4.5e1","0x1.4p5","+22.5"};
  for(unsigned n=0;n<5;n++)for(int shortopt=0;shortopt<2;shortopt++){
   char cmd[100];snprintf(cmd,sizeof(cmd),"temp --set %s %s",shortopt?"-V":"--value",numbers[n]);assert(run(cmd)==0);
   assert(!strcmp(api_name,"temp.manual")&&cJSON_GetObjectItem(api_params,"temperature_c")->valuedouble==strtod(numbers[n],NULL));
  }
  assert(run("temp --mode --value auto")==0&&!strcmp(api_name,"temp.select")&&!strcmp(cJSON_GetObjectItem(api_params,"source")->valuestring,"auto"));
  assert(run("temp --value manual --mode")==0&&!strcmp(api_name,"temp.manual")&&cJSON_IsTrue(cJSON_GetObjectItem(api_params,"enable")));
  assert(run("temp --mode -V auto")==0&&!strcmp(api_name,"temp.select"));
 }
 assert(!saves&&!loads&&!restarts);
 const char *bad[]={"net --set --ip","net --ip 10.10.99.98","net --ip=10.10.99.98","net --set 10.10.99.98","net --set --ip=10.10.99.98 10.10.99.99","net --set --ip 1 2","net --set --mode wrong","net --set --dns","net --set --ip=","temp --set","temp --set -V","temp --set --value wrong","temp --set -V 45.5junk","temp --mode --value wrong","temp --mode","temp --mode --value"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)invalid(bad[i]);
 assert(run("net --save")==0&&saves==1);assert(run("net --load")==0&&loads==1);assert(run("net --restart")==0&&restarts==1);
 /* The explicit -- delimiter remains SDK-owned; a positional address still requires --set --ip. */
 assert(run("net --set --ip -- 10.10.99.98")==0&&!strcmp(config.static_ip.ip,"10.10.99.98"));readonly("net --ip --");
 invalid("net -- 10.10.99.98");
 /* Same lexical results as SDK arg_dbl for valid numeric spellings. */
 struct arg_dbl *dbl=arg_dbl0("V","value",NULL,NULL);struct arg_end *end=arg_end(3);void *table[]={dbl,end};
 for(int i=0;i<2;i++){char *args[]={"fixture","--value",i?"4.5e1":"0x1.4p5",NULL};assert(arg_parse(3,args,table)==0&&dbl->dval[0]==(i?45:40));}
 arg_freetable(table,2);arg_freetable((void**)&s_net_args,sizeof(s_net_args)/sizeof(void*));arg_freetable((void**)&s_temp_args,sizeof(s_temp_args)/sizeof(void*));cJSON_Delete(api_params);
 puts("PASS F75 whole production commands + real SDK argtable: bare/space/equals IP, ordering, continuous set/query reset, aliases, temp numeric/mode, invalid zero writes, explicit save/load/restart");
}
'''
argtable=IDF/'components/console/argtable3'
headers={'argtable3/argtable3.h':(argtable/'argtable3.h').read_text(),
         'ts_net_manager.h':Path('components/ts_net/include/ts_net_manager.h').read_text(),
         'esp_netif.h':'#pragma once\ntypedef void esp_netif_t;\ntypedef struct {struct {uint32_t addr;} ip,netmask,gw;} esp_netif_ip_info_t;\n'}
# SDK arg_end packs int[5] before pointers for 32-bit targets. On a 64-bit
# host the SDK itself has misaligned pointer fields; keep ASan and the rest of
# UBSan there, while production commands retain full ASan/UBSan instrumentation.
with tempfile.TemporaryDirectory(prefix='ts-argtable-host-') as directory:
    objects=[]
    for name in ['arg_end.c','argtable3.c']:
        obj=Path(directory)/(name+'.o')
        subprocess.run(['cc','-std=gnu11','-g','-fsanitize=address,undefined',
                        '-fno-sanitize=alignment','-fno-sanitize-recover=all',
                        '-I'+str(argtable),'-c',str(argtable/name),'-o',str(obj)],check=True)
        objects.append(obj)
    sources=[p for p in sorted(argtable.glob('*.c')) if p.name not in ['arg_end.c','argtable3.c']]
    run_native(code,[IDF/'components/json/cJSON/cJSON.c',*sources,*objects],headers)
