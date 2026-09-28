> 后续 Reviewer 确认 R1—R5 后的修复、冻结基线与验收边界，以 [reviewer-fixes/README.md](reviewer-fixes/README.md) 为准；下文保留首次实施记录。

# WebSocket 设备端开销与生命周期修复记录

日期：2026-09-24。基线：`35238b4645ae5e9e0e99555d4fb64080d945565c`；分支 `fix/v0.5.2-reliability-and-prompts`；SDK：本地 ESP-IDF **v5.5.2**。本记录针对当前未提交 diff，不是设备验收或发布许可。

原有未跟踪的 `output/`、`tmp/`、`docs/runtime-repair/evidence/reviewer-fixes/diff-check.txt` 保留。没有回退、提交、推送、更新 PR、修改版本/发布工作流、连接设备、刷机、重启或执行远端命令。附带文档作为设计依据；按当前代码作出的调整列在下文。

## 处理状态

| 问题 | 结果及实际修改 |
|---|---|
| P1 无订阅仍采集 | 移除三个展示用 esp_timer。单一 `ws_telemetry` 任务按截止时间/通知等待；没有周期需求时不轮询。最后订阅失效后不再启动其 API 读取；当前不可中断 API 允许收尾。 |
| P2 未到期仍采集、组包 | 在 API 前选出到期、无 pending、有容量的目标；空集合不分配 JSON。dashboard 与单主题按同批指标并集读取。间隔基于实际发送完成推进，失败单独退避。 |
| P3 主题循环全体广播 | 主题只提交身份固定的目标。每个主题每批序列化一次；每个目标一次。真正全局告警仍取全部有效连接快照。 |
| D1 真实停止缺失 | WebUI stop/deinit、HTTP 独立 stop/deinit、启动失败回滚接入停止 hook；失败向上传递，保留句柄和 STOPPING，允许重试。 |
| D2 API 结果泄漏 | 每次 API 使用独立结果对象；转移 data 后置空，所有分支调用 `ts_api_result_free()`，包括业务失败和错误 message。 |
| D3 事件节点提前释放 | 有上限的处理器快照持有节点引用；注销先退休节点，不再开始新回调。同步注销等待 in-flight，超时保留句柄供重试。自注销不等待自己。事件总线 deinit 改为协作等待，不强删正在执行回调的任务。 |
| D4 共享 handler / system.info | 统一调度八个周期主题。三个事件 handler 随管理器注册/排空，不再由某个系统主题单独注销共享 handler；system.info 可独立到期采集。 |
| 新增 A1：SDK cJSON 引用辅助函数失败泄漏 | 分配失败扫描发现 `cJSON_AddItemReferenceToObject` 的临时引用在键分配失败时丢失。改为显式持有引用节点、常量键添加及失败回收；不复制一棵长期存活的数据树。 |
| 新增 A2：HTTPD 控制邮箱容量 | 当前 `CONFIG_LWIP_UDP_RECVMBOX_SIZE=6`，queue_work 使用本地 UDP，不能把几十个描述符直接塞入 SDK 邮箱。模块固定 FIFO 保存待发项，每次仅一个 TX work 在 HTTPD 中；完成只唤醒 worker，由 worker 推进下一条。停止屏障另占最多一个 work。未更改 SDK 或全局配置。 |
| 新增 A3：排空 TX 不代表 HTTP 请求结束 | 单独的 HTTPD 请求边界屏障；确认当前请求返回后才清理 SSH 资源。它不代替实际 outstanding 计数。 |
| 新增 A4：既有后台发送者越过服务器停止 | 全局、日志、SSH 输出接入受控发送归属。SSH 输出保存原连接身份；轮询任务退出前不释放会话；SSH 执行创建入口增加停止准入计数。进行中的执行不重放、不强制取消，停止返回 busy，正常结束后可重试。 |

另外核对了 HTTPD URI 的所有权：SDK 不释放应用注册时分配的 route `user_ctx`。现在由 HTTP 包装器登记其副本，只在 **httpd_stop 成功以后**释放；失败保留。此项是重复启停回归的 I08/T29 依赖，不改变路由、认证或业务处理。

## 文件及实现边界

- `components/ts_webui/src/ts_ws_subscriptions.c`：需求调度、订阅 revision / delivery、独立间隔、指标共享、事件 FIFO、结果释放、稳定控制状态、协作停止。
- 新增 `components/ts_webui/src/ts_ws_transport.c` / `include/ts_ws_transport.h`：完整 peer 身份、session free hook、有界消息/发送池、HTTPD 上下文最终验证、固定 FIFO、控制邮箱限流、失败 shutdown、统计和停止屏障。新增文件仅服务 WebUI，不是通用消息框架。
- `ts_webui_ws.c`：握手登记、所有真实断线入口、旧会话拒绝、日志过滤、后台发送适配、初始化失败传播。停止时不释放仍在使用的 SSH 对象。
- `ts_webui.c`、`ts_http_server.c/.h`：实际拥有者停止链、错误传播、HTTPD 自等待拒绝、停止中拒绝再次启动。
- `ts_event.c/.h`：节点保活、退休、同步 drain、自注销/并发注销、等待者引用；未更改优先级、事件队列投递规则和其他模块的订阅集合。
- `tests/ws_subscriptions/`、`tests/prompts/ownership.browser.test.cjs`：生产 C 回归与实际页面类兼容性测试。

## 按当前代码调整的设计

1. 当前只有 HTTP `/ws`，HTTPS 没有独立 WebSocket 注册。传输管理器明确只接纳一个 HTTPD 实例；第二个实例返回错误，未新增 HTTPS WS。仍使用 server epoch 隔离相同 handle 地址重用。
2. 当前 `/ws` 没有独立认证/主题授权检查。没有把 active 或订阅当作鉴权，也没有宣称补好了认证。新增目标检查收窄了原来的广播范围；会话有效性、订阅身份和日志过滤在执行时重验。**认证/授权撤销的端到端安全验收不适用当前实现，仍是独立安全缺口。**
3. session context 为空时才安装 free hook；已有 context 时拒绝接入，不覆盖 TLS/认证上下文。正常 close、RST、LRU、HTTPD stop 都归于 SDK session free 流程。发送失败在 HTTPD 所有者上下文对已验证的 fd 执行 shutdown，实际 close 仍归 SDK。
4. 原来 add_client 的“探测/抢占”只移除应用表项，没有结束真实会话；现改为容量不足明确拒绝登记，由 SDK 结束失败请求。原唯一调用位置是普通事件握手；其中按 terminal/SSH 类型抢占的分支在该入口并不会触发。
5. 周期 interval 最小为 dashboard/CPU 1 秒，其余 5 秒；更长 interval 真正减少读取。当前 dashboard 页面请求 1000 ms，与之兼容。新订阅首帧在一个有效间隔内；相同参数重复订阅不重置进度。事件默认 interval 0 的既有语义保留。
6. device/OTA/config 操作事件用有界 FIFO 保留顺序，不做 latest-only；满载/过大/分配失败会计数或返回提交失败，不伪造送达，也不改变原 HTTP 操作结果。
7. 全局/流式发送使用独立描述符配额；不覆盖旧日志/SSH 数据。有限容量仍可能拒绝过载流，拒绝计数可见；没有引入端到端可靠投递协议。
8. 已有终端缓冲及互斥锁保留为可重用单例，不在可能存在历史 console callback 时销毁；不会每次重启新建一套。未把本轮适配称为全部终端/SSH/日志代码的完整审计。

## 锁、所有权和停止

| 对象 | 所有者 / 同步 | 回收条件 |
|---|---|---|
| manager 控制状态 | 永久静态短临界区；入口计数 | 不释放临界区；实例状态只在 producer、worker、delivery 全部排空后回到 OFF |
| 订阅 / revision / pending | manager 锁内改值；worker 仅复制值快照 | 旧完成必须同时匹配 revision 和 delivery；不能回写新订阅 |
| peer | transport 短锁；HTTPD 创建、删除、最终发送 | server handle + epoch + fd + connection 同时匹配；旧 hook 不清理新会话 |
| API data | 本轮 worker | data 转移后原指针置空；结果和 error message 每次释放 |
| message | 固定槽引用计数；不可变字符串共享 | 最后拥有者释放；实际 free 完成前仍保留槽和字节配额 |
| TX descriptor | 固定池，PREPARED/QUEUED/EXECUTING/DONE/CANCELED | 准备/发布/执行各自的引用明确；回调早于 queue_work 返回也不会复用或二次释放 |
| handler node | 总线互斥锁；快照、调用、等待者 pin | 已退休且所有引用归零；超时 drain 保留供重试 |
| handler user_data | manager / 调用方 | 同步 unregister 成功或 NOT_FOUND 后才允许释放；本模块使用固定 topic 值上下文 |

没有在订阅锁/连接锁内调用 API、JSON、网络、queue_work 或等待 drain。HTTPD 最终校验与会话创建/删除在同一所有者上下文；已经进入传输的帧不能撤回。

停止顺序：关闭 WebUI 新准入 → manager STOPPING/失效订阅 → 同步排空自己的事件 handler → worker 收尾/取消未提交主题任务/等待真实 delivery → HTTPD 请求边界 → 排空 power handler、检查 SSH 创建/执行和协作轮询退出 → transport 停止准入、取消未提交项、等待已提交工作 → 真正 httpd_stop → 释放实例身份。任一步失败保留安全状态；不会超时强制 free。

全事件总线销毁仍依赖系统服务拥有者先停止外部事件发布者。本轮验证的是节点/回调/drain，不声称审计了所有第三方生产者与整个系统关机顺序。

## 测试与证据

[基线](evidence/baseline.json)、[旧版红灯](evidence/baseline-red.txt)、[生产 C 主机回归](evidence/host.txt)、[额外分配失败发现](evidence/allocation-finding.txt)。

| 命令 | 结果 / 证据边界 |
|---|---|
| `./tests/ws_subscriptions/run_baseline.sh` | 脚本退出 0 表示确认了预期失败：T02 无订阅仍 7 次 API；T05 未到期仍 7 次；T07 两次全体广播。原 dispatcher 的自注销触发 ASan heap-use-after-free。不是编译错误冒充红灯。 |
| `./tests/ws_subscriptions/run_host.sh` | ASan/UBSan；完整生产 manager、transport、SDK cJSON、event C；实际停止及初始化函数体。测试边界和方式见 tests README。 |
| 事件测试 `-fsanitize=thread` | 当前 pthread 事件测试通过；不代表模拟 FreeRTOS 调度器或全固件通过 TSan。见 [记录](evidence/tsan.txt)。 |
| `./tests/runtime/run_all.sh` | 原有运行时回归通过，[记录](evidence/runtime.txt)。 |
| `./tests/certificate/run_host.sh` | 原有证书/HTTPS/停止回归通过，[记录](evidence/certificate.txt)。其中 REPRODUCED 是该测试的旧版反例，不是当前失败。 |
| `npm ci --prefix tests/prompts --cache /tmp/tianshan-pr-npm-cache`、`npm test --prefix tests/prompts` | 38 项通过；未修改依赖锁文件，[记录](evidence/prompts.txt)。 |
| `npm run test:browser --prefix tests/prompts` | 本地 Chrome，38 项通过；新增中英文双页面、不同 interval、订阅/取消确认及取消后不回调；保留 R1—R6 切页回归，[记录](evidence/browser.txt)。WebSocket/设备响应模拟，不是实板网络。 |
| `git diff --check` | 通过。 |
| 独立 ESP-IDF 编译 | 见 [构建与资源记录](evidence/build.txt)。原 `build/*.bin` 未覆盖。 |

### 交接矩阵映射

“主机通过”只表示对应生产函数在可控边界下的断言成立；设备项没有执行。

| ID | 结果 |
|---|---|
| T01—T03 | 无需求/取消后计数停止、恢复、重订交错；主机通过。 |
| T04 | 发送失败、session free、幂等身份清理通过；实际 RST/LRU/close 的 SDK 接入经源码核对，实网未执行。 |
| T05—T10 | 到期前零读取、独立间隔、定向、不重复、参数代次、数值范围；主机通过。 |
| T11—T13 | 独立 system.info、共享批次、API 中取消跳过剩余指标；主机通过。 |
| T14—T16 | fd/handle 重用、旧完成不改新 pending；主机通过。第二同时服务器按当前单所有者契约拒绝。 |
| T17 | 分配/queue 失败、回调先于发布返回、单 SDK work、引用及退避；主机通过。实际 UDP 丢包未伪称已测试。 |
| T18 | 固定描述符/总字节、保留全局配额、最大帧边界、32 服务转义名称大小压力；主机通过。真实慢网络、完整设备最大组合负载需实机。 |
| T19—T22 | 延迟完成不追赶、统计层次、业务失败释放、179 个分配失败位置扫描与连续轮次回到分配基线；主机通过。 |
| T23—T25 | 连续操作结果顺序、借用数据复制、事件 FIFO 满载、非 NUL 事件、事件间隔、全局接收集合；主机通过。 |
| T26 | manager task/部分注册回滚；实际 WS 初始化的 server/transport/manager/两个锁/缓冲/URI/power handler 失败返回；主机通过。完整硬件驱动启动失败注入未执行。 |
| T27—T30 | API/queued work 中停止，HTTPD 屏障超时/重试，真实 stop 错误传播、重复生命周期、自等待拒绝；主机通过。 |
| T31 | 当前没有独立 WS 鉴权，不能宣称通过授权撤销测试；身份/未订阅隔离已测。 |
| T32 | pthread 屏障证明节点选中后注销、in-flight drain、自注销、超时重试、其他 handler 保留；ASan/UBSan/TSan 通过。 |
| T33—T34 | 等待边界通知、单调时间/向上取 tick/大值封顶及非零等待；主机通过，未测目标 RTOS 抢占和墙钟真实切换。 |

额外执行四个边界 × 五类变更：快照后、API 中、SDK 工作待执行、send 返回/完成写回前，分别取消、重订、断线、fd 重用、停止并拒绝未排空重启。每次排空后重新初始化成功。不是仅检查“没有崩溃”，也断言目标帧数、API 次数、pending/时间归属和活跃分配归零。

## 资源预算与设备验收

- 单 worker：8192 字节栈、优先级 2、无绑核；SDK 静态 TCB 对应尺寸由目标编译测得 **352 字节**。实际栈水位字段已保留，未运行设备，因此没有实机水位数值。
- 32 个订阅；4 个操作事件槽，事件消息计入同一有界字符串池。每订阅最多一条周期在途投递。
- 当前 8 连接配置：32 个主题 TX 描述符 + 8 个全局/流式描述符；独立配额。内部 FIFO 不直接对应 SDK 邮箱项；每次最多 1 个 TX work + 1 个停止屏障进入 SDK。
- 8 个消息槽，6 个供主题/事件、2 个供全局/流式，配额互不占用；遥测单帧上限 32768 字节，旧流式/全局文本单帧须小于 512 KiB；传输池拥有的消息字符串合计硬上限 1 MiB。不是预先常驻分配 1 MiB。过大明确拒绝，不截断。
- 事件 FIFO 自身至多 4 × 32768 = 128 KiB 字符串，包含在总预算内。主题 JSON 用预分配上限缓冲区输出，不是先无限分配再量长度。
- 当前 32 服务、31 字节全转义名称的尺寸压力样本为 9991 字节；512 KiB 旧流文本边界另测。这不是全部传感器真实最大组合负载的实板证明。
- 静态对象尺寸由目标 object/nm 记录在 [资源记录](evidence/resources.txt)。临时 API cJSON / 系统 CPU API 的任务快照仍按既有 API 实现分配，以及既有全局/流式生产者的临时 JSON/string，不包含在 TX 字符串预算中。
- 不预先宣称 CPU/功耗下降百分比或总 RAM 一定减少。显示采集次数减少已在主机计数验证；CPU、耗时分位、内部 RAM/PSRAM 峰值和长期堆变化必须实测。

需要另行授权的实机清单：无人订阅；一个 dashboard；10 秒间隔；多个快慢标签页与未订阅页；慢连接/断线/RST/LRU；反复切页重连与 fd 重用；最大服务/风扇/网络配置和长输出；事件连续完成及过载；有 SSH/日志输出时的 WebUI/HTTP 独立停止、失败重试及重新启动；观察栈水位、堆回归、发送耗时、计数和原全局电压告警。未执行刷机、这些操作或任何硬件保护测试。
