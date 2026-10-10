#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
# Sequential: these production tests intentionally share one disposable NVS/SD fixture.
python3 tests/runtime/test_rule_pack_crypto.py
python3 tests/runtime/test_rule_pack_crypto.py --unconfigured
python3 tests/runtime/test_rule_pack_crypto.py --store
python3 tests/runtime/test_rule_pack_crypto.py --engine
# Directed review counterexamples, using the same production fixture and PKI.
for scenario in r1 r2 r3_ssh r3_webhook; do
    python3 tests/runtime/test_rule_pack_crypto.py --engine --case="$scenario"
done
