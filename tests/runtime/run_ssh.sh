#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
build=/tmp/tianshan-runtime-tests
mkdir -p "$build"
cc -std=c11 -g -fsanitize=address,undefined -Wno-deprecated-declarations -Itests/runtime/ssh_stubs -Itests/certificate/stubs -Icomponents/ts_security/include -Icomponents/ch405labs_esp_libssh2/libssh2/include tests/runtime/test_ssh.c -lpthread -o "$build/ssh"
"$build/ssh"
