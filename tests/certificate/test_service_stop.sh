#!/bin/bash
# Historical evidence stays optional; CI exercises the current stop and restart.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
if [[ "$#" -gt 1 || ( "$#" -eq 1 && "$1" != "--current-only" ) ]]; then
    echo "Usage: $0 [--current-only]" >&2
    exit 1
fi
root=$(mktemp -d /tmp/tianshan-stop-proposal.XXXXXX)
trap 'rm -rf "$root"' EXIT
# Common preparation must also run when the historical baseline is omitted.
python3 - "$root" <<'COMMON'
from pathlib import Path
import sys
current=Path('components/ts_core/ts_service/src/ts_service.c').read_text()
a=current.index('esp_err_t ts_service_restart(ts_service_handle_t handle)\n{')
b=current.index('\n}\n',a)+3
Path(sys.argv[1],'service_restart.inc').write_text(current[a:b])
COMMON
if [[ "${1:-}" != "--current-only" ]]; then
    python3 - "$root" <<'BASELINE'
from pathlib import Path
import sys,subprocess
s=subprocess.check_output(['git','show','d6ed947a592265fa12754828bc79803fc50c1db2:components/ts_core/ts_service/src/ts_service.c'],text=True)
a=s.index('static esp_err_t stop_service_internal(ts_service_instance_t *service)\n{')
b=s.index('\n}\n',a)+3
Path(sys.argv[1],'service_stop.inc').write_text(s[a:b])
BASELINE
    cc -std=c11 -D_DEFAULT_SOURCE -fsanitize=address,undefined -Itests/certificate/stubs -I"$root" tests/certificate/test_service_stop.c -lpthread -o "$root/baseline"
    set +e
    "$root/baseline"
    result=$?
    set -e
    [[ "$result" = 2 ]]
fi
python3 - "$root" <<'CURRENT'
from pathlib import Path
import sys
s=Path('components/ts_core/ts_service/src/ts_service.c').read_text()
a=s.index('static esp_err_t stop_service_internal(ts_service_instance_t *service)\n{')
b=s.index('\n}\n',a)+3
Path(sys.argv[1],'service_stop.inc').write_text(s[a:b])
CURRENT
cc -std=c11 -D_DEFAULT_SOURCE -fsanitize=address,undefined -Itests/certificate/stubs -I"$root" tests/certificate/test_service_stop.c -lpthread -o "$root/current"
"$root/current"
