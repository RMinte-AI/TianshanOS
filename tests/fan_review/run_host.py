#!/usr/bin/env python3
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, default=ROOT / 'components/ts_drivers/src/ts_fan.c')
p.add_argument('--console', type=Path, default=ROOT / 'components/ts_console/commands/ts_cmd_fan.c')
p.add_argument('--device-api', type=Path, default=ROOT / 'components/ts_api/src/ts_api_device.c')
p.add_argument('--api', type=Path, default=ROOT / 'components/ts_api/src/ts_api_fan.c')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cjson-dir', type=Path)
p.add_argument('--period-ms', type=int, default=1000)
p.add_argument('--cadence-only', action='store_true')
p.add_argument('--check', action='store_true')
args = p.parse_args()
source, api = args.source.read_text(), args.api.read_text()
device_api = args.device_api.read_text()


def function(text, name):
    m = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;]*?\)\n\{', text, re.M)
    assert m, name
    return text[m.start():text.index('\n}', m.start()) + 2]


with tempfile.TemporaryDirectory(prefix='fan-review-') as tmp:
    out = Path(tmp)
    start = source.index('#define FAN_AUTO_GUARD_TEMP ')
    end = source.index('/*===========================================================================*/', source.index('typedef struct {'))
    nvs_end = source.index('} fan_nvs_config_t;') + len('} fan_nvs_config_t;')
    nvs_start = source.rfind('typedef struct {', 0, nvs_end)
    (out / 'fan_types.inc').write_text(source[start:end] + source[nvs_start:nvs_end])
    names = ['calc_duty_from_curve', 'clamp_float', 'apply_duty_limits',
             'is_temp_data_basic_valid', 'is_auto_temp_data_usable', 'reset_adaptive_auto_state',
             'get_adaptive_temperatures', 'record_auto_temp_sample', 'calculate_auto_slope',
             'apply_auto_rate_limit', 'update_response_learning', 'apply_adaptive_auto',
             'apply_auto_immediate', 'apply_curve_with_hysteresis', 'update_pwm',
             'fan_update_callback', 'ts_fan_configure', 'ts_fan_set_mode', 'ts_fan_set_duty',
             'ts_fan_enable', 'ts_fan_get_status', 'ts_fan_emergency_full',
             'ts_fan_save_config', 'ts_fan_save_full_config', 'ts_fan_load_config']
    if 'static void update_guard_observation(' in source:
        names.insert(names.index('apply_adaptive_auto'), 'update_guard_observation')
    (out / 'fan_core.inc').write_text('\n\n'.join(function(source, n) for n in names))
    (out / 'api_core.inc').write_text('\n\n'.join(function(api, n) for n in ['mode_to_string', 'auto_state_to_string', 'status_to_json', 'api_fan_set']))
    (out / 'device_core.inc').write_text('\n\n'.join(function(device_api, n) for n in ['fan_mode_to_str', 'fan_auto_state_to_str', 'api_device_fan_status']))
    console = args.console.read_text()
    (out / 'console_core.inc').write_text('\n\n'.join(function(console, n) for n in ['mode_to_str', 'do_fan_status']))
    cjson = ROOT / 'managed_components/espressif__cjson'
    if not (cjson / 'cJSON.c').exists():
        cjson = Path('/Users/massif/esp/v5.5.2/esp-idf/components/json/cJSON')
    if args.cjson_dir: cjson = args.cjson_dir
    exe = out / 'review'
    command = ['cc', '-std=c11', '-g', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-deprecated-declarations',
               '-fsanitize=address,undefined', '-DCONFIG_TS_DRIVERS_FAN_TEMP_UPDATE_MS=' + str(args.period_ms), '-DREPAIRED=' + str(int('guard_last_observation_ms' in source)),
               '-I' + str(ROOT / 'tests/fan_monitor/stubs'), '-I' + str(ROOT / 'components/ts_drivers/include'),
               '-I' + str(cjson), '-I' + tmp, str(ROOT / 'tests/fan_review/test_review.c'), str(cjson / 'cJSON.c'),
               '-lm', '-o', str(exe)]
    subprocess.run(command, check=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('w') as result:
        subprocess.run([str(exe), 'cadence' if args.cadence_only else 'check' if args.check else 'probe'], stdout=result, check=True)
    print('PASS execution' + (' and repaired assertions' if args.check or args.cadence_only else ' (probe, not acceptance)') + ': ' + str(args.output))
