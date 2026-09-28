#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
build=/tmp/tianshan-runtime-tests
idf=${IDF_PATH:-/Users/massif/esp/v5.5.2/esp-idf}
mkdir -p "$build"
cc -std=c11 -g -fsanitize=address,undefined -Wno-deprecated-declarations -Itests/runtime/stubs -Itests/certificate/stubs -Icomponents/ts_automation/include -I"$idf/components/json/cJSON" tests/runtime/test_codec.c components/ts_automation/src/ts_rule_codec.c "$idf/components/json/cJSON/cJSON.c" -lpthread -lm -o "$build/codec"
"$build/codec"
