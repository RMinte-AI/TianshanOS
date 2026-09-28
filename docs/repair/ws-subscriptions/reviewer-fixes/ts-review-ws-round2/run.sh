#!/bin/bash
set -euo pipefail
cd /Users/massif/TianshanOS
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cjson=/Users/massif/esp/v5.5.2/esp-idf/components/json/cJSON
cc -std=gnu11 -g -fsanitize=address,undefined -Wno-deprecated-declarations -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -I"$cjson" /tmp/ts-review-ws-round2/starvation.c "$cjson/cJSON.c" -o /tmp/ts-review-ws-round2/starvation
cc -std=gnu11 -g -fsanitize=address,undefined -Wno-deprecated-declarations -Icomponents/ts_core/ts_event/include -Itests/ws_subscriptions/stubs -Icomponents/ts_core/ts_event/src -I"$cjson" /tmp/ts-review-ws-round2/core_stop.c -o /tmp/ts-review-ws-round2/core_stop
cc -std=gnu11 -g -fsanitize=address,undefined -Itests/ws_subscriptions/stubs -I"$cjson" /tmp/ts-review-ws-round2/service_stop.c -o /tmp/ts-review-ws-round2/service_stop
/tmp/ts-review-ws-round2/core_stop
/tmp/ts-review-ws-round2/service_stop
/tmp/ts-review-ws-round2/starvation
