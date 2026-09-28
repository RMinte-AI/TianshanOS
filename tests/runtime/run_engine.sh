#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
build=/tmp/tianshan-runtime-tests
mkdir -p "$build"
python3 tests/runtime/extract_engine.py
idf=${IDF_PATH:-/Users/massif/esp/v5.5.2/esp-idf}
cc -std=c11 -g -fsanitize=address,undefined -Wno-deprecated-declarations -I"$build" -Itests/runtime/stubs -Itests/certificate/stubs -Icomponents/ts_automation/include -Icomponents/ts_security/include -I"$idf/components/json/cJSON" tests/runtime/test_engine.c components/ts_automation/src/ts_rule_codec.c "$idf/components/json/cJSON/cJSON.c" -lpthread -lm -o "$build/engine"
"$build/engine"
