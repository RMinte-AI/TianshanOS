#!/usr/bin/env python3
"""Exercise source-extracted production control; no video frame becomes an input sample."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=ROOT / 'components/ts_drivers/src/ts_fan.c')
parser.add_argument('--csv', type=Path, required=True)
args = parser.parse_args()
source = args.source.read_text()


def function(name):
    match = re.search(r'^(?:static )?(?:float|void|bool|uint8_t|esp_err_t) '
                      + name + r'\([^;]*?\)\n\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.start()) + 2]


with tempfile.TemporaryDirectory(prefix='ts-fan-decay-') as tmp:
    out = Path(tmp)
    start = source.index('#define FAN_AUTO_GUARD_TEMP ')
    end = source.index('/*===========================================================================*/', source.index('typedef struct {'))
    (out / 'fan_types.inc').write_text(source[start:end])
    names = ['calc_duty_from_curve', 'clamp_float', 'apply_duty_limits',
             'is_temp_data_basic_valid', 'is_auto_temp_data_usable',
             'reset_adaptive_auto_state', 'get_adaptive_temperatures',
             'record_auto_temp_sample', 'calculate_auto_slope', 'apply_auto_rate_limit',
             'update_response_learning', 'apply_adaptive_auto', 'apply_auto_immediate',
             'apply_curve_with_hysteresis', 'update_pwm', 'fan_update_callback',
             'ts_fan_set_mode', 'ts_fan_get_status']
    if 'static void update_guard_observation(' in source:
        names.insert(names.index('apply_adaptive_auto'), 'update_guard_observation')
    core = '\n\n'.join(function(name) for name in names)
    # Observe the demand BEFORE the production permission and slew rules.
    needle = '    bool allow_down = (guard_temp <= FAN_AUTO_GUARD_RELEASE_TEMP)'
    assert core.count(needle) == 1
    core = core.replace(needle, '    observed_demand = target;\n' + needle)
    (out / 'fan_core.inc').write_text(core)
    exe = out / 'decay'
    subprocess.run(['cc', '-std=c11', '-g', '-Wall', '-Wextra',
                    '-Wno-unused-function', '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-I' + str(ROOT / 'tests/fan_monitor/stubs'),
                    '-I' + str(ROOT / 'components/ts_drivers/include'), '-I' + tmp,
                    str(ROOT / 'tests/fan_decay/test_decay.c'), '-lm', '-o', str(exe)], check=True)
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open('w') as output:
        subprocess.run([str(exe)], stdout=output, check=True)
    print('PASS production controller invariants; trace:', args.csv)
