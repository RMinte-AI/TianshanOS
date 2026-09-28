#!/bin/bash
set -euo pipefail
cd /Users/massif/TianshanOS
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cjson=/Users/massif/esp/v5.5.2/esp-idf/components/json/cJSON
cc -std=gnu11 -g -Wno-deprecated-declarations -fsanitize=address,undefined -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -I"$cjson" /tmp/ts-review-ws/review.c "$cjson/cJSON.c" -o /tmp/ts-review-ws/review
/tmp/ts-review-ws/review
