#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${IDF_PATH:?NOT RUN: set IDF_PATH to the project SDK}"
build=$(mktemp -d /tmp/ts-ws-adapter.XXXXXX)
trap 'rm -rf "$build"' EXIT
cjson="$IDF_PATH/components/json/cJSON"
extra=()
if [[ "${PROJECT_BASELINE:-0}" == 1 ]]; then extra+=(-DPROJECT_BASELINE); fi
python3 - "$build" <<'PYCLI'
import pathlib,sys
s=pathlib.Path('components/ts_console/commands/ts_cmd_ssh.c').read_text()
a=s.index('static const char *shell_input_callback(');b=s.index('static int do_ssh_shell(',a)
pathlib.Path(sys.argv[1],'cli_shell_fixture.inc').write_text(s[a:b])
PYCLI
"${CC:-cc}" -std=gnu11 -g -Wno-deprecated-declarations -Wno-macro-redefined -fsanitize=address,undefined \
 "${extra[@]}" -I"$build" -Itests/ws_subscriptions/project_stubs -Icomponents/ch405labs_esp_libssh2/libssh2/include -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -Icomponents/ts_security/include -Icomponents/ts_automation/include -Icomponents/ts_drivers/include -Icomponents/ts_console/include -Icomponents/ts_net/include -I"$cjson" \
 tests/ws_subscriptions/test_shell_driver.c "$cjson/cJSON.c" -o "$build/adapter"
"$build/adapter" "$@"
