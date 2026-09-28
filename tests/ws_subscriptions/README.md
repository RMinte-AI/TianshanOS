# WebSocket host regressions

Run from the repository root:

```sh
./tests/ws_subscriptions/run_baseline.sh
./tests/ws_subscriptions/run_host.sh
```

`IDF_PATH` selects the SDK cJSON source; the local fallback is the existing ESP-IDF v5.5.2 installation. No device, socket server, firmware write, remote operation or git mutation is performed.

- Baseline script obtains the pinned historical **source and header** using `git show`, compiles them in a temporary directory, and expects business assertions to fail. T02/T05 execute the original dashboard callback; T07 counts actual calls to the global broadcast boundary (the 3-client fanout is an explicitly labelled boundary calculation). The event test runs the original dispatcher and ASan detects its self-unregister UAF. The current checkout is never replaced.
- Manager test compiles/includes the entire current `ts_ws_subscriptions.c` and `ts_ws_transport.c`, plus SDK cJSON. Only platform boundaries are replaced. No duplicated scheduler / delivery implementation is used.
- Clock and HTTPD execution are deterministic. Worker entry is real C, but the host does not emulate full FreeRTOS scheduling. Four barriers (snapshot, API, queued work, send-before-completion) each exercise cancellation, resubscription, disconnect, fd reuse and stop/restart exclusion. The current in-flight frame is allowed to finish at the final barrier.
- Event test compiles/includes current `ts_event.c`, using real pthread mutex/condition barriers for concurrent dispatch/unregister. It covers self-unregister, retiring another selected node, synchronous drain, timeout/retry, and unrelated handlers. ASan/UBSan are enabled. TSan was also run separately on this event test; that is not a TSan proof of the mocked RTOS scheduler.
- Lifecycle tests extract the unmodified bodies of production WebSocket init/stop and HTTP/WebUI stop/deinit/status functions at test time; HTTPD stop and hooks are controlled boundaries. They verify actual failure propagation and route-copy ownership, not a logic copy.
- The 32-service escaped-name payload is a size stress fixture for the production serializer, not a replacement implementation of the service API. The 512 KiB legacy-frame boundary and total bytes / descriptor ceilings are tested separately.

Mock success of `httpd_ws_send_frame_async` means only success at that API boundary. It is not proof of browser delivery, TCP timing, device heap, CPU utilization, or stack high-water on ESP32-S3. See `docs/repair/ws-subscriptions/README.md` for the acceptance matrix and device checklist.


## R1—R5 Reviewer 修复回归

- `bash tests/ws_subscriptions/run_reviewer_baseline.sh`：使用归档和 SHA-256 复现五项未修复行为，退出 0 表示反例成立。
- `bash tests/ws_subscriptions/run_reviewer.sh`：完整生产 manager/transport/event C + 自动提取的当前 WebUI/HTTP/服务/核心函数，验证暂停/排空、会话就绪、日志代次、聚合停止重试、主题公平性和联合资源预算。SSH/设备/SDK 是模拟，未执行远端操作。
- 修复记录：`docs/repair/ws-subscriptions/reviewer-fixes/README.md`。

## F1/F2 后续修复

```sh
bash tests/ws_subscriptions/run_f1_f2.sh --baseline
bash tests/ws_subscriptions/run_f1_f2.sh
```

第一条校验本轮冻结的真实工作区源码，并用相同 F1/F2 断言确认旧行为失败；成功退出只表示反例成立。第二条执行当前完整 transport/manager 与提取的真实 Shell、exec 输出/终态和保护事件调用方，覆盖入队/发送失败、迟到结算、身份隔离、部分失败、独立容量、停止交错和持续竞争。两者均不连接设备。SDK cJSON 可用 `IDF_PATH` 指定，编译器可由标准 `CC` 环境变量指定。

后续 CI 可在已有 ESP-IDF 源码准备好后安装 C 编译器和 ASan/UBSan，设置 `IDF_PATH`，依次执行 `run_host.sh`、`run_reviewer.sh` 和 `run_f1_f2.sh`。基线重放可作为单独的证据任务。本轮未修改 CI 工作流。详见 `docs/repair/ws-f1-f2/README.md`。
