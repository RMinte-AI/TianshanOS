# PR #42：F1/F2 本地修复与验证（2026-09-25）

本轮以 `bb0958c902d37ddc2b2deb36291b5439c0964160`、分支 `fix/v0.5.2-reliability-and-prompts` 的真实工作区为基线。F1/F2 在冻结生产源码上的业务反例成立，当前源码通过相同断言及交叉回归。现有 R1—R5 回归保留并通过。**这是本地验证结论，不是实机验收或发布许可。**

没有连接设备、刷机、重启、执行远端命令、修改保护动作/SSH 匹配规则/前端布局/外部消息类型与字段/版本/发布流程，也没有提交、推送、更新 PR、合并或发布。原未跟踪的 `output/`、`tmp/` 和其他修复记录保留。

## 基线和反例

- `baseline.json`：开始修复时的 HEAD、分支、git status、逐文件 SHA-256。
- `pre-fix-source.tar.gz`：当时相关 components、主机回归和核心入口的真实源码，不是事后另写的缺陷副本。
- `bash tests/ws_subscriptions/run_f1_f2.sh --baseline`：解包到临时目录并核对哈希，使用相同 F1/F2 断言对冻结生产 C 重放。该命令退出 0 表示确认旧行为失败；编译错误、ASan 或夹具清理错误不能充当反例。见 `before.txt`。
- `bash tests/ws_subscriptions/run_f1_f2.sh`：同样两条断言在当前源码通过，并执行下面的交叉场景。见 `after.txt`。
- 本轮没有收到另行提到的外部审查报告/反例包；依据用户给出的 F1/F2 要求，直接在上述源码建立反例。

| 缺陷 | 冻结源码证据 | 当前行为 |
|---|---|---|
| F1 普通 SSH 输出接受后静默丢失 | HTTPD 入队失败后仍维持正常流状态，没有错误帧 | 所属会话收到完成结算；Shell 停止该流并报告输出不完整；exec 结束网页观察并报告远端结果未确认，不自动终止或重放命令 |
| F2 日志挤掉保护通知 | 普通日志占满两个文本槽后，真实保护回调未发出 `power_event` | 真实保护回调先取得独立配额，再编码并提交；普通日志的对象、描述符和字节占用均不能消耗该配额 |

## 生产调用链与所有权

### F1：分类、结算和顺序

消息对象明确记录 `MSG_TOPIC / MSG_ORDINARY / MSG_RESULT / MSG_POWER / MSG_POWER_TICK` 类别。`ts_ws_transport_submit()` 按消息来源选择描述符池，完成回调的有无不再改变类别。SSH 输出即使带 `done`，仍使用普通流池，不占遥测或关键结果配额。

本地接受返回 `ESP_OK` 后，由 transport 对每个投递执行一次 `settle()`：成功、HTTPD 入队拒绝、实际发送失败、目标失效和适用的取消路径都在这里结算。同步拒绝表示没有接受，不调用 `done`，调用方自行结算它预登记的 pending。描述符持有消息、工作与发布引用；SDK 回调早于提交返回时，发布引用仍防止槽复用和重复释放。描述符的固定 peer（server/epoch/fd/connection）、FIFO order 和流代次一起限定身份。

- **Shell**：`ssh_send_output → transport_submit → publish/deliver → ssh_output_done`。提交前登记 pending；异步失败标记当前代次输出不完整，停止 Shell 输出并进入既有 error 终态。旧代次通知不改变新会话。终态先写入原有 reservation，所有先前输出结算后才提交；先请求 closed、后发现输出失败时，发出 error，不能在已知缺块后显示正常结束。新会话仍受旧 producer、pending 和终态发送状态约束。
- **exec**：真实 `ssh_exec_output_callback` 以及 started/match 通知都使用 `ssh_exec_stream_publish`。一次多目标提交先登记整个目标数，防止第一个同步完成回调提前结束整批。某目标失败时不重播已成功输出；使用原有 `ssh_exec_error` 明确说明输出不完整、远端结果未确认。一次性的 terminal claim 防止后台结束时再发正常 done。**这是结束网页观察，不是取消远端操作**；原收集、匹配、变量更新及业务取消规则不变。
- **exec 终态**：在首次终态请求时保存原全局接收者快照，等所有先前输出结算后才发出；不因重试或 fd 复用改投替代连接。新执行不得覆盖仍在等待结算的 reservation。
- **HTTPD 失败回调**：可以登记保留的错误终态，但结算结束前保持发送执行器 busy，之后唤醒 worker，避免失败回调递归调用 SDK 入队形成栈增长。
- **其余调用方**：遥测仍使用订阅校验及完成回调；普通日志、console 终端与普通全局业务事件保留原有有界 best-effort 行为。本轮不宣称所有既有流式发送都变成可靠投递。Shell connecting/connected 的就绪判定和原状态协议保留。

前端调用链也已静态核对：`terminal.js:handleSshStatus` 在 error 时退出 SSH 模式并显示 message；`app.js` 的 `ssh_exec_error` 按 session_id 结束当前观察并展示错误详情。没有改动布局或协议。既有界面错误标题仍是通用错误提示，新增详情明确写明“输出传递不完整、远端结果未确认”，不宣称远端命令确定失败。

### F2：准入、事件分类和失败处理

真实 `power_policy_event_handler` 先快照所有有效连接并预留完整目标描述符及消息，再编码原有固定字段。保护消息采用固定缓冲，不依赖普通日志正在使用的堆分配；格式化仅使用常量字符串与数字，非有限电压仍编码为 JSON null。没有目标时不构造消息。

状态转换、低电压、关机开始、受保护、恢复开始/完成等使用四个保护槽。countdown_tick/debug_tick 单独使用一个周期槽，不侵占转换容量。**本轮没有合并或覆盖事件**：已接受事件保持 FIFO；第五个并发转换或第二个未排空周期事件会明确拒绝并记录 admission 日志及 `power_rejected`，不承诺无限保存。适用的同步提交失败由调用方记录，异步目标失败记录 class/fd/result 及 `power_failed`。

只有 SDK 明确拒绝入队、尚未尝试发送的关键描述符会保留重试。最多 **300 次入队尝试**，间隔至少 100 ms；不是精确 30 秒的墙钟期限，实际调度可能更慢。达到上限会终态结算、记录失败并释放。实际 socket 发送失败或连接失效**不重发帧**，避免不明确发送导致重复。重试仅针对一个目标，不重播整个广播批次。

固定 FIFO、worker 唤醒及 next-wait 负责在没有新业务消息时推进；没有新增网络等待、无限循环或保护动作。正常容量持续释放的受控竞争测试中，日志、保护通知和八个周期主题均有进展。

### 停止与重试

沿用 R1/R4：先拒绝新业务、暂停遥测，再等待 producer 和已接受结果。新增 Shell pending/终态及 exec result-active 检查，防止在输出回调还可能提交错误终态时提前关闭 transport 准入。busy/timeout 保留 worker 和依赖，允许无新消息排空后再重试。power handler 仍同步注销并排空后才停止 transport；不把注销返回简单等同于所有回调已退出。

## 容量预算（默认 8 连接）

| 类别 | 消息对象 | 目标描述符 | payload 配额/上限 | 释放条件 |
|---|---:|---:|---|---|
| 遥测/原操作事件 | 6 | 32 | 每条 32 KiB；共享非保护字节预算 | 所有目标与 producer 引用释放 |
| 普通日志/SSH 输出/普通广播 | 2 | 8 | 单条小于 512 KiB；共享非保护预算 | 每目标结算后释放；不覆盖旧条目 |
| Shell/exec 关键终态 | 2 | 16 | Shell 4 KiB、exec 512 KiB 预留；共享非保护预算 | 前序输出结算后发布，或明确失败回收；目标排空后释放 |
| 保护转换 | 4 | 32 | 4 × 1024 B 固定缓冲及独立字节配额 | 所有目标结算后可复用 |
| 保护周期/调试事件 | 1 | 8 | 1024 B 固定缓冲及独立字节配额 | 所有目标结算后可复用 |

共 15 个消息对象、96 个目标描述符；传输 payload 总上限仍为 **1 MiB**。其中保护独占 5 KiB，其他类别合计最多 1 MiB − 5 KiB。这意味着非保护消息的可用预算比此前少 5 KiB，原“两个最大文本帧同时占满总预算”的测试已调整为新边界，未降低无预算不采集的断言。

保护缓冲 **5 KiB 常驻**，不是按需堆分配。目标编译器测得描述符表 8448 B（原 4480 B）、消息控制表 300 B（原 160 B）、exec 接收者快照 256 B；另有少量计数/原子状态。原输出 cJSON、业务缓存及 SDK 自身内存不计入 transport payload 上限，本轮不声称全固件总内存被此上限覆盖。

目标编译器单函数栈帧：power callback 736 B、exec output callback 528 B、run_batch 3360 B；详见 `resources.txt`。这些数字不是完整调用链峰值，也不是 ESP32 实测栈水位。worker 数量、栈配置和硬件采样频率未变。

## 本轮实际验证

| 层级 | 命令 | 结果和证据 |
|---|---|---|
| 冻结生产反例 | `bash tests/ws_subscriptions/run_f1_f2.sh --baseline` | 两条旧行为失败已确认；`before.txt` |
| 当前生产 C 与调用方 | `bash tests/ws_subscriptions/run_f1_f2.sh` | 相同断言及交叉场景通过；`after.txt` |
| 既有 WS | `bash tests/ws_subscriptions/run_host.sh` | 通过；`host.txt` |
| R1—R5 | `bash tests/ws_subscriptions/run_reviewer.sh` | 通过；`reviewer.txt` |
| 运行时 | `bash tests/runtime/run_all.sh` | 本轮重跑通过；`runtime.txt` |
| 证书/HTTPS | `bash tests/certificate/run_host.sh` | 本轮重跑通过；`certificate.txt`；其中 REPRODUCED 是该套件自己的固定旧版反例 |
| WebUI Node | `npm test --prefix tests/prompts` | 本轮重跑通过；`prompts.txt` |
| Chrome 浏览器 | `npm run test:browser --prefix tests/prompts` | 本轮中英文回归通过；`browser.txt`；沙箱内启动失败，获工具批准后用临时 Chrome 配置在沙箱外重跑；后端模拟 |
| ESP32-S3 编译/链接 | 固定 ESP-IDF v5.5.2：`idf.py -B /tmp/tianshan-ws-build/build -D SDKCONFIG=/tmp/tianshan-ws-build/sdkconfig build` | 本轮通过，应用 `0x213f70`，分区余 31%；`build.txt` |
| 静态检查 | `git diff --check`、完整调用链复核 | 通过；`diff-check.txt`。本轮 diff 文件与新建测试另外列入 `changes.patch` |

构建前设置 `DEVELOPER_DIR=/Library/Developer/CommandLineTools`、使用本地 idf5.5 Python 环境并 source SDK `export.sh`。构建日志中 flash 命令是 SDK 的提示，**没有执行**。首次目标编译发现事件 ID 日志格式类型不符，已改为显式 long 后重建通过；未将失败构建当作验证成功。

交叉测试明确覆盖：首次/稍后 HTTPD 入队失败、closed 先提出后发现丢块、旧 peer/fd 复用、新会话准入隔离与迟到旧代次通知、回调早于提交返回、实际 socket 失败、逐目标结算次数、exec 部分失败无重复、持续 exec 的及时错误提示、三类容量竞争、保护容量超载与事件 FIFO、300 次拒绝上限、busy/timeout 后无新消息排空、遥测取消不误取消普通输出，以及日志/保护/八主题持续竞争。

测试编译完整生产 transport/manager，并从当前文件提取完整 Shell、exec 输出回调/发送/终态、保护事件函数体；SDK cJSON 为真实库。SDK 网络、设备、时钟和 RTOS 由边界替身控制。代次测试包含实际 fd 重用/准入过程，并额外注入迟到旧回调以验证 guard；不是 ESP32 真实线程调度证明。

## 独立自查与残余验收

自查范围覆盖生产者 → 准入/预留 → 本地 FIFO → SDK 入队 → 实际 send 返回 → 完成回调 → 流状态/终态 → 上层停止及重试，也核对真实前端 error 消费路径。没有仅凭辅助函数、数量或 CI 判定通过；本轮未运行或更新 CI。

本地证据关闭的是 F1 的接受后无反馈路径和 F2 的日志争抢保护配额路径。既有兼容性回归通过。仍需另行授权的设备验收包括：

1. 真实 HTTPD 控制队列拥塞、慢 TCP/RST/LRU、多标签页与 fd 重用下的最终可见提示和无重复接收。
2. ESP32 双核并发中 producer 收尾、最后输出回调与停止重试交错，验证堆/描述符回收及完整调用链栈水位。
3. 保护状态转换密集出现时的 FIFO、周期槽拒绝和失败日志；核对原硬件保护动作及时间顺序未受网络负载影响。
4. 长时间混合日志、SSH 输出和八主题竞争时的内存、延迟和实际进展；验证 5 KiB 常驻缓冲及减少的非保护预算是否满足设备负载。
5. SSH 网页观察因输出失败结束后，真实远端任务仍按原业务规则运行/结束；不能把网页 error 当作远端取消或执行失败的确认。

已经向 SDK 提交不等于浏览器确认收到。socket 失败、断线、持续超过有限容量或持续拒绝入队时仍可能无法送达，系统给出失败计数/日志并回收，不承诺无损无限队列。`/ws` 既有独立认证/主题授权缺口亦未在本轮修复。

`final-manifest.json` 记录最终源码、证据和本地构建哈希；`changes.patch` 是相对本轮基线的代码、测试差异。后续 CI 接入建议见 `tests/ws_subscriptions/README.md`，本轮没有改动工作流。
