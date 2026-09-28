#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${IDF_PATH:?NOT RUN: set IDF_PATH to the project SDK}"
build=$(mktemp -d /tmp/ts-ws-operation.XXXXXX)
trap 'rm -rf "$build"' EXIT
cjson="$IDF_PATH/components/json/cJSON"
"${CC:-cc}" -std=gnu11 -g -Wno-deprecated-declarations -fsanitize=address,undefined -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -I"$cjson" tests/ws_subscriptions/test_operation.c "$cjson/cJSON.c" -o "$build/operation"
"$build/operation"
