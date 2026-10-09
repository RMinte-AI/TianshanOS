#!/usr/bin/env python3
"""Run the actual CSR API validation without creating device keys or certificates."""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
os.chdir(root)
idf=Path(os.environ.get('IDF_PATH','/Users/massif/esp/v5.5.2/esp-idf'))
source=Path('components/ts_api/src/ts_api_cert.c').read_text()
start=source.index('static esp_err_t api_cert_generate_csr(')
handler=source[start:source.index('\n}',start)+2]
code=r'''
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "ts_api.h"
#include "ts_cert.h"
#include "ts_cert_subject.h"
#define TAG "test"
#define TS_LOGI(...) ((void)0)
#define TS_LOGE(...) ((void)0)
static int checked_keys, custom_calls, default_calls;
static char received_id[64], received_org[256];
bool ts_cert_has_keypair(void) { ++checked_keys;return true; }
void *heap_caps_malloc(size_t n,unsigned c) { return malloc(n); }
esp_err_t ts_cert_generate_csr(const ts_cert_csr_opts_t *o,char *pem,size_t *n) {
 ++custom_calls;strcpy(received_id,o->device_id);strcpy(received_org,o->organization?o->organization:"");
 strcpy(pem,"synthetic-output");return ESP_OK;
}
esp_err_t ts_cert_generate_csr_default(char *pem,size_t *n) { ++default_calls;strcpy(pem,"synthetic-default");return ESP_OK; }
void ts_api_result_error(ts_api_result_t *r,ts_api_result_code_t c,const char *s) { r->code=c;r->message=strdup(s); }
void ts_api_result_ok(ts_api_result_t *r,cJSON *j) { r->code=TS_API_OK;r->data=j; }
static void dispose(ts_api_result_t *r) { free(r->message);cJSON_Delete(r->data);memset(r,0,sizeof(*r)); }
'''+handler+r'''
int main(void) {
 const char *bad[]={"{\"device_id\":12}","{\"organization\":null}","{\"org_unit\":false}","{\"device_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"};
 for(unsigned i=0;i<4;i++) { cJSON *p=cJSON_Parse(bad[i]);ts_api_result_t r={0};assert(api_cert_generate_csr(p,&r)!=ESP_OK&&r.code==TS_API_ERR_INVALID_ARG);dispose(&r);cJSON_Delete(p); }
 assert(checked_keys==0&&custom_calls==0&&default_calls==0);
 cJSON *p=cJSON_Parse("{\"organization\":\"ACME, Ltd\"}");ts_api_result_t r={0};assert(api_cert_generate_csr(p,&r)==ESP_OK&&r.code==TS_API_OK);assert(!strcmp(received_id,TS_CERT_DEFAULT_DEVICE_ID)&&!strcmp(received_org,"ACME, Ltd"));dispose(&r);cJSON_Delete(p);
 p=cJSON_Parse("{\"device_id\":\"\",\"org_unit\":\"Device\"}");assert(api_cert_generate_csr(p,&r)==ESP_OK&&!strcmp(received_id,TS_CERT_DEFAULT_DEVICE_ID));dispose(&r);cJSON_Delete(p);
 p=cJSON_CreateObject();cJSON_AddStringToObject(p,"device_id","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");assert(api_cert_generate_csr(p,&r)==ESP_OK&&strlen(received_id)==63);dispose(&r);
 char huge[256];memset(huge,'x',255);huge[255]=0;cJSON_AddStringToObject(p,"organization",huge);int keys=checked_keys,calls=custom_calls;assert(api_cert_generate_csr(p,&r)!=ESP_OK&&r.code==TS_API_ERR_INVALID_ARG&&checked_keys==keys&&custom_calls==calls);dispose(&r);cJSON_Delete(p);
 assert(api_cert_generate_csr(NULL,&r)==ESP_OK&&default_calls==1);dispose(&r);
 puts("PASS actual CSR API: reject wrong types/overlong inputs before key access, organization-only/empty ID use default, exact 63-byte ID, no credential changes");
}
'''
with tempfile.TemporaryDirectory(prefix='ts-csr-api-') as tmp:
    file=Path(tmp)/'test.c';binary=Path(tmp)/'test';file.write_text(code)
    includes=['tests/certificate/stubs','components/ts_api/include','components/ts_cert/include',str(idf/'components/json/cJSON')]
    env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
    subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined',*['-I'+p for p in includes],str(file),
                    'components/ts_cert/src/ts_cert_subject.c',str(idf/'components/json/cJSON/cJSON.c'),'-lm','-o',str(binary)],check=True,env=env)
    subprocess.run([str(binary)],check=True,env=env)
