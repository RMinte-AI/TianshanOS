"""Real CLI/API controls with SDK argtable, isolated NVS/views and local synthetic SSH."""
from pathlib import Path
import argparse
import re
from test_console_lifecycle import RTOS, IDF, body, run_native

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=Path, default=Path('.'))
parser.add_argument('--case', choices=['all','revoke-api','revoke-cli','known','deployed','auth'], default='all')
args = parser.parse_args()
CLI = 'components/ts_console/commands/ts_cmd_ssh.c'
HOSTS = 'components/ts_console/commands/ts_cmd_hosts.c'
API = 'components/ts_api/src/ts_api_ssh.c'
KNOWN = 'components/ts_security/src/ts_known_hosts.c'
DEPLOYED = 'components/ts_security/src/ts_ssh_hosts_config.c'


def source(path):
    candidate = args.source_root / path
    return (candidate if candidate.exists() else Path(path)).read_text()


def function(path, name):
    candidate = args.source_root / path
    return body(candidate if candidate.exists() else path, name)


code = RTOS + r'''
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include "ts_api.h"
#include "ts_ssh_client.h"
#include "ts_known_hosts.h"
#include "ts_ssh_hosts_config.h"
#include "ts_automation_config.h"
#include "argtable3/argtable3.h"
#define TAG "local-ssh-controls"
#define TS_MALLOC_PSRAM malloc
#define TS_STRDUP_PSRAM strdup
#define TS_STR(x) "fixture help"
#define TS_API_ERR_HOST_MISMATCH 1001
#define TS_API_ERR_HOST_NEW 1002
typedef struct {const char *command,*help,*hint;int (*func)(int,char**);void *argtable;} esp_console_cmd_t;
static int (*registered_ssh)(int,char**),(*registered_hosts)(int,char**);
static int esp_console_cmd_register(const esp_console_cmd_t *cmd) {if(!strcmp(cmd->command,"ssh"))registered_ssh=cmd->func;else {assert(!strcmp(cmd->command,"hosts"));registered_hosts=cmd->func;}return ESP_OK;}
static char console_output[8192];
static int ts_console_printf(const char *format,...) {va_list ap;va_start(ap,format);size_t used=strlen(console_output);int n=vsnprintf(console_output+used,sizeof(console_output)-used,format,ap);va_end(ap);return n;}
static struct {void *pointer;size_t size;} keys[32];static int keys_loaded,keys_freed;static bool fail_private_load;
static void traced_free(void *p) {for(int i=0;i<32;i++)if(p&&p==keys[i].pointer){for(size_t j=0;j<keys[i].size;j++)assert(!((uint8_t*)p)[j]);keys[i].pointer=NULL;++keys_freed;}free(p);}
#define free traced_free
esp_err_t ts_keystore_load_private_key(const char *id,char **data,size_t *len) {assert(!strcmp(id,"synthetic-key"));if(fail_private_load)return ESP_ERR_NOT_FOUND;*data=strdup("SYNTHETIC PRIVATE BYTES");*len=strlen(*data);int i=0;while(i<32&&keys[i].pointer)i++;assert(i<32);keys[i].pointer=*data;keys[i].size=*len;++keys_loaded;return ESP_OK;}
esp_err_t ts_keystore_load_public_key(const char *id,char **data,size_t *len) {assert(!strcmp(id,"synthetic-key"));*data=strdup("ssh-ed25519 SYNTHETIC fixture");*len=strlen(*data);return ESP_OK;}
struct ts_ssh_session_s {ts_ssh_config_t config;};
static int sessions,auths,expected_auth,verifications,revoke_mode,remote_calls,api_calls;
static ts_host_verify_result_t trust_status=TS_HOST_VERIFY_OK;
static char last_api[2048],last_remote[1024];
esp_err_t ts_ssh_session_create(const ts_ssh_config_t *cfg,ts_ssh_session_t *out) {
    assert(!strcmp(cfg->host,"fixture.invalid")&&cfg->port==22&&!strcmp(cfg->username,"fixture"));
    if(expected_auth==0)assert(cfg->auth_method==TS_SSH_AUTH_PASSWORD&&!strcmp(cfg->auth.password,"synthetic-password"));
    else if(expected_auth==1)assert(cfg->auth_method==TS_SSH_AUTH_PUBLICKEY&&cfg->auth.key.private_key&&!strcmp((const char*)cfg->auth.key.private_key,"SYNTHETIC PRIVATE BYTES")&&!cfg->auth.key.private_key_path);
    else assert(cfg->auth_method==TS_SSH_AUTH_PUBLICKEY&&!cfg->auth.key.private_key&&!strcmp(cfg->auth.key.private_key_path,"/fixture/key.pem"));
    *out=malloc(sizeof(**out));(*out)->config=*cfg;++sessions;return ESP_OK;
}
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t s) {free(s);--sessions;return ESP_OK;}
esp_err_t ts_ssh_disconnect(ts_ssh_session_t s) {(void)s;return ESP_OK;}
const char *ts_ssh_get_error(ts_ssh_session_t s) {(void)s;return "synthetic connection failure";}
const char *ts_ssh_get_host(ts_ssh_session_t s) {return s->config.host;}
uint16_t ts_ssh_get_port(ts_ssh_session_t s) {return s->config.port;}
const char *ts_ssh_connect_reason(ts_ssh_session_t s,const char **stage) {(void)s;*stage="connect";return "ssh_connect_failed";}
esp_err_t ts_ssh_connect_with_verifier(ts_ssh_session_t s,ts_ssh_verify_cb_t verify,void *context) {int ret=verify(s,context);if(ret!=ESP_OK)return ret;++auths;return ESP_OK;}
/* Only the remote filesystem prefix is mapped; the emitted shell logic is real. */
static void map_remote(const char *command,char *out,size_t size) {
    const char *needle="~/.ssh",*p=command;size_t n=0;
    while(*p){if(!strncmp(p,needle,strlen(needle))){const char *replacement="remote/.ssh";assert(n+strlen(replacement)<size);memcpy(out+n,replacement,strlen(replacement));n+=strlen(replacement);p+=strlen(needle);}else {assert(n+1<size);out[n++]=*p++;}}out[n]=0;assert(!strstr(out,"~/.ssh"));
}
esp_err_t ts_ssh_exec(ts_ssh_session_t s,const char *command,ts_ssh_exec_result_t *result) {
    (void)s;memset(result,0,sizeof(*result));++remote_calls;
    if(!strcmp(command,"printf normal")||!strcmp(command,"normal")){result->stdout_data=strdup("normal\n");return ESP_OK;}
    assert(strstr(command,"authorized_keys")&&strstr(command,"SYNTHETIC"));assert(strlen(command)<sizeof(last_remote));strcpy(last_remote,command);
    bool removing=strstr(command,"grep -vF")!=NULL;
    if(removing&&revoke_mode==2){assert(!rename("remote/.ssh/authorized_keys","remote/.ssh/authorized_keys.saved"));assert(!mkdir("remote/.ssh/authorized_keys",0700));}
    if(removing&&revoke_mode==5)return ESP_FAIL;
    char mapped[2048],script[2304];map_remote(command,mapped,sizeof(mapped));
    snprintf(script,sizeof(script),"%s%s",removing&&revoke_mode==3?"mv() { return 1; }; ":removing&&revoke_mode==4?"chmod() { return 1; }; ":"",mapped);
    FILE *pipe=popen(script,"r");assert(pipe);char output[512];size_t n=fread(output,1,sizeof(output)-1,pipe);output[n]=0;int status=pclose(pipe);assert(status!=-1);result->exit_code=WIFEXITED(status)?WEXITSTATUS(status):1;result->stdout_data=strdup(output);return ESP_OK;
}
void ts_ssh_exec_result_free(ts_ssh_exec_result_t *r) {free(r->stdout_data);free(r->stderr_data);memset(r,0,sizeof(*r));}
esp_err_t ts_known_hosts_verify(ts_ssh_session_t s,ts_host_verify_result_t *status,ts_known_host_t *info) {*status=trust_status;++verifications;if(info){memset(info,0,sizeof(*info));strcpy(info->host,s->config.host);info->port=s->config.port;strcpy(info->fingerprint,"synthetic-fingerprint");}return ESP_OK;}
esp_err_t ts_known_hosts_add(ts_ssh_session_t s) {(void)s;assert(false);return ESP_FAIL;}
esp_err_t ts_known_hosts_get(const char *host,uint16_t port,ts_known_host_t *info) {(void)host;(void)port;strcpy(info->fingerprint,"synthetic-fingerprint");return ESP_OK;}
esp_err_t ts_known_hosts_remove(const char *host,uint16_t port) {(void)host;(void)port;assert(false);return ESP_FAIL;}
static bool host_verify_prompt(const ts_known_host_t *host,ts_host_verify_result_t status,void *context) {(void)host;(void)status;(void)context;assert(false);return false;}
esp_err_t ts_known_hosts_verify_interactive(ts_ssh_session_t s,ts_host_prompt_cb_t prompt,void *context) {(void)s;(void)prompt;(void)context;++verifications;return trust_status==TS_HOST_VERIFY_OK?ESP_OK:ESP_FAIL;}
typedef struct {char *data;size_t len;} ssh_request_key_t;
'''
for name in ['ts_api_result_init','ts_api_result_free','ts_api_result_ok','ts_api_result_error','ts_api_result_reason']:
    code += function('components/ts_api/src/ts_api.c',name)
for name in ['cleanup_key_buffer','ssh_connect_error','configure_ssh_from_params','verify_host_fingerprint']:
    code += function(API,name)
apitext=source(API)
start=apitext.index('typedef struct {\n    const cJSON *params;')
code += apitext[start:apitext.index('} verify_context_t;',start)+len('} verify_context_t;')]
for name in ['verify_before_auth','connect_verified','api_ssh_exec','api_ssh_test','api_ssh_revoke']:
    code += function(API,name)
code += r'''
esp_err_t ts_api_call(const char *name,const cJSON *params,ts_api_result_t *r) {++api_calls;char *json=cJSON_PrintUnformatted(params);assert(strlen(json)<sizeof(last_api)&&!strstr(json,"SYNTHETIC PRIVATE BYTES"));strcpy(last_api,json);free(json);if(!strcmp(name,"ssh.exec"))return api_ssh_exec(params,r);assert(!strcmp(name,"ssh.test"));return api_ssh_test(params,r);}
'''
clitext=source(CLI)
start=clitext.index('static struct {')
code += clitext[start:clitext.index('} s_ssh_args;',start)+len('} s_ssh_args;')]
start=clitext.index('typedef struct {')
code += clitext[start:clitext.index('} ssh_auth_info_t;',start)+len('} ssh_auth_info_t;')]
for name in ['config_ssh_auth','verify_host_key','cli_verify_before_auth','do_ssh_exec','do_ssh_test','do_ssh_revoke']:
    code += function(CLI,name)
# The unchanged shell/forward transports are unused boundaries; exercise their
# existing auth configurator without creating a real session or socket.
code += r'''
static int do_ssh_shell(const char *host,int port,const char *user,const ssh_auth_info_t *auth,int timeout,bool verbose) {(void)host;(void)port;(void)user;(void)timeout;(void)verbose;ts_ssh_config_t cfg={0};config_ssh_auth(&cfg,auth);assert(cfg.auth_method==TS_SSH_AUTH_PUBLICKEY&&cfg.auth.key.private_key==auth->key_data);return 0;}
static int do_ssh_forward(const char *host,int port,const char *user,const ssh_auth_info_t *auth,const char *spec,int timeout,bool verbose) {(void)spec;return do_ssh_shell(host,port,user,auth,timeout,verbose);}
static int do_ssh_keygen(const char *type,const char *path,const char *comment) {(void)type;(void)path;(void)comment;assert(false);return 1;}
static int do_ssh_copy_id(const char *host,int port,const char *user,const char *password,const char *key,const char *id,int timeout) {(void)host;(void)port;(void)user;(void)password;(void)key;(void)id;(void)timeout;assert(false);return 1;}
'''
for name in ['ssh_cmd_handler','ts_cmd_ssh_register']:
    code += function(CLI,name)
# Dedicated in-memory NVS namespace, with actual production key generation,
# add/list/count and clear. No host machine NVS or real fingerprints are read.
code += r'''
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_TYPE_BLOB 1
typedef struct {char key[16];int type;} nvs_entry_info_t;
static struct {char key[16];uint8_t value[256];size_t len;} entries[16];
typedef int *nvs_iterator_t;
static int entry_find(const char *key) {for(int i=0;i<16;i++)if(!strcmp(entries[i].key,key))return i;return -1;}
static int nvs_open(const char *name,int mode,nvs_handle_t *out) {(void)mode;assert(!strcmp(name,"ts_ssh_hosts"));*out=1;return ESP_OK;}
static void nvs_close(nvs_handle_t h) {(void)h;}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t n) {assert(h==1&&strlen(key)<16&&n<=256);int i=entry_find(key);if(i<0)for(i=0;i<16&&entries[i].key[0];i++);assert(i<16);strcpy(entries[i].key,key);memcpy(entries[i].value,data,n);entries[i].len=n;return ESP_OK;}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n) {assert(h==1);int i=entry_find(key);if(i<0)return ESP_ERR_NVS_NOT_FOUND;assert(*n>=entries[i].len);*n=entries[i].len;memcpy(out,entries[i].value,*n);return ESP_OK;}
static int nvs_commit(nvs_handle_t h) {assert(h==1);return ESP_OK;}
static int nvs_erase_all(nvs_handle_t h) {assert(h==1);memset(entries,0,sizeof(entries));return ESP_OK;}
static int nvs_entry_find_in_handle(nvs_handle_t h,int type,nvs_iterator_t *out) {assert(h==1&&type==NVS_TYPE_BLOB);for(int i=0;i<16;i++)if(entries[i].key[0]){*out=malloc(sizeof(**out));**out=i;return ESP_OK;}return ESP_ERR_NVS_NOT_FOUND;}
static void nvs_entry_info(nvs_iterator_t it,nvs_entry_info_t *info) {strcpy(info->key,entries[*it].key);info->type=NVS_TYPE_BLOB;}
static int nvs_entry_next(nvs_iterator_t *it) {for(int i=**it+1;i<16;i++)if(entries[i].key[0]){**it=i;return ESP_OK;}free(*it);*it=NULL;return ESP_ERR_NVS_NOT_FOUND;}
static void nvs_release_iterator(nvs_iterator_t it) {free(it);}
'''
knowntext=source(KNOWN)
code += knowntext[knowntext.index('#define NVS_NAMESPACE'):knowntext.index('/* ============================================================================\n * 外部声明')]
for name in ['simple_hash','make_nvs_key','ts_known_hosts_init','ts_known_hosts_deinit','ts_known_hosts_add_manual','ts_known_hosts_list','ts_known_hosts_count','ts_known_hosts_clear']:
    code += function(KNOWN,name)
# The storage adapter reads a real immutable document shape. Mutation is the
# local configuration-store boundary, accepting only the original empty array.
code += r'''
static bool initialized=true;
struct ts_auto_config_view {ts_auto_document_t document;};
static ts_ssh_host_config_t deployed_rows[2];
static struct ts_auto_config_view deployed_view;
static int view_readers,collection_replaces;
const ts_auto_config_view_t *ts_auto_config_acquire(void) {++view_readers;return &deployed_view;}
const ts_auto_document_t *ts_auto_config_definitions(const ts_auto_config_view_t *v) {assert(v==&deployed_view&&view_readers>0);return &v->document;}
void ts_auto_config_release(const ts_auto_config_view_t *v) {assert(v==&deployed_view&&view_readers>0);--view_readers;}
esp_err_t ts_auto_config_replace_collection(ts_auto_config_kind_t kind,const cJSON *array,const char *version,ts_auto_config_error_t *error) {assert(kind==TS_AUTO_CONFIG_HOST&&cJSON_IsArray(array)&&cJSON_GetArraySize(array)==0&&!version&&!error&&!view_readers);++collection_replaces;deployed_view.document.host_count=0;return ESP_OK;}
'''
for name in ['ts_ssh_hosts_config_count','ts_ssh_hosts_config_clear','ts_ssh_hosts_config_iterate']:
    code += function(DEPLOYED,name)
code += 'typedef struct {ts_ssh_host_config_t *items;size_t count;} list_context_t;\n'
for name in ['list_item','ts_ssh_hosts_config_list']:
    code += function(DEPLOYED,name)
hosttext=source(HOSTS)
start=hosttext.index('static struct {')
code += hosttext[start:hosttext.index('} s_hosts_args;',start)+len('} s_hosts_args;')]
code += r'''
static int do_hosts_list(bool json) {(void)json;ts_known_host_t rows[32];size_t n;assert(ts_known_hosts_list(rows,32,&n)==ESP_OK);return 0;}
static int do_deployed_list(bool json) {(void)json;ts_ssh_host_config_t rows[16];size_t n;assert(ts_ssh_hosts_config_list(rows,16,&n)==ESP_OK);return 0;}
static int do_hosts_info(const char *host,int port,bool json) {(void)host;(void)port;(void)json;assert(false);return 1;}
static int do_hosts_remove(const char *host,int port) {(void)host;(void)port;assert(false);return 1;}
static int do_deployed_remove(const char *id) {(void)id;assert(false);return 1;}
static void show_help(void) {assert(false);}
'''
for name in ['do_hosts_clear','do_deployed_clear','hosts_cmd_handler','ts_cmd_hosts_register']:
    code += function(HOSTS,name)
code += r'''
static void put_file(const char *path,const char *text) {FILE *f=fopen(path,"w");assert(f);assert(fwrite(text,1,strlen(text),f)==strlen(text));assert(!fclose(f));}
static char *read_file(const char *path) {FILE *f=fopen(path,"r");assert(f);char *out=calloc(1,512);size_t n=fread(out,1,511,f);assert(!ferror(f));out[n]=0;fclose(f);return out;}
static void remote_reset(const char *text) {const char *paths[]={"remote/.ssh/authorized_keys","remote/.ssh/authorized_keys.tmp","remote/.ssh/authorized_keys.bak","remote/.ssh/authorized_keys.saved"};for(int i=0;i<4;i++){unlink(paths[i]);rmdir(paths[i]);}put_file(paths[0],text);remote_calls=0;last_remote[0]=0;revoke_mode=0;}
static void check_revoke(bool cli) {
    const char *rows[]={"ssh-ed25519 SYNTHETIC fixture\n","ssh-ed25519 SYNTHETIC first\nssh-ed25519 SYNTHETIC duplicate\n","ssh-ed25519 OTHER retained\nssh-ed25519 SYNTHETIC fixture\n"};
    for(int mode=0;mode<6;mode++)for(int i=0;i<(mode?1:3);i++){
        remote_reset(rows[i]);revoke_mode=mode;if(mode==1)assert(!mkdir("remote/.ssh/authorized_keys.tmp",0700));expected_auth=0;
        ts_api_result_t r={0};int ret;
        if(cli)ret=do_ssh_revoke("fixture.invalid",22,"fixture","synthetic-password",NULL,"synthetic-key",10);
        else {cJSON *p=cJSON_Parse("{\"host\":\"fixture.invalid\",\"user\":\"fixture\",\"password\":\"synthetic-password\",\"keyid\":\"synthetic-key\"}");ret=api_ssh_revoke(p,&r);cJSON_Delete(p);}
        assert(!sessions);
        if(mode){assert(ret!=0);if(!cli)assert(r.code!=TS_API_OK);if(mode==4){char *text=read_file("remote/.ssh/authorized_keys");assert(!text[0]);free(text);}else {char *text=read_file(mode==2?"remote/.ssh/authorized_keys.saved":"remote/.ssh/authorized_keys");assert(!strcmp(text,rows[i]));free(text);}}
        else {assert(!ret);if(!cli)assert(cJSON_IsTrue(cJSON_GetObjectItem(r.data,"revoked")));char *text=read_file("remote/.ssh/authorized_keys");assert(!strcmp(text,i==2?"ssh-ed25519 OTHER retained\n":""));free(text);struct stat st;assert(!stat("remote/.ssh/authorized_keys",&st)&&(st.st_mode&0777)==0600);}
        assert(remote_calls==(cli&&!mode?3:2));ts_api_result_free(&r);
    }
    remote_reset("ssh-ed25519 OTHER retained\n");ts_api_result_t r={0};if(cli)assert(do_ssh_revoke("fixture.invalid",22,"fixture","synthetic-password",NULL,"synthetic-key",10)==0);else {cJSON *p=cJSON_Parse("{\"host\":\"fixture.invalid\",\"user\":\"fixture\",\"password\":\"synthetic-password\",\"keyid\":\"synthetic-key\"}");assert(api_ssh_revoke(p,&r)==ESP_OK&&!cJSON_IsTrue(cJSON_GetObjectItem(r.data,"found")));cJSON_Delete(p);ts_api_result_free(&r);}assert(remote_calls==1&&!sessions);
    printf("PASS %s actual revoke: single/duplicate empty, mixed retention, absent and open/read/move/chmod/transport failures\n",cli?"CLI":"API");
}
static int command(int (*handler)(int,char**),const char *tail) {char buffer[512];snprintf(buffer,sizeof(buffer),"%s",tail);char *argv[32];int argc=0;char *p=strtok(buffer," ");while(p){assert(argc<32);argv[argc++]=p;p=strtok(NULL," ");}console_output[0]=0;return handler(argc,argv);}
static void check_known(void) {assert(ts_known_hosts_init()==ESP_OK);uint8_t unrelated=9;assert(nvs_set_blob(s_state.nvs,"other_blob",&unrelated,1)==ESP_OK);assert(ts_known_hosts_add_manual("fixture.invalid",22,"synthetic-fingerprint",TS_HOST_KEY_ED25519)==ESP_OK);assert(command(registered_hosts,"hosts --list")==0);ts_known_host_t rows[2];size_t count=0;assert(ts_known_hosts_list(rows,2,&count)==ESP_OK&&count==1&&!strcmp(rows[0].host,"fixture.invalid"));assert(ts_known_hosts_count()==1);assert(command(registered_hosts,"hosts --clear")==1&&ts_known_hosts_count()==1);assert(command(registered_hosts,"hosts --clear --yes")==0&&ts_known_hosts_count()==0);assert(command(registered_hosts,"hosts --clear --yes")==0);assert(ts_known_hosts_deinit()==ESP_OK);puts("PASS actual stored h_ key, list/count and confirmed known-host clear");}
static void check_deployed(void) {remote_reset("ssh-ed25519 OTHER retained\n");strcpy(deployed_rows[0].id,"fixture-host");deployed_view.document.hosts=deployed_rows;deployed_view.document.host_count=1;assert(command(registered_hosts,"hosts --deployed")==0);assert(command(registered_hosts,"hosts --deployed --clear")==1&&!collection_replaces&&ts_ssh_hosts_config_count()==1);assert(command(registered_hosts,"hosts --deployed --clear --yes")==0&&collection_replaces==1&&ts_ssh_hosts_config_count()==0);assert(command(registered_hosts,"hosts --deployed")==0);assert(command(registered_hosts,"hosts --deployed --clear --yes")==0&&collection_replaces==1&&!view_readers);char *remote=read_file("remote/.ssh/authorized_keys");assert(!strcmp(remote,"ssh-ed25519 OTHER retained\n")&&!remote_calls);free(remote);puts("PASS actual frozen view count, original clear confirmation and empty collection");}
static void check_auth(void) {
    const char *modes[]={"--exec normal","--test"};
    for(int i=0;i<2;i++)for(int kind=0;kind<3;kind++){expected_auth=kind;char cmd[256];snprintf(cmd,sizeof(cmd),"ssh --host fixture.invalid --user fixture %s %s",kind==0?"--password synthetic-password":kind==1?"--keyid synthetic-key":"--key /fixture/key.pem",modes[i]);assert(command(registered_ssh,cmd)==0);cJSON *p=cJSON_Parse(last_api);assert(p);assert(cJSON_HasObjectItem(p,kind==0?"password":kind==1?"keyid":"keypath"));cJSON_Delete(p);assert(!sessions&&keys_loaded==keys_freed);}
    ssh_auth_info_t auth={.password="synthetic-password",.key_path="/fixture/key.pem"};expected_auth=0;assert(do_ssh_exec("fixture.invalid",22,"fixture",&auth,"printf normal",10,false)==0&&strstr(last_api,"password")&&!strstr(last_api,"keypath"));assert(do_ssh_test("fixture.invalid",22,"fixture",&auth,10)==0&&strstr(last_api,"password")&&!strstr(last_api,"keypath"));auth.password=NULL;expected_auth=2;assert(do_ssh_test("fixture.invalid",22,"fixture",&auth,10)==0&&strstr(last_api,"keypath"));
    fail_private_load=true;int before_auth=auths,before_api=api_calls;assert(command(registered_ssh,"ssh --host fixture.invalid --user fixture --keyid synthetic-key --test")==1&&auths==before_auth&&api_calls==before_api&&!sessions&&keys_loaded==keys_freed);fail_private_load=false;expected_auth=2;assert(command(registered_ssh,"ssh --host fixture.invalid --user fixture --key /fixture/key.pem --keyid synthetic-key --test")==0&&strstr(last_api,"keypath")&&!strstr(last_api,"keyid"));
    assert(command(registered_ssh,"ssh --host fixture.invalid --user fixture --keyid synthetic-key --shell")==0);assert(command(registered_ssh,"ssh --host fixture.invalid --user fixture --keyid synthetic-key --forward L123:fixture:22")==0);assert(keys_loaded==keys_freed&&!sessions);puts("PASS SDK parsed keyid-only exec/test → actual API config/session credentials and cleanup; existing password/path and shell/forward auth");
}
int main(void) {mkdir("remote",0700);mkdir("remote/.ssh",0700);assert(ts_cmd_ssh_register()==ESP_OK&&ts_cmd_hosts_register()==ESP_OK&&registered_ssh&&registered_hosts);
'''
case_functions={'revoke-api':'check_revoke(false);','revoke-cli':'check_revoke(true);','known':'check_known();','deployed':'check_deployed();','auth':'check_auth();'}
code += ''.join(case_functions.values()) if args.case=='all' else case_functions[args.case]
code += r'''
    assert(!sessions&&!view_readers&&keys_loaded==keys_freed);arg_freetable((void**)&s_ssh_args,sizeof(s_ssh_args)/sizeof(void*));arg_freetable((void**)&s_hosts_args,sizeof(s_hosts_args)/sizeof(void*));puts("PASS local SSH controls");return 0;
}
'''
headers={}
for directory in ['components/ts_security/include','components/ts_automation/include']:
    headers.update({p.name:p.read_text() for p in Path(directory).glob('*.h')})
argtable=IDF/'components/console/argtable3'
headers['argtable3/argtable3.h']=(argtable/'argtable3.h').read_text()
run_native(code,[IDF/'components/json/cJSON/cJSON.c',*sorted(argtable.glob('*.c'))],headers)
