# PR #42 增量提交检查（2026-09-25）

结论：可以推送。用户已授权将本轮新修改提交到 Fork 并更新原 PR；不合并、不打标签、不发布、不操作设备。

本轮提交基于 `35238b4645ae5e9e0e99555d4fb64080d945565c`，包含 WebSocket 按需采集/定向发送及 R1—R5 修复、相关事件/HTTP/服务/核心停止链、生产 C 与浏览器回归、冻结基线和修复证据，以及更新的 v0.5.2 中文发布说明。原 `output/`、`tmp/` 和 `docs/runtime-repair/evidence/reviewer-fixes/diff-check.txt` 不进入本次提交并保留原状。

## 审查依据

- 查看最终 manager/transport 的资源所有权、连接身份、发送前校验、关键结果预留和公平调度，及事件注销、HTTP/WS/服务/核心停止的错误传播。
- 确认普通发送仍有界；关键结果入队重试不会重放 SSH 命令；外部设备、完整事件生产者和网络送达不作无条件保证。
- 再跑 `bash tests/ws_subscriptions/run_reviewer.sh` 与 `bash tests/ws_subscriptions/run_host.sh` 成功；日志保存在本目录 `pr-update-reviewer.txt`、`pr-update-host.txt`。
- 再跑 `python3 tests/release/test_release.py` 成功；生产源码、当前测试及发布文档的暂存差异检查成功。完整暂存 `git diff --cached --check` 会报告两份历史 `.patch` 及两份冻结旧函数夹具原有的尾随空格；为保留补丁上下文和复现证据不改写这些文件，检查时明确排除这四个证据文件。版本与发布工作流没有修改。
- 当前源码逐文件 SHA-256 与 reviewer-fixes/final-manifest.json 一致，隔离目录的构建二进制哈希也一致，因此复用该次 ESP-IDF 构建及既有运行时/证书/浏览器证据，不将旧 PR 的 CI 绿灯用于新提交。

## 边界

主机测试依赖实际生产 C、模拟 SDK/RTOS/设备响应和 pthread 屏障；浏览器使用模拟服务。没有真实 ESP32 双核并发、慢连接/RST/LRU、长期堆和栈水位验收。现有 `/ws` 独立认证/主题授权缺口仍未解决。详细资源取舍及实机清单见 reviewer-fixes/README.md。

修复记录中的“未提交、未推送”是此前交付时点的事实；本文件记录后续授权下的提交前审查，最终远端 SHA 和 CI 以 PR #42 为准。
