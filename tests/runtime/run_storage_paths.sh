#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${IDF_PATH:=/Users/massif/esp/v5.5.2/esp-idf}"
: "${STORAGE_PATH_TEST_BUILD:=/private/tmp/tianshan-storage-path-tests}"
mkdir -p "$STORAGE_PATH_TEST_BUILD"
python3 - "$STORAGE_PATH_TEST_BUILD/storage_upload.inc" <<'PYTHON'
from pathlib import Path
import sys
source=Path('components/ts_webui/src/ts_webui_api.c').read_text()
parts=[]
for name in ('url_decode','file_upload_handler'):
    start=source.index('static '+('void ' if name=='url_decode' else 'esp_err_t ')+name+'(')
    parts.append(source[start:source.index('\n}',start)+2])
Path(sys.argv[1]).write_text('\n'.join(parts))
PYTHON
cc -std=c11 -D_DEFAULT_SOURCE -g -fsanitize=address,undefined -Wno-deprecated-declarations \
  -Itests/runtime/storage_stubs -Itests/runtime/rule_pack/stubs \
  -Icomponents/ts_storage/include -Icomponents/ts_core/ts_config/include \
  -I"$STORAGE_PATH_TEST_BUILD" -Icomponents/ts_config_pack/include -Icomponents/ts_api/include -I"$IDF_PATH/components/json/cJSON" \
  tests/runtime/test_storage_paths.c "$IDF_PATH/components/json/cJSON/cJSON.c" \
  -lpthread -o "$STORAGE_PATH_TEST_BUILD/paths"
"$STORAGE_PATH_TEST_BUILD/paths"
