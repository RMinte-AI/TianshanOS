#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
build=/tmp/tianshan-runtime-tests
mkdir -p "$build"
python3 - "$build" <<'PY'
from pathlib import Path
import sys
s=Path('components/ts_core/ts_log/src/ts_log.c').read_text()
parts=[]
import re
for name in ['log_output_buffer','parse_esp_log','ts_log_vprintf_hook','get_effective_level','notify_callbacks','log_output_console','ts_log_v','ts_log_enable_esp_capture']:
 m=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',s,re.M)
 assert m,name
 a=m.start();b=s.index('\n}',a)+2;parts.append(s[a:b])
Path(sys.argv[1],'log_functions.inc').write_text('\n'.join(parts))
PY
cc -std=c11 -g -fsanitize=address,undefined -Icomponents/ts_core/ts_log/src -I"$build" tests/runtime/test_log.c -lpthread -o "$build/log"
"$build/log"
