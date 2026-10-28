#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bash tests/runtime/run_service_runs.sh
