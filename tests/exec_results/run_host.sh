#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/terminal-jobs.XXXXXX)
trap 'rm -rf "$build"' EXIT
cjson="${IDF_PATH:-/Users/massif/esp/v5.5.2/esp-idf}/components/json/cJSON"
cc -std=gnu11 -g -Wno-deprecated-declarations -Wno-macro-redefined -fsanitize=address,undefined -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -Icomponents/ts_security/include -Icomponents/ts_automation/include -Icomponents/ts_drivers/include -Icomponents/ts_console/include -Icomponents/ts_net/include -I"$cjson" tests/ws_subscriptions/test_exec_results.c "$cjson/cJSON.c" -lm -o "$build/test"
"$build/test"

python3 - "$build" <<'PYCODE'
from pathlib import Path
import sys
s=Path('components/ts_api/src/ts_api_ssh.c').read_text();parts=[]
for name in ['api_ssh_exec_stream','api_ssh_exec_status']:
 a=s.index('static esp_err_t '+name+'(');b=s.index('\n}',a)+2;parts.append(s[a:b])
Path(sys.argv[1],'exec_api.inc').write_text('\n'.join(parts))
PYCODE
cc -std=gnu11 -g -Wno-deprecated-declarations -Wno-macro-redefined -fsanitize=address,undefined -I"$build" -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -Icomponents/ts_security/include -Icomponents/ts_automation/include -Icomponents/ts_drivers/include -Icomponents/ts_console/include -Icomponents/ts_net/include -I"$cjson" tests/ws_subscriptions/test_exec_api.c "$cjson/cJSON.c" -lm -o "$build/test"
"$build/test"
