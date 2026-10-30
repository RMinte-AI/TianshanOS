#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
for suite in log codec ssh service_watch; do
    bash "tests/runtime/run_${suite}.sh"
done
bash tests/runtime/run_storage_paths.sh
bash tests/runtime/run_rule_pack.sh
python3 tests/runtime/test_completion.py
python3 tests/runtime/test_service_control.py
python3 tests/runtime/test_sensitive_auth.py
python3 tests/runtime/test_ui_widgets.py
python3 tests/runtime/test_scp_recv.py
python3 tests/runtime/test_condition_roundtrip.py
python3 tests/runtime/test_expansion.py
python3 tests/runtime/test_ssh_copyid.py
python3 tests/runtime/test_ssh_fingerprint_wire.py
python3 tests/runtime/test_failure_wire.py
python3 tests/runtime/test_copyid_quote.py
python3 tests/runtime/test_lpmu_access.py
python3 tests/runtime/test_input_repair.py
python3 tests/runtime/test_csr_input.py
python3 tests/runtime/test_keystore.py
python3 tests/runtime/test_probe.py
node --test tests/prompts/regression.test.cjs
for file in app api router terminal lang/en-US lang/zh-CN; do
    node --check "components/ts_webui/web/js/$file.js"
done
