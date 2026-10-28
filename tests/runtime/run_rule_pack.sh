#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
# Cryptographic verification remains production code with disposable PKI.
python3 tests/runtime/test_rule_pack_crypto.py
python3 tests/runtime/test_rule_pack_crypto.py --unconfigured
# Unified owner + actual rule publication/admission replaces the retired stores.
bash tests/runtime/run_config_storage.sh
