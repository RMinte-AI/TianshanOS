# F1/F2 提交前审查

结论：可以推送。用户已明确授权更新原 PR #42。本轮基于 `bb0958c` 提交，不合并、不发布或操作设备。

复查了消息类别与准入、同步拒绝和异步结算、发布引用、目标身份、Shell/exec 终态顺序、保护事件回调、重试上限及停止依赖保留。当前源码与 final-manifest.json 所列构建输入哈希一致，构建二进制哈希也一致。未发现本轮范围内阻止推送的问题；不宣称无其他缺陷。

提交前再次执行 `bash tests/ws_subscriptions/run_f1_f2.sh`、`bash tests/ws_subscriptions/run_reviewer.sh` 和 `python3 tests/release/test_release.py`，均通过。日志为 pr-update-regression.txt、pr-update-reviewer.txt、pr-update-release.txt。其余回归及固件编译复用同一源码的本轮证据，详见 README.md。

暂存范围为 F1/F2 源码、测试、冻结基线/证据及更新后的中文发布说明。原 output/、tmp/ 和 docs/runtime-repair/evidence/reviewer-fixes/diff-check.txt 排除并保留。版本与工作流未改。历史 changes.patch 保留原上下文空格，不作为源码空格错误处理；当前源码、测试和文档另行进行暂存差异检查。

边界：主机 SDK/RTOS/设备响应模拟及本地浏览器回归不等于真实设备验收。慢连接、双核并发、保护负载、长期堆与栈水位仍待实机。保护通知有界而非无限无损；exec 网页观察失败不等于远端操作被取消。旧修复记录中的未提交状态是当时事实，此后按新授权更新 PR。
