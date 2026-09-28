#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/ts-ws-host.XXXXXX)
trap 'rm -rf "$build"' EXIT
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cjson="${IDF_PATH:-/Users/massif/esp/v5.5.2/esp-idf}/components/json/cJSON"
cc -std=gnu11 -g -Wno-deprecated-declarations -fsanitize=address,undefined -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -I"$cjson" tests/ws_subscriptions/test_manager.c "$cjson/cJSON.c" -o "$build/manager"
"$build/manager"
cc -std=gnu11 -g -Wno-deprecated-declarations -fsanitize=address,undefined -Icomponents/ts_core/ts_event/include -Itests/ws_subscriptions/stubs -Icomponents/ts_core/ts_event/src -I"$cjson" tests/ws_subscriptions/test_event.c -o "$build/event"
"$build/event"
python3 - "$build" <<'PY'
from pathlib import Path
import re,sys
for source,names,out in [
 ('components/ts_net/src/ts_http_server.c',['ts_http_server_stop','ts_http_server_deinit'],'http_stop.inc'),
 ('components/ts_webui/src/ts_webui.c',['ts_webui_stop','ts_webui_deinit','ts_webui_is_running'],'webui_stop.inc')]:
 s=Path(source).read_text();parts=[]
 for name in names:
  m=re.search(r'^(?:esp_err_t|bool) '+name+r'\(void\)\s*\{',s,re.M);assert m,name
  parts.append(s[m.start():s.index('\n}',m.start())+2])
 Path(sys.argv[1],out).write_text('\n'.join(parts))
PY
cc -std=gnu11 -g -fsanitize=address,undefined -Itests/ws_subscriptions/stubs -I"$cjson" -I"$build" tests/ws_subscriptions/test_lifecycle.c -o "$build/lifecycle"
"$build/lifecycle"
python3 - "$build" <<'PY'
from pathlib import Path
import sys
s=Path('components/ts_webui/src/ts_webui_ws.c').read_text()
a=s.index('esp_err_t ts_webui_ws_init(void)');b=s.index('\n}',a)+2
Path(sys.argv[1],'ws_init.inc').write_text(s[a:b])
a=s.index('esp_err_t ts_webui_ws_stop(httpd_handle_t server)\n{');b=s.index('\n}',a)+2
Path(sys.argv[1],'ws_stop.inc').write_text(s[a:b])
parts=[]
for name in ['ts_webui_ssh_exec_start','ts_webui_ssh_exec_start_ex']:
 a=s.index('esp_err_t '+name+'(');b=s.index('\n}',a)+2;parts.append(s[a:b])
Path(sys.argv[1],'ws_creators.inc').write_text('\n'.join(parts))
PY
cc -std=gnu11 -g -fsanitize=address,undefined -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -I"$cjson" -I"$build" tests/ws_subscriptions/test_init.c -o "$build/init"
"$build/init"
