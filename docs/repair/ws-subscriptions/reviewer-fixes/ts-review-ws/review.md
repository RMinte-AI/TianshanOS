# WebSocket 本地修改独立复核 — 2026-09-25

结论：需修复后交付。前一轮“已完成”结论应撤回；当前确认 2 个 P1 和 1 个 P2，不表示不存在其他缺陷。

基线 HEAD：35238b4645ae5e9e0e99555d4fb64080d945565c。审查范围为当前未提交 WebSocket 修复及其事件总线、HTTP 生命周期、测试和交付记录；不是重审整个历史版本的所有代码。未修改仓库文件；反例仅保存在 /tmp/ts-review-ws。

## R1 / P1：停止失败后发送队列失去推进者

位置：components/ts_webui/src/ts_webui_ws.c:1149–1162；ts_ws_transport.c:247–254。

真实 ws_stop 先 deinit manager（worker 已退出），再检查 SSH exec。执行仍在进行时返回 INVALID_STATE，但保留 HTTPD 与后台执行。发送完成仅唤醒已不存在的 worker；SDK 不自动执行 PREPARED 描述符。没有后续提交/再次停止时，已接受的剩余消息可以永久滞留。停止返回超时的部分路径也有同类问题。

反例：抽取当前实际 ws_stop 函数体，联合完整生产 transport/manager C；模拟正在执行 SSH，停止返回 busy 后广播给两个连接。执行真实 SDK work 回调一次后，只有一个连接收到，另一个描述符仍待发，SDK 队列为空且 worker 为 NULL。测试没有用 pump() 自动替生产代码补 flush。

最低修复：拆开“关闭遥测准入”和“销毁发送推进者”的时序；所有可重试失败阶段，保留任务结果所需的发送推进能力。检查活动执行应在破坏其依赖之前完成，并继续考虑创建入口与停止的竞争。不能靠丢弃任务结果或要求用户重试停止才能恢复消息推进。

回归：真实停止 + 活动执行 + 多接收者 + 最后一条完成消息，停止失败后不再有新消息，仍应完成发送；覆盖 barrier 超时、轮询任务超时及重试。

## R2 / P1：SSH 错误终态被静默丢弃，最后收到的仍是 connected

位置：components/ts_webui/src/ts_webui_ws.c:223–227、412–420；ts_ws_transport.c:129–140。

非遥测消息只有两个共享 message 槽。handle_ssh_connect 在同一个 HTTPD 请求中依次发送 connecting、connected；若创建轮询任务失败，还发送 error。前两条现在改为排队，HTTPD 当前请求没有返回前无法执行其队列回调；第三条分配必然失败。ssh_send_status 忽略分配/提交失败，错误没有发出，也没有记录拒绝。任务实际上未运行、资源已清理，客户端却最后看到 connected。其他日志占槽还会降低可用容量。

反例：执行从当前源码抽取的真实 ssh_send_status，联合完整生产 transport，模拟上述同一 HTTPD 请求内三个状态调用。只收到两帧，最后帧是 {"type":"ssh_status","status":"connected","message":"SSH shell ready"}。不依赖真实设备、网络拥塞或远端连接。

最低修复：在完整启动成功前不发 connected；复用 HTTPD 所有者上下文可安全同步发送的结构，或为关键终态设置独立有界保留和可观测的失败处理。不能仅增大两个槽的常量，也不能让日志/普通输出挤掉错误后继续显示成功。

回归：实际连接处理器的任务创建失败；日志占槽 + SSH 成功/失败/远端关闭；输出满载后的终态必须明确，拒绝可观察。本反例覆盖真实发送函数和同请求调用顺序，尚未模拟完整 SSH 连接处理器的所有依赖。

## R3 / P2：连接角色变化没有撤销新 transport 的日志资格

位置：components/ts_webui/src/ts_webui_ws.c:648、363、1255；ts_ws_transport.c:378–400。

旧日志发送按 s_clients.type == LOG 判断。新发送路径使用独立 s_log_levels；terminal_start / ssh_connect 只更改旧表中的 type，没有清除 transport 中的日志资格，也未同步日志流开关。同一连接从日志切到终端/SSH 后，已排队和后来新增的日志仍可发送，导致接收集合变化和不必要的设备工作。

反例：执行真实 start_terminal_session，联合完整生产 transport。旧表已变成 TERMINAL，但先前排队和此后提交的 log 都通过最终 log_allowed 校验并送达。外围 console 和请求发送接口模拟；没有复制终端角色切换逻辑。

最低修复：在现有角色切换处统一维护旧客户端类型、transport 日志资格及全局日志回调开关；最终发送检查使用同一有效状态。补日志→终端、日志→SSH、角色切换前已排队日志的交叉测试。

## 验证记录与边界

- bash tests/ws_subscriptions/run_host.sh：本轮重新运行，退出 0；manager/event/HTTP生命周期/WS init-stop 主机测试通过。
- git diff --check：本轮退出 0。
- bash /tmp/ts-review-ws/run.sh：ASan/UBSan 编译运行真实生产 C 与抽取的真实函数体，三个缺陷断言成立，退出 0。这里退出 0 表示成功证明缺陷，绝不表示修复验收通过。
- 现有 test_init 的停止依赖由 mock 分隔，test_manager 的 pump 主动 flush，所以没有覆盖 R1 的实际组合。现有测试未覆盖 R2 同一请求内的三状态路径或 R3 角色交叉。这解释了“既有回归通过”和“新增反例失败语义”可同时成立。
- 本轮没有修改生产代码、重新构建固件或重跑浏览器；前一轮构建记录不能证明上述语义正确。
- 没有连接设备、远端 SSH、刷机、重启、提交、推送或 PR 操作。RTOS 双核调度、真实慢网络、栈水位、长期运行及硬件告警仍需设备验收。
