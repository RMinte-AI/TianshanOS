# R1—R5 修复与验证记录（2026-09-25）

结论：五项确认问题已完成本地修复及生产代码回归；按需采集、同批指标共享、单次序列化与定向发送方案保留。此结论不等同于实机验收，也不覆盖所有历史 SSH/事件总线代码。

## 基线与改动范围

本轮起点是 HEAD `35238b4645ae5e9e0e99555d4fb64080d945565c` **加上当时全部相关未提交修改**，不是回退到该提交。`baseline.json` 记录分支、HEAD、逐文件 SHA-256；`pre-fix-source.tar.gz` 保存相关生产源码和当时的主机测试。没有覆盖原来其他工作，没有 commit、push、PR、版本、发布流程或设备操作。

在修改前复跑第一、二轮反例；另提供 `bash tests/ws_subscriptions/run_reviewer_baseline.sh`：校验归档内哈希，在临时目录编译归档中的生产 C，重新确认五项缺陷。该脚本退出 0 表示反例成立，不是修复通过。`baseline-replay.txt` 是本轮冻结基线的重放记录。归档含旧测试辅助代码；SDK cJSON 使用本机 ESP-IDF v5.5.2，未把其副本封入归档。

本轮差异见 `reviewer-fixes.patch`，相对于上述未修复基线生成，避免混淆更早的 WebUI 修改。`final-manifest.json` 记录最终源文件和证据哈希。

## 逐项状态与行为证据

| 问题 | 本地处理状态 | 行为证据 |
|---|---|---|
| R1 停止失败后发送无人推进 | 已修复 | 实际 `ws_stop` 返回 busy 后，真实 worker 仍处理最后一条保留的 SSH exec 结果；不再有新生产者消息时，两连接仍收到结果。HTTPD 屏障超时、transport 排空超时也保留推进能力，重试可完成。实际 `ws_handler` 在停止中继续响应心跳，拒绝新命令但不主动断开结果连接。 |
| R2 失败启动误报成功、终态被挤掉 | 已修复 | 实际 Shell 连接处理器注入任务创建失败、创建后 Shell 已关闭，最后收到错误而非 connected；普通消息池占满不影响保留终态。旧 READY 在终态后失效；SDK 拒绝队列提交时，关键终态保留并在退避后继续发送。普通 Shell 输出提交失败会明确报错并结束该流，不伪装为完整输出。 |
| R3 日志资格与角色脱节、重新订阅恢复旧日志 | 已修复 | 实际角色切换同步日志资格；日志→终端、日志→SSH 的旧日志失效。取消后同连接重新订阅，旧排队日志仍因代次不符被丢弃，新日志可发送。日志回调注销 busy 时保留句柄，随后重新订阅可以恢复启用。 |
| R4 上层忽略停止失败 | 已修复 | 实际服务聚合停止在第一个失败处返回，保留服务表与低阶段依赖；错误消失后重试成功。真实事件回调保持在途时，核心收到真实排空超时，日志/配置未被释放。分阶段 deinit、初始化回滚失败及之后重试均保留正确状态；重复 stop 不重放 shutdown 事件。 |
| R5 主题饥饿、无发送预算仍采集 | 已修复 | 八主题、六个普通遥测消息槽的受控调度中，100 轮各主题均在模拟 SDK 发送边界观察到 75 帧；基线后两主题为 0。消息槽、描述符或总字节预算分别耗尽时均无 API 采集；容量释放后仍到期的主题可继续服务，无需先等待另一个完整 interval。 |

测试用的是完整生产 manager/transport/event C，以及从当前文件提取的完整 WebUI/服务/核心函数体。没有另写一份业务逻辑副本。SDK 网络、RTOS 调度、设备 API、SSH 远端响应为模拟；事件排空使用真实 pthread 屏障。R1 组合测试运行真实 worker 的一个调度片段及 SDK 回调，没有用原 `pump()` 替生产代码补一次 flush 来证明其进展。

## 实际逻辑与文件

- `ts_ws_subscriptions.c/.h`：增加 DRAINING 阶段。暂停新订阅/周期采集与销毁 worker 分离；已有操作结果订阅保留到生产者收尾、结果队列排空。按轮转顺序选择主题，采集前取得实际消息、发送描述符及字节所有权；不足时不前移该订阅的服务期限。堆失败有退避。
- `ts_ws_transport.c/.h`：增加有界 reservation，供遥测批次与已接受 SSH 任务使用。预留不等于已发送；实际提交后由 HTTPD 所有者检查连接身份。停止时拒绝新的提交，但继续发送已经接受的工作；关键终态遇 SDK 入队失败保持 FIFO 所有权，100 ms 退避重试，不重新执行 SSH 命令。日志消息附带订阅代次；容量/分配拒绝进入计数。
- `ts_webui_ws.c`：停止顺序为关闭新业务 → 暂停遥测 → HTTPD 请求屏障 → 等待 SSH 创建/执行与轮询收尾 → 注销自己的事件/日志生产者 → 排空结果 → transport drain → manager deinit。任何失败保留后续依赖和 worker。新握手拒绝，已有连接可心跳/取消订阅，新命令返回协议错误。角色和日志资格统一更新。
- 同文件的 Shell：任务创建成功且当前 Shell 活跃后才进入 READY；轮询任务在 STARTING 时不输出。状态使用单调会话代次，READY 不能在终态后重新生效；上一终态尚未完成发送时不接受替代会话。Shell 和 exec 在接受前分别拥有关键结果预留。终态编码失败/过大时发送明确错误或结果未知，不能冒充成功。对端已断开或实际 socket 发送失败仍可能无法送达，记录失败并按连接生命周期处理，不宣称端到端可靠送达。
- `ts_http_server.c`：停止阶段在读取请求体、调用业务 handler 前拒绝新的非 GET 请求；GET 读请求保留。底层停止失败继续保留实例。
- `ts_service.c`、`main/ts_core_init.c`：逐层传播停止、保存与销毁失败；按阶段保留初始化/停止状态，阻止未完成停止时误报重新启动成功。初始化回滚复用同一释放顺序；已完成阶段不重复释放；同一停止意图不重放 shutdown 事件。
- `tests/ws_subscriptions/`：新增冻结基线重放、manager/transport/WebUI 组合、服务、核心及 HTTP 入口回归；旧主机测试的提取依赖、SDK 请求结构与发送观察 hook 随真实接口更新。

没有改变硬件保护、风扇/传感器底层采样、SSH 匹配规则或前端框架。原普通日志/广播仍为有界发送，并未被改为“无限可靠流”；关键结果有独立配额和可观测失败处理。

## 资源预算和取舍

默认 8 个连接时，普通部分保持 32 个主题描述符 + 8 个普通广播/流描述符、6 个遥测消息槽 + 2 个普通文本槽；额外保留 2 个关键结果消息槽和 16 个结果目标描述符。**所有消息共同受 1 MiB 总字节上限约束，未上调总上限。**

Shell 接受前预留 4096 字节及一个目标；exec 接受前预留 512 KiB 最大兼容结果缓冲及最多 8 个目标。exec 完成时快照实际全局接收者，保留原广播接收集合语义。预留会占用可用堆与普通消息预算；资源不足时明确拒绝接受新任务，不等任务执行完才发现没有终态容量。此前已接受结果尚在发送时，也可能暂时不能接受下一任务。

目标编译器检查：worker 仍为一个，栈 8192 字节、优先级 2；`run_batch` 栈帧 3360 字节，`collect_batch` 176 字节，worker 自身 32 字节。它们**不是完整调用链峰值或实机水位**；API/cJSON/SDK 调用仍有额外栈开销。静态 transport 描述符表 4480 字节、消息控制表 160 字节，两份任务 reservation 各 136 字节。详见 `resources.txt`。

## 实际执行命令与证据

| 层级 | 命令 | 结果/证据 |
|---|---|---|
| 未修复基线反例 | `bash tests/ws_subscriptions/run_reviewer_baseline.sh` | 冻结源码哈希核对通过，五项缺陷再次确认；`baseline-replay.txt`。 |
| 本次生产 C 组合回归 | `bash tests/ws_subscriptions/run_reviewer.sh` | ASan/UBSan 下上述行为断言通过；`reviewer-tests.txt`。 |
| 既有 WS/事件/生命周期主机回归 | `bash tests/ws_subscriptions/run_host.sh` | 通过；`host.txt`。 |
| 既有运行时回归 | `bash tests/runtime/run_all.sh` | 通过；`runtime.txt`。 |
| 既有证书/HTTPS 回归 | `bash tests/certificate/run_host.sh` | 通过；`certificate.txt`。其中 REPRODUCED 为该套件自己的旧版反例。 |
| 前端静态/Node 逻辑 | `npm test --prefix tests/prompts` | 通过；`prompts.txt`。 |
| 本地 Chrome 浏览器 | `npm run test:browser --prefix tests/prompts` | 中英文既有回归通过；`browser.txt`。首次沙箱内 Chrome 启动失败，沙箱外使用临时配置重跑成功。WebSocket/设备响应模拟，不是 C 后端联机测试。 |
| 目标编译/链接/分区 | ESP-IDF v5.5.2：`idf.py -B /tmp/tianshan-ws-build/build -D SDKCONFIG=/tmp/tianshan-ws-build/sdkconfig build` | 本地 ESP32-S3 固件构建通过，结果与分区余量见 `build.txt`。未刷写。 |
| 静态差异 | `git diff --check` | 通过；`diff-check.txt`。 |

构建前设置 `DEVELOPER_DIR=/Library/Developer/CommandLineTools`，Python 环境为 `/Users/massif/.espressif/python_env/idf5.5_py3.12_env/bin`，并加载 `/Users/massif/esp/v5.5.2/esp-idf/export.sh`。资源检查使用同一 `compile_commands.json` 的目标编译参数加 `-fstack-usage`，输出放在 `/tmp/ws-reviewer-stack`。

## 未执行和实机验收边界

没有设备连接、刷机、重启、真实 SSH、远端模型或电源操作。没有 commit/push/PR/发布操作。未运行 ESP32 双核调度压力、真实 TCP 慢接收/RST/LRU、长时间运行、实际最大业务数据、PSRAM 碎片及真实栈水位；本地浏览器未连接实际生产 C 服务器。

需要另行授权的实机验收：

1. 多标签页快慢订阅、八主题同时到期；无需求/未到期时采集计数不增长，容量恢复后各主题有进展。
2. SSH 运行期间停止 WebUI，观察 busy/timeout、心跳及最后输出/终态；失败后不产生新消息也能排空，随后重试完成停止，再启动成功。
3. 任务创建/内存不足、启动后立即远端关闭、SDK 控制队列拥塞与连接断开；不出现迟到 connected，也不重放远端命令。
4. 日志→终端/SSH、取消再订阅、断线重连与 fd 复用；旧日志不恢复资格，原全局告警接收行为保留。
5. 服务/事件停止超时，确认依赖继续存活，资源只在排空后释放；观察堆回归、固定池峰值、发送耗时及 worker 最低栈水位。

已修复指上述本地证据覆盖的五项行为，不代表全部历史后台发送者、全局事件总线外部生产者并发销毁或全部设备故障模式已经通过验收。
