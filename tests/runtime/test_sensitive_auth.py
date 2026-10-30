"""Exercise production HTTP authorization with local API/storage boundaries."""
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
