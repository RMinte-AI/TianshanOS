#!/usr/bin/env python3
"""Exercise the real delete handlers without deleting any device configuration."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
os.chdir(root)
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))

def handler(path, name):
    source = Path(path).read_text()
    start = source.index('static esp_err_t ' + name + '(')
    return source[start:source.index('\n/**', start)]

code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_api.h"
#define ACTIONS_SDCARD_DIR "/sdcard/config/actions"
#define TAG "test"
#define TS_LOGI(...) ((void)0)
static esp_err_t outcome;
static unsigned remove_calls, unlinks;
static int test_unlink(const char *path) { assert(strstr(path, "/fixture.")); ++unlinks; return 0; }
#define unlink test_unlink
esp_err_t ts_action_template_remove(const char *id) { assert(!strcmp(id,"fixture")); ++remove_calls; return outcome; }
esp_err_t ts_ssh_commands_config_remove(const char *id) { return ts_action_template_remove(id); }
void ts_api_result_ok(ts_api_result_t *result, cJSON *data) { result->code=TS_API_OK; result->data=data; }
void ts_api_result_error(ts_api_result_t *result, ts_api_result_code_t code, const char *message) { result->code=code; result->message=strdup(message); }
'''
code += handler('components/ts_api/src/ts_api_automation.c', 'api_automation_actions_delete')
code += handler('components/ts_api/src/ts_api_ssh.c', 'api_ssh_commands_remove')
code += r'''
int main(void) {
 cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,"id","fixture");
 esp_err_t outcomes[]={ESP_OK,ESP_ERR_NOT_FOUND,ESP_ERR_INVALID_STATE,ESP_FAIL};
 ts_api_result_code_t expected[]={TS_API_OK,TS_API_ERR_NOT_FOUND,TS_API_ERR_BUSY,TS_API_ERR_INTERNAL};
 for(unsigned api=0;api<2;api++)for(unsigned i=0;i<4;i++) {
  outcome=outcomes[i];remove_calls=unlinks=0;ts_api_result_t result={0};
  esp_err_t ret=api?api_ssh_commands_remove(params,&result):api_automation_actions_delete(params,&result);
  assert(ret==ESP_OK && result.code==expected[i] && remove_calls==1);
  if(outcome==ESP_ERR_INVALID_STATE)assert(!strcmp(result.message,"service_delete_protected"));
  assert(unlinks==(outcome==ESP_OK && !api?2:0));
  free(result.message);cJSON_Delete(result.data);
 }
 cJSON_Delete(params);
 puts("delete API: protected services report BUSY, denied deletes preserve files, missing/internal errors stay distinct");
}
'''
with tempfile.TemporaryDirectory(prefix='tianshan-delete-api-') as tmp:
    source=Path(tmp)/'delete.c';binary=Path(tmp)/'delete'
    source.write_text(code)
    env=dict(os.environ,DEVELOPER_DIR=os.environ.get('DEVELOPER_DIR','/Library/Developer/CommandLineTools'))
    subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined','-Wno-deprecated-declarations','-Itests/runtime/stubs','-Itests/certificate/stubs','-Icomponents/ts_api/include','-I'+str(idf/'components/json/cJSON'),str(source),str(idf/'components/json/cJSON/cJSON.c'),'-lm','-o',str(binary)],check=True,env=env)
    subprocess.run([str(binary)],check=True,env=env)
