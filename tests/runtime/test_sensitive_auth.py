"""Exercise production credentials, sessions and HTTP authorization offline."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

source = Path('components/ts_webui/src/ts_webui_api.c').read_text()


def extract(name):
    match = re.search(r'^static [^\n;]+\b' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.start()) + 2] + '\n'


code = r'''
#define _POSIX_C_SOURCE 200809L
#define CONFIG_TS_WEBUI_AUTH_REQUIRED 1
#define API_PREFIX "/api/v1"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
typedef int esp_err_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NOT_FOUND=1, ESP_ERR_NOT_ALLOWED=2, TS_API_OK=0, TS_API_ERR_NOT_FOUND=3 };
typedef enum { TS_PERM_NONE=0, TS_PERM_READ=1, TS_PERM_ADMIN=3, TS_PERM_ROOT=4 } ts_perm_level_t;
typedef struct { int content_len; } httpd_req_t;
typedef struct { httpd_req_t *req; const char *uri; int method; const char *body; size_t body_len; const char *auth; const char *path; } ts_http_request_t;
typedef struct { int code; char *message; cJSON *data; } ts_api_result_t;
static int status, calls;
static char *response;
#define TS_LOGD(...) ((void)0)
#define TS_LOGI(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define TS_API_MALLOC(size) malloc(size)
static const char *esp_err_to_name(int e) {(void)e; return "error";}
static int64_t esp_timer_get_time(void) {return 0;}
static size_t httpd_req_get_url_query_len(httpd_req_t *r) {(void)r;return 0;}
static esp_err_t httpd_req_get_url_query_str(httpd_req_t *r,char *b,size_t n) {(void)r;(void)b;(void)n;return ESP_FAIL;}
static esp_err_t ts_http_get_header(ts_http_request_t *r,const char *key,char *out,size_t n) {
    (void)key;if (!r->auth) return ESP_ERR_NOT_FOUND;snprintf(out,n,"Bearer %s",r->auth);return ESP_OK;
}
static esp_err_t ts_auth_validate_request(const char *header,uint32_t *session,ts_perm_level_t *level) {
    if (!strcmp(header,"Bearer root")) {*session=1;*level=TS_PERM_ROOT;return ESP_OK;}
    if (!strcmp(header,"Bearer admin")) {*session=2;*level=TS_PERM_ADMIN;return ESP_OK;}
    return ESP_ERR_NOT_FOUND;
}
static esp_err_t ts_http_send_error(ts_http_request_t *r,int code,const char *message) {
    (void)r;(void)message;status=code;return ESP_OK;
}
static esp_err_t ts_http_send_json(ts_http_request_t *r,int code,const char *json) {
    (void)r;status=code;free(response);response=strdup(json);return ESP_OK;
}
static esp_err_t ts_http_get_query_param(ts_http_request_t *r,const char *key,char *out,size_t n) {
    (void)key;if(!r->path)return ESP_ERR_NOT_FOUND;snprintf(out,n,"%s",r->path);return ESP_OK;
}
static esp_err_t ts_http_send_file(ts_http_request_t *r,const char *path) {(void)r;(void)path;status=200;return ESP_OK;}
static bool ts_storage_exists(const char *path) {(void)path;return true;}
static bool ts_storage_is_dir(const char *path) {(void)path;return false;}
static esp_err_t ts_storage_check_write_path(const char *path,bool children) {
    (void)children;return strstr(path,"/config/automation/")||!strncmp(path,"/autocfg/",9)?ESP_ERR_NOT_ALLOWED:ESP_OK;
}
static cJSON *ts_api_get_info(const char *name) {
    cJSON *info=cJSON_CreateObject();
    bool required=!strcmp(name,"automation.sources.export")||!strcmp(name,"automation.actions.add")||!strcmp(name,"ui.widgets.set")||
        !strncmp(name,"automation.services.",20)||!strcmp(name,"automation.rules.trigger")||!strcmp(name,"automation.actions.execute");
    cJSON_AddBoolToObject(info,"requires_auth",required);return info;
}
static esp_err_t ts_api_call(const char *name,const cJSON *request,ts_api_result_t *result) {
    (void)request;++calls;result->code=0;result->data=cJSON_CreateObject();
    if(!strcmp(name,"automation.sources.list")) {
        cJSON *items=cJSON_AddArrayToObject(result->data,"sources"),*row=cJSON_CreateObject();
        cJSON_AddStringToObject(row,"id","weather");cJSON_AddStringToObject(row,"rest_auth","secret");cJSON_AddStringToObject(row,"auth_header","alias-secret");cJSON_AddItemToArray(items,row);
    }
    return ESP_OK;
}
static void ts_api_result_free(ts_api_result_t *r) {free(r->message);cJSON_Delete(r->data);}
static bool is_binary_upload_complete(ts_http_request_t *r) {(void)r;return true;}
static cJSON *ts_config_pack_parse_import_request(const char *body,size_t len) {(void)len;return cJSON_Parse(body);}
static esp_err_t send_key_generate_result(ts_http_request_t *r,ts_api_result_t *result,const char *id,int64_t start) {
    (void)r;(void)result;(void)id;(void)start;return ESP_OK;
}
'''
code += extract('check_auth') + extract('api_handler') + extract('url_decode') + extract('file_download_handler')
code += r'''
static void api(const char *uri,const char *body,const char *auth,int expected_status) {
    httpd_req_t native={0};ts_http_request_t req={.req=&native,.uri=uri,.method=1,.body=body,.body_len=body?strlen(body):0,.auth=auth};
    status=0;free(response);response=NULL;api_handler(&req,NULL);assert(status==expected_status);
}
static void file(const char *path,const char *auth,int expected_status) {
    ts_http_request_t req={.path=path,.auth=auth};status=0;file_download_handler(&req,NULL);assert(status==expected_status);
}
int main(void) {
    api("/api/v1/automation/sources/list",NULL,NULL,200);assert(strstr(response,"weather")&&!strstr(response,"secret"));
    api("/api/v1/automation/sources/list",NULL,"admin",200);assert(strstr(response,"weather")&&!strstr(response,"secret"));
    api("/api/v1/automation/sources/list",NULL,"root",200);assert(strstr(response,"secret")&&strstr(response,"alias-secret"));
    int before=calls;
    api("/api/v1/automation/sources/export","{}",NULL,401);assert(calls==before);
    api("/api/v1/automation/sources/export","{}","admin",401);assert(calls==before);
    api("/api/v1/automation/sources/export","{}","root",200);assert(calls==before+1);
    api("/api/v1/automation/actions/add","{}","admin",401);
    const char *home_operations[]={"services/list","services/status","services/stop","rules/trigger","actions/execute"};
    for(unsigned i=0;i<sizeof(home_operations)/sizeof(home_operations[0]);++i){
        char uri[96];snprintf(uri,sizeof(uri),"/api/v1/automation/%s",home_operations[i]);
        before=calls;api(uri,"{}",NULL,401);assert(calls==before);
        api(uri,"{}","admin",200);assert(calls==before+1);
        api(uri,"{}","root",200);assert(calls==before+2);
    }
    api("/api/v1/ui/widgets/set","{}",NULL,401);
    api("/api/v1/ui/widgets/set","{}","admin",200);
    api("/api/v1/storage/read","{\"path\":\"/sdcard/config/automation/state.json\"}",NULL,401);
    api("/api/v1/storage/read","{\"Path\":\"/sdcard/config/automation/state.json\"}",NULL,401);
    api("/api/v1/storage/read","{\"path\":\"/sdcard/config/automation/state.json\"}","root",200);
    api("/api/v1/storage/read","{\"path\":\"/sdcard/hello.txt\"}",NULL,200);
    file("/sdcard/config/automation/state.json",NULL,401);
    file("/sdcard/config/automation/state.json","admin",401);
    file("/sdcard/config/automation/state.json","root",200);
    file("/sdcard/hello.txt",NULL,200);
    free(response);puts("PASS source secrets, protected files, role policy and public reads");
}
'''

idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
cjson = idf / 'components/json/cJSON'
with tempfile.TemporaryDirectory(prefix='ts-sensitive-auth-') as directory:
    build = Path(directory)
    (build / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined', '-I' + str(cjson),
                    str(build / 'test.c'), str(cjson / 'cJSON.c'), '-o', str(build / 'test')], check=True)
    subprocess.run([str(build / 'test')], check=True)


# Run the production credential/session implementation with a persistent NVS
# fixture and a controllable boot clock. Crypto uses the SDK's actual routines.
auth_code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_security.h"
#include "ts_auth_layout.h"
#include "ts_crypto.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#define CONFIG_TS_SECURITY_TOKEN_EXPIRE_SEC 86400
#define TS_CONFIG_KEYS_PARTITION "keys"
#define NVS_READWRITE 1
#define TS_MALLOC_PSRAM malloc
typedef int nvs_handle_t;
static ts_auth_user_credential_t stored[2];
static bool present[2];
static uint8_t version;
static int writes;
static int open_handles, fail_write_user=-1;
static int64_t clock_us;
static uint32_t random_state=12345;
static char logs[8192];
static void capture_log(const char *tag,const char *format,...) {
    (void)tag;
    size_t used=strlen(logs);
    va_list ap;va_start(ap,format);vsnprintf(logs+used,sizeof(logs)-used,format,ap);va_end(ap);
}
#define TS_LOGI capture_log
#define TS_LOGW capture_log
#define TS_LOGE capture_log
static const char *esp_err_to_name(int e) {(void)e;return "fixture error";}
static int64_t esp_timer_get_time(void) {return clock_us;}
static void esp_fill_random(void *p,size_t n) {
    for(size_t i=0;i<n;i++){random_state=random_state*1664525+1013904223;((uint8_t *)p)[i]=random_state>>24;}
}
static int user_index(const char *key) {
    if(!strcmp(key,"cred_admin"))return 0;
    assert(!strcmp(key,"cred_root"));return 1;
}
static int nvs_open(const char *name,int mode,nvs_handle_t *handle) {(void)name;(void)mode;*handle=++open_handles;return ESP_OK;}
static int nvs_open_from_partition(const char *part,const char *name,int mode,nvs_handle_t *handle) {
    (void)part;return nvs_open(name,mode,handle);
}
static void nvs_close(nvs_handle_t handle) {(void)handle;assert(open_handles>0);--open_handles;}
static int nvs_get_blob(nvs_handle_t handle,const char *key,void *out,size_t *len) {
    (void)handle;int i=user_index(key);if(!present[i])return ESP_ERR_NOT_FOUND;
    assert(*len>=sizeof(stored[i]));memcpy(out,&stored[i],sizeof(stored[i]));*len=sizeof(stored[i]);return ESP_OK;
}
static int nvs_set_blob(nvs_handle_t handle,const char *key,const void *data,size_t len) {
    (void)handle;int i=user_index(key);assert(len==sizeof(stored[i]));
    if(i==fail_write_user){fail_write_user=-1;return ESP_FAIL;}
    memcpy(&stored[i],data,len);present[i]=true;++writes;return ESP_OK;
}
static int nvs_commit(nvs_handle_t handle) {(void)handle;return ESP_OK;}
static int nvs_get_u8(nvs_handle_t handle,const char *key,uint8_t *out) {(void)handle;(void)key;*out=version;return ESP_OK;}
static int nvs_set_u8(nvs_handle_t handle,const char *key,uint8_t value) {(void)handle;(void)key;version=value;return ESP_OK;}
static int nvs_erase_key(nvs_handle_t handle,const char *key) {(void)handle;(void)key;return ESP_OK;}
static int ts_ssh_hosts_config_init(void) {return ESP_OK;}
static int ts_ssh_commands_config_init(void) {return ESP_OK;}
static int ts_ssh_service_init(void) {return ESP_OK;}
static int ts_ssh_log_watch_init(void) {return ESP_OK;}
'''
auth_code = '#include <stdarg.h>\n' + auth_code
crypto_source = Path('components/ts_security/src/ts_crypto.c').read_text()
for name in ['ts_crypto_hash', 'ts_crypto_base64_encode', 'ts_crypto_base64_decode']:
    start = crypto_source.index('esp_err_t ' + name + '(')
    auth_code += crypto_source[start:crypto_source.index('\n}', start) + 2] + '\n'
for filename in ['ts_security.c', 'ts_auth.c']:
    implementation = Path('components/ts_security/src', filename).read_text()
    auth_code += '\n#undef TAG\n' + re.sub(r'^#include[^\n]*\n', '', implementation, flags=re.M)
auth_code += r'''
static void boot(void) {
    s_auth_initialized=false;s_initialized=false;clock_us=0;open_handles=0;
    assert(ts_security_init()==ESP_OK);assert(ts_auth_init()==ESP_OK);
}
static void fixture(void) {
    boot();
    assert(ts_auth_set_admin_password("admin-fixture")==ESP_OK);
    assert(ts_auth_set_root_password("root-fixture")==ESP_OK);
    logs[0]=0;
}
static uint32_t login(const char *user,const char *password,char *token) {
    uint32_t sid=0;assert(ts_auth_login(user,password,&sid,token,128)==ESP_OK);return sid;
}
static void request(const char *token,ts_perm_level_t expected) {
    char header[160];snprintf(header,sizeof(header),"Bearer %s",token);
    uint32_t sid=0;ts_perm_level_t level=TS_PERM_NONE;
    assert(ts_auth_validate_request(header,&sid,&level)==ESP_OK);assert(level==expected);
    assert(ts_security_check_permission(sid,TS_PERM_READ));
    assert(ts_security_check_permission(sid,TS_PERM_ROOT)==(expected==TS_PERM_ROOT));
}
static void lock(const char *user) {
    for(int i=0;i<5;i++)assert(ts_auth_verify_password(user,"wrong-password",NULL)==ESP_ERR_INVALID_ARG);
}
int main(int argc,char **argv) {
    assert(argc==2);fixture();const char *mode=argv[1];
    if(!strcmp(mode,"normal")) {
        char token[128];uint32_t sid=login("root","root-fixture",token);request(token,TS_PERM_ROOT);
        assert(ts_auth_logout(sid)==ESP_OK);assert(ts_security_validate_token(token,&sid)!=ESP_OK);
        sid=login("admin","admin-fixture",token);request(token,TS_PERM_ADMIN);
        assert(ts_auth_logout(sid)==ESP_OK);
        login("root","root-fixture",token);request(token,TS_PERM_ROOT);
        assert(ts_auth_password_changed("admin")&&ts_auth_password_changed("root"));
    } else if(!strcmp(mode,"sid_only")||!strcmp(mode,"altered_tail")) {
        char token[128],forged[128];uint32_t sid=login("root","root-fixture",token),result=0;
        uint8_t raw[16];size_t n=sizeof(raw);assert(ts_crypto_base64_decode(token,strlen(token),raw,&n)==ESP_OK);
        if(!strcmp(mode,"sid_only")) {
            size_t encoded=sizeof(forged);assert(ts_crypto_base64_encode(raw,4,forged,&encoded)==ESP_OK);
            assert(ts_security_validate_token(forged,&result)!=ESP_OK);
        } else for(size_t byte=0;byte<n;byte++) {
            raw[byte]^=1;
            size_t encoded=sizeof(forged);assert(ts_crypto_base64_encode(raw,n,forged,&encoded)==ESP_OK);
            assert(ts_security_validate_token(forged,&result)!=ESP_OK);raw[byte]^=1;
        }
        request(token,TS_PERM_ROOT);assert(ts_auth_logout(sid)==ESP_OK);
    } else if(!strcmp(mode,"lengths_and_unissued")) {
        uint32_t sid=0,out=0;assert(ts_security_create_session("root",TS_PERM_ROOT,&sid)==ESP_OK);
        uint8_t raw[17]={0};memcpy(raw,&sid,4);char token[128];
        for(size_t len=0;len<=sizeof(raw);len++) {
            size_t n=sizeof(token);assert(ts_crypto_base64_encode(raw,len,token,&n)==ESP_OK);
            assert(ts_security_validate_token(token,&out)!=ESP_OK);
        }
        assert(ts_security_validate_token("not-base64",&out)!=ESP_OK);
    } else if(!strcmp(mode,"expiry_and_reuse")) {
        char first[128],second[128];uint32_t before_random=random_state;
        uint32_t sid=login("root","root-fixture",first),out=0,reused=0;
        assert(ts_auth_logout(sid)==ESP_OK);
        random_state=before_random;
        assert(ts_security_create_session("admin",TS_PERM_ADMIN,&reused)==ESP_OK&&reused==sid);
        assert(ts_security_validate_token(first,&out)!=ESP_OK);
        random_state^=0x12345; /* Repeat only the SID, not the next random credential. */
        assert(ts_security_generate_token(reused,second,sizeof(second))==ESP_OK);
        assert(ts_security_validate_token(first,&out)!=ESP_OK);request(second,TS_PERM_ADMIN);
        clock_us=(int64_t)(CONFIG_TS_SECURITY_TOKEN_EXPIRE_SEC+1)*1000000;
        assert(ts_security_validate_token(second,&out)!=ESP_OK);
        login("root","root-fixture",second);request(second,TS_PERM_ROOT);
    } else if(!strcmp(mode,"lockout")) {
        clock_us=7LL*86400*1000000;lock("root");
        assert(ts_auth_verify_password("root","root-fixture",NULL)==ESP_ERR_INVALID_STATE);
        assert(ts_auth_init()==ESP_OK);
        assert(ts_auth_verify_password("root","root-fixture",NULL)==ESP_ERR_INVALID_STATE);
        assert(ts_auth_verify_password("admin","admin-fixture",NULL)==ESP_OK);
        clock_us+=299LL*1000000;
        assert(ts_auth_verify_password("root","root-fixture",NULL)==ESP_ERR_INVALID_STATE);
        clock_us+=1000000;assert(ts_auth_verify_password("root","root-fixture",NULL)==ESP_OK);
        assert(stored[1].failed_attempts==0&&stored[1].lockout_until==0);
        lock("root");assert(ts_auth_verify_password("root","root-fixture",NULL)==ESP_ERR_INVALID_STATE);
    } else if(!strcmp(mode,"reboot_locked")||!strcmp(mode,"reboot_expired")) {
        clock_us=7LL*86400*1000000;lock("root");
        assert(ts_auth_verify_password("admin","wrong-password",NULL)==ESP_ERR_INVALID_ARG);
        if(!strcmp(mode,"reboot_expired")) {
            clock_us+=300LL*1000000;assert(ts_auth_verify_password("root","root-fixture",NULL)==ESP_OK);
            assert(stored[1].failed_attempts==0&&stored[1].lockout_until==0);
        }
        ts_auth_user_credential_t root=stored[1],admin=stored[0];boot();
        assert(!memcmp(root.salt,stored[1].salt,TS_AUTH_SALT_LEN));
        assert(!memcmp(root.hash,stored[1].hash,TS_AUTH_HASH_LEN));
        assert(root.password_changed==stored[1].password_changed);
        assert(!memcmp(admin.salt,stored[0].salt,TS_AUTH_SALT_LEN));
        assert(!memcmp(admin.hash,stored[0].hash,TS_AUTH_HASH_LEN));
        assert(admin.password_changed==stored[0].password_changed&&version==TS_AUTH_CONFIG_VERSION);
        char token[128];login("root","root-fixture",token);request(token,TS_PERM_ROOT);
        assert(stored[0].failed_attempts==0&&stored[0].lockout_until==0);
        lock("root");assert(ts_auth_verify_password("root","root-fixture",NULL)==ESP_ERR_INVALID_STATE);
    } else if(!strcmp(mode,"clean_boot")) {
        int before=writes;boot();assert(writes==before);
        char token[128];login("root","root-fixture",token);request(token,TS_PERM_ROOT);
    } else if(!strcmp(mode,"init_failure_admin")||!strcmp(mode,"init_failure_root")) {
        int user=!strcmp(mode,"init_failure_admin")?0:1;
        stored[user].lockout_until=300;stored[user].failed_attempts=1;
        ts_auth_user_credential_t original=stored[user];
        s_auth_initialized=false;s_initialized=false;clock_us=0;open_handles=0;
        assert(ts_security_init()==ESP_OK);int before=open_handles;fail_write_user=user;
        assert(ts_auth_init()==ESP_FAIL);assert(!s_auth_initialized&&open_handles==before);
        assert(!memcmp(&original,&stored[user],sizeof(original)));
        assert(ts_auth_init()==ESP_OK);assert(open_handles==before+1);
        assert(!stored[user].lockout_until&&!stored[user].failed_attempts);
        char token[128];login(user?"root":"admin",user?"root-fixture":"admin-fixture",token);
        request(token,user?TS_PERM_ROOT:TS_PERM_ADMIN);
        before=open_handles;assert(ts_auth_init()==ESP_OK&&open_handles==before);
    } else if(!strcmp(mode,"logs")) {
        char token[128],sid_text[16];uint32_t sid=login("root","root-fixture",token);
        snprintf(sid_text,sizeof(sid_text),"%08lx",(unsigned long)sid);
        assert(!strstr(logs,sid_text)&&!strstr(logs,token));assert(strstr(logs,"root"));
        assert(ts_auth_logout(sid)==ESP_OK);assert(!strstr(logs,sid_text));
    } else assert(false);
    printf("PASS credential/session %s\n",mode);
}
'''
with tempfile.TemporaryDirectory(prefix='ts-auth-session-') as directory:
    build = Path(directory)
    (build / 'auth.c').write_text(auth_code)
    mbed = idf / 'components/mbedtls/mbedtls'
    subprocess.run(['cc', '-std=gnu11', '-g', '-fsanitize=address,undefined',
                    '-Itests/runtime/rule_pack/stubs', '-Icomponents/ts_security/include',
                    '-Icomponents/ts_auth_layout/include', '-I' + str(mbed / 'include'),
                    str(build / 'auth.c'), *[str(mbed / 'library' / name) for name in
                                            ['base64.c', 'sha256.c', 'sha512.c', 'platform_util.c']],
                    '-o', str(build / 'auth')], check=True)
    failed = []
    for case in ['normal', 'sid_only', 'altered_tail', 'lengths_and_unissued',
                 'expiry_and_reuse', 'lockout', 'reboot_locked', 'reboot_expired', 'clean_boot',
                 'init_failure_admin', 'init_failure_root', 'logs']:
        result = subprocess.run([str(build / 'auth'), case])
        if result.returncode:
            failed.append(case)
    assert not failed, failed
