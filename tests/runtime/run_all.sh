#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
for suite in log codec store engine ssh service_watch; do
    bash "tests/runtime/run_${suite}.sh"
done
python3 tests/runtime/test_completion.py
python3 tests/runtime/test_configuration_protocol.py
python3 tests/runtime/test_rule_reload.py
python3 tests/runtime/test_stop_protocol.py
python3 tests/runtime/test_probe.py
node tests/runtime/test_ui.cjs
for file in app api router terminal lang/en-US lang/zh-CN; do
    node --check "components/ts_webui/web/js/$file.js"
done
