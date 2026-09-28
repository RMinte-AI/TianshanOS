#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/ts-ws-f1-f2.XXXXXX)
trap 'rm -rf "$build"' EXIT
if [[ "${1:-}" == "--baseline" ]]; then
  tar -xzf docs/repair/ws-f1-f2/pre-fix-source.tar.gz -C "$build"
  python3 - "$build" <<'HASH'
import json,hashlib,sys
from pathlib import Path
for name,want in json.loads(Path('docs/repair/ws-f1-f2/baseline.json').read_text())['sha256'].items():
 assert hashlib.sha256((Path(sys.argv[1])/name).read_bytes()).hexdigest()==want,name
print('Frozen production snapshot hashes verified')
HASH
  # Preserve the original harness that matches the historical production ABI.
  tar -xzf docs/repair/ws-operation/baseline.tar.gz -C "$build" tests/ws_subscriptions/test_f1_f2.c tests/ws_subscriptions/test_reviewer.c tests/ws_subscriptions/run_f1_f2.sh tests/ws_subscriptions/stubs/platform.h
  if bash "$build/tests/ws_subscriptions/run_f1_f2.sh" > "$build/red.txt" 2>&1; then
    cat "$build/red.txt"; echo 'Unexpected baseline PASS'; exit 1
  fi
  cat "$build/red.txt"
  grep -q 'F1 assertion:.*FAIL' "$build/red.txt"
  grep -q 'F2 assertion:.*FAIL' "$build/red.txt"
  if grep -Eq 'error:|Assertion failed|Sanitizer' "$build/red.txt"; then exit 1; fi
  echo 'CONFIRMED: both failures are behavioral assertions, not compilation or cleanup failures'
  exit 0
fi
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cjson="${IDF_PATH:-/Users/massif/esp/v5.5.2/esp-idf}/components/json/cJSON"
"${CC:-cc}" -std=gnu11 -g -Wno-deprecated-declarations -fsanitize=address,undefined -Itests/ws_subscriptions/stubs -Icomponents/ts_webui/include -Icomponents/ts_webui/src -Icomponents/ts_security/include -Icomponents/ts_automation/include -Icomponents/ts_drivers/include -Icomponents/ts_console/include -Icomponents/ts_net/include -Wno-macro-redefined -I"$cjson" -I"$build" tests/ws_subscriptions/test_f1_f2.c "$cjson/cJSON.c" -o "$build/f1f2"
failed=0
scenarios="f1 f2"
if grep -q TS_WS_POWER_SLOTS components/ts_webui/include/ts_ws_transport.h; then scenarios="$scenarios late early identity exec continuous power_retry capacity retry_limit stop timeout class power_order fair"; fi
for scenario in $scenarios; do
 if ! "$build/f1f2" "$scenario"; then failed=1; fi
done
exit "$failed"
