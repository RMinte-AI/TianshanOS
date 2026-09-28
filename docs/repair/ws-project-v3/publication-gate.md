# v3 提交前独立审查

结论：可以推送，范围限已验证的本地修改。用户在实施交付后明确授权更新 PR #42。

当前基线与 PR head 同为 568b871，原 Fork 分支 fix/v0.5.2-reliability-and-prompts。以追加提交更新，不重写历史，不合并或发布。

审查依据：最终 runtime diff、实际下层 EOF/释放、四控制入口、请求/终态分类、JS 消费及后续输入、冻结反例、完整验证记录。843 项最终输入哈希无变化；v2 controller、TX 与采集预算保持。未发现本轮明确的阻断问题。

本次提交前重新执行：
- run_shell_driver.sh：真实驱动 EOF、部分写、EAGAIN、CLI 原输入回调、资源释放和停止重试通过（publication-shell.txt）。
- node tests/prompts/project-cross.cjs：真实最终 C 字节经生产 JS，再进入下一次 C 请求通过（publication-cross.txt）。
- run_control.sh：原 peer/ID 归属、取消各阶段、原因区分和停止交错通过（publication-control.txt）。

同一输入哈希的完整主机套件、中英文 Chrome、构建后消费者及 ESP-IDF v5.5.2 独立目录编译保留实施阶段证据，本次没有重复完整编译和所有浏览器测试。证据快照与 patch 保留历史空格；源码及测试差异单独做 whitespace 检查。

无关 output/、tmp/ 与 runtime-repair 未跟踪文件排除；不修改版本、发布工作流、硬件保护及匹配规则。真实设备、双核调度和网络性能仍未验收；完整 peer 不解决同连接缺少操作 ID；发送成功不是浏览器 ACK。新增 C 回归尚未接入 CI，远端只能据新 head 的实际检查报告状态。
