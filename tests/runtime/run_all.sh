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
python3 tests/runtime/test_port_forward.py
python3 tests/runtime/test_compute_monitor.py
python3 tests/runtime/test_http_file_stream.py
python3 tests/runtime/test_sftp_list.py
python3 tests/runtime/test_cli_option_contract.py
python3 tests/runtime/test_system_memory.py
python3 tests/runtime/test_system_reboot.py
python3 tests/runtime/test_registry_tables.py
python3 tests/runtime/test_console_lifecycle.py
python3 tests/runtime/test_power_lifecycle.py
python3 tests/runtime/test_ota_lifecycle.py
python3 tests/runtime/test_source_sampling.py
python3 tests/runtime/test_dhcp_bindings.py
python3 tests/runtime/test_led_lifetimes.py
python3 tests/runtime/test_led_controls.py
python3 tests/runtime/test_led_image_bounds.py
python3 tests/runtime/test_power_settings.py
python3 tests/runtime/test_power_chip_crc.py
python3 tests/runtime/test_ui_widgets.py
python3 tests/runtime/test_scp_recv.py
python3 tests/runtime/test_condition_roundtrip.py
python3 tests/runtime/test_expansion.py
python3 tests/runtime/test_ssh_copyid.py
python3 tests/runtime/test_ssh_cli_controls.py
python3 tests/runtime/test_ssh_fingerprint_wire.py
python3 tests/runtime/test_failure_wire.py
python3 tests/runtime/test_copyid_quote.py
python3 tests/runtime/test_lpmu_access.py
python3 tests/runtime/test_device_names.py
python3 tests/runtime/test_input_repair.py
python3 tests/runtime/test_csr_input.py
python3 tests/runtime/test_keystore.py
python3 tests/runtime/test_probe.py
node --test tests/prompts/regression.test.cjs
for file in app api router terminal lang/en-US lang/zh-CN; do
    node --check "components/ts_webui/web/js/$file.js"
done

python3 tests/http_admission/run_host.py
python3 tests/terminal_jobs/run_host.py
bash tests/terminal_jobs/run_ws.sh
bash tests/exec_results/run_host.sh
node --test tests/http_admission/reconnect.test.cjs
