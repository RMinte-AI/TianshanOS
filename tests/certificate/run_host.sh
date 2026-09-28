#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${IDF_PATH:=/Users/massif/esp/v5.5.2/esp-idf}"
: "${CERT_TEST_BUILD:=/tmp/tianshan-certificate-tests}"
: "${CERT_MBEDTLS_BUILD:=/tmp/tianshan-cert-mbedtls}"
: "${CERT_TEST_PYTHON:=/Users/massif/.espressif/python_env/idf5.5_py3.12_env/bin/python}"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
mkdir -p "$CERT_TEST_BUILD"
"$CERT_TEST_PYTHON" tests/certificate/generate_fixtures.py "$CERT_TEST_BUILD/fixtures"
cc -std=c11 -g -fsanitize=address,undefined -Icomponents/ts_cert/include tests/certificate/test_time_retry.c components/ts_cert/src/ts_cert_time.c -o "$CERT_TEST_BUILD/time_retry"
"$CERT_TEST_BUILD/time_retry"
cc -std=c11 -g -fsanitize=address,undefined -Wno-unused-function -Itests/certificate/stubs -Icomponents/ts_cert/include -I"$IDF_PATH/components/mbedtls/mbedtls/include" tests/certificate/test_material.c components/ts_cert/src/ts_cert_time.c "$CERT_MBEDTLS_BUILD/library/libmbedx509.a" "$CERT_MBEDTLS_BUILD/library/libmbedcrypto.a" -lpthread -o "$CERT_TEST_BUILD/material"
"$CERT_TEST_BUILD/material" "$CERT_TEST_BUILD/fixtures"
node tests/certificate/test_ui.cjs
"$CERT_TEST_PYTHON" tests/certificate/extract_lifecycle.py "$CERT_TEST_BUILD/lifecycle.inc"
cc -std=c11 -g -fsanitize=address,undefined -I"$CERT_TEST_BUILD" -Itests/certificate/stubs -Icomponents/ts_cert/include -Icomponents/ts_https/include -Icomponents/ts_https/src -I"$IDF_PATH/components/mbedtls/mbedtls/include" tests/certificate/test_lifecycle.c -lpthread -o "$CERT_TEST_BUILD/lifecycle"
"$CERT_TEST_BUILD/lifecycle"
cc -std=c11 -g -fsanitize=address,undefined -I"$CERT_TEST_BUILD" -Itests/certificate/stubs -Icomponents/ts_cert/include -Icomponents/ts_https/include -Icomponents/ts_https/src -I"$IDF_PATH/components/mbedtls/mbedtls/include" tests/certificate/test_coordinator.c -lpthread -o "$CERT_TEST_BUILD/coordinator"
"$CERT_TEST_BUILD/coordinator"
cc -std=c11 -g -fsanitize=address,undefined -Wno-deprecated-declarations -I"$CERT_TEST_BUILD" -Itests/certificate/stubs -Icomponents/ts_cert/include -Icomponents/ts_https/include -I"$IDF_PATH/components/mbedtls/mbedtls/include" -I"$IDF_PATH/components/json/cJSON" tests/certificate/test_api.c components/ts_cert/src/ts_cert_time.c "$IDF_PATH/components/json/cJSON/cJSON.c" "$CERT_MBEDTLS_BUILD/library/libmbedx509.a" "$CERT_MBEDTLS_BUILD/library/libmbedcrypto.a" -lpthread -o "$CERT_TEST_BUILD/api"
"$CERT_TEST_BUILD/api" "$CERT_TEST_BUILD/fixtures"
cc -std=c11 -g -fsanitize=address,undefined -I"$CERT_TEST_BUILD" -Itests/certificate/stubs tests/certificate/test_time_cancel.c -lpthread -o "$CERT_TEST_BUILD/time_cancel"
"$CERT_TEST_BUILD/time_cancel"

./tests/certificate/test_service_stop.sh
