#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
for suite in log codec store engine ssh service_watch; do
    bash "tests/runtime/run_${suite}.sh"
done
bash tests/runtime/run_rule_pack.sh
python3 tests/runtime/test_completion.py
python3 tests/runtime/test_action_submission.py
python3 tests/runtime/test_service_control.py
python3 tests/runtime/test_delete_api.py
python3 tests/runtime/test_condition_roundtrip.py
python3 tests/runtime/test_expansion.py
python3 tests/runtime/test_configuration_protocol.py
python3 tests/runtime/test_ssh_hosts.py
python3 tests/runtime/test_ssh_copyid.py
python3 tests/runtime/test_copyid_quote.py
python3 tests/runtime/test_lpmu_access.py
python3 tests/runtime/test_input_repair.py
python3 tests/runtime/test_action_store.py
python3 tests/runtime/test_action_pack.py
python3 tests/runtime/test_action_serialization.py
python3 tests/runtime/test_csr_input.py
python3 tests/runtime/test_keystore.py
python3 tests/runtime/test_rule_reload.py
python3 tests/runtime/test_stop_protocol.py
python3 tests/runtime/test_probe.py
node tests/runtime/test_ui.cjs
node tests/runtime/test_ssh_hosts_ui.cjs
for file in app api router terminal lang/en-US lang/zh-CN; do
    node --check "components/ts_webui/web/js/$file.js"
done
