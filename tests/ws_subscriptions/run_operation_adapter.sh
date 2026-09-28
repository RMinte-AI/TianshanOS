#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${IDF_PATH:?NOT RUN: set IDF_PATH to the project SDK}"
build=$(mktemp -d /tmp/ts-ws-adapter.XXXXXX)
trap 'rm -rf "$build"' EXIT
cjson="$IDF_PATH/components/json/cJSON"
"${CC:-cc}" -std=gnu11 -g -Wno-deprecated-declarations -Wno-macro-redefined -fsanitize=address,undefined \
 -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -Icomponents/ts_security/include -Icomponents/ts_automation/include -Icomponents/ts_drivers/include -Icomponents/ts_console/include -Icomponents/ts_net/include -I"$cjson" \
 tests/ws_subscriptions/test_operation_adapter.c "$cjson/cJSON.c" -o "$build/adapter"
"$build/adapter"
