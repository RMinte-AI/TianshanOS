#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/ts-ws-baseline.XXXXXX)
trap 'rm -rf "$build"' EXIT
# Fixed evidence baseline only. Never overwrites the working tree.
git show 35238b4645ae5e9e0e99555d4fb64080d945565c:components/ts_webui/src/ts_ws_subscriptions.c > "$build/ts_ws_subscriptions.c"
git show 35238b4645ae5e9e0e99555d4fb64080d945565c:components/ts_webui/include/ts_ws_subscriptions.h > "$build/ts_ws_subscriptions.h"
cjson="${IDF_PATH:-/Users/massif/esp/v5.5.2/esp-idf}/components/json/cJSON"
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cc -Wno-deprecated-declarations -g -fsanitize=address,undefined -Itests/ws_subscriptions/stubs -I"$build" -Icomponents/ts_webui/include -I"$cjson" tests/ws_subscriptions/test_baseline.c "$cjson/cJSON.c" -o "$build/test"
for case in T02 T05 T07; do
 set +e
 ASAN_OPTIONS=detect_leaks=0 "$build/test" "$case"
 result=$?
 set -e
 if [ "$result" != 1 ]; then echo "Unexpected baseline outcome $result"; exit 2; fi
 echo "Expected business assertion failure: $case"
done

git show 35238b4645ae5e9e0e99555d4fb64080d945565c:components/ts_core/ts_event/src/ts_event.c > "$build/ts_event.c"
cc -std=gnu11 -DBASELINE_EVENT -g -fsanitize=address,undefined -I"$build" -Icomponents/ts_core/ts_event/include -Itests/ws_subscriptions/stubs -I"$cjson" tests/ws_subscriptions/test_event.c -o "$build/event"
set +e
"$build/event" > "$build/event.log" 2>&1
result=$?
set -e
if [ "$result" = 0 ] || ! rg -q 'heap-use-after-free' "$build/event.log"; then cat "$build/event.log"; exit 2; fi
rg -m 1 'ERROR: AddressSanitizer:' "$build/event.log"
echo 'Expected T32 baseline failure: actual dispatch accesses self-unregistered freed node'
