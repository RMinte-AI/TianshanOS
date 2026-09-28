> 实施阶段之后，用户另行授权更新 PR。提交前审查与补充复跑见 [publication-gate.md](publication-gate.md)；下文“不提交”描述原实施阶段边界。

# PR #42 v3 本地修复与验收记录

本记录是本轮当前结论；ws-operation / ws-control 下的记录是历史输入，不替代这里的源码和测试证据。只完成本地修改、模拟边界测试和独立目录编译，没有提交、推送、更新 PR、设备操作或远端业务执行。

## 基线、影响与处理结果

当前分支 `fix/v0.5.2-reliability-and-prompts`，HEAD / 审查时 PR head `568b871f64902f88b3c68148ce072eda218fe9db`；PR base `main` / `d6ed947a592265fa12754828bc79803fc50c1db2`。初始已有未跟踪的 output/、tmp/、docs/runtime-repair/evidence/reviewer-fixes/diff-check.txt 不纳入本轮改动。浏览器历史套件按原入口在 output/playwright 输出截图，不清理其中其他文件。

- [影响及消息契约](impact-contract.md)：先于产品修改建立，补充了实际依赖的 CLI 本地退出。
- [累计 PR 文件范围](cumulative-files.txt)、[导航/API/服务注册证据](registries.txt)：用于确定深入、相邻回归和不修改范围。累计审查侧重实际依赖契约，不是声称逐行证明整个仓库无缺陷。
- [原始工作区源码与哈希](baseline.json)、baseline.tar.gz：本轮修改前真实 WebUI、SSH 及相关测试。H1 不使用目标分支旧代码作为反例。
- CLI 原始文件另存 baseline-console.c / baseline-console.json：从同一初始 clean tracked HEAD 导出；它不冒充第一次归档已包含的文件。
- [实际产品与测试 diff](changes.patch)、[最终输入哈希](final-inputs.json)。v2 operation、TX、订阅管理器、Exec 取消与匹配实现均未修改。

| 问题 | 原始证据 | 本轮处理及当前行为 |
|---|---|---|
| H1：请求错误误作 SSH 终态 | baseline-h1.txt/json：实际 C 发帧后生产 JS 退出 SSH，后续 input 不再产生 ssh_input；C op 仍 OPEN | 请求错误只提示本次动作；有效 SSH 不写本地提示符、不自动接管/恢复/重发。真实通道故障走原 operation 唯一终态 |
| 无数据误作 EOF | baseline-no-data.txt：零字节、无 EOF 却结束 Shell 的业务断言 | read/poll/run 使用同一下层判定；暂无数据返回 TIMEOUT 并保持 RUNNING |
| EOF/ERROR 后 close 不释放 | baseline-eof-release.txt：真实 client 销毁后 Shell 包装仍泄漏 | close 消耗所有已分配句柄，不以 RUNNING 作为释放条件；回调至多一次；上层重复清理使用已置空句柄 |
| EAGAIN 无界等待及部分写无明确失败状态 | 原 open/PTY/start/write/resize/close 循环；真实 driver 故障测试 | setup 共用 10 秒预算，write/resize 各 1 秒；部分写记录接受字节，致命/不确定错误标记 ERROR；不重放输入。close 不等远端 EOF，必要时借原 client 本地 disconnect 释放 |
| CLI Ctrl+\\ 退出意图未被循环观察 | baseline-cli-exit.txt：实际 CLI 回调置 console interrupt 后 Shell 仍 active | 该回调请求本地循环退出；保留原 drain-until-no-data 节奏，退出后由原 owner close/destroy；不声称远端已停止 |
| 旧 socket 回调污染新页 / disconnecting 被当终态 | 消费者反向检查及新旧 socket 交错 | 回调、恢复与重连 timer 绑定 socket；连接断开清 SSH 模式；disconnecting 等待真正终态；页面销毁后迟到结果不生效 |

## 实际修改映射与所有权

1. `ts_webui_ws.c`：完整检查 `ssh_request_error` 所有调用点（connect 参数、准入/创建资源失败、统一四控制入口）。这些调用均为请求结果。真正已接受操作的启动/读取/写入故障继续由 `ssh_close` 封口；没有改变公共 request API，也没有修改 operation fallback。IO mutex 外调用关闭和 JSON 编码；原 operation 引用一直保留。
2. `ts_ssh_shell.c/.h`：统一 read 分类，处理 EOF/ERROR/部分初始化的真实释放；有限等待、部分写计数；增加仅给已串行持有 Shell 的调用方使用的 `request_close`。它只是驱动现有状态上的本地退出，不是第二套操作控制层。分配的 Shell 始终需要 close；close 后不可再次使用旧指针。
3. `ts_cmd_ssh.c`：唯一新增一行把 Ctrl+\\ 退出传给上述原 Shell，CLI 原清理顺序不变。
4. `terminal.js`：请求错误、启动失败、会话终态、socket 关闭分别影响对应状态；提示符、恢复和下次输入路由一起适配。固定消息映射只翻译展示，不用于判断生命周期；未识别错误仍保留原始文本。
5. 中英文语言包、index cache key：启动、请求、观察不完整和远端结果未确认表达一致；仅更新静态资源缓存标识，没有改固件版本、布局或发布流程。
6. 新测试桥包含整个生产 WS 单元、operation、manager、TX、真正项目 SSH client/Shell。SDK、libssh2、socket、时钟、xterm 渲染为边界替身。CLI 小回调由当前生产文件提取原文字节编译，无手写逻辑副本。历史 adapter 只增加真实下层编译模式和状态查询边界，保留原断言。

下层释放依据另外检查了仓库内 libssh2 的 `_libssh2_channel_free` 与 session_free：channel_free 只在 EAGAIN 时保留 channel；其余关闭错误不阻挡 free。项目 client 的本地关闭回调使清理看到断开的传输，随后释放 session/socket。组合测试保留 EAGAIN 时的所有权，未把项目 close/destroy stub 成直接 free。模拟库不证明真实服务器/双核时序。

## 可重放验证入口与证据

环境：ESP-IDF v5.5.2，ESP32-S3，Apple CommandLineTools；ASan/UBSan 主机 C；Node 和隔离 Chrome。设置 `IDF_PATH` 指向固定 SDK；macOS 测试设置 `DEVELOPER_DIR=/Library/Developer/CommandLineTools`。无需设备或真实 SSH。

```sh
python3 tests/ws_subscriptions/replay_project_baseline.py
python3 tests/ws_subscriptions/run_project_v3.py
python3 tests/ws_subscriptions/run_project_v3.py --browser
# 固定 SDK 独立目录 build 后：
V3_BUILD=/tmp/tianshan-project-v3-build/build python3 tests/ws_subscriptions/verify_project_assets.py
V3_BUILD=/tmp/tianshan-project-v3-build/build python3 tests/ws_subscriptions/measure_project_v3.py
# 构建输出消费者：
PROJECT_WEB_ROOT=/tmp/tianshan-project-v3-build/build/esp-idf/ts_webui/web_optimized \
V3_RESULTS=docs/repair/ws-project-v3/optimized-results \
python3 tests/ws_subscriptions/run_project_v3.py --browser
```

每个套件命令及退出码见 [host.json](results/host.json)、[browser.json](results/browser.json)。runner 保留每个子进程结果并对任一失败返回非零。构建/资源检查是独立入口，不伪装成主机测试。旧缺陷重放返回 0 表示**反例确认**：必须找到指定业务断言，编译失败不算红灯。H1 冻结测试验证同一个 mode/next-input 性质的反面；当前测试增加了后续实际 C 控制，而不是删除旧性质。

| 验收链 | 证据与关键断言 |
|---|---|
| C → 实际消息字节 → JS → 下次 C 请求 | results/cross-node-*.json、cross-browser-*.json；unsupported、非法尺寸、可恢复 resize 后合法输入调用原 driver；退出后是 terminal_input，再启动仍成功 |
| 前端真实入口 | Chrome 加载当前 index、语言包、terminal.js；最终 SDK payload 的 bytes 交给真实 ws.onmessage；xterm 渲染和网络替身，不冒充实机浏览器连接 |
| 真实失败和启动拒绝 | 同一桥验证 fatal write、EOF、PTY 启动失败、重复启动和其他页面四类控制；不误伤所有者，不自动重放 |
| 普通终端恢复 | 实际 terminal_stop 后下次 terminal_input 产生 Not a terminal session；实际 JS 生成 terminal_start 回到 C 并成功恢复。SSH 请求错误不触发这条路 |
| 交错及通知失败 | request_error 编码前插入输出+真实关闭；输出在唯一终态之前；编码失败走静态请求提示；直接发送失败不递归报错；输出 HTTPD 入队失败后无需新业务即可结算 |
| 原架构不变量 | operation/adapter/control 日志：G1/G2 原回调/票据，逐目标与引用账本；V1/V2 全 peer 和 exact ID、准备/密钥/连接/认证/提交取消；USER/TIMEOUT/MATCH 分离，观察失败不取消业务 |
| 背压、保护、公平性、事件排空 | host/reviewer/f1_f2 日志：对象/描述符/字节耗尽、目标部分失败、角色日志代次、各主题进展、被选中事件节点保活与排空 |
| 实际下层及停止 | shell_driver.log：真实 open/read/write/poll/run/close/client destroy；部分写、EOF/ERROR、partial init、持续 EAGAIN、两次上层清理；停止 timeout 后 channels 与 op 保留，retry 收尾时无新业务 |
| 相邻依赖 | runtime/certificate/prompts/browser/release 原入口重新执行；配置保存/部分失败/未知、证书和 HTTP 生命周期、导航与表单异步归属等既有行为保持 |
| 编译与运行资源 | build.log、assets.json：真实 www.bin 与验证目录重新生成的 SPIFFS 逐字节相同；JS 语法、gzip 解压一致、cache key 配套；optimized-results 运行构建后的中英文消费者 |

初次 Chrome 启动及 SDK 配置曾被沙箱权限阻挡；获准本地沙箱外运行后重做成功。开发中的夹具初始化、翻译断言和调度设置失败不计作产品反例，也不替代最终记录。

## 资源与性能边界

[target-resource.json](target-resource.json) 使用实际目标 compile_commands 的 GCC 及 `-fstack-usage`，分别编译冻结原实现和当前实现。Shell 对象 1076 → 1080 字节（+4），WebUI Shell context 20 → 20。open/poll/resize/统一控制的静态栈帧变化逐项列出；这是单函数静态数值，**不是调用链峰值或实机栈水位**。新增 helper 有自己的调用栈，不能只拿某个函数栈变小推断总栈降低。

baseline/current-allocation-metrics.json：相同正常创建、写入、不支持信号、关闭基线，项目分配数依次为 5/0/0/0，最终保留 0。它不计 libssh2 内部真实分配/硬件堆碎片；真实库所有权由边界模型另计 channel/session/socket 并在收尾归零。close 从无界网络等待改为本地收尾，时间上界属于代码与模拟时钟证据，不是设备耗时基准。

无新增常驻任务、队列、轮询或采样频率。TX 预算未改，当前 8 连接配置：6 个遥测消息槽/32 描述符，2 个普通消息槽/8 描述符，2 个结果槽/16 描述符，4 个保护转换槽+1 个倒计时槽/40 描述符；共 15 槽/96 描述符。保护字节保留 5×1024=5120；总预算 1 MiB，普通非保护不能借走该保留。单帧、legacy 与重试上限均沿用头文件。请求错误走 HTTPD 请求上下文直接发送，不新增结果预留或永久缓存。所有权、退订及终态释放仍由原 TX/operation 管理。

## 消费者反向独立自查

按与实施不同的顺序，从下一次用户输入/退出、提示符、恢复、socket 事件、Exec 消费者反查消息生产者及真正下层返回值，并重新查看累计 diff 中共享 API、注册、停止和构建链：

- 通用 error 在有效 SSH 中只提示请求；普通 terminal 恢复仍由原文义明确的旧契约触发。没有新增“匹配错误文案决定是否关闭”的旁路。
- 所有已接受 Shell 的真正终态来自原 op；四种页面控制仍经统一完整 peer 获取，不在错误处理后重新查询 current Shell。service owner 入口仍独立。
- Exec 原创建/取消/输出/业务结束和 timer 收尾未改；共享 client 的已有协作取消通过 runtime/control 回归。
- 唯一新增控制出口是 CLI 本地循环退出，引用和资源仍由同一 CLI 栈持有，未让回调 close/free 活跃句柄。
- 未发现本轮已知的高优先级未关闭回归；没有以缺失键数、测试数或编译结果替代上述行为链证据。

## 本地结论与未验收事项

在记录的 SDK、主机模型和 Chrome 边界内，H1 与本轮直接相关的下层/CLI 缺陷已有反例和当前正向证据，先前有效回归保持，满足本轮**本地交付门槛**；不是可自动合并/发布或实机验收结论。

- 未运行设备、真实 SSH/网络、双核抢占压力、实际 CPU/堆碎片/栈水位、TLS 现场握手和长期运行。需设备验收多页面与网络抖动下输入/退出/停止重试、远端部分写及 EOF、连续日志+遥测+保护通知负载、固件/www 更新后的实际缓存刷新。
- 发送成功不是浏览器 ACK；网络断开或关键通知有限重试耗尽时，不能承诺网页看见每条终态。需重新连接检查，绝不自动重放远端命令。请求错误本身发送失败仅有本地可观测日志，不递归产生通知。
- 协议仍没有同一连接内的 Shell 操作 ID，无法完全区分同连接跨操作代次的迟到控制；完整 peer 只解决连接实例归属。本轮未偷偷扩展协议。
- 支持同一构建的 firmware+www，更新后刷新并重连。旧固件仍把 H1 请求错误发成终态，新消费者无法凭旧帧安全猜测真实含义；混合版本不宣称兼容。
- Chrome 回放使用实际入口与消费者，但 xterm 渲染是边界替身。原套件另测资源加载失败/重试。项目原 xterm CDN 在线依赖仍在，本轮没有新增外部依赖，也不声称完整离线终端验收。

CI 后续可接入 host runner、Chrome runner 和固定 SDK artifact checks；本轮未修改 CI 工作流。
