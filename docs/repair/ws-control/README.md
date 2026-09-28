> 后续项目级契约修复与当前本地验收见 [v3 记录](../ws-project-v3/README.md)。本文件保留上一轮历史证据，不替代 v3 结论。

# PR #42：控制请求归属与业务取消统一修复

修改前基线为 `1591ecc8e293c0fa62feda18d2517edfc179ca5a`。实现阶段仅本地修改与验证；用户随后另行授权提交、推送并更新 PR #42，见 publication-gate.md。本轮没有改版本/发布流程或操作设备。既有 output/、tmp/、另一份未跟踪 reviewer diff-check 保留。

## 结论及修复范围

V1/V2 均在本轮修复前真实源码中复现业务断言失败，当前生产控制层与完整 WebSocket / Exec 调用路径通过相同行为断言。四种 Shell 页面控制共享完整 peer 校验及引用获取；Exec 取消意图与提交资格在同一操作锁内排序，并通过原操作绑定的 SSH `cancelled/cancel_context` 覆盖后续准备、连接和执行。

没有推翻 v2：原上下文、引用、编码前票据、逐目标结算、终态预留/封存、timer daemon 排空、遥测公平性和保护配额继续使用原结构。没有新增任务、队列、业务命令重试或永久保留旧上下文。

详见 [控制契约表](control-contract.md)。`changes.patch` 是本轮实际生产/测试增量，`changes.json` 记录对应文件哈希。

## 控制目标与所有权

- `ws_handler` 先以真实 req 的 sess_ctx 校验 transport 当前完整 peer，再把该 peer 传入唯一 `handle_ssh_control`。`shell_for_peer` 在原 op 锁内完成连接身份匹配、OPEN 检查及 ref++。后续输入、信号、resize 和断开只使用这一个 op；没有二次 current 查找。
- 不匹配向原请求者返回既有 `ssh_status:error`，不操作其他 Shell。持引用后原会话若结束，资源 mutex 和 OPEN 检查阻止对已清理 shell 的访问；控制失败向请求者报错。输入/信号/resize 不自动重放。
- disconnect 保存于原 context，由原 poller 执行，保持 disconnecting/closed 顺序。服务停止使用独立 `shell_owner`，连接关闭使用完整 peer 的 `shell_connection`；页面无法使用 owner 通配入口。
- 补齐了同类旧关闭路径：原 `peer_closed -> cleanup_disconnected_client(fd)` 会查找并关闭复用 fd 的新连接。客户端登记现在保留完整 peer，关闭及清理都匹配原值，terminal takeover 也传递先前捕获的 peer，不从旧回调重新选择当前连接。
- 页面控制只持短期 borrower 引用；网络/资源 I/O 在原资源 mutex 内，op 锁外。执行者、timer、票据及投递仍按 v2 各自归还责任。HTTPD 连接登记继续由既有 owner 管理，没有新增跨任务登记写入者。

## 持久取消与业务阶段

业务状态与网页观察阶段正交：PREPARING、SUBMITTED（表示已认领提交资格，不证明远端收到）、ENDED。原操作固定保存所有停止原因位和首个原因。

1. exact 非零 ID 的取消入口在同一 op 锁内校验已接受业务、尚未 ENDED、登记 USER 并取得原引用；不以 session 当前存在为接受条件。重复取消幂等，错误/旧 ID 和已结束任务拒绝。
2. 配置的 `cancel_context` 指向原 op，session 只借用；执行者一直保活到断开并销毁 session。密钥加载前后观察意图；创建 session 时若发生取消，新 session 的配置继续读到它。连接/认证的底层协作检查使用同一 callback，不重置意图。
3. `claim_submit` 和接受取消使用同一锁。取消先赢不能再获得资格；资格先赢后只能协作中止，可能已经执行，不能承诺从未提交或远端确定停止。这一保守边界在进入原 SSH exec 调用之前，不扩大锁到网络操作，也不重放命令。
4. 业务返回/准备失败在锁内进入 ENDED 后才构造终态。后续取消拒绝，不修改封存结果；执行者、timer、投递可能仍在排空，所以 ENDED 与可回收不同。服务停止仍以依赖是否排空判断 busy/timeout。
5. USER、TIMEOUT、MATCH 分开登记。先接受的原因决定取消解释，后续原因位仍保留。手动取消不会因驱动用 ESP_ERR_TIMEOUT 表示中止而被误报超时。MATCH 的首次提取停止不再因旧共享标记被误报超时；触发匹配、变量提取和 stop_on_match 的条件没有改变。原“已找到匹配时对 deadline 结果按匹配事实处理”的产品规则保留，不把匹配判断全部改成新策略。
6. 观察/输出交付失败只封闭观察，不登记业务取消；此时业务仍可匹配、更新变量及接受明确的用户取消。服务停止也不登记 Exec 取消。已封存的观察失败不会被后来的业务取消重新改写或再发第二终态。

`ssh.cancel` 去掉先查 wildcard 状态再取消的旁路，直接 exact-ID 登记；用 valuedouble 校验完整 uint32 整数，避免高位 ID 被 valueint 截断。既有 `cancelled:true` 表示接受意图，协议字段不变。仅调整双语取消说明为远端终止未确认；前端布局和消息协议没有变化。

## 反例、测试与复现

设置 `IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf`、`DEVELOPER_DIR=/Library/Developer/CommandLineTools`。

| 命令 | 本轮结果与证据 |
|---|---|
| `python3 tests/ws_subscriptions/replay_control_baseline.py` | `baseline-results.txt`：V1/V2 都为业务断言 FAIL，脚本成功表示旧缺陷确认 |
| `bash tests/ws_subscriptions/run_control.sh v1` / `v2` | 当前完整生产路径对应断言 PASS；无参数执行下面全部交叉场景 |
| `bash tests/ws_subscriptions/run_control.sh` | `control-tests.txt`：WS 四入口、完整 peer/旧关闭/持引用、任务/密钥/创建/连接/认证/提交/业务封存阶段、原因、停止和 API |
| `bash tests/ws_subscriptions/run_operation.sh` | `operation-tests.txt`：票据、目标、引用和两阶段终态账本 |
| `bash tests/ws_subscriptions/run_operation_adapter.sh` | `adapter-tests.txt`：G1/G2、完整生产创建/回调/清理、观察失败仍执行业务、timer 排空/停止重试 |
| `bash tests/ws_subscriptions/run_host.sh` | `host-tests.txt`：TX、遥测、事件注销及上层生命周期 |
| `bash tests/ws_subscriptions/run_reviewer.sh` | `reviewer-tests.txt`：R1—R5 有效回归 |
| `bash tests/ws_subscriptions/run_f1_f2.sh` | `f1-f2-tests.txt`：流式失败、保护容量、资源不足和公平性 |
| `python3 tests/ws_subscriptions/replay_operation_baseline.py` | `historical-g1-g2.txt`：原完整冻结生产 G1/G2 仍可重放 |
| `bash tests/ws_subscriptions/run_f1_f2.sh --baseline` | `historical-f1-f2.txt`：旧 F1/F2 反例仍可重放 |
| `bash tests/runtime/run_all.sh` | `runtime-tests.txt`：含真实 SSH 驱动的六阶段 EAGAIN/deadline/cancellation；网络/libssh2 为边界替身 |
| `bash tests/certificate/run_host.sh` | `certificate-tests.txt`：证书/HTTPS 主机回归 |
| `npm --prefix tests/prompts test` | `webui-node-tests.txt`：38 项，含实际取消消息处理及中英文“远端终止未确认”断言 |
| `npm --prefix tests/prompts run test:browser` | `webui-browser-tests.txt`：Chrome 双语 38 项，后端模拟，不是设备联机 |
| 固定 SDK `idf.py -B /tmp/tianshan-control-build/build -D SDKCONFIG=/tmp/tianshan-control-build/sdkconfig build` | `build.txt`、`build-artifacts.json`：ESP32-S3 独立目录编译链接成功 |
| `git diff --check` | 通过，生产/测试修改；原始工具输出和冻结证据不做格式清洗 |

新控制测试编译实际 controller、manager、TX 及整个 `ts_webui_ws.c`。`api_ssh_cancel` 从当前生产文件原样抽出编译，结果容器为边界替身；不是复制一套取消选择逻辑。网络边界注入确定性阶段屏障，真实底层 SSH 驱动另由 runtime 套件验证 callback 在握手、认证及执行等待中的持续检查；二者不能替代真实网络联机。

V1/V2 baseline.tar.gz 保存修改前真实生产源码和原测试；baseline.json 为逐文件哈希和 HEAD/status。baseline-adapter.c 仅替换 SDK/网络、观察输出和调度，baseline-control.c 含相同行为断言；原选择/取消逻辑未经替换。baseline-evidence.json 标识这些证据文件。首次重放因冻结目录缺少未改动的公共头而编译失败，补齐真实头后才得到业务红灯；没有将该编译失败算作缺陷证据。

回调/票据/投递/执行者账本继续由既有套件检查；新增交叉测试每场景最终要求 op 不忙、分配归零、timer 及 TX 排空。测试不依赖随机调度、sleep 或新业务消息来推进停止恢复。新增入口尚未接入 CI，本轮未改 CI。

## 资源与构建

目标编译器测量详见 resources.txt。op 槽由 928 B 增至 944 B，两个槽共增加 32 B；客户端登记由 20 B 增至 56 B，八个槽增加 288 B。相关静态总增 **320 B**（不含链接对齐）。Shell context 仍 20 B、Exec params 仍 792 B、输出票据仍 280 B；没有新动态对象、池或队列。

TX 仍 15 消息对象、96 描述符、1 MiB payload（保护独占 5 KiB），Shell/Exec 终态预留仍 4 KiB/512 KiB。控制 borrower 执行完释放；取消借用在登记及原资源 abort 请求后释放；配置 callback 的原上下文由原 executor/timer 保活至 session/timer 排空。没有额外任务或栈配置。

目标单函数帧：统一 Shell 控制 64 B，cancel/claim 各 32 B；Shell poller 2096 B、Exec callback 1088 B、Exec task 544 B。它们不是完整调用链峰值或实测栈水位。应用镜像 `0x214eb0`，最小应用分区剩余 `0xeb150`，约 31%。首次 SDK/Chrome 启动受沙箱限制，获得工具授权后本地完成；没有执行构建输出中的 flash 示例命令。

## 完整调用链自查与边界

1. **还有页面控制绕过统一目标获取吗？** 当前四个 Shell WS 控制均走同一入口；连接关闭走完整 peer，服务停止单独 owner。Exec API 直接 exact-ID 登记，没有用状态通配选择控制目标。
2. **还有阶段会丢掉已接受取消吗？** 当前受测任务准备、密钥、session 创建、连接/认证、提交、执行均保存同一意图；业务 ENDED 后拒绝控制。同步密钥读取和 lwIP DNS 不可即时打断，但返回后仍观察同一取消，不会重新取得提交资格。
3. **旧请求/回调还会查询当前操作并影响替换实例吗？** 页面控制取得引用后不重新查询；连接关闭不按旧 fd 查找新连接；Exec callback/timer 均持原上下文。保留的 wildcard 仅用于状态/服务维护：维护是对当前服务状态的主动请求，不是旧操作回调。

这些结论基于源码与确定性主机测试，不声称穷尽双核调度。仍需实机：真实慢连接及断网下 Shell 控制/重连、Exec 密钥/DNS/认证/提交时取消、远端实际是否终止、timer/结果排空与服务停止重试、连续启停后的堆/句柄及栈水位。

协议限制：Shell 帧无操作 ID，同一连接上的旧操作控制帧与该连接的新 Shell 无法区分；本轮只保证完整连接身份和已取得的原引用，不扩展协议。Exec cancel 沿用原 API 访问权限，不把 exact-ID 隔离等同新的用户授权机制。没有新增 ACK，传输结算不等于浏览器确认收到；取消被接受也不证明远端命令从未执行或已经终止。
