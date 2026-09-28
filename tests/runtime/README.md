# Runtime repair host tests

Run `./tests/runtime/run_all.sh` from the repository. Requires Apple Command Line Tools (or compatible C compiler), Python 3, Node, and the ESP-IDF cJSON source path used by the individual scripts. Build output is isolated under `/tmp/tianshan-runtime-tests`.

These tests compile production codec, store, SSH, watcher/service and probe sources. Log, engine and synchronous action completion tests extract production function bodies; they do not maintain copies of the algorithms. External FreeRTOS/network/NVS calls are mocked; engine action execution and persistence are separate integration seams, tested independently in their own suites. C tests use ASan/UBSan. The store suite deliberately simulates process death with longjmp; LeakSanitizer is disabled only there because abandoned process allocations are intentional. NVS is a failure-injected memory model, not an ESP-IDF power-loss emulator. UI tests execute actual JS functions with a synthetic DOM/API, not a real browser.

Fixtures contain only synthetic credentials/hosts. Nothing here connects to hardware or runs a real model. Test output does not certify ESP32 dual-core timing, FAT/NVS electrical power loss, real libssh2 transport cleanup, browser rendering or resource p95.
