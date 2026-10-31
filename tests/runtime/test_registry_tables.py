#!/usr/bin/env python3
"""Exercise production registry lookups and their caller-visible results."""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
os.chdir(root)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-ref', help='Replay a local Git revision without changing the worktree')
args = parser.parse_args()
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
cjson = idf / 'components/json/cJSON'


def source(path):
    if args.source_ref:
        return subprocess.check_output(['git', 'show', f'{args.source_ref}:{path}'], text=True)
    return Path(path).read_text()


# Compile the actual API registry, excluding only the all-component aggregator.
# Hardware allocation and synchronization use the existing host stubs.
api = source('components/ts_api/src/ts_api.c')
api = api[:api.index('\nesp_err_t ts_api_register_all(void)')]
api = re.sub(r'^#include[^\n]*\n', '', api, flags=re.M)
code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_api.h"
#include "freertos/semphr.h"
#define TS_LOGD(...) ((void)0)
#define TS_LOGI(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define TS_STRDUP_PSRAM strdup
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static void *heap_caps_calloc(size_t n, size_t size, unsigned caps) {
    (void)caps; return calloc(n, size);
}
static cJSON *ts_config_pack_parse_import_request(const char *s, size_t n) {
    (void)n; return cJSON_Parse(s);
}
'''
code += api
code += r'''
static esp_err_t fixture_handler(const cJSON *params, ts_api_result_t *result) {
    (void)params; ts_api_result_ok(result, cJSON_CreateObject()); return ESP_OK;
}
int main(void) {
    static const char *expected[] = {
        "system", "config", "hal", "led", "fan", "power", "network",
        "device", "storage", "security"
    };
    assert(sizeof(expected) / sizeof(expected[0]) == TS_API_CAT_MAX);
    assert(ts_api_init() == ESP_OK);
    for (int i = 0; i < TS_API_CAT_MAX; ++i) {
        assert(!strcmp(ts_api_category_name(i), expected[i]));
        assert(ts_api_category_by_name(expected[i]) == i);
        char name[64]; snprintf(name, sizeof(name), "%s.fixture", expected[i]);
        ts_api_endpoint_t endpoint = {
            .name = name, .description = "Registry fixture", .category = i,
            .handler = fixture_handler, .requires_auth = true, .permission = "admin"
        };
        assert(ts_api_register(&endpoint) == ESP_OK);
        cJSON *info = ts_api_get_info(name);
        assert(info);
        /* Check the serialized metadata consumed by the HTTP policy lookup. */
        char *wire = cJSON_PrintUnformatted(info); cJSON_Delete(info);
        info = cJSON_Parse(wire); free(wire); assert(info);
        assert(!strcmp(cJSON_GetObjectItem(info, "name")->valuestring, name));
        assert(!strcmp(cJSON_GetObjectItem(info, "category")->valuestring, expected[i]));
        assert(cJSON_IsTrue(cJSON_GetObjectItem(info, "requires_auth")));
        assert(!strcmp(cJSON_GetObjectItem(info, "permission")->valuestring, "admin"));
        cJSON_Delete(info);
        cJSON *list = ts_api_list(i); assert(cJSON_GetArraySize(list) == 1);
        assert(!strcmp(cJSON_GetArrayItem(list, 0)->valuestring, name)); cJSON_Delete(list);
    }
    assert(!strcmp(ts_api_category_name(TS_API_CAT_MAX), "unknown"));
    assert(ts_api_category_by_name("missing") == TS_API_CAT_MAX);
    assert(ts_api_category_by_name(NULL) == TS_API_CAT_MAX);
    assert(ts_api_get_info("missing") == NULL);
    assert(ts_api_deinit() == ESP_OK);
    puts("PASS all API categories, reverse lookup and serialized get_info metadata");
}
'''

with tempfile.TemporaryDirectory(prefix='ts-registry-tables-') as directory:
    build = Path(directory)
    (build / 'test.c').write_text(code)
    subprocess.run([
        'cc', '-std=gnu11', '-g', '-Wno-deprecated-declarations',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-Itests/runtime/storage_stubs', '-Icomponents/ts_api/include', '-I' + str(cjson),
        str(build / 'test.c'), str(cjson / 'cJSON.c'), '-lpthread', '-o', str(build / 'test')
    ], check=True)
    subprocess.run([str(build / 'test')], check=True)
