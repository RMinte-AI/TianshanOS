# PR #42 本轮提交前检查

结论：可以推送，限本轮 SSH poller 最小修复及证据。

- 远端 PR head 与本地基线均为 3a19128d8638771c958fe47df1d6a7d339a10a4c，分支一致。
- 生产差异仅 8192 bytes PSRAM stack 预算与配套 vTaskDeleteWithCaps；不改变输出缓冲、优先级、协议、operation 或保护策略。
- 当前生产文件 SHA256 与 manifest 中构建输入一致：e53fa635a9775b571a143f9e54fdfd329eeb501345d878833b6567a3f41b21bb。未因提交准备修改生产输入，未重复完整构建。
- 本次重跑 `IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf bash tests/ws_subscriptions/run_operation_adapter.sh` 退出0；涵盖生产调用方及10次关闭/回收，ASan/UBSan。首次未设 IDF_PATH 被脚本拒绝，补齐固定 SDK 路径后重跑。
- 现存固定 SDK 两版构建及真机证据见 README；新 head CI 必须单独查看，不沿用旧 head 绿灯。
- 凭据模式检查未发现提交材料含真实 SSH 密码、Bearer token 或明文 token。无固件二进制、node_modules 或构建目录纳入提交。
- 排除原有 output/、tmp/、docs/runtime-repair/evidence/reviewer-fixes/diff-check.txt，保留原地。
- 余下边界：30秒安全 debug_tick 不等于低压关机测试；未直接读取真机 operation ledger；小幅堆净差未归因，不能宣称零泄漏。多终端抢占现象如实记录。

用户授权更新 PR；本轮不合并、不打 tag、不发布。

暂存检查补充：全量 `git diff --cached --check` 报告原始构建/串口日志的尾随空格、CRLF，以及 change.patch 的空白上下文行。保留这些证据原文，不为消除格式告警改写日志；排除上述原始 .log、crash.txt、change.patch 后，生产、测试及文档差异检查通过。
