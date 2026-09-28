# J1 提交前审查

结论：可以推送。用户已另行授权更新 PR #42，不授权合并或发布。

基线：8ad39a9b1ca39d225f94fb4db602fb2ee7438d09；远端 PR 仍为 OPEN，head 与本地基线相同。

## 范围与不变量

检查完整六文件 diff：生产代码仅 handlePowerEvent 与静态资源缓存标识；测试夹具调用真实保护通知、socket 消费、输入回调与 C handler。无生产后端、协议、版本或工作流修改。原事件筛选及本地行恢复保留；非本地可交互状态只显示通知，不改变状态或输入目的地。原跨层断言保留。

## 本次补跑

- `node --test --test-name-pattern=J1 tests/prompts/regression.test.cjs`：通过，见 publication-node.txt。
- `IDF_PATH=/Users/massif/esp/v5.5.2/esp-idf DEVELOPER_DIR=/Library/Developer/CommandLineTools node tests/prompts/project-cross.cjs`：通过，见 publication-cross.txt。实际 C 告警经过生产 JS 后，下一输入仍进入原 SSH，正常退出并回收。
- `git diff --check`：通过。六项当前源码/测试 SHA256 与 inputs.json 的 after 值全部一致。

本次发布准备未重复完整浏览器套件或固件编译，采用本轮 J1 实施阶段相同输入的 WebUI、双语跨层、优化资源 Chrome、固定 SDK 独立目录编译及 SPIFFS 一致性证据，详见 README。历史 v3 结果单独保留，不代替 J1 验证。

## 提交与验收边界

只纳入六个 J1 源码/测试文件及本目录。output/、tmp/ 与 runtime-repair 下原未跟踪文件排除，保持原样。没有发现本轮范围内阻断提交的问题；这不代表整个 PR 无缺陷。真实设备电源告警、SSH/xterm 呈现、网络时序及设备缓存升级仍需实机验收，未操作设备。
