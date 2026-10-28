#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${IDF_PATH:=/Users/massif/esp/v5.5.2/esp-idf}"
: "${SERVICE_TEST_BUILD:=/tmp/tianshan-service-runs-tests}"
mkdir -p "$SERVICE_TEST_BUILD"
cc -std=c11 -D_DEFAULT_SOURCE -g -fsanitize=address,undefined -Wno-deprecated-declarations \
  -Itests/runtime/state_stubs -Itests/runtime/ssh_stubs -Itests/runtime/stubs \
  -Itests/runtime/storage_stubs -Itests/certificate/stubs \
  -Icomponents/ts_security/include -Icomponents/ts_automation/include \
  -I"$IDF_PATH/components/json/cJSON" tests/runtime/test_service_runs.c \
  components/ts_security/src/ts_ssh_run_store.c components/ts_security/src/ts_ssh_log_watch.c \
  components/ts_security/src/ts_ssh_probe.c -lpthread -o "$SERVICE_TEST_BUILD/runs"
"$SERVICE_TEST_BUILD/runs"

# Exercise the emitted shell against Linux /proc; use only local test processes.
if [[ "$(uname -s)" == Linux ]]; then
  TS_REAL_REMOTE=1 "$SERVICE_TEST_BUILD/runs"
fi
