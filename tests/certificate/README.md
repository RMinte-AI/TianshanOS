# Certificate repair host tests

These tests use **production C/JavaScript**, ESP-IDF 5.5.2's bundled mbedTLS 3.6.5
and cJSON sources. They do not connect to devices or change the host clock.
NVS, ESP heap allocation, event delivery, scheduler and HTTP server sockets are
substitutes. The mbedTLS host configuration is its default configuration, not the
ESP32 hardware configuration. This does not validate hardware TLS or Flash.

Prerequisites: C compiler with ASan/UBSan, CMake, Node, Python with `cryptography`,
and the existing ESP-IDF checkout. No production credentials are read. Credentials
are generated into the temporary build directory with fixed 2026 validity dates.
Private keys are test-only, temporary, and never printed.

On this macOS workspace (Command Line Tools avoids requiring changes to Xcode's
license/global selection):

```sh
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
export IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf
export CERT_MBEDTLS_BUILD=/tmp/tianshan-cert-mbedtls
/Users/massif/.espressif/tools/cmake/3.30.2/CMake.app/Contents/bin/cmake \
  -S "$IDF_PATH/components/mbedtls/mbedtls" -B "$CERT_MBEDTLS_BUILD" \
  -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DCMAKE_BUILD_TYPE=Debug
/Users/massif/.espressif/tools/cmake/3.30.2/CMake.app/Contents/bin/cmake \
  --build "$CERT_MBEDTLS_BUILD" -j 6
./tests/certificate/run_host.sh
```

Override `CERT_TEST_PYTHON`, `CERT_TEST_BUILD`, `CERT_MBEDTLS_BUILD`, `IDF_PATH`
as needed. On other systems set `DEVELOPER_DIR` appropriately or remove it.
Nothing is installed globally. The runner recompiles tests with ASan/UBSan;
prebuilt mbedTLS is not sanitizer-instrumented. The application under test is.

- `test_time_retry.c`: pure UTC conversion, calendar limits, serial output boundaries,
  retry schedule and control intent policy.
- `test_material.c`: includes the entire production certificate implementation;
  real mbedTLS parses generated PEM and checks keys. Injected heap/parse allocation,
  NVS set/commit/read and notification failures; pthread readers/writers; metadata
  polling and CSR retention. A failed NVS commit deliberately leaves the mock write
  visible, proving the implementation does not assume rollback.
- `test_lifecycle.c`: production HTTPS lifecycle, with mocked server and snapshot
  provider. Tests failed URI registration and failed cleanup without losing ownership.
- `test_coordinator.c`: production coordinator on a simulated pthread scheduler and
  monotonic clock. Tests retry exhaustion, event duplication, fallback, explicit
  stop/restart and stop during an in-flight start. Timing is simulated, not measured
  FreeRTOS scheduling or worst-case latency.
- `test_api.c`: production JSON handlers plus the production certificate module and
  SDK cJSON. HTTP transport/auth middleware are not exercised.
- `test_time_cancel.c`: production clock notification/readiness and cancellation
  functions with mocked syscalls. Tests never call the real `settimeofday`.
- `test_ui.cjs`: executes production functions in Node with DOM/API substitutes;
  bilingual keys, text errors, uncertain results, button recovery and refresh errors.
  This is not browser layout or real Web UI acceptance.

`extract_lifecycle.py` extracts unchanged function bodies into the temporary build
folder to avoid compiling unrelated SDK request handlers. It does not reimplement
these functions. Tests fail to compile if the source boundaries change. Full target
compilation separately validates the complete translation units.

See `docs/CERTIFICATE_FIX_REPORT.md` for all A/B/C/D acceptance mappings, limits,
and the remaining device acceptance procedure.

`./tests/certificate/test_service_stop.sh` (also included in `run_host.sh`)
reproduces the generic manager defect from HEAD and tests the current production
stop/restart functions with mocked callbacks and event delivery. Failure, timeout,
and invalid-state errors prevent restart; successful and duplicate stops and a
successful restart are covered. The user explicitly approved this minimal scope
extension. No source files are changed by this test.
