# 第二轮 Reviewer 复核 — 2026-09-25

结论：需修复后交付。新增 2 项确认问题，合并上一轮为 5 项。未修改仓库文件。本轮并非宣布不存在其他问题。

HEAD 仍为 35238b4645ae5e9e0e99555d4fb64080d945565c；当前修复仍未提交。第一轮 R1 停止失败后发送停滞、R2 SSH 错误终态丢失、R3 日志角色失效未撤销，继续保留。第一轮记录：/tmp/ts-review-ws/review.md。

## R4 / P1：新增停止失败契约没有贯穿服务和核心调用链

源码：components/ts_core/ts_service/src/ts_service.c:383、147；main/ts_core_init.c:175（同函数还忽略 service_deinit 返回）；main/ts_services.c:950 为 WebUI 真实服务停止入口。

WebUI 和事件总线现在可能返回 busy/timeout 并保留活跃对象，这是正确的底层契约。但 ts_service_stop_all 忽略单项失败，继续停止低阶段依赖，最后返回成功；ts_service_deinit 不检查 stop_all 结果，继续释放服务登记表和锁；ts_core_deinit 忽略事件排空超时，继续销毁日志、配置并清除核心初始化标志。结果是“底层仍活跃，上层已宣布关闭/释放”。本轮修改的安全停止只落实了下层，未覆盖这些现有调用者。

证据 A：生产 stop_service_internal / ts_service_stop / ts_service_stop_all / ts_service_deinit 原函数体，外围 RTOS 和服务回调模拟。WebUI 停止回调返回 TIMEOUT，单项状态正确保持 RUNNING，但 stop_all 返回 OK 并执行低阶段服务停止；deinit 再次遇到失败仍释放登记表并返回 OK。

证据 B：完整生产 ts_event.c 与实际 ts_core_deinit 函数体。pthread 屏障保留一个在途同步事件回调，虚拟时钟推进到 6 秒，真实事件 deinit 超时。真实 core_deinit 仍返回 OK，已调用模拟的日志/配置销毁，核心标志 false，而事件系统仍 initialized 且回调仍在途。测试随后正常释放屏障并排空，没有真实设备操作。

性质：吞错代码原已存在；这是本次引入可重试停止后未迁移完整调用链的缺口，不应宣称这些行是本次新增代码。并不意味着用户每次关机都触发；条件是停止/排空失败时走聚合停止或核心反初始化入口。

最小修复：向上传递失败，保留重试所需的服务状态与控制资源，遇到仍依赖下层资源的活跃服务时不要继续销毁其依赖；核心不应在事件排空失败后销毁日志/配置或清除 initialized。保持原有停机顺序，不扩展为框架改造。

回归门槛：将 WebUI busy/timeout 注入真实聚合停止链；事件回调未退出时触发核心 deinit；确认返回失败、资源保留、依赖仍在，以及回调结束后重试成功。

## R5 / P2：消息预算与固定调度顺序导致后部主题饥饿

源码：components/ts_webui/src/ts_ws_subscriptions.c:190、158–179；components/ts_webui/src/ts_ws_transport.c:133–134。

周期消息只有 6 个槽；run_batch 只把 message_capacity 当作有/无开关，然后按描述符数预留到期目标；collect_batch 固定按 8 个主题的顺序采集/组包。若同批包含 8 个独立周期主题，且 HTTPD 在该批组包结束后才排空，前 6 个占满消息槽，后 2 个分配失败。它们已经付出了 API 采集成本，却被当作失败再延后完整 interval；下一批仍按相同顺序竞争，缺乏公平进展保证。

证据：完整生产 manager + transport C；单连接订阅所有 8 个周期主题，interval 均为合法的 10000 ms；控制时钟与调度，使同批到期、批末排空。100 轮中前 6 个主题各序列化 100 次，fan.status/service.list 各序列化 0 次，但对应 API 各调用 100 次。没有持续堆分配失败、无穷输入或无限慢连接。

证据边界：这是合法受控调度下的确定性反例，不表示真实板上每种调度都必然连续丢 100 轮；实际 HTTPD 若在组包中途释放槽可能改变结果。反例证明当前实现没有公平进展保证。

最小修复：采集前同时按独立消息数、描述符预算选择本批主题，并让未获容量的主题公平获得后续机会；容量不足不要伪装成一次已尝试后的完整周期失败。优先复用现有 worker、表和通知，不靠单纯扩容遮蔽固定顺序问题。

回归门槛：8 个合法主题同批到期，消息容量小于主题数；慢消费者与不同 interval 混合；确认每个有效主题最终有进展，无消息预算时不重复采集必然丢弃的数据。

## 本轮执行和未执行

- /tmp/ts-review-ws-round2/core_stop：ASan/UBSan；确认真实事件超时未传递到核心关闭结果。
- /tmp/ts-review-ws-round2/service_stop：ASan/UBSan；确认聚合停止/反初始化忽略失败。
- /tmp/ts-review-ws-round2/starvation：ASan/UBSan；确认 100 轮主题饥饿。
- 三者退出 0 表示反例成立，不表示产品验收通过。生产逻辑来自当前文件 include 或原函数体提取；依赖和时间/线程屏障受控。
- git diff --check：退出 0。
- 未修改生产代码，未重复固件构建或浏览器测试；没有设备、远端 SSH、刷机、重启、commit/push/PR 操作。
- 事件节点选中后注销、自注销、同步排空和超时保活路径继续检查，未确认独立的新节点回收缺陷。完整全局事件总线销毁与外部生产者并发准入仍不属于已证明安全的范围；不要把 handler 排空测试等同于全局销毁并发验收。
