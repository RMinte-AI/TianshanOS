#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
build=/tmp/tianshan-runtime-tests
mkdir -p "$build"
cc -std=c11 -g -fsanitize=address,undefined -Itests/runtime/state_stubs -Itests/runtime/ssh_stubs -Itests/runtime/stubs -Itests/certificate/stubs -Icomponents/ts_security/include -Icomponents/ts_automation/include tests/runtime/test_service_watch.c components/ts_security/src/ts_ssh_service.c components/ts_security/src/ts_ssh_log_watch.c components/ts_security/src/ts_ssh_probe.c -lpthread -o "$build/service_watch"
"$build/service_watch"

cc -std=c11 -g -fsanitize=address,undefined -Itests/runtime/state_stubs -Itests/runtime/ssh_stubs -Itests/runtime/stubs -Itests/certificate/stubs -Icomponents/ts_security/include -Icomponents/ts_automation/include tests/runtime/test_service_protocol.c components/ts_security/src/ts_ssh_service.c components/ts_security/src/ts_ssh_log_watch.c components/ts_security/src/ts_ssh_probe.c -lpthread -o "$build/service_protocol"
"$build/service_protocol"
