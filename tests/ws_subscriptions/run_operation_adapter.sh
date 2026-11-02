#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${IDF_PATH:?NOT RUN: set IDF_PATH to the project SDK}"
build=$(mktemp -d /tmp/ts-ws-adapter.XXXXXX)
trap 'rm -rf "$build"' EXIT
cjson="$IDF_PATH/components/json/cJSON"
"${CC:-cc}" -std=gnu11 -g -Wno-deprecated-declarations -Wno-macro-redefined -fsanitize=address,undefined -fno-sanitize-recover=all \
 -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -Icomponents/ts_security/include -Icomponents/ts_automation/include -Icomponents/ts_drivers/include -Icomponents/ts_console/include -Icomponents/ts_net/include -I"$cjson" \
 tests/ws_subscriptions/test_operation_adapter.c "$cjson/cJSON.c" -lm -o "$build/adapter"
"$build/adapter"
"${CC:-cc}" -std=gnu11 -g -Wno-deprecated-declarations -Wno-macro-redefined -fsanitize=address,undefined -fno-sanitize-recover=all \
 -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -Icomponents/ts_security/include -Icomponents/ts_automation/include -Icomponents/ts_drivers/include -Icomponents/ts_console/include -Icomponents/ts_net/include -I"$cjson" \
 tests/ws_subscriptions/test_terminal_auth.c "$cjson/cJSON.c" -lm -o "$build/terminal_auth"
"$build/terminal_auth"

# Only platform headers are synthesized; compile the complete HTTP source.
printf '%s\n' '#include "platform.h"' > "$build/ts_core.h"
printf '%s\n' '#include "platform.h"' > "$build/ts_storage.h"
python3 - "$build" <<'PY_ROUTES'
from pathlib import Path
import sys
source=Path('components/ts_webui/src/ts_webui_api.c').read_text()
a=source.index('esp_err_t ts_webui_api_init(void)');b=source.index('\n}',a)+2
Path(sys.argv[1],'webui_route_registration.inc').write_text(source[a:b])
PY_ROUTES
for auth in enabled disabled; do
 auth_flags=()
 if [[ "$auth" == enabled ]]; then auth_flags=(-DCONFIG_TS_WEBUI_AUTH_REQUIRED=1); fi
 "${CC:-cc}" -std=gnu11 -g -Wno-deprecated-declarations -fsanitize=address,undefined -fno-sanitize-recover=all "${auth_flags[@]}" \
  -I"$build" -Itests/ws_subscriptions/stubs -Icomponents/ts_net/include -Icomponents/ts_net/src -Icomponents/ts_security/include -I"$cjson" \
  tests/ws_subscriptions/test_http_auth.c "$cjson/cJSON.c" -o "$build/http_auth"
 "$build/http_auth"
done
