# PR #42：Shell / Exec 生命周期统一修复（实现与验证记录）

本轮修改前基线为 `74b1b646ec4a45f9159bd028e622b13fca5eff75`，分支 `fix/v0.5.2-reliability-and-prompts`。本轮已完成代码迁移；G1/G2 在完整冻结生产编译单元中复现，当前完整生产路径通过对应行为断言。**这是本地证据，不是设备验收或发布许可。**

实现与验证阶段未提交、推送或更新 PR；随后用户另行授权将本轮代码和证据提交至 Fork 并更新 PR #42，见 `publication-gate.md`。本轮没有改变版本或发布流程，也没有连接设备、刷机、重启或执行远端命令。原有 `output/`、`tmp/`、未跟踪的 reviewer diff-check 文件保留。

## 1. 基线与实际反例

- `baseline.tar.gz`、`baseline.json`：开始修改前的生产源码、原测试、HEAD、分支、状态和逐文件 SHA256。
- `public-headers.json`：反例编译所用且与基线相同的真实业务头文件哈希。
- `design.md`：编码前形成的调用图、状态、所有权和迁移底稿。
- `baseline-g1-g2.txt`：完整冻结 WS / manager / TX 编译单元的反例日志；只插入调度暂停点，不替换状态声明或清理函数。

**G1 的可达调度**：poller 提交输出后已返回；发送 worker 稍后向 SDK 入队失败；失败回调减掉输出计数后暂停。HTTPD 关闭原连接，真实 `ssh_cleanup` 等待独立 poller 退出；旧目标已失效，旧终态同步拒绝并结算。随后 HTTPD 重连并创建新 Shell。旧 worker 回调恢复后误用新全局状态，把新会话关闭。冻结源码断言 `replacement_corrupted=1`。

**G2 的可达调度**：输出 A 等待 SDK 提交，生产者开始编码 B。B 通过旧 stream_publish 的失败检查后暂停；worker 处理 A 的真实入队失败，真实 output_done 封口并发出 error。B 随后仍提交成功，观察到终态之后又出现 output。冻结源码断言 `terminal_before_late_output=1`。

本轮早期的隔离夹具用于定位；最终证据已替换成上述完整编译单元、真实声明及清理路径。G1 不依赖把同一 poller 的同步调用人为当成两个并发任务。反例脚本只把编译成功后的业务断言失败视为红灯。冻结旧 Exec 另有凭据字符串泄漏，反例夹具在真实 task cleanup **之后**释放这些独立字符串，仅用于恢复测试分配账本，不代替生命周期逻辑。当前统一析构已正确释放这些原操作拥有的字符串。

## 2. 唯一状态来源与并发决定点

新增内部 `ts_ws_operation.c/.h`，固定 Shell、Exec 各一个槽，没有新增任务或扩展会话并发。

| 状态 | 准入和退出条件 |
|---|---|
| FREE | 无资源和活引用；create 与停止创建门在同一个 `s_op_lock` 内排序 |
| STARTING | 创建租约已占槽，保留终态实际内存及描述符；资源准备失败走 discard |
| OPEN | 允许取输出票据；Shell 必须成功发送同步 connected 后才开放；Exec 只表示观察准入，start 消息仍在 SSH 连接成功后发送 |
| CLOSING | 同锁关闭准入；首个关闭者持 builder 引用冻结接收者、准备结果；先前失败可以补入最终事实 |
| FINALIZING | builder 完成、preparing=0、unsettled=0 后唯一认领；移动并清空槽内 reservation 别名 |
| TERMINAL_PENDING | 终态文本已封存；整批目标先登记，再逐目标提交；finalizer 引用保留到全部 submit 返回 |
| TERMINAL_SETTLED | 每个终态目标已有本地结算；不代表浏览器确认收到，也不代表业务执行者结束 |
| RECLAIMING | refs=0 且所有执行、准备、投递责任配平；拒绝 acquire/create，锁外析构，再标 FREE |

决定点均在 `s_op_lock`：create/stop、acquire、output_begin、close_locked、整批 target 注册/单次结算、finalizer 认领、RECLAIMING。锁内只处理固定大小状态及计数，不执行 JSON、分配、网络或等待；没有和 TX 锁嵌套。

票据在复制观察输出、JSON 编码、payload 分配前取得，携带原操作、序号、原目标快照及准备责任。票据持续到整批交接返回。失败回调把“失败事实、关门、目标计数变化”放在同一个锁内，保留原操作引用到回调最后一次访问之后。重复/旧 token 不重复减计数，也不会作用于替换操作。

普通观察消息的生产者保持单一：Shell poller；Exec executor（start/output/match）。用户 disconnect 请求交给原 Shell poller 顺序发 `disconnecting` 再关闭，保留原有消息类型和字段。connecting/connected 是 HTTPD 同步控制路径，由创建引用和 STARTING 阶段保护；poller 在 OPEN 前不能输出，停止 barrier 必须等 handler 返回。

## 3. 所有权与完整迁移

| 资源/责任 | 原 owner 与释放点 |
|---|---|
| 创建租约 | create 在分配前占槽；task-create 前增加独立 executor 引用；创建者直到返回前都保留自己的引用，允许 task 早于 create 返回执行完 |
| Shell 资源 | task arg 的 `ssh_shell_context_t`；poller/失败创建者关闭 session/shell；输入、resize、signal 借用原操作，和清理共用资源 mutex；最后析构仅释放 context/mutex |
| Exec 参数、凭据、输出缓存 | task arg 的 `ssh_exec_task_params_t`；输出回调拿原 params 并借用原 op；业务结束关闭 session，最终析构释放参数、字符串、缓存、mutex |
| 输出准备票据 | begin 增 preparing/ref；无目标不编码；全部交接或失败后统一 finish，不能中途漏还 |
| 输出/终态投递 | 固定 token 记录；接受后由 TX done 结算，拒绝由发布者结算；早回调时 ticket/finalizer 仍保活；不重播整个多目标批次 |
| 关闭 builder | 首个 close 独占；Exec 在此冻结全局有效接收者，Shell 固定原 peer；之后加入者不追补旧终态 |
| 终态预留 | 每个原操作创建时取得；finalizer move 一次；封存后不修改文本；无目标、拒绝、SDK 重试耗尽和实际发送失败都释放 |
| Exec timer | timer ID 为原 params；创建到 timer daemon 排空 barrier 完成期间额外持引用；不把 delete 命令入队当成回调退出 |

删除了旧 `s_ssh_state/generation/running/poll_alive/result_pending/output_pending/terminal_ready` 和 Exec 的 current params/session、creators、pending、claimed/result_active 等竞争状态。会话身份统一由原操作 ID 给出，跨服务重启不复用；32 位公开 ID 耗尽后拒绝新创建，不回绕制造 ABA。

`ssh_send_output`、Exec output 的真实上游编码、started/match、错误/完成、cancel、连接失效、输入/resize/signal、cleanup、创建失败和停止均已迁移。业务模式匹配、变量更新、底层 SSH deadline/abort 规则未改；观察输出失败只封闭 Exec 网页观察，后续业务仍正常收集和匹配，不自动重放或额外取消远端命令。

额外补齐同一不变量所需的错误处理：真实创建错误码向上传递；JSON 任一必要字段构造失败使用完整 fallback，不发送半个协议对象；失败的 task-create 释放自己的 executor 责任；cancel(0) 仍按原契约拒绝。

## 4. 推进与停止

现有 subscription worker 的 `run_batch` 调用 `ts_ws_op_poll`，负责封口、回收及 timer 排空；没有另起线程、事件队列或无限保留消息。

停止顺序：关闭创建门 → 暂停遥测 → HTTPD handler barrier → 请求原 Shell 退出 → Exec 业务仍执行则返回 busy；其他原操作在最多 2 秒的协作窗口排空 → 同步注销并排空保护事件、退出日志回调 → manager drain → TX drain → worker 退出。

任何 busy/timeout/error 都保留后续所需依赖；worker/HTTPD/event 回调自停被拒绝。健康 Shell 能在同次停止中排空，慢发送或旧 timer 仍占用时保留状态供重试。上层 HTTP / WebUI / service / core 的错误传播不需再改，已运行其真实函数回归。

Timer delete 和 daemon barrier 每个阶段最多 300 次失败入队尝试，失败间隔至少 100 ms；成功的 delete 不再重发，只重试尚未接受的 barrier。耗尽后记录错误并保留原引用，停止重试显式恢复同一阶段。SDK 接受 barrier 后若 daemon 永久不运行，不能安全推断回调已退出：保持 busy/timeout 与有界占用，不伪造回收。临时队列拥塞在恢复后无新业务消息也能自行推进。

SDK 依据：固定 v5.5.2 的 `FreeRTOS-Kernel/timers.c::prvProcessReceivedCommands` 在同一 daemon 顺序执行 DELETE 和 pended function。原 Exec 仍使用 SSH 驱动自身 deadline；本轮未改变额外 timer 分配/启动失败时的业务策略，测试确认其引用可正确退出。当前 sdkconfig 启用 `CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND=y`；已核查 SDK newlib 外部 RAM 原子操作路径及目标编译代码，没有假定原生 CAS 能访问 PSRAM。

## 5. 验证与证据边界

下列均为本轮实际执行；日志在同目录。主机 C 使用真实 SDK cJSON、ASan/UBSan，SDK/网络/时钟由可控边界替身提供。

| 命令（先设置 IDF_PATH） | 结果 / 日志 |
|---|---|
| `python3 tests/ws_subscriptions/replay_operation_baseline.py` | 确认 G1/G2 真实旧行为失败，`baseline-g1-g2.txt` |
| `bash tests/ws_subscriptions/run_operation.sh` | 生产控制层+TX：36 个屏障调度、16 个编码/SDK/socket/stale 组合及重复 token 账本，`operation-tests.txt` |
| `bash tests/ws_subscriptions/run_operation_adapter.sh` | **整个生产 WS 编译单元**、实际声明、创建/任务/回调/cleanup/stop，`adapter-tests.txt` |
| `bash tests/ws_subscriptions/run_host.sh` | 遥测、TX、事件节点保活/排空、HTTP/WebUI、初始化失败，`host-tests.txt` |
| `bash tests/ws_subscriptions/run_reviewer.sh` | R1/R4 停止、R3 角色、R5 公平性及上层依赖，`reviewer-tests.txt`；Shell R2 的创建失败/早关闭在完整 adapter 入口保留 |
| `bash tests/ws_subscriptions/run_f1_f2.sh` | F1/F2、分配/描述符/字节容量、分类、部分失败、连续业务、停止、公平性，`f1-f2-tests.txt` |
| `bash tests/ws_subscriptions/run_f1_f2.sh --baseline` | 历史 F1/F2 红灯仍可复现；使用匹配旧 ABI 的冻结夹具，`historical-f1-f2.txt` |
| `bash tests/runtime/run_all.sh` | 当前运行时回归，`runtime-tests.txt` |
| `bash tests/certificate/run_host.sh` | 当前证书/HTTPS 回归，`certificate-tests.txt`；其中 REPRODUCED 为套件固定旧版反例 |
| `npm --prefix tests/prompts test` | 38 项 Node 检查，`webui-node-tests.txt` |
| `npm --prefix tests/prompts run test:browser` | 中英文 Chrome 38 项，后端模拟，`webui-browser-tests.txt` |
| `idf.py -B /tmp/tianshan-op-build/build -D SDKCONFIG=/tmp/tianshan-op-build/sdkconfig build` | ESP-IDF v5.5.2 / ESP32-S3 独立目录编译、链接成功，`build.txt`、`build-artifacts.json` |
| `git diff --check` | 通过，`diff-check.txt` |

B01—B10 覆盖取得引用/票据、准备与注册、SDK 回调早于 submit 返回、失败回调尚未退出、关闭构造暂停、终态部分交接、业务仍运行、回收前拒绝复用；B11 使用真实停止和现有 worker 验证无新消息推进。新增 adapter 场景还覆盖：旧连接失效重连、终态快照后新增连接、用户 disconnect 消息、cancel ID、timer 排空/超限重试/早 barrier、真实匹配和变量更新、16 个创建分配位置及 48 个输出编码分配位置。

账本断言为 `tickets=finished+preparing`、`registered=settled+unsettled`、finalizer 至多一次、重复 token 不重复结算、FREE 无活引用/资源；结合 TX 原有 accepted/settled 与分配计数检查泄漏和双重释放。不是用测试数量代替行为证明。

旧 reviewer/F1/F2 测试已迁移到完整 WS 编译单元，保留有效行为断言。初始化故障注入仍是单独的 initializer 测试，不再用简化 cleanup/global 夹具证明新的会话协议。没有修改 CI 工作流；后续可在提供 IDF_PATH 的主机 job 接入上述两个新 operation 入口。

首次构建被沙箱的系统进程查询限制阻挡、首次 Chrome 启动被沙箱阻挡；获工具批准后完成本地重跑。没有将启动失败当作业务反例。没有运行 TSan：当前 SDK 替身的全局可控调度不是任意并发线程模型。屏障枚举不等于穷尽 ESP32 双核调度。

## 6. 资源预算（目标编译器测量）

详见 `resources.txt`，目标工具链的 sizeof、nm 和 `-fstack-usage`，不是主机指针尺寸估算。

- 两个固定操作槽各 **928 B**，共 **1856 B**；控制锁/序号/标志另 26 B。移除旧 Shell/Exec 全局 630 B，相关静态净增 **1252 B**（不含链接对齐）。
- 每槽固定 16 个普通输出 token、8 个终态 token，每 token 16 B；它们是责任记录，**没有增加 TX 消息或描述符容量**。Shell 实际终态只有一个目标。
- 栈上单个票据 **280 B**；Shell context 堆对象 20 B；Exec params 从 744 B 到 792 B。每种操作各一个资源 mutex（SDK 类型 92 B），同时活跃的额外业务堆开销约 **252 B**，不含分配器对齐。Exec params 仍优先 PSRAM。
- Exec timer 对象 44 B，同时至多一个；从旧全局长期留存改为每代排空释放。daemon 使用既有队列，没有新任务或私有消息队列。
- 传输预算仍为 15 个消息对象、96 个目标描述符、payload 总上限 1 MiB，其中保护独占 5 KiB；普通/遥测/结果/保护转换/周期分类不变。Shell 4 KiB、Exec 512 KiB 的终态容量仍在创建前预留。
- 单函数目标栈帧：`ts_ws_op_poll` 576 B、`ts_ws_op_output_finish` 176 B、Shell output 320 B、Shell poller 2096 B、Exec output callback 1088 B、Exec task 560 B、run_batch 3360 B、power callback 736 B。**不是完整调用链峰值或实测水位**。原 worker 8192 B、Shell 4096 B、Exec 8192 B 配置未增加。
- 当前应用 `0x214a50`，最小应用分区剩余 `0xeb5b0`（约 31%）。代码/固件 SHA256 记录用于识别本次产物，不证明设备运行效果。

U01/U02 对应原上下文与引用；U03/U04 对应 ticket/close 同锁；U05/U06 对应 preparing、目标和回调保活；U07/U08 对应 token 一次结算与原 TX 类别；U09/U10 对应终态封存及 executor 分离；U11 对应锁表；U12 对应真实 stop；U13 对应固定资源和 worker 推进；U14 对应当前 R1—R5/F1/F2、运行时、证书和双语回归。

## 7. 未执行及实机验收

没有发现本地证据中尚未关闭的 G1/G2；也不据此宣称无竞态。尚未执行：真实 SSH 服务/设备网络、ESP32 双核并发、慢浏览器/多标签连接、固件运行、堆水位和栈水位、真实 timer daemon 拥塞与完整 WebUI 服务重启。

需要另行授权实机验收：Shell 创建/输入/远端关闭/断开重连、Exec 正常/匹配/取消/超时及观察断线后业务继续；慢连接下普通输出与保护通知、遥测的公平性；发送/事件/timer 排空超时后的停止重试；多轮启停后的堆/句柄/任务和高水位。特别检查新增票据后的 Shell/Exec/worker 最低栈余量，以及 PSRAM 原子操作配置和真实调度表现。

本轮自查按完整调用链检查了旧字段残留、创建返回前 task 退出、回调计数归零后的引用、半成品 JSON、断开状态消息、cancel(0)、timer delete 的假排空、停止自等待及资源门限；发现的问题已修复并进入回归。不存在未经迁移保留的旧会话全局协议。前端布局、外部消息类型和字段、硬件保护以及 SSH 匹配规则保持原样。
