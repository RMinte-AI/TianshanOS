# PR #42 更新前检查（2026-09-26）

结论：**可以推送**。用户本轮已授权追加提交并更新现有 PR；不合并、不发布、不操作设备。

发布前本地 HEAD、Fork 分支和上游 PR head 均为 `74b1b646ec4a45f9159bd028e622b13fca5eff75`。范围为 `changes.json` 列出的 22 个生产/测试文件及本目录实现、基线和验证材料。`output/`、`tmp/` 和另一份未跟踪的 runtime reviewer diff-check 不包含在提交中。

检查了创建租约、ticket/close 同锁、输出与终态结算、旧回调保活、原 timer 排空、停止失败后依赖保留，以及 SDK 最终发送边界的现有断言。没有发现阻止本次发布供审查的问题；这不是穷尽并发证明。

本次重新执行（IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf，DEVELOPER_DIR=/Library/Developer/CommandLineTools）：

- `bash tests/ws_subscriptions/run_operation.sh`：通过，publication-operation.txt。
- `bash tests/ws_subscriptions/run_operation_adapter.sh`：通过，publication-adapter.txt。
- `bash tests/ws_subscriptions/run_reviewer.sh`：通过，publication-reviewer.txt。
- `bash tests/ws_subscriptions/run_f1_f2.sh`：通过，publication-f1f2.txt。
- `python3 tests/release/test_release.py`：通过，publication-release.txt。
- `git diff --check`：生产/测试修改通过。

逐文件核对 changes.json，所有生产/测试 SHA256 及 changes.patch 哈希一致；build-artifacts.json 的 app、www 和独立 sdkconfig 实物哈希一致。本次发布前未重复完整编译、运行时、证书和浏览器套件，沿用同一源码快照在实施阶段实际执行的日志；不引用旧 PR head 的 CI 为新提交背书。新增回归仍未接入 CI。

README 的未执行/设备验收清单保持有效。资源测量是目标工具链静态帧与 sizeof，不是板上调用链峰值；传输结算不是浏览器收到确认。changes.json 的 local-only scope 和 working-tree-status.txt 保留为实现阶段历史快照，不代表本轮发布授权。
