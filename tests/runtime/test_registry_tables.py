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
parser.add_argument('--only', choices=['categories', 'modules', 'pins'], help='Run one registry regression')
args = parser.parse_args()
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
cjson = idf / 'components/json/cJSON'


def source(path):
    if args.source_ref:
        return subprocess.check_output(['git', 'show', f'{args.source_ref}:{path}'], text=True)
    return Path(path).read_text()


def function(text, name):
    match = re.search(r'^(?:static )?[^\n;]+\b' + name + r'\([^;]+?\)\s*\{', text, re.M)
    assert match, name
    return text[match.start():text.index('\n}', match.start()) + 2] + '\n'


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
#include <strings.h>
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
static void test_categories(void) {
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

config = source('components/ts_api/src/ts_api_config.c')
code += r'''
#include "ts_config_module.h"
bool ts_config_module_is_registered(ts_config_module_t mod) { return mod % 2 != 0; }
const char *ts_config_module_get_nvs_namespace(ts_config_module_t mod) { (void)mod; return "fixture"; }
uint16_t ts_config_module_get_schema_version(ts_config_module_t mod) { (void)mod; return 7; }
bool ts_config_module_is_dirty(ts_config_module_t mod) { return mod == TS_CONFIG_MODULE_FAN; }
static uint32_t ts_config_meta_get_global_seq(void) { return 42; }
'''
for name in ['module_to_name', 'parse_module_name', 'api_config_module_list']:
    code += function(config, name)
code += r'''
static void test_modules(void) {
    static const char *expected[] = {
        "net", "dhcp", "wifi", "nat", "led", "fan", "device", "system",
        "temp", "rules", "actions", "sources"
    };
    assert(sizeof(expected) / sizeof(expected[0]) == TS_CONFIG_MODULE_MAX);
    assert(ts_api_init() == ESP_OK);
    ts_api_endpoint_t endpoint = {
        .name = "config.module.list", .category = TS_API_CAT_CONFIG,
        .handler = api_config_module_list
    };
    assert(ts_api_register(&endpoint) == ESP_OK);
    ts_api_result_t result = {0};
    assert(ts_api_call_str("config.module.list", NULL, &result) == ESP_OK);
    char *wire = ts_api_result_to_string(&result); ts_api_result_free(&result);
    cJSON *json = cJSON_Parse(wire); free(wire); assert(json);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(json, "success")));
    cJSON *data = cJSON_GetObjectItem(json, "data");
    assert(cJSON_GetObjectItem(data, "global_seq")->valueint == 42);
    cJSON *modules = cJSON_GetObjectItem(data, "modules");
    assert(cJSON_GetArraySize(modules) == TS_CONFIG_MODULE_MAX);
    for (int i = 0; i < TS_CONFIG_MODULE_MAX; ++i) {
        cJSON *item = cJSON_GetArrayItem(modules, i);
        assert(!strcmp(cJSON_GetObjectItem(item, "name")->valuestring, expected[i]));
        assert(cJSON_IsTrue(cJSON_GetObjectItem(item, "registered")) == (i % 2 != 0));
        if (i % 2 != 0) {
            assert(!strcmp(cJSON_GetObjectItem(item, "nvs_namespace")->valuestring, "fixture"));
            assert(cJSON_GetObjectItem(item, "version")->valueint == 7);
            assert(cJSON_IsTrue(cJSON_GetObjectItem(item, "dirty")) == (i == TS_CONFIG_MODULE_FAN));
        } else {
            assert(!cJSON_HasObjectItem(item, "nvs_namespace"));
            assert(!cJSON_HasObjectItem(item, "version"));
            assert(!cJSON_HasObjectItem(item, "dirty"));
        }
        /* Listing extra modules must not expand the existing read/write parser. */
        assert(parse_module_name(expected[i]) == (i < 8 ? i : TS_CONFIG_MODULE_MAX + 1));
    }
    assert(parse_module_name("all") == TS_CONFIG_MODULE_MAX);
    assert(parse_module_name("NeT") == TS_CONFIG_MODULE_NET);
    assert(!strcmp(module_to_name(TS_CONFIG_MODULE_MAX), "unknown"));
    cJSON_Delete(json);
    assert(ts_api_deinit() == ESP_OK);
    puts("PASS serialized module.list: all 12 names, registered metadata and unchanged parser");
}
'''

# Compile initialization/default mappings and public in-memory pin operations.
# No GPIO driver or persistence operation is used by this regression.
pins = source('components/ts_hal/src/ts_pin_manager.c')
prefix = pins[:pins.index('\nesp_err_t ts_pin_manager_load_config(')]
prefix = re.sub(r'^#include[^\n]*\n|^#define TAG[^\n]*\n', '', prefix, flags=re.M)
code += '\n#include "ts_pin_manager.h"\n#include "soc/soc_caps.h"\n#define CONFIG_IDF_TARGET_ESP32S3 1\n'
code += prefix
for name in ['ts_pin_manager_load_defaults', 'ts_pin_manager_set_mapping',
             'ts_pin_manager_get_gpio', 'ts_pin_manager_get_function',
             'ts_pin_manager_acquire', 'ts_pin_manager_release',
             'ts_pin_manager_is_available', 'ts_pin_manager_get_capabilities',
             'ts_pin_manager_get_mappings', 'ts_pin_manager_get_status']:
    code += function(pins, name)
code += r'''
static void test_pins(void) {
    assert(ts_pin_manager_init() == ESP_OK);
    assert(ts_pin_manager_load_defaults() == ESP_OK);
    assert(ts_pin_manager_get_gpio(TS_PIN_FUNC_USB_MUX_2) == 48);
    assert(ts_pin_manager_get_function(48) == TS_PIN_FUNC_USB_MUX_2);
    assert(ts_pin_manager_get_gpio(TS_PIN_FUNC_POWER_UART_RX) == 47);
    assert(ts_pin_manager_get_function(47) == TS_PIN_FUNC_POWER_UART_RX);
    assert(ts_pin_manager_get_gpio(TS_PIN_FUNC_FAN_PWM_0) == 41);
    assert(ts_pin_manager_get_gpio(TS_PIN_FUNC_LED_TOUCH) == 45);
    ts_pin_status_t status[49]; size_t count = 49;
    assert(ts_pin_manager_get_status(status, &count) == ESP_OK && count == 49);
    assert(status[48].gpio_num == 48 && status[48].function == TS_PIN_FUNC_USB_MUX_2);
    assert(status[48].state == TS_PIN_STATE_FREE && !strcmp(status[48].owner, ""));
    assert(ts_pin_manager_get_capabilities(48) & TS_PIN_CAP_GPIO_OUT);
    assert(ts_pin_manager_get_capabilities(48) & TS_PIN_CAP_PWM);
    assert(ts_pin_manager_is_available(48));
    assert(ts_pin_manager_acquire(TS_PIN_FUNC_USB_MUX_2, "fixture") == ESP_OK);
    assert(!ts_pin_manager_is_available(48));
    count = 49; assert(ts_pin_manager_get_status(status, &count) == ESP_OK);
    assert(status[48].state == TS_PIN_STATE_ALLOCATED && !strcmp(status[48].owner, "fixture"));
    assert(ts_pin_manager_release(TS_PIN_FUNC_USB_MUX_2) == ESP_OK);
    assert(ts_pin_manager_is_available(48));
    assert(ts_pin_manager_set_mapping(TS_PIN_FUNC_DEBUG_0, 48) == ESP_ERR_INVALID_STATE);
    assert(ts_pin_manager_set_mapping(TS_PIN_FUNC_USB_MUX_2, -1) == ESP_OK);
    assert(ts_pin_manager_get_gpio(TS_PIN_FUNC_USB_MUX_2) == -1);
    assert(ts_pin_manager_get_function(48) == TS_PIN_FUNC_MAX);
    assert(ts_pin_manager_set_mapping(TS_PIN_FUNC_USB_MUX_2, 48) == ESP_OK);
    ts_pin_mapping_t mappings[TS_PIN_FUNC_MAX]; count = TS_PIN_FUNC_MAX;
    assert(ts_pin_manager_get_mappings(mappings, &count) == ESP_OK);
    bool found = false;
    for (size_t i = 0; i < count; ++i) if (mappings[i].function == TS_PIN_FUNC_USB_MUX_2) {
        assert(mappings[i].gpio_num == 48 && !strcmp(mappings[i].name, "USB_MUX_2")); found = true;
    }
    assert(found);
    assert(ts_pin_manager_set_mapping(TS_PIN_FUNC_DEBUG_0, -2) == ESP_ERR_INVALID_ARG);
    assert(ts_pin_manager_set_mapping(TS_PIN_FUNC_DEBUG_0, 49) == ESP_ERR_INVALID_ARG);
    assert(ts_pin_manager_get_function(49) == TS_PIN_FUNC_MAX);
    assert(ts_pin_manager_get_capabilities(49) == 0);
    assert(!ts_pin_manager_is_available(49));
    assert(ts_pin_manager_get_function(48) == TS_PIN_FUNC_USB_MUX_2);
    assert(ts_pin_manager_deinit() == ESP_OK);
    puts("PASS default GPIO48, public status/mappings, acquire/release, -1 and upper boundary");
}
int main(int argc, char **argv) {
    if (argc == 1 || !strcmp(argv[1], "categories")) test_categories();
    if (argc == 1 || !strcmp(argv[1], "modules")) test_modules();
    if (argc == 1 || !strcmp(argv[1], "pins")) test_pins();
}
'''

with tempfile.TemporaryDirectory(prefix='ts-registry-tables-') as directory:
    build = Path(directory)
    (build / 'test.c').write_text(code)
    subprocess.run([
        'cc', '-std=gnu11', '-g', '-Wno-deprecated-declarations',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-Itests/runtime/storage_stubs', '-Icomponents/ts_api/include', '-I' + str(cjson),
        '-Icomponents/ts_core/ts_config/include', '-Icomponents/ts_hal/include',
        '-I' + str(idf / 'components/soc/esp32s3/include'),
        str(build / 'test.c'), str(cjson / 'cJSON.c'), '-lpthread', '-o', str(build / 'test')
    ], check=True)
    subprocess.run([str(build / 'test')] + ([args.only] if args.only else []), check=True)
