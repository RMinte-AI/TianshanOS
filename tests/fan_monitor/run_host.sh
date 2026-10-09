#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/ts-fan-monitor.XXXXXX)
trap 'rm -rf "$build"' EXIT
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"

# Exercise the real timer, controller, temperature cache and status getters;
# only hardware, clock, variable input and synchronization are host stubs.
python3 - "$build" <<'PY'
from pathlib import Path
import re
import sys

out = Path(sys.argv[1])
fan = Path('components/ts_drivers/src/ts_fan.c').read_text()
temp = Path('components/ts_drivers/src/ts_temp_source.c').read_text()

def structs(source):
    start = source.index('typedef struct {')
    return source[start:source.index('/*===========================================================================*/', start)]

def functions(source, names):
    parts = []
    for name in names:
        match = re.search(r'^(?:static )?(?:int64_t|float|void|bool|int16_t|uint8_t|esp_err_t) '
                          + name + r'\([^;]*?\)\n\{', source, re.M)
        assert match, name
        parts.append(source[match.start():source.index('\n}', match.start()) + 2])
    return '\n\n'.join(parts)

start = fan.index('#define FAN_AUTO_GUARD_TEMP ')
out.joinpath('fan_types.inc').write_text(
    fan[start:fan.index('/*===========================================================================*/', start)] + structs(fan))
out.joinpath('temp_types.inc').write_text(structs(temp))
out.joinpath('temp_core.inc').write_text(functions(temp, [
    'get_current_ms', 'sanitize_bound_weight', 'store_variable_snapshot',
    'read_fresh_variable_float', 'read_variable_temp_snapshot', 'is_provider_valid',
    'evaluate_active_source_with_lock_mode', 'evaluate_active_source', 'sync_bound_variable_compat',
    'ts_temp_bind_variables', 'ts_temp_unbind_variable', 'is_cached_active_source_valid_locked',
    'fill_invalid_temp_data', 'fill_effective_data_locked', 'ts_temp_get_effective_impl',
    'ts_temp_get_effective_nonblocking', 'ts_temp_get_status']))
out.joinpath('fan_core.inc').write_text(functions(fan, [
    'calc_duty_from_curve', 'clamp_float', 'apply_duty_limits', 'is_temp_data_basic_valid',
    'is_auto_temp_data_usable', 'reset_adaptive_auto_state', 'get_adaptive_temperatures',
    'record_auto_temp_sample', 'calculate_auto_slope', 'apply_auto_rate_limit',
    'update_response_learning', 'update_guard_observation', 'apply_adaptive_auto', 'apply_curve_with_hysteresis',
    'update_pwm', 'fan_update_callback', 'ts_fan_get_status']))
PY

cc -std=c11 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -fsanitize=address,undefined -Itests/fan_monitor/stubs \
    -Icomponents/ts_drivers/include -I"$build" tests/fan_monitor/test_monitor.c -o "$build/monitor"
"$build/monitor"
