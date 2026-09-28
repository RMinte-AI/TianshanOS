#!/bin/bash
# Reproduce the five defects against the frozen uncommitted pre-fix source, not HEAD.
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
fixture=docs/repair/ws-subscriptions/reviewer-fixes
build=$(mktemp -d /tmp/ts-ws-pre-fix.XXXXXX)
trap 'rm -rf "$build"' EXIT
tar -xzf "$fixture/pre-fix-source.tar.gz" -C "$build"
python3 - "$build" "$fixture" <<'PY'
from pathlib import Path
import json,hashlib,sys
base=Path(sys.argv[1]);fixture=Path(sys.argv[2])
for name,digest in json.loads((fixture/'baseline.json').read_text())['sha256'].items():
 assert hashlib.sha256((base/name).read_bytes()).hexdigest()==digest,name
for origin,dest in [('ts-review-ws/review.c','r1_r3.c'),('ts-review-ws-round2/starvation.c','r5.c'),('ts-review-ws-round2/core_stop.c','r4_core.c'),('ts-review-ws-round2/service_stop.c','r4_service.c')]:
 s=(fixture/origin).read_text().replace('/Users/massif/TianshanOS/tests/ws_subscriptions/test_manager.c','test_manager.c')
 (base/dest).write_text(s)
PY
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cjson="${IDF_PATH:-/Users/massif/esp/v5.5.2/esp-idf}/components/json/cJSON"
for case in r1_r3 r5 r4_core r4_service; do
 extra=()
 if [[ "$case" == r4_core ]]; then extra=(-I"$build/components/ts_core/ts_event/include"); fi
 cc -std=gnu11 -g -Wno-deprecated-declarations -fsanitize=address,undefined "${extra[@]}" -I"$build/tests/ws_subscriptions" -I"$build/tests/ws_subscriptions/stubs" -I"$build/components/ts_webui/include" -I"$build/components/ts_webui/src" -I"$build/components/ts_core/ts_event/include" -I"$build/components/ts_core/ts_event/src" -I"$cjson" "$build/$case.c" "$cjson/cJSON.c" -o "$build/$case"
 "$build/$case"
done
