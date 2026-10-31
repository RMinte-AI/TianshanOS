#!/usr/bin/env python3
"""Local production API/CLI power-setting probes; no hardware or NVS writes."""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
os.chdir(root)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--case', choices=['cli-low', 'cli-recovery', 'cli-pair', 'cli-get-failure', 'api-bad0', 'api-bad301', 'api-valid', 'api-save-failure', 'policy-hold'])
args = parser.parse_args()
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))


def function(path, name):
    text = Path(path).read_text()
    match = re.search(r'^(?:static )?[^\n;]+\b' + name + r'\([^;]+?\)\s*\{', text, re.M)
    assert match, name
    return text[match.start():text.index('\n}', match.start()) + 2] + '\n'


policy = 'components/ts_drivers/src/ts_power_policy.c'
cli = 'components/ts_console/commands/ts_cmd_voltprot.c'
api = 'components/ts_api/src/ts_api_power.c'
core = 'components/ts_api/src/ts_api.c'
code = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ts_api.h"
#include "ts_power_policy.h"
#include "freertos/semphr.h"
#include "argtable3.h"
#define TS_LOGI(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define TS_STRDUP_PSRAM strdup
#define pdMS_TO_TICKS(n) (n)
static bool get_failure, save_failure;
static unsigned threshold_sets, hold_sets, delay_sets, fan_sets, saves, starts, stops;
static char output[4096];
static int ts_console_printf(const char *fmt, ...) {
    size_t n = strlen(output); va_list ap; va_start(ap, fmt);
    int result = vsnprintf(output + n, sizeof(output) - n, fmt, ap); va_end(ap); return result;
}
static const char *esp_err_to_name(esp_err_t ret) { (void)ret; return "fixture-error"; }
'''
text = Path(policy).read_text()
start = text.index('typedef struct {')
code += text[start:text.index('static power_policy_state_t s_pp = {0};', start)]
code += 'static power_policy_state_t s_pp = {0};\n'
for name in ['ts_power_policy_get_config', 'ts_power_policy_is_initialized', 'ts_power_policy_is_running']:
    code += function(policy, name)
for name in ['get_thresholds', 'set_thresholds', 'set_recovery_hold', 'set_shutdown_delay', 'set_fan_stop_delay']:
    code += function(policy, 'ts_power_policy_' + name).replace('ts_power_policy_' + name + '(', 'real_' + name + '(', 1)
code += r'''
esp_err_t ts_power_policy_get_thresholds(float *low, float *recovery) {
    /* Fail at the violated getter contract before old code can use uninitialized data. */
    assert(low && recovery);
    if (get_failure) return ESP_FAIL;
    return real_get_thresholds(low, recovery);
}
esp_err_t ts_power_policy_set_thresholds(float low, float recovery) {
    ++threshold_sets; return real_set_thresholds(low, recovery);
}
esp_err_t ts_power_policy_set_recovery_hold(uint32_t seconds) {
    ++hold_sets; return real_set_recovery_hold(seconds);
}
esp_err_t ts_power_policy_set_shutdown_delay(uint32_t seconds) {
    ++delay_sets; return real_set_shutdown_delay(seconds);
}
esp_err_t ts_power_policy_set_fan_stop_delay(uint32_t seconds) {
    ++fan_sets; return real_set_fan_stop_delay(seconds);
}
esp_err_t ts_power_policy_save_config(void) { ++saves; return save_failure ? ESP_FAIL : ESP_OK; }
esp_err_t ts_power_policy_start(void) { ++starts; s_pp.running = true; return ESP_OK; }
esp_err_t ts_power_policy_stop(void) { ++stops; s_pp.running = false; return ESP_OK; }
esp_err_t ts_power_policy_init(const ts_power_policy_config_t *config) { (void)config; assert(false); return ESP_FAIL; }
esp_err_t ts_power_policy_trigger_test(void) { assert(false); return ESP_FAIL; }
esp_err_t ts_power_policy_reset(void) { assert(false); return ESP_FAIL; }
esp_err_t ts_power_policy_set_debug_mode(bool enabled, uint32_t seconds) { (void)enabled; (void)seconds; assert(false); return ESP_FAIL; }
bool ts_power_policy_is_debug_mode(void) { assert(false); return false; }
static void print_status(bool json) { (void)json; assert(false); }
static void print_config(void) { ts_console_printf("fixture-config\n"); }
typedef struct { const char *command, *help, *hint; int (*func)(int, char **); void *argtable; } esp_console_cmd_t;
static int (*registered_command)(int, char **);
static esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd) { registered_command = cmd->func; return ESP_OK; }
'''
text = Path(cli).read_text()
start = text.index('static struct {')
code += text[start:text.index('} s_voltprot_args;', start) + len('} s_voltprot_args;')]
code += function(cli, 'cmd_voltprot_handler')
code += function(cli, 'ts_cmd_voltprot_register')
for name in ['ts_api_result_ok', 'ts_api_result_error', 'ts_api_result_free']:
    code += function(core, name)
code += function(api, 'api_power_protection_set')
code += r'''
static void reset(void) {
    s_pp.initialized = true; s_pp.running = true;
    s_pp.config = (ts_power_policy_config_t) {
        .low_voltage_threshold = 12.6f, .recovery_voltage_threshold = 18.0f,
        .shutdown_delay_sec = 60, .recovery_hold_sec = 5, .fan_stop_delay_sec = 60
    };
    threshold_sets = hold_sets = delay_sets = fan_sets = saves = starts = stops = 0;
    get_failure = save_failure = false; output[0] = 0;
}
static int command(const char *low, const char *recovery) {
    char *argv[8] = {"voltprot", "-c"}; int argc = 2;
    if (low) { argv[argc++] = "--low"; argv[argc++] = (char *)low; }
    if (recovery) { argv[argc++] = "--recovery"; argv[argc++] = (char *)recovery; }
    return registered_command(argc, argv);
}
static void check_api_invalid(int hold) {
    ts_power_policy_config_t before = s_pp.config;
    char body[160]; snprintf(body, sizeof(body), "{\"low_threshold\":13.5,\"recovery_hold\":%d,\"persist\":true,\"enable\":false}", hold);
    cJSON *params = cJSON_Parse(body); ts_api_result_t result = {0};
    assert(api_power_protection_set(params, &result) == ESP_ERR_INVALID_ARG);
    assert(!threshold_sets && !hold_sets && !delay_sets && !fan_sets && !saves && !starts && !stops);
    assert(!memcmp(&s_pp.config, &before, sizeof(before)) && s_pp.running);
    ts_api_result_free(&result); cJSON_Delete(params);
}
int main(int argc, char **argv) {
    assert(argc == 2); s_pp.state_mutex = xSemaphoreCreateMutex(); assert(s_pp.state_mutex);
    assert(ts_cmd_voltprot_register() == ESP_OK); reset();
    if (!strcmp(argv[1], "cli-low")) {
        assert(command("13.5", NULL) == 0); assert(threshold_sets == 1);
        assert(s_pp.config.low_voltage_threshold == 13.5f && s_pp.config.recovery_voltage_threshold == 18.0f);
    } else if (!strcmp(argv[1], "cli-recovery")) {
        assert(command(NULL, "19.0") == 0); assert(threshold_sets == 1);
        assert(s_pp.config.low_voltage_threshold == 12.6f && s_pp.config.recovery_voltage_threshold == 19.0f);
    } else if (!strcmp(argv[1], "cli-pair")) {
        assert(command("20.0", "24.0") == 0); assert(threshold_sets == 1);
        assert(s_pp.config.low_voltage_threshold == 20.0f && s_pp.config.recovery_voltage_threshold == 24.0f);
    } else if (!strcmp(argv[1], "cli-get-failure")) {
        get_failure = true; assert(command("13.5", NULL) != 0); assert(!threshold_sets);
        assert(s_pp.config.low_voltage_threshold == 12.6f && s_pp.config.recovery_voltage_threshold == 18.0f);
        assert(strstr(output, "fixture-error"));
    } else if (!strcmp(argv[1], "api-bad0")) {
        check_api_invalid(0);
    } else if (!strcmp(argv[1], "api-bad301")) {
        check_api_invalid(301);
    } else if (!strcmp(argv[1], "api-valid")) {
        for (int i = 0; i < 3; ++i) {
            reset(); const char *body = i == 0 ? "{\"low_threshold\":13.5,\"persist\":true}" :
                i == 1 ? "{\"recovery_hold\":1,\"persist\":true}" : "{\"recovery_hold\":300,\"persist\":true}";
            cJSON *params = cJSON_Parse(body); ts_api_result_t result = {0};
            assert(api_power_protection_set(params, &result) == ESP_OK && result.code == TS_API_OK);
            assert(s_pp.config.recovery_voltage_threshold == 18.0f && s_pp.config.shutdown_delay_sec == 60 && s_pp.config.fan_stop_delay_sec == 60);
            assert(s_pp.config.low_voltage_threshold == (i == 0 ? 13.5f : 12.6f));
            assert(s_pp.config.recovery_hold_sec == (i == 0 ? 5 : i == 1 ? 1 : 300));
            assert(saves == 1 && !starts && !stops);
            char *wire = cJSON_PrintUnformatted(result.data); cJSON *data = cJSON_Parse(wire); free(wire);
            assert(cJSON_IsTrue(cJSON_GetObjectItem(data, "persisted"))); cJSON_Delete(data);
            ts_api_result_free(&result); cJSON_Delete(params);
        }
    } else if (!strcmp(argv[1], "policy-hold")) {
        assert(ts_power_policy_set_recovery_hold(0) == ESP_ERR_INVALID_ARG);
        assert(ts_power_policy_set_recovery_hold(301) == ESP_ERR_INVALID_ARG);
        assert(s_pp.config.recovery_hold_sec == 5);
        assert(ts_power_policy_set_recovery_hold(1) == ESP_OK);
        assert(s_pp.config.recovery_hold_sec == 1);
        assert(ts_power_policy_set_recovery_hold(300) == ESP_OK);
        assert(s_pp.config.recovery_hold_sec == 300);
    } else if (!strcmp(argv[1], "api-save-failure")) {
        save_failure = true; cJSON *params = cJSON_Parse("{\"low_threshold\":13.5,\"persist\":true}"); ts_api_result_t result = {0};
        assert(api_power_protection_set(params, &result) == ESP_FAIL && result.code != TS_API_OK);
        assert(s_pp.config.low_voltage_threshold == 13.5f && saves == 1);
        ts_api_result_free(&result); cJSON_Delete(params);
    } else { assert(false); }
    arg_freetable((void **)&s_voltprot_args, sizeof(s_voltprot_args) / sizeof(void *));
    vSemaphoreDelete(s_pp.state_mutex); printf("PASS %s\n", argv[1]);
}
'''
cjson = idf / 'components/json/cJSON'
argtable = idf / 'components/console/argtable3'
with tempfile.TemporaryDirectory(prefix='ts-power-settings-') as directory:
    build = Path(directory); (build / 'test.c').write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-Itests/runtime/storage_stubs', '-Icomponents/ts_api/include',
                    '-Icomponents/ts_drivers/include', '-I' + str(cjson), '-I' + str(argtable),
                    str(build / 'test.c'), str(cjson / 'cJSON.c'), *map(str, argtable.glob('*.c')),
                    '-lpthread', '-lm', '-o', str(build / 'test')], check=True)
    cases = [args.case] if args.case else ['cli-low', 'cli-recovery', 'cli-pair', 'cli-get-failure', 'api-bad0', 'api-bad301', 'api-valid', 'api-save-failure', 'policy-hold']
    for case in cases:
        subprocess.run([str(build / 'test'), case], check=True)
