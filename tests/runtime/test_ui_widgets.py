"""Check actual widget reads against simulated NVS outcomes."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

source = Path('components/ts_api/src/ts_api_ui.c').read_text()


def extract(name):
    match = re.search(r'^static [^\n;]+\b' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.start()) + 2] + '\n'


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef struct {int code;char *message;cJSON *data;} ts_api_result_t;
enum { ESP_OK=0,ESP_FAIL=1,ESP_ERR_NVS_NOT_FOUND=2,ESP_ERR_NO_MEM=3,ESP_ERR_INVALID_RESPONSE=4,
       TS_API_ERR_NO_MEM=5,TS_API_ERR_INTERNAL=6,NVS_READONLY=0,MALLOC_CAP_SPIRAM=0 };
#define UI_WIDGETS_NVS_NS "ts_ui"
#define UI_WIDGETS_NVS_KEY "widgets"
#define TS_LOGW(...) ((void)0)
#define TS_LOGI(...) ((void)0)
static int scenario;
static const char *blob;
static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h) {
 (void)name;(void)mode;*h=1;return scenario==1?ESP_ERR_NVS_NOT_FOUND:scenario==2?ESP_FAIL:ESP_OK;
}
static void nvs_close(nvs_handle_t h){(void)h;}
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size) {
 (void)h;(void)key;if(scenario==3)return ESP_ERR_NVS_NOT_FOUND;if(scenario==4)return ESP_FAIL;
 if(!out){*size=strlen(blob);return ESP_OK;}if(scenario==5)return ESP_FAIL;
 memcpy(out,blob,*size);return ESP_OK;
}
static void *host_malloc(size_t size){return scenario==6?NULL:malloc(size);}
static void *heap_caps_malloc(size_t size,int caps){(void)caps;return host_malloc(size);}
static void ts_api_result_error(ts_api_result_t *r,int code,const char *message){r->code=code;r->message=strdup(message);}
static void ts_api_result_ok(ts_api_result_t *r,cJSON *data){r->code=0;r->data=data;}
#define malloc host_malloc
'''
code += extract('load_widgets_from_nvs') + extract('api_ui_widgets_get')
code += r'''
#undef malloc
static void clear(ts_api_result_t *r){free(r->message);cJSON_Delete(r->data);memset(r,0,sizeof(*r));}
int main(void){
 ts_api_result_t result={0};
 for(scenario=1;scenario<=3;scenario+=2){api_ui_widgets_get(NULL,&result);assert(!result.code&&!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(result.data,"source")),"default"));clear(&result);}
 for(scenario=2;scenario<=6;++scenario){if(scenario==3)continue;blob="{\"widgets\":[]}";api_ui_widgets_get(NULL,&result);assert(result.code!=0&&!result.data);clear(&result);}
 scenario=0;blob="invalid";api_ui_widgets_get(NULL,&result);assert(result.code!=0&&!result.data);clear(&result);
 blob="{\"widgets\":\"invalid\"}";api_ui_widgets_get(NULL,&result);assert(result.code!=0&&!result.data);clear(&result);
 blob="{\"widgets\":[{\"id\":\"saved\"}],\"refresh_interval\":1234}";
 assert(api_ui_widgets_get(NULL,&result)==ESP_OK&&!result.code);
 assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(result.data,"source")),"nvs"));
 assert(cJSON_GetArraySize(cJSON_GetObjectItem(result.data,"widgets"))==1);clear(&result);
 puts("PASS widget reads distinguish absent, I/O, allocation, malformed and saved data");
}
'''
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
cjson = idf / 'components/json/cJSON'
with tempfile.TemporaryDirectory(prefix='ts-widget-read-') as directory:
    build = Path(directory)
    (build / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                    '-I' + str(cjson), str(build / 'test.c'), str(cjson / 'cJSON.c'), '-o', str(build / 'test')], check=True)
    subprocess.run([str(build / 'test')], check=True)
