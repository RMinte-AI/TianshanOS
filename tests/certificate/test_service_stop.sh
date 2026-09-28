#!/bin/bash
# Tests the original baseline and current stop/restart implementation in /tmp.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
root=$(mktemp -d /tmp/tianshan-stop-proposal.XXXXXX)
python3 - "$root" <<'PY'
from pathlib import Path
import sys, subprocess
s=subprocess.check_output(["git", "show", "d6ed947a592265fa12754828bc79803fc50c1db2:components/ts_core/ts_service/src/ts_service.c"], text=True)
current=Path('components/ts_core/ts_service/src/ts_service.c').read_text()
a=s.index('static esp_err_t stop_service_internal(ts_service_instance_t *service)\n{')
b=s.index('\n}\n',a)+3
Path(sys.argv[1],'service_stop.inc').write_text(s[a:b])
a=current.index('esp_err_t ts_service_restart(ts_service_handle_t handle)\n{')
b=current.index('\n}\n',a)+3
Path(sys.argv[1],'service_restart.inc').write_text(current[a:b])
PY
cc -std=c11 -fsanitize=address,undefined -Itests/certificate/stubs -I"$root" tests/certificate/test_service_stop.c -lpthread -o "$root/baseline"
set +e
"$root/baseline"
result=$?
set -e
[[ "$result" = 2 ]]
python3 - "$root" <<'PY'
from pathlib import Path
import sys
s=Path('components/ts_core/ts_service/src/ts_service.c').read_text()
a=s.index('static esp_err_t stop_service_internal(ts_service_instance_t *service)\n{')
b=s.index('\n}\n',a)+3
Path(sys.argv[1],'service_stop.inc').write_text(s[a:b])
PY
cc -std=c11 -fsanitize=address,undefined -Itests/certificate/stubs -I"$root" tests/certificate/test_service_stop.c -lpthread -o "$root/current"
"$root/current"
